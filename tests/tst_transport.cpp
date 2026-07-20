#include <QtTest>
#include <QSignalSpy>
#include <QTemporaryDir>
#include "FakeTransport.h"
#include "DownloadTask.h"
#include "DownloadManager.h"

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
};

QTEST_MAIN(TstTransport)
#include "tst_transport.moc"
