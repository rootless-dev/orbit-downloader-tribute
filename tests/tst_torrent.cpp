#include <QtTest>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QCryptographicHash>
#include <QNetworkAccessManager>
#include <QFile>
#include <QDir>
#include <QUrl>
#include <QUuid>

#include "torrent/TorrentTask.h"
#include "torrent/TorrentMetainfo.h"
#include "torrent/Bencode.h"
#include "torrent/DhtNode.h"
#include "RateLimiter.h"
#include "TestSeeder.h"
#include "TestUdpTracker.h"
#include "TestMetadataPeer.h"
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

// Bencodes a minimal-but-valid single-file BEP3 info dict for `m` (piece
// length/pieces/name/length only - no "files" list, no extra keys), key order
// ascending (QMap<QByteArray,...> already sorts that way), which is exactly
// canonical bencode dict order. Used by singleMeta() below so its `.infoHash`
// is the REAL SHA-1 of `.infoDict`'s bytes - required for
// magnetResolvesViaDhtThenDownloads, where TestMetadataPeer serves .infoDict
// and PeerConnection's own ut_metadata fetch verifies its SHA-1 against the
// magnet's requested info-hash before ever accepting it.
QByteArray encodeSingleFileInfoDict(const TorrentMetainfo& m) {
    QMap<QByteArray, BencodeValue> info;
    info.insert("length", BencodeValue::makeInt(m.totalLength));
    info.insert("name", BencodeValue::makeBytes(m.name.toUtf8()));
    info.insert("piece length", BencodeValue::makeInt(m.pieceLength));
    QByteArray pieces;
    for (const QByteArray& h : m.pieceHashes) pieces += h;
    info.insert("pieces", BencodeValue::makeBytes(pieces));
    return Bencode::encode(BencodeValue::makeDict(info));
}

// singleMeta()'s return type: a TorrentMetainfo plus the raw bencoded info
// dict bytes it was derived from (SHA1(infoDict) == infoHash) - needed by
// magnetResolvesViaDhtThenDownloads to hand a TestMetadataPeer something to
// serve. Public inheritance so every existing singleMeta() call site (which
// only ever uses the TorrentMetainfo base-class fields) keeps compiling
// unchanged.
struct MetaFixture : TorrentMetainfo {
    QByteArray infoDict;
};

// Single-file torrent over `data`. infoHash is the REAL SHA-1 of the encoded
// info dict (infoDict) - required so a metadata peer serving that exact
// infoDict for this infoHash passes PeerConnection's own SHA-1 verification;
// the piece hashes are genuine SHA-1s of the data either way.
MetaFixture singleMeta(const QByteArray& data, qint64 pieceLen) {
    MetaFixture m;
    m.name = QStringLiteral("single.bin");
    m.pieceLength = pieceLen;
    m.totalLength = data.size();
    m.isMultiFile = false;
    FileEntry f; f.path = m.name; f.length = data.size(); f.offset = 0;
    m.files = { f };
    appendPieceHashes(m, data);
    m.infoDict = encodeSingleFileInfoDict(m);
    m.infoHash = QCryptographicHash::hash(m.infoDict, QCryptographicHash::Sha1);
    return m;
}

// Regression fixture (Task 15 verbatim-cache fix): same shape as singleMeta(),
// but the info dict also carries a "private" key (BEP 27) that
// TorrentMetainfo::parse doesn't extract into any field. Used to prove
// DownloadManager caches the RESOLVED magnet's .torrent from the verbatim
// info-dict bytes (which preserve "private") rather than re-encoding from the
// parsed TorrentMetainfo (which would silently drop it and re-derive a
// DIFFERENT info-hash for the cached file).
MetaFixture singleMetaWithPrivateFlag(const QByteArray& data, qint64 pieceLen) {
    MetaFixture m;
    m.name = QStringLiteral("single.bin");
    m.pieceLength = pieceLen;
    m.totalLength = data.size();
    m.isMultiFile = false;
    FileEntry f; f.path = m.name; f.length = data.size(); f.offset = 0;
    m.files = { f };
    appendPieceHashes(m, data);

    QMap<QByteArray, BencodeValue> info;
    info.insert("length", BencodeValue::makeInt(m.totalLength));
    info.insert("name", BencodeValue::makeBytes(m.name.toUtf8()));
    info.insert("piece length", BencodeValue::makeInt(m.pieceLength));
    QByteArray pieces;
    for (const QByteArray& h : m.pieceHashes) pieces += h;
    info.insert("pieces", BencodeValue::makeBytes(pieces));
    info.insert("private", BencodeValue::makeInt(1)); // unrecognized by TorrentMetainfo::parse
    m.infoDict = Bencode::encode(BencodeValue::makeDict(info));
    m.infoHash = QCryptographicHash::hash(m.infoDict, QCryptographicHash::Sha1);
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

// Like writeTorrentFixture(), but with GENUINE piece SHA-1s over `payload`
// (writeTorrentFixture's "pieces" field is a fake, fixed 20-byte pattern --
// fine for the metadata-only tests that never actually leech, but useless for
// one that must download `payload` to completion and pass hash verification).
// The returned infoHash is TorrentMetainfo::parse's real SHA-1-of-the-encoded-
// info-dict, so it's exactly what a TestSeeder/DhtNode holder must be keyed on
// for DownloadManager::addTorrent()'s own parse of this file to line up with
// them end-to-end.
QString writeRealTorrentFixture(const QString& dir, const QByteArray& payload, qint64 pieceLen,
                                QByteArray* infoHashOut,
                                const QString& fileName = QStringLiteral("real.torrent"),
                                // TorrentMetainfo::parse requires a well-formed "announce" (spec
                                // §parse), so a .torrent fixture can never omit it -- but a caller
                                // whose test waits for the download to actually finish (spins the
                                // event loop long enough for AnnounceController to fire) should NOT
                                // default to a real, DNS-resolvable host: use a local, nothing's-
                                // listening address (fails instantly, no outbound DNS/HTTP) instead.
                                const QString& announce = QStringLiteral("http://tracker.example/announce")) {
    QByteArray pieces;
    const int pc = int((payload.size() + pieceLen - 1) / pieceLen);
    for (int p = 0; p < pc; ++p) {
        const qint64 off = qint64(p) * pieceLen;
        const qint64 sz = qMin<qint64>(pieceLen, payload.size() - off);
        pieces += QCryptographicHash::hash(payload.mid(off, sz), QCryptographicHash::Sha1);
    }

    QMap<QByteArray, BencodeValue> info;
    info.insert("length", BencodeValue::makeInt(payload.size()));
    info.insert("name", BencodeValue::makeBytes("single.bin"));
    info.insert("piece length", BencodeValue::makeInt(pieceLen));
    info.insert("pieces", BencodeValue::makeBytes(pieces));

    QMap<QByteArray, BencodeValue> root;
    root.insert("announce", BencodeValue::makeBytes(announce.toUtf8()));
    root.insert("info", BencodeValue::makeDict(info));

    const QByteArray infoBytes = Bencode::encode(BencodeValue::makeDict(info));
    if (infoHashOut)
        *infoHashOut = QCryptographicHash::hash(infoBytes, QCryptographicHash::Sha1);

    const QByteArray bytes = Bencode::encode(BencodeValue::makeDict(root));
    const QString path = dir + "/" + fileName;
    writeBytes(path, bytes);
    return path;
}

} // namespace

class TstTorrent : public QObject {
    Q_OBJECT
private slots:
    // Suite-wide, not per-test: e2e_downloadsThroughUdpTracker and
    // downloadsWithPeersFromDht both need these (loopback peers allowed;
    // DhtNode/UdpTrackerClient's internal query timeouts shortened), and
    // unlike a per-test qputenv/qScopeGuard pair, a shared env var set once
    // here can't be un-set out from under a LATER test in the same process.
    void initTestCase() {
        qputenv("ORBIT_ALLOW_LOOPBACK_PEERS", "1");
        qputenv("ORBIT_UDP_FAST_TIMEOUT", "1");
    }
    void cleanupTestCase() {
        qunsetenv("ORBIT_ALLOW_LOOPBACK_PEERS");
        qunsetenv("ORBIT_UDP_FAST_TIMEOUT");
    }

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

    // Regression for the field "stall against a remote seed" finding (spec
    // 2026-07-24 §10, root-caused 2026-07-27). A seed that advertises via BEP 6
    // <have_all> (id 14) instead of a <bitfield> frame: we never negotiate the
    // fast extension, yet real seeds send have_all anyway. Before the fix id 14
    // fell into PeerConnection's default: branch and was ignored, so the peer
    // bitfield stayed empty, bitfieldReceived never fired, and PiecePicker
    // starved -- the download stalled with a fully-available seed connected and
    // unchoked. Must download to completion, byte-identical.
    void downloadsFromHaveAllSeed() {
        const QByteArray data = makeData(50000);         // 4 pieces @ 16384
        auto m = singleMeta(data, 16384);
        TestSeeder seeder(m.infoHash, data, 16384, TestSeeder::Advertise::HaveAll);
        QTemporaryDir dir; QVERIFY(dir.isValid());
        QNetworkAccessManager nam; RateLimiter rl;
        TorrentTask t(m, dir.path(), allFiles(m), PieceStrategy::RarestFirst, 6881, 50,
                      ResumeVerifyMode::TrustBitfield, 1, &nam, &rl, nullptr, dir.path());
        t.addPeerForTest({QStringLiteral("127.0.0.1"), seeder.port()});
        t.start();
        QTRY_VERIFY_WITH_TIMEOUT(t.state() == DownloadState::Completed, 10000);
        QCOMPARE(readFile(QDir(dir.path()).filePath(m.name)), data);
        QCOMPARE(t.receivedBytes(), t.totalBytes());
    }

    // Framing robustness guard (rules out the other field hypothesis): the
    // <bitfield> frame arrives across two readyRead deliveries, as a >MTU
    // bitfield does on a real network but never on loopback. PeerConnection must
    // accumulate the partial frame in its member buffer and assemble it. Must
    // download to completion.
    void downloadsWithFragmentedBitfield() {
        const QByteArray data = makeData(50000);         // 4 pieces @ 16384
        auto m = singleMeta(data, 16384);
        TestSeeder seeder(m.infoHash, data, 16384, TestSeeder::Advertise::FragmentedBitfield);
        QTemporaryDir dir; QVERIFY(dir.isValid());
        QNetworkAccessManager nam; RateLimiter rl;
        TorrentTask t(m, dir.path(), allFiles(m), PieceStrategy::RarestFirst, 6881, 50,
                      ResumeVerifyMode::TrustBitfield, 1, &nam, &rl, nullptr, dir.path());
        t.addPeerForTest({QStringLiteral("127.0.0.1"), seeder.port()});
        t.start();
        QTRY_VERIFY_WITH_TIMEOUT(t.state() == DownloadState::Completed, 10000);
        QCOMPARE(readFile(QDir(dir.path()).filePath(m.name)), data);
        QCOMPARE(t.receivedBytes(), t.totalBytes());
    }

    // End-to-end proof of the full Sub-phase B tracker path: peers are NOT
    // injected via addPeerForTest. Instead, the torrent's announce-list points
    // at a local in-process UDP tracker (TestUdpTracker), which the real
    // AnnounceController -> UdpTrackerClient path must contact to learn about
    // the seeder. Mirrors downloadsMultiBlockPieces' fixture (same payload
    // shape, same multi-block piece length so the picker's multi-block path
    // stays covered) but swaps addPeerForTest for a real tracker round-trip.
    //
    // Loopback seam: TrackerPeers::dropBogons() strips 127.0.0.0/8 by design
    // (a real tracker reporting a loopback peer is malicious or broken), but
    // this offline E2E's only dialable peer -- TestSeeder -- binds to
    // 127.0.0.1. ORBIT_ALLOW_LOOPBACK_PEERS (see TrackerPeers.{h,cpp}) is a
    // test-only env-var seam that keeps loopback peers ONLY when set;
    // initTestCase() sets it for this whole suite. See
    // .superpowers/sdd/task-8-report.md for the original root-cause writeup.
    void e2e_downloadsThroughUdpTracker() {
        const qint64 pl = 16 * 16384;                      // 262144: 16 blocks/piece (> pipeline depth 8)
        const QByteArray data = makeData(int(2 * pl + 40000)); // 2 full pieces + short last (3 blocks, last partial)
        auto m = singleMeta(data, pl);
        QCOMPARE(m.pieceHashes.size(), 3);                 // 2 full + 1 short (40000 bytes)

        // 1) Stand up the in-process seeder for this exact payload.
        TestSeeder seeder(m.infoHash, data, pl);
        const quint16 seederPort = seeder.port();

        // 2) Stand up the UDP tracker returning the seeder as the only peer.
        TestUdpTracker tracker;
        tracker.setPeers({{QStringLiteral("127.0.0.1"), seederPort}});

        // 3) Point the torrent's announce-list at the local UDP tracker ONLY
        //    -- the only way to discover the seeder is through the real
        //    AnnounceController -> UdpTrackerClient path.
        m.announce = QUrl();
        m.announceList = {{QUrl(QStringLiteral("udp://127.0.0.1:%1").arg(tracker.port()))}};

        QTemporaryDir dir; QVERIFY(dir.isValid());
        QNetworkAccessManager nam; RateLimiter rl;
        TorrentTask t(m, dir.path(), allFiles(m), PieceStrategy::Sequential, 6881, 50,
                      ResumeVerifyMode::TrustBitfield, 1, &nam, &rl, nullptr, dir.path());
        t.start();
        QTRY_VERIFY_WITH_TIMEOUT(t.state() == DownloadState::Completed, 15000);
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

    // Wiring smoke test (Task 7): a torrent with a two-tier BEP-12
    // announceList must construct/drive an AnnounceController without
    // crashing or hanging, even though neither tracker is reachable (both
    // point at closed loopback ports). No peers will arrive; the point is
    // that the controller path is exercised end-to-end from start().
    void multiTracker_startsAndAnnouncesWithoutCrash() {
        const QByteArray data = makeData(16384); // 1 piece
        auto m = singleMeta(data, 16384);
        m.announceList = {{QUrl(QStringLiteral("udp://127.0.0.1:1"))},
                           {QUrl(QStringLiteral("http://127.0.0.1:1/announce"))}};
        QTemporaryDir dir; QVERIFY(dir.isValid());
        QNetworkAccessManager nam; RateLimiter rl;
        TorrentTask t(m, dir.path(), allFiles(m), PieceStrategy::Sequential, 6881, 50,
                      ResumeVerifyMode::TrustBitfield, 1, &nam, &rl, nullptr, dir.path());
        t.start();
        QTRY_VERIFY(t.state() == DownloadState::Connecting || t.state() == DownloadState::Checking);
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

    // Task 15: an unresolved magnet (never reached metainfoReady before the
    // process restarted) must round-trip through torrents.json the same way
    // a torrent does - same id, same destDir, restarted as a fresh
    // FetchingMetadata placeholder (never silently dropped, never
    // resurrected as some OTHER id). No DHT is wired here on purpose: this
    // proves the restore path itself (parse the persisted magnet fields,
    // re-derive the id, re-create the placeholder + kick off a new
    // MetadataFetch) independently of whether that fetch can actually
    // succeed - magnetResolvesViaDhtThenDownloads already covers the
    // full-resolution path end-to-end.
    void sessionRoundTripsUnresolvedMagnet() {
        const QByteArray data = makeData(50000);
        auto m = singleMeta(data, 16384);
        const QString magnet = QStringLiteral("magnet:?xt=urn:btih:") +
                               QString::fromLatin1(m.infoHash.toHex()) + QStringLiteral("&dn=my-torrent");

        QTemporaryDir sessionDir; QVERIFY(sessionDir.isValid());
        QTemporaryDir destDir;    QVERIFY(destDir.isValid());

        QUuid id;
        {
            DownloadManager mgr(EngineConfig{}, sessionDir.path());
            id = mgr.addMagnet(magnet, destDir.path(), PieceStrategy::RarestFirst);
            QVERIFY(!id.isNull());
            auto* t = mgr.taskById(id);
            QVERIFY(t != nullptr);
            QCOMPARE(t->state(), DownloadState::FetchingMetadata);
            QCOMPARE(t->displayName(), QStringLiteral("my-torrent"));
            QCOMPARE(t->kind(), AbstractTask::Kind::MagnetFetch);
        }   // mgr destroyed: session already saved by addMagnet() itself

        DownloadManager mgr2(EngineConfig{}, sessionDir.path());
        mgr2.loadSession();
        QCOMPARE(mgr2.tasks().size(), 1);
        auto* t2 = mgr2.taskById(id);
        QVERIFY(t2 != nullptr);
        QCOMPARE(t2->state(), DownloadState::FetchingMetadata);
        QCOMPARE(t2->displayName(), QStringLiteral("my-torrent"));

        // Restarting a second time (no resolution happened - no DHT wired)
        // must not duplicate the entry: same id, still exactly one task.
        DownloadManager mgr3(EngineConfig{}, sessionDir.path());
        mgr3.loadSession();
        QCOMPARE(mgr3.tasks().size(), 1);
        QVERIFY(mgr3.taskById(id) != nullptr);
    }

    // --- Task 14: DHT as a peer source --------------------------------------

    // End-to-end proof that TorrentTask can find peers via DHT alone, with NO
    // tracker and NO addPeerForTest: `holder` is an in-process DHT node that
    // already has the seeder stored as a peer for this info_hash (as if the
    // seeder had announced itself for real); `dht` is the task's own DHT node,
    // bootstrapped only to `holder` (never touches the real internet). Once
    // t.setDht(&dht) runs, TorrentTask must itself call dht.lookup() (from
    // beginLeeching(), since the task isn't running yet when setDht() is
    // called), receive `holder`'s peer via DhtNode::peersFound, connect to
    // TestSeeder, and complete the download byte-identically.
    void downloadsWithPeersFromDht() {
        const QByteArray data = makeData(50000);          // 4 pieces @ 16384
        auto m = singleMeta(data, 16384);
        TestSeeder seeder(m.infoHash, data, 16384);

        DhtNode holder(NodeId::fromSeed(200), 0, 200);
        QVERIFY(holder.start());
        holder.storePeerForTest(m.infoHash, QStringLiteral("127.0.0.1"), seeder.port());

        DhtNode dht(NodeId::fromSeed(1), 0, 1);
        QVERIFY(dht.start());
        dht.bootstrap({QStringLiteral("127.0.0.1:%1").arg(holder.boundPort())});

        QTemporaryDir dir; QVERIFY(dir.isValid());
        QNetworkAccessManager nam; RateLimiter rl;
        TorrentTask t(m, dir.path(), allFiles(m), PieceStrategy::RarestFirst, 6881, 50,
                      ResumeVerifyMode::TrustBitfield, 1, &nam, &rl, nullptr, dir.path());
        t.setDht(&dht);      // NO addPeerForTest -- peers must come from DHT alone
        t.start();
        QTRY_VERIFY_WITH_TIMEOUT(t.state() == DownloadState::Completed, 15000);
        QCOMPARE(readFile(QDir(dir.path()).filePath(m.name)), data);
        QCOMPARE(t.receivedBytes(), t.totalBytes());
    }

    // Same end-to-end shape as downloadsWithPeersFromDht above, but exercising
    // the MANAGER-level wiring instead of TorrentTask::setDht() directly:
    // mgr.setDhtForTest(&dht) followed by mgr.addTorrent() (which builds the
    // TorrentTask via the private makeTorrentTask(), the only call site that
    // does `t->setDht(m_dht)`). downloadsWithPeersFromDht never touches either
    // of those -- it builds a TorrentTask by hand -- so this is the only test
    // proving DownloadManager actually wires DHT into a torrent it creates.
    //
    // Uses a REAL .torrent fixture (writeRealTorrentFixture, genuine piece
    // SHA-1s over `payload`) rather than writeTorrentFixture's fake-hash one:
    // addTorrent() goes through TorrentMetainfo::parse(), which computes the
    // info-hash from the actual bencoded bytes, so the seeder/DHT holder must
    // be keyed on that same real hash for the download to find peers and
    // verify pieces at all. This lets the test go all the way to Completed,
    // not just assert a non-null dht pointer.
    void downloadsViaDownloadManagerDht() {
        const QByteArray payload = makeData(50000);       // 4 pieces @ 16384
        const qint64 pieceLen = 16384;

        QTemporaryDir data; QVERIFY(data.isValid());
        QByteArray infoHash;
        // Nothing listens on 127.0.0.1:1 -- the tracker announce this fixture
        // must legally have (TorrentMetainfo::parse requires one) fails
        // instantly with connection-refused, no outbound DNS/HTTP, so this
        // test's DHT-only peer discovery isn't racing a real network call
        // during its QTRY_VERIFY_WITH_TIMEOUT wait below.
        const QString tpath = writeRealTorrentFixture(data.path(), payload, pieceLen, &infoHash,
                                                       QStringLiteral("real.torrent"),
                                                       QStringLiteral("http://127.0.0.1:1/announce"));

        TestSeeder seeder(infoHash, payload, pieceLen);

        // `holder`: in-process DHT node standing in for "the seeder already
        // announced itself for real" -- pre-seeded via storePeerForTest().
        DhtNode holder(NodeId::fromSeed(310), 0, 310);
        QVERIFY(holder.start());
        holder.storePeerForTest(infoHash, QStringLiteral("127.0.0.1"), seeder.port());

        // `dht`: the manager's own DHT node, bootstrapped ONLY to `holder`
        // (never the real internet) -- this is what setDhtForTest() injects.
        DhtNode dht(NodeId::fromSeed(3), 0, 3);
        QVERIFY(dht.start());
        dht.bootstrap({QStringLiteral("127.0.0.1:%1").arg(holder.boundPort())});

        DownloadManager mgr(EngineConfig{}, data.path());
        mgr.setDhtForTest(&dht);   // must run before any addTorrent()/loadTorrentSession()

        auto id = mgr.addTorrent(tpath, data.path(), {0}, PieceStrategy::RarestFirst);
        QVERIFY(!id.isNull());
        auto* t = qobject_cast<TorrentTask*>(mgr.taskById(id));
        QVERIFY(t != nullptr);

        // NO addPeerForTest, NO tracker -- the only way this can ever
        // discover the seeder is through the DHT node the manager wired in.
        QTRY_VERIFY_WITH_TIMEOUT(t->state() == DownloadState::Completed, 15000);
        QCOMPARE(readFile(QDir(data.path()).filePath("single.bin")), payload);
        QCOMPARE(t->receivedBytes(), t->totalBytes());
    }

    // --- Task 17: DHT enable/port live-toggle (settings wiring) -------------

    // Deliberately never calls setDhtEnabled(true)/setDhtConfig()/setDhtPort()
    // while DHT is enabled: any of those fire the REAL internet bootstrap
    // (DhtNode::kDefaultRouters is a hardcoded hostname list, so even a
    // caller-injected setDhtForTest() double would get bootstrapped against
    // the real routers). The disable path never bootstraps, so it's the only
    // enable-state transition this test can safely exercise offline.
    void dhtDisableDetachesTorrentTasksAndTearsDownNode() {
        const QByteArray payload = makeData(50000);
        const qint64 pieceLen = 16384;
        QTemporaryDir data; QVERIFY(data.isValid());
        QByteArray infoHash;
        const QString tpath = writeRealTorrentFixture(data.path(), payload, pieceLen, &infoHash,
                                                       QStringLiteral("t17.torrent"),
                                                       QStringLiteral("http://127.0.0.1:1/announce"));

        DhtNode testDht(NodeId::fromSeed(717), 0, 717);
        QVERIFY(testDht.start());

        DownloadManager mgr(EngineConfig{}, data.path());
        mgr.setDhtForTest(&testDht);   // must run before addTorrent(); never touches the real internet

        auto id = mgr.addTorrent(tpath, data.path(), {0}, PieceStrategy::RarestFirst);
        QVERIFY(!id.isNull());
        QCOMPARE(mgr.dhtNodeCount(), testDht.nodeCount());   // wired to the injected node

        mgr.setDhtEnabled(false);        // disable path never calls startDhtBootstrap()
        QCOMPARE(mgr.dhtNodeCount(), 0); // torn down from the manager's point of view

        // The injected node itself is caller-owned -- setDhtEnabled(false)
        // detaches from it but must never delete it (still perfectly usable).
        QVERIFY(testDht.boundPort() != 0);

        // Changing the port while disabled is stored for a future re-enable
        // only -- no live rebind/bootstrap while DHT stays off.
        mgr.setDhtPort(9999);
        QCOMPARE(mgr.dhtNodeCount(), 0);
    }

    // --- Task 15: addMagnet() crown E2E --------------------------------------

    // The full offline magnet -> download path: DownloadManager::addMagnet()
    // parses a bare "magnet:?xt=urn:btih:<hex>" (no display name, no
    // trackers -- DHT is the ONLY peer source available), resolves its info
    // dict via MetadataFetch against a DHT-discovered metadata peer, caches
    // the reconstructed .torrent, hands off to the exact same
    // makeTorrentTask() addTorrent() uses, and the resulting TorrentTask
    // downloads the real payload from a DHT-discovered seeder to completion.
    //
    // `holder` advertises BOTH the metadata peer and the seeder for the SAME
    // info-hash (mirroring the brief): MetadataFetch's own DHT lookup will
    // try connecting to both (the seeder just idles as a metadata candidate,
    // never answering the ut_metadata extended handshake - harmless, see
    // TestSeeder's "any other message ids are ignored" comment), and once
    // metadata resolves, the new TorrentTask's OWN DHT lookup (via
    // makeTorrentTask()'s setDht()) again gets both back, but only the seeder
    // ever has anything to serve as a download peer.
    void magnetResolvesViaDhtThenDownloads() {
        const QByteArray data = makeData(50000);                 // 4 pieces @ 16384
        auto m = singleMeta(data, 16384);                        // infoDict + infoHash + piece hashes
        TestSeeder seeder(m.infoHash, data, 16384);
        TestMetadataPeer metaPeer(m.infoHash, m.infoDict);

        // DHT holder advertises BOTH the metadata peer and the seeder for infoHash.
        DhtNode holder(NodeId::fromSeed(200), 0, 200);
        QVERIFY(holder.start());
        holder.storePeerForTest(m.infoHash, QStringLiteral("127.0.0.1"), metaPeer.port());
        holder.storePeerForTest(m.infoHash, QStringLiteral("127.0.0.1"), seeder.port());

        DhtNode dht(NodeId::fromSeed(1), 0, 1);
        QVERIFY(dht.start());
        dht.bootstrap({QStringLiteral("127.0.0.1:%1").arg(holder.boundPort())});

        QTemporaryDir dir; QVERIFY(dir.isValid());
        DownloadManager mgr(EngineConfig{}, dir.path());
        mgr.setDhtForTest(&dht);   // must run before any addMagnet()/addTorrent()/loadTorrentSession()

        const QString magnet = QStringLiteral("magnet:?xt=urn:btih:") + QString::fromLatin1(m.infoHash.toHex());
        const QUuid id = mgr.addMagnet(magnet, dir.path(), PieceStrategy::RarestFirst);
        QVERIFY(!id.isNull());

        QTRY_VERIFY_WITH_TIMEOUT(mgr.taskById(id) &&
                                  mgr.taskById(id)->state() == DownloadState::Completed, 20000);
        QCOMPARE(readFile(QDir(dir.path()).filePath(m.name)), data);

        // The task that finished is a real TorrentTask, same id addMagnet()
        // returned up front -- never a second/different task.
        auto* t = qobject_cast<TorrentTask*>(mgr.taskById(id));
        QVERIFY(t != nullptr);
        QCOMPARE(t->receivedBytes(), t->totalBytes());

        // The resolved torrent was cached for a future restart.
        const QString hex = QString::fromLatin1(m.infoHash.toHex());
        QVERIFY(QFile::exists(dir.path() + "/torrents/" + hex + ".torrent"));
    }

    // Regression (Task 15 verbatim-cache fix): the resolved magnet's info
    // dict carries a "private" key TorrentMetainfo::parse doesn't extract
    // into any TorrentMetainfo field. The cached torrents/<hex>.torrent must
    // still re-parse to the SAME info-hash the magnet was resolved for --
    // this would FAIL under the old encodeInfoDict/encodeTorrentBytes
    // re-encode (it drops "private", producing different bytes and thus a
    // different info-hash on re-parse), and only passes because
    // DownloadManager now splices MetadataFetch's verbatim infoDict via
    // TorrentMetainfo::wrapInfoDictAsTorrent instead.
    void magnetCachesVerbatimInfoDictWithUnknownKeys() {
        const QByteArray data = makeData(50000);
        auto m = singleMetaWithPrivateFlag(data, 16384);
        TestMetadataPeer metaPeer(m.infoHash, m.infoDict);

        DhtNode holder(NodeId::fromSeed(210), 0, 210);
        QVERIFY(holder.start());
        holder.storePeerForTest(m.infoHash, QStringLiteral("127.0.0.1"), metaPeer.port());

        DhtNode dht(NodeId::fromSeed(2), 0, 2);
        QVERIFY(dht.start());
        dht.bootstrap({QStringLiteral("127.0.0.1:%1").arg(holder.boundPort())});

        QTemporaryDir dir; QVERIFY(dir.isValid());
        DownloadManager mgr(EngineConfig{}, dir.path());
        mgr.setDhtForTest(&dht);

        const QString magnet = QStringLiteral("magnet:?xt=urn:btih:") + QString::fromLatin1(m.infoHash.toHex());
        const QUuid id = mgr.addMagnet(magnet, dir.path(), PieceStrategy::RarestFirst);
        QVERIFY(!id.isNull());

        // Wait for the FetchingMetadata placeholder to be replaced by the real
        // TorrentTask (same id) -- proof metadata resolved successfully.
        QTRY_VERIFY_WITH_TIMEOUT(
            mgr.taskById(id) && mgr.taskById(id)->kind() == AbstractTask::Kind::Torrent, 20000);

        const QString hex = QString::fromLatin1(m.infoHash.toHex());
        const QString cachedPath = dir.path() + "/torrents/" + hex + ".torrent";
        QVERIFY(QFile::exists(cachedPath));

        bool ok = false; QString err;
        const TorrentMetainfo reparsed = TorrentMetainfo::parse(readFile(cachedPath), &ok, &err);
        QVERIFY2(ok, qPrintable(err));
        QCOMPARE(reparsed.infoHash, m.infoHash);
    }

    // --- Final-review Fix I1 --------------------------------------------------
    // A MetadataFetch in flight (addMagnet(), never resolved -- no peer source
    // ever answers, so it just sits FetchingMetadata) holds a raw DhtNode* and
    // a periodic (kDhtRetryMs = 1s) retry timer that calls m_dht->lookup(...).
    // DownloadManager::setDhtEnabled(false) tears down the manager-OWNED
    // DhtNode this fetch is wired to; before Fix I1 nothing told the fetch,
    // so the retry timer's very next tick dereferenced freed memory. Uses the
    // ctor's own default DhtNode (never setDhtForTest()) specifically so this
    // delete is REAL, not a no-op on a caller-owned test double -- and never
    // enables/bootstraps it (setDhtEnabled(false) never calls
    // startDhtBootstrap(), see dhtDisableDetachesTorrentTasksAndTearsDownNode
    // above), so this stays fully offline like every other test here.
    void dhtDisableDuringInFlightMetadataFetchDoesNotDangle() {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        DownloadManager mgr(EngineConfig{}, dir.path());

        const QByteArray infoHash =
            QCryptographicHash::hash("i1-fix-test-info-hash", QCryptographicHash::Sha1);
        const QString magnet =
            QStringLiteral("magnet:?xt=urn:btih:") + QString::fromLatin1(infoHash.toHex());
        const QUuid id = mgr.addMagnet(magnet, dir.path(), PieceStrategy::RarestFirst);
        QVERIFY(!id.isNull());
        QVERIFY(mgr.taskById(id) != nullptr);
        QCOMPARE(mgr.taskById(id)->state(), DownloadState::FetchingMetadata);

        // Deletes the manager-owned DhtNode the in-flight MetadataFetch is
        // wired to. Fix I1: DownloadManager must rewire every m_metadataFetches
        // entry to nullptr (which stops its retry timer) BEFORE this delete.
        mgr.setDhtEnabled(false);
        QCOMPARE(mgr.dhtNodeCount(), 0);

        // Outlive the 1s retry period several times over -- a pre-fix build
        // dereferences the freed DhtNode here (crash/UB under a sanitizer).
        QTest::qWait(3000);

        // Still alive and untouched: nothing else was ever going to resolve
        // this magnet (no DHT, no trackers, no injected peer), so it's still
        // exactly where it started.
        QVERIFY(mgr.taskById(id) != nullptr);
        QCOMPARE(mgr.taskById(id)->state(), DownloadState::FetchingMetadata);
    }
};

QTEST_GUILESS_MAIN(TstTorrent)
#include "tst_torrent.moc"
