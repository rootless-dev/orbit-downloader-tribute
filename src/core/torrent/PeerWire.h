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

    // BEP 6 (Fast Extension) message ids. We never negotiate the fast
    // extension (our handshake's reserved bytes are all zero), yet real
    // seeds still send have_all/have_none in place of a bitfield. We must
    // understand these two or we mistake a full seed for a peer that holds
    // nothing — the peer's bitfield stays empty and the picker starves. The
    // remaining fast-extension ids (suggest=13, reject=16, allowed_fast=17)
    // stay ignored: without negotiation a peer shouldn't send them, and we
    // serve nothing.
    HaveAll = 14,
    HaveNone = 15,

    // BEP 10 (Extension Protocol). All extension messages share id 20; a
    // second byte (the "extended message id") distinguishes them: 0 is
    // reserved for the handshake itself, other values are negotiated per
    // peer via the handshake's "m" dict (e.g. ut_metadata, Task 12).
    Extended = 20,
};

// Handshake length is fixed: 1 (pstrlen) + 19 (pstr) + 8 (reserved) + 20
// (info_hash) + 20 (peer_id).
constexpr int kHandshakeSize = 68;

// Guard against a peer claiming an absurd message length (e.g. a corrupt or
// hostile stream). 2 MiB comfortably covers the largest legitimate frame (a
// "piece" message: 9-byte header + a block, blocks are conventionally 16
// KiB) with headroom.
constexpr int kMaxMessageLength = 2 * 1024 * 1024;

// Builds our 68-byte handshake: <19>"BitTorrent protocol"<8 reserved
// bytes><infoHash><peerId>. infoHash and peerId must each be 20 bytes.
// Reserved bytes advertise our support for the extension protocol (BEP 10:
// byte[5] bit 0x10) and DHT (BEP 5: byte[7] bit 0x01) — the only two
// extensions we negotiate; every other bit stays zero.
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

// Our BEP 10 extended handshake (id=20, ext-id=0): a bencoded dict
// advertising the extensions we support and our locally-chosen id for
// each, e.g. d1:md11:ut_metadatai<utMetadataId>eee. Framed as
// <len:4be><20><0><bencoded payload>.
QByteArray extendedHandshakeMsg(int utMetadataId);

// Parses a peer's BEP 10 extended handshake payload (the bencoded dict
// that follows the ext-id byte 0 — NOT including the outer message
// length/id/ext-id). Reads m.ut_metadata (the peer's chosen id for the
// ut_metadata extension) and the top-level metadata_size. Malformed input
// or missing keys yield 0 for the corresponding out-param rather than
// failing — a peer that doesn't support an extension simply omits it.
// Returns false only on payloads that don't even bdecode to a dict (out
// params are still set to 0 in that case).
bool parseExtendedHandshake(const QByteArray& payload, int* utMetadataId, int* metadataSize);

// A BEP 9 ut_metadata "request" message (msg_type 0): asks for piece number
// `piece` of the info dict. `extId` is the PEER's chosen id for the
// ut_metadata extension (learned from its extended handshake) — every
// ut_metadata message is addressed to whichever id its recipient announced.
// Framed as <len:4be><20><extId><bencoded {msg_type:0,piece:N}>>.
QByteArray utMetadataRequest(int extId, int piece);

} // namespace PeerWire
