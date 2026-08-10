#include <QtTest>
#include "torrent/Bencode.h"

class TstBencode : public QObject { Q_OBJECT
private slots:
    void decodesInt() {
        bool ok=false; auto v = Bencode::decode("i42e", &ok);
        QVERIFY(ok); QCOMPARE(v.type(), BencodeValue::Type::Int); QCOMPARE(v.toInt(), 42LL);
    }
    void decodesNegativeInt() {
        bool ok=false; auto v = Bencode::decode("i-7e", &ok);
        QVERIFY(ok); QCOMPARE(v.toInt(), -7LL);
    }
    void rejectsLeadingZeroAndNegZero() {
        bool ok=true; Bencode::decode("i03e", &ok); QVERIFY(!ok);
        ok=true; Bencode::decode("i-0e", &ok); QVERIFY(!ok);
    }
    void decodesByteString() {
        bool ok=false; auto v = Bencode::decode("4:spam", &ok);
        QVERIFY(ok); QCOMPARE(v.toBytes(), QByteArray("spam"));
    }
    void decodesListAndDict() {
        bool ok=false; auto v = Bencode::decode("d3:cow3:moo4:spam4:eggse", &ok);
        QVERIFY(ok); QCOMPARE(v.type(), BencodeValue::Type::Dict);
        QCOMPARE(v[QByteArray("cow")].toBytes(), QByteArray("moo"));
    }
    void rejectsTruncated() { bool ok=true; Bencode::decode("i42", &ok); QVERIFY(!ok); }
    void roundTrips() {
        QByteArray in = "d1:ai1e1:bl3:foo3:baree";
        bool ok=false; auto v = Bencode::decode(in, &ok); QVERIFY(ok);
        QCOMPARE(Bencode::encode(v), in);   // canonical (sorted keys) matches this canonical input
    }
    void exposesRawSpanOfNestedValue() {
        QByteArray in = "d4:infod1:ni1eee";
        bool ok=false; auto v = Bencode::decode(in, &ok); QVERIFY(ok);
        const BencodeValue& info = v[QByteArray("info")];
        QCOMPARE(in.mid(info.rawBegin(), info.rawEnd()-info.rawBegin()), QByteArray("d1:ni1ee"));
    }
    // Attacker-controlled bencode (e.g. a peer's ut_metadata payload) with
    // deep nesting must fail cleanly rather than overflow the native stack
    // via unbounded recursive descent. 300 nested "l"s is well past the
    // 200-deep cap but small enough to keep the test fast.
    void rejectsExcessiveNestingWithoutCrashing() {
        QByteArray in(300, 'l');
        in += QByteArray(300, 'e');
        bool ok = true;
        Bencode::decode(in, &ok);
        QVERIFY(!ok); // must not crash; must report failure
    }
};
QTEST_MAIN(TstBencode)
#include "tst_bencode.moc"
