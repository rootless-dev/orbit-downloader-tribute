#include "DownloadTask.h"
#include "Segmentation.h"
#include "Logger.h"
#include <QFile>
#include <QFileInfo>
#include <QDir>

DownloadTask::DownloadTask(Transport* transport, const EngineConfig& cfg,
                           RateLimiter* limiter, QObject* parent)
    : QObject(parent), m_transport(transport), m_cfg(cfg), m_limiter(limiter) {}

void DownloadTask::init(const QUuid& id, const QUrl& url, const QString& destPath, int segmentCount,
                        const HeaderList& extraHeaders, bool provisionalName) {
    m_id = id; m_url = url; m_destPath = destPath; m_segmentCount = segmentCount;
    m_extraHeaders = extraHeaders;
    m_provisionalName = provisionalName;
}

void DownloadTask::restore(const DownloadRecord& rec, const QVector<Segment>& segs,
                           const QString& etag, const QString& lastModified, bool validated) {
    m_id = rec.id; m_url = rec.url; m_destPath = rec.destPath;
    m_segmentCount = rec.segmentCount; m_totalBytes = rec.totalBytes;
    m_supportsRange = rec.supportsRange; m_segments = segs;
    m_etag = etag; m_lastModified = lastModified; m_validated = validated;
    m_extraHeaders = rec.extraHeaders;
    m_probed = !segs.isEmpty();
    m_priority = rec.priority;
    m_state = (rec.state == DownloadState::Cancelled) ? DownloadState::Cancelled
                                                       : DownloadState::Paused;
}

void DownloadTask::setState(DownloadState s) {
    if (m_state == s) return;
    m_state = s;
    emit stateChanged(s);
}

void DownloadTask::start() {
    m_error.clear();
    setState(DownloadState::Connecting);
    if (!m_probed) {
        Probe* probe = m_transport->createProbe(m_cfg, this);
        connect(probe, &Probe::finished, this, [this, probe](const ProbeResult& r) {
            probe->deleteLater();
            onProbed(r);
        });
        probe->start(m_url, m_creds, m_extraHeaders);
    } else {
        beginSegments();
    }
}

void DownloadTask::onProbed(const ProbeResult& r) {
    // O probe é assíncrono: se a task saiu de Connecting enquanto ele estava em
    // voo (pause()/cancel() durante a sondagem), o resultado que chega agora é
    // obsoleto e não pode ressuscitar o download (recriando o arquivo, burlando
    // o cap de concorrência). Todo onProbed legítimo roda direto de Connecting.
    if (m_state != DownloadState::Connecting) return;
    if (!r.ok) {
        if (r.authRequired) { askForCredentials(); return; }   // spec §3.6 (a)
        m_error = r.error;
        logLine(LogLevel::Error, QStringLiteral("probe failed: %1").arg(r.error));
        setState(DownloadState::Error);
        return;
    }
    m_totalBytes    = r.totalBytes;
    m_supportsRange = r.supportsRange;
    m_etag          = r.etag;
    m_lastModified  = r.lastModified;
    m_validated     = !r.etag.isEmpty() || !r.lastModified.isEmpty();
    m_segments      = computeSegments(m_totalBytes, m_supportsRange, m_segmentCount, m_cfg.minSegSize);
    logLine(LogLevel::Info, QStringLiteral("probe ok: status=%1 type=%2 total=%3 range=%4 segments=%5")
                .arg(r.httpStatus).arg(r.contentType.isEmpty() ? QStringLiteral("?") : r.contentType)
                .arg(m_totalBytes).arg(m_supportsRange ? "yes" : "no").arg(m_segments.size()));
    // Nome provisório (browser/clipboard/drag): o destino atual é só um palpite
    // derivado da URL (ex.: Drive dá path "/download"). Se o servidor informou o
    // nome via Content-Disposition, adotamos — reusando resolveUniquePath p/
    // evitar colisão. O arquivo ainda NÃO foi aberto (beginSegments faz isso a
    // seguir), então basta trocar o caminho. O diálogo New passa
    // provisionalName=false, então a escolha explícita do usuário nunca é
    // sobrescrita.
    if (m_provisionalName && !r.suggestedFileName.isEmpty()) {
        const QString safe = QFileInfo(r.suggestedFileName).fileName();   // sem path/traversal
        if (!safe.isEmpty()) {
            const QString dir = QFileInfo(m_destPath).absolutePath();
            m_destPath = Persistence::resolveUniquePath(QDir(dir).filePath(safe));
            logLine(LogLevel::Info,
                    QStringLiteral("filename from Content-Disposition: %1").arg(safe));
        }
    }
    m_provisionalName = false;   // decisão aplicada uma vez

    m_probed        = true;
    beginSegments();
}

void DownloadTask::beginSegments() {
    // NOTE (brief deviation): the brief's sample unconditionally did
    // `m_file = new QFile(m_destPath, this)` here. beginSegments() is also
    // re-entered from onRestartRequired() (and would be from a same-instance
    // pause()/start() resume), so recreating it unconditionally leaked the
    // previous QFile object (never closed, kept its fd open for the life of
    // the DownloadTask) and violated the "one shared QFile" invariant the
    // brief itself states. Only open the file the first time; subsequent
    // beginSegments() calls reuse the already-open handle.
    if (!m_file) {
        m_file = new QFile(m_destPath, this);
        if (!m_file->open(QIODevice::ReadWrite)) {
            delete m_file;
            m_file = nullptr;
            setState(DownloadState::Error);
            return;
        }
        if (m_totalBytes > 0) m_file->resize(m_totalBytes);   // preallocate
    }

    // Spec 3.4: fallback/unknown-size downloads (single segment, no Range
    // support) are NOT resumable. If a segment being (re)started here is a
    // fallback segment (end < 0) that carried partial progress forward from
    // a restored .meta or a prior pause() in this same instance, discard
    // that partial and restart it from zero. Without this, resuming such a
    // segment sends a plain Range-less/If-Range-less request, the server
    // replies 200 with the FULL body, and SegmentWorker would write that
    // full body starting at the stale m_seg.current offset - corrupting the
    // file with extra leading bytes instead of overwriting from the start.
    // Range segments (end >= 0) are unaffected and keep resuming from their
    // persisted `current` as before.
    bool fallbackReset = false;
    for (auto& s : m_segments) {
        if (s.end < 0 && s.current > s.start) {
            s.current = s.start;
            fallbackReset = true;
        }
    }
    if (fallbackReset) {
        m_file->resize(0);   // truncate the stale partial so the rewritten body is clean
    }

    setState(DownloadState::Downloading);
    for (const auto& seg : m_segments)
        logLine(LogLevel::Debug, QStringLiteral("segment %1 [%2..%3]")
                    .arg(seg.index).arg(seg.start).arg(seg.end));
    if (!m_metaTimer) {
        m_metaTimer = new QTimer(this);
        m_metaTimer->setInterval(2000);
        connect(m_metaTimer, &QTimer::timeout, this, [this] {
            if (m_file) m_file->flush();
            Persistence::writeMeta(m_destPath, m_segments, m_etag, m_lastModified, m_validated);
        });
    }
    m_metaTimer->start();
    m_completedCount = 0;
    for (const auto& seg : m_segments) {
        if (seg.isComplete()) { ++m_completedCount; continue; }
        spawnWorker(seg);
    }
    checkAllComplete();     // in case everything was already complete (restore)
}

void DownloadTask::spawnWorker(const Segment& seg) {
    SegmentSource* w = m_transport->createWorker(m_file, m_cfg, m_limiter, this);
    m_workers.append(w);
    connect(w, &SegmentSource::progressed, this, [this](int idx, qint64 cur) {
        for (auto& s : m_segments) if (s.index == idx) s.current = cur;
        emit segmentProgress(idx, cur);
        // Coalesce the (potentially very chatty) `progress` signal: schedule
        // at most one emit per progressThrottleMs via a single-shot timer,
        // rather than emitting synchronously on every worker update.
        // segmentProgress above stays unthrottled - only this aggregate
        // signal is rate-limited.
        if (!m_progressTimer) {
            m_progressTimer = new QTimer(this);
            m_progressTimer->setSingleShot(true);
            connect(m_progressTimer, &QTimer::timeout, this, &DownloadTask::emitProgressNow);
        }
        if (!m_progressTimer->isActive() && !m_progressPending) {
            m_progressPending = true;
            m_progressTimer->start(m_cfg.progressThrottleMs);
        }
    });
    connect(w, &SegmentSource::completed,        this, &DownloadTask::onSegmentCompleted);
    connect(w, &SegmentSource::failed,           this, &DownloadTask::onSegmentFailed);
    connect(w, &SegmentSource::restartRequired,  this, &DownloadTask::onRestartRequired);
    const QString validator = m_validated ? (!m_etag.isEmpty() ? m_etag : m_lastModified) : QString();
    w->start(seg, m_url, validator, m_creds, m_extraHeaders);
}

void DownloadTask::onSegmentCompleted(int index) {
    for (auto& s : m_segments) if (s.index == index) s.current = s.end + 1;
    ++m_completedCount;
    logLine(LogLevel::Debug, QStringLiteral("segment %1 complete").arg(index));
    checkAllComplete();
}

void DownloadTask::onSegmentFailed(int index, const QString& error, FailureKind kind) {
    // Ponto de entrada (b) do fluxo de credenciais (spec §3.6): no resume o probe
    // não roda, então o 530 chega aqui pelo worker. askForCredentials() já para
    // os demais workers via pause() e emite credentialsRequired uma única vez.
    if (kind == FailureKind::AuthRequired) { askForCredentials(); return; }
    if (m_metaTimer) m_metaTimer->stop();
    if (m_progressPending) emitProgressNow();   // flush the last coalesced value
    if (m_progressTimer) m_progressTimer->stop();
    for (auto* w : m_workers) w->stop();
    Persistence::writeMeta(m_destPath, m_segments, m_etag, m_lastModified, m_validated);
    m_error = error;
    logLine(LogLevel::Warn, QStringLiteral("segment %1 failed: %2").arg(index).arg(error));
    setState(DownloadState::Error);
}

void DownloadTask::onRestartRequired(int index) {
    Q_UNUSED(index);
    // Guarda: com N segmentos, cada worker detecta a mudança do recurso por
    // conta própria (HTTP: 200 no lugar de 206; FTP: MDTM divergente, spec
    // §3.5) e todos emitem restartRequired na mesma volta do event loop. Só a
    // primeira emissão vale; as seguintes chegariam com m_workers já esvaziado.
    if (m_restarting) return;
    m_restarting = true;
    logLine(LogLevel::Warn, QStringLiteral("resource changed, restarting from zero"));

    // deleteLater(), nunca delete/qDeleteAll: o emissor deste sinal está entre
    // os workers e seu frame de pilha ainda vai retornar. Destruí-lo aqui é
    // use-after-free (spec §9.1). stop() já o torna inerte imediatamente; a
    // destruição fica para o retorno ao event loop.
    for (auto* w : m_workers) { w->stop(); w->deleteLater(); }
    m_workers.clear();

    for (auto& s : m_segments) s.current = s.start;   // reset to zero
    m_validated = false;                               // don't send If-Range again
    m_etag.clear(); m_lastModified.clear();
    beginSegments();
    m_restarting = false;
}

void DownloadTask::emitProgressNow() {
    m_progressPending = false;
    emit progress(receivedBytes(), m_totalBytes);
}

void DownloadTask::checkAllComplete() {
    if (m_completedCount < m_segments.size()) return;
    // NOTE (brief deviation): the brief says to stop m_metaTimer "at the top"
    // of checkAllComplete(). checkAllComplete() runs after *every* segment
    // completion, not just the final one, so stopping the timer before the
    // completedCount guard above would kill the periodic .meta flush the
    // first time any single segment finishes while others are still
    // downloading. The stop is placed here instead, at the top of the
    // "we are actually done" branch, so the timer only stops once the whole
    // download is complete and immediately before m_file->close() -
    // guaranteeing it can never fire (and touch m_file) after the file is
    // closed.
    if (m_metaTimer) m_metaTimer->stop();
    // Same reasoning applies to the progress-coalescing timer: cancel any
    // pending throttled emit so it can never fire after this DownloadTask
    // has moved past Completed, then deliver one immediate, final emit with
    // the definitive received/total values so no update is ever lost to the
    // throttle window.
    if (m_progressTimer) m_progressTimer->stop();
    m_progressPending = false;
    emit progress(receivedBytes(), m_totalBytes);
    m_file->flush();
    m_file->close();
    Persistence::removeMeta(m_destPath);
    setState(DownloadState::Completed);
}

void DownloadTask::requeue() {
    // Move a Paused/Error task back to Queued so DownloadManager::pump() can
    // promote it up to maxConcurrentDownloads, exactly like a freshly-added
    // download. Deliberately minimal: only flips the state (setState() emits
    // stateChanged only on a real transition). Does not touch m_probed,
    // m_segments, or the on-disk .meta - start() on a Queued task with
    // m_probed == true and populated segments resumes from beginSegments()
    // (skipping the probe), and each segment's `current` offset is exactly
    // what pause()'s prior .meta write persisted, so the task remains fully
    // resumable from where it left off.
    if (m_state == DownloadState::Paused || m_state == DownloadState::Error ||
        m_state == DownloadState::Cancelled)
        setState(DownloadState::Queued);
}

// Cancela um download em curso: para todos os workers, descarta o parcial
// (o próprio destPath - não existe .part separado neste código) e o .meta,
// e zera todo o estado de probe/segmentos para que um Start futuro (a partir
// de Cancelled) recomece do zero, não retome de onde parou.
void DownloadTask::cancel() {
    for (auto* w : m_workers) { w->stop(); w->deleteLater(); }
    m_workers.clear();
    if (m_metaTimer) m_metaTimer->stop();
    if (m_progressTimer) m_progressTimer->stop();
    m_progressPending = false;
    if (m_file) { m_file->close(); delete m_file; m_file = nullptr; }
    QFile::remove(m_destPath);                 // parcial = próprio destPath
    Persistence::removeMeta(m_destPath);
    m_segments.clear();                        // -> Start recomeça do zero
    m_completedCount = 0;
    m_probed = false;
    m_totalBytes = -1;
    setState(DownloadState::Cancelled);
}

void DownloadTask::setDestPath(const QString& path) {
    if (m_file) { m_file->close(); delete m_file; m_file = nullptr; }
    m_destPath = path;
}

void DownloadTask::pause() {
    for (auto* w : m_workers) w->stop();
    if (m_metaTimer) m_metaTimer->stop();
    if (m_progressPending) emitProgressNow();   // flush the last coalesced value
    if (m_progressTimer) m_progressTimer->stop();
    // Guard: only persist a .meta when there is real segment state to save.
    // A task that never started (still Queued, m_segments empty because it
    // was never probed) has nothing meaningful to write - writing one
    // anyway fabricates a spurious empty .meta for a download that never
    // ran. This lets DownloadManager::pause(id) call pause() on a Queued
    // task (to genuinely hold it) without that side effect.
    if (!m_segments.isEmpty())
        Persistence::writeMeta(m_destPath, m_segments, m_etag, m_lastModified, m_validated);
    setState(DownloadState::Paused);
}

void DownloadTask::setCredentials(const Credentials& c) {
    m_creds = c;
    m_awaitingCredentials = false;    // nova rodada: pode perguntar de novo se falhar
}

// Para tudo, persiste o progresso e pede credenciais UMA vez. Com N segmentos,
// cada worker leva 530 e chama isto — só a primeira chamada emite (spec §3.6).
// pause() (e não setState) porque é preciso parar os N-1 workers restantes:
// sem isso, seguiriam tentando e levando 530.
void DownloadTask::askForCredentials() {
    if (m_awaitingCredentials) return;
    m_awaitingCredentials = true;
    logLine(LogLevel::Info, QStringLiteral("credentials required for host %1").arg(m_url.host()));
    pause();                                        // para workers, escreve .meta, -> Paused
    emit credentialsRequired(m_id, m_url.host());
}

qint64 DownloadTask::receivedBytes() const {
    qint64 total = 0;
    for (const auto& s : m_segments) total += s.downloaded();
    return total;
}

void DownloadTask::logLine(LogLevel level, const QString& msg) {
    if (m_logger) m_logger->logTask(m_id, m_destPath, level, msg);
}

QVector<Segment> DownloadTask::segments() const { return m_segments; }

DownloadRecord DownloadTask::record() const {
    DownloadRecord r;
    r.id = m_id; r.url = m_url; r.destPath = m_destPath;
    r.totalBytes = m_totalBytes; r.supportsRange = m_supportsRange;
    r.state = m_state; r.segmentCount = m_segmentCount;
    r.priority = m_priority;
    r.extraHeaders = m_extraHeaders;
    return r;
}
