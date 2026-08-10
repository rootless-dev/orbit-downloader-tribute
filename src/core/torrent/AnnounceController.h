#pragma once

#include "torrent/ITrackerClient.h"
#include "torrent/TrackerTypes.h"

#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QSet>
#include <QUrl>
#include <QVector>

#include <functional>

class QNetworkAccessManager;

// Owns the announce tiers (BEP 12) and every ITrackerClient, and presents
// TorrentTask the same two signals a single tracker did. It routes WHICH
// trackers to contact; TorrentTask still owns WHEN (the announce timer/cadence).
class AnnounceController : public QObject {
    Q_OBJECT
public:
    using ClientFactory = std::function<ITrackerClient*(const QUrl&, QObject* parent)>;

    AnnounceController(const QVector<QVector<QUrl>>& tiers, QNetworkAccessManager* nam,
                       quint32 rngSeed, QObject* parent = nullptr, ClientFactory factory = {});

    int trackerCount() const;

    // hungry=false: BEP 12 — front of each tier, advancing to the next tracker
    // in a tier on failure, promoting the responder to front. hungry=true:
    // contact every tracker in every tier this round.
    void announce(const QByteArray& infoHash, const QByteArray& peerId, quint16 port,
                  qint64 downloaded, qint64 left, TrackerEvent ev, bool hungry);

signals:
    void peersReceived(QVector<PeerAddress> peers, int intervalSecs, int minIntervalSecs);
    void announceFailed(QString reason);

private:
    struct Tier {
        QVector<ITrackerClient*> clients; // index 0 = current front
    };

    // Per-round accounting record, keyed by the generation that created it.
    // Each async reply is attributed to the exact round it was dispatched for
    // (via m_dispatchGen), so overlapping rounds never share counters.
    struct Round {
        bool hungry = false;
        int  outstanding = 0;    // obligations still awaiting a terminal reply
        bool anySuccess = false; // did any client succeed this round
        int  interval = 0;       // min positive interval seen this round
        int  minInterval = 0;    // max min-interval seen this round
    };

    ITrackerClient* makeClient(const QUrl& url);
    void onClientPeers(int tierIdx, ITrackerClient* c, const QVector<PeerAddress>& peers,
                       int interval, int minInterval);
    void onClientFailed(int tierIdx, ITrackerClient* c, const QString& why);
    void dispatch(quint64 gen, ITrackerClient* c); // mark in-flight for gen and announce()

    QNetworkAccessManager* m_nam;
    quint32                m_rngSeed;
    ClientFactory          m_factory;
    QVector<Tier>          m_tiers;

    // Announce params of the most recent announce(), retained because a
    // non-hungry round's in-tier BEP12 fallback re-dispatches a backup client
    // for the SAME round after the initial call has returned.
    QByteArray m_infoHash, m_peerId;
    quint16    m_port = 0;
    qint64     m_downloaded = 0, m_left = 0;
    TrackerEvent m_ev = TrackerEvent::None;

    // Overlap safety. A repeating announce cadence can start round N+1 while
    // round N's async client replies are still in flight (a dead UDP tracker's
    // give-up can outlast the starved cadence — the steady state in hungry
    // mode, where every client is re-contacted every round). Two invariants
    // keep overlapping rounds from corrupting each other:
    //   1. A client already in m_inFlight is NEVER re-dispatched: a later round
    //      simply skips it (it's still working for the earlier round).
    //   2. Every reply carries the generation it was dispatched for
    //      (m_dispatchGen) and is booked against that generation's Round in
    //      m_rounds — not against whatever round happens to be current. A reply
    //      whose round has already been finalized (absent from m_rounds) is
    //      ignored outright.
    // m_generation is bumped at the top of every announce(); each live round is
    // finalized and erased once its outstanding count reaches 0.
    quint64 m_generation = 0;
    QHash<quint64, Round>           m_rounds;      // live rounds, keyed by generation
    QSet<ITrackerClient*>           m_inFlight;    // clients with a pending announce
    QHash<ITrackerClient*, quint64> m_dispatchGen; // generation each in-flight client was dispatched in
};
