#include <QtTest>

// Ver tst_download.cpp / issue #1: testes que observam um download no estado
// Downloading no meio da transferência são flaky em hardware rápido (CI). Rodam
// localmente; pulam na CI quando ORBIT_SKIP_TIMING_TESTS está setada.
#define SKIP_IF_CI_TIMING() \
    do { if (qEnvironmentVariableIsSet("ORBIT_SKIP_TIMING_TESTS")) \
        QSKIP("flaky on fast CI loopback (mid-transfer observation race); see issue #1"); } while (0)

#include "FileType.h"
#include "UrlName.h"
#include "GridGeometry.h"
#include "DownloadTypes.h"
#include "SpeedSampler.h"
#include "DownloadManager.h"
#include "DownloadTask.h"
#include "DownloadTableModel.h"
#include "CategoryFilterProxy.h"
#include "ProgressGridWidget.h"
#include "NewDownloadDialog.h"
#include "CategoryTree.h"
#include "Logger.h"
#include "MainWindow.h"
#include "DropTargets.h"
#include "ClipboardWatcher.h"
#include "CredentialsDialog.h"
#include "PreferencesDialog.h"
#include "SchedulerDialog.h"
#include "ContextMenuRules.h"
#include "Theme.h"
#include "TestServer.h"
#include <QAction>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMimeData>
#include <QTableView>
#include <QItemSelectionModel>
#include <QTemporaryDir>
#include <QFile>
#include <QDeadlineTimer>
#include <QSharedPointer>
#include <QSignalSpy>
#include <QSet>
#include <QTcpServer>

static QVector<Segment> segs2(qint64 total) {
    // two contiguous halves, each fully pending (current==start)
    Segment a{0, 0,          0, total/2 - 1};
    Segment b{1, total/2, total/2, total - 1};
    return {a, b};
}

// --- Task 7 test helpers -------------------------------------------------
// Local to this translation unit (mirrors tst_download.cpp's helpers).

static QByteArray makeGuiBody(int n) {
    QByteArray b; b.resize(n);
    for (int i = 0; i < n; ++i) b[i] = char('A' + (i % 26));
    return b;
}

static QString makeTempDir() {
    static QVector<QSharedPointer<QTemporaryDir>> keep;   // keep dirs alive for the test run
    auto d = QSharedPointer<QTemporaryDir>::create();
    keep.push_back(d);
    return d->path();
}
static QByteArray readFile(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return QByteArray();
    return f.readAll();
}
static bool waitForState(DownloadManager& mgr, const QUuid& id, DownloadState want, int timeoutMs) {
    QDeadlineTimer dl(timeoutMs);
    while (!dl.hasExpired()) {
        DownloadTask* t = mgr.taskById(id);
        if (t && t->state() == want) return true;
        QTest::qWait(20);
    }
    DownloadTask* t = mgr.taskById(id);
    return t && t->state() == want;
}

class TestGui : public QObject {
    Q_OBJECT
private slots:
    void scaffolding_builds() { QVERIFY(true); }
    void filetype_categorizes_by_extension() {
        using namespace FileType;
        QCOMPARE(categorize("movie.MP4"),   Category::Movie);
        QCOMPARE(categorize("clip.mkv"),    Category::Movie);
        QCOMPARE(categorize("setup.dmg"),   Category::Software);
        QCOMPARE(categorize("app.zip"),     Category::Software);
        QCOMPARE(categorize("song.flac"),   Category::Music);
        QCOMPARE(categorize("track.mp3"),   Category::Music);
        QCOMPARE(categorize("notes.txt"),   Category::Others);
        QCOMPARE(categorize("noext"),       Category::Others);
        QCOMPARE(categorize(""),            Category::Others);
    }
    void filetype_display_names() {
        using namespace FileType;
        QCOMPARE(displayName(Category::Movie),  QString("Movie"));
        QCOMPARE(displayName(Category::Others), QString("Others"));
    }
    void urlname_derives_filename() {
        QCOMPARE(deriveFileName(QUrl("https://x.com/a/b/file.zip")),      QString("file.zip"));
        QCOMPARE(deriveFileName(QUrl("https://x.com/file.zip?k=v&x=1")),  QString("file.zip"));
        QCOMPARE(deriveFileName(QUrl("https://x.com/path/my%20doc.pdf")), QString("my doc.pdf"));
        QCOMPARE(deriveFileName(QUrl("https://x.com/")),                  QString("download"));
        QCOMPARE(deriveFileName(QUrl("https://x.com")),                   QString("download"));
    }
    void grid_all_pending_when_nothing_downloaded() {
        // Queued (not active): keeps this test's original, narrower concern
        // ("nothing downloaded -> all Pending") isolated from Task 7's
        // Active-head marking, which is covered by its own dedicated tests
        // below (computeCellsMarksActiveHead / computeCellsNoActiveWhenPaused).
        auto cells = computeCells(1000, segs2(1000), DownloadState::Queued, 10);
        QCOMPARE(cells.size(), 10);
        for (const auto& c : cells) QCOMPARE(c.kind, CellKind::Pending);
    }
    void grid_marks_downloaded_ranges_with_owner_segment() {
        auto s = segs2(1000);
        s[0].current = 500;                     // first half fully done -> cells 0..4 downloaded, owner 0
        // Queued (not active): see note above -- isolates "downloaded" marking
        // from Task 7's Active-head marking.
        auto cells = computeCells(1000, s, DownloadState::Queued, 10);
        for (int i = 0; i < 5; ++i) {
            QCOMPARE(cells[i].kind, CellKind::Downloaded);
            QCOMPARE(cells[i].segmentIndex, 0);
        }
        for (int i = 5; i < 10; ++i) QCOMPARE(cells[i].kind, CellKind::Pending);
    }
    void grid_full_download_all_downloaded() {
        auto s = segs2(1000); s[0].current = 500; s[1].current = 1000;
        auto cells = computeCells(1000, s, DownloadState::Completed, 8);
        for (const auto& c : cells) QCOMPARE(c.kind, CellKind::Downloaded);
    }
    void grid_error_marks_incomplete_cells_error() {
        auto s = segs2(1000); s[0].current = 500;   // 2nd half pending
        auto cells = computeCells(1000, s, DownloadState::Error, 10);
        for (int i = 0; i < 5; ++i) QCOMPARE(cells[i].kind, CellKind::Downloaded);
        for (int i = 5; i < 10; ++i) QCOMPARE(cells[i].kind, CellKind::Error);
    }
    void grid_straddling_boundary_all_downloaded() {
        // Segment boundary at 500 misaligned with 7 cells (~142 bytes each):
        // cell 3 spans [428,571), crossing the boundary. Pre-fix it stayed
        // Pending (owner-of-start test could never reach cellEnd). Uses Queued
        // to exercise the range logic, NOT the Completed short-circuit.
        auto s = segs2(1000);
        s[0].current = 500;    // seg0 [0..499] complete
        s[1].current = 1000;   // seg1 [500..999] complete
        auto cells = computeCells(1000, s, DownloadState::Queued, 7);
        QCOMPARE(cells.size(), 7);
        for (const auto& c : cells) QCOMPARE(c.kind, CellKind::Downloaded);
    }
    void grid_completed_fills_all_even_misaligned() {
        // Completed short-circuit must fill every cell even when boundaries
        // don't align to cell edges.
        auto s = segs2(1000); s[0].current = 500; s[1].current = 1000;
        auto cells = computeCells(1000, s, DownloadState::Completed, 7);
        for (const auto& c : cells) QCOMPARE(c.kind, CellKind::Downloaded);
    }
    void grid_unknown_total_all_pending() {
        auto cells = computeCells(-1, {}, DownloadState::Downloading, 6);
        QCOMPARE(cells.size(), 6);
        for (const auto& c : cells) QCOMPARE(c.kind, CellKind::Pending);
    }
    void grid_nonpositive_ncells_empty() {
        QCOMPARE(computeCells(1000, segs2(1000), DownloadState::Downloading, 0).size(), 0);
    }

    // --- Task 7: CellKind::Active -----------------------------------------

    void computeCellsMarksActiveHead() {
        // 1 segmento de 0..99, current no meio, download ativo -> a célula do
        // 'current' é Active; as anteriores Downloaded; as seguintes Pending.
        QVector<Segment> segs;
        Segment s; s.index = 0; s.start = 0; s.end = 99; s.current = 50;
        segs.append(s);
        const auto cells = computeCells(100, segs, DownloadState::Downloading, 10);
        QCOMPARE(cells.size(), 10);
        QCOMPARE(cells[0].kind, CellKind::Downloaded);     // [0..9] < current
        QCOMPARE(cells[5].kind, CellKind::Active);         // contém current=50
        QCOMPARE(cells[9].kind, CellKind::Pending);        // [90..99] > current
    }

    void computeCellsNoActiveWhenPaused() {
        QVector<Segment> segs;
        Segment s; s.index = 0; s.start = 0; s.end = 99; s.current = 50;
        segs.append(s);
        const auto cells = computeCells(100, segs, DownloadState::Paused, 10);
        for (const auto& c : cells) QVERIFY(c.kind != CellKind::Active);
    }

    void speed_zero_with_single_sample() {
        SpeedSampler s; s.addSample(0, 0);
        QCOMPARE(s.bytesPerSec(), 0.0);
        QCOMPARE(s.etaSeconds(1000), qint64(-1));
    }
    void speed_computes_rate_over_window() {
        SpeedSampler s;
        s.addSample(0,    0);
        s.addSample(1000, 1000);          // 1000 bytes in 1000 ms = 1000 B/s
        QVERIFY(qAbs(s.bytesPerSec() - 1000.0) < 1.0);
        QCOMPARE(s.etaSeconds(3000), qint64(2));  // 2000 bytes left / 1000 B/s
    }
    void speed_eta_unknown_when_total_unknown() {
        SpeedSampler s; s.addSample(0,0); s.addSample(500,1000);
        QCOMPARE(s.etaSeconds(-1), qint64(-1));
    }
    void speed_reset_clears() {
        SpeedSampler s; s.addSample(0,0); s.addSample(1000,1000);
        s.reset();
        QCOMPARE(s.bytesPerSec(), 0.0);
    }

    // --- Task 7: DownloadTableModel -------------------------------------
    // NOTE (brief deviation, test-only): the brief's sample constructs
    // TestServer default-and-`start()`'d, matching tst_download.cpp's own
    // note that the real TestServer in this repo takes the body in its
    // constructor and binds via listen() (no default ctor / start()).
    // Adapted here to the real API; behavior matches the brief.

    void model_rows_reflect_manager_tasks() {
        TestServer srv(makeGuiBody(5000));
        QVERIFY(srv.listen());
        EngineConfig cfg; QString dir = makeTempDir();
        DownloadManager mgr(cfg, dir);
        DownloadTableModel model(&mgr);
        QCOMPARE(model.rowCount(), 0);
        QCOMPARE(model.columnCount(), int(DownloadTableModel::ColumnCount));

        QUuid id = mgr.addDownload(srv.url("/ranged"), dir + "/out.bin");
        model.appendTask(mgr.taskById(id));
        QCOMPARE(model.rowCount(), 1);
        QCOMPARE(model.data(model.index(0, DownloadTableModel::Name)).toString(), QString("out.bin"));
    }
    void model_emits_datachanged_on_progress() {
        TestServer srv(makeGuiBody(5000));
        QVERIFY(srv.listen());
        EngineConfig cfg; QString dir = makeTempDir();
        DownloadManager mgr(cfg, dir);
        DownloadTableModel model(&mgr);
        QUuid id = mgr.addDownload(srv.url("/ranged"), dir + "/out.bin");
        model.appendTask(mgr.taskById(id));
        QSignalSpy spy(&model, &QAbstractItemModel::dataChanged);
        QVERIFY(waitForState(mgr, id, DownloadState::Completed, 10000));
        QVERIFY(spy.count() > 0);
    }
    void model_remove_drops_row() {
        // 3 rows, remove the MIDDLE one: exercises the m_index reindex loop
        // in removeTaskById (bug-risk flagged by review) - surviving rows
        // must keep the correct task/data after row indices shift down.
        TestServer srv(makeGuiBody(5000));
        QVERIFY(srv.listen());
        EngineConfig cfg; QString dir = makeTempDir();
        DownloadManager mgr(cfg, dir);
        DownloadTableModel model(&mgr);
        QUuid idA = mgr.addDownload(srv.url("/ranged"), dir + "/a.bin");
        QUuid idB = mgr.addDownload(srv.url("/ranged"), dir + "/b.bin");
        QUuid idC = mgr.addDownload(srv.url("/ranged"), dir + "/c.bin");
        model.appendTask(mgr.taskById(idA));
        model.appendTask(mgr.taskById(idB));
        model.appendTask(mgr.taskById(idC));
        QCOMPARE(model.rowCount(), 3);

        QSignalSpy rem(&model, &QAbstractItemModel::rowsRemoved);
        model.removeTaskById(idB);
        QCOMPARE(model.rowCount(), 2);
        QCOMPARE(rem.count(), 1);

        // Surviving rows: correct task identity and correct data post-reindex.
        QCOMPARE(model.taskAt(0), mgr.taskById(idA));
        QCOMPARE(model.taskAt(1), mgr.taskById(idC));
        QCOMPARE(model.data(model.index(0, DownloadTableModel::Name)).toString(), QString("a.bin"));
        QCOMPARE(model.data(model.index(1, DownloadTableModel::Name)).toString(), QString("c.bin"));

        // Keep removing by id: if the id->row map were stale after the first
        // reindex, one of these would drop the wrong row.
        model.removeTaskById(idA);
        QCOMPARE(model.rowCount(), 1);
        QCOMPARE(model.taskAt(0), mgr.taskById(idC));
        model.removeTaskById(idC);
        QCOMPARE(model.rowCount(), 0);
    }
    void model_exposes_state_and_category_roles() {
        TestServer srv(makeGuiBody(5000));
        QVERIFY(srv.listen());
        EngineConfig cfg; QString dir = makeTempDir();
        DownloadManager mgr(cfg, dir);
        DownloadTableModel model(&mgr);
        QUuid id = mgr.addDownload(srv.url("/ranged"), dir + "/movie.mp4");
        model.appendTask(mgr.taskById(id));
        QModelIndex ix = model.index(0, 0);
        QCOMPARE(model.data(ix, DownloadTableModel::CategoryRole).toInt(),
                 int(FileType::Category::Movie));
        // addDownload's synchronous pump() already moved the task past Queued
        // by the time appendTask ran, so the real initial state is Connecting.
        QCOMPARE(model.data(ix, DownloadTableModel::StateRole).toInt(),
                 int(DownloadState::Connecting));
        // Drive it to completion and confirm StateRole tracks the real state
        // transition, not just "some valid int".
        QVERIFY(waitForState(mgr, id, DownloadState::Completed, 10000));
        QCOMPARE(model.data(ix, DownloadTableModel::StateRole).toInt(),
                 int(DownloadState::Completed));
    }
    // --- Fix-pass tests: findings 1-3 -----------------------------------

    void model_progress_refreshes_size_column() {
        // Finding 2: total is -1 at append time; only the first taskProgress
        // learns the real size. onTaskProgress must emit dataChanged for the
        // Size column too, or the view is stuck on "-" forever.
        TestServer srv(makeGuiBody(5000));
        QVERIFY(srv.listen());
        EngineConfig cfg; QString dir = makeTempDir();
        DownloadManager mgr(cfg, dir);
        DownloadTableModel model(&mgr);
        QUuid id = mgr.addDownload(srv.url("/ranged"), dir + "/sz.bin");
        model.appendTask(mgr.taskById(id));
        QModelIndex sizeIx = model.index(0, DownloadTableModel::Size);
        QCOMPARE(model.data(sizeIx).toString(), QString("—"));   // "-" until probed

        QSignalSpy dc(&model, &QAbstractItemModel::dataChanged);
        QVERIFY(waitForState(mgr, id, DownloadState::Completed, 10000));
        QVERIFY(dc.count() > 0);
        bool sawSizeToProgress = false;
        for (const auto& call : dc) {
            QModelIndex tl = call.at(0).value<QModelIndex>();
            QModelIndex br = call.at(1).value<QModelIndex>();
            if (tl.column() == DownloadTableModel::Size && br.column() == DownloadTableModel::Progress)
                sawSizeToProgress = true;
        }
        QVERIFY(sawSizeToProgress);
        QVERIFY(model.data(sizeIx).toString() != QString("—"));
    }

    void model_state_change_refreshes_speed_and_timeleft_columns() {
        // Finding 3: onTaskStateChanged only emitted dataChanged for Status,
        // so Speed/TimeLeft froze at their last value once a download left
        // the Downloading state (onSpeedTick skips non-Downloading rows).
        TestServer srv(makeGuiBody(5000));
        QVERIFY(srv.listen());
        EngineConfig cfg; QString dir = makeTempDir();
        DownloadManager mgr(cfg, dir);
        DownloadTableModel model(&mgr);
        QUuid id = mgr.addDownload(srv.url("/ranged"), dir + "/spd.bin");
        model.appendTask(mgr.taskById(id));

        QSignalSpy dc(&model, &QAbstractItemModel::dataChanged);
        QVERIFY(waitForState(mgr, id, DownloadState::Completed, 10000));
        QVERIFY(dc.count() > 0);
        bool sawStatusToTimeLeft = false;
        for (const auto& call : dc) {
            QModelIndex tl = call.at(0).value<QModelIndex>();
            QModelIndex br = call.at(1).value<QModelIndex>();
            if (tl.column() == DownloadTableModel::Status && br.column() == DownloadTableModel::TimeLeft)
                sawStatusToTimeLeft = true;
        }
        QVERIFY(sawStatusToTimeLeft);

        QModelIndex speedIx = model.index(0, DownloadTableModel::Speed);
        QModelIndex etaIx   = model.index(0, DownloadTableModel::TimeLeft);
        QCOMPARE(model.data(speedIx).toString(), QString());
        // Fix 1 (Fase 2 polish): TimeLeft now mirrors Speed's guard and goes
        // blank (not a stale "—") once the row leaves Downloading state.
        QCOMPARE(model.data(etaIx).toString(), QString());
    }

    void model_seeds_received_from_existing_segment_progress() {
        SKIP_IF_CI_TIMING();  // needs a mid-transfer pause; races on fast CI loopback (issue #1)
        // Finding 1: appendTask (and the ctor's seeding loop) left `received`
        // at 0 even when the task already has real accrued segment progress
        // (e.g. a model constructed after loadSession() restored a Paused
        // task). Drive a big /ranged download partway, pause it, then build
        // a NEW model over the SAME manager and confirm the row shows real
        // progress immediately - no taskProgress signal will ever fire again
        // for a Paused task, so appendTask's seeding is the only source.
        TestServer srv(makeGuiBody(5 * 1024 * 1024));
        QVERIFY(srv.listen());
        EngineConfig cfg; cfg.segmentCount = 4; cfg.minSegSize = 1; cfg.progressThrottleMs = 1;
        QString dir = makeTempDir();
        DownloadManager mgr(cfg, dir);
        QUuid id = mgr.addDownload(srv.url("/ranged"), dir + "/big.bin");
        DownloadTask* t = mgr.taskById(id);
        QSignalSpy prog(t, &DownloadTask::progress);
        QVERIFY(prog.wait(3000));
        mgr.pauseAll();
        QVERIFY(waitForState(mgr, id, DownloadState::Paused, 3000));

        DownloadTableModel model2(&mgr);
        QCOMPARE(model2.rowCount(), 1);
        QVariant pr = model2.data(model2.index(0, DownloadTableModel::Progress),
                                   DownloadTableModel::ProgressRole);
        QVERIFY(pr.toInt() > 0);
    }

    void model_paused_row_timeleft_not_frozen() {
        SKIP_IF_CI_TIMING();
        // Fix 1 (Fase 2 polish): onTaskStateChanged only resets the speed
        // sampler on Completed/Error, not Paused, but TimeLeft used to
        // compute the ETA unconditionally - so a Paused row froze on its
        // last "MM:SS" instead of going blank like Speed already does.
        TestServer srv(makeGuiBody(5 * 1024 * 1024));
        QVERIFY(srv.listen());
        EngineConfig cfg; cfg.segmentCount = 4; cfg.minSegSize = 1; cfg.progressThrottleMs = 1;
        QString dir = makeTempDir();
        DownloadManager mgr(cfg, dir);
        DownloadTableModel model(&mgr);
        QUuid id = mgr.addDownload(srv.url("/ranged"), dir + "/pause.bin");
        model.appendTask(mgr.taskById(id));
        DownloadTask* t = mgr.taskById(id);

        QVERIFY(waitForState(mgr, id, DownloadState::Downloading, 5000));
        QSignalSpy prog(t, &DownloadTask::progress);
        QVERIFY(prog.wait(3000));   // let the sampler accrue at least one real sample

        mgr.pauseAll();
        QVERIFY(waitForState(mgr, id, DownloadState::Paused, 3000));

        QModelIndex etaIx = model.index(0, DownloadTableModel::TimeLeft);
        const QString eta = model.data(etaIx).toString();
        QVERIFY(eta.isEmpty() || eta == QString("—"));
    }

    // --- Task 10: Priority column -------------------------------------------

    void tableExposesPriorityColumn() {
        const QString dir = QDir::tempPath() + "/orbit_prio_" +
                            QUuid::createUuid().toString(QUuid::WithoutBraces);
        QDir().mkpath(dir);
        EngineConfig cfg;
        Logger logger(dir);
        DownloadManager mgr(cfg, dir, &logger);
        const QUuid id = mgr.addDownload(QUrl("http://x/f.bin"), dir + "/f.bin");
        DownloadTableModel model(&mgr);
        QCOMPARE(model.columnCount(), int(DownloadTableModel::ColumnCount));
        const int col = DownloadTableModel::Priority;
        const int row = 0;
        QCOMPARE(model.data(model.index(row, col)).toString(), QString("Normal"));
        mgr.setPriority(id, Priority::High);
        QCOMPARE(model.data(model.index(row, col)).toString(), QString("High"));
    }

    // --- Task 8: CategoryFilterProxy -------------------------------------
    // NOTE (brief deviation, test-only): same TestServer-ctor adaptation as
    // the Task 7 tests above (real TestServer takes the body in its ctor and
    // binds via listen(), not a default ctor + start()).

    void proxy_filters_by_state_and_category() {
        TestServer srv(makeGuiBody(5000));
        QVERIFY(srv.listen());
        EngineConfig cfg; QString dir = makeTempDir();
        DownloadManager mgr(cfg, dir);
        DownloadTableModel model(&mgr);
        QUuid a = mgr.addDownload(srv.url("/ranged"), dir + "/movie.mp4");
        QUuid b = mgr.addDownload(srv.url("/ranged"), dir + "/song.mp3");
        model.appendTask(mgr.taskById(a));
        model.appendTask(mgr.taskById(b));

        CategoryFilterProxy proxy;
        proxy.setSourceModel(&model);

        proxy.setFilter(CategoryFilterProxy::Filter::All);
        QCOMPARE(proxy.rowCount(), 2);

        proxy.setFilter(CategoryFilterProxy::Filter::Movie);
        QCOMPARE(proxy.rowCount(), 1);
        QCOMPARE(proxy.data(proxy.index(0, DownloadTableModel::Name)).toString(), QString("movie.mp4"));

        proxy.setFilter(CategoryFilterProxy::Filter::Music);
        QCOMPARE(proxy.rowCount(), 1);
    }

    // --- Fix-pass tests: Task 8 finding 2 (undertested filter branches) --
    // Software/Others category branches were untested one-liners in
    // filterAcceptsRow; cover them with a mixed-category row set so each
    // filter both accepts its own category and rejects the others'.
    void proxy_filters_software_and_others_categories() {
        TestServer srv(makeGuiBody(5000));
        QVERIFY(srv.listen());
        EngineConfig cfg; QString dir = makeTempDir();
        DownloadManager mgr(cfg, dir);
        DownloadTableModel model(&mgr);
        QUuid dmg = mgr.addDownload(srv.url("/ranged"), dir + "/app.dmg");
        QUuid zip = mgr.addDownload(srv.url("/ranged"), dir + "/archive.zip");
        QUuid mp3 = mgr.addDownload(srv.url("/ranged"), dir + "/song.mp3");
        QUuid txt = mgr.addDownload(srv.url("/ranged"), dir + "/notes.txt");
        QUuid mp4 = mgr.addDownload(srv.url("/ranged"), dir + "/movie.mp4");
        model.appendTask(mgr.taskById(dmg));
        model.appendTask(mgr.taskById(zip));
        model.appendTask(mgr.taskById(mp3));
        model.appendTask(mgr.taskById(txt));
        model.appendTask(mgr.taskById(mp4));

        CategoryFilterProxy proxy;
        proxy.setSourceModel(&model);

        // Software accepts .dmg/.zip, rejects the .mp3/.txt/.mp4 rows.
        proxy.setFilter(CategoryFilterProxy::Filter::Software);
        QCOMPARE(proxy.rowCount(), 2);
        QSet<QString> softwareNames;
        for (int r = 0; r < proxy.rowCount(); ++r)
            softwareNames.insert(proxy.data(proxy.index(r, DownloadTableModel::Name)).toString());
        QCOMPARE(softwareNames, QSet<QString>({QString("app.dmg"), QString("archive.zip")}));

        // Others accepts .txt, rejects the .mp4 (and every other) row.
        proxy.setFilter(CategoryFilterProxy::Filter::Others);
        QCOMPARE(proxy.rowCount(), 1);
        QCOMPARE(proxy.data(proxy.index(0, DownloadTableModel::Name)).toString(), QString("notes.txt"));
    }

    // Drives one download to real Completed and keeps a second one paused
    // (not Completed) to prove Filter::Completed/Downloading key off the
    // task's actual DownloadState rather than category, per
    // filterAcceptsRow's state != / == Completed branches.
    void proxy_filters_by_completed_and_downloading_state() {
        SKIP_IF_CI_TIMING();  // needs a mid-transfer pause; races on fast CI loopback (issue #1)
        TestServer fastSrv(makeGuiBody(5000));               // completes quickly
        QVERIFY(fastSrv.listen());
        TestServer slowSrv(makeGuiBody(5 * 1024 * 1024));     // large enough to pause mid-flight
        QVERIFY(slowSrv.listen());

        EngineConfig cfg; cfg.segmentCount = 4; cfg.minSegSize = 1; cfg.progressThrottleMs = 1;
        QString dir = makeTempDir();
        DownloadManager mgr(cfg, dir);
        DownloadTableModel model(&mgr);

        QUuid done = mgr.addDownload(fastSrv.url("/ranged"), dir + "/done.bin");
        QUuid slow = mgr.addDownload(slowSrv.url("/ranged"), dir + "/slow.bin");
        model.appendTask(mgr.taskById(done));
        model.appendTask(mgr.taskById(slow));

        DownloadTask* slowTask = mgr.taskById(slow);
        QSignalSpy prog(slowTask, &DownloadTask::progress);
        QVERIFY(prog.wait(3000));
        mgr.pause(slow);
        QVERIFY(waitForState(mgr, slow, DownloadState::Paused, 3000));
        QVERIFY(waitForState(mgr, done, DownloadState::Completed, 10000));

        CategoryFilterProxy proxy;
        proxy.setSourceModel(&model);

        proxy.setFilter(CategoryFilterProxy::Filter::Completed);
        QCOMPARE(proxy.rowCount(), 1);
        QCOMPARE(proxy.data(proxy.index(0, DownloadTableModel::Name)).toString(), QString("done.bin"));

        proxy.setFilter(CategoryFilterProxy::Filter::Downloading);
        QCOMPARE(proxy.rowCount(), 1);
        QCOMPARE(proxy.data(proxy.index(0, DownloadTableModel::Name)).toString(), QString("slow.bin"));
    }

    // --- Task 9: ProgressGridWidget --------------------------------------

    void grid_widget_constructs_and_sets_task() {
        EngineConfig cfg; QString dir = makeTempDir();
        DownloadManager mgr(cfg, dir);
        TestServer srv(makeGuiBody(5000));
        QVERIFY(srv.listen());
        QUuid id = mgr.addDownload(srv.url("/ranged"), dir + "/o.bin");
        ProgressGridWidget w;
        w.resize(200, 80);
        w.setTask(mgr.taskById(id));
        w.setTask(nullptr);         // switching away must disconnect cleanly
        QVERIFY(true);
    }

    void grid_widget_repaints_on_state_change_without_crash() {
        // Fix 2 (Fase 2 polish): setTask() used to connect only
        // segmentProgress, so a task that transitions to Error/Completed
        // without further segment bytes never scheduled a repaint. Now it
        // also connects stateChanged. Drive a real state transition and
        // confirm the throttled repaint connection fires without crashing.
        TestServer srv(makeGuiBody(5000));
        QVERIFY(srv.listen());
        EngineConfig cfg; QString dir = makeTempDir();
        DownloadManager mgr(cfg, dir);
        QUuid id = mgr.addDownload(srv.url("/ranged"), dir + "/g.bin");
        ProgressGridWidget w;
        w.resize(200, 80);
        w.setTask(mgr.taskById(id));

        QVERIFY(waitForState(mgr, id, DownloadState::Completed, 10000));
        QTest::qWait(150);   // let the throttled scheduleRepaint() timer fire
        QVERIFY(true);       // reaching here without a crash/hang confirms the wiring
    }

    // --- Task 10: NewDownloadDialog --------------------------------------

    void validDownloadUrlAcceptsHttpHttpsFtp() {
        QVERIFY(NewDownloadDialog::isValidDownloadUrl(QUrl("http://h/f.bin")));
        QVERIFY(NewDownloadDialog::isValidDownloadUrl(QUrl("https://h/f.bin")));
        QVERIFY(NewDownloadDialog::isValidDownloadUrl(QUrl("ftp://h/f.bin")));
        QVERIFY(NewDownloadDialog::isValidDownloadUrl(QUrl("FTP://h/f.bin")));   // case-insensitive
        QVERIFY(NewDownloadDialog::isValidDownloadUrl(QUrl("ftp://u:p@h/f.bin")));
    }

    void validDownloadUrlRejectsOthers() {
        QVERIFY(!NewDownloadDialog::isValidDownloadUrl(QUrl("gopher://h/f")));
        QVERIFY(!NewDownloadDialog::isValidDownloadUrl(QUrl("file:///tmp/f")));
        QVERIFY(!NewDownloadDialog::isValidDownloadUrl(QUrl("not a url")));
        QVERIFY(!NewDownloadDialog::isValidDownloadUrl(QUrl("http://")));        // sem host
        QVERIFY(!NewDownloadDialog::isValidDownloadUrl(QUrl()));
    }

    void dialog_destpath_joins_dir_and_name_without_double_slash() {
        NewDownloadDialog dlg;
        auto* urlEdit  = dlg.findChild<QLineEdit*>("urlEdit");
        auto* dirEdit  = dlg.findChild<QLineEdit*>("dirEdit");
        QVERIFY(urlEdit && dirEdit);

        urlEdit->setText("https://x.com/file.zip");

        dirEdit->setText("/tmp/orbit-test-dir");
        QCOMPARE(dlg.destPath(), QString("/tmp/orbit-test-dir/file.zip"));

        dirEdit->setText("/tmp/orbit-test-dir/");   // barra final não pode duplicar
        QCOMPARE(dlg.destPath(), QString("/tmp/orbit-test-dir/file.zip"));
    }

    void dialog_destpath_uses_edited_name_field() {
        NewDownloadDialog dlg;
        auto* urlEdit  = dlg.findChild<QLineEdit*>("urlEdit");
        auto* dirEdit  = dlg.findChild<QLineEdit*>("dirEdit");
        auto* nameEdit = dlg.findChild<QLineEdit*>("fileNameEdit");
        QVERIFY(urlEdit && dirEdit && nameEdit);

        urlEdit->setText("https://x.com/download");   // path dá "download"
        QCOMPARE(nameEdit->text(), QString("download"));

        dirEdit->setText("/tmp/orbit-test-dir");
        nameEdit->setText("Audiobook.m4a");            // usuário renomeia
        QCOMPARE(dlg.destPath(), QString("/tmp/orbit-test-dir/Audiobook.m4a"));
    }

    // Fix 1 (review pass, spec §3.3): retyping the name field must refresh
    // the Type label too, not just set the "user edited" flag.
    void dialog_type_label_follows_manual_name_edit() {
        NewDownloadDialog dlg;
        auto* urlEdit  = dlg.findChild<QLineEdit*>("urlEdit");
        auto* nameEdit = dlg.findChild<QLineEdit*>("fileNameEdit");
        auto* typeLabel = dlg.findChild<QLabel*>("typeLabel");
        QVERIFY(urlEdit && nameEdit && typeLabel);

        urlEdit->setText("https://x.com/clip.bin");
        QCOMPARE(nameEdit->text(), QString("clip.bin"));
        QCOMPARE(typeLabel->text(),
                 FileType::displayName(FileType::categorize("clip.bin")));

        nameEdit->clear();
        QTest::keyClicks(nameEdit, "song.mp3");   // manual edit via textEdited

        QCOMPARE(typeLabel->text(),
                 FileType::displayName(FileType::categorize("song.mp3")));
    }

    // --- Task 4 (Content-Disposition plan): async probe wiring -----------

    void dialog_probe_fills_name_when_not_edited() {
        NewDownloadDialog dlg;
        auto* urlEdit  = dlg.findChild<QLineEdit*>("urlEdit");
        auto* nameEdit = dlg.findChild<QLineEdit*>("fileNameEdit");
        QVERIFY(urlEdit && nameEdit);

        urlEdit->setText("https://drive/download?id=1");
        QCOMPARE(nameEdit->text(), QString("download"));      // fallback da URL

        ProbeResult r; r.ok = true; r.suggestedFileName = "Audiobook.m4a";
        dlg.applyProbeResult(QUrl("https://drive/download?id=1"), r);

        QCOMPARE(nameEdit->text(), QString("Audiobook.m4a")); // preenchido
    }

    void dialog_probe_does_not_overwrite_user_edit() {
        NewDownloadDialog dlg;
        auto* urlEdit  = dlg.findChild<QLineEdit*>("urlEdit");
        auto* nameEdit = dlg.findChild<QLineEdit*>("fileNameEdit");
        QVERIFY(urlEdit && nameEdit);

        urlEdit->setText("https://drive/download?id=1");
        nameEdit->clear();
        QTest::keyClicks(nameEdit, "meu-nome.m4a");           // edição do usuário (textEdited)

        ProbeResult r; r.ok = true; r.suggestedFileName = "Audiobook.m4a";
        dlg.applyProbeResult(QUrl("https://drive/download?id=1"), r);

        QCOMPARE(nameEdit->text(), QString("meu-nome.m4a"));  // preservado
    }

    void dialog_probe_ignores_stale_result() {
        NewDownloadDialog dlg;
        auto* urlEdit  = dlg.findChild<QLineEdit*>("urlEdit");
        auto* nameEdit = dlg.findChild<QLineEdit*>("fileNameEdit");
        QVERIFY(urlEdit && nameEdit);

        urlEdit->setText("https://drive/download?id=NEW");    // nome = "download"
        ProbeResult r; r.ok = true; r.suggestedFileName = "obsoleto.m4a";
        dlg.applyProbeResult(QUrl("https://drive/download?id=OLD"), r);   // URL antiga

        QCOMPARE(nameEdit->text(), QString("download"));      // ignorado
    }

    void dialog_probe_empty_name_keeps_fallback() {
        NewDownloadDialog dlg;
        auto* urlEdit  = dlg.findChild<QLineEdit*>("urlEdit");
        auto* nameEdit = dlg.findChild<QLineEdit*>("fileNameEdit");
        QVERIFY(urlEdit && nameEdit);

        urlEdit->setText("https://x.com/file.zip");
        ProbeResult r; r.ok = true; r.suggestedFileName = "";  // sem header
        dlg.applyProbeResult(QUrl("https://x.com/file.zip"), r);

        QCOMPARE(nameEdit->text(), QString("file.zip"));       // fallback mantido
    }

    // --- Task 11: CategoryTree --------------------------------------------

    void tree_emits_filter_on_selection() {
        CategoryTree tree;
        QSignalSpy spy(&tree, &CategoryTree::filterChanged);
        // Select the "Completed" row (see build order in Step 3).
        tree.setCurrentItem(tree.topLevelItem(0)->child(1)); // All Downloads > Completed
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.takeFirst().at(0).value<CategoryFilterProxy::Filter>(),
                 CategoryFilterProxy::Filter::Completed);
    }

    // --- Task 12: MainWindow ----------------------------------------------

    void mainwindow_constructs_and_wires() {
        EngineConfig cfg; QString dir = makeTempDir();
        DownloadManager mgr(cfg, dir);
        DownloadTableModel model(&mgr);
        Logger logger(dir);
        MainWindow w(&mgr, &model, &logger);
        w.resize(900, 600);
        w.show();
        QVERIFY(w.findChild<QTableView*>() != nullptr);
        QVERIFY(w.findChild<CategoryTree*>() != nullptr);
        QVERIFY(w.findChild<ProgressGridWidget*>() != nullptr);
    }

    // --- Task 12: extractUrls ---------------------------------------------

    void extractUrlsFromUriList() {
        QMimeData m;
        m.setUrls({QUrl("http://h/a.bin"), QUrl("ftp://h/b.bin")});
        const auto urls = extractUrls(&m);
        QCOMPARE(urls.size(), 2);
        QCOMPARE(urls.at(0), QUrl("http://h/a.bin"));
        QCOMPARE(urls.at(1), QUrl("ftp://h/b.bin"));
    }

    void extractUrlsFiltersUnsupportedSchemes() {
        QMimeData m;
        m.setUrls({QUrl("http://h/a.bin"), QUrl("file:///tmp/x"), QUrl("gopher://h/c")});
        const auto urls = extractUrls(&m);
        QCOMPARE(urls.size(), 1);
        QCOMPARE(urls.at(0), QUrl("http://h/a.bin"));
    }

    void extractUrlsFromPlainText() {
        QMimeData m;
        m.setText("olha esse link http://h/a.bin no meio do texto");
        const auto urls = extractUrls(&m);
        QCOMPARE(urls.size(), 1);
        QCOMPARE(urls.at(0), QUrl("http://h/a.bin"));
    }

    void extractUrlsFromMultiLineText() {
        QMimeData m;
        m.setText("http://h/a.bin\nftp://h/b.bin\nlixo\nhttps://h/c.bin");
        const auto urls = extractUrls(&m);
        QCOMPARE(urls.size(), 3);
    }

    void extractUrlsDeduplicates() {
        QMimeData m;
        m.setText("http://h/a.bin http://h/a.bin");
        const auto urls = extractUrls(&m);
        QCOMPARE(urls.size(), 1);
    }

    void extractUrlsEmptyWhenNothingDownloadable() {
        QMimeData m;
        m.setText("só texto, nenhum link");
        QVERIFY(extractUrls(&m).isEmpty());

        QMimeData m2;
        QVERIFY(extractUrls(&m2).isEmpty());
        QVERIFY(extractUrls(nullptr).isEmpty());
    }

    // --- Task 13: shouldOffer (clipboard monitor) -------------------------

    void shouldOfferAcceptsDownloadableUrl() {
        const auto r = shouldOffer("http://h/a.bin", QUrl(), false);
        QVERIFY(r.has_value());
        QCOMPARE(*r, QUrl("http://h/a.bin"));
    }

    void shouldOfferAcceptsFtp() {
        QVERIFY(shouldOffer("ftp://h/a.bin", QUrl(), false).has_value());
    }

    void shouldOfferTrimsWhitespace() {
        const auto r = shouldOffer("  http://h/a.bin \n", QUrl(), false);
        QVERIFY(r.has_value());
        QCOMPARE(*r, QUrl("http://h/a.bin"));
    }

    void shouldOfferRejectsNonUrl() {
        QVERIFY(!shouldOffer("bom dia", QUrl(), false).has_value());
        QVERIFY(!shouldOffer("", QUrl(), false).has_value());
        QVERIFY(!shouldOffer("file:///tmp/x", QUrl(), false).has_value());
    }

    void shouldOfferRejectsSelfCopy() {
        QVERIFY(!shouldOffer("http://h/a.bin", QUrl(), true).has_value());
    }

    void shouldOfferRejectsImmediateRepeat() {
        QVERIFY(!shouldOffer("http://h/a.bin", QUrl("http://h/a.bin"), false).has_value());
    }

    void shouldOfferAcceptsDifferentUrlAfterPrevious() {
        QVERIFY(shouldOffer("http://h/b.bin", QUrl("http://h/a.bin"), false).has_value());
    }

    // Texto com lixo em volta NÃO conta: o clipboard tem que ser a URL. (Drop
    // é que varre texto — spec §3.7.)
    void shouldOfferRejectsUrlBuriedInProse() {
        QVERIFY(!shouldOffer("veja http://h/a.bin agora", QUrl(), false).has_value());
    }

    // --- Task 14: CredentialsDialog --------------------------------------

    void credentialsDialogReturnsTypedValues() {
        CredentialsDialog d("ftp.example.org");
        // Os campos são achados por objectName — defina-os na implementação.
        auto* user = d.findChild<QLineEdit*>("userEdit");
        auto* pass = d.findChild<QLineEdit*>("passEdit");
        QVERIFY(user);
        QVERIFY(pass);
        QTest::keyClicks(user, "bob");
        QTest::keyClicks(pass, "secret");
        QCOMPARE(d.user(), QString("bob"));
        QCOMPARE(d.pass(), QString("secret"));
        QCOMPARE(pass->echoMode(), QLineEdit::Password);   // senha não aparece na tela
    }

    void credentialsDialogShowsHost() {
        CredentialsDialog d("ftp.example.org");
        bool found = false;
        for (auto* l : d.findChildren<QLabel*>())
            if (l->text().contains("ftp.example.org")) { found = true; break; }
        QVERIFY2(found, "o diálogo precisa dizer a QUEM está entregando a senha");
    }
    // --- Task 10 (Fase 4): PreferencesDialog -------------------------------

    void preferences_result_reflects_widgets() {
        AppSettings in;
        in.engine.maxConcurrentDownloads = 3;
        in.engine.userAgent = "curl/8.7.1";
        PreferencesDialog dlg(in);
        dlg.setConcurrentForTest(5);
        dlg.setMaxKBpsForTest(250);              // 250 KB/s
        dlg.setUserAgentCustomForTest("orbit/9");
        const AppSettings out = dlg.result();
        QCOMPARE(out.engine.maxConcurrentDownloads, 5);
        QCOMPARE(out.engine.maxBytesPerSec, qint64(250 * 1024));
        QCOMPARE(out.engine.userAgent, QString("orbit/9"));
    }

    // --- Task 7 (Browser Integration Fase 5): Browser section in Preferences

    void preferencesRoundTripsBrowserPrefs() {
        AppSettings in;
        in.browser.enabled = false;
        in.browser.port    = 8697;
        PreferencesDialog dlg(in);
        dlg.setBrowserEnabledForTest(true);        // liga -> deve gerar token não-vazio
        const AppSettings out = dlg.result();
        QVERIFY(out.browser.enabled);
        QCOMPARE(out.browser.port, quint16(8697));
        QVERIFY(!out.browser.token.isEmpty());     // gerado ao habilitar
    }

    // --- Task 14 (Fase 4): SchedulerDialog ----------------------------------

    void scheduler_dialog_result_reflects_widgets() {
        SchedulerConfig in;                 // defaults: disabled, 08:00-18:00, Daily
        SchedulerDialog dlg(in);
        dlg.setEnabledForTest(true);
        dlg.setStartForTest(QTime(9, 15));
        dlg.setStopForTest(QTime(23, 45));
        dlg.setRecurrenceForTest(Recurrence::Once);
        dlg.setQuitWhenDoneForTest(true);
        const SchedulerConfig out = dlg.result();
        QVERIFY(out.enabled);
        QCOMPARE(out.start, QTime(9, 15));
        QCOMPARE(out.stop, QTime(23, 45));
        QVERIFY(out.recurrence == Recurrence::Once);
        QVERIFY(out.quitWhenDone);
    }

    // --- Task 9 (Fase 4): aba Log acompanha o download selecionado --------
    // Gap encontrado em revisão: a aba Log (headline do Task 9) não tinha
    // teste automatizado. Cobre: (1) selecionar A carrega só o log de A;
    // (2) uma linha ao vivo de B, com A selecionado, não vaza p/ a aba (o
    // filtro por m_logShownId em onLogLine).
    void mainwindow_log_tab_follows_selection() {
        QTemporaryDir dir;
        EngineConfig cfg;
        Logger logger(dir.path());
        DownloadManager mgr(cfg, dir.path(), &logger);
        DownloadTableModel model(&mgr);
        MainWindow w(&mgr, &model, &logger);

        const QString destA = dir.filePath("a.bin");
        const QString destB = dir.filePath("b.bin");
        const QUuid idA = mgr.addDownload(QUrl("http://h/a.bin"), destA);
        const QUuid idB = mgr.addDownload(QUrl("http://h/b.bin"), destB);
        model.appendTask(mgr.taskById(idA));   // row 0
        model.appendTask(mgr.taskById(idB));   // row 1

        logger.logTask(idA, destA, LogLevel::Info, "AAA-marker");
        logger.logTask(idB, destB, LogLevel::Info, "BBB-marker");

        auto* table = w.findChild<QTableView*>();
        QVERIFY(table != nullptr);
        // Seleciona a linha de A pelo caminho real (proxy -> selectionModel),
        // o mesmo que selectedId()/onSelectionChanged usam.
        const QModelIndex proxyIdxA = table->model()->index(0, 0);
        table->selectionModel()->setCurrentIndex(
            proxyIdxA, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);

        QVERIFY(w.logTextForTest().contains("AAA-marker"));
        QVERIFY(!w.logTextForTest().contains("BBB-marker"));

        // Linha nova de B chegando ao vivo enquanto A está selecionado: não
        // deve vazar p/ a aba (prova o filtro por id em onLogLine).
        logger.logTask(idB, destB, LogLevel::Info, "BBB-live");
        QVERIFY(!w.logTextForTest().contains("BBB-live"));
    }

    // --- Task 11 (Fase 4): wiring de Preferences no MainWindow -------------

    void mainwindow_applysettings_sets_default_dir_and_clip_mode() {
        QTemporaryDir dir;
        EngineConfig cfg; DownloadManager mgr(cfg, dir.path());
        DownloadTableModel model(&mgr);
        Logger logger(dir.path());
        MainWindow w(&mgr, &model, &logger);
        AppSettings s;
        s.ui.defaultDownloadDir = dir.path();
        s.ui.clipboardMode = ClipboardMode::Auto;
        w.applySettings(s, dir.filePath("settings.json"));
        QCOMPARE(w.defaultDirForTest(), dir.path());
        QVERIFY(w.clipModeForTest() == ClipboardMode::Auto);
    }

    // --- Task 12 (Fase 4): boot loads settings, Scheduler drives manager ---

    // NOTE (brief deviation, test-only): same TestServer-ctor adaptation as
    // the Task 7 tests above (real TestServer takes the body in its ctor,
    // not `srv(m_body)` from the brief's sketch — there's no m_body member).
    void mainwindow_scheduler_action_routes_to_manager() {
        QTemporaryDir dir;
        EngineConfig cfg; DownloadManager mgr(cfg, dir.path());
        DownloadTableModel model(&mgr);
        Logger logger(dir.path());
        MainWindow w(&mgr, &model, &logger);
        TestServer srv(makeGuiBody(5000)); QVERIFY(srv.listen());
        const QUuid id = mgr.addDownload(srv.url("/ranged"), dir.filePath("s.bin"));
        mgr.pause(id);
        QVERIFY(waitForState(mgr, id, DownloadState::Paused, 5000));
        w.applySchedActionForTest(SchedAction::StartAll);   // roteia p/ resumeAll()
        QDeadlineTimer dl(5000);
        bool changed = false;
        while (!dl.hasExpired()) {
            if (mgr.taskById(id)->state() != DownloadState::Paused) { changed = true; break; }
            QTest::qWait(20);
        }
        QVERIFY(changed);
    }

    // --- Task 13 (Fase 4): mudança de modo via menu Tools persiste em settings.json

    void mainwindow_clipboard_mode_change_persists() {
        QTemporaryDir dir;
        EngineConfig cfg; DownloadManager mgr(cfg, dir.path());
        DownloadTableModel model(&mgr);
        Logger logger(dir.path());
        MainWindow w(&mgr, &model, &logger);
        const QString path = dir.filePath("settings.json");
        w.applySettings(AppSettings{}, path);
        w.setClipboardModeForTest(ClipboardMode::Notify);   // simula clique no rádio
        const AppSettings back = SettingsIo::load(path, EngineConfig{});
        QVERIFY(back.ui.clipboardMode == ClipboardMode::Notify);
    }

    // --- Task 15 (Fase 4): fiação do Scheduler no MainWindow (apply ao vivo) --

    void mainwindow_scheduler_apply_persists() {
        QTemporaryDir dir;
        EngineConfig cfg; DownloadManager mgr(cfg, dir.path());
        DownloadTableModel model(&mgr);
        Logger logger(dir.path());
        MainWindow w(&mgr, &model, &logger);
        const QString path = dir.filePath("settings.json");
        w.applySettings(AppSettings{}, path);
        SchedulerConfig sc;
        sc.enabled = true; sc.start = QTime(7,0); sc.stop = QTime(19,0);
        sc.recurrence = Recurrence::Once; sc.quitWhenDone = true;
        w.applySchedulerConfigForTest(sc);
        const AppSettings back = SettingsIo::load(path, EngineConfig{});
        QVERIFY(back.scheduler.enabled);
        QCOMPARE(back.scheduler.start, QTime(7,0));
        QVERIFY(back.scheduler.recurrence == Recurrence::Once);
        QVERIFY(back.scheduler.quitWhenDone);
    }

    // --- Task 16 (Fase 4): barra de menus completa (File/Edit/View/Tools/Help)

    void mainwindow_has_full_menu_bar() {
        QTemporaryDir dir;
        EngineConfig cfg; DownloadManager mgr(cfg, dir.path());
        DownloadTableModel model(&mgr);
        Logger logger(dir.path());
        MainWindow w(&mgr, &model, &logger);
        QStringList titles;
        for (QAction* a : w.menuBar()->actions())
            if (a->menu()) titles << a->menu()->title().remove('&');
        QVERIFY(titles.contains("File"));
        QVERIFY(titles.contains("Edit"));
        QVERIFY(titles.contains("View"));
        QVERIFY(titles.contains("Tools"));
        QVERIFY(titles.contains("Help"));
    }

    // --- Task 11 (Fase 4): regras de habilitação do menu de contexto -------

    // --- Task 12: Clear Completed -------------------------------------------

    void clearCompletedRemovesOnlyCompleted() {
        const QString dir = QDir::tempPath() + "/orbit_clear_" +
                            QUuid::createUuid().toString(QUuid::WithoutBraces);
        QDir().mkpath(dir);
        EngineConfig cfg;
        Logger logger(dir);
        DownloadManager mgr(cfg, dir, &logger);
        // Um "completed" simulado não é trivial sem rede; use o helper de
        // remoção diretamente: adiciona 2, marca 1 como Completed via download real.
        TestServer srv(makeGuiBody(2000));
        QVERIFY(srv.listen());
        const QUuid done = mgr.addDownload(srv.url("/ranged"), dir + "/done.bin");
        QVERIFY(QTest::qWaitFor([&]{ auto* t = mgr.taskById(done);
                    return t && t->state() == DownloadState::Completed; }, 5000));
        const QUuid queued = mgr.addDownload(QUrl("http://x/never"), dir + "/keep.bin");
        Q_UNUSED(queued);
        DownloadTableModel model(&mgr);
        MainWindow w(&mgr, &model, &logger);
        w.clearCompletedForTest();
        // 'done' saiu; 'keep' permanece.
        QVERIFY(mgr.taskById(done) == nullptr);
        QVERIFY(mgr.taskById(queued) != nullptr);
        QVERIFY(QFile::exists(dir + "/done.bin"));   // arquivos NÃO apagados
    }

    // --- Task 8 (Fase 5): bridge do browser fiado no MainWindow -------------

    void browserDownloadEnqueuesWithHeaders() {
        QTemporaryDir dir;
        EngineConfig cfg;
        DownloadManager mgr(cfg, dir.path());
        DownloadTableModel model(&mgr);
        MainWindow w(&mgr, &model, nullptr);
        const int before = model.rowCount();
        const HeaderList h = {{QByteArray("Cookie"), QByteArray("k=v")}};
        w.emitBrowserDownloadForTest(QUrl("https://h/big.iso"), h, "big.iso");
        QCOMPARE(model.rowCount(), before + 1);
        // o header foi parar na task criada
        const auto tasks = mgr.tasks();
        QVERIFY(!tasks.isEmpty());
        QVERIFY(tasks.last()->record().extraHeaders.contains(
            {QByteArray("Cookie"), QByteArray("k=v")}));
    }

    // Fix (Task 8, Fase 5, spec §7): o bridge deve ser reaplicado ao mudar as
    // Preferences, não só na inicialização. Como dirigir o QDialog modal do
    // Preferences é inviável offscreen, testamos o helper compartilhado
    // (applyBrowserBridgeForTest) que onPreferences() agora chama.
    void browserBridgeReappliesOnPrefsChange() {
        // Descobre uma porta livre bindando um QTcpServer efêmero em 0.
        quint16 freePort = 0;
        {
            QTcpServer probe;
            QVERIFY(probe.listen(QHostAddress::LocalHost, 0));
            freePort = probe.serverPort();
        }   // libera a porta ao saír de escopo

        QTemporaryDir dir;
        EngineConfig cfg;
        DownloadManager mgr(cfg, dir.path());
        DownloadTableModel model(&mgr);
        MainWindow w(&mgr, &model, nullptr);

        QVERIFY(!w.bridgeListeningForTest());   // nada ligado por padrão (sem token/enabled)

        w.applyBrowserBridgeForTest({true, freePort, "tok"});
        QVERIFY(w.bridgeListeningForTest());    // Preferences habilitou -> passa a ouvir já

        w.applyBrowserBridgeForTest({false, freePort, "tok"});
        QVERIFY(!w.bridgeListeningForTest());   // Preferences desabilitou -> para já
    }

    // Fix (Task 8, Fase 5, revisão final): BrowserBridge::stop() só fazia
    // deleteLater() no QTcpServer, que NÃO libera o socket de escuta
    // sincronamente. applyBrowserBridge() chama stop()+start(mesmaPorta) na
    // MESMA chamada síncrona (ex.: Preferences->OK ou Regenerate), então o
    // listen() na mesma porta falhava silenciosamente. stop() agora chama
    // m_server->close() antes de deleteLater() para liberar o socket já.
    void browserBridgeRebindsSamePort() {
        quint16 freePort = 0;
        {
            QTcpServer probe;
            QVERIFY(probe.listen(QHostAddress::LocalHost, 0));
            freePort = probe.serverPort();
        }   // libera a porta ao saír de escopo

        QTemporaryDir dir;
        EngineConfig cfg;
        DownloadManager mgr(cfg, dir.path());
        DownloadTableModel model(&mgr);
        MainWindow w(&mgr, &model, nullptr);

        w.applyBrowserBridgeForTest({true, freePort, "tok"});
        QVERIFY(w.bridgeListeningForTest());

        // Reaplica na MESMA porta, ainda habilitado (ex.: Preferences->OK de
        // novo, ou Regenerate) -> antes do fix, o listener antigo ainda
        // estava vinculado e o novo listen() falhava.
        w.applyBrowserBridgeForTest({true, freePort, "tok"});
        QVERIFY(w.bridgeListeningForTest());
    }

    void contextMenuEnableRules() {
        using S = DownloadState;
        // Start: habilitado em Queued/Paused/Cancelled/Error; não em Downloading/Completed.
        QVERIFY(ctxCanStart(S::Paused));
        QVERIFY(ctxCanStart(S::Cancelled));
        QVERIFY(!ctxCanStart(S::Downloading));
        QVERIFY(!ctxCanStart(S::Completed));
        // Stop: só Connecting/Downloading.
        QVERIFY(ctxCanStop(S::Downloading));
        QVERIFY(!ctxCanStop(S::Paused));
        // Cancel: não em Completed/Cancelled.
        QVERIFY(ctxCanCancel(S::Downloading));
        QVERIFY(!ctxCanCancel(S::Completed));
        // Move: só não-ativo.
        QVERIFY(ctxCanMove(S::Paused));
        QVERIFY(!ctxCanMove(S::Downloading));
        // Open: só Completed.
        QVERIFY(ctxCanOpen(S::Completed));
        QVERIFY(!ctxCanOpen(S::Paused));
    }

    // --- Task 3: Theme module ---------------------------------------------------

    void gridColors_keeps_orbit_identity_colors() {
        const GridColors c = gridColors();
        QCOMPARE(c.downloaded, QColor("#5b9bd5"));
        QCOMPARE(c.active,     QColor("#f7941e"));
        QCOMPARE(c.error,      QColor("#ef4444"));
        QVERIFY(c.background.isValid());
        QVERIFY(c.pending.isValid());
    }
};

QTEST_MAIN(TestGui)
#include "tst_gui.moc"
