#include <QtTest>
#include "torrent/NodeId.h"

class TstNodeId : public QObject {
    Q_OBJECT
private slots:
    void closerByXorDistance() {
        NodeId target(QByteArray(20, '\x00'));
        NodeId a(QByteArray(19, '\x00') + QByteArray(1, '\x01')); // distance 1
        NodeId b(QByteArray(19, '\x00') + QByteArray(1, '\xFF')); // distance 255
        QVERIFY(NodeId::closer(a, b, target));
        QVERIFY(!NodeId::closer(b, a, target));
    }
    void bucketIndexIsHighestDifferingBit() {
        NodeId zero(QByteArray(20, '\x00'));
        NodeId one(QByteArray(19, '\x00') + QByteArray(1, '\x01')); // differ in last bit
        QCOMPARE(zero.bucketIndex(one), 159);
        QCOMPARE(zero.bucketIndex(zero), 160);
        NodeId top(QByteArray(1, '\x80') + QByteArray(19, '\x00'));
        QCOMPARE(zero.bucketIndex(top), 0);
    }
    void fromSeedIsDeterministic() {
        QCOMPARE(NodeId::fromSeed(42).bytes(), NodeId::fromSeed(42).bytes());
        QVERIFY(NodeId::fromSeed(1).bytes() != NodeId::fromSeed(2).bytes());
        QCOMPARE(NodeId::fromSeed(1).bytes().size(), 20);
    }
};
QTEST_APPLESS_MAIN(TstNodeId)
#include "tst_nodeid.moc"
