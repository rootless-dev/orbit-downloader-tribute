#include "torrent/UdpTrackerClient.h"

#include "TestUdpTracker.h"

#include <QSignalSpy>
#include <QtEndian>
#include <QtTest>

// Named TstUdpTracker (not TestUdpTracker) to avoid colliding with the
// TestUdpTracker in-process server class included above (see TestUdpTracker.h).
class TstUdpTracker : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { qputenv("ORBIT_UDP_FAST_TIMEOUT", "1"); }
    void cleanupTestCase() { qunsetenv("ORBIT_UDP_FAST_TIMEOUT"); }

    void connectRequest_hasMagicActionAndTxId() {
        const QByteArray req = UdpTrackerProto::buildConnectRequest(0x11223344u);
        QCOMPARE(req.size(), 16);
        QCOMPARE(qFromBigEndian<quint64>(reinterpret_cast<const uchar*>(req.constData())),
                 UdpTrackerProto::kProtocolId);
        QCOMPARE(qFromBigEndian<quint32>(reinterpret_cast<const uchar*>(req.constData() + 8)), 0u); // action=connect
        QCOMPARE(qFromBigEndian<quint32>(reinterpret_cast<const uchar*>(req.constData() + 12)), 0x11223344u);
    }

    void parseConnectResponse_extractsConnectionId() {
        QByteArray dg(16, char(0));
        qToBigEndian<quint32>(0u, reinterpret_cast<uchar*>(dg.data()));       // action=connect
        qToBigEndian<quint32>(0x11223344u, reinterpret_cast<uchar*>(dg.data() + 4)); // txId
        qToBigEndian<quint64>(0xDEADBEEFCAFEULL, reinterpret_cast<uchar*>(dg.data() + 8));
        quint64 connId = 0;
        QVERIFY(UdpTrackerProto::parseConnectResponse(dg, 0x11223344u, &connId));
        QCOMPARE(connId, 0xDEADBEEFCAFEULL);
    }

    void parseConnectResponse_rejectsWrongTxId() {
        QByteArray dg(16, char(0));
        qToBigEndian<quint32>(0x99u, reinterpret_cast<uchar*>(dg.data() + 4));
        quint64 connId = 0;
        QVERIFY(!UdpTrackerProto::parseConnectResponse(dg, 0x11223344u, &connId));
    }

    void announceRequest_layoutIsBep15() {
        const QByteArray ih(20, char(0xA1)), pid(20, char(0xB2));
        const QByteArray req = UdpTrackerProto::buildAnnounceRequest(
            0xCAFEULL, 0x2222u, ih, pid, /*dl*/100, /*left*/200, /*up*/0,
            TrackerEvent::Started, /*key*/0x9999u, /*port*/6881);
        QCOMPARE(req.size(), 98);
        QCOMPARE(qFromBigEndian<quint64>(reinterpret_cast<const uchar*>(req.constData())), 0xCAFEULL);
        QCOMPARE(qFromBigEndian<quint32>(reinterpret_cast<const uchar*>(req.constData() + 8)), 1u); // action=announce
        QCOMPARE(req.mid(16, 20), ih);
        QCOMPARE(req.mid(36, 20), pid);
        QCOMPARE(qFromBigEndian<quint32>(reinterpret_cast<const uchar*>(req.constData() + 80)), 2u); // event=started
        QCOMPARE(qFromBigEndian<quint16>(reinterpret_cast<const uchar*>(req.constData() + 96)), quint16(6881));
    }

    void parseAnnounceResponse_extractsIntervalAndPeers() {
        QByteArray dg(20, char(0));
        qToBigEndian<quint32>(1u, reinterpret_cast<uchar*>(dg.data()));        // action=announce
        qToBigEndian<quint32>(0x2222u, reinterpret_cast<uchar*>(dg.data() + 4)); // txId
        qToBigEndian<quint32>(1800u, reinterpret_cast<uchar*>(dg.data() + 8));  // interval
        // one compact peer 1.2.3.4:6881
        QByteArray peer(6, char(0));
        peer[0] = char(1); peer[1] = char(2); peer[2] = char(3); peer[3] = char(4);
        qToBigEndian<quint16>(quint16(6881), reinterpret_cast<uchar*>(peer.data() + 4)); // port, in-bounds now
        dg += peer;
        int interval = 0; QVector<PeerAddress> peers; QString err; bool isTrackerError = true;
        QVERIFY(UdpTrackerProto::parseAnnounceResponse(dg, 0x2222u, &interval, &peers, &err, &isTrackerError));
        QCOMPARE(interval, 1800);
        QCOMPARE(peers.size(), 1);
        QCOMPARE(peers[0].host, QStringLiteral("1.2.3.4"));
        QCOMPARE(peers[0].port, quint16(6881));
        QVERIFY(!isTrackerError); // success is never a tracker error
    }

    void parseAnnounceResponse_reportsErrorAction() {
        QByteArray dg(8, char(0));
        qToBigEndian<quint32>(3u, reinterpret_cast<uchar*>(dg.data()));        // action=error
        qToBigEndian<quint32>(0x2222u, reinterpret_cast<uchar*>(dg.data() + 4));
        dg += "bad request";
        int interval = 0; QVector<PeerAddress> peers; QString err; bool isTrackerError = false;
        QVERIFY(!UdpTrackerProto::parseAnnounceResponse(dg, 0x2222u, &interval, &peers, &err, &isTrackerError));
        QCOMPARE(err, QStringLiteral("bad request"));
        QVERIFY(isTrackerError); // action==3 with matching txId IS a genuine tracker error
    }

    void parseAnnounceResponse_treatsMismatchedAndMalformedAsNonFatal() {
        // txId mismatch: not for us, must not be flagged as a tracker error.
        {
            QByteArray dg(20, char(0));
            qToBigEndian<quint32>(1u, reinterpret_cast<uchar*>(dg.data()));         // action=announce
            qToBigEndian<quint32>(0x9999u, reinterpret_cast<uchar*>(dg.data() + 4)); // wrong txId
            int interval = 0; QVector<PeerAddress> peers; QString err; bool isTrackerError = true;
            QVERIFY(!UdpTrackerProto::parseAnnounceResponse(dg, 0x2222u, &interval, &peers, &err, &isTrackerError));
            QVERIFY(!isTrackerError);
        }
        // Short/garbage datagram: also not a tracker error, just junk.
        {
            const QByteArray dg(4, char(0xFF));
            int interval = 0; QVector<PeerAddress> peers; QString err; bool isTrackerError = true;
            QVERIFY(!UdpTrackerProto::parseAnnounceResponse(dg, 0x2222u, &interval, &peers, &err, &isTrackerError));
            QVERIFY(!isTrackerError);
        }
    }

    void timeoutSecs_bep15CappedSchedule() {
        QCOMPARE(UdpTrackerProto::timeoutSecs(0), 15);
        QCOMPARE(UdpTrackerProto::timeoutSecs(1), 30);
        QCOMPARE(UdpTrackerProto::timeoutSecs(2), 60);
    }

    void liveAnnounce_returnsConfiguredPeers() {
        TestUdpTracker server;
        server.setPeers({{QStringLiteral("9.8.7.6"), 6881}});
        server.setInterval(1200);

        const QUrl url(QStringLiteral("udp://127.0.0.1:%1").arg(server.port()));
        UdpTrackerClient client(url, /*rngSeed*/12345u);
        QSignalSpy ok(&client, &ITrackerClient::peersReceived);
        QSignalSpy bad(&client, &ITrackerClient::announceFailed);

        client.announce(QByteArray(20, char(0xA1)), QByteArray(20, char(0xB2)), 6881, 0, 100, TrackerEvent::Started);
        QVERIFY(ok.wait(4000));
        QCOMPARE(bad.count(), 0);
        const auto args = ok.takeFirst();
        const auto peers = args[0].value<QVector<PeerAddress>>();
        QCOMPARE(peers.size(), 1);
        QCOMPARE(peers[0].host, QStringLiteral("9.8.7.6"));
        QCOMPARE(args[1].toInt(), 1200); // interval
    }

    void liveAnnounce_duplicateReplyDoesNotUseAfterFree() {
        // Covers Finding 1: the server sends the real announce reply TWICE,
        // back-to-back, simulating a second datagram already queued on the
        // exchange's socket at the moment it resolves. On the pre-fix code,
        // cleanup() deletes `ex` synchronously while the readyRead lambda (which
        // captured `ex`) can still be re-invoked for the still-pending second
        // datagram -> use-after-free. Assert exactly one peersReceived, no
        // announceFailed, and (implicitly) no crash.
        TestUdpTracker server;
        server.setPeers({{QStringLiteral("9.8.7.6"), 6881}});
        server.setInterval(1200);
        server.setDuplicateAnnounceReply(true);

        const QUrl url(QStringLiteral("udp://127.0.0.1:%1").arg(server.port()));
        UdpTrackerClient client(url, /*rngSeed*/222u);
        QSignalSpy ok(&client, &ITrackerClient::peersReceived);
        QSignalSpy bad(&client, &ITrackerClient::announceFailed);

        client.announce(QByteArray(20, char(0xA1)), QByteArray(20, char(0xB2)), 6881, 0, 100, TrackerEvent::Started);
        QVERIFY(ok.wait(4000));
        // Pump the event loop a bit longer so a re-fired readyRead (or a stray
        // deferred deletion) has a chance to run and (pre-fix) crash/misbehave.
        QTest::qWait(300);
        QCOMPARE(bad.count(), 0);
        QCOMPARE(ok.count(), 1); // exactly one success, the duplicate was ignored
        const auto args = ok.at(0);
        const auto peers = args[0].value<QVector<PeerAddress>>();
        QCOMPARE(peers.size(), 1);
        QCOMPARE(peers[0].host, QStringLiteral("9.8.7.6"));
    }

    void liveAnnounce_junkDatagramIsIgnoredNotFatal() {
        // Covers Finding 2: the server sends a stray 4-byte garbage datagram
        // immediately before the real announce reply. On the pre-fix code any
        // parse failure other than the exact string "transaction id mismatch"
        // is treated as fatal, so the junk datagram tears down the exchange
        // before the real reply is ever read -> announceFailed, no peers.
        // After the fix, junk is ignored and the exchange survives to receive
        // the real reply.
        TestUdpTracker server;
        server.setPeers({{QStringLiteral("5.6.7.8"), 6882}});
        server.setInterval(900);
        server.setSendJunkDatagram(true);

        const QUrl url(QStringLiteral("udp://127.0.0.1:%1").arg(server.port()));
        UdpTrackerClient client(url, /*rngSeed*/333u);
        QSignalSpy ok(&client, &ITrackerClient::peersReceived);
        QSignalSpy bad(&client, &ITrackerClient::announceFailed);

        client.announce(QByteArray(20, char(0xC1)), QByteArray(20, char(0xD2)), 6882, 0, 50, TrackerEvent::Started);
        QVERIFY(ok.wait(4000));
        QCOMPARE(bad.count(), 0);
        const auto args = ok.takeFirst();
        const auto peers = args[0].value<QVector<PeerAddress>>();
        QCOMPARE(peers.size(), 1);
        QCOMPARE(peers[0].host, QStringLiteral("5.6.7.8"));
    }

    void liveAnnounce_failsOnSilentTracker() {
        // Point at a closed port; the client must give up (capped retries) and
        // emit announceFailed rather than hang forever.
        UdpTrackerClient client(QUrl(QStringLiteral("udp://127.0.0.1:1")), 1u);
        QSignalSpy bad(&client, &ITrackerClient::announceFailed);
        client.announce(QByteArray(20, char(0)), QByteArray(20, char(0)), 6881, 0, 1, TrackerEvent::Started);
        // With the test override below, the retry schedule is compressed so this
        // resolves quickly; see UdpTrackerClient's ORBIT_UDP_FAST_TIMEOUT seam.
        QVERIFY(bad.wait(6000));
    }

    void destroyMidFlight_silentTracker_doesNotLeakOrCrash() {
        // Fix 2 regression: the per-announce heap Exchange was freed only by
        // cleanup() on a terminal signal path (success/failure/give-up). If
        // the client is destroyed while an exchange is still in flight (e.g.
        // pause/cancel during a dead tracker's ~15/30/60s retry window), the
        // QObject-parented sock/timer get torn down and the this-context
        // lambdas auto-disconnect, so cleanup() never runs -> Exchange leaks.
        // Point at a closed/silent port so the exchange never resolves, then
        // destroy the client immediately: it must not crash (the dtor sweep
        // over m_liveExchanges frees the still-live Exchange instead).
        auto* client = new UdpTrackerClient(QUrl(QStringLiteral("udp://127.0.0.1:1")), 42u);
        client->announce(QByteArray(20, char(0)), QByteArray(20, char(0)), 6881, 0, 1, TrackerEvent::Started);
        // Don't wait for the retry/give-up schedule: the exchange is still
        // alive and in flight (Connecting phase, first timer running) here.
        delete client;
        // Let the event loop turn over once more; nothing should fire (no
        // dangling lambda should still be connected to anything live).
        QTest::qWait(50);
        // Reaching this point without crashing/asserting is the pass
        // condition. (No ASan/leaks build is wired into this project's
        // CMake; under `leaks --atExit -- ./tst_udptracker` this test's
        // Exchange must not show up as a leak — see final-fix-report.md.)
    }
};

QTEST_MAIN(TstUdpTracker)
#include "tst_udptracker.moc"
