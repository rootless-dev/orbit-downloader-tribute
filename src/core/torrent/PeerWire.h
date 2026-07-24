#pragma once

#include <QByteArray>
#include <cstdint>

// Pure BitTorrent peer wire protocol (BEP 3) framing: builds and parses the
// byte layouts exchanged with a peer. No I/O, no QObject, no sockets — kept
// free of side effects so it can be unit-tested in isolation from
// PeerConnection's QTcpSocket plumbing.
namespace PeerWire {

// Standard message ids (BEP 3 "peer messages"). request/piece/cancel share
// a common index/begin(/length) payload shape.
enum MessageId {
    Choke = 0,
    Unchoke = 1,
    Interested = 2,
    NotInterested = 3,
    Have = 4,
    Bitfield = 5,
    Request = 6,
    Piece = 7,
    Cancel = 8,
};

// Handshake length is fixed: 1 (pstrlen) + 19 (pstr) + 8 (reserved) + 20
// (info_hash) + 20 (peer_id).
constexpr int kHandshakeSize = 68;

// Guard against a peer claiming an absurd message length (e.g. a corrupt or
// hostile stream). 2 MiB comfortably covers the largest legitimate frame (a
// "piece" message: 9-byte header + a block, blocks are conventionally 16
// KiB) with headroom.
constexpr int kMaxMessageLength = 2 * 1024 * 1024;

// Builds our 68-byte handshake: <19>"BitTorrent protocol"<8 zero
// bytes><infoHash><peerId>. infoHash and peerId must each be 20 bytes.
QByteArray handshake(const QByteArray& infoHash, const QByteArray& peerId);

// One parsed peer-wire message. id == -1 marks a keep-alive (zero-length
// frame with no id byte); payload is empty for keep-alive, have, choke,
// unchoke, interested, not_interested, and holds everything after the id
// byte otherwise.
struct Msg {
    int id = -1;
    QByteArray payload;
};

// Parses one length-prefixed message (4-byte big-endian length, then, if
// length>0, a 1-byte id followed by payload) from the front of `buf`.
// Returns the number of bytes consumed (== 4 for a keep-alive, == 4+length
// otherwise) and fills *out. Returns 0 if buf doesn't yet hold a complete
// frame (fewer than 4 bytes, or fewer than 4+length bytes) — the caller
// should wait for more data and retry. Returns a negative value if the
// declared length exceeds kMaxMessageLength (protocol error): the caller
// should treat this as a fatal, unrecoverable stream and disconnect rather
// than wait for more bytes that would never complete a sane frame.
int parseMessage(const QByteArray& buf, Msg* out);

// interested (id=2, no payload): <0001><2>
QByteArray interested();

// request (id=6): <0013><6><piece:4be><begin:4be><length:4be> — 17 bytes.
QByteArray request(int piece, qint64 begin, qint64 length);

} // namespace PeerWire
