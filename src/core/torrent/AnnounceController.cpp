#include "torrent/AnnounceController.h"

#include "torrent/HttpTrackerClient.h"
#include "torrent/UdpTrackerClient.h"

#include <QNetworkAccessManager>

AnnounceController::AnnounceController(const QVector<QVector<QUrl>>& tiers, QNetworkAccessManager* nam,
                                       quint32 rngSeed, QObject* parent, ClientFactory factory)
    : QObject(parent), m_nam(nam), m_rngSeed(rngSeed), m_factory(std::move(factory)) {
    // Eagerly build every client so behavior (and tests) can rely on them
    // existing up-front; ordering within a tier is the announce-list order.
    for (int t = 0; t < tiers.size(); ++t) {
        Tier tier;
        for (const QUrl& url : tiers[t]) {
            ITrackerClient* c = makeClient(url);
            if (!c) continue;
            const int tierIdx = t;
            connect(c, &ITrackerClient::peersReceived, this,
                    [this, tierIdx, c](QVector<PeerAddress> peers, int interval, int minInterval) {
                        onClientPeers(tierIdx, c, peers, interval, minInterval);
                    });
            connect(c, &ITrackerClient::announceFailed, this,
                    [this, tierIdx, c](const QString& why) { onClientFailed(tierIdx, c, why); });
            tier.clients.append(c);
        }
        if (!tier.clients.isEmpty()) m_tiers.append(tier);
    }
}

ITrackerClient* AnnounceController::makeClient(const QUrl& url) {
    if (m_factory) return m_factory(url, this);
    const QString scheme = url.scheme().toLower();
    if (scheme == QLatin1String("http") || scheme == QLatin1String("https"))
        return m_nam ? new HttpTrackerClient(m_nam, url, this) : nullptr;
    if (scheme == QLatin1String("udp"))
        return new UdpTrackerClient(url, m_rngSeed, this);
    return nullptr; // unknown scheme dropped
}

int AnnounceController::trackerCount() const {
    int n = 0;
    for (const Tier& t : m_tiers) n += t.clients.size();
    return n;
}

void AnnounceController::announce(const QByteArray& infoHash, const QByteArray& peerId, quint16 port,
                                  qint64 downloaded, qint64 left, TrackerEvent ev, bool hungry) {
    const quint64 gen = ++m_generation; // new round
    m_infoHash = infoHash; m_peerId = peerId; m_port = port;
    m_downloaded = downloaded; m_left = left; m_ev = ev;

    if (m_tiers.isEmpty()) { emit announceFailed(QStringLiteral("no usable trackers")); return; }

    // Build this round's obligations, skipping any client whose previous
    // announce is still in flight (it's still working for an earlier round and
    // must not be re-dispatched or counted here).
    QVector<ITrackerClient*> toDispatch;
    if (hungry) {
        // Every not-in-flight client across every tier.
        for (const Tier& tier : m_tiers)
            for (ITrackerClient* c : tier.clients)
                if (!m_inFlight.contains(c)) toDispatch.append(c);
    } else {
        // The front (index 0) of every tier whose front isn't in flight.
        for (const Tier& tier : m_tiers) {
            if (tier.clients.isEmpty()) continue;
            ITrackerClient* front = tier.clients.front();
            if (!m_inFlight.contains(front)) toDispatch.append(front);
        }
    }

    // Everything was skipped (or nothing to do): we're still legitimately
    // waiting on in-flight announces — create no round and emit nothing.
    if (toDispatch.isEmpty()) return;

    // Record the round before dispatching so a (hypothetical) synchronous
    // reply is booked against a round that already exists.
    m_rounds.insert(gen, Round{hungry, static_cast<int>(toDispatch.size()), false, 0, 0});
    for (ITrackerClient* c : toDispatch) dispatch(gen, c);
}

void AnnounceController::dispatch(quint64 gen, ITrackerClient* c) {
    m_inFlight.insert(c);
    m_dispatchGen.insert(c, gen);
    c->announce(m_infoHash, m_peerId, m_port, m_downloaded, m_left, m_ev);
}

void AnnounceController::onClientPeers(int tierIdx, ITrackerClient* c, const QVector<PeerAddress>& peers,
                                       int interval, int minInterval) {
    const quint64 gen = m_dispatchGen.take(c);
    m_inFlight.remove(c);

    auto it = m_rounds.find(gen);
    if (it == m_rounds.end()) return; // round already finalized: ignore entirely
    Round& r = it.value();

    // Promote the responder to the front of its tier (persists across rounds).
    Tier& tier = m_tiers[tierIdx];
    const int at = tier.clients.indexOf(c);
    if (at > 0) tier.clients.move(at, 0);

    // Aggregate this round's interval/min-interval (min positive / max min).
    if (interval > 0 && (r.interval == 0 || interval < r.interval)) r.interval = interval;
    if (minInterval > r.minInterval) r.minInterval = minInterval;
    r.anySuccess = true;

    // Snapshot before emitting: a slot could re-enter announce() and rehash
    // m_rounds, invalidating the iterator/reference.
    const int roundInterval = r.interval;
    const int roundMin = r.minInterval;
    if (--r.outstanding == 0) m_rounds.remove(gen); // finalize (success -> emit nothing extra)

    emit peersReceived(peers, roundInterval, roundMin);
}

void AnnounceController::onClientFailed(int tierIdx, ITrackerClient* c, const QString& /*why*/) {
    const quint64 gen = m_dispatchGen.take(c);
    m_inFlight.remove(c);

    auto it = m_rounds.find(gen);
    if (it == m_rounds.end()) return; // round already finalized: ignore entirely
    Round& r = it.value();

    if (!r.hungry) {
        // Non-hungry BEP12 in-tier fallback: advance to the next client in this
        // tier that isn't already in flight (skipping c itself). The tier is
        // still one outstanding obligation, so outstanding is unchanged.
        Tier& tier = m_tiers[tierIdx];
        const int at = tier.clients.indexOf(c);
        for (int i = at + 1; i < tier.clients.size(); ++i) {
            if (!m_inFlight.contains(tier.clients[i])) { dispatch(gen, tier.clients[i]); return; }
        }
        // Tier exhausted: fall through to decrement this obligation.
    }

    // Hungry client failed, or a non-hungry tier ran out of backups.
    if (--r.outstanding == 0) {
        const bool anySuccess = r.anySuccess;
        m_rounds.remove(gen); // finalize
        if (!anySuccess) emit announceFailed(QStringLiteral("all trackers failed"));
    }
}
