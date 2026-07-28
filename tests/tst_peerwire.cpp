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

    // BEP 6 <have_all> (id 14): a seed may advertise a full set with this in
    // place of a <bitfield> frame even though we never negotiate the fast
    // extension. It must land as a complete peer bitfield and fire
    // bitfieldReceived — otherwise the picker mistakes a full seed for an empty
    // peer and starves (the 2026-07-27 field stall).
    void haveAllAdvertisesFullBitfield() {
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        const QByteArray infoHash(20, '\x01');
        PeerAddress addr; addr.host = "127.0.0.1"; addr.port = server.serverPort();
        PeerConnection conn(addr, infoHash, QByteArray(20, '\x02'), /*pieceCount=*/4, nullptr);
        QSignalSpy newConnSpy(&server, &QTcpServer::newConnection);
        QSignalSpy handshakeSpy(&conn, &PeerConnection::handshakeOk);
        QSignalSpy bitfieldSpy(&conn, &PeerConnection::bitfieldReceived);

        conn.connectToPeer();
        QVERIFY(newConnSpy.count() > 0 || newConnSpy.wait(2000));
        QTcpSocket* peerSock = server.nextPendingConnection();
        QVERIFY(peerSock);
        while (peerSock->bytesAvailable() < 68) QVERIFY(peerSock->waitForReadyRead(2000));
        peerSock->read(68);

        QByteArray reply = PeerWire::handshake(infoHash, QByteArray(20, '\x03'));
        QByteArray haveAll; appendBe32(haveAll, 1); haveAll.append(char(14)); // <len=1><id=14>
        reply += haveAll;
        peerSock->write(reply); peerSock->flush();

        QVERIFY(handshakeSpy.count() > 0 || handshakeSpy.wait(2000));
        QVERIFY(bitfieldSpy.count() > 0 || bitfieldSpy.wait(2000));
        QCOMPARE(conn.peerBitfield().count(), 4); // all 4 pieces
    }

    // BEP 6 <have_none> (id 15): the symmetric empty advertisement. Must fire
    // bitfieldReceived with an empty peer bitfield (peer holds nothing yet).
    void haveNoneAdvertisesEmptyBitfield() {
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        const QByteArray infoHash(20, '\x01');
        PeerAddress addr; addr.host = "127.0.0.1"; addr.port = server.serverPort();
        PeerConnection conn(addr, infoHash, QByteArray(20, '\x02'), /*pieceCount=*/4, nullptr);
        QSignalSpy newConnSpy(&server, &QTcpServer::newConnection);
        QSignalSpy handshakeSpy(&conn, &PeerConnection::handshakeOk);
        QSignalSpy bitfieldSpy(&conn, &PeerConnection::bitfieldReceived);

        conn.connectToPeer();
        QVERIFY(newConnSpy.count() > 0 || newConnSpy.wait(2000));
        QTcpSocket* peerSock = server.nextPendingConnection();
        QVERIFY(peerSock);
        while (peerSock->bytesAvailable() < 68) QVERIFY(peerSock->waitForReadyRead(2000));
        peerSock->read(68);

        QByteArray reply = PeerWire::handshake(infoHash, QByteArray(20, '\x03'));
        QByteArray haveNone; appendBe32(haveNone, 1); haveNone.append(char(15)); // <len=1><id=15>
        reply += haveNone;
        peerSock->write(reply); peerSock->flush();

        QVERIFY(handshakeSpy.count() > 0 || handshakeSpy.wait(2000));
        QVERIFY(bitfieldSpy.count() > 0 || bitfieldSpy.wait(2000));
        QCOMPARE(conn.peerBitfield().count(), 0); // holds nothing
    }

    // BEP 10: the metadata-mode ctor (no pieceCount) does the BT handshake
    // then the extended handshake only. The peer's extended handshake
    // advertises its own ut_metadata id + metadata_size; we must parse and
    // surface both via the extendedHandshake signal.
    void parsesExtendedHandshake() {
        QTcpServer server; QVERIFY(server.listen(QHostAddress::LocalHost));
        const QByteArray ih(20, '\x01');
        PeerAddress addr; addr.host = "127.0.0.1"; addr.port = server.serverPort();
        PeerConnection conn(addr, ih, QByteArray(20,'\x02'), nullptr); // metadata-mode ctor
        QSignalSpy newConn(&server, &QTcpServer::newConnection);
        QSignalSpy extSpy(&conn, &PeerConnection::extendedHandshake);
        conn.connectToPeer();
        QVERIFY(newConn.count() > 0 || newConn.wait(2000));
        QTcpSocket* s = server.nextPendingConnection(); QVERIFY(s);
        while (s->bytesAvailable() < 68) QVERIFY(s->waitForReadyRead(2000));
        s->read(68);
        // reply: our handshake (ext bit set) + extended handshake advertising ut_metadata=3, metadata_size=1234
        QByteArray reply = PeerWire::handshake(ih, QByteArray(20,'\x03'));
        // d1:md11:ut_metadatai3ee13:metadata_sizei1234ee  (bencoded), framed as ext msg id 0
        QByteArray ext = "d1:md11:ut_metadatai3ee13:metadata_sizei1234ee";
        QByteArray msg; quint32 len = 2 + ext.size();
        msg.append(char((len>>24)&0xFF)); msg.append(char((len>>16)&0xFF));
        msg.append(char((len>>8)&0xFF));  msg.append(char(len&0xFF));
        msg.append(char(20)); msg.append(char(0)); msg.append(ext);
        reply += msg; s->write(reply); s->flush();
        QVERIFY(extSpy.count() > 0 || extSpy.wait(2000));
        QCOMPARE(extSpy.at(0).at(0).toInt(), 3);      // peer's ut_metadata id
        QCOMPARE(extSpy.at(0).at(1).toInt(), 1234);   // metadata_size
    }
};
QTEST_MAIN(TstPeerWire)
#include "tst_peerwire.moc"
