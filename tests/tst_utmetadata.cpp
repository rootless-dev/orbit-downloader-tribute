#include <QtTest>
#include <QSignalSpy>
#include <QCryptographicHash>

#include "torrent/PeerConnection.h"
#include "torrent/Bencode.h"
#include "TestMetadataPeer.h"

namespace {

// A valid bencoded BEP 3 info dict, padded (via a large "pieces" field) past
// one 16 KiB ut_metadata block so the test exercises multi-piece
// request/accumulate/reassemble, not just the single-piece happy path.
QByteArray makeInfoDict() {
    QByteArray pieces;
    pieces.reserve(20 * 900);
    for (int i = 0; i < 900; ++i)
        pieces += QCryptographicHash::hash(QByteArray::number(i), QCryptographicHash::Sha1);

    QMap<QByteArray, BencodeValue> dict;
    dict.insert("name", BencodeValue::makeBytes("test-torrent.iso"));
    dict.insert("piece length", BencodeValue::makeInt(16384));
    dict.insert("pieces", BencodeValue::makeBytes(pieces));
    dict.insert("length", BencodeValue::makeInt(qint64(16384) * 900));
    return Bencode::encode(BencodeValue::makeDict(dict));
}

} // namespace

class TstUtMetadata : public QObject {
    Q_OBJECT
private slots:
    void fetchesAndVerifiesMetadata() {
        const QByteArray infoDict = makeInfoDict();
        QVERIFY(infoDict.size() > 16384); // sanity: must span >1 metadata piece
        const QByteArray ih = QCryptographicHash::hash(infoDict, QCryptographicHash::Sha1);
        TestMetadataPeer peer(ih, infoDict);
        PeerConnection conn({"127.0.0.1", peer.port()}, ih, QByteArray(20, '\x02'), nullptr);
        QSignalSpy done(&conn, &PeerConnection::metadataComplete);
        QSignalSpy failed(&conn, &PeerConnection::metadataFailed);
        conn.connectToPeer();
        QVERIFY(done.wait(5000));
        QCOMPARE(failed.count(), 0);
        QCOMPARE(done.at(0).at(0).toByteArray(), infoDict); // exact bytes recovered
    }

    void rejectsTamperedMetadata() {
        QByteArray infoDict = makeInfoDict();
        const QByteArray ih = QCryptographicHash::hash(infoDict, QCryptographicHash::Sha1);
        QByteArray tampered = infoDict;
        tampered[10] = char(tampered[10] ^ 0xFF);
        TestMetadataPeer peer(ih, tampered); // serves wrong bytes for a right hash
        PeerConnection conn({"127.0.0.1", peer.port()}, ih, QByteArray(20, '\x02'), nullptr);
        QSignalSpy done(&conn, &PeerConnection::metadataComplete);
        QSignalSpy fail(&conn, &PeerConnection::metadataFailed);
        conn.connectToPeer();
        QVERIFY(fail.wait(5000));
        QCOMPARE(done.count(), 0);
    }

    // A peer advertising an implausibly large metadata_size (bigger than any
    // real torrent's info dict could be) must be rejected before
    // PeerConnection allocates a buffer of that size or issues any
    // ut_metadata requests for it.
    void rejectsOversizedMetadataSize() {
        const QByteArray infoDict = makeInfoDict();
        const QByteArray ih = QCryptographicHash::hash(infoDict, QCryptographicHash::Sha1);
        // Peer really has `infoDict` (small) but lies in its extended
        // handshake and claims 500 MiB of metadata.
        TestMetadataPeer peer(ih, infoDict, nullptr, 500 * 1024 * 1024);
        PeerConnection conn({"127.0.0.1", peer.port()}, ih, QByteArray(20, '\x02'), nullptr);
        QSignalSpy done(&conn, &PeerConnection::metadataComplete);
        QSignalSpy failed(&conn, &PeerConnection::metadataFailed);
        conn.connectToPeer();
        QVERIFY(failed.wait(5000));
        QCOMPARE(done.count(), 0);
    }
};

QTEST_GUILESS_MAIN(TstUtMetadata)
#include "tst_utmetadata.moc"
