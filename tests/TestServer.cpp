#include "TestServer.h"
#include <QHttpServerResponder>
#include <QHttpServerResponse>
#include <QRegularExpression>

// NOTE (Qt 6.11 API adaptation): QHttpServerResponse in this Qt version has no
// setHeader()/data()-returning-response convenience API as sketched in the task
// brief (that shape matches older Qt 6.x). Instead headers are built as a
// QHttpHeaders value and attached via QHttpServerResponse::setHeaders(). See
// /opt/homebrew/.../QtHttpServer.framework/Headers/qhttpserverresponse.h.

TestServer::TestServer(QByteArray body) : m_body(std::move(body)) {}

QByteArray TestServer::partial(qint64 start, qint64 end) const {
    if (end < 0 || end >= m_body.size()) end = m_body.size() - 1;
    if (start < 0) start = 0;
    return m_body.mid(int(start), int(end - start + 1));
}

// Parse "bytes=start-end" -> {start,end}; end=-1 means open.
static bool parseRange(const QByteArray& h, qint64& start, qint64& end) {
    QRegularExpression re("bytes=(\\d+)-(\\d*)");
    auto m = re.match(QString::fromUtf8(h));
    if (!m.hasMatch()) return false;
    start = m.captured(1).toLongLong();
    end   = m.captured(2).isEmpty() ? -1 : m.captured(2).toLongLong();
    return true;
}

bool TestServer::listen() {
    using Resp = QHttpServerResponse;

    m_http.route("/ranged", [this](const QHttpServerRequest& req) {
        m_uaSeen.insert(QString::fromUtf8(req.value("User-Agent")));
        const QByteArray cookie = req.value("Cookie");
        if (!cookie.isEmpty()) m_cookiesSeen.insert(QString::fromUtf8(cookie));
        const QByteArray referer = req.value("Referer");
        if (!referer.isEmpty()) m_referersSeen.insert(QString::fromUtf8(referer));
        qint64 s, e;
        const QByteArray rh = req.value("Range");
        if (parseRange(rh, s, e)) {
            const QByteArray b = partial(s, e < 0 ? m_body.size() - 1 : e);
            Resp r("application/octet-stream", b, Resp::StatusCode::PartialContent);
            QHttpHeaders headers;
            headers.append(QHttpHeaders::WellKnownHeader::ContentRange,
                QString("bytes %1-%2/%3").arg(s).arg(s + b.size() - 1).arg(m_body.size()).toUtf8());
            headers.append(QHttpHeaders::WellKnownHeader::AcceptRanges, "bytes");
            headers.append(QHttpHeaders::WellKnownHeader::ETag, m_etag.toUtf8());
            r.setHeaders(std::move(headers));
            return r;
        }
        Resp full("application/octet-stream", m_body);
        QHttpHeaders headers;
        headers.append(QHttpHeaders::WellKnownHeader::ETag, m_etag.toUtf8());
        full.setHeaders(std::move(headers));
        return full;
    });

    m_http.route("/plain", [this](const QHttpServerRequest&) {
        return Resp("application/octet-stream", m_body);   // ignores Range, always 200
    });

    // /redirect: 302 Found -> /ranged (Location absoluto). Simula o comportamento
    // de links tipo Google Takeout que respondem 302 antes do arquivo real.
    m_http.route("/redirect", [this](const QHttpServerRequest&) {
        Resp r("text/plain", QByteArray(), Resp::StatusCode::Found);   // 302
        QHttpHeaders headers;
        headers.append(QHttpHeaders::WellKnownHeader::Location, url("/ranged").toString().toUtf8());
        r.setHeaders(std::move(headers));
        return r;
    });

    m_http.route("/named", [this](const QHttpServerRequest& req) {
        qint64 s = 0, e = -1;
        QByteArray b = m_body;
        Resp::StatusCode code = Resp::StatusCode::Ok;
        QHttpHeaders headers;
        if (parseRange(req.value("Range"), s, e)) {
            b = partial(s, e < 0 ? m_body.size() - 1 : e);
            code = Resp::StatusCode::PartialContent;
            headers.append(QHttpHeaders::WellKnownHeader::ContentRange,
                QString("bytes %1-%2/%3").arg(s).arg(s + b.size() - 1).arg(m_body.size()).toUtf8());
        }
        Resp r("application/octet-stream", b, code);
        headers.append("Content-Disposition", "attachment; filename=\"Audiobook.m4a\"");
        r.setHeaders(std::move(headers));
        return r;
    });

    m_http.route("/nolength", [this](QHttpServerResponder& responder) {
        // QHttpServerResponse always resolves a Content-Length from its data,
        // so there is no header setter to "unset" it (unlike the sketch in the
        // brief). Writing a chunked response instead genuinely omits
        // Content-Length and sets Transfer-Encoding: chunked, which is the
        // real-world shape of a server reporting an unknown size.
        responder.writeBeginChunked(QByteArray("application/octet-stream"));
        responder.writeEndChunked(m_body);
    });

    m_http.route("/notfound", [](const QHttpServerRequest&) {
        return Resp("text/plain", "no", Resp::StatusCode::NotFound);
    });

    m_http.route("/changed", [this](const QHttpServerRequest& req) {
        // On resume (Range present) reply 200 with a different ETag -> triggers restart.
        if (!req.value("Range").isEmpty()) {
            Resp r("application/octet-stream", m_body);
            QHttpHeaders headers;
            headers.append(QHttpHeaders::WellKnownHeader::ETag, "\"v2\"");
            r.setHeaders(std::move(headers));
            return r;                                       // 200, not 206
        }
        Resp r("application/octet-stream", m_body);
        QHttpHeaders headers;
        headers.append(QHttpHeaders::WellKnownHeader::ETag, m_etag.toUtf8());
        headers.append(QHttpHeaders::WellKnownHeader::AcceptRanges, "bytes");
        r.setHeaders(std::move(headers));
        return r;
    });

    // /flaky?failTimes=K: simulates a dropped connection for the first K
    // requests to this path. QHttpServer fully buffers responses (no raw
    // socket control), so we can't literally sever the TCP connection mid
    // transfer. Instead we send a response whose actual body is only half of
    // the requested range while the Content-Range header still advertises
    // the full range/total. QHttpServerResponse derives Content-Length from
    // the (truncated) body it's given, so the client sees a clean,
    // error-free transfer that ends having delivered fewer bytes than the
    // segment needs. SegmentWorker::onFinished() detects that as a short
    // read (m_seg.current <= m_seg.end after finished()) and retries -
    // observably identical to a mid-transfer drop, without needing raw
    // socket access. After failTimes requests, subsequent ones serve the
    // full requested range with correct 206 + Content-Range + ETag.
    m_http.route("/flaky", [this](const QHttpServerRequest& req) {
        const int failTimes = req.query().queryItemValue("failTimes").toInt();
        const int n = ++m_hits["/flaky"];
        qint64 s = 0, e = -1;
        parseRange(req.value("Range"), s, e);
        const QByteArray full = partial(s, e);
        const QByteArray body = (n <= failTimes) ? full.left(full.size() / 2) : full;
        Resp r("application/octet-stream", body, Resp::StatusCode::PartialContent);
        QHttpHeaders headers;
        headers.append(QHttpHeaders::WellKnownHeader::ContentRange,
            QString("bytes %1-%2/%3").arg(s).arg(s + full.size() - 1).arg(m_body.size()).toUtf8());
        headers.append(QHttpHeaders::WellKnownHeader::ETag, m_etag.toUtf8());
        r.setHeaders(std::move(headers));
        return r;
    });

    // /stall: always serves a short read (a handful of bytes) while the
    // Content-Range header still promises the whole body. QHttpServer fully
    // buffers responses and closes cleanly once its (short) body is sent, so
    // this can't literally freeze the socket - the client sees a fast, clean
    // close having received far fewer bytes than Content-Range promised.
    // SegmentWorker::onFinished() treats that as a short read and retries;
    // with idleTimeoutMs/connectTimeoutMs and maxSegmentRetries small, the
    // point of Task 9's test is that the whole config (timeouts + retry
    // budget) is wired end-to-end into an eventual Error, not that this
    // particular route forces the idle timer itself to fire (a truly hung
    // socket isn't practical to produce portably from QHttpServer).
    m_http.route("/stall", [this](const QHttpServerRequest&) {
        const QByteArray body = m_body.left(8);
        Resp r("application/octet-stream", body, Resp::StatusCode::PartialContent);
        QHttpHeaders headers;
        headers.append(QHttpHeaders::WellKnownHeader::ContentRange,
            QString("bytes 0-%1/%2").arg(m_body.size() - 1).arg(m_body.size()).toUtf8());
        r.setHeaders(std::move(headers));
        return r;
    });

    m_tcp.listen(QHostAddress::LocalHost);
    m_port = m_tcp.serverPort();
    return m_http.bind(&m_tcp);
}

QUrl TestServer::url(const QString& path) const {
    return QUrl(QString("http://127.0.0.1:%1%2").arg(m_port).arg(path));
}
