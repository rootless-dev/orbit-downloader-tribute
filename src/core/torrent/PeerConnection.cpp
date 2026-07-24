#include "torrent/PeerConnection.h"
#include "torrent/PeerWire.h"
#include "RateLimiter.h"

#include <QDateTime>
#include <QTcpSocket>
#include <QTimer>

namespace {

quint32 readBe32(const QByteArray& b, int offset) {
    return (quint32(uint8_t(b[offset])) << 24) | (quint32(uint8_t(b[offset + 1])) << 16) |
           (quint32(uint8_t(b[offset + 2])) << 8) | quint32(uint8_t(b[offset + 3]));
}

} // namespace

PeerConnection::PeerConnection(const PeerAddress& addr, const QByteArray& infoHash,
                               const QByteArray& peerId, int pieceCount, RateLimiter* limiter,
                               QObject* parent)
    : QObject(parent), m_limiter(limiter), m_addr(addr), m_infoHash(infoHash), m_peerId(peerId),
      m_pieceCount(pieceCount), m_peerBitfield(pieceCount) {}

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

void PeerConnection::onConnected() {
    if (m_dead || !m_sock) return;
    m_sock->write(PeerWire::handshake(m_infoHash, m_peerId));
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
            emit bitfieldReceived();
            return;

        case PeerWire::Piece: {
            if (payload.size() < 8) return; // malformed; ignore rather than crash
            const int index = int(readBe32(payload, 0));
            const qint64 begin = qint64(readBe32(payload, 4));
            emit blockReceived(index, begin, payload.mid(8));
            return;
        }

        default:
            // interested/not_interested/request/cancel and any choke-state
            // request FROM the peer: we're download-only and serve nothing,
            // so these are simply ignored.
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
