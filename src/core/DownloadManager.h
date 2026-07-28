#pragma once
#include "DownloadTypes.h"
#include "AbstractTask.h"
#include "DownloadTask.h"
#include "Transport.h"
#include "RateLimiter.h"
#include "torrent/MagnetUri.h"   // MagnetInfo - held by value in MagnetSessionMeta
#include <QObject>
#include <QVector>
#include <QHash>
#include <QSet>
#include <memory>
#include <vector>

class Logger;
class QNetworkAccessManager;
class TorrentTask;
class DhtNode;
class MetadataFetch;
struct TorrentMetainfo;

class DownloadManager : public QObject {
    Q_OBJECT
public:
    DownloadManager(const EngineConfig& cfg, const QString& dataDir,
                    Logger* logger = nullptr, QObject* parent = nullptr);
    // §6.3: persists the live DhtNode's id + routing table (dht.dat) on
    // shutdown, guarded to a manager-owned node that actually exists (DHT
    // disabled this session, or a test double installed via
    // setDhtForTest(), leave nothing to save here -- see that method's
    // "caller keeps it alive" contract, mirrored by the `parent() == this`
    // check below).
    ~DownloadManager() override;
    QUuid addDownload(const QUrl& url, const QString& destPath, const HeaderList& extraHeaders = {},
                      bool provisionalName = false);
    // Parses `torrentPath`, copies it into <dataDir>/torrents/<hexInfoHash>.torrent
    // (so resume never depends on the user's original file), and constructs +
    // wires a TorrentTask. Returns a null QUuid (nothing added) if the file
    // can't be read or fails to parse as a valid .torrent. A second call with
    // the same info-hash returns the existing task's id instead of duplicating it.
    QUuid addTorrent(const QString& torrentPath, const QString& destDir,
                      const QSet<int>& selectedFiles, PieceStrategy strategy);
    // Task 15: resolves `uri` (a magnet: link) into a running TorrentTask.
    // Parses the magnet immediately (a null QUuid means malformed - nothing
    // added, mirrors addTorrent()'s contract for an unparsable .torrent); a
    // valid magnet's id is derived from its info-hash the SAME way
    // TorrentTask derives its own id (QUuid::fromRfc4122(infoHash.left(16))),
    // so the id returned here is the FINAL task's id from the start, even
    // though the task behind it doesn't exist yet. Until MetadataFetch
    // resolves the info dict, tasks()/taskById(id) surface a lightweight
    // FetchingMetadata placeholder (displayName/state only - no progress);
    // on success it's replaced (same id) by the normal TorrentTask
    // makeTorrentTask() builds (wired to DHT exactly like addTorrent()), and
    // the reconstructed .torrent is cached to torrents/<hexInfoHash>.torrent
    // so a restart never needs to re-run MetadataFetch for it. On failure the
    // placeholder is marked Error and left in place (retried on the next
    // loadSession()). `selectedFiles` empty (the default) means ALL files -
    // callers can't validate indices against a torrent whose file count
    // isn't known until it resolves; an interactive per-file selection
    // dialog is the GUI's job once resolved, not this call's.
    QUuid addMagnet(const QString& uri, const QString& destDir, PieceStrategy strategy,
                    const QVector<int>& selectedFiles = {});
    void  pauseAll();
    void  resumeAll();
    void  remove(const QUuid& id, bool deleteFiles);
    void  loadSession();
    void  setConfig(const EngineConfig& cfg);   // banda + cap ao vivo; resto p/ próximos downloads
    // Plain values (not the GUI's BitTorrentPrefs struct — core can't depend on
    // gui/Settings.h): applies to torrents added/resumed AFTER this call, same
    // "resto p/ próximos downloads" contract as setConfig() above.
    void  setTorrentDefaults(int maxPeersPerTorrent, quint16 listenPort, ResumeVerifyMode verify);
    QVector<AbstractTask*> tasks() const { return m_tasks; }
    AbstractTask* taskById(const QUuid& id) const;
    void  pause(const QUuid& id);
    void  resume(const QUuid& id);
    void  cancel(const QUuid& id);
    void  setPriority(const QUuid& id, Priority p);
    bool  moveFiles(const QUuid& id, const QString& newDir);
    bool  retarget(const QUuid& id, const QString& newDestPath);
    void  provideCredentials(const QUuid& id, const QString& user, const QString& pass);
    Transport* transportFor(const QUrl& url) const;   // nullptr se esquema desconhecido

    // TEST SEAM (Task 14): swap the real, internet-bootstrapping DhtNode this
    // manager creates at construction (when m_dhtEnabled) for an in-process
    // one the test fully controls (e.g. bootstrapped only to a local
    // holder). MUST be called before any addTorrent()/loadTorrentSession() --
    // a TorrentTask already built before the swap keeps pointing at (and
    // stays subscribed to) whatever DhtNode existed when it was made; this
    // does not retroactively re-wire it. `dht` is NOT owned/deleted by this
    // manager -- the caller keeps it alive at least as long as this
    // DownloadManager and every TorrentTask it goes on to build.
    void  setDhtForTest(DhtNode* dht);

    // Task 17 hook: fires the real, internet-bootstrapping DHT join
    // (DhtNode::kDefaultRouters) on the DhtNode this manager created at
    // construction. The constructor only start()s that node (binds the UDP
    // socket locally; no outbound traffic) -- nothing calls this yet. Meant
    // to be invoked by the settings-gated wiring once DHT-enable is a real
    // Preferences toggle. No-op if m_dht is null (DHT disabled, or already
    // swapped for a test double via setDhtForTest()).
    void  startDhtBootstrap();

    // Task 17 live toggle: makes m_dht match `enabled`. Turning it ON creates
    // the shared DhtNode if one doesn't already exist (or re-binds it if it's
    // bound to a stale port -- e.g. the ctor's ephemeral bind before the
    // configured port was known), rewires every live TorrentTask to it, and
    // fires startDhtBootstrap(). Turning it OFF detaches every live
    // TorrentTask (setDht(nullptr)) and destroys the node (only if this
    // manager owns it -- a test double installed via setDhtForTest() is never
    // deleted here, matching that method's "caller keeps it alive" contract).
    // Safe to call repeatedly / redundantly (e.g. every Preferences "OK").
    void  setDhtEnabled(bool enabled);
    // Task 17 live toggle: sets the UDP port future (re)binds should use. If
    // DHT is currently enabled and the live node isn't already bound to
    // `port`, rebinds immediately (tearing down + recreating m_dht, rewiring
    // every live TorrentTask, and re-bootstrapping) -- otherwise just records
    // the value for the next setDhtEnabled(true)/rebind.
    void  setDhtPort(quint16 port);
    // Task 17 convenience: applies both in one shot. Equivalent to calling
    // setDhtPort() then setDhtEnabled(), but without the intermediate rebind
    // + bootstrap setDhtEnabled(true) alone would do at whatever port was
    // configured BEFORE this call, when both the enabled flag and the port
    // change together (e.g. Preferences applied for the first time with a
    // non-default port) -- exactly one rebind/bootstrap either way.
    void  setDhtConfig(bool enabled, quint16 port);
    // Diagnostics for the Preferences BitTorrent page: routing-table size, or
    // 0 when DHT is disabled/not yet created.
    int   dhtNodeCount() const;
signals:
    void taskProgress(const QUuid& id, qint64 received, qint64 total);
    void taskStateChanged(const QUuid& id, DownloadState state);
    void credentialsRequired(const QUuid& id, const QString& host);
    // Final-review Fix C1: a magnet's transient MagnetTask placeholder is
    // deleteLater()'d and replaced by a brand-new TorrentTask carrying the
    // SAME id (see onMetadataReady()) - any observer that cached the OLD
    // AbstractTask* for this id (namely DownloadTableModel's Row::task) is
    // left pointing at freed memory the moment that deleteLater() actually
    // runs, unless it re-fetches taskById(id) after this fires. Emitted
    // AFTER the swap (placeholder gone from m_tasks, new TorrentTask
    // appended + wired) so taskById(id) already returns the new task.
    void taskReplaced(const QUuid& id);
private:
    QString sessionPath() const;
    QString torrentsSessionPath() const;   // separate file: <dataDir>/torrents.json
    QString torrentsDir() const;           // <dataDir>/torrents (copied .torrent + resume files)
    void    saveSession();
    void    saveTorrentSession();
    void    loadTorrentSession();
    void    pump();                 // promote Queued -> Downloading up to maxConcurrent
    void    wire(AbstractTask* t);
    // Shared TorrentTask construction (addTorrent() + loadTorrentSession()):
    // one place for the listen-port/maxPeers/verify-mode defaults and the
    // rngSeed/nam/limiter/logger/resumeDir wiring, so the two call sites can't
    // drift once EngineConfig eventually grows torrent-specific fields.
    TorrentTask* makeTorrentTask(const TorrentMetainfo& meta, const QString& destDir,
                                 const QSet<int>& selectedFiles, PieceStrategy strategy);

    // Task 15: shared by addMagnet() and loadTorrentSession()'s magnet-restore
    // branch. Records `mi`/destDir/selectedFiles/strategy in m_magnetMeta
    // (keyed by `id`, the info-hash-derived id addMagnet() already computed),
    // appends the transient FetchingMetadata placeholder to m_tasks, and
    // starts a MetadataFetch wired to onMetadataReady()/onMetadataFailed().
    void startMetadataFetch(const QUuid& id, const MagnetInfo& mi, const QString& destDir,
                            const QSet<int>& selectedFiles, PieceStrategy strategy);
    // MetadataFetch::metainfoReady handler: caches the resolved .torrent
    // (spliced verbatim from `infoDict` via
    // TorrentMetainfo::wrapInfoDictAsTorrent -- NEVER re-encoded from `meta`'s
    // own fields, which would silently drop any info-dict key this codebase
    // doesn't itself recognize and corrupt the info-hash on a later
    // restore), swaps the FetchingMetadata placeholder for a real
    // TorrentTask (same id - see addMagnet()'s doc comment), and drops the
    // now-resolved entry from m_magnetMeta. A no-op if `id` isn't in
    // m_magnetMeta any more (the magnet was removed while the fetch was
    // still in flight).
    void onMetadataReady(const QUuid& id, const TorrentMetainfo& meta, const QByteArray& infoDict);
    // MetadataFetch::failed handler: marks the placeholder Error and logs;
    // m_magnetMeta keeps the entry (still persisted, still retried on the
    // next loadSession()) rather than giving up on it permanently. Same
    // remove()-raced no-op guard as onMetadataReady().
    void onMetadataFailed(const QUuid& id, const QString& reason);

    // Per-torrent fields addTorrent()/loadTorrentSession() know but TorrentTask
    // itself doesn't re-expose (destDir/selectedFiles/strategy as originally
    // requested) - kept here, keyed by task id, purely for session round-trips.
    struct TorrentSessionMeta {
        QString    destDir;
        QSet<int>  selectedFiles;
        PieceStrategy strategy = PieceStrategy::RarestFirst;
    };
    QHash<QUuid, TorrentSessionMeta> m_torrentMeta;

    // Task 15: an unresolved magnet's own session-round-trip fields, keyed by
    // the SAME id its (eventual) TorrentTask will use. Present from
    // addMagnet()/loadTorrentSession()'s restore branch until
    // onMetadataReady() resolves it (entry removed - it becomes a normal
    // m_torrentMeta entry instead) or the magnet is remove()d outright
    // (entry removed there too, so a stale one can't keep reappearing in
    // torrents.json). selectedFiles empty means ALL files once resolved.
    struct MagnetSessionMeta {
        MagnetInfo    info;
        QString       destDir;
        QSet<int>     selectedFiles;
        PieceStrategy strategy = PieceStrategy::RarestFirst;
    };
    QHash<QUuid, MagnetSessionMeta> m_magnetMeta;
    // The in-flight MetadataFetch for each unresolved magnet id - owned
    // (parented to `this`), but ALSO explicitly deleteLater()'d from
    // onMetadataReady()/onMetadataFailed()/remove() the moment it's done or
    // no longer wanted, rather than waiting for `this` to be destroyed (a
    // magnet the user removes must stop its background DHT lookup/peer
    // connections right away, not linger for the DownloadManager's lifetime).
    QHash<QUuid, MetadataFetch*>    m_metadataFetches;

    EngineConfig            m_cfg;
    QString                 m_dataDir;
    // Torrent defaults threaded through makeTorrentTask(); initialized to the
    // engine's previous hardcoded values, overridable via setTorrentDefaults().
    int                     m_torrentMaxPeers    = 50;
    quint16                 m_torrentListenPort  = 6881;
    ResumeVerifyMode        m_torrentVerify      = ResumeVerifyMode::TrustBitfield;
    RateLimiter             m_limiter;       // teto global de banda, consultado pelos workers
    QHash<QString, Transport*>              m_transports;   // scheme -> transport (não-dono)
    std::vector<std::unique_ptr<Transport>> m_owned;        // dono de verdade
    QVector<AbstractTask*>  m_tasks;
    bool                    m_inPump = false;
    Logger*                 m_logger = nullptr;   // não-dono; pode ser nullptr
    QNetworkAccessManager*  m_torrentNam = nullptr; // owned (parented to this); shared by all TorrentTasks

    // Task 14: DHT as a peer source, shared by every TorrentTask. Gate is a
    // plain bool for now (defaults on); the real Preferences-driven toggle is
    // Task 17. m_dht is owned (parented to this) when this manager created
    // it; setDhtForTest() may replace it with a caller-owned instance instead
    // (see that method's contract).
    bool                    m_dhtEnabled = true;
    DhtNode*                m_dht = nullptr;
    // Task 17: UDP port for m_dht, settings-driven (default matches
    // DhtSettings::port in the GUI's Settings.h). Only takes effect on the
    // NEXT (re)bind -- see setDhtPort()/setDhtEnabled().
    quint16                 m_dhtPort = 6881;

    // Task 17 shared helper: tears down the current m_dht (if this manager
    // owns it) and creates+start()s a fresh one bound to m_dhtPort, rewiring
    // every live TorrentTask to the new instance. Does NOT bootstrap -- callers
    // (setDhtEnabled()/setDhtPort()) decide that afterward. Never touches a
    // test-injected (caller-owned) m_dht's lifetime, only detaches from it.
    void                    rebindDht();
};
