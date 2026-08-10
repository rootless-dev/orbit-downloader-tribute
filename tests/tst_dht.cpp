#include <QFile>
#include <QSet>
#include <QTemporaryDir>
#include <QtTest>

#include "torrent/DhtNode.h"

class TstDht : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {
        qputenv("ORBIT_ALLOW_LOOPBACK_PEERS", "1");
        qputenv("ORBIT_UDP_FAST_TIMEOUT", "1");
    }

    void pingPopulatesRoutingTable() {
        DhtNode a(NodeId::fromSeed(1), 0, 1), b(NodeId::fromSeed(2), 0, 2);
        QVERIFY(a.start());
        QVERIFY(b.start());
        a.ping("127.0.0.1", b.boundPort());
        QTRY_VERIFY_WITH_TIMEOUT(a.nodeCount() >= 1, 3000); // learned b from its pong
        QTRY_VERIFY_WITH_TIMEOUT(b.nodeCount() >= 1, 3000); // learned a from its ping
    }

    void findNodeLearnsThirdNode() {
        DhtNode a(NodeId::fromSeed(1), 0, 1), b(NodeId::fromSeed(2), 0, 2), c(NodeId::fromSeed(3), 0, 3);
        QVERIFY(a.start()); QVERIFY(b.start()); QVERIFY(c.start());
        b.ping("127.0.0.1", c.boundPort());                 // B knows C
        QTRY_VERIFY_WITH_TIMEOUT(b.nodeCount() >= 1, 3000);
        a.findNodeForTest("127.0.0.1", b.boundPort(), c.id()); // test-only shim -> internal findNode
        QTRY_VERIFY_WITH_TIMEOUT(a.nodeCount() >= 2, 4000);   // A learned B and (via B's nodes) C
    }

    void getPeersReturnsAnnouncedPeer() {
        DhtNode a(NodeId::fromSeed(1), 0, 1), b(NodeId::fromSeed(2), 0, 2);
        QVERIFY(a.start()); QVERIFY(b.start());
        const QByteArray ih(20, '\x07');
        // C and D announce themselves on B via the raw wire (test shim): get_peers to obtain token, then
        // announce_peer. Two distinct peers are stored (not one) so this test would fail if "values" were
        // still (wrongly) encoded/parsed as a single concatenated compact-peer string per BEP 5 instead of a
        // bencode list of 6-byte strings: a concatenated-string reader can accidentally still "work" for a
        // single peer, but genuinely exercising the list format requires proving multiple distinct peers
        // round-trip correctly.
        b.storePeerForTest(ih, "203.0.113.9", 6881);        // test-only: seed B's peer store directly
        b.storePeerForTest(ih, "198.51.100.7", 6882);
        QVector<PeerAddress> got;
        connect(&a, &DhtNode::peersFound, this, [&](QByteArray, QVector<PeerAddress> p){ got = p; });
        a.getPeersForTest("127.0.0.1", b.boundPort(), ih);   // single-hop get_peers -> emits peersFound
        QTRY_VERIFY_WITH_TIMEOUT(got.size() == 2, 3000);
        QSet<QString> hosts;
        for (const PeerAddress& p : got) hosts.insert(p.host + ":" + QString::number(p.port));
        QVERIFY(hosts.contains("203.0.113.9:6881"));
        QVERIFY(hosts.contains("198.51.100.7:6882"));
    }
    void announcePeerRequiresValidToken() {
        DhtNode b(NodeId::fromSeed(2), 0, 2); QVERIFY(b.start());
        QVERIFY(!b.acceptAnnounceForTest(QByteArray(20,'\x08'), "1.2.3.4", 5, "badtoken"));
    }

    void lookupFindsPeerAcrossMesh() {
        // router R knows everyone; nodes N1..N4 form the reachable set; holder H stores the info_hash peer.
        DhtNode R(NodeId::fromSeed(100), 0, 100); QVERIFY(R.start());
        QList<DhtNode*> mesh;
        for (quint32 s = 1; s <= 4; ++s) { auto* n = new DhtNode(NodeId::fromSeed(s), 0, s, this);
            QVERIFY(n->start()); n->ping("127.0.0.1", R.boundPort()); mesh << n; }
        DhtNode H(NodeId::fromSeed(200), 0, 200); QVERIFY(H.start());
        H.ping("127.0.0.1", R.boundPort());
        const QByteArray ih(20, '\x42');
        H.storePeerForTest(ih, "198.51.100.7", 51413);
        QTRY_VERIFY_WITH_TIMEOUT(R.nodeCount() >= 5, 4000); // R learned the mesh + H
        DhtNode seeker(NodeId::fromSeed(9), 0, 9); QVERIFY(seeker.start());
        seeker.ping("127.0.0.1", R.boundPort());
        QTRY_VERIFY_WITH_TIMEOUT(seeker.nodeCount() >= 1, 3000);
        QVector<PeerAddress> got;
        connect(&seeker, &DhtNode::peersFound, this, [&](QByteArray, QVector<PeerAddress> p){ got += p; });
        seeker.lookup(ih);
        QTRY_VERIFY_WITH_TIMEOUT(!got.isEmpty(), 8000);
        QCOMPARE(got.first().host, QString("198.51.100.7"));
    }

    void bootstrapsAgainstLoopbackRouter() {
        DhtNode router(NodeId::fromSeed(100), 0, 100); QVERIFY(router.start());
        DhtNode n(NodeId::fromSeed(1), 0, 1); QVERIFY(n.start());
        QSignalSpy spy(&n, &DhtNode::bootstrapped);
        n.bootstrap({ QString("127.0.0.1:%1").arg(router.boundPort()) }); // numeric host -> no DNS needed
        QVERIFY(spy.wait(4000));
        QVERIFY(n.nodeCount() >= 1);
    }

    void bootstrappedNotEmittedWithoutBootstrap() {
        // A plain ping (never routed through bootstrap()) must not fire
        // bootstrapped(), even though it fills the routing table just like a
        // real bootstrap response would -- the signal is scoped to
        // bootstrap() itself, not to "table went non-empty".
        DhtNode a(NodeId::fromSeed(1), 0, 1), b(NodeId::fromSeed(2), 0, 2);
        QVERIFY(a.start());
        QVERIFY(b.start());
        QSignalSpy spy(&a, &DhtNode::bootstrapped);
        a.ping("127.0.0.1", b.boundPort());
        QTRY_VERIFY_WITH_TIMEOUT(a.nodeCount() >= 1, 3000);
        QCOMPARE(spy.count(), 0);
    }

    void persistsNodeIdAndNodes() {
        QTemporaryDir dir; const QString path = dir.path() + "/dht.dat";
        {
            DhtNode a(NodeId::fromSeed(5), 0, 5); QVERIFY(a.start());
            a.seedNodeForTest(DhtNodeEntry{ NodeId::fromSeed(6), "127.0.0.1", 6881 });
            a.saveState(path);
        }
        DhtNode b(NodeId::fromSeed(999), 0, 999); QVERIFY(b.start());
        QVERIFY(b.loadState(path));
        QCOMPARE(b.id().bytes(), NodeId::fromSeed(5).bytes()); // id restored, not the ctor's
        QVERIFY(b.nodeCount() >= 1);                            // node restored
    }

    void loadStateReturnsFalseForMissingOrCorruptFile() {
        DhtNode b(NodeId::fromSeed(999), 0, 999); QVERIFY(b.start());
        QVERIFY(!b.loadState("/nonexistent/path/dht.dat"));
        QCOMPARE(b.id().bytes(), NodeId::fromSeed(999).bytes()); // unchanged on missing file

        QTemporaryDir dir; const QString path = dir.path() + "/garbage.dat";
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("not a valid dht.dat file");
        f.close();
        QVERIFY(!b.loadState(path));
        QCOMPARE(b.id().bytes(), NodeId::fromSeed(999).bytes()); // unchanged on corrupt file
    }

    void loadStateRejectsBogusNodeCount() {
        QTemporaryDir dir; const QString path = dir.path() + "/huge_count.dat";
        // Construct a file with valid magic+version+id but bogus huge count to test robustness
        QByteArray blob;
        QDataStream out(&blob, QIODevice::WriteOnly);
        out.setVersion(QDataStream::Qt_5_15);
        out << quint32(0x4F524244u) << quint32(1);  // magic, version
        out << NodeId::fromSeed(7).bytes();          // id
        out << quint32(0xFFFFFFFFu);                 // huge count (should be rejected)
        // (no node records follow)

        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(blob);
        f.close();

        DhtNode b(NodeId::fromSeed(999), 0, 999); QVERIFY(b.start());
        QVERIFY(!b.loadState(path));  // should reject the huge count
        QCOMPARE(b.id().bytes(), NodeId::fromSeed(999).bytes()); // id unchanged on corrupt count
    }
};

QTEST_MAIN(TstDht)
#include "tst_dht.moc"
