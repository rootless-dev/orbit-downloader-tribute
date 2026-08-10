#include <QtTest>
#include <QSignalSpy>
#include <QTemporaryDir>
#include "FakeTransport.h"
#include "DownloadTask.h"
#include "DownloadManager.h"
#include <algorithm>

static QByteArray makeBody(int n) {
    QByteArray b; b.resize(n);
    for (int i = 0; i < n; ++i) b[i] = char('A' + (i % 26));
    return b;
}

class TstTransport : public QObject {
    Q_OBJECT
private slots:

    // Critério 1: um Transport sem rede leva a task de Queued a Completed.
    void fakeTransportDrivesTaskToCompletion() {
        const QByteArray body = makeBody(4096);
        QTemporaryDir dir;
        const QString dest = dir.path() + "/out.bin";

        FakeTransport tr;
        tr.setBody(body);
        ProbeResult r;
        r.ok = true; r.totalBytes = body.size(); r.supportsRange = true; r.etag = "\"v1\"";
        tr.setProbeResult(r);

        EngineConfig cfg;
        DownloadTask task(&tr, cfg);
        task.init(QUuid::createUuid(), QUrl("fake://host/f.bin"), dest, 4);
        QSignalSpy spy(&task, &DownloadTask::stateChanged);
        task.start();

        QVERIFY(QTest::qWaitFor([&]{ return task.state() == DownloadState::Completed; }, 3000));
        QFile f(dest);
        QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(f.readAll(), body);
    }

    // Critério 2: esquema desconhecido -> QUuid nulo, nenhuma task criada.
    void unknownSchemeIsRejected() {
        QTemporaryDir dir;
        EngineConfig cfg;
        DownloadManager mgr(cfg, dir.path());
        const QUuid id = mgr.addDownload(QUrl("gopher://host/f.bin"), dir.path() + "/f.bin");
        QVERIFY(id.isNull());
        QCOMPARE(mgr.tasks().size(), 0);
    }

    // Critério 2: http/https resolvem; ftp ainda não (chega na Task 7).
    void registryResolvesHttpSchemes() {
        QTemporaryDir dir;
        EngineConfig cfg;
        DownloadManager mgr(cfg, dir.path());
        QVERIFY(mgr.transportFor(QUrl("http://h/f"))  != nullptr);
        QVERIFY(mgr.transportFor(QUrl("https://h/f")) != nullptr);
        QVERIFY(mgr.transportFor(QUrl("HTTP://h/f"))  != nullptr);   // case-insensitive
        QVERIFY(mgr.transportFor(QUrl("gopher://h/f")) == nullptr);
    }

    // Spec §9.1: N workers emitindo restartRequired na mesma volta do event
    // loop. O primeiro dispara onRestartRequired, que hoje faz qDeleteAll() —
    // destruindo os outros (e o próprio emissor) durante a emissão.
    void simultaneousRestartsDoNotDestroySender() {
        const QByteArray body = makeBody(1 << 16);
        QTemporaryDir dir;
        const QString dest = dir.path() + "/out.bin";

        FakeTransport tr;
        tr.setBody(body);
        tr.setRestartOnce(true);
        ProbeResult r;
        r.ok = true; r.totalBytes = body.size(); r.supportsRange = true;
        r.lastModified = "20260717000000";        // validador não-vazio -> pede restart
        tr.setProbeResult(r);

        EngineConfig cfg;
        DownloadTask task(&tr, cfg);
        task.init(QUuid::createUuid(), QUrl("fake://host/f.bin"), dest, 4);
        task.start();

        // Sem a correção: crash / corrupção sob ASAN.
        // Com a correção: os 4 workers reiniciam e o download completa do zero.
        QVERIFY(QTest::qWaitFor([&]{ return task.state() == DownloadState::Completed; }, 5000));
        QFile f(dest);
        QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(f.readAll(), body);
    }

    // --- divisão dinâmica: manter N conexões até o fim -----------------

    // Sem a divisão, o segmento mais lento termina sozinho enquanto os outros
    // 3 workers já morreram (é a cauda que trava o download em ~90%). Com ela,
    // cada worker que termina rouba metade do maior resto: nascem segmentos
    // além dos 4 iniciais e o download acaba byte-idêntico.
    void completedSegmentSplitsTheLongestRemaining() {
        const QByteArray body = makeBody(64000);
        QTemporaryDir dir;
        const QString dest = dir.path() + "/out.bin";

        FakeTransport tr;
        tr.setBody(body);
        tr.setSteppedBase(4000);                  // 4000, 2000, 1333, 1000, ... bytes/tick
        ProbeResult r;
        r.ok = true; r.totalBytes = body.size(); r.supportsRange = true;
        tr.setProbeResult(r);

        EngineConfig cfg;
        cfg.minSegSize = 512;                     // divide enquanto sobrar >= 1 KiB
        DownloadTask task(&tr, cfg);
        task.init(QUuid::createUuid(), QUrl("fake://host/f.bin"), dest, 4);
        task.start();

        QVERIFY(QTest::qWaitFor([&]{ return task.state() == DownloadState::Completed; }, 10000));
        QFile f(dest);
        QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(f.readAll(), body);              // faixas disjuntas: nada sobrescrito
        QVERIFY2(task.segments().size() > 4, "nenhum segmento foi dividido");
        QCOMPARE(tr.peakLiveWorkers(), 4);        // nunca passou do teto de conexões
        QCOMPARE(tr.liveWorkers(), 0);            // e nenhum ficou vivo no fim
        // Segmentos disjuntos e cobrindo o arquivo inteiro.
        auto segs = task.segments();
        std::sort(segs.begin(), segs.end(),
                  [](const Segment& a, const Segment& b){ return a.start < b.start; });
        QCOMPARE(segs.first().start, 0LL);
        QCOMPARE(segs.last().end, qint64(body.size()) - 1);
        for (int i = 1; i < segs.size(); ++i) QCOMPARE(segs[i].start, segs[i-1].end + 1);
    }

    // O caso da retomada: o .meta traz 3 de 4 segmentos prontos. Ao voltar, a
    // única faixa que falta é dividida até reabrir as 4 conexões, em vez de
    // arrastar o resto do arquivo numa conexão só.
    void resumeWithOneSegmentLeftReopensAllConnections() {
        const QByteArray body = makeBody(64000);
        QTemporaryDir dir;
        const QString dest = dir.path() + "/out.bin";
        {   // o parcial das 3 primeiras faixas já está no disco
            QFile f(dest);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(body.left(48000));
            f.resize(body.size());
        }

        FakeTransport tr;
        tr.setBody(body);
        tr.setSteppedBase(2000);
        ProbeResult r;
        r.ok = true; r.totalBytes = body.size(); r.supportsRange = true;
        tr.setProbeResult(r);

        EngineConfig cfg;
        cfg.minSegSize = 512;
        DownloadTask task(&tr, cfg);
        DownloadRecord rec;
        rec.id = QUuid::createUuid(); rec.url = QUrl("fake://host/f.bin"); rec.destPath = dest;
        rec.totalBytes = body.size(); rec.supportsRange = true; rec.segmentCount = 4;
        QVector<Segment> segs = {{0,     0, 16000, 15999},   // completo
                                 {1, 16000, 32000, 31999},   // completo
                                 {2, 32000, 48000, 47999},   // completo
                                 {3, 48000, 48000, 63999}};  // intocado
        task.restore(rec, segs, QString(), QString(), false);
        task.start();

        QVERIFY(QTest::qWaitFor([&]{ return tr.liveWorkers() == 4; }, 3000));
        QVERIFY(QTest::qWaitFor([&]{ return task.state() == DownloadState::Completed; }, 10000));
        QFile f(dest);
        QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(f.readAll(), body);
        QCOMPARE(tr.peakLiveWorkers(), 4);
    }

    // Cauda curta: dividir 1 KiB em quatro só somaria conexões novas a um
    // download que já está acabando - planSplit recusa e o worker termina só.
    void tinyRemainderIsNotSplit() {
        const QByteArray body = makeBody(64000);
        QTemporaryDir dir;
        const QString dest = dir.path() + "/out.bin";
        {
            QFile f(dest);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(body.left(63000));
            f.resize(body.size());
        }

        FakeTransport tr;
        tr.setBody(body);
        tr.setSteppedBase(100);
        ProbeResult r;
        r.ok = true; r.totalBytes = body.size(); r.supportsRange = true;
        tr.setProbeResult(r);

        EngineConfig cfg;
        cfg.minSegSize = 1 << 20;                 // 1 MiB: a cauda de 1000 B é minúscula
        DownloadTask task(&tr, cfg);
        DownloadRecord rec;
        rec.id = QUuid::createUuid(); rec.url = QUrl("fake://host/f.bin"); rec.destPath = dest;
        rec.totalBytes = body.size(); rec.supportsRange = true; rec.segmentCount = 4;
        QVector<Segment> segs = {{0, 0, 63000, 62999}, {1, 63000, 63000, 63999}};
        task.restore(rec, segs, QString(), QString(), false);
        task.start();

        QVERIFY(QTest::qWaitFor([&]{ return task.state() == DownloadState::Completed; }, 10000));
        QCOMPARE(task.segments().size(), 2);      // nada dividido
        QFile f(dest);
        QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(f.readAll(), body);
    }
};

QTEST_MAIN(TstTransport)
#include "tst_transport.moc"
