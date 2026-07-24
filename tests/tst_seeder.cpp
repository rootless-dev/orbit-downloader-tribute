#include <QtTest>
#include <QTcpSocket>
#include "TestSeeder.h"
#include "torrent/PeerWire.h"
#include "torrent/Bitfield.h"

namespace {

// Local mirror of PeerWire's internal be32 reader, for decoding what the
// seeder sends back (kept independent of the production/test helpers under
// test).
int be32(const QByteArray& b, int off) {
    return (quint8(b[off]) << 24) | (quint8(b[off + 1]) << 16) | (quint8(b[off + 2]) << 8) |
           quint8(b[off + 3]);
}

// QTcpSocket::waitForReadyRead() only polls that socket's own descriptor —
// it does not pump the Qt event loop, so a same-process QTcpServer's
// newConnection/readyRead notifiers (which are event-loop driven) never
// fire while it blocks. QTest::qWaitFor pumps the full event loop instead,
// which is what actually lets TestSeeder (or TestFtpServer, see tst_ftp.cpp)
// run in the same thread as the client socket under test. Drains whatever
// is available into `buf` until it holds at least `minSize` bytes.
bool waitForBytes(QTcpSocket& sock, QByteArray& buf, qsizetype minSize, int timeoutMs = 3000) {
    return QTest::qWaitFor([&] {
        buf += sock.readAll();
        return buf.size() >= minSize;
    }, timeoutMs);
}

} // namespace

class TstSeeder : public QObject {
    Q_OBJECT
private slots:
    void completesHandshakeAndUnchokes() {
        QByteArray ih(20, '\x09'), data(50000, 'Z');
        TestSeeder s(ih, data, 16384);
        QTcpSocket sock;
        sock.connectToHost("127.0.0.1", s.port());
        QVERIFY(sock.waitForConnected());
        sock.write(PeerWire::handshake(ih, QByteArray(20, '\x01')));

        QByteArray in;
        QVERIFY(waitForBytes(sock, in, 68)); // seeder echoes handshake, then bitfield+unchoke
        QVERIFY(in.size() >= 68);
        QCOMPARE(in.left(68), PeerWire::handshake(ih, in.mid(48, 20)));
    }

    void sendsBitfieldThenUnchoke() {
        QByteArray ih(20, '\x09'), data(50000, 'Z');
        const qint64 pieceLength = 16384;
        const int pieceCount = 4; // ceil(50000/16384)
        TestSeeder s(ih, data, pieceLength);
        QTcpSocket sock;
        sock.connectToHost("127.0.0.1", s.port());
        QVERIFY(sock.waitForConnected());
        sock.write(PeerWire::handshake(ih, QByteArray(20, '\x01')));

        QByteArray in;
        QVERIFY(waitForBytes(sock, in, 68 + 5));
        QByteArray rest = in.mid(68);

        // bitfield frame
        QVERIFY(waitForBytes(sock, rest, 4 + 1));
        const int bfLen = be32(rest, 0);
        QVERIFY(waitForBytes(sock, rest, 4 + bfLen));
        QCOMPARE(quint8(rest[4]), quint8(PeerWire::Bitfield));
        Bitfield bf = Bitfield::fromBytes(rest.mid(5, bfLen - 1), pieceCount);
        for (int i = 0; i < pieceCount; ++i) QVERIFY(bf.has(i));
        rest.remove(0, 4 + bfLen);

        // unchoke frame: <0001><1>
        QVERIFY(waitForBytes(sock, rest, 5));
        QCOMPARE(be32(rest, 0), 1);
        QCOMPARE(quint8(rest[4]), quint8(PeerWire::Unchoke));
    }

    void repliesToRequestWithPiece() {
        QByteArray ih(20, '\x09');
        QByteArray data(50000, 'Z');
        for (int i = 0; i < data.size(); ++i) data[i] = char(i % 251); // distinguishable bytes
        const qint64 pieceLength = 16384;
        TestSeeder s(ih, data, pieceLength);
        QTcpSocket sock;
        sock.connectToHost("127.0.0.1", s.port());
        QVERIFY(sock.waitForConnected());
        sock.write(PeerWire::handshake(ih, QByteArray(20, '\x01')));

        // Drain handshake + bitfield + unchoke before sending our request.
        QByteArray in;
        QVERIFY(waitForBytes(sock, in, 68));
        QByteArray buf = in.mid(68);

        PeerWire::Msg msg;
        int consumed;
        // consume bitfield
        while ((consumed = PeerWire::parseMessage(buf, &msg)) == 0)
            QVERIFY(waitForBytes(sock, buf, buf.size() + 1));
        QCOMPARE(msg.id, int(PeerWire::Bitfield));
        buf.remove(0, consumed);
        // consume unchoke
        while ((consumed = PeerWire::parseMessage(buf, &msg)) == 0)
            QVERIFY(waitForBytes(sock, buf, buf.size() + 1));
        QCOMPARE(msg.id, int(PeerWire::Unchoke));
        buf.remove(0, consumed);

        // request piece 1, begin 100, length 500 (crosses into second piece)
        const int piece = 1;
        const qint64 begin = 100, length = 500;
        sock.write(PeerWire::request(piece, begin, length));

        while ((consumed = PeerWire::parseMessage(buf, &msg)) == 0)
            QVERIFY(waitForBytes(sock, buf, buf.size() + 1));
        QCOMPARE(msg.id, int(PeerWire::Piece));
        QCOMPARE(be32(msg.payload, 0), piece);
        QCOMPARE(be32(msg.payload, 4), int(begin));
        QByteArray block = msg.payload.mid(8);
        QCOMPARE(block, data.mid(piece * pieceLength + begin, length));
    }
};

QTEST_GUILESS_MAIN(TstSeeder)
#include "tst_seeder.moc"
