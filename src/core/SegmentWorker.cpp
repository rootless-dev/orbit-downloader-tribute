#include "SegmentWorker.h"
#include "RateLimiter.h"
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QFile>
#include <QTimer>
#include <QDateTime>

SegmentWorker::SegmentWorker(QNetworkAccessManager* nam, QFile* file,
                             const EngineConfig& cfg, RateLimiter* limiter, QObject* parent)
    : SegmentSource(parent), m_nam(nam), m_file(file), m_cfg(cfg), m_limiter(limiter) {}

void SegmentWorker::start(const Segment& seg, const QUrl& url,
                          const QString& validator, const Credentials& creds,
                          const HeaderList& extraHeaders) {
    Q_UNUSED(creds);
    m_extraHeaders = extraHeaders;   // guardado; aplicado em openRequest() na Task 3
    m_seg = seg;
    m_url = url;
    m_validator = validator;
    m_stopped = false;
    m_attempt = 0;
    m_expectPartial = (m_seg.start > 0) || !m_validator.isEmpty();
    openRequest();
}

void SegmentWorker::openRequest() {
    // Defensive: stop() also cancels m_retryTimer, so in the normal case this
    // never fires after a stop. It stays as a belt-and-suspenders guard
    // against a retry timer that was already queued to fire on this event
    // loop turn when stop() ran (QTimer::stop() prevents *future* timeouts,
    // but this keeps the invariant explicit rather than implicit).
    if (m_stopped) return;
    if (m_seg.isComplete()) { emit completed(m_seg.index); return; }
    QNetworkRequest req(m_url);
    // Range: resume from current; open-ended if end<0 (fallback).
    QByteArray range = "bytes=" + QByteArray::number(m_seg.current) + "-";
    if (m_seg.end >= 0) range += QByteArray::number(m_seg.end);
    req.setRawHeader("Range", range);
    req.setRawHeader("User-Agent", m_cfg.userAgent.toUtf8());
    for (const auto& h : m_extraHeaders) req.setRawHeader(h.first, h.second);
    if (!m_validator.isEmpty()) req.setRawHeader("If-Range", m_validator.toUtf8());
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    m_reply = m_nam->get(req);
    if (m_limiter && m_limiter->rate() > 0)
        m_reply->setReadBufferSize(qMax<qint64>(m_limiter->rate(), 64 * 1024));  // ~1s buffer, backpressure via TCP
    connect(m_reply, &QNetworkReply::readyRead,     this, &SegmentWorker::onReadyRead);
    connect(m_reply, &QNetworkReply::finished,      this, &SegmentWorker::onFinished);
    connect(m_reply, &QNetworkReply::errorOccurred, this, &SegmentWorker::onErrorOccurred);
    armIdleTimer(m_cfg.connectTimeoutMs);   // deadline until first byte
}

void SegmentWorker::armIdleTimer(int ms) {
    if (!m_idleTimer) {
        m_idleTimer = new QTimer(this);
        m_idleTimer->setSingleShot(true);
        connect(m_idleTimer, &QTimer::timeout, this, &SegmentWorker::onTimeout);
    }
    m_idleTimer->start(ms);
}

void SegmentWorker::onReadyRead() {
    if (m_stopped || !m_reply) return;
    armIdleTimer(m_cfg.idleTimeoutMs);   // activity seen -> re-arm as an idle (not connect) deadline
    const int status = m_reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (m_expectPartial && status == 200) {   // server ignored If-Range -> resource changed
        m_reply->abort();
        emit restartRequired(m_seg.index);
        return;
    }
    const qint64 avail = m_reply->bytesAvailable();
    if (avail <= 0) return;
    qint64 grant = avail;
    if (m_limiter) {
        grant = m_limiter->take(avail, QDateTime::currentMSecsSinceEpoch());
        if (grant <= 0) { scheduleDrain(); return; }   // sem tokens: tentar de novo em breve
    }
    const QByteArray chunk = m_reply->read(grant);
    if (chunk.isEmpty()) return;
    m_file->seek(m_seg.current);
    const qint64 written = m_file->write(chunk);
    if (written < 0) {                          // disk full / permission
        m_reply->abort();
        emit failed(m_seg.index, "write error: " + m_file->errorString(), FailureKind::Fatal);
        return;
    }
    m_seg.current += written;
    emit progressed(m_seg.index, m_seg.current);
    if (m_reply->bytesAvailable() > 0) scheduleDrain();  // restou dado sob throttle
}

void SegmentWorker::scheduleDrain() {
    if (m_stopped) return;
    if (!m_drainTimer) {
        m_drainTimer = new QTimer(this);
        m_drainTimer->setSingleShot(true);
        connect(m_drainTimer, &QTimer::timeout, this, &SegmentWorker::onReadyRead);
    }
    if (!m_drainTimer->isActive()) m_drainTimer->start(20);   // ~20ms até repor tokens
}

void SegmentWorker::onFinished() {
    // Guard order matters: stop() flips m_stopped before touching m_reply, so any
    // reentrant/synchronous emission during its abort() call is a no-op here.
    // The !m_reply half of the guard also covers the case where
    // onErrorOccurred() already classified this same reply's error and
    // (via scheduleRetry's synchronous "retries exhausted" -> failed ->
    // DownloadTask::onSegmentFailed -> stop()) had this worker's m_reply
    // cleared out from under us before Qt gets around to emitting finished().
    if (m_stopped || !m_reply) return;
    if (m_idleTimer) m_idleTimer->stop();   // this reply is done one way or another; a fresh
                                             // attempt (if any) re-arms it from openRequest()
    if (m_drainTimer) m_drainTimer->stop(); // ditto: don't let a stale drain re-enter onReadyRead()

    QNetworkReply* reply = m_reply;
    m_reply = nullptr;
    const bool hadError = (reply->error() != QNetworkReply::NoError);
    reply->deleteLater();

    // hadError covers genuine network errors (already classified and handled
    // by onErrorOccurred, which runs first and either scheduled a retry or
    // emitted failed) and our own abort() calls from onReadyRead
    // (write-error / restartRequired), which surface as
    // OperationCanceledError and were already reported at the point of the
    // abort. Either way, the outcome was already decided; don't act again.
    if (hadError) return;

    // The transfer itself is complete at this point; drain whatever is still
    // sitting in the reply's (possibly bounded, see openRequest()) buffer
    // before evaluating the short-read check below. Bypass the limiter here:
    // these bytes are already received off the wire, so throttling them
    // further is pointless and would only produce a spurious short-read
    // retry (burning the retry budget) while data that arrived on time sits
    // unread.
    if (reply->bytesAvailable() > 0) {
        const QByteArray rest = reply->readAll();
        m_file->seek(m_seg.current);
        const qint64 w = m_file->write(rest);
        if (w > 0) { m_seg.current += w; emit progressed(m_seg.index, m_seg.current); }
    }

    // No transport error, but fewer bytes arrived than the segment still
    // needs (e.g. the server declared a larger Content-Range than it
    // actually sent, or genuinely dropped the connection after a clean
    // Content-Length). Treat as a recoverable short read and retry from the
    // advanced m_seg.current - not from scratch.
    if (m_seg.end >= 0 && m_seg.current <= m_seg.end) {
        scheduleRetry("short read");
        return;
    }

    m_file->flush();
    // Fallback mode (end<0): completion is EOF. Mark end so isComplete() is true.
    if (m_seg.end < 0) m_seg.end = m_seg.current - 1;
    emit completed(m_seg.index);
}

void SegmentWorker::onErrorOccurred() {
    if (m_stopped || !m_reply) return;
    if (m_idleTimer) m_idleTimer->stop();   // a real network error is its own kind of "done";
                                             // don't let a stale idle deadline also fire for it
    const auto err = m_reply->error();
    if (err == QNetworkReply::OperationCanceledError) return; // our own abort()
    // Cleanup (deleteLater + null m_reply) is left to onFinished(), which Qt
    // guarantees fires right after errorOccurred() for the same reply - see
    // the single cleanup point there. That keeps reply-lifetime handling in
    // one place and mirrors the pre-retry behavior of this method.
    if (isRecoverable(err)) scheduleRetry("network error: " + m_reply->errorString());
    else {
        // AuthenticationRequiredError vira AuthRequired; 403/404 seguem Fatal.
        const bool isAuth = (err == QNetworkReply::AuthenticationRequiredError);
        emit failed(m_seg.index, "non-recoverable network error: " + m_reply->errorString(),
                    isAuth ? FailureKind::AuthRequired : FailureKind::Fatal);
    }
}

bool SegmentWorker::isRecoverable(QNetworkReply::NetworkError e) const {
    switch (e) {
        case QNetworkReply::ContentNotFoundError:        // 404
        case QNetworkReply::ContentAccessDenied:         // 403
        case QNetworkReply::AuthenticationRequiredError:
            return false;
        default:
            return true;                                 // timeouts, resets, remote-closed, ...
    }
}

// Fires when no byte has arrived within connectTimeoutMs (before the first
// byte) or idleTimeoutMs (after the last one). This is deliberately NOT
// symmetric with onFinished()'s "single cleanup point" pattern:
//
//   - onFinished() is the only place that deleteLater()s + nulls m_reply for
//     the *normal* lifecycle, and onErrorOccurred() deliberately leaves the
//     reply alone, relying on Qt to emit finished() right after
//     errorOccurred() for the same reply so onFinished() can do that
//     cleanup moments later - m_reply stays valid across the gap.
//   - onTimeout() can't rely on that: it does NOT set m_stopped (the worker
//     keeps going and retries), and scheduleRetry() below starts a timer
//     that will call openRequest() - which reassigns m_reply to a *new*
//     reply - after only retryBackoffBaseMs. If the old, aborted reply's
//     finished()/errorOccurred() signals were still connected and arrived
//     after that reassignment, onFinished()/onErrorOccurred() would read
//     m_reply expecting it to describe the signal's sender and instead find
//     the new, unrelated, in-flight reply - corrupting its state. (The
//     `if (m_stopped || !m_reply) return;` guard alone doesn't cover this:
//     m_stopped stays false and m_reply gets non-null again as soon as the
//     retry opens.)
//
// So this handler disconnects the old reply from `this` before aborting it,
// closing that window outright: whatever the old reply does after abort()
// is inert. It then does its own cleanup (deleteLater + null) and calls
// scheduleRetry() directly, instead of going through onFinished/
// onErrorOccurred at all.
void SegmentWorker::onTimeout() {
    if (m_stopped || !m_reply) return;
    if (m_drainTimer) m_drainTimer->stop();   // don't let a stale drain re-enter onReadyRead()
                                               // against the reply openRequest() is about to open
    QNetworkReply* reply = m_reply;
    m_reply = nullptr;
    reply->disconnect(this);
    reply->abort();
    reply->deleteLater();
    scheduleRetry("timeout");
}

void SegmentWorker::scheduleRetry(const QString& why) {
    Q_UNUSED(why);
    if (m_attempt >= m_cfg.maxSegmentRetries) {
        emit failed(m_seg.index, "retries exhausted", FailureKind::Fatal);
        return;
    }
    ++m_attempt;
    const int delay = m_cfg.retryBackoffBaseMs * (1 << (m_attempt - 1));
    if (!m_retryTimer) {
        m_retryTimer = new QTimer(this);
        m_retryTimer->setSingleShot(true);
        connect(m_retryTimer, &QTimer::timeout, this, &SegmentWorker::openRequest);
    }
    m_retryTimer->start(delay);
}

void SegmentWorker::stop() {
    m_stopped = true;
    if (m_retryTimer) m_retryTimer->stop();   // a pending retry must not fire after stop()
    if (m_idleTimer) m_idleTimer->stop();     // ditto for a pending idle/connect deadline
    if (m_drainTimer) m_drainTimer->stop();   // ditto for a pending drain re-entry
    if (m_reply) {
        QNetworkReply* reply = m_reply;
        m_reply = nullptr;
        reply->abort();
        reply->deleteLater();
    }
}
