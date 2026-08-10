#include "torrent/DhtNode.h"

#include "Persistence.h"
#include "torrent/KrpcCodec.h"

#include <QCryptographicHash>
#include <QDataStream>
#include <QDateTime>
#include <QFile>
#include <QHostAddress>
#include <QHostInfo>
#include <QNetworkDatagram>
#include <QSet>
#include <QTimer>
#include <QUdpSocket>
#include <QtGlobal>

namespace {
const QByteArray kIdKey = "id";
const QByteArray kTargetKey = "target";
const QByteArray kNodesKey = "nodes";
const QByteArray kInfoHashKey = "info_hash";
const QByteArray kTokenKey = "token";
const QByteArray kValuesKey = "values";
const QByteArray kPortKey = "port";
constexpr quint32 kTidSalt = 0x9E3779B9u; // mirrors UdpTrackerClient's rngSeed mixing
constexpr int kCompactNodeSize = 26;      // 20-byte id + 4-byte IPv4 + 2-byte port
constexpr int kCompactPeerSize = 6;       // 4-byte IPv4 + 2-byte port
constexpr int kTokenRotateMs = 10 * 60 * 1000; // 10 minutes (BEP 5 recommends short-lived tokens)
constexpr qint64 kPeerExpiryMs = 30 * 60 * 1000; // 30 minutes (BEP 5 announce interval is ~30 min)
constexpr int kLookupAlpha = 3;   // concurrent in-flight get_peers queries per lookup
constexpr int kLookupK = 8;       // convergence width: stop once the k closest have all been tried
constexpr int kLookupCap = 100;   // hard cap on total nodes queried, guards against runaway meshes
constexpr int kMaintenanceIntervalMs = 5 * 60 * 1000;  // ~5 min, per BEP 5 bucket-refresh guidance
constexpr int kMaintenanceIntervalFastMs = 2000;       // short but not a storm: still bounded per test run
constexpr int kMaintenanceAlpha = 3; // contacts asked per bucket refresh find_node

// dht.dat on-disk format: magic + version guard the blob so a foreign/corrupt
// file (or a future incompatible layout) is rejected outright by loadState
// rather than partially/incorrectly parsed. Version is pinned to a fixed
// QDataStream wire format (Qt_5_15) independent of the Qt version this is
// built against, so the file stays readable across Qt upgrades.
constexpr quint32 kDhtStateMagic = 0x4F524244u; // 'O','R','B','D'
constexpr quint32 kDhtStateVersion = 1;
constexpr int kMaxPersistedNodes = 200; // bounded so dht.dat can't grow unbounded
constexpr QDataStream::Version kDhtStateStreamVersion = QDataStream::Qt_5_15;
}

const QStringList DhtNode::kDefaultRouters = {
    QStringLiteral("router.bittorrent.com:6881"),
    QStringLiteral("router.utorrent.com:6881"),
    QStringLiteral("dht.transmissionbt.com:6881"),
    QStringLiteral("dht.libtorrent.org:25401"),
};

// One in-flight query = one heap Pending, keyed by transaction id in
// m_pending. The QTimer is a child of the owning DhtNode (so it is reaped if
// the node itself is destroyed mid-flight); the wrapper struct is not a
// QObject and is freed explicitly wherever it is removed from m_pending
// (on-response in handleResponse, on-timeout in the lambda below, and in
// ~DhtNode for anything still outstanding at shutdown).
struct DhtNode::Pending {
    QTimer* timer = nullptr;
    QString host;
    quint16 port = 0;
    // Non-empty only for an in-flight get_peers query: lets handleResponse
    // know which info_hash a "values"/"nodes" reply belongs to (BEP 5's
    // get_peers response does not itself echo back the info_hash).
    QByteArray infoHash;
    // Set only by pingNode()'s health-check re-ping of a node we already
    // have in the routing table: gives the timeout handler a known id to
    // RoutingTable::markBad(). A first-contact ping/findNode/getPeers has no
    // confirmed id yet and leaves this false, so their timeouts stay a no-op.
    bool hasTargetId = false;
    NodeId targetId;
};

// One heap instance per in-flight lookup() walk, keyed by infoHash in
// m_lookups. `shortlist` is kept sorted by ascending XOR distance to
// `target` (== infoHash reinterpreted as a NodeId); `inFlight` counts
// currently-outstanding get_peers queries belonging to this lookup so
// lookupAdvance knows how many more it may fire (<= kLookupAlpha) and when
// the walk has fully drained (inFlight == 0, nothing left to query).
struct DhtNode::Lookup {
    struct Candidate {
        DhtNodeEntry entry;
        bool queried = false;
        bool responded = false;
    };
    NodeId target;
    QVector<Candidate> shortlist;
    int inFlight = 0;
    int totalQueried = 0;
};

DhtNode::DhtNode(const NodeId& self, quint16 port, quint32 rngSeed, QObject* parent)
    : QObject(parent), m_self(self), m_port(port), m_key(rngSeed ^ kTidSalt), m_table(self) {
    // Deterministic initial secret derived from rngSeed+self, so tests that
    // construct a DhtNode with a fixed seed get reproducible tokens without
    // any wall-clock or RNG dependency; it's rotated (and only ever compared
    // by hash) so its derivation need not be cryptographically strong.
    m_tokenSecret = QByteArray::number(rngSeed) + self.bytes();
    m_prevTokenSecret = m_tokenSecret;

    m_tokenRotateTimer = new QTimer(this);
    connect(m_tokenRotateTimer, &QTimer::timeout, this, &DhtNode::rotateTokenSecret);
    m_tokenRotateTimer->start(kTokenRotateMs);

    // Distinct salt from m_key (tid mixing) so the two seeded streams don't
    // just mirror each other; still fully deterministic for a fixed rngSeed.
    m_maintRngState = rngSeed ^ 0x2545F491u;
    m_maintenanceTimer = new QTimer(this);
    connect(m_maintenanceTimer, &QTimer::timeout, this, &DhtNode::maintenanceTick);
    const bool fast = qEnvironmentVariableIsSet("ORBIT_UDP_FAST_TIMEOUT");
    m_maintenanceTimer->start(fast ? kMaintenanceIntervalFastMs : kMaintenanceIntervalMs);
}

DhtNode::~DhtNode() {
    // Timers are QObject children of `this` and would be torn down anyway by
    // ~QObject, but the Pending wrappers themselves are plain heap structs we
    // own directly and must free here to avoid leaking them.
    for (auto it = m_pending.begin(); it != m_pending.end(); ++it) delete it.value();
    m_pending.clear();
    // Same story for any lookup() walks still in progress: heap Lookup*
    // instances we own directly, freed here so a seeker destroyed mid-lookup
    // doesn't leak its shortlist.
    for (auto it = m_lookups.begin(); it != m_lookups.end(); ++it) delete it.value();
    m_lookups.clear();
}

bool DhtNode::start() {
    if (m_socket) return true; // idempotent: already bound
    m_socket = new QUdpSocket(this);
    connect(m_socket, &QUdpSocket::readyRead, this, &DhtNode::onReadyRead);
    if (!m_socket->bind(QHostAddress::AnyIPv4, m_port)) {
        m_socket->deleteLater();
        m_socket = nullptr;
        return false;
    }
    return true;
}

quint16 DhtNode::boundPort() const {
    return m_socket ? m_socket->localPort() : 0;
}

int DhtNode::nodeCount() const {
    return m_table.nodeCount();
}

QByteArray DhtNode::nextTid() {
    // Deterministic 2-byte transaction id: incrementing counter mixed with a
    // seed-derived key, same construction UdpTrackerClient uses for its 32-bit
    // txId (see UdpTrackerClient.cpp). No QRandomGenerator on this path.
    const quint16 v = quint16((++m_txCounter) ^ m_key);
    QByteArray tid(2, char(0));
    tid[0] = char((v >> 8) & 0xFF);
    tid[1] = char(v & 0xFF);
    return tid;
}

void DhtNode::sendQuery(const QByteArray& tid, const QByteArray& datagram, const QString& host, quint16 port,
                         const QByteArray& infoHash, const NodeId* expectedId) {
    auto* p = new Pending;
    p->host = host;
    p->port = port;
    p->infoHash = infoHash;
    if (expectedId) {
        p->hasTargetId = true;
        p->targetId = *expectedId;
    }
    p->timer = new QTimer(this);
    p->timer->setSingleShot(true);
    m_pending.insert(tid, p);

    const bool fast = qEnvironmentVariableIsSet("ORBIT_UDP_FAST_TIMEOUT");
    const int timeoutMs = fast ? 200 : 15000;

    // Captures `this` and `tid` by value only (never the Pending* itself), so
    // a Pending freed elsewhere (e.g. a response arriving first) never leaves
    // a dangling pointer for this lambda to touch: it re-looks-up by tid and
    // no-ops if the entry is already gone.
    connect(p->timer, &QTimer::timeout, this, [this, tid]() {
        auto it = m_pending.find(tid);
        if (it == m_pending.end()) return;
        Pending* pending = it.value();
        m_pending.erase(it);
        pending->timer->deleteLater();
        const QByteArray timedOutInfoHash = pending->infoHash;
        // Only a health-check re-ping (pingNode) has a confirmed id to mark
        // bad; a first-contact ping/findNode/getPeers never sets this, so
        // its timeout stays a no-op as before (nothing in the routing table
        // yet to mark bad).
        const bool hadTargetId = pending->hasTargetId;
        const NodeId timedOutTargetId = pending->targetId;
        delete pending;

        if (hadTargetId) m_table.markBad(timedOutTargetId);

        // A dead/unreachable node must not stall an in-progress lookup()
        // walk forever: give up on this candidate (it stays queried=true,
        // responded=false) and let the walk try the next one.
        if (!timedOutInfoHash.isEmpty()) {
            auto lit = m_lookups.find(timedOutInfoHash);
            if (lit != m_lookups.end()) {
                lit.value()->inFlight--;
                lookupAdvance(timedOutInfoHash);
            }
        }
    });
    p->timer->start(timeoutMs);

    m_socket->writeDatagram(datagram, QHostAddress(host), port);
}

void DhtNode::ping(const QString& host, quint16 port) {
    if (!m_socket) return;

    const QByteArray tid = nextTid();
    QMap<QByteArray, BencodeValue> args;
    args[kIdKey] = BencodeValue::makeBytes(m_self.bytes());
    const QByteArray packet = KrpcCodec::encodeQuery(tid, QStringLiteral("ping"), args);

    sendQuery(tid, packet, host, port);
}

void DhtNode::pingNode(const DhtNodeEntry& entry) {
    if (!m_socket) return;

    const QByteArray tid = nextTid();
    QMap<QByteArray, BencodeValue> args;
    args[kIdKey] = BencodeValue::makeBytes(m_self.bytes());
    const QByteArray packet = KrpcCodec::encodeQuery(tid, QStringLiteral("ping"), args);

    sendQuery(tid, packet, entry.host, entry.port, QByteArray(), &entry.id);
}

void DhtNode::findNode(const QString& host, quint16 port, const NodeId& target) {
    if (!m_socket) return;

    const QByteArray tid = nextTid();
    QMap<QByteArray, BencodeValue> args;
    args[kIdKey] = BencodeValue::makeBytes(m_self.bytes());
    args[kTargetKey] = BencodeValue::makeBytes(target.bytes());
    const QByteArray packet = KrpcCodec::encodeQuery(tid, QStringLiteral("find_node"), args);

    sendQuery(tid, packet, host, port);
}

void DhtNode::getPeers(const QString& host, quint16 port, const QByteArray& infoHash) {
    if (!m_socket) return;

    const QByteArray tid = nextTid();
    QMap<QByteArray, BencodeValue> args;
    args[kIdKey] = BencodeValue::makeBytes(m_self.bytes());
    args[kInfoHashKey] = BencodeValue::makeBytes(infoHash);
    const QByteArray packet = KrpcCodec::encodeQuery(tid, QStringLiteral("get_peers"), args);

    sendQuery(tid, packet, host, port, infoHash);
}

QByteArray DhtNode::packNodes(const QVector<DhtNodeEntry>& nodes) {
    QByteArray out;
    out.reserve(nodes.size() * kCompactNodeSize);
    for (const DhtNodeEntry& n : nodes) {
        const QHostAddress addr(n.host);
        if (addr.protocol() != QAbstractSocket::IPv4Protocol) continue; // IPv6 not in scope here
        out += n.id.bytes();
        const quint32 ip = addr.toIPv4Address();
        out += char((ip >> 24) & 0xFF);
        out += char((ip >> 16) & 0xFF);
        out += char((ip >> 8) & 0xFF);
        out += char(ip & 0xFF);
        out += char((n.port >> 8) & 0xFF);
        out += char(n.port & 0xFF);
    }
    return out;
}

QVector<DhtNodeEntry> DhtNode::unpackNodes(const QByteArray& compact) {
    QVector<DhtNodeEntry> out;
    // Bounds-safe: only ever reads whole kCompactNodeSize chunks, so a
    // truncated/garbage trailing remainder is silently ignored rather than
    // causing an out-of-bounds read.
    int offset = 0;
    while (offset + kCompactNodeSize <= compact.size()) {
        const QByteArray idBytes = compact.mid(offset, 20);
        const auto* p = reinterpret_cast<const uchar*>(compact.constData()) + offset + 20;
        const quint32 ip = (quint32(p[0]) << 24) | (quint32(p[1]) << 16) | (quint32(p[2]) << 8) | quint32(p[3]);
        const quint16 nodePort = quint16((quint16(p[4]) << 8) | quint16(p[5]));
        out.append(DhtNodeEntry{NodeId(idBytes), QHostAddress(ip).toString(), nodePort});
        offset += kCompactNodeSize;
    }
    return out;
}

QByteArray DhtNode::packPeer(const PeerAddress& peer) {
    const QHostAddress addr(peer.host);
    if (addr.protocol() != QAbstractSocket::IPv4Protocol) return {}; // IPv6 not in scope here
    const quint32 ip = addr.toIPv4Address();
    QByteArray out;
    out.reserve(kCompactPeerSize);
    out += char((ip >> 24) & 0xFF);
    out += char((ip >> 16) & 0xFF);
    out += char((ip >> 8) & 0xFF);
    out += char(ip & 0xFF);
    out += char((peer.port >> 8) & 0xFF);
    out += char(peer.port & 0xFF);
    return out;
}

bool DhtNode::unpackPeer(const QByteArray& compact, PeerAddress* out) {
    // Bounds-safe: only ever decodes an exact kCompactPeerSize element; a
    // short/malformed element is rejected rather than read out of bounds.
    if (compact.size() != kCompactPeerSize) return false;
    const auto* p = reinterpret_cast<const uchar*>(compact.constData());
    const quint32 ip = (quint32(p[0]) << 24) | (quint32(p[1]) << 16) | (quint32(p[2]) << 8) | quint32(p[3]);
    const quint16 peerPort = quint16((quint16(p[4]) << 8) | quint16(p[5]));
    *out = PeerAddress{QHostAddress(ip).toString(), peerPort};
    return true;
}

QByteArray DhtNode::makeToken(const QHostAddress& ip) const {
    QCryptographicHash hash(QCryptographicHash::Sha1);
    hash.addData(m_tokenSecret);
    hash.addData(ip.toString().toUtf8());
    return hash.result();
}

bool DhtNode::validToken(const QByteArray& token, const QHostAddress& ip) const {
    if (token.isEmpty()) return false;
    QCryptographicHash cur(QCryptographicHash::Sha1);
    cur.addData(m_tokenSecret);
    cur.addData(ip.toString().toUtf8());
    if (token == cur.result()) return true;

    QCryptographicHash prev(QCryptographicHash::Sha1);
    prev.addData(m_prevTokenSecret);
    prev.addData(ip.toString().toUtf8());
    return token == prev.result();
}

void DhtNode::rotateTokenSecret() {
    m_prevTokenSecret = m_tokenSecret;
    // Rotated in place from the outgoing secret (not from any wall-clock or
    // RNG source) so behaviour under ORBIT_UDP_FAST_TIMEOUT-style test knobs
    // stays deterministic; only the timer's firing is time-based.
    m_tokenSecret = QCryptographicHash::hash(m_tokenSecret, QCryptographicHash::Sha1);
}

void DhtNode::storePeer(const QByteArray& infoHash, const QString& host, quint16 port) {
    QVector<StoredPeer>& bucket = m_peerStore[infoHash];
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    // Sweep expired entries for this info_hash before inserting.
    for (int i = bucket.size() - 1; i >= 0; --i) {
        if (bucket[i].expiryMs <= now) bucket.remove(i);
    }
    for (StoredPeer& sp : bucket) {
        if (sp.host == host && sp.port == port) {
            sp.expiryMs = now + kPeerExpiryMs; // re-announce refreshes expiry
            return;
        }
    }
    bucket.append(StoredPeer{host, port, now + kPeerExpiryMs});
}

QVector<PeerAddress> DhtNode::peersFor(const QByteArray& infoHash) {
    auto it = m_peerStore.find(infoHash);
    if (it == m_peerStore.end()) return {};

    QVector<StoredPeer>& bucket = it.value();
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (int i = bucket.size() - 1; i >= 0; --i) {
        if (bucket[i].expiryMs <= now) bucket.remove(i);
    }

    QVector<PeerAddress> out;
    out.reserve(bucket.size());
    for (const StoredPeer& sp : bucket) out.append(PeerAddress{sp.host, sp.port});
    return out;
}

void DhtNode::storePeerForTest(const QByteArray& infoHash, const QString& host, quint16 port) {
    storePeer(infoHash, host, port);
}

bool DhtNode::acceptAnnounceForTest(const QByteArray& infoHash, const QString& host, quint16 port,
                                     const QByteArray& token) {
    if (!validToken(token, QHostAddress(host))) return false;
    storePeer(infoHash, host, port);
    return true;
}

void DhtNode::saveState(const QString& path) const {
    QByteArray blob;
    QDataStream out(&blob, QIODevice::WriteOnly);
    out.setVersion(kDhtStateStreamVersion);
    out << kDhtStateMagic << kDhtStateVersion;
    out << m_self.bytes();

    const QVector<DhtNodeEntry> nodes = m_table.allNodes();
    const int count = qMin(nodes.size(), kMaxPersistedNodes);
    out << quint32(count);
    for (int i = 0; i < count; ++i) {
        out << nodes[i].id.bytes() << nodes[i].host << nodes[i].port;
    }

    // Best-effort: this is diagnostics/warm-start data, not a hard
    // dependency, so a write failure (e.g. unwritable directory) is not
    // reported to the caller.
    Persistence::writeFileAtomic(path, blob);
}

bool DhtNode::loadState(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return false;
    const QByteArray blob = f.readAll();

    QDataStream in(blob);
    in.setVersion(kDhtStateStreamVersion);

    quint32 magic = 0, version = 0;
    in >> magic >> version;
    if (in.status() != QDataStream::Ok) return false;
    if (magic != kDhtStateMagic || version != kDhtStateVersion) return false;

    QByteArray idBytes;
    in >> idBytes;
    if (in.status() != QDataStream::Ok || idBytes.size() != 20) return false;

    quint32 count = 0;
    in >> count;
    if (in.status() != QDataStream::Ok) return false;
    if (count > quint32(kMaxPersistedNodes)) return false;

    // Parse into a local vector first so a truncated/corrupt tail leaves the
    // live node id and routing table completely untouched (loadState returns
    // false without having mutated any member).
    QVector<DhtNodeEntry> loaded;
    loaded.reserve(int(count));
    for (quint32 i = 0; i < count; ++i) {
        QByteArray nid;
        QString host;
        quint16 port = 0;
        in >> nid >> host >> port;
        if (in.status() != QDataStream::Ok || nid.size() != 20) return false;
        loaded.append(DhtNodeEntry{NodeId(nid), host, port});
    }

    // Commit: overwrite self id, then rebuild the routing table from scratch
    // rekeyed to the new self id (bucketIndex is relative to self, so an
    // id-overwrite without this would leave stale/incorrectly-bucketed
    // entries), and seed it with the nodes just loaded.
    m_self = NodeId(idBytes);
    m_table = RoutingTable(m_self);
    for (const DhtNodeEntry& e : loaded) m_table.sawNode(e);
    return true;
}

void DhtNode::lookupMergeCandidate(Lookup* lk, const DhtNodeEntry& entry) {
    if (entry.id == m_self) return; // never query ourselves
    for (const Lookup::Candidate& c : lk->shortlist) {
        if (c.entry.id == entry.id) return; // dedup by NodeId
    }
    Lookup::Candidate cand;
    cand.entry = entry;
    // Insertion sort: keep the shortlist sorted by ascending XOR distance to
    // the lookup target so lookupAdvance can always take unqueried
    // candidates off the front in closest-first order.
    int pos = lk->shortlist.size();
    for (int i = 0; i < lk->shortlist.size(); ++i) {
        if (NodeId::closer(entry.id, lk->shortlist[i].entry.id, lk->target)) {
            pos = i;
            break;
        }
    }
    lk->shortlist.insert(pos, cand);
}

void DhtNode::lookupAdvance(const QByteArray& infoHash) {
    auto it = m_lookups.find(infoHash);
    if (it == m_lookups.end()) return; // already torn down (or never existed)
    Lookup* lk = it.value();

    // Fire get_peers at up to alpha closest un-queried candidates, in
    // closest-first order (shortlist is kept sorted).
    while (lk->inFlight < kLookupAlpha && lk->totalQueried < kLookupCap) {
        int idx = -1;
        for (int i = 0; i < lk->shortlist.size(); ++i) {
            if (!lk->shortlist[i].queried) { idx = i; break; }
        }
        if (idx < 0) break; // nothing left in the shortlist to query

        Lookup::Candidate& c = lk->shortlist[idx];
        c.queried = true;
        lk->inFlight++;
        lk->totalQueried++;
        getPeers(c.entry.host, c.entry.port, infoHash);
    }

    // Termination: the walk has converged once the k closest candidates
    // currently known have all been tried (queried, whether they answered or
    // timed out) -- any nodes beyond that are farther than our best-k and
    // querying them further couldn't improve the result. The hard cap
    // guards against a shortlist that keeps growing with genuinely-closer
    // nodes across a large mesh. Either way we only actually tear down the
    // Lookup once nothing is still in flight (inFlight == 0): otherwise a
    // response/timeout still outstanding wouldn't have anywhere to land.
    const int topN = qMin(lk->shortlist.size(), kLookupK);
    bool topAllQueried = true;
    for (int i = 0; i < topN; ++i) {
        if (!lk->shortlist[i].queried) { topAllQueried = false; break; }
    }
    const bool capReached = lk->totalQueried >= kLookupCap;

    if (lk->inFlight == 0 && (topAllQueried || capReached)) {
        m_lookups.erase(it);
        delete lk;
    }
}

void DhtNode::lookup(const QByteArray& infoHash) {
    if (!m_socket) return;
    if (infoHash.size() != 20) return; // NodeId requires exactly 20 bytes
    if (m_lookups.contains(infoHash)) return; // a walk for this infoHash is already in progress

    auto* lk = new Lookup;
    lk->target = NodeId(infoHash);
    for (const DhtNodeEntry& e : m_table.closest(lk->target, kLookupK)) {
        lookupMergeCandidate(lk, e);
    }
    m_lookups.insert(infoHash, lk);
    lookupAdvance(infoHash);
}

quint32 DhtNode::nextMaintRandom() {
    // splitmix32: deterministic given m_maintRngState's seeding in the
    // constructor, so randomIdInBucket() stays reproducible for a fixed
    // rngSeed -- no QRandomGenerator on this path.
    m_maintRngState += 0x9E3779B9u;
    quint32 z = m_maintRngState;
    z = (z ^ (z >> 16)) * 0x85EBCA6Bu;
    z = (z ^ (z >> 13)) * 0xC2B2AE35u;
    z = z ^ (z >> 16);
    return z;
}

NodeId DhtNode::randomIdInBucket(int bucket) {
    bucket = qBound(0, bucket, 159);
    QByteArray out = m_self.bytes(); // start identical to self: shares every bit up to `bucket`
    const int byteIdx = bucket / 8;
    // Matches NodeId::bucketIndex's convention exactly: bucket b's byte is
    // b/8, and within it the bit tested is (7 - b%8) counting from the MSB.
    const int bitOffset = 7 - (bucket % 8);
    out[byteIdx] = char(quint8(out[byteIdx]) ^ (1u << bitOffset)); // force the first differing bit at `bucket`

    // Randomize everything strictly after bit `bucket`: the remaining bits
    // of this byte (below bitOffset)...
    const quint8 lowMask = bitOffset == 0 ? 0 : quint8((1u << bitOffset) - 1);
    const quint32 r = nextMaintRandom();
    out[byteIdx] = char((quint8(out[byteIdx]) & ~lowMask) | (quint8(r) & lowMask));
    // ...and every following byte in full.
    for (int i = byteIdx + 1; i < 20; ++i) out[i] = char(nextMaintRandom() & 0xFF);

    return NodeId(out);
}

void DhtNode::maintenanceTick() {
    if (!m_socket) return;

    const QVector<DhtNodeEntry> nodes = m_table.allNodes();

    // Health-check: re-ping every currently known node. RoutingTable doesn't
    // expose a per-node last-seen timestamp, so treating every node as
    // "questionable" once per (multi-minute) maintenance interval is the
    // simplest correct approximation; a node that fails to answer this
    // known-id re-ping is what actually drives markBad (via pingNode's
    // Pending::targetId), unlike a first-contact ping's timeout.
    for (const DhtNodeEntry& n : nodes) pingNode(n);

    // Bucket refresh (BEP 5): for each currently-occupied bucket, ask our
    // closest known contacts for nodes near a random id inside that bucket,
    // to keep sparsely populated buckets fed with new candidates.
    QSet<int> buckets;
    for (const DhtNodeEntry& n : nodes) buckets.insert(m_self.bucketIndex(n.id));
    for (int b : buckets) {
        const NodeId target = randomIdInBucket(b);
        for (const DhtNodeEntry& c : m_table.closest(target, kMaintenanceAlpha)) {
            findNode(c.host, c.port, target);
        }
    }
}

void DhtNode::bootstrap(const QStringList& routers) {
    if (!m_socket) return;

    m_bootstrapRequested = true;

    for (const QString& router : routers) {
        const int colon = router.lastIndexOf(':');
        if (colon <= 0 || colon == router.length() - 1) continue; // malformed, not "host:port"
        const QString host = router.left(colon);
        bool portOk = false;
        const quint16 port = router.mid(colon + 1).toUShort(&portOk);
        if (!portOk) continue;

        QHostAddress numeric;
        if (numeric.setAddress(host)) {
            ping(host, port); // numeric IP: no DNS round trip needed
            continue;
        }

        // Async DNS resolution. Passing `this` as the context object means
        // Qt disconnects this callback automatically if this DhtNode is
        // destroyed before the lookup completes -- no dangling `this` use.
        QHostInfo::lookupHost(host, this, [this, port](const QHostInfo& info) {
            if (info.error() != QHostInfo::NoError) return;
            for (const QHostAddress& addr : info.addresses()) ping(addr.toString(), port);
        });
    }
}

void DhtNode::onReadyRead() {
    while (m_socket->hasPendingDatagrams()) {
        const QNetworkDatagram dg = m_socket->receiveDatagram();
        const KrpcMessage msg = KrpcCodec::decode(dg.data());
        if (msg.kind == KrpcMessage::Invalid) continue; // ignore junk on the shared socket

        const QString host = dg.senderAddress().toString();
        const quint16 port = dg.senderPort();

        if (msg.kind == KrpcMessage::Query) {
            handleQuery(msg, host, port);
        } else if (msg.kind == KrpcMessage::Response) {
            handleResponse(msg, host, port);
        }
        // Error messages carry no "id" (BEP 5) and there is no query state
        // machine beyond ping's pending map yet, so they are dropped here.
    }
}

void DhtNode::handleQuery(const KrpcMessage& msg, const QString& host, quint16 port) {
    static const QString kPing = QStringLiteral("ping");
    static const QString kFindNode = QStringLiteral("find_node");
    static const QString kGetPeers = QStringLiteral("get_peers");
    static const QString kAnnouncePeer = QStringLiteral("announce_peer");

    if (msg.method != kPing && msg.method != kFindNode && msg.method != kGetPeers &&
        msg.method != kAnnouncePeer) {
        return;
    }

    const BencodeValue idVal = msg.args.value(kIdKey);
    if (idVal.type() != BencodeValue::Type::Bytes || idVal.toBytes().size() != 20) return;

    m_table.sawNode(DhtNodeEntry{NodeId(idVal.toBytes()), host, port});

    if (msg.method == kAnnouncePeer) {
        const BencodeValue tokenVal = msg.args.value(kTokenKey);
        const BencodeValue portVal = msg.args.value(kPortKey);
        if (tokenVal.type() != BencodeValue::Type::Bytes) return;
        if (portVal.type() != BencodeValue::Type::Int) return;
        if (!validToken(tokenVal.toBytes(), QHostAddress(host))) return; // bad/stale token: silently drop

        const BencodeValue ihVal = msg.args.value(kInfoHashKey);
        if (ihVal.type() != BencodeValue::Type::Bytes || ihVal.toBytes().size() != 20) return;
        storePeer(ihVal.toBytes(), host, quint16(portVal.toInt()));

        QMap<QByteArray, BencodeValue> r;
        r[kIdKey] = BencodeValue::makeBytes(m_self.bytes());
        m_socket->writeDatagram(KrpcCodec::encodeResponse(msg.tid, r), QHostAddress(host), port);
        return;
    }

    QMap<QByteArray, BencodeValue> r;
    r[kIdKey] = BencodeValue::makeBytes(m_self.bytes());

    if (msg.method == kFindNode) {
        const BencodeValue targetVal = msg.args.value(kTargetKey);
        if (targetVal.type() != BencodeValue::Type::Bytes || targetVal.toBytes().size() != 20) return;
        const NodeId target(targetVal.toBytes());
        r[kNodesKey] = BencodeValue::makeBytes(packNodes(m_table.closest(target, 8)));
    } else if (msg.method == kGetPeers) {
        const BencodeValue ihVal = msg.args.value(kInfoHashKey);
        if (ihVal.type() != BencodeValue::Type::Bytes || ihVal.toBytes().size() != 20) return;
        const QByteArray infoHash = ihVal.toBytes();

        r[kTokenKey] = BencodeValue::makeBytes(makeToken(QHostAddress(host)));
        const QVector<PeerAddress> peers = peersFor(infoHash);
        if (!peers.isEmpty()) {
            // BEP 5: "values" is a bencode LIST of 6-byte compact peer
            // strings (unlike "nodes", which stays one concatenated string).
            QList<BencodeValue> values;
            values.reserve(peers.size());
            for (const PeerAddress& p : peers) {
                const QByteArray packed = packPeer(p);
                if (packed.size() == kCompactPeerSize) values.append(BencodeValue::makeBytes(packed));
            }
            r[kValuesKey] = BencodeValue::makeList(values);
        } else {
            const NodeId target(infoHash); // treat info_hash as a 160-bit id for XOR-distance purposes
            r[kNodesKey] = BencodeValue::makeBytes(packNodes(m_table.closest(target, 8)));
        }
    }

    m_socket->writeDatagram(KrpcCodec::encodeResponse(msg.tid, r), QHostAddress(host), port);
}

void DhtNode::handleResponse(const KrpcMessage& msg, const QString& host, quint16 port) {
    auto it = m_pending.find(msg.tid);
    if (it == m_pending.end()) return; // stray/late reply we no longer care about

    Pending* pending = it.value();
    m_pending.erase(it);
    pending->timer->stop();
    pending->timer->deleteLater();
    const QByteArray infoHash = pending->infoHash; // capture before freeing
    delete pending;

    const BencodeValue idVal = msg.args.value(kIdKey);
    if (idVal.type() == BencodeValue::Type::Bytes && idVal.toBytes().size() == 20) {
        m_table.sawNode(DhtNodeEntry{NodeId(idVal.toBytes()), host, port});
    }

    // Bootstrap completion: the first response, after bootstrap() was called,
    // that leaves the routing table non-empty means we've successfully
    // reached the DHT. Ask that same node (the one that just answered) for
    // nodes near our own id to pull in more contacts, and emit bootstrapped()
    // -- guarded so it only fires as a result of bootstrap() (never from a
    // plain ping/find_node/get_peers reply that happens to populate the table
    // first) and fires exactly once for this DhtNode's lifetime regardless of
    // how many more responses/bootstrap() calls follow.
    if (m_bootstrapRequested && !m_bootstrapDone && m_table.nodeCount() > 0) {
        m_bootstrapDone = true;
        findNode(host, port, m_self);
        emit bootstrapped();
    }

    // If this response belongs to an in-progress lookup() walk (infoHash is
    // only ever non-empty for a get_peers query, and only walks we started
    // register themselves in m_lookups), fold it into that walk's state:
    // one fewer in-flight query, and the responder counts as tried.
    Lookup* lk = nullptr;
    if (!infoHash.isEmpty()) {
        auto lit = m_lookups.find(infoHash);
        if (lit != m_lookups.end()) lk = lit.value();
    }
    if (lk) {
        lk->inFlight--;
        for (Lookup::Candidate& c : lk->shortlist) {
            if (c.entry.host == host && c.entry.port == port) {
                c.responded = true;
                break;
            }
        }
    }

    // get_peers response: per BEP 5 a real reply carries "values" XOR
    // "nodes", never both, but we don't rely on that here. "values" is a
    // bencode LIST of 6-byte compact peer strings (unlike "nodes", which is
    // one concatenated string) — parse it only when it actually arrives as a
    // list, and skip any element that isn't exactly a 6-byte string so a
    // malformed/foreign reply can't crash us.
    if (!infoHash.isEmpty()) {
        const BencodeValue valuesVal = msg.args.value(kValuesKey);
        if (valuesVal.type() == BencodeValue::Type::List) {
            QVector<PeerAddress> peers;
            for (const BencodeValue& elem : valuesVal.toList()) {
                if (elem.type() != BencodeValue::Type::Bytes) continue;
                PeerAddress peer;
                if (unpackPeer(elem.toBytes(), &peer)) peers.append(peer);
            }
            emit peersFound(infoHash, peers);
        }
    }

    const BencodeValue nodesVal = msg.args.value(kNodesKey);
    if (nodesVal.type() == BencodeValue::Type::Bytes) {
        const QVector<DhtNodeEntry> nodes = unpackNodes(nodesVal.toBytes());
        for (const DhtNodeEntry& n : nodes) {
            m_table.sawNode(n);
            if (lk) lookupMergeCandidate(lk, n);
        }
    }

    // Must run last: lookupAdvance may erase and delete lk (via m_lookups),
    // so nothing below this point may dereference lk again.
    if (!infoHash.isEmpty() && m_lookups.contains(infoHash)) lookupAdvance(infoHash);
}
