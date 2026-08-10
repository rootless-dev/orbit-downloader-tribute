#include "torrent/TrackerPeers.h"

#include <QScopeGuard>
#include <QtTest>

class TestTrackerPeers : public QObject {
    Q_OBJECT
private slots:
    void compactV4_parsesEachSixByteRecord() {
        // 1.2.3.4:0x1a0b (6667), 255.0.0.1:80
        QByteArray raw;
        raw.append(char(1)).append(char(2)).append(char(3)).append(char(4)).append(char(0x1a)).append(char(0x0b));
        raw.append(char(255)).append(char(0)).append(char(0)).append(char(1)).append(char(0)).append(char(80));
        const auto peers = TrackerPeers::fromCompactV4(raw);
        QCOMPARE(peers.size(), 2);
        QCOMPARE(peers[0].host, QStringLiteral("1.2.3.4"));
        QCOMPARE(peers[0].port, quint16(0x1a0b));
        QCOMPARE(peers[1].host, QStringLiteral("255.0.0.1"));
        QCOMPARE(peers[1].port, quint16(80));
    }

    void compactV4_ignoresTrailingPartialRecord() {
        QByteArray raw(6 + 3, char(9)); // one full record + 3 stray bytes
        QCOMPARE(TrackerPeers::fromCompactV4(raw).size(), 1);
    }

    void compactV6_parsesEachEighteenByteRecord() {
        QByteArray raw(18, char(0));
        raw[0] = char(0x20); raw[1] = char(0x01); // 2001:...
        raw[15] = char(0x01);                     // ...::1 host-part
        raw[16] = char(0x1a); raw[17] = char(0x0b); // port 6667
        const auto peers = TrackerPeers::fromCompactV6(raw);
        QCOMPARE(peers.size(), 1);
        QCOMPARE(peers[0].port, quint16(0x1a0b));
        QVERIFY(peers[0].host.contains(':')); // an IPv6 literal
        QVERIFY(!peers[0].host.contains('[')); // bracketless — QTcpSocket wants it plain
    }

    void isBogon_flagsAlwaysInvalidButKeepsPrivate() {
        QVERIFY(TrackerPeers::isBogon({QStringLiteral("0.0.0.0"), 6881}));
        QVERIFY(TrackerPeers::isBogon({QStringLiteral("127.0.0.1"), 6881}));
        QVERIFY(TrackerPeers::isBogon({QStringLiteral("::1"), 6881}));
        QVERIFY(TrackerPeers::isBogon({QStringLiteral("239.1.2.3"), 6881})); // multicast
        QVERIFY(TrackerPeers::isBogon({QStringLiteral("1.2.3.4"), 0}));       // port 0
        // Private ranges are KEPT (LAN swarms are legitimate):
        QVERIFY(!TrackerPeers::isBogon({QStringLiteral("192.168.1.5"), 6881}));
        QVERIFY(!TrackerPeers::isBogon({QStringLiteral("10.0.0.9"), 6881}));
        QVERIFY(!TrackerPeers::isBogon({QStringLiteral("1.2.3.4"), 6881}));
    }

    void isBogon_loopbackSeamGatedByEnvVar() {
        // Default (env var unset): existing production behavior is unchanged
        // -- loopback is bogon and dropBogons strips it.
        QVERIFY(TrackerPeers::isBogon({QStringLiteral("127.0.0.1"), 6881}));
        QVector<PeerAddress> v{{QStringLiteral("127.0.0.1"), 6881},
                               {QStringLiteral("1.2.3.4"), 6881}};
        TrackerPeers::dropBogons(v);
        QCOMPARE(v.size(), 1);
        QCOMPARE(v[0].host, QStringLiteral("1.2.3.4"));

        // With the test-only seam enabled: loopback is kept, but every other
        // always-invalid category is still bogon. Scope guard ensures the
        // env var never leaks into other slots, even if a QVERIFY below
        // fails and returns early.
        qputenv("ORBIT_ALLOW_LOOPBACK_PEERS", "1");
        auto unsetGuard = qScopeGuard([] { qunsetenv("ORBIT_ALLOW_LOOPBACK_PEERS"); });
        QVERIFY(!TrackerPeers::isBogon({QStringLiteral("127.0.0.1"), 6881}));
        QVERIFY(TrackerPeers::isBogon({QStringLiteral("0.0.0.0"), 6881}));
        QVERIFY(TrackerPeers::isBogon({QStringLiteral("239.1.2.3"), 6881})); // multicast
        QVERIFY(TrackerPeers::isBogon({QStringLiteral("1.2.3.4"), 0}));       // port 0
        qunsetenv("ORBIT_ALLOW_LOOPBACK_PEERS"); // unset now so the check below sees default behavior

        // Unset again afterward: seam is off, matches default behavior.
        QVERIFY(TrackerPeers::isBogon({QStringLiteral("127.0.0.1"), 6881}));
    }

    void dropBogons_removesInPlace() {
        QVector<PeerAddress> v{{QStringLiteral("1.2.3.4"), 6881},
                               {QStringLiteral("0.0.0.0"), 6881},
                               {QStringLiteral("10.0.0.9"), 6881}};
        TrackerPeers::dropBogons(v);
        QCOMPARE(v.size(), 2);
        QCOMPARE(v[0].host, QStringLiteral("1.2.3.4"));
        QCOMPARE(v[1].host, QStringLiteral("10.0.0.9"));
    }
};

QTEST_MAIN(TestTrackerPeers)
#include "tst_trackerpeers.moc"
