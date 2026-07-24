#include <QtTest>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include "torrent/PeerConnection.h"
#include "torrent/PeerWire.h"

namespace {
// Big-endian helper for hand-assembling wire messages in the smoke test below.
void appendBe32(QByteArray& out, quint32 n) {
    out.append(char((n >> 24) & 0xFF)); out.append(char((n >> 16) & 0xFF));
    out.append(char((n >> 8) & 0xFF));  out.append(char(n & 0xFF));
}
} // namespace

class TstPeerWire : public QObject { Q_OBJECT
private slots:
    void buildsHandshake() {
        QByteArray ih(20,'\x01'), id(20,'\x02');
        auto h = PeerWire::handshake(ih, id);
        QCOMPARE(h.size(), 68); QCOMPARE(h[0], char(19));
        QCOMPARE(h.mid(1,19), QByteArray("BitTorrent protocol"));
        QCOMPARE(h.mid(28,20), ih);
    }
    void parsesKeepAlive() { PeerWire::Msg m; int n = PeerWire::parseMessage(QByteArray(4,'\x00'), &m);
        QCOMPARE(n,4); QCOMPARE(m.id,-1); }
    void parsesUnchoke() { QByteArray b; b.append(QByteArray::fromHex("00000001")); b.append(char(1));
        PeerWire::Msg m; int n = PeerWire::parseMessage(b,&m); QCOMPARE(n,5); QCOMPARE(m.id,1); }
    void needsMoreOnPartial() { PeerWire::Msg m; QCOMPARE(PeerWire::parseMessage(QByteArray(2,'\x00'), &m), 0); }
    void buildsRequest() { auto r = PeerWire::request(1, 16384, 16384);
        QCOMPARE(r.size(), 17); QCOMPARE(r[4], char(6)); }
    void rejectsOversizedLength() {
        QByteArray b; appendBe32(b, quint32(PeerWire::kMaxMessageLength) + 1);
        PeerWire::Msg m;
        QVERIFY(PeerWire::parseMessage(b, &m) < 0);
    }
    void parsesPieceSplitAcrossBuffers() {
        // A piece message (id=7) whose payload arrives in two pieces should
        // report "need more" (0) until the full frame is buffered.
        QByteArray payload; appendBe32(payload, 2); appendBe32(payload, 0); payload += "hi";
        QByteArray full; appendBe32(full, quint32(1 + payload.size())); full.append(char(7)); full += payload;
        PeerWire::Msg m;
        QCOMPARE(PeerWire::parseMessage(full.left(full.size() - 1), &m), 0);
        QCOMPARE(PeerWire::parseMessage(full, &m), full.size());
        QCOMPARE(m.id, 7);
        QCOMPARE(m.payload, payload);
    }

    // A light end-to-end smoke test: a real PeerConnection talking over a
    // loopback QTcpSocket to a QTcpServer standing in for a peer. Exercises
    // the handshake validation + message dispatch wiring that the pure
    // PeerWire tests above can't reach; full protocol-compliance testing
    // against a real seeder is Task 10's job (tst_torrent + TestSeeder).
    void connectionDispatchesHandshakeAndMessages() {
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));

        const QByteArray infoHash(20, '\x01');
        const QByteArray ourPeerId(20, '\x02');
        const QByteArray theirPeerId(20, '\x03');

        PeerAddress addr; addr.host = "127.0.0.1"; addr.port = server.serverPort();
        PeerConnection conn(addr, infoHash, ourPeerId, /*pieceCount=*/4, nullptr);

        QSignalSpy newConnSpy(&server, &QTcpServer::newConnection);
        QSignalSpy handshakeSpy(&conn, &PeerConnection::handshakeOk);
        QSignalSpy bitfieldSpy(&conn, &PeerConnection::bitfieldReceived);
        QSignalSpy unchokeSpy(&conn, &PeerConnection::unchoked);
        QSignalSpy blockSpy(&conn, &PeerConnection::blockReceived);

        conn.connectToPeer();
        QVERIFY(newConnSpy.count() > 0 || newConnSpy.wait(2000));
        QTcpSocket* peerSock = server.nextPendingConnection();
        QVERIFY(peerSock);

        while (peerSock->bytesAvailable() < 68) QVERIFY(peerSock->waitForReadyRead(2000));
        const QByteArray ourHandshake = peerSock->read(68);
        QCOMPARE(ourHandshake.size(), 68);
        QCOMPARE(ourHandshake.mid(28, 20), infoHash);

        // Reply with our own handshake, then a bitfield (all 4 pieces present),
        // an unchoke, and a piece message — all in one write, to also exercise
        // draining several frames out of one readyRead.
        QByteArray reply = PeerWire::handshake(infoHash, theirPeerId);
        QByteArray bfMsg; appendBe32(bfMsg, 2); bfMsg.append(char(5)); bfMsg.append(char(0xF0));
        reply += bfMsg;
        QByteArray unchokeMsg; appendBe32(unchokeMsg, 1); unchokeMsg.append(char(1));
        reply += unchokeMsg;
        QByteArray piecePayload; appendBe32(piecePayload, 2); appendBe32(piecePayload, 0); piecePayload += "hi";
        QByteArray pieceMsg; appendBe32(pieceMsg, quint32(1 + piecePayload.size())); pieceMsg.append(char(7));
        pieceMsg += piecePayload;
        reply += pieceMsg;
        peerSock->write(reply);
        peerSock->flush();

        QVERIFY(handshakeSpy.count() > 0 || handshakeSpy.wait(2000));
        QVERIFY(bitfieldSpy.count() > 0 || bitfieldSpy.wait(2000));
        QVERIFY(unchokeSpy.count() > 0 || unchokeSpy.wait(2000));
        QVERIFY(blockSpy.count() > 0 || blockSpy.wait(2000));

        QCOMPARE(conn.peerBitfield().count(), 4);
        QVERIFY(conn.amUnchoked());
        QCOMPARE(blockSpy.at(0).at(0).toInt(), 2);
        QCOMPARE(blockSpy.at(0).at(1).toLongLong(), qint64(0));
        QCOMPARE(blockSpy.at(0).at(2).toByteArray(), QByteArray("hi"));
    }
};
QTEST_MAIN(TstPeerWire)
#include "tst_peerwire.moc"
