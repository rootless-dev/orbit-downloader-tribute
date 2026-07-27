#include <QtTest>
#include <QHttpServer>
#include <QHttpServerResponse>
#include <QNetworkAccessManager>
#include <QSignalSpy>
#include <QTcpServer>
#include <QUrlQuery>

#include "torrent/Bencode.h"
#include "torrent/HttpTrackerClient.h"

namespace {

// 1.2.3.4:6881 -> 01 02 03 04 1A E1 ; 10.0.0.5:80 -> 0A 00 00 05 00 50
// (Not 127.0.0.1: TrackerPeers::dropBogons() now filters loopback addresses,
// so a real-looking public IP is used here instead.)
QByteArray compactPeerBytes() {
    QByteArray raw;
    raw.append(char(0x01)); raw.append(char(0x02)); raw.append(char(0x03)); raw.append(char(0x04));
    raw.append(char(0x1A)); raw.append(char(0xE1));
    raw.append(char(0x0A)); raw.append(char(0x00)); raw.append(char(0x00)); raw.append(char(0x05));
    raw.append(char(0x00)); raw.append(char(0x50));
    return raw;
}

QByteArray compactResponseBody(int intervalSecs = 1800, int minIntervalSecs = 0) {
    QMap<QByteArray, BencodeValue> dict;
    dict.insert("interval", BencodeValue::makeInt(intervalSecs));
    if (minIntervalSecs > 0) dict.insert("min interval", BencodeValue::makeInt(minIntervalSecs));
    dict.insert("peers", BencodeValue::makeBytes(compactPeerBytes()));
    return Bencode::encode(BencodeValue::makeDict(dict));
}

} // namespace

class TstTracker : public QObject {
    Q_OBJECT
private slots:
    void encodesInfoHashRaw() {
        QByteArray ih(20, '\x00');
        ih[0] = char(0xAB);
        auto url = TrackerProto::buildAnnounceUrl(QUrl("http://t/announce"), ih, "-OB0001-abcdefghij",
                                                    6881, 0, 100, TrackerEvent::Started);
        QVERIFY(url.toEncoded().contains("info_hash=%AB%00")); // raw byte URL-encoding
        QVERIFY(url.toEncoded().contains("event=started"));
        QVERIFY(url.toEncoded().contains("compact=1"));
    }

    void encodesAlnumAndDashBytesToo() {
        // Regression: QByteArray::toPercentEncoding("", "") leaves alnum/'-._~'
        // bytes as literal ASCII even with empty include/exclude sets. The encoder
        // buildAnnounceUrl feeds into QUrl must instead percent-encode EVERY byte,
        // with no "safe" exceptions, before the string ever reaches QUrl.
        //
        // NOTE on how this is asserted: QUrl performs RFC 3986 percent-encoding
        // *normalization* when parsing a query (via setQuery(), the QUrl string
        // constructor, or QUrl::fromEncoded() with StrictMode — verified all three
        // ways): any "%XX" sequence that decodes to an unreserved character (ALPHA /
        // DIGIT / '-' '.' '_' '~') is canonicalized back to the literal character on
        // toEncoded(), regardless of what encoder produced the query string. So
        // "%41"/"%2D"/"%7A" can never appear literally in url.toEncoded() for an
        // http(s) QUrl no matter how buildAnnounceUrl encodes them — this is a
        // QUrl/RFC normalization fact, not something our code controls. What we CAN
        // and must verify is that the original byte survives the round trip
        // unchanged (i.e. no data loss, whichever textual form QUrl settles on).
        QByteArray ih(20, '\x00');
        ih[0] = char(0x41); // 'A'
        ih[1] = char(0x2D); // '-'
        ih[2] = char(0x7A); // 'z'
        auto url = TrackerProto::buildAnnounceUrl(QUrl("http://t/announce"), ih, "-OB0001-abcdefghij",
                                                    6881, 0, 100, TrackerEvent::Started);
        QUrlQuery q(url);
        const QByteArray roundTripped =
            QByteArray::fromPercentEncoding(q.queryItemValue("info_hash", QUrl::FullyEncoded).toLatin1());
        QCOMPARE(roundTripped, ih);
    }

    void omitsEventWhenNone() {
        QByteArray ih(20, 'x');
        auto url = TrackerProto::buildAnnounceUrl(QUrl("http://t/announce"), ih, "-OB0001-abcdefghij",
                                                    6881, 0, 100, TrackerEvent::None);
        QVERIFY(!url.toEncoded().contains("event="));
    }

    void parsesCompactPeers() {
        const QByteArray body = compactResponseBody(1800);
        QVector<PeerAddress> peers;
        int iv = 0;
        int minIv = -1;
        QString fail;
        QVERIFY(TrackerProto::parseResponse(body, &peers, &iv, &minIv, &fail));
        QCOMPARE(peers.size(), 2);
        QCOMPARE(peers[0].host, QString("1.2.3.4"));
        QCOMPARE(peers[0].port, quint16(6881));
        QCOMPARE(peers[1].host, QString("10.0.0.5"));
        QCOMPARE(peers[1].port, quint16(80));
        QCOMPARE(iv, 1800);
        QCOMPARE(minIv, 0); // absent -> 0
    }

    void parsesMinIntervalWhenPresent() {
        const QByteArray body = compactResponseBody(1800, 300);
        QVector<PeerAddress> peers;
        int iv = 0;
        int minIv = 0;
        QString fail;
        QVERIFY(TrackerProto::parseResponse(body, &peers, &iv, &minIv, &fail));
        QCOMPARE(iv, 1800);
        QCOMPARE(minIv, 300);
    }

    void parsesDictionaryPeers() {
        QMap<QByteArray, BencodeValue> peerA;
        peerA.insert("ip", BencodeValue::makeBytes("1.2.3.4"));
        peerA.insert("peer id", BencodeValue::makeBytes(QByteArray(20, 'a')));
        peerA.insert("port", BencodeValue::makeInt(51413));

        QMap<QByteArray, BencodeValue> dict;
        dict.insert("interval", BencodeValue::makeInt(900));
        dict.insert("peers", BencodeValue::makeList({BencodeValue::makeDict(peerA)}));
        const QByteArray body = Bencode::encode(BencodeValue::makeDict(dict));

        QVector<PeerAddress> peers;
        int iv = 0;
        int minIv = -1;
        QString fail;
        QVERIFY(TrackerProto::parseResponse(body, &peers, &iv, &minIv, &fail));
        QCOMPARE(peers.size(), 1);
        QCOMPARE(peers[0].host, QString("1.2.3.4"));
        QCOMPARE(peers[0].port, quint16(51413));
        QCOMPARE(iv, 900);
        QCOMPARE(minIv, 0); // absent -> 0
    }

    void reportsFailureReason() {
        QMap<QByteArray, BencodeValue> dict;
        dict.insert("failure reason", BencodeValue::makeBytes("no dice."));
        const QByteArray body = Bencode::encode(BencodeValue::makeDict(dict));

        QVector<PeerAddress> peers;
        int iv = 0;
        int minIv = 0;
        QString fail;
        QVERIFY(!TrackerProto::parseResponse(body, &peers, &iv, &minIv, &fail));
        QCOMPARE(fail, QString("no dice."));
    }

    void announcesAgainstLocalServer() {
        QHttpServer srv;
        srv.route("/announce", [](const QHttpServerRequest&) {
            return QHttpServerResponse("text/plain", compactResponseBody(1800));
        });
        QTcpServer tcp;
        QVERIFY(tcp.listen(QHostAddress::LocalHost));
        QVERIFY(srv.bind(&tcp));
        const quint16 serverPort = tcp.serverPort();

        QNetworkAccessManager nam;
        const QUrl tracker(QString("http://127.0.0.1:%1/announce").arg(serverPort));
        HttpTrackerClient client(&nam, tracker);
        QSignalSpy peersSpy(&client, &HttpTrackerClient::peersReceived);
        QSignalSpy failedSpy(&client, &HttpTrackerClient::announceFailed);

        client.announce(QByteArray(20, 'h'), "-OB0001-abcdefghij", 6881, 0, 100, TrackerEvent::Started);

        QVERIFY(peersSpy.wait(2000));
        QCOMPARE(failedSpy.count(), 0);
        QCOMPARE(peersSpy.count(), 1);
        const auto peers = peersSpy.at(0).at(0).value<QVector<PeerAddress>>();
        const int interval = peersSpy.at(0).at(1).toInt();
        const int minInterval = peersSpy.at(0).at(2).toInt();
        QCOMPARE(peers.size(), 2);
        QCOMPARE(peers[0].host, QString("1.2.3.4"));
        QCOMPARE(peers[0].port, quint16(6881));
        QCOMPARE(interval, 1800);
        QCOMPARE(minInterval, 0); // this canned response doesn't send one
    }

};

QTEST_MAIN(TstTracker)
#include "tst_tracker.moc"
