#include "torrent/PeerWire.h"

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
    out.append(QByteArray(8, char(0))); // reserved
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

} // namespace PeerWire
