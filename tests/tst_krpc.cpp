#include <QtTest>
#include "torrent/KrpcCodec.h"

class TstKrpc : public QObject {
    Q_OBJECT
private slots:
    void roundTripsPingQuery() {
        QMap<QByteArray, BencodeValue> a; a["id"] = BencodeValue::makeBytes(QByteArray(20, '\x01'));
        QByteArray dg = KrpcCodec::encodeQuery("aa", "ping", a);
        KrpcMessage m = KrpcCodec::decode(dg);
        QCOMPARE(m.kind, KrpcMessage::Query);
        QCOMPARE(m.tid, QByteArray("aa"));
        QCOMPARE(m.method, QString("ping"));
        QCOMPARE(m.args["id"].toBytes(), QByteArray(20, '\x01'));
    }
    void roundTripsResponseAndError() {
        QMap<QByteArray, BencodeValue> r; r["id"] = BencodeValue::makeBytes(QByteArray(20, '\x02'));
        QByteArray rdg = KrpcCodec::encodeResponse("bb", r);
        KrpcMessage rm = KrpcCodec::decode(rdg);
        QCOMPARE(rm.kind, KrpcMessage::Response);
        QCOMPARE(rm.tid, QByteArray("bb"));
        QCOMPARE(rm.args["id"].toBytes(), QByteArray(20, '\x02'));

        KrpcMessage e = KrpcCodec::decode(KrpcCodec::encodeError("cc", 201, "Generic"));
        QCOMPARE(e.kind, KrpcMessage::Error);
        QCOMPARE(e.tid, QByteArray("cc"));
        QCOMPARE(e.errorCode, 201);
        QCOMPARE(e.errorMsg, QString("Generic"));
    }
    void garbageIsInvalidNeverCrashes() {
        // Genuinely malformed/unparseable byte strings: these fail before any
        // "t"/"y" presence or per-"y" branch check is ever reached -- either
        // the top-level bencode decode itself fails (ok=0), or the decoded
        // value's top-level type isn't a dict.
        for (const QByteArray& bad : { QByteArray(),                       // empty
                                       QByteArray("x"),                     // not bencode at all
                                       QByteArray("d"),                     // truncated dict opener
                                       QByteArray("d1:td2:te"),             // truncated/unparseable bencode (ok=0)
                                       QByteArray("i42e"),                  // top-level not a dict
                                       QByteArray("le"),                    // top-level list, not dict
                                       QByteArray(2000, '\xFF') })          // oversized garbage bytes
            QCOMPARE(KrpcCodec::decode(bad).kind, KrpcMessage::Invalid);

        // From here on, every vector is a well-formed bencode dict built
        // programmatically (same technique as the valid round-trip tests):
        // BencodeValue::makeDict + Bencode::encode, with a valid "t" (bytes)
        // so the input parses (ok=1) and clears the top-level guards. Each
        // vector omits or mistypes ONLY the field under test, forcing
        // decode() down the exact guard/branch being exercised.
        auto encode = [](const QMap<QByteArray, BencodeValue>& top) {
            return Bencode::encode(BencodeValue::makeDict(top));
        };
        const BencodeValue validT = BencodeValue::makeBytes("cc");

        // "t" present but wrong type (Int, not Bytes) -> bails at the "t" type guard.
        {
            QMap<QByteArray, BencodeValue> top;
            top[QByteArray("t")] = BencodeValue::makeInt(1);
            top[QByteArray("y")] = BencodeValue::makeBytes("q");
            QCOMPARE(KrpcCodec::decode(encode(top)).kind, KrpcMessage::Invalid);
        }

        // "y" present but wrong type (Int, not Bytes) -> bails at the "y" type guard.
        {
            QMap<QByteArray, BencodeValue> top;
            top[QByteArray("t")] = validT;
            top[QByteArray("y")] = BencodeValue::makeInt(1);
            QCOMPARE(KrpcCodec::decode(encode(top)).kind, KrpcMessage::Invalid);
        }

        // Unrecognized "y" value with valid "t" -> falls through to the final invalid().
        {
            QMap<QByteArray, BencodeValue> top;
            top[QByteArray("t")] = validT;
            top[QByteArray("y")] = BencodeValue::makeBytes("x");
            QCOMPARE(KrpcCodec::decode(encode(top)).kind, KrpcMessage::Invalid);
        }

        // y=="q", "t" present, but "q" missing -> bails at the query "q" presence guard.
        {
            QMap<QByteArray, BencodeValue> top;
            top[QByteArray("t")] = validT;
            top[QByteArray("y")] = BencodeValue::makeBytes("q");
            QCOMPARE(KrpcCodec::decode(encode(top)).kind, KrpcMessage::Invalid);
        }

        // y=="q", "t"+"q" present, but "a" missing -> bails at the query "a" presence guard.
        {
            QMap<QByteArray, BencodeValue> top;
            top[QByteArray("t")] = validT;
            top[QByteArray("y")] = BencodeValue::makeBytes("q");
            top[QByteArray("q")] = BencodeValue::makeBytes("ping");
            QCOMPARE(KrpcCodec::decode(encode(top)).kind, KrpcMessage::Invalid);
        }

        // y=="r", "t" present, but "r" missing -> bails at the response "r" presence guard.
        {
            QMap<QByteArray, BencodeValue> top;
            top[QByteArray("t")] = validT;
            top[QByteArray("y")] = BencodeValue::makeBytes("r");
            QCOMPARE(KrpcCodec::decode(encode(top)).kind, KrpcMessage::Invalid);
        }

        // y=="e", "t" present, but "e" missing -> bails at the error "e" presence guard.
        {
            QMap<QByteArray, BencodeValue> top;
            top[QByteArray("t")] = validT;
            top[QByteArray("y")] = BencodeValue::makeBytes("e");
            QCOMPARE(KrpcCodec::decode(encode(top)).kind, KrpcMessage::Invalid);
        }

        // "e" list with wrong arity/types, embedded in an otherwise well-formed
        // envelope (t + y=e present) so these genuinely exercise the "e" field
        // validation rather than being short-circuited by an earlier missing-key check.
        {
            QMap<QByteArray, BencodeValue> topArity;
            topArity[QByteArray("t")] = validT;
            topArity[QByteArray("y")] = BencodeValue::makeBytes("e");
            topArity[QByteArray("e")] = BencodeValue::makeList({ BencodeValue::makeInt(200) }); // only 1 element
            QCOMPARE(KrpcCodec::decode(encode(topArity)).kind, KrpcMessage::Invalid);

            QMap<QByteArray, BencodeValue> topTypes;
            topTypes[QByteArray("t")] = validT;
            topTypes[QByteArray("y")] = BencodeValue::makeBytes("e");
            topTypes[QByteArray("e")] = BencodeValue::makeList(
                { BencodeValue::makeBytes("xx"), BencodeValue::makeBytes("yy") }); // code should be Int, not Bytes
            QCOMPARE(KrpcCodec::decode(encode(topTypes)).kind, KrpcMessage::Invalid);
        }
    }
};
QTEST_APPLESS_MAIN(TstKrpc)
#include "tst_krpc.moc"
