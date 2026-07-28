#include <QtTest>
#include "torrent/RoutingTable.h"

static DhtNodeEntry mk(quint32 seed, quint16 port) {
    return DhtNodeEntry{ NodeId::fromSeed(seed), "127.0.0.1", port };
}
class TstRoutingTable : public QObject {
    Q_OBJECT
private slots:
    void storesAndReturnsClosest() {
        RoutingTable rt(NodeId::fromSeed(0), 8);
        // Some inserts may be dropped: ~half of random ids land in bucket 0
        // (first bit differs), which caps at k=8. That is correct behavior, so
        // do NOT assert every sawNode() succeeds.
        for (quint32 s = 1; s <= 20; ++s) rt.sawNode(mk(s, quint16(1000 + s)));
        QVERIFY(rt.nodeCount() > 0);
        auto near = rt.closest(NodeId::fromSeed(1), 4);
        QVERIFY(near.size() >= 1 && near.size() <= 4);
        // seed-1 is inserted first (its bucket is empty then), so it is always
        // retained and is the unique nearest node to its own id.
        QCOMPARE(near.first().id.bytes(), NodeId::fromSeed(1).bytes());
    }
    void retainsNodesAcrossManyBuckets() {
        RoutingTable rt(NodeId::fromSeed(0), 2); // small k per bucket
        int stored = 0;
        for (quint32 s = 1; s <= 40; ++s) if (rt.sawNode(mk(s, quint16(s)))) ++stored;
        // Random ids spread across many per-bit buckets, each capped at k=2,
        // so the table retains far more than a single bucket's worth.
        QVERIFY(stored > 2);
    }
    void markBadFreesSpaceInSameBucket() {
        // self = all-zero; two ids whose highest differing bit is identical
        // (last byte 0x02 and 0x03 -> bucketIndex 158 for both) share a bucket.
        RoutingTable rt(NodeId(QByteArray(20, '\x00')), 1); // k=1
        DhtNodeEntry a{ NodeId(QByteArray(19,'\x00') + QByteArray(1,'\x02')), "127.0.0.1", 1 };
        DhtNodeEntry b{ NodeId(QByteArray(19,'\x00') + QByteArray(1,'\x03')), "127.0.0.1", 2 };
        QVERIFY(rt.sawNode(a));
        QVERIFY(!rt.sawNode(b));   // bucket full (k=1), a is good -> b dropped
        rt.markBad(a.id);
        QVERIFY(rt.sawNode(b));    // a marked bad -> evicted, b admitted
    }
};
QTEST_APPLESS_MAIN(TstRoutingTable)
#include "tst_routingtable.moc"
