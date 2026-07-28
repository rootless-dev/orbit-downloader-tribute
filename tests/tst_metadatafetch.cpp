#include <QtTest>
#include <QSignalSpy>
#include <QCryptographicHash>
#include <QNetworkAccessManager>

#include "torrent/MetadataFetch.h"
#include "torrent/Bencode.h"
#include "RateLimiter.h"
#include "TestMetadataPeer.h"

namespace {

// A valid bencoded BEP 3 info dict (mirrors tst_utmetadata's fixture).
QByteArray makeInfoDict() {
    QByteArray pieces;
    pieces.reserve(20 * 3);
    for (int i = 0; i < 3; ++i)
        pieces += QCryptographicHash::hash(QByteArray::number(i), QCryptographicHash::Sha1);

    QMap<QByteArray, BencodeValue> dict;
    dict.insert("name", BencodeValue::makeBytes("test-torrent.iso"));
    dict.insert("piece length", BencodeValue::makeInt(16384));
    dict.insert("pieces", BencodeValue::makeBytes(pieces));
    dict.insert("length", BencodeValue::makeInt(qint64(16384) * 3));
    return Bencode::encode(BencodeValue::makeDict(dict));
}

} // namespace

class TstMetadataFetch : public QObject {
    Q_OBJECT
private slots:
    void producesMetainfoFromPeer() {
        const QByteArray infoDict = makeInfoDict();
        const QByteArray ih = QCryptographicHash::hash(infoDict, QCryptographicHash::Sha1);
        TestMetadataPeer peer(ih, infoDict);

        MagnetInfo mi;
        mi.infoHash = ih;
        mi.displayName = "x";

        QNetworkAccessManager nam;
        RateLimiter rl;
        MetadataFetch mf(mi, nullptr, &nam, 1, &rl);
        QSignalSpy ready(&mf, &MetadataFetch::metainfoReady);
        QSignalSpy failed(&mf, &MetadataFetch::failed);

        mf.addPeerForTest({"127.0.0.1", peer.port()});
        mf.start();

        QVERIFY(ready.wait(5000));
        QCOMPARE(failed.count(), 0);

        auto meta = ready.at(0).at(0).value<TorrentMetainfo>();
        QCOMPARE(meta.infoHash, ih);
        QVERIFY(meta.pieceHashes.size() > 0);

        // metainfoReady's second argument is the verbatim, SHA-1-verified raw
        // info dict -- NOT a re-encoding -- so it must be byte-identical to
        // what the peer served (and thus hash back to the same info-hash).
        const auto emittedInfoDict = ready.at(0).at(1).value<QByteArray>();
        QCOMPARE(emittedInfoDict, infoDict);
    }
};

QTEST_GUILESS_MAIN(TstMetadataFetch)
#include "tst_metadatafetch.moc"
