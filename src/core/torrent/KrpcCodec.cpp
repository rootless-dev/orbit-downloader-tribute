#include "torrent/KrpcCodec.h"

// KRPC (BEP 5) wire codec, built on top of the pure Bencode codec.
//
// Top-level dict shape:
//   { "t": <bytes>, "y": "q"|"r"|"e",
//     "q": <bytes method name>, "a": {...}   (queries)
//     "r": {...}                              (responses)
//     "e": [ <int code>, <bytes msg> ]        (errors)
//   }
//
// decode() never throws and never crashes on malformed input: every field
// access is guarded by a type/presence check before use, and any failure
// short-circuits to a default-constructed KrpcMessage (kind == Invalid).

namespace KrpcCodec {

namespace {
const QByteArray kT = "t";
const QByteArray kY = "y";
const QByteArray kQ = "q";
const QByteArray kA = "a";
const QByteArray kR = "r";
const QByteArray kE = "e";
}

QByteArray encodeQuery(const QByteArray& tid, const QString& method,
                       const QMap<QByteArray, BencodeValue>& args) {
    QMap<QByteArray, BencodeValue> top;
    top[kT] = BencodeValue::makeBytes(tid);
    top[kY] = BencodeValue::makeBytes(kQ);
    top[kQ] = BencodeValue::makeBytes(method.toUtf8());
    top[kA] = BencodeValue::makeDict(args);
    return Bencode::encode(BencodeValue::makeDict(top));
}

QByteArray encodeResponse(const QByteArray& tid, const QMap<QByteArray, BencodeValue>& r) {
    QMap<QByteArray, BencodeValue> top;
    top[kT] = BencodeValue::makeBytes(tid);
    top[kY] = BencodeValue::makeBytes(kR);
    top[kR] = BencodeValue::makeDict(r);
    return Bencode::encode(BencodeValue::makeDict(top));
}

QByteArray encodeError(const QByteArray& tid, int code, const QString& msg) {
    QMap<QByteArray, BencodeValue> top;
    top[kT] = BencodeValue::makeBytes(tid);
    top[kY] = BencodeValue::makeBytes(kE);
    QList<BencodeValue> e;
    e.append(BencodeValue::makeInt(code));
    e.append(BencodeValue::makeBytes(msg.toUtf8()));
    top[kE] = BencodeValue::makeList(e);
    return Bencode::encode(BencodeValue::makeDict(top));
}

namespace {
KrpcMessage invalid() { return KrpcMessage(); }
}

KrpcMessage decode(const QByteArray& datagram) {
    bool ok = false;
    BencodeValue root = Bencode::decode(datagram, &ok, nullptr);
    if (!ok) return invalid();
    if (root.type() != BencodeValue::Type::Dict) return invalid();

    if (!root.contains(kT)) return invalid();
    const BencodeValue& tVal = root[kT];
    if (tVal.type() != BencodeValue::Type::Bytes) return invalid();

    if (!root.contains(kY)) return invalid();
    const BencodeValue& yVal = root[kY];
    if (yVal.type() != BencodeValue::Type::Bytes) return invalid();
    const QByteArray y = yVal.toBytes();

    KrpcMessage msg;
    msg.tid = tVal.toBytes();

    if (y == kQ) {
        if (!root.contains(kQ)) return invalid();
        const BencodeValue& qVal = root[kQ];
        if (qVal.type() != BencodeValue::Type::Bytes) return invalid();

        if (!root.contains(kA)) return invalid();
        const BencodeValue& aVal = root[kA];
        if (aVal.type() != BencodeValue::Type::Dict) return invalid();

        msg.kind = KrpcMessage::Query;
        msg.method = QString::fromUtf8(qVal.toBytes());
        msg.args = aVal.toDict();
        return msg;
    }

    if (y == kR) {
        if (!root.contains(kR)) return invalid();
        const BencodeValue& rVal = root[kR];
        if (rVal.type() != BencodeValue::Type::Dict) return invalid();

        msg.kind = KrpcMessage::Response;
        msg.args = rVal.toDict();
        return msg;
    }

    if (y == kE) {
        if (!root.contains(kE)) return invalid();
        const BencodeValue& eVal = root[kE];
        if (eVal.type() != BencodeValue::Type::List) return invalid();
        const QList<BencodeValue>& eList = eVal.toList();
        if (eList.size() != 2) return invalid();
        if (eList[0].type() != BencodeValue::Type::Int) return invalid();
        if (eList[1].type() != BencodeValue::Type::Bytes) return invalid();

        msg.kind = KrpcMessage::Error;
        msg.errorCode = static_cast<int>(eList[0].toInt());
        msg.errorMsg = QString::fromUtf8(eList[1].toBytes());
        return msg;
    }

    return invalid();
}

}
