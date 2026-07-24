#include "DownloadManager.h"
#include "HttpTransport.h"
#include "FtpTransport.h"
#include "Persistence.h"
#include "Logger.h"
#include "torrent/TorrentTask.h"
#include "torrent/TorrentMetainfo.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QJsonArray>
#include <QJsonObject>
#include <algorithm>

DownloadManager::DownloadManager(const EngineConfig& cfg, const QString& dataDir,
                                 Logger* logger, QObject* parent)
    : QObject(parent), m_cfg(cfg), m_dataDir(dataDir), m_logger(logger) {
    m_limiter.setRate(cfg.maxBytesPerSec);
    auto http = std::make_unique<HttpTransport>(this);
    m_transports.insert("http",  http.get());
    m_transports.insert("https", http.get());
    m_owned.push_back(std::move(http));

    auto ftp = std::make_unique<FtpTransport>();
    m_transports.insert("ftp", ftp.get());
    m_owned.push_back(std::move(ftp));

    m_torrentNam = new QNetworkAccessManager(this);   // shared by every TorrentTask (trackers)

    QDir().mkpath(m_dataDir);
}

Transport* DownloadManager::transportFor(const QUrl& url) const {
    return m_transports.value(url.scheme().toLower(), nullptr);
}

QString DownloadManager::sessionPath() const { return m_dataDir + "/downloads.json"; }

void DownloadManager::wire(AbstractTask* t) {
    connect(t, &AbstractTask::progress, this, [this, t](qint64 r, qint64 tot) {
        emit taskProgress(t->id(), r, tot);
    });
    connect(t, &AbstractTask::stateChanged, this, [this, t](DownloadState s) {
        if (m_logger) {
            // destPath is DownloadTask-specific (not on AbstractTask); every
            // task is one today, but guard with qobject_cast so a future
            // non-DownloadTask Kind doesn't crash here.
            QString dest;
            if (auto* dt = qobject_cast<DownloadTask*>(t)) dest = dt->record().destPath;
            m_logger->logTask(t->id(), dest,
                              s == DownloadState::Error ? LogLevel::Error : LogLevel::Info,
                              QStringLiteral("state -> %1").arg(stateName(s)));
        }
        emit taskStateChanged(t->id(), s);
        saveSession();
        if (s == DownloadState::Completed || s == DownloadState::Error ||
            s == DownloadState::Paused)
            pump();                       // a slot may have freed up
    });
    // credentialsRequired is DownloadTask-specific (HTTP/FTP auth flow, spec
    // §3.6) - not part of the AbstractTask interface.
    if (auto* dt = qobject_cast<DownloadTask*>(t)) {
        connect(dt, &DownloadTask::credentialsRequired, this,
                [this](const QUuid& id, const QString& host) {
            emit credentialsRequired(id, host);
        });
    }
}

QUuid DownloadManager::addDownload(const QUrl& url, const QString& destPath,
                                   const HeaderList& extraHeaders, bool provisionalName) {
    Transport* tr = transportFor(url);
    if (!tr) return QUuid();          // esquema desconhecido: nada criado (spec critério 2)
    const QString finalPath = Persistence::resolveUniquePath(destPath);
    auto* t = new DownloadTask(tr, m_cfg, &m_limiter, this);
    t->init(QUuid::createUuid(), url, finalPath, m_cfg.segmentCount, extraHeaders, provisionalName);
    wire(t);
    t->setLogger(m_logger);
    m_tasks.append(t);
    saveSession();
    pump();
    return t->id();
}

namespace {
// Stable per-torrent RNG seed derived from the info-hash (first 4 bytes,
// big-endian) - never a global/wall-clock RNG, so the peer_id TorrentTask
// derives from it is reproducible across restarts of the SAME torrent.
quint32 seedFromInfoHash(const QByteArray& infoHash) {
    quint32 seed = 0;
    for (int i = 0; i < 4 && i < infoHash.size(); ++i)
        seed = (seed << 8) | quint8(infoHash[i]);
    return seed;
}

QString strategyToString(PieceStrategy s) {
    return s == PieceStrategy::RarestFirst ? QStringLiteral("rarest") : QStringLiteral("sequential");
}
PieceStrategy strategyFromString(const QString& s) {
    return s == QLatin1String("rarest") ? PieceStrategy::RarestFirst : PieceStrategy::Sequential;
}
} // namespace

QString DownloadManager::torrentsDir() const { return m_dataDir + "/torrents"; }
QString DownloadManager::torrentsSessionPath() const { return m_dataDir + "/torrents.json"; }

TorrentTask* DownloadManager::makeTorrentTask(const TorrentMetainfo& meta, const QString& destDir,
                                              const QSet<int>& selectedFiles, PieceStrategy strategy) {
    return new TorrentTask(meta, destDir, selectedFiles, strategy,
                           m_torrentListenPort, m_torrentMaxPeers, m_torrentVerify,
                           seedFromInfoHash(meta.infoHash),
                           m_torrentNam, &m_limiter, m_logger, torrentsDir(), this);
}

void DownloadManager::setTorrentDefaults(int maxPeersPerTorrent, quint16 listenPort,
                                         ResumeVerifyMode verify) {
    m_torrentMaxPeers   = maxPeersPerTorrent;
    m_torrentListenPort = listenPort;
    m_torrentVerify     = verify;
}

QUuid DownloadManager::addTorrent(const QString& torrentPath, const QString& destDir,
                                  const QSet<int>& selectedFiles, PieceStrategy strategy) {
    QFile in(torrentPath);
    if (!in.open(QIODevice::ReadOnly)) return QUuid();
    const QByteArray bytes = in.readAll();
    in.close();

    bool ok = false;
    TorrentMetainfo meta = TorrentMetainfo::parse(bytes, &ok);
    if (!ok) return QUuid();          // malformed .torrent: nothing added (spec)

    // Dedup: a task for this info-hash already exists -> return it, add nothing.
    for (AbstractTask* t : m_tasks)
        if (auto* tt = qobject_cast<TorrentTask*>(t))
            if (tt->metainfo().infoHash == meta.infoHash)
                return tt->id();

    const QString hex = QString::fromLatin1(meta.infoHash.toHex());
    QDir().mkpath(torrentsDir());
    const QString storedPath = torrentsDir() + "/" + hex + ".torrent";
    Persistence::writeFileAtomic(storedPath, bytes);   // makes resume independent of the original file

    auto* t = makeTorrentTask(meta, destDir, selectedFiles, strategy);
    wire(t);
    m_tasks.append(t);
    m_torrentMeta.insert(t->id(), TorrentSessionMeta{destDir, selectedFiles, strategy});
    saveSession();
    pump();
    return t->id();
}

void DownloadManager::pump() {
    // Re-entrancy guard: requeue() (used by resumeAll()) moves a Paused/Error
    // task straight back to Queued without touching m_probed or m_segments,
    // so a subsequent t->start() from the loop below can resume via
    // beginSegments() *synchronously* - and reach a terminal state (Error, if
    // the destination file can't be reopened; Completed, if every segment was
    // already done on disk) before start() even returns. wire()'s
    // stateChanged handler calls pump() again for exactly those terminal
    // states, so that nested pump() call happens while this very loop is
    // still mid-iteration. Without a guard the nested pass promotes tasks
    // that are invisible to the outer loop's hoisted `active` count, letting
    // the two calls jointly promote more than maxConcurrentDownloads at once.
    // The guard makes the nested call a no-op; Part B below (recomputing
    // `active` every iteration) then makes sure the outer loop still sees an
    // up-to-date count that reflects whatever the guarded-out nested call
    // would have changed (e.g. an Error/Completed task no longer counts as
    // active) before deciding whether to promote the next Queued task.
    if (m_inPump) return;
    m_inPump = true;
    struct Guard { bool& b; ~Guard() { b = false; } } guard{m_inPump};

    // Promove os Queued em ordem de prioridade (High->Normal->Low), estável
    // dentro de cada nível (std::stable_sort preserva a ordem de inserção).
    QVector<AbstractTask*> queued;
    for (auto* t : m_tasks)
        if (t->state() == DownloadState::Queued) queued.append(t);
    std::stable_sort(queued.begin(), queued.end(),
        [](AbstractTask* x, AbstractTask* y){ return int(x->priority()) < int(y->priority()); });

    for (auto* t : queued) {
        int active = 0;
        for (auto* u : m_tasks)
            if (u->state() == DownloadState::Downloading || u->state() == DownloadState::Connecting)
                ++active;
        if (active >= m_cfg.maxConcurrentDownloads) break;
        t->start();
    }
}

void DownloadManager::pauseAll() {
    for (auto* t : m_tasks)
        if (t->state() == DownloadState::Downloading || t->state() == DownloadState::Connecting)
            t->pause();
    saveSession();
}

void DownloadManager::resumeAll() {
    // Route resume through pump()'s cap instead of starting every paused/
    // error task directly. DownloadTask::requeue() flips Paused/Error back
    // to Queued (emitting stateChanged(Queued) but starting nothing); the
    // handler in wire() saves the session but only calls pump() for
    // Completed/Error/Paused - Queued is deliberately excluded from that
    // trigger set - so this loop cannot recursively promote anything while
    // it runs. Calling pump() once, after every task has been requeued,
    // promotes up to maxConcurrentDownloads; the rest stay Queued and get
    // promoted incrementally as each active task later reaches a terminal-
    // ish state via the same handler.
    for (auto* t : m_tasks)
        if (t->state() == DownloadState::Paused || t->state() == DownloadState::Error)
            t->requeue();
    pump();
}

AbstractTask* DownloadManager::taskById(const QUuid& id) const {
    for (AbstractTask* t : m_tasks)
        if (t->id() == id) return t;
    return nullptr;
}

void DownloadManager::pause(const QUuid& id) {
    // Queued/Connecting/Downloading all call t->pause() so that pausing a
    // Queued task genuinely HOLDS it: without this, pausing a Queued
    // download was a no-op and pump() would later auto-promote it to
    // Downloading the moment a slot freed up, silently defeating the pause.
    // This is safe against the spurious-.meta hazard (see remove()'s NOTE
    // below) because DownloadTask::pause() itself now guards the .meta write
    // behind `!m_segments.isEmpty()` - a never-started Queued task simply
    // writes nothing and lands at Paused.
    // saveSession() only runs when a task's state actually changed, so a
    // no-op call (Paused/Completed/Error) doesn't trigger a needless disk
    // write.
    AbstractTask* t = taskById(id);
    if (!t) return;
    switch (t->state()) {
        case DownloadState::Queued:
        case DownloadState::Connecting:
        case DownloadState::Downloading:
            t->pause();
            saveSession();
            break;
        default: break;   // Paused/Completed/Error: no-op
    }
}

void DownloadManager::resume(const QUuid& id) {
    // Route through requeue() + pump() - never t->start() directly - so a
    // resumed task is still subject to maxConcurrentDownloads. See pump()'s
    // re-entrancy guard for why this is safe even when requeue() lets a
    // restored task reach a terminal state synchronously inside start().
    AbstractTask* t = taskById(id);
    if (!t) return;
    if (t->state() == DownloadState::Paused || t->state() == DownloadState::Error ||
        t->state() == DownloadState::Cancelled) {
        t->requeue();     // -> Queued
        pump();           // promotes Queued -> Downloading only up to the cap
    }
}

// Cancela um download: descarta parcial + .meta e leva a task a Cancelled
// (ela permanece na lista - remove() é o caminho separado para excluí-la de
// vez). Completed/Cancelled não têm nada a cancelar.
void DownloadManager::cancel(const QUuid& id) {
    AbstractTask* t = taskById(id);
    if (!t) return;
    if (t->state() == DownloadState::Completed || t->state() == DownloadState::Cancelled)
        return;                        // nada a cancelar
    t->cancel();
    saveSession();
    pump();                            // um slot pode ter liberado
}

void DownloadManager::setPriority(const QUuid& id, Priority p) {
    AbstractTask* t = taskById(id);
    if (!t) return;
    t->setPriority(p);
    saveSession();
    pump();                 // a reordenação pode mudar quem é promovido a seguir
}

// Move os arquivos (destPath final/parcial + sidecar .meta) para newDir.
// Recusa (no-op) enquanto o download está ativo, pois setDestPath fecha/
// apaga o m_file aberto — mover embaixo de um download em andamento
// corromperia o estado.
bool DownloadManager::moveFiles(const QUuid& id, const QString& newDir) {
    // moveFiles is a DownloadTask-specific operation (record()/setDestPath()
    // aren't on AbstractTask); today every task is a DownloadTask, so the
    // cast never actually fails.
    DownloadTask* t = qobject_cast<DownloadTask*>(taskById(id));
    if (!t) return false;
    const DownloadState s = t->state();
    if (s == DownloadState::Downloading || s == DownloadState::Connecting)
        return false;                          // só com download parado
    const QString oldPath  = t->record().destPath;
    // Mover para a MESMA pasta é no-op: sem isto, resolveUniquePath veria o
    // próprio arquivo como colisão e o renomearia para "nome (1).ext".
    if (QFileInfo(oldPath).absolutePath() == QDir(newDir).absolutePath())
        return true;
    const QString fileName = QFileInfo(oldPath).fileName();
    const QString newPath = Persistence::resolveUniquePath(QDir(newDir).filePath(fileName));
    if (QFileInfo::exists(oldPath) && !QFile::rename(oldPath, newPath))
        return false;
    const QString oldMeta = Persistence::metaPath(oldPath);
    if (QFileInfo::exists(oldMeta))
        QFile::rename(oldMeta, Persistence::metaPath(newPath));
    t->setDestPath(newPath);
    saveSession();
    return true;
}

// Retarget a download to a new FULL path (directory and/or basename), preserving
// bytes already fetched. Unlike moveFiles (dir-only, keeps basename, refuses while
// active), retarget pauses an active download, renames+moves the partial + .meta,
// updates the path, and resumes. Safe for Completed/Paused/Queued (pause/resume are
// no-ops there). Returns false on IO failure, leaving the task resumed at old path.
bool DownloadManager::retarget(const QUuid& id, const QString& newDestPath) {
    // Same DownloadTask-specific rationale as moveFiles() above.
    DownloadTask* t = qobject_cast<DownloadTask*>(taskById(id));
    if (!t) return false;
    const QString oldPath = t->record().destPath;
    if (newDestPath == oldPath) return true;               // no change requested
    const QString finalPath = Persistence::resolveUniquePath(newDestPath);
    const DownloadState s = t->state();
    const bool wasActive = (s == DownloadState::Downloading || s == DownloadState::Connecting);
    if (wasActive) pause(id);                              // safe hold; stops workers/writes
    if (QFileInfo::exists(oldPath) && !QFile::rename(oldPath, finalPath)) {
        if (wasActive) resume(id);
        return false;
    }
    const QString oldMeta = Persistence::metaPath(oldPath);
    if (QFileInfo::exists(oldMeta))
        QFile::rename(oldMeta, Persistence::metaPath(finalPath));
    t->setDestPath(finalPath);
    saveSession();
    if (wasActive) resume(id);
    return true;
}

// Credenciais vivem SÓ em memória, nunca no .meta (spec §3.6): senha em texto
// puro no disco não. Depois de recarregar a sessão, a app pergunta de novo.
void DownloadManager::provideCredentials(const QUuid& id, const QString& user, const QString& pass) {
    // setCredentials() is DownloadTask-specific (HTTP/FTP auth, spec §3.6).
    DownloadTask* t = qobject_cast<DownloadTask*>(taskById(id));
    if (!t) return;
    t->setCredentials(Credentials{user, pass});
    resume(id);                 // requeue + pump: respeita o cap de concorrência
}

void DownloadManager::remove(const QUuid& id, bool deleteFiles) {
    for (int i = 0; i < m_tasks.size(); ++i) {
        if (m_tasks[i]->id() != id) continue;
        AbstractTask* t = m_tasks[i];
        // NOTE (deviation from the brief's sample): the brief calls
        // t->pause() here unconditionally. DownloadTask::pause() forces the
        // state to Paused and rewrites a .meta file regardless of the prior
        // state, so pausing a Queued task fabricates a spurious empty .meta
        // for a download that never started, and pausing an already
        // Completed task resurrects a .meta that checkAllComplete() had
        // already removed - corrupting a finished download's on-disk state
        // moments before it's removed. Guarding this the same way
        // pauseAll() does (only stop tasks that are actually
        // Downloading/Connecting) avoids both, with no change to remove()'s
        // signature or behavior for the case that matters (an in-flight
        // download).
        if (t->state() == DownloadState::Downloading || t->state() == DownloadState::Connecting)
            t->pause();
        // Compute per-kind deletion targets BEFORE removing/deleting the task.
        // record().destPath is DownloadTask-specific; guarded for a future
        // non-DownloadTask Kind (nothing to delete off disk for those today).
        QString dest;
        if (auto* dt = qobject_cast<DownloadTask*>(t)) dest = dt->record().destPath;

        // A TorrentTask has TWO on-disk footprints the DownloadTask path never
        // touched: the payload root (<destDir>/<name>, a directory for a
        // multi-file torrent) and the app-internal resume artifacts stored
        // under <dataDir>/torrents/<hexInfoHash>.{torrent,bitfield}. Without
        // handling this, remove(deleteFiles=true) on a torrent deleted nothing
        // (the DownloadTask cast is null, so dest was empty).
        bool    isTorrent = false;
        QString torrentPayloadRoot, storedTorrent, storedBitfield;
        if (auto* tt = qobject_cast<TorrentTask*>(t)) {
            isTorrent = true;
            torrentPayloadRoot = tt->payloadRootPath();
            const QString hex = QString::fromLatin1(tt->metainfo().infoHash.toHex());
            storedTorrent  = torrentsDir() + "/" + hex + ".torrent";
            storedBitfield = torrentsDir() + "/" + hex + ".bitfield";
        }

        m_tasks.removeAt(i);
        m_torrentMeta.remove(id);   // no-op for non-torrent tasks
        t->deleteLater();

        if (isTorrent) {
            // Payload is user data: delete it only when deleteFiles (mirrors the
            // download keep-files semantics). Recursively for a multi-file
            // torrent (payload root is a directory), as a plain file otherwise.
            if (deleteFiles && !torrentPayloadRoot.isEmpty()) {
                QFileInfo fi(torrentPayloadRoot);
                if (fi.isDir()) QDir(torrentPayloadRoot).removeRecursively();
                else            QFile::remove(torrentPayloadRoot);
            }
            // The .torrent/.bitfield are app-internal resume artifacts, not user
            // data: always cleaned on any remove, or they orphan forever.
            QFile::remove(storedTorrent);
            QFile::remove(storedBitfield);
        } else if (deleteFiles) {
            QFile::remove(dest);
            Persistence::removeMeta(dest);
        }
        break;
    }
    saveSession();
    pump();
}

void DownloadManager::saveSession() {
    // record() is DownloadTask-specific; every task is one today. A future
    // non-DownloadTask Kind would need its own persistence path here.
    QVector<DownloadRecord> recs;
    for (auto* t : m_tasks)
        if (auto* dt = qobject_cast<DownloadTask*>(t)) recs.append(dt->record());
    Persistence::writeSession(sessionPath(), recs);
    saveTorrentSession();
}

// Torrents are round-tripped through a SEPARATE file (<dataDir>/torrents.json)
// rather than folded into downloads.json: Persistence::writeSession/readSession
// have their own tests (tst_persistence) and tst_download pokes downloads.json
// directly with a bare JSON array (see tst_download.cpp's cbad-record setup),
// so changing that file's root shape (array -> object) would break both. A
// companion file is a strictly additive, zero-risk way to satisfy "session
// gains a torrents array; absent -> zero torrents" (spec §10/§12): absent file
// -> loadTorrentSession() reads an empty object -> zero torrents, and every
// existing download record/session path is untouched.
void DownloadManager::saveTorrentSession() {
    QJsonArray arr;
    for (auto* t : m_tasks) {
        auto* tt = qobject_cast<TorrentTask*>(t);
        if (!tt) continue;
        const TorrentSessionMeta meta = m_torrentMeta.value(tt->id());
        QJsonArray sel;
        for (int i : meta.selectedFiles) sel.append(i);
        arr.append(QJsonObject{
            {"infoHash", QString::fromLatin1(tt->metainfo().infoHash.toHex())},
            {"destDir", meta.destDir},
            {"selectedFiles", sel},
            {"strategy", strategyToString(meta.strategy)},
            {"state", int(tt->state())}});
    }
    // Don't materialize an (empty-array) torrents.json for the common
    // pure-HTTP/FTP user who has never added a torrent - only start writing
    // it once there's at least one torrent to record. But if the file
    // already exists (a torrent was added at some point this session/a prior
    // one), keep writing it even when `arr` goes back to empty - otherwise
    // removing the last torrent would leave a STALE torrents.json on disk
    // whose old entry loadTorrentSession() would incorrectly resurrect on
    // the next loadSession().
    if (arr.isEmpty() && !QFile::exists(torrentsSessionPath())) return;
    Persistence::writeJsonObject(torrentsSessionPath(), QJsonObject{{"torrents", arr}});
}

void DownloadManager::loadTorrentSession() {
    const QJsonObject root = Persistence::readJsonObject(torrentsSessionPath());
    for (const QJsonValue& v : root.value("torrents").toArray()) {
        const QJsonObject o = v.toObject();
        const QString hex = o.value("infoHash").toString();
        const QString destDir = o.value("destDir").toString();
        const PieceStrategy strategy = strategyFromString(o.value("strategy").toString());
        const DownloadState state = DownloadState(o.value("state").toInt(int(DownloadState::Queued)));
        if (state == DownloadState::Completed) continue;       // nothing to resume

        QSet<int> selectedFiles;
        for (const QJsonValue& fv : o.value("selectedFiles").toArray())
            selectedFiles.insert(fv.toInt());

        const QString storedPath = torrentsDir() + "/" + hex + ".torrent";
        QFile in(storedPath);
        if (!in.open(QIODevice::ReadOnly)) {
            if (m_logger) m_logger->logApp(LogLevel::Warn,
                QStringLiteral("session: torrent %1 skipped - stored .torrent missing").arg(hex));
            continue;
        }
        const QByteArray bytes = in.readAll();
        in.close();
        bool ok = false;
        TorrentMetainfo meta = TorrentMetainfo::parse(bytes, &ok);
        if (!ok) {
            if (m_logger) m_logger->logApp(LogLevel::Warn,
                QStringLiteral("session: torrent %1 skipped - stored .torrent failed to parse").arg(hex));
            continue;
        }

        auto* t = makeTorrentTask(meta, destDir, selectedFiles, strategy);
        // When the user chose to re-check on open, the Checking pass run inside
        // start() is the authoritative source of truth for what is actually on
        // disk; do NOT pre-seed a TRUSTED bitfield that a corrupt-on-disk piece
        // could then hide behind (that would silently degrade RecheckOnOpen to
        // TrustBitfield). runChecking() re-verifies every wanted piece from the
        // file itself, so it needs no restored bitfield to know what to check.
        // For TrustBitfield, adopt the saved progress as before.
        if (t->verifyMode() != ResumeVerifyMode::RecheckOnOpen)
            t->restoreBitfield();

        // Apply the persisted state via TorrentTask's own public API (never
        // a direct field poke) so pause()/cancel()'s bookkeeping (tracker
        // Stopped announce - a no-op here since nothing has started yet -
        // and re-flushing the just-restored bitfield) runs consistently.
        // Mirrors DownloadTask::restore()'s "anything not Cancelled comes
        // back Paused, requires deliberate resume" rule - a torrent left
        // Queued (never started before the session was saved) is the one
        // exception, since Queued already means "not running" and pump()
        // naturally promotes it later, same as a freshly-added torrent.
        if (state == DownloadState::Cancelled) {
            t->cancel();
        } else if (state != DownloadState::Queued) {
            t->pause();
        }

        wire(t);
        m_tasks.append(t);
        m_torrentMeta.insert(t->id(), TorrentSessionMeta{destDir, selectedFiles, strategy});
    }
}

void DownloadManager::loadSession() {
    const auto recs = Persistence::readSession(sessionPath());
    for (const auto& rec : recs) {
        if (rec.state == DownloadState::Completed) continue;   // nothing to resume
        Transport* tr = transportFor(rec.url);
        if (!tr) continue;            // registro órfão de esquema desconhecido: ignora
        QVector<Segment> segs; QString etag, lm; bool validated = false;
        Persistence::readMeta(rec.destPath, segs, etag, lm, validated);
        auto* t = new DownloadTask(tr, m_cfg, &m_limiter, this);
        t->restore(rec, segs, etag, lm, validated);
        wire(t);
        t->setLogger(m_logger);
        m_tasks.append(t);
    }
    loadTorrentSession();
}

// Banda (m_limiter) e cap de concorrência (via pump()) aplicam ao vivo, a
// downloads já em andamento. Os demais campos de EngineConfig (timeouts,
// segmentCount, etc.) só valem para tasks criadas DEPOIS desta chamada -
// tasks já em curso mantêm o m_cfg (por valor) que receberam na criação.
void DownloadManager::setConfig(const EngineConfig& cfg) {
    m_cfg = cfg;                             // vale para PRÓXIMOS downloads
    m_limiter.setRate(cfg.maxBytesPerSec);   // banda: ao vivo
    pump();                                  // cap de concorrência: ao vivo
}
