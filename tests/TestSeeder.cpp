#include "TestSeeder.h"
#include "torrent/Bitfield.h"
#include "torrent/PeerWire.h"

#include <QHostAddress>
#include <QTcpSocket>
#include <QTimer>

namespace {

// Appends a 4-byte big-endian representation of n (mirrors PeerWire's
// internal helper — PeerWire only exposes client/download-side builders, so
// the seeder-side framing lives here, test-only).
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

// Frames one peer-wire message: <len:4be><id><payload>, len = 1 + payload.size().
QByteArray frame(int id, const QByteArray& payload = QByteArray()) {
    QByteArray out;
    appendBe32(out, quint32(1 + payload.size()));
    out.append(char(id));
    out.append(payload);
    return out;
}

QByteArray seederPeerId() {
    return QByteArray("-TS0001-seedseedseed").left(20);
}

} // namespace

struct TestSeeder::Session {
    QTcpSocket* sock = nullptr;
    QByteArray  buf;
    bool        handshakeDone = false;
};

TestSeeder::TestSeeder(const QByteArray& infoHash, const QByteArray& data, qint64 pieceLength,
                       QObject* parent)
    : TestSeeder(infoHash, data, pieceLength, Advertise::NormalBitfield, parent) {}

TestSeeder::TestSeeder(const QByteArray& infoHash, const QByteArray& data, qint64 pieceLength,
                       Advertise advertise, QObject* parent)
    : QObject(parent),
      m_infoHash(infoHash),
      m_data(data),
      m_pieceLength(pieceLength),
      m_pieceCount(pieceLength > 0 ? int((data.size() + pieceLength - 1) / pieceLength) : 0),
      m_advertise(advertise) {
    connect(&m_server, &QTcpServer::newConnection, this, &TestSeeder::onNewConnection);
    m_server.listen(QHostAddress::LocalHost, 0);
}

void TestSeeder::onNewConnection() {
    while (QTcpSocket* c = m_server.nextPendingConnection()) {
        auto* s = new Session;
        s->sock = c;

        connect(c, &QTcpSocket::readyRead, this, [this, s] {
            s->buf += s->sock->readAll();

            if (!s->handshakeDone) {
                if (s->buf.size() < PeerWire::kHandshakeSize) return; // wait for more
                s->handshakeDone = true;
                s->buf.remove(0, PeerWire::kHandshakeSize);

                // Echo our own handshake, then advertise the pieces we hold
                // (full set — we are a seeder) and unchoke unconditionally.
                s->sock->write(PeerWire::handshake(m_infoHash, seederPeerId()));
                Bitfield bf(m_pieceCount);
                for (int i = 0; i < m_pieceCount; ++i) bf.set(i);

                if (m_advertise == Advertise::HaveAll) {
                    // BEP 6 <have_all> (id 14), no bitfield frame.
                    s->sock->write(frame(14));
                    s->sock->write(frame(PeerWire::Unchoke));
                    s->sock->flush();
                } else if (m_advertise == Advertise::FragmentedBitfield) {
                    // Split the bitfield frame across two readyRead deliveries,
                    // as a >MTU bitfield does on a real network. Write the first
                    // half now; write the remainder + unchoke after one turn of
                    // the event loop so the client sees a partial frame first.
                    const QByteArray bfFrame = frame(PeerWire::Bitfield, bf.toBytes());
                    const int half = bfFrame.size() / 2;
                    s->sock->write(bfFrame.left(half));
                    s->sock->flush();
                    QByteArray rest = bfFrame.mid(half) + frame(PeerWire::Unchoke);
                    QTcpSocket* sock = s->sock;
                    QTimer::singleShot(5, this, [sock, rest] {
                        sock->write(rest);
                        sock->flush();
                    });
                } else {
                    s->sock->write(frame(PeerWire::Bitfield, bf.toBytes()));
                    s->sock->write(frame(PeerWire::Unchoke));
                    s->sock->flush();
                }
            }

            PeerWire::Msg msg;
            int consumed;
            while ((consumed = PeerWire::parseMessage(s->buf, &msg)) != 0) {
                if (consumed < 0) { // protocol error: fatal, unrecoverable stream
                    s->sock->disconnectFromHost();
                    return;
                }
                s->buf.remove(0, consumed);

                if (msg.id == PeerWire::Request && msg.payload.size() >= 12) {
                    const int    index  = int(readBe32(msg.payload, 0));
                    const qint64 begin  = qint64(readBe32(msg.payload, 4));
                    const qint64 length = qint64(readBe32(msg.payload, 8));
                    const QByteArray block =
                        m_data.mid(qint64(index) * m_pieceLength + begin, length);

                    QByteArray payload;
                    appendBe32(payload, quint32(index));
                    appendBe32(payload, quint32(begin));
                    payload.append(block);
                    s->sock->write(frame(PeerWire::Piece, payload));
                }
                // keep-alives (id == -1) and any other message ids are ignored.
            }
            s->sock->flush();
        });

        connect(c, &QTcpSocket::disconnected, this, [this, s] {
            // Sever readyRead first: the protocol-error branch above calls
            // disconnectFromHost(), which can emit `disconnected`
            // synchronously/re-entrantly while that readyRead lambda is
            // still on the stack. Deleting `s` immediately would free it
            // out from under that frame (see the identical note in
            // TestFtpServer). Defer the actual delete to the next turn of
            // the event loop.
            s->sock->disconnect(this);
            s->sock->deleteLater();
            QTimer::singleShot(0, this, [s] { delete s; });
        });
    }
}
