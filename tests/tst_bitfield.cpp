#include <QtTest>
#include "torrent/Bitfield.h"

class TstBitfield : public QObject { Q_OBJECT
private slots:
    void setsAndCounts() { Bitfield bf(10); QVERIFY(!bf.has(3)); bf.set(3);
        QVERIFY(bf.has(3)); QCOMPARE(bf.count(),1); QVERIFY(!bf.isComplete()); }
    void wireBitOrderMsbFirst() { Bitfield bf(8); bf.set(0);
        QCOMPARE(bf.toBytes(), QByteArray(1, char(0x80))); }   // piece 0 -> MSB
    void roundTripsBytes() { Bitfield bf(12); bf.set(0); bf.set(11);
        auto b = bf.toBytes(); auto bf2 = Bitfield::fromBytes(b, 12);
        QVERIFY(bf2.has(0)); QVERIFY(bf2.has(11)); QVERIFY(!bf2.has(5)); }
    void completeWhenAllSet() { Bitfield bf(3); bf.set(0); bf.set(1); bf.set(2);
        QVERIFY(bf.isComplete()); }
};
QTEST_MAIN(TstBitfield)
#include "tst_bitfield.moc"
