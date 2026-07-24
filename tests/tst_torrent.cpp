#include <QtTest>
#include <QTemporaryDir>
#include <QCryptographicHash>
#include <QNetworkAccessManager>
#include <QFile>
#include <QDir>
#include <QUuid>

#include "torrent/TorrentTask.h"
#include "torrent/TorrentMetainfo.h"
#include "torrent/Bencode.h"
#include "RateLimiter.h"
#include "TestSeeder.h"
#include "DownloadManager.h"

namespace {

// Deterministic, per-byte-varying payload so every piece gets a distinct
// SHA-1 (the seeder serves this exact stream; the task verifies each piece
// against the hashes computed below).
QByteArray makeData(int n) {
    QByteArray d(n, '\0');
    for (int i = 0; i < n; ++i) d[i] = char((i * 131 + 17) & 0xFF);
    return d;
}

void appendPieceHashes(TorrentMetainfo& m, const QByteArray& data) {
    const int pc = int((data.size() + m.pieceLength - 1) / m.pieceLength);
    for (int p = 0; p < pc; ++p) {
        const qint64 off = qint64(p) * m.pieceLength;
        const qint64 sz = qMin<qint64>(m.pieceLength, data.size() - off);
        m.pieceHashes.append(QCryptographicHash::hash(data.mid(off, sz), QCryptographicHash::Sha1));
    }
}

// Single-file torrent over `data`. infoHash is arbitrary-but-stable 20 bytes;
// only the piece hashes must be genuine SHA-1s of the data.
TorrentMetainfo singleMeta(const QByteArray& data, qint64 pieceLen) {
    TorrentMetainfo m;
    m.name = QStringLiteral("single.bin");
    m.pieceLength = pieceLen;
    m.totalLength = data.size();
    m.isMultiFile = false;
    FileEntry f; f.path = m.name; f.length = data.size(); f.offset = 0;
    m.files = { f };
    appendPieceHashes(m, data);
    m.infoHash = QCryptographicHash::hash(data + "single", QCryptographicHash::Sha1);
    return m;
}

// Two-file torrent whose files are piece-aligned: fileA = aPieces pieces,
// fileB = the rest. `data` is the concatenated logical stream.
TorrentMetainfo twoFileMeta(const QByteArray& data, qint64 pieceLen, qint64 aLen) {
    TorrentMetainfo m;
    m.name = QStringLiteral("bundle");
    m.pieceLength = pieceLen;
    m.totalLength = data.size();
    m.isMultiFile = true;
    FileEntry a; a.path = QStringLiteral("a.bin"); a.length = aLen;              a.offset = 0;
    FileEntry b; b.path = QStringLiteral("b.bin"); b.length = data.size() - aLen; b.offset = aLen;
    m.files = { a, b };
    appendPieceHashes(m, data);
    m.infoHash = QCryptographicHash::hash(data + "bundle", QCryptographicHash::Sha1);
    return m;
}

QSet<int> allFiles(const TorrentMetainfo& m) {
    QSet<int> s;
    for (int i = 0; i < m.files.size(); ++i) s.insert(i);
    return s;
}

QByteArray readFile(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return QByteArray();
    return f.readAll();
}

int haveCount(const TorrentTask& t, int pieceCount) {
    int n = 0;
    for (int i = 0; i < pieceCount; ++i)
        if (t.pieceState(i) == PieceState::Have) ++n;
    return n;
}

// --- DownloadManager::addTorrent fixtures -----------------------------------

bool writeBytes(const QString& path, const QByteArray& bytes) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return false;
    return f.write(bytes) == bytes.size();
}

// Builds a minimal-but-valid single-file .torrent (structurally valid bencode,
// one piece), writes it to <dir>/<fileName>, and returns its path. Also
// reports the hex info-hash TorrentMetainfo::parse will compute for it (SHA-1
// over the canonically-encoded info dict — Bencode::encode is deterministic,
// so hashing the info dict alone yields the same bytes it occupies inside the
// full encoding). `length` is a discriminator: two calls with different
// lengths produce two distinct info-hashes (distinct torrents), which a test
// needs when it must add a SECOND, unrelated torrent (dedup is by info-hash,
// so re-using the same fixture would just collapse into the original task).
QString writeTorrentFixture(const QString& dir, QString* hexInfoHashOut = nullptr,
                            qint64 length = 100, const QString& fileName = QStringLiteral("fixture.torrent")) {
    QMap<QByteArray, BencodeValue> info;
    info.insert("length", BencodeValue::makeInt(length));
    info.insert("name", BencodeValue::makeBytes("single.bin"));
    info.insert("piece length", BencodeValue::makeInt(length));
    info.insert("pieces", BencodeValue::makeBytes(QByteArray(20, '\x11')));

    QMap<QByteArray, BencodeValue> root;
    root.insert("announce", BencodeValue::makeBytes("http://tracker.example/announce"));
    root.insert("info", BencodeValue::makeDict(info));

    const QByteArray infoBytes = Bencode::encode(BencodeValue::makeDict(info));
    if (hexInfoHashOut)
        *hexInfoHashOut = QCryptographicHash::hash(infoBytes, QCryptographicHash::Sha1).toHex();

    const QByteArray bytes = Bencode::encode(BencodeValue::makeDict(root));
    const QString path = dir + "/" + fileName;
    writeBytes(path, bytes);
    return path;
}

} // namespace

class TstTorrent : public QObject {
    Q_OBJECT
private slots:
    void downloadsSingleFileFromSeeder() {
        const QByteArray data = makeData(50000);         // 4 pieces @ 16384
        auto m = singleMeta(data, 16384);
        TestSeeder seeder(m.infoHash, data, 16384);
        QTemporaryDir dir; QVERIFY(dir.isValid());
        QNetworkAccessManager nam; RateLimiter rl;
        TorrentTask t(m, dir.path(), allFiles(m), PieceStrategy::RarestFirst, 6881, 50,
                      ResumeVerifyMode::TrustBitfield, 1, &nam, &rl, nullptr, dir.path());
        t.addPeerForTest({QStringLiteral("127.0.0.1"), seeder.port()});
        t.start();
        // Diagnostics getters (part A): while leeching, the injected seeder
        // shows up as a connected, unchoked peer. The seeder is on
        // loopback and the fixture is tiny, so the download can finish
        // before this poll's next tick — accept "already Completed" too
        // (peers are torn down on completion, which would otherwise flake
        // this assertion) since reaching Completed is only possible via the
        // unchoked path anyway.
        QTRY_VERIFY_WITH_TIMEOUT(t.connectedPeerCount() > 0, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(t.unchokedPeerCount() > 0 ||
                                 t.state() == DownloadState::Completed, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(t.state() == DownloadState::Completed, 10000);
        QCOMPARE(readFile(QDir(dir.path()).filePath(m.name)), data);
        QCOMPARE(t.receivedBytes(), t.totalBytes());
    }

    // Regression for the multi-block-piece completion hang: when pieceLength is
    // a MULTIPLE of the 16 KiB block size, each piece has several blocks. The
    // old picker re-offered already-RECEIVED blocks forever and never advanced
    // to the un-received ones, so such pieces never completed and the torrent
    // never finished. Every other E2E test uses pieceLength == blockSize (one
    // block/piece), so this path was never exercised. The bug only bites when a
    // piece has MORE blocks than the per-peer pipeline depth (8): with <=8
    // blocks the first pick() grabs the whole piece in one batch, masking the
    // re-pick loop. Here pieceLength = 16*16384 = 262144 (16 blocks/piece) as in
    // the confirmed wire-trace, 2 full pieces + a short final piece whose last
    // block is partial. Must complete and be byte-identical.
    void downloadsMultiBlockPieces() {
        const qint64 pl = 16 * 16384;                      // 262144: 16 blocks/piece (> pipeline depth 8)
        const QByteArray data = makeData(int(2 * pl + 40000)); // 2 full pieces + short last (3 blocks, last partial)
        auto m = singleMeta(data, pl);
        QCOMPARE(m.pieceHashes.size(), 3);                 // 2 full + 1 short (40000 bytes)
        TestSeeder seeder(m.infoHash, data, pl);
        QTemporaryDir dir; QVERIFY(dir.isValid());
        QNetworkAccessManager nam; RateLimiter rl;
        TorrentTask t(m, dir.path(), allFiles(m), PieceStrategy::Sequential, 6881, 50,
                      ResumeVerifyMode::TrustBitfield, 1, &nam, &rl, nullptr, dir.path());
        t.addPeerForTest({QStringLiteral("127.0.0.1"), seeder.port()});
        t.start();
        QTRY_VERIFY_WITH_TIMEOUT(t.state() == DownloadState::Completed, 10000);
        QCOMPARE(readFile(QDir(dir.path()).filePath(m.name)), data);
        QCOMPARE(t.receivedBytes(), t.totalBytes());
    }

    void nextAnnounceDelaySecsAdaptsToPeerHealth() {
        // Starved: 0 peers, no progress -> aggressive re-announce, bounded [30,90].
        // No min interval (0) reproduces the old, pre-fast-follow behavior.
        int d = TorrentTask::nextAnnounceDelaySecsForTest(0, false, 1800, 0);
        QVERIFY(d >= 30 && d <= 90);

        // Healthy: enough peers AND real progress -> back off to the tracker's
        // own interval verbatim.
        QCOMPARE(TorrentTask::nextAnnounceDelaySecsForTest(4, true, 1800, 0), 1800);
        QCOMPARE(TorrentTask::nextAnnounceDelaySecsForTest(10, true, 900, 0), 900);

        // Floor respected: even a tiny tracker interval never goes below 30s
        // while starved.
        QCOMPARE(TorrentTask::nextAnnounceDelaySecsForTest(0, false, 5, 0), 30);

        // Peers alone (no progress) still counts as starved.
        d = TorrentTask::nextAnnounceDelaySecsForTest(10, false, 1800, 0);
        QVERIFY(d >= 30 && d <= 90);

        // Progress alone (too few peers) still counts as starved.
        d = TorrentTask::nextAnnounceDelaySecsForTest(1, true, 1800, 0);
        QVERIFY(d >= 30 && d <= 90);

        // trackerIntervalSecs<=0 fallback (no interval known yet): clamps the
        // 1800s default into the starved ceiling.
        QCOMPARE(TorrentTask::nextAnnounceDelaySecsForTest(0, false, 0, 0), 90);
        // ...and passes it through unclamped once healthy.
        QCOMPARE(TorrentTask::nextAnnounceDelaySecsForTest(4, true, 0, 0), 1800);

        // --- min-interval fast-follow: a tracker's advertised floor must
        // never be violated, even while starved. ---

        // (a) minInterval=0 (tracker never sent one) -> unchanged 30-90 behavior.
        d = TorrentTask::nextAnnounceDelaySecsForTest(0, false, 1800, 0);
        QVERIFY(d >= 30 && d <= 90);

        // (b) minInterval=300, starved -> never faster than 300s (the tracker's
        // floor exceeds our usual 90s starved ceiling, so it wins).
        QCOMPARE(TorrentTask::nextAnnounceDelaySecsForTest(0, false, 1800, 300), 300);
        QCOMPARE(TorrentTask::nextAnnounceDelaySecsForTest(1, false, 900, 300), 300);

        // (c) minInterval=300 but healthy (enough peers + progress) -> the full
        // tracker interval still wins verbatim; the min-interval floor only
        // matters for the aggressive/starved branch.
        QCOMPARE(TorrentTask::nextAnnounceDelaySecsForTest(4, true, 1800, 300), 1800);
    }

    void downloadsMultiFileWithSelection() {
        // fileA = 2 pieces (all in A), fileB = 3 pieces (all in B); select B only.
        const qint64 pl = 16384;
        const qint64 aLen = 2 * pl;                       // 32768
        const QByteArray data = makeData(int(aLen + 3 * pl)); // 81920, 5 pieces
        auto m = twoFileMeta(data, pl, aLen);
        TestSeeder seeder(m.infoHash, data, pl);
        QTemporaryDir dir; QVERIFY(dir.isValid());
        QNetworkAccessManager nam; RateLimiter rl;
        TorrentTask t(m, dir.path(), {1}, PieceStrategy::Sequential, 6881, 50,
                      ResumeVerifyMode::TrustBitfield, 1, &nam, &rl, nullptr, dir.path());
        t.addPeerForTest({QStringLiteral("127.0.0.1"), seeder.port()});
        t.start();
        QTRY_VERIFY_WITH_TIMEOUT(t.state() == DownloadState::Completed, 10000);

        // Selected file B is present and byte-identical.
        const QString bPath = QDir(dir.path()).filePath(QStringLiteral("bundle/b.bin"));
        QCOMPARE(readFile(bPath), data.mid(aLen, data.size() - aLen));
        // Pieces entirely inside unselected file A were never requested/written.
        QVERIFY(!QFile::exists(QDir(dir.path()).filePath(QStringLiteral("bundle/a.bin"))));
        QCOMPARE(t.pieceState(0), PieceState::Missing);
        QCOMPARE(t.pieceState(1), PieceState::Missing);
    }

    void resumesAfterRestart() {
        const QByteArray data = makeData(50000);          // 4 pieces
        const int pieceCount = 4;
        auto m = singleMeta(data, 16384);
        TestSeeder seeder(m.infoHash, data, 16384);
        QTemporaryDir dir; QVERIFY(dir.isValid());
        QNetworkAccessManager nam;

        // First run: throttle so we can catch it mid-flight, stop after >=1 piece.
        {
            RateLimiter rl; rl.setRate(24000);            // ~24 KB/s
            auto* t1 = new TorrentTask(m, dir.path(), allFiles(m), PieceStrategy::Sequential,
                                       6881, 50, ResumeVerifyMode::TrustBitfield, 1,
                                       &nam, &rl, nullptr, dir.path());
            t1->addPeerForTest({QStringLiteral("127.0.0.1"), seeder.port()});
            t1->start();
            QTRY_VERIFY_WITH_TIMEOUT(haveCount(*t1, pieceCount) >= 1, 10000);
            const int got = haveCount(*t1, pieceCount);
            QVERIFY(got < pieceCount || t1->state() == DownloadState::Completed);
            delete t1;                                    // dtor persists the bitfield
        }

        // Bitfield file must exist on disk for the resume.
        QVERIFY(QFile::exists(QDir(dir.path()).filePath(m.infoHash.toHex() + ".bitfield")));

        // Second run over the same dest + resume dir: adopts prior progress and
        // downloads ONLY the missing pieces (already-have pieces must never be
        // re-requested).
        {
            RateLimiter rl2;
            TorrentTask t2(m, dir.path(), allFiles(m), PieceStrategy::Sequential,
                           6881, 50, ResumeVerifyMode::TrustBitfield, 1,
                           &nam, &rl2, nullptr, dir.path());

            QSet<int> restoredHave;  // pieces adopted from the resume file
            QSet<int> wentInFlight;  // pieces that were (re)requested in run 2
            connect(&t2, &TorrentTask::pieceStateChanged, &t2,
                    [&](int piece, PieceState st) {
                        if (st == PieceState::InFlight) wentInFlight.insert(piece);
                    });

            t2.addPeerForTest({QStringLiteral("127.0.0.1"), seeder.port()});
            t2.start();
            // restoreBitfield() emits its Have transitions synchronously inside
            // start(), before any peer I/O — snapshot them now.
            for (int i = 0; i < pieceCount; ++i)
                if (t2.pieceState(i) == PieceState::Have) restoredHave.insert(i);

            QTRY_VERIFY_WITH_TIMEOUT(t2.state() == DownloadState::Completed, 10000);

            // Prove "only missing pieces downloaded": no restored-have piece was
            // ever put back in flight, and the download really did resume from
            // saved progress (at least one piece was adopted, fewer than all).
            QVERIFY(!restoredHave.isEmpty());
            QVERIFY(restoredHave.size() < pieceCount);
            for (int p : restoredHave)
                QVERIFY2(!wentInFlight.contains(p),
                         qPrintable(QStringLiteral("already-have piece %1 was re-requested").arg(p)));
            QCOMPARE(wentInFlight.size(), pieceCount - restoredHave.size());
        }
        QCOMPARE(readFile(QDir(dir.path()).filePath(m.name)), data);
    }

    // Regression for the in-flight-leak / completion-hang when a peer is dropped
    // after repeated bad pieces. A second seeder is given the REAL info-hash but
    // CORRUPTED data, so every block it serves fails verification; after
    // kMaxBadPieces the engine must drop it, release its in-flight blocks back
    // to the picker, and finish via the good seeder. Without the fix (the
    // dropped peer's blocks stay marked in-flight forever) this hangs and times
    // out.
    void droppedBadPeerReleasesInflightAndCompletes() {
        const QByteArray data = makeData(200000);         // 13 pieces @ 16384
        auto m = singleMeta(data, 16384);
        QCOMPARE(m.pieceHashes.size(), 13);

        QByteArray garbage = data;
        for (int i = 0; i < garbage.size(); ++i) garbage[i] = char(~garbage[i]); // every piece wrong

        TestSeeder bad(m.infoHash, garbage, 16384);
        TestSeeder good(m.infoHash, data, 16384);
        QTemporaryDir dir; QVERIFY(dir.isValid());
        QNetworkAccessManager nam; RateLimiter rl;
        TorrentTask t(m, dir.path(), allFiles(m), PieceStrategy::Sequential, 6881, 50,
                      ResumeVerifyMode::TrustBitfield, 1, &nam, &rl, nullptr, dir.path());
        // Inject the bad peer first so it grabs a full pipeline of in-flight
        // blocks before the good one.
        t.addPeerForTest({QStringLiteral("127.0.0.1"), bad.port()});
        t.addPeerForTest({QStringLiteral("127.0.0.1"), good.port()});
        t.start();
        QTRY_VERIFY_WITH_TIMEOUT(t.state() == DownloadState::Completed, 10000);
        QCOMPARE(readFile(QDir(dir.path()).filePath(m.name)), data);
    }

    void forceRecheckRebuildsBitfield() {
        const QByteArray data = makeData(50000);
        auto m = singleMeta(data, 16384);
        QTemporaryDir dir; QVERIFY(dir.isValid());
        QNetworkAccessManager nam;

        // Complete a full download first.
        {
            TestSeeder seeder(m.infoHash, data, 16384);
            RateLimiter rl;
            TorrentTask t(m, dir.path(), allFiles(m), PieceStrategy::RarestFirst,
                          6881, 50, ResumeVerifyMode::TrustBitfield, 1,
                          &nam, &rl, nullptr, dir.path());
            t.addPeerForTest({QStringLiteral("127.0.0.1"), seeder.port()});
            t.start();
            QTRY_VERIFY_WITH_TIMEOUT(t.state() == DownloadState::Completed, 10000);
        }

        // Fresh task, no seeder: forceRecheck() must rebuild the bitfield from
        // the on-disk data (every piece verifies) and reach Completed without
        // any peer/download.
        RateLimiter rl2;
        TorrentTask t2(m, dir.path(), allFiles(m), PieceStrategy::RarestFirst,
                       6881, 50, ResumeVerifyMode::TrustBitfield, 1,
                       &nam, &rl2, nullptr, dir.path());
        t2.forceRecheck();
        t2.start();
        QTRY_VERIFY_WITH_TIMEOUT(t2.state() == DownloadState::Completed, 10000);
        QCOMPARE(t2.receivedBytes(), t2.totalBytes());
        QCOMPARE(readFile(QDir(dir.path()).filePath(m.name)), data);
    }

    // FINDING 1: RecheckOnOpen must be AUTHORITATIVE, not additive — a piece
    // that is present in the restored bitfield but CORRUPT on disk must be
    // downgraded and re-fetched, not left silently "complete". Under the old
    // additive adoptHaveBitfield() this test fails (the task stays Completed
    // with corrupt bytes); after the fix runChecking() rebuilds the have-set
    // from disk and the seeder repairs the piece.
    void recheckOnOpenReDownloadsCorruptPiece() {
        const QByteArray data = makeData(50000);          // 4 pieces @ 16384
        auto m = singleMeta(data, 16384);
        QTemporaryDir dir; QVERIFY(dir.isValid());
        QNetworkAccessManager nam;

        // 1) Complete a full download to disk (payload + resume bitfield).
        {
            TestSeeder seeder(m.infoHash, data, 16384);
            RateLimiter rl;
            TorrentTask t(m, dir.path(), allFiles(m), PieceStrategy::Sequential,
                          6881, 50, ResumeVerifyMode::TrustBitfield, 1,
                          &nam, &rl, nullptr, dir.path());
            t.addPeerForTest({QStringLiteral("127.0.0.1"), seeder.port()});
            t.start();
            QTRY_VERIFY_WITH_TIMEOUT(t.state() == DownloadState::Completed, 10000);
        }
        const QString bitfield = QDir(dir.path()).filePath(m.infoHash.toHex() + ".bitfield");
        QVERIFY(QFile::exists(bitfield));   // marks ALL pieces present

        // 2) Corrupt one piece's bytes on disk (piece 1).
        const QString payload = QDir(dir.path()).filePath(m.name);
        {
            QFile f(payload);
            QVERIFY(f.open(QIODevice::ReadWrite));
            QVERIFY(f.seek(16384 + 5));     // inside piece 1
            f.write(QByteArray(64, '\xEE'));
            f.close();
        }
        QVERIFY(readFile(payload) != data); // corruption confirmed

        // 3) New task over the same dir/resumeDir with RecheckOnOpen; restore
        //    the (still-all-present) bitfield, then start(). The corrupt piece
        //    must be re-fetched and the final file must be byte-correct.
        TestSeeder seeder(m.infoHash, data, 16384);
        RateLimiter rl2;
        TorrentTask t2(m, dir.path(), allFiles(m), PieceStrategy::Sequential,
                       6881, 50, ResumeVerifyMode::RecheckOnOpen, 1,
                       &nam, &rl2, nullptr, dir.path());
        t2.restoreBitfield();               // still marks piece 1 as present
        t2.addPeerForTest({QStringLiteral("127.0.0.1"), seeder.port()});
        t2.start();
        QTRY_VERIFY_WITH_TIMEOUT(t2.state() == DownloadState::Completed, 10000);
        QCOMPARE(readFile(payload), data);
        QCOMPARE(t2.receivedBytes(), t2.totalBytes());
    }

    // FINDING 2: Force re-check must actually RUN a check regardless of state —
    // in particular on a Completed torrent, where it was previously a pure
    // no-op (a flag only consumed by start(), which early-returns on Completed).
    void forceRecheckRunsCheckAndRefetchesFromCompleted() {
        const QByteArray data = makeData(50000);          // 4 pieces
        auto m = singleMeta(data, 16384);
        QTemporaryDir dir; QVERIFY(dir.isValid());
        QNetworkAccessManager nam;

        TestSeeder seeder(m.infoHash, data, 16384);
        RateLimiter rl;
        TorrentTask t(m, dir.path(), allFiles(m), PieceStrategy::Sequential,
                      6881, 50, ResumeVerifyMode::TrustBitfield, 1,
                      &nam, &rl, nullptr, dir.path());
        t.addPeerForTest({QStringLiteral("127.0.0.1"), seeder.port()});
        t.start();
        QTRY_VERIFY_WITH_TIMEOUT(t.state() == DownloadState::Completed, 10000);

        // (a) forceRecheck() on an intact, Completed torrent transitions THROUGH
        //     Checking and settles back to Completed, data intact.
        {
            QVector<DownloadState> seen;
            connect(&t, &TorrentTask::stateChanged, &t,
                    [&](DownloadState s) { seen.append(s); });
            t.forceRecheck();
            QTRY_VERIFY_WITH_TIMEOUT(t.state() == DownloadState::Completed, 10000);
            QVERIFY(seen.contains(DownloadState::Checking));
            QCOMPARE(readFile(QDir(dir.path()).filePath(m.name)), data);
            disconnect(&t, &TorrentTask::stateChanged, &t, nullptr);
        }

        // (b) Combined with Finding 1: corrupt a piece on disk, then
        //     forceRecheck() must downgrade + re-fetch it. Use a fresh seeder on
        //     its own port so the (deduped) injected peer is genuinely re-queued.
        const QString payload = QDir(dir.path()).filePath(m.name);
        {
            QFile f(payload);
            QVERIFY(f.open(QIODevice::ReadWrite));
            QVERIFY(f.seek(16384 * 2 + 3)); // inside piece 2
            f.write(QByteArray(50, '\xAB'));
            f.close();
        }
        QVERIFY(readFile(payload) != data);

        TestSeeder seeder2(m.infoHash, data, 16384);
        t.addPeerForTest({QStringLiteral("127.0.0.1"), seeder2.port()});
        t.forceRecheck();
        QTRY_VERIFY_WITH_TIMEOUT(t.state() == DownloadState::Completed, 10000);
        QCOMPARE(readFile(payload), data);
        QCOMPARE(t.receivedBytes(), t.totalBytes());
    }

    // --- DownloadManager::addTorrent + session persistence (Task 11) -------

    void addTorrentCreatesRowAndCopiesFile() {
        QTemporaryDir data; QVERIFY(data.isValid());
        DownloadManager mgr(EngineConfig{}, data.path());
        QString hexInfoHash;
        QString tpath = writeTorrentFixture(data.path(), &hexInfoHash);
        auto id = mgr.addTorrent(tpath, data.path(), {0}, PieceStrategy::RarestFirst);
        QVERIFY(!id.isNull());
        QCOMPARE(mgr.tasks().size(), 1);
        QCOMPARE(mgr.taskById(id)->kind(), AbstractTask::Kind::Torrent);
        QVERIFY(QFile::exists(data.path() + "/torrents/" + hexInfoHash + ".torrent"));
    }

    void rejectsMalformedTorrent() {
        QTemporaryDir data; QVERIFY(data.isValid());
        DownloadManager mgr(EngineConfig{}, data.path());
        QString bad = data.path() + "/bad.torrent";
        QVERIFY(writeBytes(bad, "not bencode"));
        QVERIFY(mgr.addTorrent(bad, data.path(), {}, PieceStrategy::Sequential).isNull());
        QCOMPARE(mgr.tasks().size(), 0);
    }

    void addTorrentDedupsSameInfoHash() {
        QTemporaryDir data; QVERIFY(data.isValid());
        DownloadManager mgr(EngineConfig{}, data.path());
        QString tpath = writeTorrentFixture(data.path());
        auto id1 = mgr.addTorrent(tpath, data.path(), {0}, PieceStrategy::RarestFirst);
        auto id2 = mgr.addTorrent(tpath, data.path(), {0}, PieceStrategy::Sequential);
        QVERIFY(!id1.isNull());
        QCOMPARE(id2, id1);
        QCOMPARE(mgr.tasks().size(), 1);
    }

    // FINDING 3: remove(id, deleteFiles=true) on a torrent must delete the
    // payload root AND the app-internal <dataDir>/torrents/<hex>.{torrent,
    // bitfield} artifacts (previously all no-ops: the DownloadTask cast was
    // null, so nothing was deleted and the artifacts orphaned forever).
    void removeTorrentWithDeleteFilesRemovesPayloadAndArtifacts() {
        QTemporaryDir data; QVERIFY(data.isValid());
        DownloadManager mgr(EngineConfig{}, data.path());
        QString hex;
        QString tpath = writeTorrentFixture(data.path(), &hex);
        const QString destDir = data.path() + "/dl";       // keep payload out of the data root
        QVERIFY(QDir().mkpath(destDir));
        auto id = mgr.addTorrent(tpath, destDir, {0}, PieceStrategy::RarestFirst);
        QVERIFY(!id.isNull());

        const QString storedTorrent  = data.path() + "/torrents/" + hex + ".torrent";
        const QString storedBitfield = data.path() + "/torrents/" + hex + ".bitfield";
        const QString payload        = destDir + "/single.bin";   // single-file payload root
        QVERIFY(QFile::exists(storedTorrent));

        // Simulate on-disk progress: a written payload piece + a resume bitfield.
        QVERIFY(writeBytes(payload, QByteArray(100, '\x22')));
        QVERIFY(writeBytes(storedBitfield, QByteArray(8, '\0')));

        mgr.remove(id, /*deleteFiles=*/true);

        QVERIFY(!QFile::exists(payload));         // user payload deleted
        QVERIFY(!QFile::exists(storedTorrent));   // internal .torrent deleted
        QVERIFY(!QFile::exists(storedBitfield));  // internal .bitfield deleted
        QCOMPARE(mgr.tasks().size(), 0);
    }

    // FINDING 3 (keep-files semantics): remove(deleteFiles=false) keeps the
    // user payload but STILL cleans the app-internal .torrent/.bitfield.
    void removeTorrentKeepFilesStillCleansInternalArtifacts() {
        QTemporaryDir data; QVERIFY(data.isValid());
        DownloadManager mgr(EngineConfig{}, data.path());
        QString hex;
        QString tpath = writeTorrentFixture(data.path(), &hex);
        const QString destDir = data.path() + "/dl";
        QVERIFY(QDir().mkpath(destDir));
        auto id = mgr.addTorrent(tpath, destDir, {0}, PieceStrategy::RarestFirst);
        QVERIFY(!id.isNull());

        const QString storedTorrent  = data.path() + "/torrents/" + hex + ".torrent";
        const QString storedBitfield = data.path() + "/torrents/" + hex + ".bitfield";
        const QString payload        = destDir + "/single.bin";
        QVERIFY(writeBytes(payload, QByteArray(100, '\x22')));
        QVERIFY(writeBytes(storedBitfield, QByteArray(8, '\0')));

        mgr.remove(id, /*deleteFiles=*/false);

        QVERIFY(QFile::exists(payload));          // user payload kept
        QVERIFY(!QFile::exists(storedTorrent));   // internal artifacts cleaned anyway
        QVERIFY(!QFile::exists(storedBitfield));
        QCOMPARE(mgr.tasks().size(), 0);
    }

    // Task 15: Preferences' maxPeersPerTorrent/listenPort/verify are plain
    // values threaded through DownloadManager::setTorrentDefaults() into every
    // TorrentTask makeTorrentTask() builds afterwards — not dead knobs.
    void setTorrentDefaultsReachesNewTorrentTask() {
        QTemporaryDir data; QVERIFY(data.isValid());
        DownloadManager mgr(EngineConfig{}, data.path());
        mgr.setTorrentDefaults(80, 51413, ResumeVerifyMode::RecheckOnOpen);
        QString tpath = writeTorrentFixture(data.path());
        auto id = mgr.addTorrent(tpath, data.path(), {0}, PieceStrategy::RarestFirst);
        QVERIFY(!id.isNull());
        auto* t = qobject_cast<TorrentTask*>(mgr.taskById(id));
        QVERIFY(t);
        QCOMPARE(t->maxPeers(), 80);
        QCOMPARE(t->listenPort(), quint16(51413));
        QCOMPARE(t->verifyMode(), ResumeVerifyMode::RecheckOnOpen);
    }

    void sessionRoundTripsTorrent() {
        QTemporaryDir data; QVERIFY(data.isValid());
        QUuid id;
        {
            DownloadManager mgr(EngineConfig{}, data.path());
            QString tpath = writeTorrentFixture(data.path());
            id = mgr.addTorrent(tpath, data.path(), {0}, PieceStrategy::Sequential);
            QVERIFY(!id.isNull());
        }
        DownloadManager mgr2(EngineConfig{}, data.path());
        mgr2.loadSession();
        QCOMPARE(mgr2.tasks().size(), 1);
        auto* t = mgr2.taskById(id);
        QVERIFY(t != nullptr);
        QCOMPARE(t->kind(), AbstractTask::Kind::Torrent);
        auto* tt = qobject_cast<TorrentTask*>(t);
        QVERIFY(tt != nullptr);
        QCOMPARE(tt->metainfo().name, QString("single.bin"));
    }

    // Regression: a torrent explicitly paused (or cancelled) before shutdown
    // must NOT silently reactivate on the next loadSession() - pump() (fired
    // by any subsequent add/remove/resumeAll/setConfig) promotes any Queued
    // task, so restoring every reloaded torrent at the constructor default
    // (Queued) would auto-resume something the user deliberately stopped.
    void sessionRestoresPausedTorrentAsPaused() {
        QTemporaryDir data; QVERIFY(data.isValid());
        QUuid id;
        {
            DownloadManager mgr(EngineConfig{}, data.path());
            QString tpath = writeTorrentFixture(data.path());
            id = mgr.addTorrent(tpath, data.path(), {0}, PieceStrategy::Sequential);
            QVERIFY(!id.isNull());
            mgr.pause(id);                          // user explicitly pauses it
            QCOMPARE(mgr.taskById(id)->state(), DownloadState::Paused);
        }                                            // mgr destroyed: session already saved by pause()

        DownloadManager mgr2(EngineConfig{}, data.path());
        mgr2.loadSession();
        QCOMPARE(mgr2.tasks().size(), 1);
        auto* t = mgr2.taskById(id);
        QVERIFY(t != nullptr);
        QCOMPARE(t->state(), DownloadState::Paused);

        // A pump()-triggering call (adding a second, UNRELATED torrent - a
        // different info-hash, so it doesn't just dedup into the same task)
        // must not flip the restored task back to Connecting/Downloading.
        QString tpath2 = writeTorrentFixture(data.path(), nullptr, 200, QStringLiteral("fixture2.torrent"));
        auto id2 = mgr2.addTorrent(tpath2, data.path(), {0}, PieceStrategy::Sequential);
        QVERIFY(!id2.isNull());
        QVERIFY(id2 != id);
        QCOMPARE(mgr2.taskById(id)->state(), DownloadState::Paused);
    }
};

QTEST_GUILESS_MAIN(TstTorrent)
#include "tst_torrent.moc"
