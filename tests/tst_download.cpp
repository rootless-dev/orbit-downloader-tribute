#include <QtTest>
#include <QNetworkAccessManager>
#include <QSignalSpy>
#include <QTemporaryFile>
#include <QTemporaryDir>
#include <QDir>
#include <QTcpServer>
#include <QTcpSocket>
#include <QFileInfo>
#include "TestServer.h"
#include "Transport.h"
#include "HttpTransport.h"
#include "HttpProbe.h"
#include "SegmentWorker.h"
#include "DownloadTask.h"
#include "DownloadManager.h"
#include "Persistence.h"
#include "Logger.h"

static QByteArray makeBody(int n) {
    QByteArray b; b.resize(n);
    for (int i = 0; i < n; ++i) b[i] = char('A' + (i % 26));
    return b;
}

// --- Task 2 test helpers -----------------------------------------------
// The brief sketches these against a TestServer API (default-constructible,
// start(), body(path)) that doesn't match the real TestServer in this file
// (constructor takes the body, listen() binds, and there's no body(path)
// accessor - callers already hold the QByteArray they built). Adapted here
// to the real API/signatures; behavior of the helpers themselves matches
// the brief.

static QString makeTempDir() {
    const QString path = QDir::tempPath() + "/orbit_task2_" + QUuid::createUuid().toString(QUuid::WithoutBraces);
    QDir().mkpath(path);
    return path;
}

static QByteArray readFile(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return QByteArray();
    return f.readAll();
}

static bool waitForState(DownloadManager& mgr, const QUuid& id, DownloadState want, int timeoutMs) {
    return QTest::qWaitFor([&]{
        DownloadTask* t = mgr.taskById(id);
        return t && t->state() == want;
    }, timeoutMs);
}

class TstDownload : public QObject {
    Q_OBJECT
    QByteArray m_body;
private slots:
    void initTestCase() { m_body = makeBody(5000); }

    void probeRangedReportsSizeAndSupport() {
        TestServer srv(m_body);
        QVERIFY(srv.listen());
        QNetworkAccessManager nam;
        HttpProbe probe(&nam, "curl/8.7.1");
        QSignalSpy spy(&probe, &HttpProbe::finished);
        probe.start(srv.url("/ranged"), Credentials{}, HeaderList{});
        QVERIFY(spy.wait(3000));
        const auto res = qvariant_cast<ProbeResult>(spy.at(0).at(0));
        QVERIFY(res.ok);
        QVERIFY(res.supportsRange);
        QCOMPARE(res.totalBytes, qint64(m_body.size()));
        QCOMPARE(res.etag, QString("\"v1\""));
    }

    void probePlainReportsNoRange() {
        TestServer srv(m_body);
        QVERIFY(srv.listen());
        QNetworkAccessManager nam;
        HttpProbe probe(&nam, "curl/8.7.1");
        QSignalSpy spy(&probe, &HttpProbe::finished);
        probe.start(srv.url("/plain"), Credentials{}, HeaderList{});
        QVERIFY(spy.wait(3000));
        const auto res = qvariant_cast<ProbeResult>(spy.at(0).at(0));
        QVERIFY(res.ok);
        QVERIFY(!res.supportsRange);
        QCOMPARE(res.totalBytes, qint64(m_body.size()));
    }

    // Um 302 intermediário não pode ser reportado como "HTTP 302": o probe deve
    // seguir o redirect e ler a resposta final (ex.: links Google Takeout).
    void probeFollowsRedirect() {
        TestServer srv(m_body);
        QVERIFY(srv.listen());
        QNetworkAccessManager nam;
        HttpProbe probe(&nam, "curl/8.7.1");
        QSignalSpy spy(&probe, &HttpProbe::finished);
        probe.start(srv.url("/redirect"), Credentials{}, HeaderList{});
        QVERIFY(spy.wait(3000));
        const auto res = qvariant_cast<ProbeResult>(spy.at(0).at(0));
        QVERIFY2(res.ok, qPrintable(res.error));   // não pode ser "HTTP 302"
        QVERIFY(res.supportsRange);
        QCOMPARE(res.totalBytes, qint64(m_body.size()));
    }

    // Download completo através de um 302 (probe + workers seguem o redirect).
    void downloadFollowsRedirect() {
        TestServer srv(m_body);
        QVERIFY(srv.listen());
        const QString dir = makeTempDir();
        EngineConfig cfg;
        DownloadManager mgr(cfg, dir);
        const QUuid id = mgr.addDownload(srv.url("/redirect"), dir + "/movie.bin");
        QVERIFY(waitForState(mgr, id, DownloadState::Completed, 5000));
        QCOMPARE(readFile(dir + "/movie.bin"), m_body);
    }

    void probeReadsContentDispositionFilename() {
        TestServer srv(m_body);
        QVERIFY(srv.listen());
        QNetworkAccessManager nam;
        HttpProbe probe(&nam, "curl/8.7.1");
        QSignalSpy spy(&probe, &HttpProbe::finished);
        probe.start(srv.url("/named"), Credentials{}, HeaderList{});
        QVERIFY(spy.wait(3000));
        const auto res = qvariant_cast<ProbeResult>(spy.at(0).at(0));
        QVERIFY(res.ok);
        QCOMPARE(res.suggestedFileName, QString("Audiobook.m4a"));
    }

    void probeWithoutContentDispositionHasEmptyName() {
        TestServer srv(m_body);
        QVERIFY(srv.listen());
        QNetworkAccessManager nam;
        HttpProbe probe(&nam, "curl/8.7.1");
        QSignalSpy spy(&probe, &HttpProbe::finished);
        probe.start(srv.url("/ranged"), Credentials{}, HeaderList{});
        QVERIFY(spy.wait(3000));
        const auto res = qvariant_cast<ProbeResult>(spy.at(0).at(0));
        QVERIFY(res.suggestedFileName.isEmpty());
    }

    void probe404NotOk() {
        TestServer srv(m_body);
        QVERIFY(srv.listen());
        QNetworkAccessManager nam;
        HttpProbe probe(&nam, "curl/8.7.1");
        QSignalSpy spy(&probe, &HttpProbe::finished);
        probe.start(srv.url("/notfound"), Credentials{}, HeaderList{});
        QVERIFY(spy.wait(3000));
        const auto res = qvariant_cast<ProbeResult>(spy.at(0).at(0));
        QVERIFY(!res.ok);
    }

    void segmentWritesItsRange() {
        TestServer srv(m_body);
        QVERIFY(srv.listen());
        QTemporaryFile tmp; QVERIFY(tmp.open());
        const QString path = tmp.fileName();
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadWrite));
        file.resize(m_body.size());                 // preallocate

        QNetworkAccessManager nam;
        EngineConfig cfg;
        SegmentWorker w(&nam, &file, cfg);
        QSignalSpy done(&w, &SegmentWorker::completed);
        Segment seg{0, 1000, 1000, 1999};           // bytes 1000..1999
        w.start(seg, srv.url("/ranged"), QString(), Credentials{}, HeaderList{});
        QVERIFY(done.wait(3000));

        file.seek(1000);
        QCOMPARE(file.read(1000), m_body.mid(1000, 1000));
        QCOMPARE(w.segment().current, 2000LL);      // advanced past end
    }

    void downloadsMultiSegmentByteIdentical() {
        TestServer srv(m_body);
        QVERIFY(srv.listen());
        QTemporaryDir dir;
        const QString dest = dir.filePath("out.bin");
        QNetworkAccessManager nam;
        EngineConfig cfg; cfg.segmentCount = 4; cfg.minSegSize = 1;
        HttpTransport tr(&nam);
        DownloadTask task(&tr, cfg);
        task.init(QUuid::createUuid(), srv.url("/ranged"), dest, 4);
        task.start();
        QTRY_COMPARE_WITH_TIMEOUT(task.state(), DownloadState::Completed, 5000);
        QFile f(dest); QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(f.readAll(), m_body);
        QVERIFY(!QFile::exists(Persistence::metaPath(dest)));   // meta removed on completion
    }

    void fallbackSingleConnectionWhenNoRange() {
        TestServer srv(m_body);
        QVERIFY(srv.listen());
        QTemporaryDir dir;
        const QString dest = dir.filePath("plain.bin");
        QNetworkAccessManager nam;
        EngineConfig cfg;
        HttpTransport tr(&nam);
        DownloadTask task(&tr, cfg);
        task.init(QUuid::createUuid(), srv.url("/plain"), dest, 4);
        task.start();
        QTRY_COMPARE_WITH_TIMEOUT(task.state(), DownloadState::Completed, 5000);
        QFile f(dest); QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(f.readAll(), m_body);
    }

    void pauseThenResumeIsByteIdentical() {
        TestServer srv(m_body);
        QVERIFY(srv.listen());
        QTemporaryDir dir;
        const QString dest = dir.filePath("pr.bin");
        QNetworkAccessManager nam;
        EngineConfig cfg; cfg.segmentCount = 4; cfg.minSegSize = 1;
        HttpTransport tr(&nam);
        DownloadTask task(&tr, cfg);
        task.init(QUuid::createUuid(), srv.url("/ranged"), dest, 4);
        task.start();
        // pause as soon as some progress arrives
        QSignalSpy prog(&task, &DownloadTask::progress);
        QVERIFY(prog.wait(3000));
        task.pause();
        QTRY_COMPARE(task.state(), DownloadState::Paused);
        QVERIFY(QFile::exists(Persistence::metaPath(dest)));

        task.start();                         // resume in same session
        QTRY_COMPARE_WITH_TIMEOUT(task.state(), DownloadState::Completed, 5000);
        QFile f(dest); QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(f.readAll(), m_body);
    }

    void fallbackResumeDiscardsPartialAndCompletes() {
        // Spec 3.4: fallback (non-Range, single-segment) downloads are not
        // resumable. Resuming one must discard whatever partial bytes were
        // already written and restart the whole body from offset 0 - never
        // write the server's fresh 200 response at the stale non-zero
        // offset the partial left `current` at (that would corrupt the file
        // with extra leading bytes and an over-long, wrong-content result).
        QByteArray big = makeBody(2 * 1024 * 1024);   // 2 MiB, large enough for pause() to
                                                        // reliably land mid-transfer
        TestServer srv(big);
        QVERIFY(srv.listen());
        QTemporaryDir dir;
        const QString dest = dir.filePath("fbresume.bin");
        QNetworkAccessManager nam;
        EngineConfig cfg; cfg.progressThrottleMs = 1;
        HttpTransport tr(&nam);
        DownloadTask task(&tr, cfg);
        task.init(QUuid::createUuid(), srv.url("/plain"), dest, 4);
        task.start();

        QSignalSpy prog(&task, &DownloadTask::progress);
        QVERIFY(prog.wait(3000));           // some bytes have landed (fallback segment current > 0)
        task.pause();
        QTRY_COMPARE(task.state(), DownloadState::Paused);
        QVERIFY(QFile::exists(Persistence::metaPath(dest)));

        task.start();                       // resume in same session -> must restart from 0
        QTRY_COMPARE_WITH_TIMEOUT(task.state(), DownloadState::Completed, 8000);
        QFile f(dest); QVERIFY(f.open(QIODevice::ReadOnly));
        const QByteArray got = f.readAll();
        QCOMPARE(got.size(), big.size());
        QCOMPARE(got, big);
    }

    void retriesRecoverableFailureThenCompletes() {
        TestServer srv(m_body);
        QVERIFY(srv.listen());
        QTemporaryDir dir;
        const QString dest = dir.filePath("flaky.bin");
        QNetworkAccessManager nam;
        EngineConfig cfg; cfg.segmentCount = 1;      // single segment, deterministic
        cfg.maxSegmentRetries = 5; cfg.retryBackoffBaseMs = 10;
        HttpTransport tr(&nam);
        DownloadTask task(&tr, cfg);
        task.init(QUuid::createUuid(),
                  srv.url("/flaky?failTimes=2"), dest, 1);
        task.start();
        QTRY_COMPARE_WITH_TIMEOUT(task.state(), DownloadState::Completed, 8000);
        QFile f(dest); QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(f.readAll(), m_body);
    }

    void exhaustedRetriesGoesToError() {
        TestServer srv(m_body);
        QVERIFY(srv.listen());
        QTemporaryDir dir;
        const QString dest = dir.filePath("dead.bin");
        QNetworkAccessManager nam;
        EngineConfig cfg; cfg.segmentCount = 1;
        cfg.maxSegmentRetries = 2; cfg.retryBackoffBaseMs = 10;
        HttpTransport tr(&nam);
        DownloadTask task(&tr, cfg);
        task.init(QUuid::createUuid(),
                  srv.url("/flaky?failTimes=99"), dest, 1);
        task.start();
        QTRY_COMPARE_WITH_TIMEOUT(task.state(), DownloadState::Error, 8000);
    }

    void errorThenRestartResumesToCompletion() {
        // Criterion 7: Error -> resume -> Completed.
        //
        // Tuned against /flaky's hit-counting semantics (see
        // TestServer::listen()'s /flaky route): the very first hit is
        // HttpProbe's own "bytes=0-0" GET (it only inspects status/headers,
        // never the body, so a truncated probe response never affects it) -
        // only hits after that are the actual segment's GET attempts. With
        // maxSegmentRetries=1 the segment gets exactly 2 tries per start()
        // (attempt 0 + 1 retry). failTimes=3 means hits 1..3 are short
        // reads and hit 4+ succeed in full:
        //   hit1 = probe                                    (ignored, always 206)
        //   hit2 = segment attempt 0  -> short read -> retry
        //   hit3 = segment attempt 1  -> short read -> retries exhausted -> Error
        //   [task.start() again; m_probed is already true, so no new probe hit]
        //   hit4 = segment attempt 0  -> failTimes(3) exceeded -> full remaining body -> Completed
        TestServer srv(m_body);
        QVERIFY(srv.listen());
        QTemporaryDir dir;
        const QString dest = dir.filePath("errresume.bin");
        QNetworkAccessManager nam;
        EngineConfig cfg; cfg.segmentCount = 1;
        cfg.maxSegmentRetries = 1; cfg.retryBackoffBaseMs = 10;
        HttpTransport tr(&nam);
        DownloadTask task(&tr, cfg);
        task.init(QUuid::createUuid(), srv.url("/flaky?failTimes=3"), dest, 1);
        task.start();
        QTRY_COMPARE_WITH_TIMEOUT(task.state(), DownloadState::Error, 8000);

        task.start();   // resume: same instance, retained segment `current` + fresh m_attempt
        QTRY_COMPARE_WITH_TIMEOUT(task.state(), DownloadState::Completed, 8000);
        QFile f(dest); QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(f.readAll(), m_body);
    }

    void oneSegmentRetriesWhileOthersComplete() {
        // Criterion 6: parallel retry - one segment retries a short read
        // while the other segments proceed and complete normally, and the
        // whole download still lands byte-identical.
        //
        // /flaky's hit counter (see TestServer::listen()) is global across
        // ALL requests to the path, including HttpProbe's own "bytes=0-0"
        // probe request, which always lands first as hit 1 (before any
        // SegmentWorker starts) regardless of failTimes - the probe only
        // inspects status/headers, so a truncated body never affects it.
        // With segmentCount=4 the four segments then fire their initial GET
        // requests concurrently, landing as hits 2,3,4,5 in whatever order
        // the server happens to receive them. failTimes=2 makes exactly one
        // of those four requests (whichever lands as hit 2) get a short
        // (truncated) read and need a retry, while the other three (hits
        // 3,4,5, all > failTimes) succeed in full on their very first
        // attempt - i.e. one segment retries in parallel with the others
        // completing, without needing to isolate in advance which specific
        // segment that will be.
        TestServer srv(m_body);
        QVERIFY(srv.listen());
        QTemporaryDir dir;
        const QString dest = dir.filePath("parretry.bin");
        QNetworkAccessManager nam;
        EngineConfig cfg; cfg.segmentCount = 4; cfg.minSegSize = 1;
        cfg.maxSegmentRetries = 3; cfg.retryBackoffBaseMs = 10;
        HttpTransport tr(&nam);
        DownloadTask task(&tr, cfg);
        task.init(QUuid::createUuid(), srv.url("/flaky?failTimes=2"), dest, 4);
        task.start();
        QTRY_COMPARE_WITH_TIMEOUT(task.state(), DownloadState::Completed, 8000);
        QFile f(dest); QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(f.readAll(), m_body);
    }

    void notFoundIsNonRecoverableError() {
        TestServer srv(m_body);
        QVERIFY(srv.listen());
        QTemporaryDir dir;
        const QString dest = dir.filePath("nf.bin");
        QNetworkAccessManager nam;
        EngineConfig cfg;
        HttpTransport tr(&nam);
        DownloadTask task(&tr, cfg);
        task.init(QUuid::createUuid(), srv.url("/notfound"), dest, 4);
        task.start();
        QTRY_COMPARE_WITH_TIMEOUT(task.state(), DownloadState::Error, 5000);
    }

    void requestsCarryCurlUserAgent() {
        // O UA é configurável via EngineConfig; probe E segmentos usam o mesmo.
        TestServer srv(m_body);
        QVERIFY(srv.listen());
        QTemporaryDir dir;
        const QString dest = dir.filePath("ua.bin");
        QNetworkAccessManager nam;
        EngineConfig cfg; cfg.segmentCount = 4; cfg.minSegSize = 1;
        cfg.userAgent = "orbit-test/1.0";
        HttpTransport tr(&nam);
        DownloadTask task(&tr, cfg);
        task.init(QUuid::createUuid(), srv.url("/ranged"), dest, 4);
        task.start();
        QTRY_COMPARE_WITH_TIMEOUT(task.state(), DownloadState::Completed, 5000);
        QCOMPARE(srv.userAgentsSeen(), QStringList{"orbit-test/1.0"});
    }

    void probeErrorIsExposedOnTask() {
        // O texto do erro do probe (ex.: "HTTP 404") deve ficar disponível na
        // task, não ser engolido — a GUI/Log precisa dele para diagnóstico.
        TestServer srv(m_body);
        QVERIFY(srv.listen());
        QTemporaryDir dir;
        const QString dest = dir.filePath("err.bin");
        QNetworkAccessManager nam;
        EngineConfig cfg;
        HttpTransport tr(&nam);
        DownloadTask task(&tr, cfg);
        task.init(QUuid::createUuid(), srv.url("/notfound"), dest, 4);
        task.start();
        QTRY_COMPARE_WITH_TIMEOUT(task.state(), DownloadState::Error, 5000);
        QVERIFY(task.error().contains("404"));
    }

    void idleTimeoutTriggersRetryThenError() {
        TestServer srv(m_body);
        QVERIFY(srv.listen());
        QTemporaryDir dir;
        const QString dest = dir.filePath("stall.bin");
        QNetworkAccessManager nam;
        EngineConfig cfg; cfg.segmentCount = 1;
        cfg.idleTimeoutMs = 150; cfg.connectTimeoutMs = 150;
        cfg.maxSegmentRetries = 2; cfg.retryBackoffBaseMs = 10;
        HttpTransport tr(&nam);
        DownloadTask task(&tr, cfg);
        task.init(QUuid::createUuid(), srv.url("/stall"), dest, 1);
        task.start();
        // /stall never delivers the full range -> short reads/timeouts -> retries exhaust -> Error
        QTRY_COMPARE_WITH_TIMEOUT(task.state(), DownloadState::Error, 8000);
    }

    // Beyond the brief: /stall (above) always finishes with a clean short
    // read, so it exercises Task 8's short-read retry path end-to-end but
    // never forces SegmentWorker's own connect-timeout timer to actually
    // fire (the response lands well under connectTimeoutMs on loopback).
    // This test isolates the timer itself: a raw TCP listener accepts the
    // connection (so QNetworkAccessManager sees a live socket) but never
    // writes a single byte back. With no application-level response,
    // nothing but SegmentWorker's own m_idleTimer (armed with
    // connectTimeoutMs in openRequest()) can ever move this forward -
    // readyRead/finished/errorOccurred never fire on their own. Before the
    // timer exists this hangs until QSignalSpy::wait's own timeout (RED);
    // once onTimeout() aborts and retries, it fails fast after the retry
    // budget is exhausted (GREEN).
    void segmentConnectTimeoutRetriesThenFails() {
        QTcpServer blackhole;
        QVERIFY(blackhole.listen(QHostAddress::LocalHost));
        QList<QTcpSocket*> conns;   // keep accepted sockets alive; never write to them
        connect(&blackhole, &QTcpServer::newConnection, [&] {
            while (blackhole.hasPendingConnections())
                conns.append(blackhole.nextPendingConnection());
        });

        QTemporaryFile tmp;
        QVERIFY(tmp.open());
        QFile file(tmp.fileName());
        QVERIFY(file.open(QIODevice::ReadWrite));
        file.resize(100);

        QNetworkAccessManager nam;
        EngineConfig cfg;
        cfg.connectTimeoutMs = 100; cfg.idleTimeoutMs = 100;
        cfg.maxSegmentRetries = 2; cfg.retryBackoffBaseMs = 10;
        SegmentWorker w(&nam, &file, cfg);
        QSignalSpy failedSpy(&w, &SegmentWorker::failed);
        Segment seg{0, 0, 0, 99};
        const QUrl url(QString("http://127.0.0.1:%1/black").arg(blackhole.serverPort()));
        w.start(seg, url, QString(), Credentials{}, HeaderList{});
        QVERIFY(failedSpy.wait(5000));
        QCOMPARE(failedSpy.at(0).at(1).toString(), QString("retries exhausted"));
    }

    void openFailureGoesToError() {
        TestServer srv(m_body);
        QVERIFY(srv.listen());
        QTemporaryDir dir;
        // Intentionally do NOT create "no_such_subdir" so QFile::open(ReadWrite)
        // fails inside beginSegments() (the parent directory doesn't exist).
        const QString dest = dir.filePath("no_such_subdir/out.bin");
        QNetworkAccessManager nam;
        EngineConfig cfg;
        HttpTransport tr(&nam);
        DownloadTask task(&tr, cfg);
        task.init(QUuid::createUuid(), srv.url("/ranged"), dest, 4);
        task.start();
        QTRY_COMPARE_WITH_TIMEOUT(task.state(), DownloadState::Error, 5000);
    }

    void progressIsThrottled() {
        // Use a large body so many raw SegmentWorker::progressed events fire
        // (segmentProgress is unthrottled) and the throttled `progress`
        // signal has a real chance to coalesce a meaningfully smaller count.
        // A tiny body doesn't generate enough updates for the comparison
        // below to be discriminating (it would pass even with the throttle
        // removed).
        QByteArray big = makeBody(2 * 1024 * 1024);   // 2 MiB
        TestServer srv(big);
        QVERIFY(srv.listen());
        QTemporaryDir dir;
        const QString dest = dir.filePath("thr.bin");
        QNetworkAccessManager nam;
        EngineConfig cfg; cfg.segmentCount = 32; cfg.minSegSize = 1;
        cfg.progressThrottleMs = 50;
        HttpTransport tr(&nam);
        DownloadTask task(&tr, cfg);
        task.init(QUuid::createUuid(), srv.url("/ranged"), dest, 32);
        QSignalSpy seg(&task, &DownloadTask::segmentProgress);   // unthrottled baseline
        QSignalSpy prog(&task, &DownloadTask::progress);         // throttled
        task.start();
        QTRY_COMPARE_WITH_TIMEOUT(task.state(), DownloadState::Completed, 10000);
        QFile f(dest); QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(f.readAll(), big);
        // segmentCount=32 => at least one segmentProgress per segment on loopback
        // (measured stable at seg=32, prog=2). >20 keeps the comparison meaningful.
        QVERIFY(seg.count() > 20);   // enough raw updates for the comparison to be meaningful
        QVERIFY2(prog.count() < seg.count(),
                 qPrintable(QString("throttled progress (%1) not fewer than raw segmentProgress (%2)")
                                .arg(prog.count()).arg(seg.count())));
        QVERIFY(prog.count() >= 1);
    }

    void changedResourceRestartsAndCompletes() {
        TestServer srv(m_body);
        QVERIFY(srv.listen());
        QTemporaryDir dir;
        const QString dest = dir.filePath("chg.bin");

        // Seed a .meta as if a prior partial download exists, with an old validator.
        QVector<Segment> segs = { {0, 0, 100, qint64(m_body.size()-1)} };
        Persistence::writeMeta(dest, segs, "\"v1-old\"", "", true);
        // Preallocate the dest so resume path opens it.
        { QFile f(dest); QVERIFY(f.open(QIODevice::WriteOnly)); f.resize(m_body.size()); }

        QNetworkAccessManager nam;
        EngineConfig cfg; cfg.segmentCount = 1;
        HttpTransport tr(&nam);
        DownloadTask task(&tr, cfg);
        DownloadRecord rec;
        rec.id = QUuid::createUuid(); rec.url = srv.url("/changed");
        rec.destPath = dest; rec.totalBytes = m_body.size();
        rec.supportsRange = true; rec.segmentCount = 1;
        task.restore(rec, segs, "\"v1-old\"", "", true);
        task.start();          // resume -> server 200 (ETag changed) -> restart from 0
        QTRY_COMPARE_WITH_TIMEOUT(task.state(), DownloadState::Completed, 6000);
        QFile f(dest); QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(f.readAll(), m_body);
    }

    void managerRespectsMaxConcurrent() {
        // Weak version of this test used to sample concurrency exactly once,
        // right after addDownload() returned - a moment that reads 0 whether
        // or not the cap is honored (nothing has connected yet). Instead,
        // track the PEAK number of tasks in Downloading|Connecting across
        // every taskStateChanged emission for the whole run: that's a real
        // invariant that would be violated at some point if pump() ever let
        // more than maxConcurrentDownloads tasks run at once.
        TestServer srv(m_body);
        QVERIFY(srv.listen());
        QTemporaryDir data, out;
        EngineConfig cfg; cfg.maxConcurrentDownloads = 1; cfg.segmentCount = 1; cfg.minSegSize = 1;
        DownloadManager mgr(cfg, data.path());

        int peak = 0;
        auto trackPeak = [&] {
            int active = 0;
            for (auto* t : mgr.tasks())
                if (t->state() == DownloadState::Downloading || t->state() == DownloadState::Connecting)
                    ++active;
            if (active > peak) peak = active;
        };
        connect(&mgr, &DownloadManager::taskStateChanged, &mgr,
                [&](const QUuid&, DownloadState) { trackPeak(); });

        mgr.addDownload(srv.url("/ranged"), out.filePath("a.bin"));
        mgr.addDownload(srv.url("/ranged"), out.filePath("b.bin"));
        mgr.addDownload(srv.url("/ranged"), out.filePath("c.bin"));

        // All three eventually complete (proves the queue drains via
        // re-promotion, not just that nothing crashed).
        QTRY_VERIFY_WITH_TIMEOUT(
            [&]{ int c=0; for (auto* t: mgr.tasks()) if (t->state()==DownloadState::Completed) ++c;
                 return c==3; }(), 8000);

        QVERIFY2(peak >= 1, "never observed any task actively downloading - test isn't exercising anything");
        QVERIFY2(peak <= 1, qPrintable(QString("peak concurrent active tasks (%1) exceeded cap (1)").arg(peak)));
    }

    void resumeAllRespectsMaxConcurrent() {
        // Proves DownloadManager::resumeAll() routes resumed tasks through
        // pump()'s concurrency cap rather than starting all of them at once.
        // Peak-tracks Downloading|Connecting across the entire run, including
        // the resumeAll() call itself, so a resumeAll that starts every
        // Paused task directly (bypassing the cap) would be caught by peak
        // exceeding 1.
        //
        // Getting three genuinely-Paused tasks into the manager is done one
        // at a time (add -> wait for Downloading -> pause() the task object
        // directly) rather than via mgr.pauseAll() on three pre-added tasks.
        // mgr.pauseAll()'s single loop pass, combined with pump()'s
        // promote-on-pause behavior, can pause a task that pump() *just*
        // promoted into Connecting as a side effect of pausing an earlier
        // task in the very same pauseAll() call - and DownloadTask::pause()
        // on a task that's still mid-probe doesn't cancel the in-flight
        // HttpProbe, so that probe's callback (onProbed() -> beginSegments())
        // later force-starts the "paused" task regardless, outside pump()'s
        // accounting entirely. That's a real, pre-existing race in
        // pauseAll()/pause()-while-Connecting, but it is orthogonal to what
        // this test is verifying (resumeAll()'s cap), so each task here is
        // only ever paused once it has left Connecting and is actually
        // Downloading with real segments - no probe is ever left in flight
        // across a pause.
        // 5 MiB per task (matches the body size proven reliable, across 15
        // repeated runs, for observing a task still Downloading mid-transfer
        // in restartProcessResumesFromDisk below - a smaller body risks the
        // whole transfer completing inside a single QTRY poll window,
        // jumping straight from Connecting to Completed with no observable
        // Downloading state in between).
        QByteArray body = makeBody(5 * 1024 * 1024);
        TestServer srv(body);
        QVERIFY(srv.listen());
        QTemporaryDir data, out;
        EngineConfig cfg; cfg.maxConcurrentDownloads = 1; cfg.segmentCount = 4; cfg.minSegSize = 1;
        cfg.progressThrottleMs = 1;
        DownloadManager mgr(cfg, data.path());

        int peak = 0;
        auto trackPeak = [&] {
            int active = 0;
            for (auto* t : mgr.tasks())
                if (t->state() == DownloadState::Downloading || t->state() == DownloadState::Connecting)
                    ++active;
            if (active > peak) peak = active;
        };
        connect(&mgr, &DownloadManager::taskStateChanged, &mgr,
                [&](const QUuid&, DownloadState) { trackPeak(); });

        auto addWaitPause = [&](const QString& dest) {
            mgr.addDownload(srv.url("/ranged"), dest);
            DownloadTask* t = mgr.tasks().last();
            QTRY_VERIFY_WITH_TIMEOUT(t->state() == DownloadState::Downloading, 5000);
            t->pause();
            QTRY_COMPARE(t->state(), DownloadState::Paused);
        };
        addWaitPause(out.filePath("r1.bin"));
        addWaitPause(out.filePath("r2.bin"));
        addWaitPause(out.filePath("r3.bin"));
        QCOMPARE(mgr.tasks().size(), 3);
        for (auto* t : mgr.tasks()) QCOMPARE(t->state(), DownloadState::Paused);

        mgr.resumeAll();

        QTRY_VERIFY_WITH_TIMEOUT(
            [&]{ int c=0; for (auto* t: mgr.tasks()) if (t->state()==DownloadState::Completed) ++c;
                 return c==3; }(), 15000);

        QVERIFY2(peak <= 1, qPrintable(QString("peak concurrent active tasks (%1) exceeded cap (1) across resumeAll()").arg(peak)));

        for (auto* t : mgr.tasks()) {
            QFile f(t->record().destPath);
            QVERIFY(f.open(QIODevice::ReadOnly));
            QCOMPARE(f.readAll(), body);
        }
    }

    void resumeAllRespectsCapWithSyncTerminalTask() {
        // Regression test for the CRITICAL pump() re-entrancy bug: resumeAll()
        // requeue()s every Paused/Error task straight to Queued, and a
        // *restored* Queued task's start() can run beginSegments()
        // SYNCHRONOUSLY and land in a terminal state (Error, exercised here
        // via a destination that can never be opened) before start() even
        // returns. That synchronous stateChanged(Error) fires wire()'s
        // handler, which calls pump() again *while the outer pump() from
        // resumeAll() is still mid-loop*. Without a re-entrancy guard, plus
        // a freshly-recomputed `active` count each iteration, the outer
        // loop's hoisted `active` counter is blind to what the nested
        // pump() call just did and can promote one Queued task too many -
        // violating maxConcurrentDownloads. See DownloadManager::pump().
        // 5 MiB per resumable task, matching the body size proven reliable
        // (resumeAllRespectsMaxConcurrent, restartProcessResumesFromDisk)
        // for observing a task still Downloading mid-transfer before it's
        // paused - a smaller body risks the whole transfer completing inside
        // a single QTRY poll window, jumping straight from Connecting to
        // Completed with no observable Downloading state in between.
        QByteArray body = makeBody(5 * 1024 * 1024);
        TestServer srv(body);
        QVERIFY(srv.listen());
        QTemporaryDir data, out;
        EngineConfig cfg; cfg.maxConcurrentDownloads = 2; cfg.segmentCount = 4; cfg.minSegSize = 1;
        cfg.progressThrottleMs = 1;
        DownloadManager mgr(cfg, data.path());

        // Seed a restored task ("Cbad") whose destPath can never be opened:
        // make the path itself an already-existing *directory* rather than a
        // plain file, so QFile::open(ReadWrite) inside beginSegments()
        // deterministically fails every time, with no timing dependency. It
        // gets a non-empty segment vector so restore() sets m_probed = true -
        // that's what lets start() skip the async HttpProbe and reach
        // beginSegments()/setState(Error) synchronously, inside pump()'s loop.
        const QString cbadDest = out.filePath("cbad_is_a_dir");
        QVERIFY(QDir().mkpath(cbadDest));
        DownloadRecord cbadRec;
        cbadRec.id = QUuid::createUuid();
        cbadRec.url = srv.url("/ranged");
        cbadRec.destPath = cbadDest;
        cbadRec.totalBytes = 1000;
        cbadRec.supportsRange = true;
        cbadRec.state = DownloadState::Paused;
        cbadRec.segmentCount = 1;
        QVector<Segment> cbadSegs = { {0, 0, 0, 999} };   // partial, non-empty -> m_probed = true
        QVERIFY(Persistence::writeMeta(cbadDest, cbadSegs, "", "", false));
        QVERIFY(Persistence::writeSession(data.path() + "/downloads.json", { cbadRec }));

        // Load Cbad first, so it sits first in mgr.tasks() - matches the
        // reviewer's reproduction order [Cbad, R1, R2, R3].
        mgr.loadSession();
        QCOMPARE(mgr.tasks().size(), 1);
        QCOMPARE(mgr.tasks().first()->state(), DownloadState::Paused);

        int peak = 0;
        auto trackPeak = [&] {
            int active = 0;
            for (auto* t : mgr.tasks())
                if (t->state() == DownloadState::Downloading || t->state() == DownloadState::Connecting)
                    ++active;
            if (active > peak) peak = active;
        };
        connect(&mgr, &DownloadManager::taskStateChanged, &mgr,
                [&](const QUuid&, DownloadState) { trackPeak(); });

        // Three genuine resumable tasks, added/paused one at a time exactly
        // like resumeAllRespectsMaxConcurrent (avoids the pauseAll()-vs-
        // still-probing race documented there). Waiting on state() ==
        // Downloading via QTRY (polling) is itself racy on a fast loopback:
        // a 5 MiB / 4-segment transfer can occasionally run Connecting ->
        // Downloading -> Completed between two ~50ms polls, so the poll
        // never observes "Downloading" and t->pause() is never reached
        // (reproduced directly: 2/12 runs). Waiting on the first
        // `segmentProgress` signal instead is not racy - it is unthrottled
        // (unlike `progress`) and fires on the first raw chunk any worker
        // receives, which is always well before that worker's segment - let
        // alone all of them - can finish, so the task is still definitely
        // Downloading the instant the wait returns and pause() runs.
        auto addWaitPause = [&](const QString& dest) {
            mgr.addDownload(srv.url("/ranged"), dest);
            DownloadTask* t = mgr.tasks().last();
            QSignalSpy segProg(t, &DownloadTask::segmentProgress);
            QVERIFY(segProg.wait(5000));
            QCOMPARE(t->state(), DownloadState::Downloading);
            t->pause();
            QTRY_COMPARE(t->state(), DownloadState::Paused);
        };
        addWaitPause(out.filePath("r1.bin"));
        addWaitPause(out.filePath("r2.bin"));
        addWaitPause(out.filePath("r3.bin"));
        QCOMPARE(mgr.tasks().size(), 4);

        mgr.resumeAll();

        QTRY_VERIFY_WITH_TIMEOUT(
            [&]{ int c=0; for (auto* t: mgr.tasks()) if (t->state()==DownloadState::Completed) ++c;
                 return c==3; }(), 15000);

        QCOMPARE(mgr.tasks().first()->state(), DownloadState::Error);   // Cbad: never resumable

        QVERIFY2(peak <= 2, qPrintable(QString("peak concurrent active tasks (%1) exceeded cap (2) "
                                                "across resumeAll() with a synchronously-terminal "
                                                "restored task").arg(peak)));

        for (auto* t : mgr.tasks()) {
            if (t == mgr.tasks().first()) continue;   // Cbad never wrote real bytes
            QFile f(t->record().destPath);
            QVERIFY(f.open(QIODevice::ReadOnly));
            QCOMPARE(f.readAll(), body);
        }
    }

    void restartProcessResumesFromDisk() {
        // NOTE (brief deviation, test-only): the brief's sample reuses the
        // suite-wide 5000-byte m_body and default progressThrottleMs (200ms)
        // here. On this machine's loopback, a whole 4-segment /ranged
        // transfer - measured up to several MiB - regularly completes
        // *inside a single QSignalSpy::wait() call*, faster than the 200ms
        // throttle window. checkAllComplete() bypasses the throttle and
        // fires the *final* `progress` signal immediately once the last
        // segment lands, so with the brief's config the first (and only)
        // `progress` emission `prog.wait()` observes is already the
        // completion signal - i.e. the task is already Completed by the
        // time pauseAll() runs. DownloadManager::pauseAll() correctly only
        // pauses tasks still Downloading/Connecting (it must not force-pause
        // a task that already finished), so it becomes a no-op and the
        // subsequent QTRY_COMPARE(..., Paused) deterministically fails - not
        // a rare flake, but a guaranteed race on fast loopback. Two changes
        // together make the mid-flight pause deterministic: a multi-MB body
        // (so the transfer has enough real bytes to still be in flight at
        // *some* point) and cfg.progressThrottleMs = 1 (so the throttled
        // `progress` signal is emitted almost as soon as the first bytes of
        // any segment arrive, rather than only at completion). Verified
        // received < total at the point `prog.wait()` returns, and state ==
        // Downloading when pauseAll() is called, across 15 repeated runs.
        // This changes only test data/timing, not any production interface
        // or behavior.
        QByteArray bigBody = makeBody(5 * 1024 * 1024);   // 5 MiB
        TestServer srv(bigBody);
        QVERIFY(srv.listen());
        QTemporaryDir data, out;
        const QString dest = out.filePath("resume.bin");
        EngineConfig cfg; cfg.segmentCount = 4; cfg.minSegSize = 1; cfg.progressThrottleMs = 1;

        {   // "session 1": start, pause mid-way, let manager persist session
            DownloadManager mgr(cfg, data.path());
            const QUuid id = mgr.addDownload(srv.url("/ranged"), dest);
            DownloadTask* t = mgr.tasks().first();
            QSignalSpy prog(t, &DownloadTask::progress);
            QVERIFY(prog.wait(3000));
            mgr.pauseAll();                 // writes downloads.json + .meta
            QTRY_COMPARE(t->state(), DownloadState::Paused);
            Q_UNUSED(id);
        }   // mgr destroyed == process restart

        {   // "session 2": fresh manager, load from disk, resume
            DownloadManager mgr2(cfg, data.path());
            mgr2.loadSession();
            QCOMPARE(mgr2.tasks().size(), 1);
            QCOMPARE(mgr2.tasks().first()->state(), DownloadState::Paused);
            mgr2.resumeAll();
            QTRY_COMPARE_WITH_TIMEOUT(mgr2.tasks().first()->state(),
                                      DownloadState::Completed, 6000);
        }
        QFile f(dest); QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(f.readAll(), bigBody);
    }

    // --- Task 2: per-id pause/resume/taskById on DownloadManager -------

    void pause_then_resume_via_manager_completes() {
        // 2 MiB body (matches the sizing other concurrency-sensitive tests
        // in this file settled on) so the task is reliably still
        // Downloading when mgr.pause(id) runs, instead of racing straight
        // to Completed on fast loopback.
        QByteArray body = makeBody(2 * 1024 * 1024);
        TestServer srv(body);
        QVERIFY(srv.listen());
        EngineConfig cfg; cfg.segmentCount = 4; cfg.maxConcurrentDownloads = 3;
        cfg.minSegSize = 1; cfg.progressThrottleMs = 1;
        QString dir = makeTempDir();
        DownloadManager mgr(cfg, dir);
        QUuid id = mgr.addDownload(srv.url("/ranged"), dir + "/out.bin");
        QVERIFY(waitForState(mgr, id, DownloadState::Downloading, 3000));

        mgr.pause(id);
        QVERIFY(waitForState(mgr, id, DownloadState::Paused, 3000));

        mgr.resume(id);
        QVERIFY(waitForState(mgr, id, DownloadState::Completed, 10000));
        QCOMPARE(readFile(dir + "/out.bin"), body);
    }

    void resume_respects_concurrency_cap() {
        // Finding 2 fix pass: the original version of this test (per the
        // brief's sample) could not have caught a resume() that bypasses the
        // concurrency cap. With cap=2 and 3 added downloads, pauseAll() only
        // ever pauses the (at most 2) tasks that reached
        // Downloading/Connecting - the 3rd stays Queued the whole time,
        // untouched by pauseAll() - so exactly 2 tasks get resumed and a
        // buggy resume() that called t->start() directly instead of routing
        // through pump() would still coincidentally land at exactly 2
        // concurrently-Downloading tasks (== cap), passing by accident.
        //
        // This version peak-tracks Downloading|Connecting across the WHOLE
        // per-id resume() loop with 3 GENUINELY Paused tasks under cap=2,
        // mirroring resumeAllRespectsMaxConcurrent /
        // resumeAllRespectsCapWithSyncTerminalTask elsewhere in this file: a
        // resume() that ever let more than `cap` tasks run concurrently
        // would be caught by peak exceeding 2, regardless of how the count
        // happens to look at any single poll afterward.
        QByteArray body = makeBody(5 * 1024 * 1024);
        TestServer srv(body);
        QVERIFY(srv.listen());
        QTemporaryDir data, out;
        EngineConfig cfg; cfg.maxConcurrentDownloads = 2; cfg.segmentCount = 4; cfg.minSegSize = 1;
        cfg.progressThrottleMs = 1;
        DownloadManager mgr(cfg, data.path());

        int peak = 0;
        auto trackPeak = [&] {
            int active = 0;
            for (auto* t : mgr.tasks())
                if (t->state() == DownloadState::Downloading || t->state() == DownloadState::Connecting)
                    ++active;
            if (active > peak) peak = active;
        };
        connect(&mgr, &DownloadManager::taskStateChanged, &mgr,
                [&](const QUuid&, DownloadState) { trackPeak(); });

        // Get three tasks genuinely into Paused, one at a time (add -> wait
        // for the first unthrottled segmentProgress -> pause()), exactly
        // like resumeAllRespectsCapWithSyncTerminalTask's addWaitPause -
        // avoids the race where a fast loopback transfer completes before a
        // polled QTRY ever observes it as Downloading.
        QVector<QUuid> ids;
        auto addWaitPause = [&](const QString& dest) {
            QUuid id = mgr.addDownload(srv.url("/ranged"), dest);
            DownloadTask* t = mgr.taskById(id);
            QSignalSpy segProg(t, &DownloadTask::segmentProgress);
            QVERIFY(segProg.wait(5000));
            QCOMPARE(t->state(), DownloadState::Downloading);
            t->pause();
            QTRY_COMPARE(t->state(), DownloadState::Paused);
            ids << id;
        };
        addWaitPause(out.filePath("r1.bin"));
        addWaitPause(out.filePath("r2.bin"));
        addWaitPause(out.filePath("r3.bin"));
        QCOMPARE(ids.size(), 3);
        for (auto* t : mgr.tasks()) QCOMPARE(t->state(), DownloadState::Paused);

        for (auto id : ids) mgr.resume(id);

        QTRY_VERIFY_WITH_TIMEOUT(
            [&]{ int c=0; for (auto* t: mgr.tasks()) if (t->state()==DownloadState::Completed) ++c;
                 return c==3; }(), 15000);

        QVERIFY2(peak <= 2, qPrintable(QString("peak concurrent active tasks (%1) exceeded cap (2) "
                                                "across a per-id resume() loop").arg(peak)));

        for (auto* t : mgr.tasks()) {
            QFile f(t->record().destPath);
            QVERIFY(f.open(QIODevice::ReadOnly));
            QCOMPARE(f.readAll(), body);
        }
    }

    void pause_holds_a_queued_download() {
        // Finding 1 fix pass: pausing a Queued download must genuinely HOLD
        // it. Before the fix, DownloadManager::pause(id) excluded Queued
        // from the states that call task->pause() (to avoid
        // DownloadTask::pause() writing a spurious empty .meta for a
        // never-started task), so pausing a Queued task was a no-op - the
        // task silently stayed Queued and pump() later auto-promoted it to
        // Downloading the instant a slot freed up, defeating the pause.
        //
        // maxConcurrentDownloads=1 with 2 added downloads guarantees the
        // 2nd stays Queued behind the 1st. Pause it while still Queued, then
        // let the 1st complete (freeing pump()'s only slot and triggering a
        // pump() pass via wire()'s stateChanged handler) and assert the
        // paused-while-queued task is still Paused, not auto-promoted to
        // Downloading/Completed.
        QByteArray body = makeBody(2 * 1024 * 1024);
        TestServer srv(body);
        QVERIFY(srv.listen());
        EngineConfig cfg; cfg.maxConcurrentDownloads = 1;
        cfg.segmentCount = 4; cfg.minSegSize = 1; cfg.progressThrottleMs = 1;
        QString dir = makeTempDir();
        DownloadManager mgr(cfg, dir);

        QUuid id1 = mgr.addDownload(srv.url("/ranged"), dir + "/q0.bin");
        QUuid id2 = mgr.addDownload(srv.url("/ranged"), dir + "/q1.bin");
        QVERIFY(waitForState(mgr, id1, DownloadState::Downloading, 3000));
        QCOMPARE(mgr.taskById(id2)->state(), DownloadState::Queued);

        mgr.pause(id2);   // pause while still Queued
        QCOMPARE(mgr.taskById(id2)->state(), DownloadState::Paused);

        // Free the only slot: let the 1st task run to completion. Each of
        // its stateChanged emissions (including the final Completed one)
        // triggers a pump() pass via wire() - a buggy pause(id) that left
        // id2 sitting at Queued would let this promote it right here.
        QVERIFY(waitForState(mgr, id1, DownloadState::Completed, 10000));
        QTest::qWait(200);   // let any pump() pass fully settle
        QCOMPARE(mgr.taskById(id2)->state(), DownloadState::Paused);   // still held, NOT Downloading

        // Sanity: resume(id2) still works normally from here.
        mgr.resume(id2);
        QVERIFY(waitForState(mgr, id2, DownloadState::Completed, 10000));
        QCOMPARE(readFile(dir + "/q1.bin"), body);
    }

    void pause_resume_unknown_id_is_noop() {
        EngineConfig cfg; QString dir = makeTempDir();
        DownloadManager mgr(cfg, dir);
        mgr.pause(QUuid::createUuid());   // must not crash
        mgr.resume(QUuid::createUuid());  // must not crash
        QVERIFY(true);
    }

    // --- Task 6: DownloadManager::setConfig() raises the concurrency cap live ---

    void setConfigRaisesConcurrencyCapLive() {
        TestServer srv(m_body);
        QVERIFY(srv.listen());
        QTemporaryDir dir;
        EngineConfig cfg; cfg.maxConcurrentDownloads = 1; cfg.segmentCount = 1; cfg.minSegSize = 1;
        DownloadManager mgr(cfg, dir.path());
        const QUuid a = mgr.addDownload(srv.url("/ranged"), dir.filePath("a.bin"));
        const QUuid b = mgr.addDownload(srv.url("/ranged"), dir.filePath("b.bin"));
        Q_UNUSED(a);
        // com cap=1, 'b' fica Queued (ou completa muito depois); subir o cap deve
        // permitir ambos concluírem.
        EngineConfig hi = cfg; hi.maxConcurrentDownloads = 4;
        mgr.setConfig(hi);
        DownloadTask* tb = mgr.taskById(b);
        QVERIFY(tb);
        QTRY_COMPARE_WITH_TIMEOUT(tb->state(), DownloadState::Completed, 5000);
    }

    // --- Task 7: throttle do worker HTTP via RateLimiter ---

    // --- Task 3: cancel() discards the partial and restarts from Cancelled ---

    void cancelDiscardsPartialAndKeepsItemCancelled() {
        const QByteArray big = makeBody(300000);
        TestServer srv(big);
        QVERIFY(srv.listen());
        const QString dir = makeTempDir();
        EngineConfig cfg;
        cfg.maxBytesPerSec = 50000;                 // throttle: segura em Downloading
        DownloadManager mgr(cfg, dir);
        const QUuid id = mgr.addDownload(srv.url("/ranged"), dir + "/movie.bin");
        QVERIFY(waitForState(mgr, id, DownloadState::Downloading, 5000));
        QVERIFY(QFile::exists(dir + "/movie.bin"));                 // pré-alocado ao iniciar
        mgr.cancel(id);
        DownloadTask* t = mgr.taskById(id);
        QVERIFY(t);
        QCOMPARE(t->state(), DownloadState::Cancelled);            // fica na lista
        QVERIFY(!QFile::exists(dir + "/movie.bin"));               // parcial apagado
        QVERIFY(!QFile::exists(Persistence::metaPath(dir + "/movie.bin")));
    }

    void startFromCancelledRestartsFromZero() {
        const QByteArray big = makeBody(300000);
        TestServer srv(big);
        QVERIFY(srv.listen());
        const QString dir = makeTempDir();
        EngineConfig cfg;
        cfg.maxBytesPerSec = 50000;
        DownloadManager mgr(cfg, dir);
        const QUuid id = mgr.addDownload(srv.url("/ranged"), dir + "/m.bin");
        QVERIFY(waitForState(mgr, id, DownloadState::Downloading, 5000));
        mgr.cancel(id);
        QCOMPARE(mgr.taskById(id)->state(), DownloadState::Cancelled);
        mgr.resume(id);                                            // Start de Cancelled
        QVERIFY(waitForState(mgr, id, DownloadState::Completed, 15000));  // throttle ~6s
        QCOMPARE(readFile(dir + "/m.bin"), big);                   // baixou tudo de novo
    }

    // Cancelar durante Connecting (probe em voo): o resultado do probe NÃO pode
    // ressuscitar o download. Logo após addDownload a task está Connecting com o
    // probe assíncrono ainda pendente; cancelamos antes de girar o event loop.
    void cancelDuringConnectingStaysCancelled() {
        TestServer srv(m_body);
        QVERIFY(srv.listen());
        const QString dir = makeTempDir();
        EngineConfig cfg;
        DownloadManager mgr(cfg, dir);
        const QUuid id = mgr.addDownload(srv.url("/ranged"), dir + "/movie.bin");
        QCOMPARE(mgr.taskById(id)->state(), DownloadState::Connecting);  // probe em voo
        mgr.cancel(id);
        QCOMPARE(mgr.taskById(id)->state(), DownloadState::Cancelled);
        QTest::qWait(1000);   // deixa o probe pendente disparar onProbed
        QCOMPARE(mgr.taskById(id)->state(), DownloadState::Cancelled);   // não reviveu
        QVERIFY(!QFile::exists(dir + "/movie.bin"));                     // arquivo não recriado
    }

    // --- Task 4: setPriority() + priority-ordered pump() -------------------

    void pumpPromotesByPriority() {
        // Corpo menor + timeout folgado: com maxConcurrent=1 os 3 downloads
        // drenam em série (~7s a 50 KB/s); antes eram 300 KB (~18s) contra um
        // timeout de 20s — margem apertada em máquina carregada.
        const QByteArray big = makeBody(120000);
        TestServer srv(big);
        QVERIFY(srv.listen());
        const QString dir = makeTempDir();
        EngineConfig cfg;
        cfg.maxConcurrentDownloads = 1;         // 1 ativo por vez -> ordem importa
        cfg.maxBytesPerSec = 50000;             // 'a' segura Downloading enquanto ajustamos
        DownloadManager mgr(cfg, dir);
        const QUuid a = mgr.addDownload(srv.url("/ranged"), dir + "/a.bin");
        QVERIFY(waitForState(mgr, a, DownloadState::Downloading, 5000));
        const QUuid b = mgr.addDownload(srv.url("/ranged"), dir + "/b.bin");   // Queued
        const QUuid c = mgr.addDownload(srv.url("/ranged"), dir + "/c.bin");   // Queued
        mgr.setPriority(b, Priority::Low);
        mgr.setPriority(c, Priority::High);
        // Registra a ORDEM em que b e c entram em Downloading.
        QSignalSpy spy(&mgr, &DownloadManager::taskStateChanged);
        // Deixa a fila drenar (a -> c -> b).
        QVERIFY(waitForState(mgr, b, DownloadState::Completed, 30000));
        QVERIFY(waitForState(mgr, c, DownloadState::Completed, 30000));
        // Procura o primeiro Downloading de b e de c no histórico do spy.
        int idxC = -1, idxB = -1;
        for (int i = 0; i < spy.count(); ++i) {
            const QUuid id = spy.at(i).at(0).value<QUuid>();
            const auto st = spy.at(i).at(1).value<DownloadState>();
            if (st != DownloadState::Downloading) continue;
            if (id == c && idxC < 0) idxC = i;
            if (id == b && idxB < 0) idxB = i;
        }
        QVERIFY(idxC >= 0 && idxB >= 0);
        QVERIFY2(idxC < idxB, "High-priority c must be promoted before Low-priority b");
    }

    void throttledDownloadStillCompletesIntact() {
        // Corpo grande o bastante para que o drain sob throttle abranja
        // vários ticks de 20ms - isso expõe a corrida do short-read
        // espúrio em onFinished() se os bytes restantes no buffer do
        // reply não forem drenados antes da checagem.
        const QByteArray big = makeBody(256 * 1024);
        TestServer srv(big);
        QVERIFY(srv.listen());
        QTemporaryDir dir;
        const QString dest = dir.filePath("cap.bin");
        EngineConfig cfg; cfg.segmentCount = 4; cfg.minSegSize = 1;
        cfg.maxBytesPerSec = 64 * 1024;          // teto baixo, mas download deve terminar
        DownloadManager mgr(cfg, dir.path());
        const QUuid id = mgr.addDownload(srv.url("/ranged"), dest);
        DownloadTask* t = mgr.taskById(id);
        QVERIFY(t);
        QTRY_COMPARE_WITH_TIMEOUT(t->state(), DownloadState::Completed, 20000);
        QFile f(dest); QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(f.readAll(), big);              // conteúdo íntegro sob throttle
    }

    void moveFilesRelocatesAndUpdatesDest() {
        TestServer srv(m_body);
        QVERIFY(srv.listen());
        const QString dir = makeTempDir();
        const QString dest2 = makeTempDir();          // pasta destino do move
        EngineConfig cfg;
        DownloadManager mgr(cfg, dir);
        const QUuid id = mgr.addDownload(srv.url("/ranged"), dir + "/movie.bin");
        QVERIFY(waitForState(mgr, id, DownloadState::Completed, 5000));
        QVERIFY(mgr.moveFiles(id, dest2));
        QVERIFY(!QFile::exists(dir + "/movie.bin"));            // saiu da origem
        QVERIFY(QFile::exists(dest2 + "/movie.bin"));           // chegou ao destino
        QCOMPARE(mgr.taskById(id)->record().destPath, dest2 + "/movie.bin");
    }

    // Mover para a MESMA pasta é no-op: não pode renomear para "movie (1).bin".
    void moveFilesToSameDirIsNoOp() {
        TestServer srv(m_body);
        QVERIFY(srv.listen());
        const QString dir = makeTempDir();
        EngineConfig cfg;
        DownloadManager mgr(cfg, dir);
        const QUuid id = mgr.addDownload(srv.url("/ranged"), dir + "/movie.bin");
        QVERIFY(waitForState(mgr, id, DownloadState::Completed, 5000));
        QVERIFY(mgr.moveFiles(id, dir));                        // mesma pasta
        QCOMPARE(mgr.taskById(id)->record().destPath, dir + "/movie.bin");  // nome intacto
        QVERIFY(QFile::exists(dir + "/movie.bin"));
        QVERIFY(!QFile::exists(dir + "/movie (1).bin"));        // não duplicou
    }

    void moveFilesRefusedWhileActive() {
        const QByteArray big = makeBody(300000);
        TestServer srv(big);
        QVERIFY(srv.listen());
        const QString dir = makeTempDir();
        const QString dest2 = makeTempDir();
        EngineConfig cfg;
        cfg.maxBytesPerSec = 50000;                            // segura em Downloading
        DownloadManager mgr(cfg, dir);
        const QUuid id = mgr.addDownload(srv.url("/ranged"), dir + "/m.bin");
        QVERIFY(waitForState(mgr, id, DownloadState::Downloading, 5000));
        QCOMPARE(mgr.moveFiles(id, dest2), false);            // recusado enquanto ativo
        QVERIFY(QFile::exists(dir + "/m.bin"));               // não moveu
    }

    void moveFilesRelocatesMetaWhenPaused() {
        // Review gap: the two existing moveFiles tests above never exercise
        // the .meta-relocation branch — moveFilesRelocatesAndUpdatesDest
        // moves a Completed task (whose .meta was already deleted on
        // completion), and moveFilesRefusedWhileActive returns early before
        // ever reaching the rename logic. This test pauses a genuinely
        // in-flight download (writing a real, non-empty .meta per
        // DownloadTask::pause()) and then moves it, proving both the file
        // AND its .meta sidecar relocate together.
        const QByteArray big = makeBody(300000);
        TestServer srv(big);
        QVERIFY(srv.listen());
        const QString dir = makeTempDir();
        const QString dest2 = makeTempDir();
        EngineConfig cfg;
        cfg.maxBytesPerSec = 50000;                 // segura em Downloading
        DownloadManager mgr(cfg, dir);
        const QUuid id = mgr.addDownload(srv.url("/ranged"), dir + "/movie.bin");
        QVERIFY(waitForState(mgr, id, DownloadState::Downloading, 5000));
        mgr.pause(id);                              // grava o .meta
        QVERIFY(waitForState(mgr, id, DownloadState::Paused, 5000));
        const QString oldMeta = Persistence::metaPath(dir + "/movie.bin");
        QVERIFY(QFile::exists(oldMeta));            // pré-condição: .meta existe
        QVERIFY(mgr.moveFiles(id, dest2));
        QVERIFY(!QFile::exists(dir + "/movie.bin"));            // arquivo saiu
        QVERIFY(!QFile::exists(oldMeta));                       // .meta saiu
        QVERIFY(QFile::exists(dest2 + "/movie.bin"));           // arquivo chegou
        QVERIFY(QFile::exists(Persistence::metaPath(dest2 + "/movie.bin")));  // .meta chegou
    }

    // --- Task 3 (Fase 5): extra headers (Cookie/Referer/User-Agent) reach the wire ---

    void extraHeadersReachTheWire() {
        TestServer srv(makeBody(3 * 1024 * 1024));
        QVERIFY(srv.listen());
        QTemporaryDir dir;
        EngineConfig cfg;
        DownloadManager mgr(cfg, dir.path());
        const HeaderList hdrs = {
            {QByteArray("Cookie"),     QByteArray("sid=zzz")},
            {QByteArray("Referer"),    QByteArray("https://ref.example/p")},
            {QByteArray("User-Agent"), QByteArray("BrowserUA/9")},
        };
        const QString dest = dir.filePath("out.bin");
        mgr.addDownload(srv.url("/ranged"), dest, hdrs);
        QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(dest) &&
                                  QFileInfo(dest).size() == 3 * 1024 * 1024, 10000);
        QVERIFY(srv.cookiesSeen().contains("sid=zzz"));
        QVERIFY(srv.referersSeen().contains("https://ref.example/p"));
        QVERIFY(srv.userAgentsSeen().contains("BrowserUA/9"));       // override do default
        QVERIFY(!srv.userAgentsSeen().contains("curl/8.7.1"));       // default NÃO usado
    }

    // Fase 5 (fix): downloads com nome PROVISÓRIO (browser/clipboard/drag) adotam
    // o filename do Content-Disposition quando o servidor informa. Aqui a URL
    // "/named" tem path que derivaria um nome ruim, mas o servidor manda
    // Content-Disposition: filename="Audiobook.m4a" -> o destino final vira esse.
    void provisionalNameAdoptsContentDisposition() {
        TestServer srv(m_body);
        QVERIFY(srv.listen());
        QTemporaryDir dir;
        DownloadManager mgr(EngineConfig{}, dir.path());
        const QUuid id = mgr.addDownload(srv.url("/named"), dir.filePath("download"),
                                         HeaderList{}, /*provisionalName=*/true);
        QVERIFY(waitForState(mgr, id, DownloadState::Completed, 5000));
        QCOMPARE(QFileInfo(mgr.taskById(id)->record().destPath).fileName(),
                 QString("Audiobook.m4a"));
        QVERIFY(QFile::exists(dir.filePath("Audiobook.m4a")));
    }

    // O diálogo New passa provisionalName=false (padrão): o nome que o usuário
    // escolheu é mantido, mesmo que o servidor mande um Content-Disposition.
    void nonProvisionalKeepsGivenName() {
        TestServer srv(m_body);
        QVERIFY(srv.listen());
        QTemporaryDir dir;
        DownloadManager mgr(EngineConfig{}, dir.path());
        const QUuid id = mgr.addDownload(srv.url("/named"), dir.filePath("myname.bin"),
                                         HeaderList{}, /*provisionalName=*/false);
        QVERIFY(waitForState(mgr, id, DownloadState::Completed, 5000));
        QCOMPARE(QFileInfo(mgr.taskById(id)->record().destPath).fileName(),
                 QString("myname.bin"));
    }

    void completedDownloadWritesTaskLog() {
        TestServer srv(m_body);
        QVERIFY(srv.listen());
        const QString dir = makeTempDir();
        EngineConfig cfg;
        Logger logger(dir);
        DownloadManager mgr(cfg, dir, &logger);
        const QUuid id = mgr.addDownload(srv.url("/ranged"), dir + "/movie.bin");
        QVERIFY(waitForState(mgr, id, DownloadState::Completed, 5000));
        const QString logContent = readFile(logger.taskLogPath(id, dir + "/movie.bin"));
        QVERIFY(logContent.contains("Downloading"));    // logou a transição
        QVERIFY(logContent.contains("Completed"));
    }
};

QTEST_MAIN(TstDownload)
#include "tst_download.moc"
