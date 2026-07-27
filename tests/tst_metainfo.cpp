#include <QtTest>
#include <QCryptographicHash>
#include "torrent/TorrentMetainfo.h"
#include "torrent/Bencode.h"

// helper: build a minimal single-file .torrent with the given piece hashes
static QByteArray singleFile(qint64 len, qint64 pieceLen, const QByteArray& pieces) {
    QMap<QByteArray, BencodeValue> info;
    info.insert("length", BencodeValue::makeInt(len));
    info.insert("name", BencodeValue::makeBytes("file"));
    info.insert("piece length", BencodeValue::makeInt(pieceLen));
    info.insert("pieces", BencodeValue::makeBytes(pieces));

    QMap<QByteArray, BencodeValue> root;
    root.insert("announce", BencodeValue::makeBytes("http://tracker.example/announce"));
    root.insert("info", BencodeValue::makeDict(info));

    return Bencode::encode(BencodeValue::makeDict(root));
}

// helper: build a minimal single-file .torrent with an explicit (possibly
// unsafe) top-level "name", for exercising name validation.
static QByteArray singleFileNamed(const QByteArray& name, qint64 len, qint64 pieceLen, const QByteArray& pieces) {
    QMap<QByteArray, BencodeValue> info;
    info.insert("length", BencodeValue::makeInt(len));
    info.insert("name", BencodeValue::makeBytes(name));
    info.insert("piece length", BencodeValue::makeInt(pieceLen));
    info.insert("pieces", BencodeValue::makeBytes(pieces));

    QMap<QByteArray, BencodeValue> root;
    root.insert("announce", BencodeValue::makeBytes("http://tracker.example/announce"));
    root.insert("info", BencodeValue::makeDict(info));

    return Bencode::encode(BencodeValue::makeDict(root));
}

// helper: build a single-file .torrent by hand-assembling raw bencode bytes,
// with the info dict's keys in NON-canonical (unsorted) order. Bencode::encode
// always emits keys in sorted order, so a fixture built via encode() cannot
// distinguish "hash the raw slice" from "hash a re-encoded copy" (they'd be
// byte-identical). This helper is independent of Bencode::encode's key
// ordering, so it can actually catch a regression to re-encoding.
static QByteArray singleFileNonCanonical(qint64 len, qint64 pieceLen, const QByteArray& pieces) {
    QByteArray info;
    info += "d";
    info += "12:piece length"; info += "i" + QByteArray::number(pieceLen) + "e";
    info += "6:pieces"; info += QByteArray::number(pieces.size()) + ":" + pieces;
    info += "6:length"; info += "i" + QByteArray::number(len) + "e";
    info += "4:name"; info += "4:file";
    info += "e";

    const QByteArray announceUrl = "http://tracker.example/announce";
    QByteArray root;
    root += "d";
    root += "8:announce"; root += QByteArray::number(announceUrl.size()) + ":" + announceUrl;
    root += "4:info"; root += info;
    root += "e";
    return root;
}

// helper: build a minimal single-file .torrent with an explicit BEP 12
// "announce-list" (a list of tiers, each tier a list of tracker URL strings).
static QByteArray singleFileWithAnnounceList(qint64 len, qint64 pieceLen, const QByteArray& pieces,
                                              const QVector<QVector<QByteArray>>& tiers) {
    QMap<QByteArray, BencodeValue> info;
    info.insert("length", BencodeValue::makeInt(len));
    info.insert("name", BencodeValue::makeBytes("file"));
    info.insert("piece length", BencodeValue::makeInt(pieceLen));
    info.insert("pieces", BencodeValue::makeBytes(pieces));

    QList<BencodeValue> tierList;
    for (const auto& tier : tiers) {
        QList<BencodeValue> urlList;
        for (const auto& url : tier)
            urlList.append(BencodeValue::makeBytes(url));
        tierList.append(BencodeValue::makeList(urlList));
    }

    QMap<QByteArray, BencodeValue> root;
    root.insert("announce", BencodeValue::makeBytes("http://tracker.example/announce"));
    root.insert("announce-list", BencodeValue::makeList(tierList));
    root.insert("info", BencodeValue::makeDict(info));

    return Bencode::encode(BencodeValue::makeDict(root));
}

// helper: build a minimal multi-file .torrent with the given piece hashes
static QByteArray multiFile(const QVector<QPair<QString, qint64>>& fileSpecs,
                             qint64 pieceLen, const QByteArray& pieces) {
    QList<BencodeValue> files;
    for (const auto& spec : fileSpecs) {
        QMap<QByteArray, BencodeValue> fileDict;
        fileDict.insert("length", BencodeValue::makeInt(spec.second));
        QList<BencodeValue> path;
        for (const QString& segment : spec.first.split('/'))
            path.append(BencodeValue::makeBytes(segment.toUtf8()));
        fileDict.insert("path", BencodeValue::makeList(path));
        files.append(BencodeValue::makeDict(fileDict));
    }

    QMap<QByteArray, BencodeValue> info;
    info.insert("name", BencodeValue::makeBytes("multi"));
    info.insert("piece length", BencodeValue::makeInt(pieceLen));
    info.insert("pieces", BencodeValue::makeBytes(pieces));
    info.insert("files", BencodeValue::makeList(files));

    QMap<QByteArray, BencodeValue> root;
    root.insert("announce", BencodeValue::makeBytes("http://tracker.example/announce"));
    root.insert("info", BencodeValue::makeDict(info));

    return Bencode::encode(BencodeValue::makeDict(root));
}

class TstMetainfo : public QObject { Q_OBJECT
private slots:
    void parsesSingleFile() {
        QByteArray pieces(20, '\x11'); // one piece hash
        auto bytes = singleFile(/*len*/100, /*pieceLen*/100, pieces);
        bool ok=false; auto m = TorrentMetainfo::parse(bytes, &ok);
        QVERIFY(ok);
        QCOMPARE(m.isMultiFile, false);
        QCOMPARE(m.totalLength, 100LL);
        QCOMPARE(m.pieceHashes.size(), 1);
        QCOMPARE(m.files.size(), 1);
        QCOMPARE(m.files[0].offset, 0LL);
        QCOMPARE(m.name, QString("file"));
        QCOMPARE(m.pieceLength, 100LL);
        QCOMPARE(m.announce, QUrl("http://tracker.example/announce"));
    }
    void computesInfoHashFromRawBytes() {
        // Info dict keys are deliberately in non-canonical (unsorted) order,
        // so this fixture cannot be produced by Bencode::encode. This makes
        // "hash the raw slice" and "hash a re-encoded copy" diverge, which is
        // the exact bug this test must catch.
        QByteArray pieces(20, '\x11');
        auto bytes = singleFileNonCanonical(100, 100, pieces);
        bool ok=false; auto m = TorrentMetainfo::parse(bytes, &ok);
        QVERIFY(ok);
        // recompute expected: SHA1 over the exact info dict slice
        bool ok2=false; auto root = Bencode::decode(bytes, &ok2);
        QVERIFY(ok2);
        const auto& info = root[QByteArray("info")];
        auto expectRaw = QCryptographicHash::hash(bytes.mid(info.rawBegin(), info.rawEnd()-info.rawBegin()),
                                                  QCryptographicHash::Sha1);
        QCOMPARE(m.infoHash, expectRaw);

        // Discriminate against a buggy impl that re-encodes info before
        // hashing: since the raw bytes are non-canonical, the canonical
        // re-encoding must hash to something different.
        auto expectReencoded = QCryptographicHash::hash(Bencode::encode(info), QCryptographicHash::Sha1);
        QVERIFY(m.infoHash != expectReencoded);
    }
    void parsesMultiFileOffsets() {
        // two files of 30 and 70; assert files[1].offset == 30, totalLength == 100
        QByteArray pieces(20 * 1, '\x22'); // ceil(100/100)=1 piece
        QVector<QPair<QString, qint64>> specs = { {"a.txt", 30}, {"sub/b.txt", 70} };
        auto bytes = multiFile(specs, /*pieceLen*/100, pieces);
        bool ok=false; auto m = TorrentMetainfo::parse(bytes, &ok);
        QVERIFY(ok);
        QCOMPARE(m.isMultiFile, true);
        QCOMPARE(m.totalLength, 100LL);
        QCOMPARE(m.files.size(), 2);
        QCOMPARE(m.files[0].offset, 0LL);
        QCOMPARE(m.files[0].path, QString("a.txt"));
        QCOMPARE(m.files[1].offset, 30LL);
        QCOMPARE(m.files[1].path, QString("sub/b.txt"));
    }
    void rejectsBadPiecesLength() {
        QByteArray pieces(19, '\x00'); // not a multiple of 20
        auto bytes = singleFile(100, 100, pieces);
        bool ok=true; TorrentMetainfo::parse(bytes, &ok); QVERIFY(!ok);
    }
    void rejectsMissingInfoDict() {
        QMap<QByteArray, BencodeValue> root;
        root.insert("announce", BencodeValue::makeBytes("http://tracker.example/announce"));
        auto bytes = Bencode::encode(BencodeValue::makeDict(root));
        bool ok=true; TorrentMetainfo::parse(bytes, &ok); QVERIFY(!ok);
    }
    void rejectsPieceCountMismatch() {
        QByteArray pieces(20, '\x11'); // one piece hash, but totalLength implies 2 pieces
        auto bytes = singleFile(/*len*/150, /*pieceLen*/100, pieces);
        bool ok=true; TorrentMetainfo::parse(bytes, &ok); QVERIFY(!ok);
    }
    void rejectsBothLengthAndFiles() {
        QByteArray pieces(20, '\x11');
        QMap<QByteArray, BencodeValue> fileDict;
        fileDict.insert("length", BencodeValue::makeInt(50));
        fileDict.insert("path", BencodeValue::makeList({ BencodeValue::makeBytes("part") }));

        QMap<QByteArray, BencodeValue> info;
        info.insert("length", BencodeValue::makeInt(100));
        info.insert("name", BencodeValue::makeBytes("file"));
        info.insert("piece length", BencodeValue::makeInt(100));
        info.insert("pieces", BencodeValue::makeBytes(pieces));
        info.insert("files", BencodeValue::makeList({ BencodeValue::makeDict(fileDict) }));

        QMap<QByteArray, BencodeValue> root;
        root.insert("announce", BencodeValue::makeBytes("http://tracker.example/announce"));
        root.insert("info", BencodeValue::makeDict(info));

        auto bytes = Bencode::encode(BencodeValue::makeDict(root));
        bool ok=true; TorrentMetainfo::parse(bytes, &ok); QVERIFY(!ok);
    }
    void rejectsUnsafeName() {
        QByteArray pieces(20, '\x11'); // ceil(100/100) = 1 piece

        // Relative traversal: name = "../evil" would let a destDir join
        // escape upward.
        auto traversal = singleFileNamed("../evil", 100, 100, pieces);
        bool ok1 = true; TorrentMetainfo::parse(traversal, &ok1); QVERIFY(!ok1);

        // Absolute path: name = "/etc/passwd" would let a destDir join be
        // ignored entirely (QDir::filePath on an absolute path returns it
        // as-is), writing straight to an arbitrary filesystem location.
        auto absolute = singleFileNamed("/etc/passwd", 100, 100, pieces);
        bool ok2 = true; TorrentMetainfo::parse(absolute, &ok2); QVERIFY(!ok2);
    }
    void rejectsPathTraversalSegment() {
        QByteArray pieces(20, '\x11'); // ceil(100/100) = 1 piece
        QVector<QPair<QString, qint64>> specs = { {"..", 30}, {"b.txt", 70} };
        auto bytes = multiFile(specs, /*pieceLen*/100, pieces);
        bool ok=true; TorrentMetainfo::parse(bytes, &ok); QVERIFY(!ok);
    }
    void announceList_parsedIntoTiers() {
        QByteArray pieces(20, '\x11'); // one piece hash
        QVector<QVector<QByteArray>> tiers = {
            { "http://primary/annou" },
            { "udp://backup.example:9" },
        };
        auto bytes = singleFileWithAnnounceList(/*len*/100, /*pieceLen*/100, pieces, tiers);
        bool ok = false; QString err;
        const TorrentMetainfo m = TorrentMetainfo::parse(bytes, &ok, &err);
        QVERIFY2(ok, qPrintable(err));
        QCOMPARE(m.announceList.size(), 2);              // two tiers
        QCOMPARE(m.announceList[0].size(), 1);
        QCOMPARE(m.announceList[0][0], QUrl("http://primary/annou"));
        QCOMPARE(m.announceList[1][0], QUrl("udp://backup.example:9"));
    }
    void announceList_absent_fallsBackToAnnounce() {
        QByteArray pieces(20, '\x11'); // one piece hash
        auto bytes = singleFile(/*len*/100, /*pieceLen*/100, pieces);
        bool ok = false; QString err;
        const TorrentMetainfo m = TorrentMetainfo::parse(bytes, &ok, &err);
        QVERIFY2(ok, qPrintable(err));
        QCOMPARE(m.announceList.size(), 1);
        QCOMPARE(m.announceList[0].size(), 1);
        QCOMPARE(m.announceList[0][0], QUrl("http://tracker.example/announce"));
    }
};
QTEST_MAIN(TstMetainfo)
#include "tst_metainfo.moc"
