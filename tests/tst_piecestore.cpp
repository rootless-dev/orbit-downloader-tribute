#include <QtTest>
#include <QTemporaryDir>
#include <QCryptographicHash>
#include "torrent/PieceStore.h"
#include "torrent/TorrentMetainfo.h"

// Build a 2-file metainfo directly: fileA=30, fileB=70, pieceLength=25, total=100 (4 pieces).
static TorrentMetainfo twoFileMeta() {
    TorrentMetainfo m;
    m.name = QStringLiteral("multi");
    m.pieceLength = 25;
    m.totalLength = 100;
    m.isMultiFile = true;

    FileEntry a; a.path = QStringLiteral("a.txt"); a.length = 30; a.offset = 0;
    FileEntry b; b.path = QStringLiteral("sub/b.txt"); b.length = 70; b.offset = 30;
    m.files = { a, b };

    // 4 pieces of 25 bytes each -> 4 fake 20-byte hashes (content doesn't matter
    // for the pure-mapping/selection tests that use this fixture).
    for (int i = 0; i < 4; ++i)
        m.pieceHashes.append(QByteArray(20, char('A' + i)));

    return m;
}

// Build a 1-file metainfo with real, known data and a real SHA-1 piece hash,
// so verify()/verifyOnDisk() genuinely exercise the hash comparison.
static QByteArray knownPieceBytes() {
    QByteArray data(50, '\0');
    for (int i = 0; i < data.size(); ++i)
        data[i] = char(i % 256);
    return data;
}

static TorrentMetainfo oneFileMeta(qint64 len, qint64 pieceLen, const QByteArray& knownData) {
    TorrentMetainfo m;
    m.name = QStringLiteral("single.bin");
    m.pieceLength = pieceLen;
    m.totalLength = len;
    m.isMultiFile = false;

    FileEntry f; f.path = m.name; f.length = len; f.offset = 0;
    m.files = { f };

    m.pieceHashes.append(QCryptographicHash::hash(knownData, QCryptographicHash::Sha1));
    return m;
}

class TstPieceStore : public QObject { Q_OBJECT
private slots:
    void mapsPieceSpanningTwoFiles() {
        auto m = twoFileMeta();            // fileA[0..30) fileB[30..100), pieceLen 25
        PieceStore s(m, "/tmp/x", {0,1});  // both selected
        auto r = s.regionsFor(1, 0, 25);   // piece 1 = bytes [25..50): 5 in fileA, 20 in fileB
        QCOMPARE(r.size(), 2);
        QCOMPARE(r[0].length, 5LL); QCOMPARE(r[1].length, 20LL);
        QCOMPARE(r[0].fileOffset, 25LL); QCOMPARE(r[0].bufOffset, 0LL);
        QCOMPARE(r[1].fileOffset, 0LL);  QCOMPARE(r[1].bufOffset, 5LL);
        // PieceStore holds its own copy of the metainfo, so region.file points
        // into that copy, not into the caller's `m` -- compare by content.
        QCOMPARE(r[0].file->path, QString("a.txt"));
        QCOMPARE(r[1].file->path, QString("sub/b.txt"));
    }
    void regionsForWithinSingleFile() {
        auto m = twoFileMeta();
        PieceStore s(m, "/tmp/x", {0,1});
        auto r = s.regionsFor(0, 0, 25); // piece 0 = [0..25), entirely in fileA
        QCOMPARE(r.size(), 1);
        QCOMPARE(r[0].length, 25LL);
        QCOMPARE(r[0].fileOffset, 0LL);
        QCOMPARE(r[0].bufOffset, 0LL);
        QCOMPARE(r[0].file->path, QString("a.txt"));
    }
    void lastPieceIsShort() {
        auto m = twoFileMeta(); PieceStore s(m,"/tmp/x",{0,1});
        // 100 / 25 = 4 exact pieces -> last piece (index 3) is still full-sized (25).
        QCOMPARE(s.pieceSize(3), 25LL);
    }
    void lastPieceIsShortWhenNotExact() {
        TorrentMetainfo m = twoFileMeta();
        m.totalLength = 90; // last piece short: pieces [0,25,50,75..90) -> piece3 = 15
        m.pieceHashes.resize(4);
        PieceStore s(m, "/tmp/x", {0,1});
        QCOMPARE(s.pieceSize(3), 15LL);
    }
    void shortLastPieceRegionsAndRoundTrip() {
        // Closes the composition gap between pieceSize() and
        // regionsFor()/writePiece(): a short last piece must map to (and
        // round-trip through disk as) exactly its short length, not the
        // full pieceLength.
        //
        // fileA[0..30) fileB[30..90), pieceLen 25, total 90 -> pieces of
        // 25,25,25,15 bytes. Piece 3 = [75,90) lies entirely within fileB.
        TorrentMetainfo m;
        m.name = QStringLiteral("multi");
        m.pieceLength = 25;
        m.totalLength = 90;
        m.isMultiFile = true;
        FileEntry a; a.path = QStringLiteral("a.txt"); a.length = 30; a.offset = 0;
        FileEntry b; b.path = QStringLiteral("sub/b.txt"); b.length = 60; b.offset = 30;
        m.files = { a, b };

        QByteArray whole(90, '\0');
        for (int i = 0; i < whole.size(); ++i) whole[i] = char(i);
        for (int p = 0; p < 4; ++p) {
            const qint64 sz = (p < 3) ? 25 : 15;
            m.pieceHashes.append(QCryptographicHash::hash(whole.mid(p * 25, sz), QCryptographicHash::Sha1));
        }

        QTemporaryDir dir; QVERIFY(dir.isValid());
        PieceStore s(m, dir.path(), {0, 1});

        QCOMPARE(s.pieceSize(3), 15LL);
        auto r = s.regionsFor(3, 0, s.pieceSize(3));
        QCOMPARE(r.size(), 1);
        QCOMPARE(r[0].length, 15LL);
        QCOMPARE(r[0].fileOffset, 45LL); // 15 bytes into fileB's 60-byte range (75-30)
        QCOMPARE(r[0].bufOffset, 0LL);
        QCOMPARE(r[0].file->path, QString("sub/b.txt"));

        const QByteArray piece3 = whole.mid(75, 15);
        QString e;
        QVERIFY2(s.writePiece(3, piece3, &e), qPrintable(e));
        QVERIFY2(s.verifyOnDisk(3, &e), qPrintable(e));
    }
    void selectionMarksBoundaryPieceWanted() {
        auto m = twoFileMeta(); PieceStore s(m, "/tmp/x", {1}); // only fileB selected
        QVERIFY(s.pieceIsWanted(1));   // boundary piece overlaps fileB
        QVERIFY(!s.pieceIsWanted(0));  // piece 0 = [0..25) entirely in fileA (unselected)
        QVERIFY(s.pieceIsWanted(2));
        QVERIFY(s.pieceIsWanted(3));
    }
    void wantedPiecesListsOnlyOverlapping() {
        auto m = twoFileMeta(); PieceStore s(m, "/tmp/x", {1});
        QCOMPARE(s.wantedPieces(), (QVector<int>{1, 2, 3}));
    }
    void emptySelectionWantsNothing() {
        auto m = twoFileMeta(); PieceStore s(m, "/tmp/x", {});
        QVERIFY(s.wantedPieces().isEmpty());
        QVERIFY(!s.pieceIsWanted(0));
        QVERIFY(!s.pieceIsWanted(1));
    }
    void verifyPassesOnCorrectDataAndFailsOnWrongData() {
        QByteArray data = knownPieceBytes();
        auto m = oneFileMeta(50, 50, data);
        PieceStore s(m, "/tmp/x", {0});
        QVERIFY(s.verify(0, data));
        QByteArray wrong = data;
        wrong[0] = char(wrong[0] + 1);
        QVERIFY(!s.verify(0, wrong));
    }
    void writeVerifyRoundTrip() {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        auto data = knownPieceBytes();
        auto m = oneFileMeta(/*len*/50, /*pieceLen*/50, data);
        PieceStore s(m, dir.path(), {0});
        QString e; QVERIFY2(s.writePiece(0, data, &e), qPrintable(e));
        QVERIFY(s.verify(0, data));
        QVERIFY2(s.verifyOnDisk(0, &e), qPrintable(e));

        // File should be at destDir/name for a single-file torrent.
        QVERIFY(QFile::exists(QDir(dir.path()).filePath("single.bin")));
    }
    void writeVerifyRoundTripMultiFileSpanningTwoFiles() {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        TorrentMetainfo m = twoFileMeta();
        // Replace fake hashes with real SHA-1s of the concatenated logical stream.
        QByteArray whole(100, '\0');
        for (int i = 0; i < whole.size(); ++i) whole[i] = char(i);
        m.pieceHashes.clear();
        for (int p = 0; p < 4; ++p) {
            QByteArray piece = whole.mid(p * 25, 25);
            m.pieceHashes.append(QCryptographicHash::hash(piece, QCryptographicHash::Sha1));
        }
        PieceStore s(m, dir.path(), {0, 1});
        QString e;
        for (int p = 0; p < 4; ++p) {
            QByteArray piece = whole.mid(p * 25, 25);
            QVERIFY2(s.writePiece(p, piece, &e), qPrintable(e));
        }
        for (int p = 0; p < 4; ++p) {
            QVERIFY2(s.verifyOnDisk(p, &e), qPrintable(e));
        }
        QVERIFY(QFile::exists(QDir(dir.path()).filePath("multi/a.txt")));
        QVERIFY(QFile::exists(QDir(dir.path()).filePath("multi/sub/b.txt")));

        QFile fa(QDir(dir.path()).filePath("multi/a.txt"));
        QVERIFY(fa.open(QIODevice::ReadOnly));
        QCOMPARE(fa.readAll(), whole.mid(0, 30));

        QFile fb(QDir(dir.path()).filePath("multi/sub/b.txt"));
        QVERIFY(fb.open(QIODevice::ReadOnly));
        QCOMPARE(fb.readAll(), whole.mid(30, 70));
    }
    void untouchedFilesAreNeverCreated() {
        // A file that no *written* piece's regions ever touch must never be
        // created on disk (no preallocation). Piece 0 = [0,25) lies entirely
        // within fileA, so writing only piece 0 must never create fileB.
        QTemporaryDir dir; QVERIFY(dir.isValid());
        TorrentMetainfo m = twoFileMeta();
        PieceStore s(m, dir.path(), {0, 1});
        QByteArray piece0(25, '\0');
        QString e; QVERIFY2(s.writePiece(0, piece0, &e), qPrintable(e));
        QVERIFY(QFile::exists(QDir(dir.path()).filePath("multi/a.txt")));
        QVERIFY(!QFile::exists(QDir(dir.path()).filePath("multi/sub/b.txt")));
    }
};
QTEST_MAIN(TstPieceStore)
#include "tst_piecestore.moc"
