#include "torrent/PeerConnection.h"
#include "torrent/PeerWire.h"
#include "torrent/Bencode.h"
#include "RateLimiter.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QTcpSocket>
#include <QTimer>

namespace {

quint32 readBe32(const QByteArray& b, int offset) {
    return (quint32(uint8_t(b[offset])) << 24) | (quint32(uint8_t(b[offset + 1])) << 16) |
           (quint32(uint8_t(b[offset + 2])) << 8) | quint32(uint8_t(b[offset + 3]));
}

// Same rationale as Bencode.cpp's kMaxNestingDepth: bounds this scanner's
// own l/d recursion against a maliciously deep header (e.g. sent as a
// ut_metadata "data" message) so it fails cleanly instead of overflowing
// the native stack.
constexpr int kMaxNestingDepth = 200;

// BEP 9 "data" messages are a bencoded header dict immediately followed by
// the raw metadata block, with no separator — Bencode::decode can't locate
// that boundary itself (it rejects any trailing bytes after the value as
// malformed). This walks one bencode value from buf[pos] and returns the
// number of bytes it occupies (i.e. where the value ends, equivalent to
// what BencodeValue::rawEnd() would report were decode() willing to leave
// trailing bytes unconsumed), or -1 if truncated/malformed. Deliberately
// minimal — it only needs to skip the header, not validate its contents.
int bencodeValueLength(const QByteArray& buf, int pos, int depth = 0) {
    if (pos < 0 || pos >= buf.size()) return -1;
    const char c = buf[pos];
    if (c == 'i') {
        int p = pos + 1;
        while (p < buf.size() && buf[p] != 'e') ++p;
        if (p >= buf.size()) return -1;
        return p - pos + 1;
    }
    if (c == 'l' || c == 'd') {
        if (depth + 1 > kMaxNestingDepth) return -1;
        int p = pos + 1;
        while (true) {
            if (p >= buf.size()) return -1;
            if (buf[p] == 'e') { ++p; break; }
            if (c == 'd') { // dict key: always a byte string
                const int keyLen = bencodeValueLength(buf, p, depth + 1);
                if (keyLen < 0) return -1;
                p += keyLen;
            }
            const int itemLen = bencodeValueLength(buf, p, depth + 1);
            if (itemLen < 0) return -1;
            p += itemLen;
        }
        return p - pos;
    }
    if (c >= '0' && c <= '9') {
        int p = pos;
        while (p < buf.size() && buf[p] >= '0' && buf[p] <= '9') ++p;
        if (p >= buf.size() || buf[p] != ':') return -1;
        bool ok = false;
        const qint64 len = buf.mid(pos, p - pos).toLongLong(&ok);
        if (!ok || len < 0) return -1;
        ++p; // skip ':'
        if (qint64(p) + len > buf.size()) return -1;
        return int(p + len - pos);
    }
    return -1;
}

constexpr int kMetadataBlockSize = 16384;

// Sane upper bound on a peer-declared metadata_size. No real torrent's info
// dict comes anywhere close to this; without a cap, a malicious peer could
// advertise a multi-GB size and force an equally large allocation (and a
// huge request loop) before a single byte is verified.
constexpr int kMaxMetadataSize = 8 * 1024 * 1024; // 8 MiB

} // namespace

PeerConnection::PeerConnection(const PeerAddress& addr, const QByteArray& infoHash,
                               const QByteArray& peerId, int pieceCount, RateLimiter* limiter,
                               QObject* parent)
    : QObject(parent), m_limiter(limiter), m_addr(addr), m_infoHash(infoHash), m_peerId(peerId),
      m_pieceCount(pieceCount), m_peerBitfield(pieceCount) {}

PeerConnection::PeerConnection(const PeerAddress& addr, const QByteArray& infoHash,
                               const QByteArray& peerId, RateLimiter* limiter, QObject* parent)
    : QObject(parent), m_limiter(limiter), m_addr(addr), m_infoHash(infoHash), m_peerId(peerId),
      m_pieceCount(0), m_metadataMode(true), m_peerBitfield(0) {}

void PeerConnection::connectToPeer() {
    m_sock = new QTcpSocket(this);
    if (m_limiter && m_limiter->rate() > 0)
        m_sock->setReadBufferSize(qMax<qint64>(m_limiter->rate(), 64 * 1024)); // ~1s buffer, backpressure via TCP
    connect(m_sock, &QTcpSocket::connected,     this, &PeerConnection::onConnected);
    connect(m_sock, &QTcpSocket::readyRead,     this, &PeerConnection::onReadyRead);
    connect(m_sock, &QTcpSocket::errorOccurred, this, &PeerConnection::onSocketError);
    connect(m_sock, &QTcpSocket::disconnected,  this, &PeerConnection::onSocketDisconnected);
    m_sock->connectToHost(m_addr.host, m_addr.port);
}

namespace { constexpr int kOurUtMetadataId = 1; }

void PeerConnection::onConnected() {
    if (m_dead || !m_sock) return;
    m_sock->write(PeerWire::handshake(m_infoHash, m_peerId));
    // BEP 10: our reserved bytes already advertise extension-protocol
    // support, so send our extended handshake right away rather than
    // waiting to see the peer's — real clients tolerate receiving one from
    // a peer that turns out not to support extensions (they just ignore it).
    m_sock->write(PeerWire::extendedHandshakeMsg(kOurUtMetadataId));
}

void PeerConnection::sendInterested() {
    if (m_dead || !m_sock) return;
    m_sock->write(PeerWire::interested());
}

void PeerConnection::sendRequest(int piece, qint64 begin, qint64 length) {
    if (m_dead || !m_sock) return;
    m_sock->write(PeerWire::request(piece, begin, length));
}

const Bitfield& PeerConnection::peerBitfield() const { return m_peerBitfield; }

bool PeerConnection::amUnchoked() const { return m_unchoked; }

void PeerConnection::onReadyRead() {
    if (m_dead || !m_sock) return;
    const qint64 avail = m_sock->bytesAvailable();
    if (avail <= 0) return;

    QByteArray chunk;
    if (m_limiter) {
        const qint64 grant = m_limiter->take(avail, QDateTime::currentMSecsSinceEpoch());
        if (grant <= 0) { scheduleDrain(); return; } // sem tokens: tentar de novo em breve
        chunk = m_sock->read(grant);
    } else {
        chunk = m_sock->readAll();
    }
    if (chunk.isEmpty()) return;

    m_buf += chunk;
    processBuffer();
    if (m_dead) return; // processBuffer may have torn down the socket (bad handshake/frame)

    if (m_sock->bytesAvailable() > 0) scheduleDrain(); // restou dado sob throttle
}

void PeerConnection::scheduleDrain() {
    if (m_dead) return;
    if (!m_drainTimer) {
        m_drainTimer = new QTimer(this);
        m_drainTimer->setSingleShot(true);
        connect(m_drainTimer, &QTimer::timeout, this, &PeerConnection::onReadyRead);
    }
    if (!m_drainTimer->isActive()) m_drainTimer->start(20); // ~20ms até repor tokens
}

void PeerConnection::processBuffer() {
    if (!m_handshakeDone) {
        if (m_buf.size() < PeerWire::kHandshakeSize) return; // wait for the rest
        const QByteArray theirHandshake = m_buf.left(PeerWire::kHandshakeSize);
        m_buf.remove(0, PeerWire::kHandshakeSize);
        const QByteArray theirInfoHash = theirHandshake.mid(28, 20);
        if (theirInfoHash != m_infoHash) {
            failAndDisconnect("handshake info_hash mismatch");
            return;
        }
        m_handshakeDone = true;
        // Reserved bytes (BEP 3 offset 20..27) advertise peer extensions:
        // last byte bit 0x04 = fast extension (BEP 6), 0x01 = DHT; byte[5]
        // bit 0x10 = extension protocol (BEP 10). We negotiate none, but
        // logging them explains why a peer sends have_all/extended anyway.
        emit peerLog(QStringLiteral("reserved=%1").arg(QString::fromLatin1(
            theirHandshake.mid(20, 8).toHex())));
        emit handshakeOk();
    }

    while (true) {
        PeerWire::Msg msg;
        const int n = PeerWire::parseMessage(m_buf, &msg);
        if (n == 0) break;      // need more bytes
        if (n < 0) {            // oversized/malformed frame: unrecoverable
            failAndDisconnect("peer sent an oversized message frame");
            return;
        }
        m_buf.remove(0, n);
        handleMessage(msg.id, msg.payload);
        if (m_dead) return;
    }
}

void PeerConnection::handleMessage(int id, const QByteArray& payload) {
    switch (id) {
        case -1: // keep-alive
            return;

        case PeerWire::Choke:
            m_unchoked = false;
            emit choked();
            return;

        case PeerWire::Unchoke:
            m_unchoked = true;
            emit unchoked();
            return;

        case PeerWire::Have: {
            if (payload.size() < 4) return; // malformed; ignore rather than crash
            const int piece = int(readBe32(payload, 0));
            m_peerBitfield.set(piece);
            emit haveReceived(piece);
            return;
        }

        case PeerWire::Bitfield:
            m_peerBitfield = Bitfield::fromBytes(payload, m_pieceCount);
            emit peerLog(QStringLiteral("recv bitfield (%1 bytes)").arg(payload.size()));
            emit bitfieldReceived();
            return;

        case PeerWire::HaveAll: {
            // BEP 6: peer holds every piece (a seed). Treat exactly like a
            // full bitfield so the picker sees it as a complete source.
            Bitfield full(m_pieceCount);
            for (int i = 0; i < m_pieceCount; ++i) full.set(i);
            m_peerBitfield = full;
            emit peerLog(QStringLiteral("recv have_all"));
            emit bitfieldReceived();
            return;
        }

        case PeerWire::HaveNone:
            // BEP 6: peer holds nothing (yet). Empty bitfield; still emit so
            // downstream treats it like any other advertised availability.
            m_peerBitfield = Bitfield(m_pieceCount);
            emit peerLog(QStringLiteral("recv have_none"));
            emit bitfieldReceived();
            return;

        case PeerWire::Piece: {
            if (payload.size() < 8) return; // malformed; ignore rather than crash
            const int index = int(readBe32(payload, 0));
            const qint64 begin = qint64(readBe32(payload, 4));
            emit blockReceived(index, begin, payload.mid(8));
            return;
        }

        case PeerWire::Extended: {
            if (payload.isEmpty()) return; // malformed; ignore rather than crash
            const int extId = uint8_t(payload[0]);
            const QByteArray extPayload = payload.mid(1);
            if (extId == 0) { // extended handshake
                int utMetadataId = 0, metadataSize = 0;
                PeerWire::parseExtendedHandshake(extPayload, &utMetadataId, &metadataSize);
                emit peerLog(QStringLiteral("recv extended handshake (ut_metadata=%1, metadata_size=%2)")
                                 .arg(utMetadataId).arg(metadataSize));
                emit extendedHandshake(utMetadataId, metadataSize);
                if (m_metadataMode) startMetadataFetch(utMetadataId, metadataSize);
            } else if (m_metadataMode && extId == kOurUtMetadataId) {
                handleUtMetadataMessage(extPayload);
            } else {
                // ut_metadata data/request/reject etc. — Task 12's job.
                if (!m_loggedUnhandled.contains(id)) {
                    m_loggedUnhandled.insert(id);
                    emit peerLog(QStringLiteral("recv unhandled extended message ext-id %1").arg(extId));
                }
            }
            return;
        }

        default:
            // interested/not_interested/request/cancel and any choke-state
            // request FROM the peer: we're download-only and serve nothing,
            // so these are simply ignored. Surface each unhandled id once so a
            // wire trace shows what a real peer sends (e.g. extension protocol
            // id 20, or fast-extension suggest/reject/allowed_fast).
            if (!m_loggedUnhandled.contains(id)) {
                m_loggedUnhandled.insert(id);
                emit peerLog(QStringLiteral("recv unhandled message id %1").arg(id));
            }
            return;
    }
}

void PeerConnection::onSocketError() {
    if (m_dead || !m_sock) return;
    failAndDisconnect(m_sock->errorString());
}

void PeerConnection::onSocketDisconnected() {
    if (m_dead) return;
    failAndDisconnect("connection closed by peer");
}

void PeerConnection::failAndDisconnect(const QString& reason) {
    if (m_dead) return;
    m_dead = true;
    if (m_drainTimer) m_drainTimer->stop();
    if (m_sock) {
        m_sock->disconnect(this);
        m_sock->abort();
    }
    emit disconnected(reason);
}

void PeerConnection::startMetadataFetch(int peerUtMetadataId, int metadataSize) {
    if (m_metadataRequested || m_metadataDone) return; // don't re-request on a stray 2nd handshake
    if (peerUtMetadataId <= 0 || metadataSize <= 0) {
        // Peer doesn't support ut_metadata, or doesn't have the full
        // metadata itself yet — nothing to request, this fetch can't succeed.
        failMetadata();
        return;
    }
    if (metadataSize > kMaxMetadataSize) {
        // Peer is advertising an implausibly large metadata size (no real
        // torrent's info dict is anywhere close to this). Refuse before
        // allocating the buffer or issuing any requests.
        failMetadata();
        return;
    }
    m_metadataRequested = true;
    m_peerUtMetadataId = peerUtMetadataId;
    m_metadataSize = metadataSize;
    m_metadataBuf = QByteArray(metadataSize, '\0');
    m_metadataPieceCount = (metadataSize + kMetadataBlockSize - 1) / kMetadataBlockSize;
    m_metadataPiecesReceived.clear();
    for (int piece = 0; piece < m_metadataPieceCount; ++piece) {
        if (m_dead || !m_sock) return;
        m_sock->write(PeerWire::utMetadataRequest(m_peerUtMetadataId, piece));
    }
}

void PeerConnection::handleUtMetadataMessage(const QByteArray& extPayload) {
    if (m_metadataDone) return; // already resolved; ignore anything further

    const int dictLen = bencodeValueLength(extPayload, 0);
    if (dictLen < 0) { failMetadata(); return; } // truncated/malformed header

    bool ok = false;
    const BencodeValue dict = Bencode::decode(extPayload.left(dictLen), &ok);
    if (!ok || dict.type() != BencodeValue::Type::Dict || !dict.contains("msg_type") ||
        dict["msg_type"].type() != BencodeValue::Type::Int) {
        failMetadata();
        return;
    }

    const int msgType = int(dict["msg_type"].toInt());
    if (msgType == 2) { // reject
        failMetadata();
        return;
    }
    if (msgType != 1) return; // not a "data" message; nothing else expected from a peer here

    if (!dict.contains("piece") || dict["piece"].type() != BencodeValue::Type::Int) {
        failMetadata();
        return;
    }
    const int piece = int(dict["piece"].toInt());
    const QByteArray block = extPayload.mid(dictLen);
    const qint64 offset = qint64(piece) * kMetadataBlockSize;

    if (piece < 0 || piece >= m_metadataPieceCount || block.isEmpty() ||
        offset + block.size() > m_metadataBuf.size()) {
        failMetadata(); // out-of-range/empty piece: can't trust this peer's metadata
        return;
    }
    if (m_metadataPiecesReceived.contains(piece)) return; // duplicate; already have it

    m_metadataBuf.replace(int(offset), block.size(), block);
    m_metadataPiecesReceived.insert(piece);
    if (m_metadataPiecesReceived.size() == m_metadataPieceCount) finishMetadata();
}

void PeerConnection::finishMetadata() {
    if (m_metadataDone) return;
    const QByteArray hash = QCryptographicHash::hash(m_metadataBuf, QCryptographicHash::Sha1);
    if (hash != m_infoHash) {
        failMetadata();
        return;
    }
    m_metadataDone = true;
    emit metadataComplete(m_metadataBuf);
}

void PeerConnection::failMetadata() {
    if (m_metadataDone) return;
    m_metadataDone = true;
    emit metadataFailed();
}
