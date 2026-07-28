#include "torrent/MetadataFetch.h"
#include "torrent/AnnounceController.h"
#include "torrent/DhtNode.h"
#include "torrent/PeerConnection.h"

#include <QMetaType>
#include <QTimer>
#include <QUrl>

#include <random>

namespace {
QString peerKey(const PeerAddress& p) { return p.host + QLatin1Char(':') + QString::number(p.port); }

// Deterministic (rngSeed-derived) peer id, same scheme TorrentTask uses:
// reproducible across runs for a fixed seed, never QRandomGenerator.
QByteArray derivePeerId(quint32 rngSeed) {
    std::mt19937 rng(rngSeed);
    QByteArray id = QByteArray("-OB0001-");
    for (int i = 0; i < 12; ++i) id.append(char(rng() & 0xFF));
    return id;
}
} // namespace

MetadataFetch::MetadataFetch(const MagnetInfo& mi, DhtNode* dht, QNetworkAccessManager* nam,
                             quint32 rngSeed, RateLimiter* limiter, QObject* parent)
    : QObject(parent), m_mi(mi), m_dht(dht), m_nam(nam), m_rngSeed(rngSeed), m_limiter(limiter),
      m_peerId(derivePeerId(rngSeed)) {
    qRegisterMetaType<TorrentMetainfo>();
}

void MetadataFetch::start() {
    if (m_started) return;
    m_started = true;

    if (m_dht) {
        m_dhtPeersConn = connect(m_dht, &DhtNode::peersFound, this,
                [this](const QByteArray& infoHash, const QVector<PeerAddress>& peers) {
                    if (infoHash != m_mi.infoHash) return;
                    onPeersDiscovered(peers);
                });
        m_dht->lookup(m_mi.infoHash);

        // See kDhtRetryMs's doc comment (MetadataFetch.h): the lookup() call
        // just above will almost always find an empty routing table (no
        // event-loop turn has happened yet for any bootstrap ping/find_node
        // reply to land), so keep re-walking periodically until something
        // pans out or the overall timeout gives up.
        m_dhtRetryTimer = new QTimer(this);
        connect(m_dhtRetryTimer, &QTimer::timeout, this, [this] {
            if (!m_done && m_dht) m_dht->lookup(m_mi.infoHash);
        });
        m_dhtRetryTimer->start(kDhtRetryMs);
    }

    if (!m_mi.trackers.isEmpty()) {
        QVector<QUrl> tier;
        for (const QString& t : m_mi.trackers) {
            const QUrl u(t);
            if (u.isValid() && !u.scheme().isEmpty()) tier.append(u);
        }
        if (!tier.isEmpty()) {
            QVector<QVector<QUrl>> tiers{tier};
            m_announce = new AnnounceController(tiers, m_nam, m_rngSeed, this);
            connect(m_announce, &AnnounceController::peersReceived, this,
                    [this](QVector<PeerAddress> peers, int /*interval*/, int /*minInterval*/) {
                        onPeersDiscovered(peers);
                    });
            // No listen port / download progress to report yet -- this is a
            // one-shot "give me peers" announce, not a real download cadence
            // (TorrentTask owns that once the real metainfo exists).
            m_announce->announce(m_mi.infoHash, m_peerId, /*port*/ 0, /*downloaded*/ 0, /*left*/ 1,
                                 TrackerEvent::Started, /*hungry*/ true);
        }
    }

    m_timeoutTimer = new QTimer(this);
    m_timeoutTimer->setSingleShot(true);
    connect(m_timeoutTimer, &QTimer::timeout, this,
            [this] { finishFailed(QStringLiteral("timed out waiting for metadata from any peer")); });
    m_timeoutTimer->start(kOverallTimeoutMs);

    openMore(); // consumes any peer already known (test-injected, or a same-tick discovery above)
}

void MetadataFetch::setDht(DhtNode* dht) {
    if (m_dht == dht) return;
    if (m_dhtPeersConn) {
        QObject::disconnect(m_dhtPeersConn);
        m_dhtPeersConn = QMetaObject::Connection();
    }
    m_dht = dht;
    if (!m_dht) {
        // DHT disabled/torn down out from under us: nothing left to retry
        // against. finishFailed()'s eventual "exhausted attempts"/overall
        // timeout is still the fallback if no other peer source pans out.
        if (m_dhtRetryTimer) m_dhtRetryTimer->stop();
        return;
    }
    if (m_done) return;   // already resolved/failed - nothing left to wire up
    m_dhtPeersConn = connect(m_dht, &DhtNode::peersFound, this,
            [this](const QByteArray& infoHash, const QVector<PeerAddress>& peers) {
                if (infoHash != m_mi.infoHash) return;
                onPeersDiscovered(peers);
            });
    m_dht->lookup(m_mi.infoHash);
    if (!m_dhtRetryTimer) {
        // First time this fetch has ever had a DHT node (started with DHT
        // disabled, then it was turned on mid-fetch) - stand up the retry
        // timer exactly like start() would have.
        m_dhtRetryTimer = new QTimer(this);
        connect(m_dhtRetryTimer, &QTimer::timeout, this, [this] {
            if (!m_done && m_dht) m_dht->lookup(m_mi.infoHash);
        });
    }
    if (m_started && !m_dhtRetryTimer->isActive()) m_dhtRetryTimer->start(kDhtRetryMs);
}

void MetadataFetch::addPeerForTest(const PeerAddress& addr) {
    queuePeer(addr);
    if (m_started) openMore();
}

void MetadataFetch::onPeersDiscovered(const QVector<PeerAddress>& peers) {
    if (m_done) return;
    for (const auto& p : peers) queuePeer(p);
    openMore();
}

void MetadataFetch::queuePeer(const PeerAddress& addr) {
    const QString k = peerKey(addr);
    if (m_knownPeers.contains(k)) return;
    m_knownPeers.insert(k);
    m_pendingPeers.append(addr);
}

void MetadataFetch::openMore() {
    if (m_done) return;
    while (m_active.size() < kMaxConcurrent && !m_pendingPeers.isEmpty() && m_attempts < kMaxAttempts)
        openPeer(m_pendingPeers.takeFirst());

    // Every candidate we're ever going to get (this attempt cap) has been
    // tried and none is still in flight: no point waiting for the overall
    // timeout to say so.
    if (m_attempts >= kMaxAttempts && m_active.isEmpty())
        finishFailed(QStringLiteral("exhausted %1 peer attempts without recovering metadata").arg(m_attempts));
}

void MetadataFetch::openPeer(const PeerAddress& addr) {
    ++m_attempts;
    auto* pc = new PeerConnection(addr, m_mi.infoHash, m_peerId, m_limiter, this);
    m_active.append(pc);
    connect(pc, &PeerConnection::metadataComplete, this,
            [this, pc](QByteArray infoDict) { onMetadataComplete(pc, infoDict); });
    connect(pc, &PeerConnection::metadataFailed, this, [this, pc] { onPeerDone(pc); });
    connect(pc, &PeerConnection::disconnected, this, [this, pc](const QString&) { onPeerDone(pc); });
    pc->connectToPeer();
}

void MetadataFetch::onMetadataComplete(PeerConnection* pc, const QByteArray& infoDict) {
    Q_UNUSED(pc);
    if (m_done) return;
    m_done = true;
    if (m_timeoutTimer) m_timeoutTimer->stop();
    if (m_dhtRetryTimer) m_dhtRetryTimer->stop();

    const TorrentMetainfo meta = TorrentMetainfo::fromInfoDict(infoDict, m_mi.trackers);

    // Tear down every connection, including the winner: we're inside its own
    // metadataComplete emit right now, so this MUST be disconnect(this) +
    // deleteLater(), never a synchronous delete (mirrors TorrentTask).
    for (auto* p : m_active) {
        p->disconnect(this);
        p->deleteLater();
    }
    m_active.clear();

    emit metainfoReady(meta, infoDict);
}

void MetadataFetch::onPeerDone(PeerConnection* pc) {
    // metadataFailed and disconnected can both fire for the same peer (or
    // neither more than once); only act the first time this pc is handled.
    if (!m_active.removeOne(pc)) return;
    pc->disconnect(this);
    pc->deleteLater();
    if (m_done) return;
    openMore();
}

void MetadataFetch::finishFailed(const QString& reason) {
    if (m_done) return;
    m_done = true;
    if (m_timeoutTimer) m_timeoutTimer->stop();
    if (m_dhtRetryTimer) m_dhtRetryTimer->stop();
    for (auto* p : m_active) {
        p->disconnect(this);
        p->deleteLater();
    }
    m_active.clear();
    emit failed(reason);
}
