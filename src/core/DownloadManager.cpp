#include "DownloadManager.h"
#include "HttpTransport.h"
#include "FtpTransport.h"
#include "Persistence.h"
#include "Logger.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
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

    QDir().mkpath(m_dataDir);
}

Transport* DownloadManager::transportFor(const QUrl& url) const {
    return m_transports.value(url.scheme().toLower(), nullptr);
}

QString DownloadManager::sessionPath() const { return m_dataDir + "/downloads.json"; }

void DownloadManager::wire(DownloadTask* t) {
    connect(t, &DownloadTask::progress, this, [this, t](qint64 r, qint64 tot) {
        emit taskProgress(t->id(), r, tot);
    });
    connect(t, &DownloadTask::stateChanged, this, [this, t](DownloadState s) {
        if (m_logger)
            m_logger->logTask(t->id(), t->record().destPath,
                              s == DownloadState::Error ? LogLevel::Error : LogLevel::Info,
                              QStringLiteral("state -> %1").arg(stateName(s)));
        emit taskStateChanged(t->id(), s);
        saveSession();
        if (s == DownloadState::Completed || s == DownloadState::Error ||
            s == DownloadState::Paused)
            pump();                       // a slot may have freed up
    });
    connect(t, &DownloadTask::credentialsRequired, this,
            [this](const QUuid& id, const QString& host) {
        emit credentialsRequired(id, host);
    });
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
    QVector<DownloadTask*> queued;
    for (auto* t : m_tasks)
        if (t->state() == DownloadState::Queued) queued.append(t);
    std::stable_sort(queued.begin(), queued.end(),
        [](DownloadTask* x, DownloadTask* y){ return int(x->priority()) < int(y->priority()); });

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

DownloadTask* DownloadManager::taskById(const QUuid& id) const {
    for (DownloadTask* t : m_tasks)
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
    DownloadTask* t = taskById(id);
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
    DownloadTask* t = taskById(id);
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
    DownloadTask* t = taskById(id);
    if (!t) return;
    if (t->state() == DownloadState::Completed || t->state() == DownloadState::Cancelled)
        return;                        // nada a cancelar
    t->cancel();
    saveSession();
    pump();                            // um slot pode ter liberado
}

void DownloadManager::setPriority(const QUuid& id, Priority p) {
    DownloadTask* t = taskById(id);
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
    DownloadTask* t = taskById(id);
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
    DownloadTask* t = taskById(id);
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
    DownloadTask* t = taskById(id);
    if (!t) return;
    t->setCredentials(Credentials{user, pass});
    resume(id);                 // requeue + pump: respeita o cap de concorrência
}

void DownloadManager::remove(const QUuid& id, bool deleteFiles) {
    for (int i = 0; i < m_tasks.size(); ++i) {
        if (m_tasks[i]->id() != id) continue;
        DownloadTask* t = m_tasks[i];
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
        const QString dest = t->record().destPath;
        m_tasks.removeAt(i);
        t->deleteLater();
        if (deleteFiles) { QFile::remove(dest); Persistence::removeMeta(dest); }
        break;
    }
    saveSession();
    pump();
}

void DownloadManager::saveSession() {
    QVector<DownloadRecord> recs;
    for (auto* t : m_tasks) recs.append(t->record());
    Persistence::writeSession(sessionPath(), recs);
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
