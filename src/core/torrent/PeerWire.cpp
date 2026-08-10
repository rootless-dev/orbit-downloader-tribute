#include "torrent/PeerWire.h"
#include "torrent/Bencode.h"

namespace {

// Appends a 4-byte big-endian representation of n.
void appendBe32(QByteArray& out, quint32 n) {
    out.append(char((n >> 24) & 0xFF));
    out.append(char((n >> 16) & 0xFF));
    out.append(char((n >> 8) & 0xFF));
    out.append(char(n & 0xFF));
}

quint32 readBe32(const QByteArray& buf, int offset) {
    return (quint32(uint8_t(buf[offset])) << 24) | (quint32(uint8_t(buf[offset + 1])) << 16) |
           (quint32(uint8_t(buf[offset + 2])) << 8) | quint32(uint8_t(buf[offset + 3]));
}

} // namespace

namespace PeerWire {

QByteArray handshake(const QByteArray& infoHash, const QByteArray& peerId) {
    QByteArray out;
    out.append(char(19));
    out.append("BitTorrent protocol");
    QByteArray reserved(8, char(0));
    reserved[5] = char(uint8_t(reserved[5]) | 0x10); // extension protocol (BEP 10)
    reserved[7] = char(uint8_t(reserved[7]) | 0x01); // DHT (BEP 5)
    out.append(reserved);
    out.append(infoHash);
    out.append(peerId);
    return out;
}

int parseMessage(const QByteArray& buf, Msg* out) {
    if (buf.size() < 4) return 0;
    const quint32 length = readBe32(buf, 0);
    if (length == 0) {
        out->id = -1;
        out->payload.clear();
        return 4;
    }
    if (length > quint32(kMaxMessageLength)) return -1;
    const qint64 total = 4 + qint64(length);
    if (buf.size() < total) return 0;
    out->id = uint8_t(buf[4]);
    out->payload = buf.mid(5, int(length) - 1);
    return int(total);
}

QByteArray interested() {
    QByteArray out;
    appendBe32(out, 1);
    out.append(char(Interested));
    return out;
}

QByteArray request(int piece, qint64 begin, qint64 length) {
    QByteArray out;
    appendBe32(out, 13);
    out.append(char(Request));
    appendBe32(out, quint32(piece));
    appendBe32(out, quint32(begin));
    appendBe32(out, quint32(length));
    return out;
}

QByteArray extendedHandshakeMsg(int utMetadataId) {
    // d1:md11:ut_metadatai<N>eee — hand-built rather than routed through
    // Bencode::encode since the shape is fixed and tiny; avoids constructing
    // a BencodeValue dict just to immediately serialize it.
    const QByteArray idStr = QByteArray::number(utMetadataId);
    QByteArray payload = "d1:md11:ut_metadatai" + idStr + "eee";

    QByteArray out;
    appendBe32(out, quint32(2 + payload.size())); // ext-id byte + id byte + payload
    out.append(char(Extended));
    out.append(char(0)); // ext-id 0 == handshake
    out.append(payload);
    return out;
}

bool parseExtendedHandshake(const QByteArray& payload, int* utMetadataId, int* metadataSize) {
    *utMetadataId = 0;
    *metadataSize = 0;

    bool ok = false;
    const BencodeValue root = Bencode::decode(payload, &ok);
    if (!ok || root.type() != BencodeValue::Type::Dict) return false;

    if (root.contains("m")) {
        const BencodeValue& m = root["m"];
        if (m.type() == BencodeValue::Type::Dict && m.contains("ut_metadata")) {
            const BencodeValue& utm = m["ut_metadata"];
            if (utm.type() == BencodeValue::Type::Int) *utMetadataId = int(utm.toInt());
        }
    }
    if (root.contains("metadata_size")) {
        const BencodeValue& sz = root["metadata_size"];
        if (sz.type() == BencodeValue::Type::Int) *metadataSize = int(sz.toInt());
    }
    return true;
}

QByteArray utMetadataRequest(int extId, int piece) {
    // d8:msg_typei0e5:piecei<N>ee — hand-built like extendedHandshakeMsg;
    // the shape is fixed and tiny.
    const QByteArray pieceStr = QByteArray::number(piece);
    QByteArray payload = "d8:msg_typei0e5:piecei" + pieceStr + "ee";

    QByteArray out;
    appendBe32(out, quint32(2 + payload.size())); // ext-id byte + id byte + payload
    out.append(char(Extended));
    out.append(char(uint8_t(extId)));
    out.append(payload);
    return out;
}

} // namespace PeerWire
