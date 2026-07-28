#include "TestMetadataPeer.h"
#include "torrent/PeerWire.h"
#include "torrent/Bencode.h"

#include <QHostAddress>
#include <QTcpSocket>
#include <QTimer>

namespace {

constexpr int kBlockSize = 16384;
// This peer's own chosen id for the ut_metadata extension, advertised in its
// extended handshake's "m" dict — deliberately different from
// PeerConnection's (1) to prove the code addresses messages by the id each
// side actually announced, not by a hardcoded constant.
constexpr int kServerUtMetadataId = 2;

void appendBe32(QByteArray& out, quint32 n) {
    out.append(char((n >> 24) & 0xFF));
    out.append(char((n >> 16) & 0xFF));
    out.append(char((n >> 8) & 0xFF));
    out.append(char(n & 0xFF));
}

QByteArray frame(int id, const QByteArray& payload = QByteArray()) {
    QByteArray out;
    appendBe32(out, quint32(1 + payload.size()));
    out.append(char(id));
    out.append(payload);
    return out;
}

QByteArray metadataPeerId() {
    return QByteArray("-TM0001-metametameta").left(20);
}

// Our extended handshake: advertises ut_metadata's id AND metadata_size —
// unlike PeerWire::extendedHandshakeMsg (the download-only client side,
// which never knows the size upfront), a metadata-serving peer must
// advertise both.
QByteArray extendedHandshakeWithMetadata(int utMetadataId, int metadataSize) {
    QMap<QByteArray, BencodeValue> m;
    m.insert("ut_metadata", BencodeValue::makeInt(utMetadataId));
    QMap<QByteArray, BencodeValue> root;
    root.insert("m", BencodeValue::makeDict(m));
    root.insert("metadata_size", BencodeValue::makeInt(metadataSize));
    const QByteArray payload = Bencode::encode(BencodeValue::makeDict(root));
    return frame(PeerWire::Extended, QByteArray(1, char(0)) + payload);
}

// A ut_metadata "data" message (msg_type 1): bencoded header dict followed
// immediately by the raw block bytes — no separator, the receiver locates
// the boundary by finding where the bencoded dict ends.
QByteArray utMetadataData(int addressedExtId, int piece, int totalSize, const QByteArray& block) {
    QMap<QByteArray, BencodeValue> d;
    d.insert("msg_type", BencodeValue::makeInt(1));
    d.insert("piece", BencodeValue::makeInt(piece));
    d.insert("total_size", BencodeValue::makeInt(totalSize));
    const QByteArray header = Bencode::encode(BencodeValue::makeDict(d));
    return frame(PeerWire::Extended, QByteArray(1, char(uint8_t(addressedExtId))) + header + block);
}

} // namespace

struct TestMetadataPeer::Session {
    QTcpSocket* sock = nullptr;
    QByteArray  buf;
    bool        handshakeDone = false;
    // The client's own chosen ut_metadata id, learned from ITS extended
    // handshake (ext-id 0) — reply "data" messages must be addressed to
    // this id, not to our own.
    int         clientUtMetadataId = 0;
};

TestMetadataPeer::TestMetadataPeer(const QByteArray& infoHash, const QByteArray& infoDict,
                                   QObject* parent, int advertisedMetadataSize)
    : QObject(parent), m_infoHash(infoHash), m_infoDict(infoDict),
      m_advertisedMetadataSize(advertisedMetadataSize >= 0 ? advertisedMetadataSize
                                                            : int(infoDict.size())) {
    connect(&m_server, &QTcpServer::newConnection, this, &TestMetadataPeer::onNewConnection);
    m_server.listen(QHostAddress::LocalHost, 0);
}

void TestMetadataPeer::onNewConnection() {
    while (QTcpSocket* c = m_server.nextPendingConnection()) {
        auto* s = new Session;
        s->sock = c;

        connect(c, &QTcpSocket::readyRead, this, [this, s] {
            s->buf += s->sock->readAll();

            if (!s->handshakeDone) {
                if (s->buf.size() < PeerWire::kHandshakeSize) return; // wait for more
                s->handshakeDone = true;
                s->buf.remove(0, PeerWire::kHandshakeSize);

                s->sock->write(PeerWire::handshake(m_infoHash, metadataPeerId()));
                s->sock->write(extendedHandshakeWithMetadata(kServerUtMetadataId, m_advertisedMetadataSize));
                s->sock->flush();
            }

            PeerWire::Msg msg;
            int consumed;
            while ((consumed = PeerWire::parseMessage(s->buf, &msg)) != 0) {
                if (consumed < 0) { // protocol error: fatal, unrecoverable stream
                    s->sock->disconnectFromHost();
                    return;
                }
                s->buf.remove(0, consumed);

                if (msg.id == PeerWire::Extended && !msg.payload.isEmpty()) {
                    const int extId = uint8_t(msg.payload[0]);
                    const QByteArray extPayload = msg.payload.mid(1);

                    if (extId == 0) {
                        // Client's extended handshake: learn its ut_metadata id.
                        int clientUtMetadataId = 0, unusedSize = 0;
                        PeerWire::parseExtendedHandshake(extPayload, &clientUtMetadataId, &unusedSize);
                        s->clientUtMetadataId = clientUtMetadataId;
                    } else if (extId == kServerUtMetadataId) {
                        // A ut_metadata request addressed to us (no trailing
                        // raw bytes on a request, so plain decode is exact).
                        bool ok = false;
                        const BencodeValue dict = Bencode::decode(extPayload, &ok);
                        if (ok && dict.type() == BencodeValue::Type::Dict &&
                            dict.contains("msg_type") && dict["msg_type"].toInt() == 0 &&
                            dict.contains("piece")) {
                            const int piece = int(dict["piece"].toInt());
                            const qint64 offset = qint64(piece) * kBlockSize;
                            if (offset < m_infoDict.size()) {
                                const QByteArray block = m_infoDict.mid(offset, kBlockSize);
                                s->sock->write(utMetadataData(s->clientUtMetadataId, piece,
                                                               int(m_infoDict.size()), block));
                            }
                        }
                    }
                }
                // keep-alives (id == -1) and any other message ids are ignored.
            }
            s->sock->flush();
        });

        connect(c, &QTcpSocket::disconnected, this, [this, s] {
            // Defer the delete: see the identical note in TestSeeder — the
            // protocol-error branch above may call disconnectFromHost()
            // re-entrantly while this lambda's readyRead frame is still on
            // the stack.
            s->sock->disconnect(this);
            s->sock->deleteLater();
            QTimer::singleShot(0, this, [s] { delete s; });
        });
    }
}
