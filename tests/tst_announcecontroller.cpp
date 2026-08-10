#include "torrent/AnnounceController.h"
#include "torrent/ITrackerClient.h"

#include <QSignalSpy>
#include <QtTest>

// A scriptable fake tracker client: each announce() emits whatever the test
// queued for this URL (peers+interval, or a failure), on the next event-loop
// turn. The outcome is snapshotted at the moment announce() is called (not at
// emission time), so a test can re-script the fake and dispatch a second
// round before the first round's queued reply has fired without the second
// dispatch retroactively changing what the first, already-in-flight reply
// says. In manual mode (m_manual), announce() doesn't schedule anything by
// itself: the test calls fireOldest() when it wants that specific dispatch's
// reply delivered (still asynchronously, via a queued invoke) — this is what
// lets a test hold a round's reply "in flight" across a later announce() call
// to construct a deterministic overlap between two rounds.
class FakeClient : public ITrackerClient {
    Q_OBJECT
public:
    struct Pending { bool fail; QVector<PeerAddress> peers; int interval; int minInterval; };

    FakeClient(const QUrl& url, QObject* parent) : ITrackerClient(parent), m_url(url) {}
    QUrl trackerUrl() const override { return m_url; }
    void announce(const QByteArray&, const QByteArray&, quint16, qint64, qint64, TrackerEvent) override {
        ++announces;
        Pending p{m_fail, m_peers, m_interval, m_minInterval};
        if (m_manual) { m_pending.push_back(p); return; }
        QMetaObject::invokeMethod(this, [this, p] {
            if (p.fail) emit announceFailed(QStringLiteral("boom"));
            else emit peersReceived(p.peers, p.interval, p.minInterval);
        }, Qt::QueuedConnection);
    }
    // Manual mode only: deliver the oldest still-pending dispatch's reply.
    void fireOldest() {
        if (m_pending.isEmpty()) return;
        Pending p = m_pending.takeFirst();
        QMetaObject::invokeMethod(this, [this, p] {
            if (p.fail) emit announceFailed(QStringLiteral("boom"));
            else emit peersReceived(p.peers, p.interval, p.minInterval);
        }, Qt::QueuedConnection);
    }
    QUrl m_url; QVector<PeerAddress> m_peers; int m_interval = 0; int m_minInterval = 0; bool m_fail = false;
    int announces = 0;
    bool m_manual = false;
    QVector<Pending> m_pending;
};

class TestAnnounceController : public QObject {
    Q_OBJECT
    // maps URL -> FakeClient so tests can script each one
    QHash<QString, FakeClient*> m_fakes;

    AnnounceController::ClientFactory factory() {
        return [this](const QUrl& u, QObject* parent) -> ITrackerClient* {
            auto* f = new FakeClient(u, parent);
            m_fakes.insert(u.toString(), f);
            return f;
        };
    }

private slots:
    void init() { m_fakes.clear(); }

    // Rewritten from the illustrative brief scaffolding: script every fake
    // client's peers BEFORE calling announce() (trackerCount() forces eager
    // client creation via the factory), then assert the union of what both
    // tiers' fronts reported is forwarded across the round's signal(s).
    void nonHungry_frontOfEachTier_forwardsUnion() {
        QVector<QVector<QUrl>> tiers{{QUrl("udp://a")}, {QUrl("http://b")}};
        AnnounceController ctl(tiers, nullptr, 1u, this, factory());
        QCOMPARE(ctl.trackerCount(), 2); // eagerly creates both fakes

        m_fakes["udp://a"]->m_peers = {{QStringLiteral("1.1.1.1"), 1}};
        m_fakes["http://b"]->m_peers = {{QStringLiteral("2.2.2.2"), 2}};

        QSignalSpy spy(&ctl, &AnnounceController::peersReceived);
        ctl.announce("ih", "pid", 6881, 0, 100, TrackerEvent::Started, /*hungry*/false);

        QTRY_COMPARE(spy.count(), 2); // one emission per tier's front

        QVector<PeerAddress> unionPeers;
        for (const QList<QVariant>& args : spy)
            unionPeers += args.at(0).value<QVector<PeerAddress>>();

        QCOMPARE(unionPeers.size(), 2);
        bool sawA = false, sawB = false;
        for (const PeerAddress& p : unionPeers) {
            if (p.host == QStringLiteral("1.1.1.1") && p.port == 1) sawA = true;
            if (p.host == QStringLiteral("2.2.2.2") && p.port == 2) sawB = true;
        }
        QVERIFY(sawA);
        QVERIFY(sawB);
    }

    void inTierFailover_advancesToNextTracker_andPromotes() {
        QVector<QVector<QUrl>> tiers{{QUrl("udp://dead"), QUrl("http://live")}};
        AnnounceController ctl(tiers, nullptr, 1u, this, factory());
        // Build clients eagerly so we can script them: trackerCount() forces creation.
        QCOMPARE(ctl.trackerCount(), 2);
        m_fakes["udp://dead"]->m_fail = true;
        m_fakes["http://live"]->m_peers = {{QStringLiteral("3.3.3.3"), 3}};
        QSignalSpy ok(&ctl, &AnnounceController::peersReceived);
        ctl.announce("ih", "pid", 6881, 0, 100, TrackerEvent::Started, false);
        QVERIFY(ok.wait(1000));
        QCOMPARE(m_fakes["udp://dead"]->announces, 1);
        QCOMPARE(m_fakes["http://live"]->announces, 1); // advanced to backup
        // Next round: live is now front, dead is not contacted.
        m_fakes["udp://dead"]->announces = 0;
        m_fakes["http://live"]->announces = 0;
        QSignalSpy ok2(&ctl, &AnnounceController::peersReceived);
        ctl.announce("ih", "pid", 6881, 0, 100, TrackerEvent::None, false);
        QVERIFY(ok2.wait(1000));
        QCOMPARE(m_fakes["http://live"]->announces, 1);
        QCOMPARE(m_fakes["udp://dead"]->announces, 0); // promoted 'live' is now front
    }

    void hungry_contactsEveryTracker() {
        QVector<QVector<QUrl>> tiers{{QUrl("udp://a"), QUrl("http://a2")}, {QUrl("udp://b")}};
        AnnounceController ctl(tiers, nullptr, 1u, this, factory());
        QCOMPARE(ctl.trackerCount(), 3);
        for (auto* f : m_fakes) f->m_peers = {{QStringLiteral("4.4.4.4"), 4}};
        ctl.announce("ih", "pid", 6881, 0, 100, TrackerEvent::Started, /*hungry*/true);
        QTRY_COMPARE(m_fakes["udp://a"]->announces, 1);
        QCOMPARE(m_fakes["http://a2"]->announces, 1);
        QCOMPARE(m_fakes["udp://b"]->announces, 1);
    }

    void allFail_emitsAnnounceFailed() {
        QVector<QVector<QUrl>> tiers{{QUrl("udp://x")}};
        AnnounceController ctl(tiers, nullptr, 1u, this, factory());
        QCOMPARE(ctl.trackerCount(), 1);
        m_fakes["udp://x"]->m_fail = true;
        QSignalSpy bad(&ctl, &AnnounceController::announceFailed);
        ctl.announce("ih", "pid", 6881, 0, 100, TrackerEvent::Started, false);
        QVERIFY(bad.wait(1000));
    }

    // Closes the review finding: a hungry round where one client in a
    // multi-client tier fails and another succeeds must forward exactly the
    // successful peers once, must NOT emit announceFailed (the failure isn't
    // "all trackers failed" since a success was seen), and must NOT trigger
    // the non-hungry in-tier advance/promotion machinery (no duplicate
    // announce to either fake).
    void hungry_partialFailure_noSpuriousAnnounceFailed() {
        QVector<QVector<QUrl>> tiers{{QUrl("udp://p1"), QUrl("http://p2")}};
        AnnounceController ctl(tiers, nullptr, 1u, this, factory());
        QCOMPARE(ctl.trackerCount(), 2); // eagerly creates both fakes

        m_fakes["udp://p1"]->m_fail = true;
        m_fakes["http://p2"]->m_peers = {{QStringLiteral("5.5.5.5"), 5}};

        QSignalSpy ok(&ctl, &AnnounceController::peersReceived);
        QSignalSpy bad(&ctl, &AnnounceController::announceFailed);
        ctl.announce("ih", "pid", 6881, 0, 100, TrackerEvent::Started, /*hungry*/true);

        QTRY_COMPARE(ok.count(), 1);
        // Let the event loop settle further so a spurious/late announceFailed
        // or a duplicate announce (from a wrongly-triggered in-tier advance)
        // would have had a chance to show up.
        QTest::qWait(50);

        QCOMPARE(bad.count(), 0);
        QCOMPARE(ok.count(), 1);
        const auto peers = ok.at(0).at(0).value<QVector<PeerAddress>>();
        QCOMPARE(peers.size(), 1);
        QCOMPARE(peers.at(0).host, QStringLiteral("5.5.5.5"));
        QCOMPARE(peers.at(0).port, quint16(5));

        // Each client was contacted exactly once this round: no spurious
        // in-tier fallback re-announce triggered by the failing client.
        QCOMPARE(m_fakes["udp://p1"]->announces, 1);
        QCOMPARE(m_fakes["http://p2"]->announces, 1);
    }

    // Closes the review finding: a hungry round where every client across
    // every tier fails must emit announceFailed exactly once (not zero, not
    // once per failing client) and forward zero peersReceived.
    void hungry_allFail_emitsAnnounceFailedOnce() {
        QVector<QVector<QUrl>> tiers{{QUrl("udp://f1"), QUrl("http://f2")}, {QUrl("udp://f3")}};
        AnnounceController ctl(tiers, nullptr, 1u, this, factory());
        QCOMPARE(ctl.trackerCount(), 3); // eagerly creates all three fakes

        for (auto* f : m_fakes) f->m_fail = true;

        QSignalSpy bad(&ctl, &AnnounceController::announceFailed);
        QSignalSpy ok(&ctl, &AnnounceController::peersReceived);
        ctl.announce("ih", "pid", 6881, 0, 100, TrackerEvent::Started, /*hungry*/true);

        QTRY_COMPARE(bad.count(), 1);
        // Let the loop settle further to make sure it doesn't fire again
        // once all three failures have been processed.
        QTest::qWait(50);

        QCOMPARE(bad.count(), 1);
        QCOMPARE(ok.count(), 0);
    }

    // Overlapping rounds, non-hungry, single tier [A, B]. Under the
    // per-generation model each reply is booked against the round it was
    // dispatched for, and the in-flight skip keeps a later round from ever
    // re-dispatching (and thus corrupting) a client that's still working for
    // an earlier round. A late reply from a round that is STILL LIVE is
    // therefore correctly honored — it belongs to that round's obligation:
    //  - Round 1 dispatches A (tier front). A fails -> in-tier BEP12 fallback
    //    dispatches B in round 1's generation. B is manual, so round 1 stays
    //    live with its single tier-obligation pending on B.
    //  - Round 2 dispatches the front A again (A isn't in flight). Its
    //    obligation (front B never got promoted) is B... no: front is still A.
    //    A is rescripted to succeed, so round 2 emits exactly one peersReceived
    //    carrying A's peer, then finalizes. B (round 1) is untouched by round 2.
    //  - B's round-1 reply (a success) finally lands. Round 1 is still live, so
    //    the reply is attributed to it: peers forwarded, B promoted to front
    //    (BEP12 — a responsive tracker moves up), round 1 finalized. This is
    //    correct behavior, NOT corruption: the counters of round 2 were never
    //    touched (it never re-dispatched B), so no spurious announceFailed and
    //    no counter underflow occur.
    void roundOverlap_lateReplyFromStillLiveRound_isHonoredNotCorrupting() {
        QVector<QVector<QUrl>> tiers{{QUrl("udp://a"), QUrl("udp://b")}};
        AnnounceController ctl(tiers, nullptr, 1u, this, factory());
        QCOMPARE(ctl.trackerCount(), 2);

        FakeClient* a = m_fakes["udp://a"];
        FakeClient* b = m_fakes["udp://b"];
        b->m_manual = true; // hold b's reply in flight until we explicitly fire it
        // B's reply is snapshotted when it is dispatched (the round-1 fallback,
        // below), so script it as a success BEFORE round 1 dispatches it.
        b->m_fail = false;
        b->m_peers = {{QStringLiteral("9.9.9.9"), 9}};

        QSignalSpy peersSpy(&ctl, &AnnounceController::peersReceived);
        QSignalSpy failSpy(&ctl, &AnnounceController::announceFailed);

        // Round 1: A fails, triggering the in-tier fallback to B (held in flight).
        a->m_fail = true;
        ctl.announce("ih", "pid", 6881, 0, 100, TrackerEvent::Started, /*hungry*/false);
        QTest::qWait(50); // let A's queued failure process and dispatch B
        QCOMPARE(a->announces, 1);
        QCOMPARE(b->announces, 1);        // fallback dispatched B this round
        QCOMPARE(b->m_pending.size(), 1); // ...but B's reply is still in flight
        QCOMPARE(peersSpy.count(), 0);
        QCOMPARE(failSpy.count(), 0);

        // Round 2: tier front is still A (B never succeeded), and A is not in
        // flight, so A is re-dispatched. Script A to succeed this time. B stays
        // in flight for round 1 and is NOT touched by round 2.
        a->m_fail = false;
        a->m_peers = {{QStringLiteral("7.7.7.7"), 7}};
        ctl.announce("ih", "pid", 6881, 0, 100, TrackerEvent::None, /*hungry*/false);
        QTRY_COMPARE(peersSpy.count(), 1); // round 2 resolves independently
        QCOMPARE(peersSpy.at(0).at(0).value<QVector<PeerAddress>>().at(0).host,
                 QStringLiteral("7.7.7.7"));
        QCOMPARE(failSpy.count(), 0);

        // Now let round 1's B reply (a success, scripted above) land. Round 1
        // is still live, so it is honored: a second peersReceived carrying B's
        // peer, no failure.
        b->fireOldest();
        QTest::qWait(50);

        QCOMPARE(peersSpy.count(), 2); // round 1's own obligation resolved
        QCOMPARE(peersSpy.at(1).at(0).value<QVector<PeerAddress>>().at(0).host,
                 QStringLiteral("9.9.9.9"));
        QCOMPARE(failSpy.count(), 0);  // never a spurious announceFailed

        // B responded, so BEP12 promoted it to the front: round 3 contacts B.
        a->announces = 0; b->announces = 0;
        ctl.announce("ih", "pid", 6881, 0, 100, TrackerEvent::None, /*hungry*/false);
        QTest::qWait(20);
        QCOMPARE(b->announces, 1);
        QCOMPARE(a->announces, 0);
    }

    // Residual of finding I1 the partial (per-client scalar) guard did NOT
    // close: when the SAME client is re-dispatched in consecutive rounds
    // (steady state in hungry mode — every client, every round), the scalar
    // m_dispatchGen entry is OVERWRITTEN, so a stale round's late reply still
    // passes the generation check and corrupts the current round's counters.
    //
    // Scenario, HUNGRY, single tier [A, B], both manual (replies held in
    // flight). A always fails, B always succeeds.
    //  - Round 1 dispatches A and B (both in flight, replies pending).
    //  - Round 2 starts before any reply fires. Both A and B are still in
    //    flight, so the correct behavior is to SKIP them entirely: round 2
    //    creates no obligations, re-dispatches nobody, and emits nothing
    //    (it's still legitimately waiting on round 1).
    //  - Then round 1's replies fire, A first (fail) then B (success). Round 1
    //    resolves as: one peersReceived (B's peer), and NO announceFailed
    //    (a success was seen).
    //
    // Partial-fix (scalar guard) is WRONG here: round 2 re-dispatches A and B
    // (no in-flight skip) and resets the round-global hungry counters
    // (pending=2, success=false). Round 1's two A-failures then drive pending
    // to 0 with no success yet -> a SPURIOUS, premature announceFailed; the
    // subsequent B successes underflow pending and emit peersReceived twice.
    // The complete per-generation fix attributes each reply to its own round,
    // so round 1 resolves correctly and round 2 stays a silent no-op.
    void hungry_overlappingRounds_staleReplyDoesNotCorruptLaterRound() {
        QVector<QVector<QUrl>> tiers{{QUrl("udp://a"), QUrl("udp://b")}};
        AnnounceController ctl(tiers, nullptr, 1u, this, factory());
        QCOMPARE(ctl.trackerCount(), 2);

        FakeClient* a = m_fakes["udp://a"];
        FakeClient* b = m_fakes["udp://b"];
        a->m_manual = true;
        b->m_manual = true;
        a->m_fail = true;                                  // A always fails
        b->m_fail = false;                                 // B always succeeds
        b->m_peers = {{QStringLiteral("8.8.8.8"), 8}};

        QSignalSpy peersSpy(&ctl, &AnnounceController::peersReceived);
        QSignalSpy failSpy(&ctl, &AnnounceController::announceFailed);

        // Round 1: dispatch both A and B; replies held in flight (manual).
        ctl.announce("ih", "pid", 6881, 0, 100, TrackerEvent::Started, /*hungry*/true);
        QCOMPARE(a->announces, 1);
        QCOMPARE(b->announces, 1);
        QCOMPARE(a->m_pending.size(), 1);
        QCOMPARE(b->m_pending.size(), 1);

        // Round 2: both clients are still in flight -> both skipped. No
        // re-dispatch, no round created, nothing emitted.
        ctl.announce("ih", "pid", 6881, 0, 100, TrackerEvent::None, /*hungry*/true);
        QCOMPARE(a->announces, 1);   // NOT re-dispatched (in flight)
        QCOMPARE(b->announces, 1);   // NOT re-dispatched (in flight)
        QCOMPARE(peersSpy.count(), 0);
        QCOMPARE(failSpy.count(), 0);

        // Now let round 1's replies land: drain all of A's pending (failures)
        // first and process them, then all of B's (successes). This ordering
        // is what makes the partial-fix code emit a premature announceFailed.
        while (!a->m_pending.isEmpty()) a->fireOldest();
        QTest::qWait(30);
        while (!b->m_pending.isEmpty()) b->fireOldest();
        QTest::qWait(30);

        // Correct terminal for round 1: exactly one peersReceived (B's peer),
        // and NO announceFailed (a success was seen this round).
        QCOMPARE(failSpy.count(), 0);
        QCOMPARE(peersSpy.count(), 1);
        const auto peers = peersSpy.at(0).at(0).value<QVector<PeerAddress>>();
        QCOMPARE(peers.size(), 1);
        QCOMPARE(peers.at(0).host, QStringLiteral("8.8.8.8"));
        QCOMPARE(peers.at(0).port, quint16(8));
    }

    // Non-hungry companion: the in-flight skip must also prevent re-dispatching
    // a tier front whose previous announce is still pending, and the pending
    // reply must resolve its own (still-live) round exactly once.
    void nonHungry_inFlightFront_notReDispatched_resolvesOwnRound() {
        QVector<QVector<QUrl>> tiers{{QUrl("udp://only")}};
        AnnounceController ctl(tiers, nullptr, 1u, this, factory());
        QCOMPARE(ctl.trackerCount(), 1);

        FakeClient* f = m_fakes["udp://only"];
        f->m_manual = true;
        f->m_peers = {{QStringLiteral("6.6.6.6"), 6}};

        QSignalSpy peersSpy(&ctl, &AnnounceController::peersReceived);
        QSignalSpy failSpy(&ctl, &AnnounceController::announceFailed);

        // Round 1: dispatch the front; reply held in flight.
        ctl.announce("ih", "pid", 6881, 0, 100, TrackerEvent::Started, /*hungry*/false);
        QCOMPARE(f->announces, 1);

        // Round 2: front is still in flight -> skipped, nobody re-dispatched,
        // nothing emitted.
        ctl.announce("ih", "pid", 6881, 0, 100, TrackerEvent::None, /*hungry*/false);
        QCOMPARE(f->announces, 1);
        QCOMPARE(peersSpy.count(), 0);
        QCOMPARE(failSpy.count(), 0);

        // Round 1's reply resolves its own round exactly once.
        f->fireOldest();
        QTest::qWait(30);
        QCOMPARE(peersSpy.count(), 1);
        QCOMPARE(failSpy.count(), 0);
        QCOMPARE(peersSpy.at(0).at(0).value<QVector<PeerAddress>>().at(0).host,
                 QStringLiteral("6.6.6.6"));
    }
};

QTEST_MAIN(TestAnnounceController)
#include "tst_announcecontroller.moc"
