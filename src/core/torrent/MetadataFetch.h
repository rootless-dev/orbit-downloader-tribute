#pragma once

#include "torrent/MagnetUri.h"
#include "torrent/TorrentMetainfo.h"
#include "torrent/TrackerTypes.h" // PeerAddress

#include <QByteArray>
#include <QMetaObject>
#include <QObject>
#include <QSet>
#include <QString>
#include <QVector>

class DhtNode;
class QNetworkAccessManager;
class QTimer;
class RateLimiter;
class PeerConnection;
class AnnounceController;

// Orchestrates BEP 9 magnet-link metadata (info dict) recovery.
//
// Given only a MagnetInfo (info-hash + display name + trackers -- no piece
// data at all), MetadataFetch gathers candidate peers via a DHT lookup
// (DhtNode::lookup/peersFound, skipped entirely when dht == nullptr) and a
// tracker announce (AnnounceController, skipped when the magnet carries no
// trackers), opens metadata-mode PeerConnections to them up to a bounded
// concurrency cap, and on the FIRST peer whose ut_metadata fetch completes
// and SHA-1-verifies (PeerConnection::metadataComplete), builds a full
// TorrentMetainfo via TorrentMetainfo::fromInfoDict and tears every other
// in-flight connection down. If every attempted peer fails/disconnects
// without ever yielding metadata (attempt cap exhausted) or nothing pans out
// within a bounded overall timeout, emits `failed` instead.
//
// metainfoReady carries BOTH the parsed TorrentMetainfo AND the verbatim,
// SHA-1-verified raw info dict bytes it was parsed from: callers that need to
// persist a byte-exact .torrent (e.g. DownloadManager caching one for a
// future restart) must splice in THESE bytes -- via
// TorrentMetainfo::wrapInfoDictAsTorrent -- rather than re-encoding from
// TorrentMetainfo's own (necessarily incomplete) fields, or any info-dict key
// this codebase doesn't itself recognize (BEP 27 "private", "source",
// per-file "md5sum", ...) would silently be dropped and the cached file's
// re-derived info-hash would come out different from this one.
class MetadataFetch : public QObject {
    Q_OBJECT
public:
    MetadataFetch(const MagnetInfo& mi, DhtNode* dht, QNetworkAccessManager* nam, quint32 rngSeed,
                  RateLimiter* limiter, QObject* parent = nullptr);

    // Kicks off the DHT lookup + tracker announce (whichever collaborators
    // are available) and starts opening connections to any peer already
    // known (test-injected or discovered before start()). Idempotent: a
    // second call is a no-op.
    void start();

    // TEST SEAM: injects a peer address directly, bypassing DHT/tracker
    // discovery. Safe to call before or after start(); duplicates (already
    // known peer, by host:port) are ignored.
    void addPeerForTest(const PeerAddress& addr);

    // Final-review Fix I1: DownloadManager::rebindDht()/setDhtEnabled(false)/
    // setDhtPort() delete the shared DhtNode and build a fresh one (or none at
    // all) out from under any MetadataFetch still in flight. Mirrors
    // TorrentTask::setDht's null-safety: drops the subscription to the OLD
    // node (so a stray peersFound after this call can't touch us), adopts
    // `dht` (which may be nullptr), and either stops the periodic DHT-retry
    // timer (dht == nullptr -- nothing to retry against) or re-subscribes +
    // fires an immediate lookup() on the new node so the fetch keeps making
    // progress instead of silently stalling until the overall timeout. MUST
    // be called by the manager BEFORE it deletes the old DhtNode.
    void setDht(DhtNode* dht);

signals:
    // `infoDict` is the verbatim, SHA-1-verified raw info dict bytes `meta`
    // was parsed from (see class doc comment above) -- NOT a re-encoding.
    void metainfoReady(TorrentMetainfo meta, QByteArray infoDict);
    void failed(QString reason);

private:
    void onPeersDiscovered(const QVector<PeerAddress>& peers);
    void queuePeer(const PeerAddress& addr);
    void openMore();
    void openPeer(const PeerAddress& addr);
    void onMetadataComplete(PeerConnection* pc, const QByteArray& infoDict);
    void onPeerDone(PeerConnection* pc); // metadataFailed and/or disconnected -> drop, try more
    void finishFailed(const QString& reason);

    MagnetInfo             m_mi;
    DhtNode*               m_dht;
    QNetworkAccessManager* m_nam;
    quint32                m_rngSeed;
    RateLimiter*           m_limiter;
    QByteArray             m_peerId; // deterministic, derived from m_rngSeed (same scheme as TorrentTask)

    AnnounceController* m_announce = nullptr; // owned (QObject child); null if the magnet has no trackers

    QVector<PeerAddress>    m_pendingPeers; // discovered/injected, not yet opened
    QSet<QString>           m_knownPeers;   // dedup key: "host:port", already queued or attempted
    QVector<PeerConnection*> m_active;      // owned (QObject children); currently connecting/fetching

    bool m_started = false;
    bool m_done = false; // metainfoReady or failed already emitted; guards re-entrancy/double-emit
    int  m_attempts = 0; // total peers ever opened, towards kMaxAttempts

    QTimer* m_timeoutTimer = nullptr; // bounded overall give-up timer
    // Periodic dht->lookup() retry while unresolved (Task 15). A single
    // lookup() call right at start() almost always finds nothing: lookup()
    // only walks candidates ALREADY in the DHT's routing table at that
    // instant, but a freshly bootstrap()'d (or entirely fresh) DhtNode
    // populates its table from PING/FIND_NODE *replies*, which can only
    // arrive on a later turn of the event loop - never within the same
    // synchronous call chain that invoked bootstrap() and start() back to
    // back. Without a retry, that first empty-table walk would be the only
    // one ever made. lookup() itself is a no-op while a walk for this
    // info_hash is already in flight, so calling it again on every tick is
    // always safe - it only ever does new work once the previous walk has
    // actually finished.
    QTimer* m_dhtRetryTimer = nullptr;
    // Connection to m_dht's peersFound, tracked explicitly (rather than relying
    // on the sender-destroyed auto-disconnect) so setDht() can drop it BEFORE
    // m_dht is repointed -- a plain connect(m_dht, ...) capturing `this` as
    // context still auto-disconnects when m_dht itself is destroyed, but
    // setDht() is called precisely to avoid ever reaching that point (the old
    // node is deleted only AFTER every subscriber has already been rewired).
    QMetaObject::Connection m_dhtPeersConn;

    static constexpr int kMaxConcurrent = 10;
    static constexpr int kMaxAttempts = 50;
    static constexpr int kOverallTimeoutMs = 30000;
    static constexpr int kDhtRetryMs = 1000;
};
