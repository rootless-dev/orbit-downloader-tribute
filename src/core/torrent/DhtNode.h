#pragma once

#include "torrent/NodeId.h"
#include "torrent/RoutingTable.h"
#include "torrent/TrackerTypes.h" // PeerAddress

#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QString>
#include <QVector>

class QUdpSocket;
class QTimer;
class QHostAddress;
struct KrpcMessage;

// One peer stored in the server-side peer store for a given info_hash,
// learned via an announce_peer query. `expiryMs` is an absolute
// QDateTime::currentMSecsSinceEpoch() deadline; entries past it are swept
// out lazily on the next access to that info_hash's bucket.
struct StoredPeer {
    QString host;
    quint16 port = 0;
    qint64 expiryMs = 0;
};

// DhtNode owns a UDP socket and speaks KRPC (BEP 5) to bootstrap and query the
// mainline DHT. This task implements only the transport plumbing and the
// `ping` query/response round trip; `lookup` (get_peers/find_node walk) and
// bootstrapping are later tasks (8/9) and are declared here as stubs so the
// full public shape is stable from the start.
class DhtNode : public QObject {
    Q_OBJECT
public:
    DhtNode(const NodeId& self, quint16 port, quint32 rngSeed, QObject* parent = nullptr);
    ~DhtNode() override;

    // Binds the UDP socket on `port` (0 = ephemeral). Returns false on bind failure.
    bool start();
    quint16 boundPort() const;
    int nodeCount() const; // routing table size (diagnostics)

    // Sends a ping query to host:port with a fresh transaction id.
    void ping(const QString& host, quint16 port);

    // Iterative get_peers/find_node walk toward infoHash. Task 8.
    void lookup(const QByteArray& infoHash);

    // Bootstraps into the DHT: for each "host:port" entry, pings it directly
    // if `host` is a numeric IP, or resolves it via async QHostInfo and pings
    // every resolved address otherwise (no dangling lookup if this DhtNode is
    // destroyed first -- QHostInfo::lookupHost's context-object overload
    // drops the callback automatically). The first response that makes the
    // routing table non-empty fires a find_node for our own id at that same
    // host (to pull in more contacts) and emits bootstrapped() -- exactly
    // once for this DhtNode's lifetime, however many routers/responses come
    // in afterward.
    void bootstrap(const QStringList& routers);

    // Well-known mainline DHT bootstrap routers (BEP 5).
    static const QStringList kDefaultRouters;

    const NodeId& id() const { return m_self; }

    // Test-only shim: forwards to the private find_node query so tests can
    // drive it directly without a full lookup() walk (that's Task 8).
    void findNodeForTest(const QString& host, quint16 port, const NodeId& target) {
        findNode(host, port, target);
    }

    // Test-only shim: forwards to the private get_peers query (single hop);
    // the full iterative lookup() walk is Task 8.
    void getPeersForTest(const QString& host, quint16 port, const QByteArray& infoHash) {
        getPeers(host, port, infoHash);
    }

    // Test-only shim: seeds the peer store directly (bypassing the wire),
    // as if a real peer had already announced itself for infoHash.
    void storePeerForTest(const QByteArray& infoHash, const QString& host, quint16 port);

    // Test-only shim: exercises the same token-validate + store logic that
    // handleQuery uses for an incoming announce_peer, without needing to
    // first drive a real get_peers round trip to mint a valid token.
    bool acceptAnnounceForTest(const QByteArray& infoHash, const QString& host, quint16 port,
                                const QByteArray& token);

    // Test-only shim: forwards to RoutingTable::sawNode so tests can seed the
    // routing table directly without a real wire round trip.
    void seedNodeForTest(const DhtNodeEntry& entry) { m_table.sawNode(entry); }

    // Persists our node id and up to a bounded number of good routing-table
    // nodes to `path` (QDataStream blob: magic + version + id + node list).
    // Best-effort: failure to write is not reported (mirrors saveState's
    // "diagnostics only" role -- callers don't treat persistence as a hard
    // dependency).
    void saveState(const QString& path) const;

    // Restores node id + routing-table nodes previously written by
    // saveState(). On success, OVERWRITES the in-memory node id (m_self)
    // with the stored one and rebuilds the routing table (rekeyed to the new
    // self id) seeded with the stored nodes. Returns false -- leaving all
    // current state untouched -- if the file is missing, unreadable, or its
    // magic/version don't match.
    bool loadState(const QString& path);

signals:
    void peersFound(QByteArray infoHash, QVector<PeerAddress> peers); // Task 8
    void bootstrapped();                                              // Task 9

private:
    struct Pending; // defined in the .cpp: one heap instance per in-flight query
    struct Lookup;  // defined in the .cpp: one heap instance per in-flight lookup() walk

    void onReadyRead();
    void handleQuery(const KrpcMessage& msg, const QString& host, quint16 port);
    void handleResponse(const KrpcMessage& msg, const QString& host, quint16 port);
    QByteArray nextTid();

    // Sends a find_node query to host:port asking for nodes close to target.
    void findNode(const QString& host, quint16 port, const NodeId& target);

    // Sends a get_peers query to host:port for infoHash. On response: if
    // "values" is present, parses compact peers and emits peersFound;
    // otherwise parses "nodes" (closer to infoHash) and feeds them into the
    // routing table for the iterative walk (Task 8).
    void getPeers(const QString& host, quint16 port, const QByteArray& infoHash);

    // Shared boilerplate factored out of ping/findNode/getPeers: registers a
    // Pending entry keyed by tid, arms its timeout timer, and sends the
    // datagram. `onTimeout` runs only if the query is still pending when the
    // timer fires (a response already erases the entry first). `expectedId`
    // is non-null only for a health-check re-ping of a node we already know
    // (see pingNode): it's stashed on the Pending so the timeout can
    // RoutingTable::markBad() that specific id -- a first-contact
    // ping/findNode/getPeers (id unknown) never sets it, so those timeouts
    // stay a no-op as before.
    void sendQuery(const QByteArray& tid, const QByteArray& datagram, const QString& host, quint16 port,
                   const QByteArray& infoHash = QByteArray(), const NodeId* expectedId = nullptr);

    // Health-check re-ping for a node already in the routing table: carries
    // its known NodeId in the Pending entry (see sendQuery) so a timeout
    // marks it bad. This is the timeout->markBad path deferred from Task 5's
    // first-contact ping, which never has an id to mark bad.
    void pingNode(const DhtNodeEntry& entry);

    // Maintenance timer tick (~5 min, or short under
    // ORBIT_UDP_FAST_TIMEOUT): health-check-pings every currently known node
    // and refreshes each occupied routing-table bucket with a find_node for
    // a random id inside it, asking our closest known contacts.
    void maintenanceTick();

    // Deterministic (seed-derived, not QRandomGenerator) id that falls in
    // bucket `bucket` relative to m_self: shares m_self's first `bucket`
    // bits, differs at bit `bucket`, random beyond it -- matches
    // NodeId::bucketIndex's bit convention exactly.
    NodeId randomIdInBucket(int bucket);

    // Deterministic 32-bit PRNG (splitmix32-style), seeded from the
    // constructor's rngSeed, used only by randomIdInBucket -- keeps
    // maintenance behaviour reproducible for fixed-seed tests.
    quint32 nextMaintRandom();

    // Compact node info packing (BEP 5): 26 bytes/entry = 20-byte id +
    // 4-byte big-endian IPv4 + 2-byte big-endian port. Entries with a
    // non-IPv4 host are skipped by packNodes; unpackNodes stops as soon as
    // fewer than 26 bytes remain (tolerates truncated/garbage input).
    static QByteArray packNodes(const QVector<DhtNodeEntry>& nodes);
    static QVector<DhtNodeEntry> unpackNodes(const QByteArray& compact);

    // Compact peer packing (BEP 5): 6 bytes = 4-byte big-endian IPv4 + 2-byte
    // big-endian port. Unlike compact node info, a get_peers response's
    // "values" is a bencode LIST with one such 6-byte string per element (not
    // one concatenated string), so packing/unpacking works one peer at a
    // time: packPeer returns an empty array for a non-IPv4 host (caller
    // skips it when building the list); unpackPeer rejects any input whose
    // size isn't exactly 6 bytes (tolerates malformed/foreign elements).
    static QByteArray packPeer(const PeerAddress& peer);
    static bool unpackPeer(const QByteArray& compact, PeerAddress* out);

    // Task 8 iterative get_peers walk helpers. `lookupMergeCandidate` inserts
    // `entry` into `lk`'s shortlist (kept sorted by ascending XOR distance to
    // lk->target), de-duplicated by NodeId and excluding self.
    // `lookupAdvance` fires get_peers at up to alpha closest un-queried
    // shortlist candidates, then tears the Lookup down (erasing it from
    // m_lookups and freeing it) once it has converged/capped/drained; safe to
    // call repeatedly as responses/timeouts trickle in.
    void lookupMergeCandidate(Lookup* lk, const DhtNodeEntry& entry);
    void lookupAdvance(const QByteArray& infoHash);

    // Token issuance/validation (BEP 5 announce_peer anti-spoofing): the
    // token handed out in a get_peers reply must be echoed back in the
    // subsequent announce_peer. It is derived from a rotating secret so we
    // never need to remember tokens we've issued; validToken accepts either
    // the current or the just-previous secret so tokens minted just before a
    // rotation remain valid for one more rotation period.
    QByteArray makeToken(const QHostAddress& ip) const;
    bool validToken(const QByteArray& token, const QHostAddress& ip) const;
    void rotateTokenSecret();

    // Peer store bookkeeping shared by the real announce_peer handler and
    // the storePeerForTest/acceptAnnounceForTest shims. Sweeps expired
    // entries for infoHash before inserting/reading.
    void storePeer(const QByteArray& infoHash, const QString& host, quint16 port);
    QVector<PeerAddress> peersFor(const QByteArray& infoHash);

    NodeId m_self;
    quint16 m_port;
    quint32 m_txCounter = 0;
    quint32 m_key = 0;
    QUdpSocket* m_socket = nullptr;
    RoutingTable m_table;
    QHash<QByteArray, Pending*> m_pending;
    QHash<QByteArray, Lookup*> m_lookups; // keyed by infoHash; one active lookup() per infoHash at a time

    QByteArray m_tokenSecret;
    QByteArray m_prevTokenSecret;
    QTimer* m_tokenRotateTimer = nullptr;

    QHash<QByteArray, QVector<StoredPeer>> m_peerStore;

    // bootstrapped() must fire only as a result of bootstrap() (never from a
    // plain ping/find_node/get_peers that happens to fill the table first):
    // m_bootstrapRequested is set true by bootstrap() itself; m_bootstrapDone
    // guards the emit to happen at most once, ever, once both are satisfied.
    bool m_bootstrapRequested = false;
    bool m_bootstrapDone = false;
    quint32 m_maintRngState = 0;       // seeded PRNG state for randomIdInBucket
    QTimer* m_maintenanceTimer = nullptr;
};
