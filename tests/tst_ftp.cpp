#include <QtTest>
#include <QSignalSpy>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QFile>
#include "FtpReply.h"
#include "FtpControlChannel.h"
#include "FtpProbe.h"
#include "FtpSegmentWorker.h"
#include "DownloadManager.h"
#include "TestFtpServer.h"

static QByteArray makeBody(int n) {
    QByteArray b; b.resize(n);
    for (int i = 0; i < n; ++i) b[i] = char('A' + (i % 26));
    return b;
}

class TstFtp : public QObject {
    Q_OBJECT
private slots:

    // --- parseReply -----------------------------------------------------
    void parseSingleLineReply() {
        int consumed = 0;
        const FtpReply r = parseReply("220 Welcome\r\n", &consumed);
        QVERIFY(r.complete);
        QCOMPARE(r.code, 220);
        QCOMPARE(r.text, QString("Welcome"));
        QCOMPARE(consumed, 13);
    }

    void parseIncompleteReplyIsNotComplete() {
        int consumed = 0;
        const FtpReply r = parseReply("220 Welco", &consumed);
        QVERIFY(!r.complete);
        QCOMPARE(consumed, 0);
    }

    void parseMultiLineReply() {
        int consumed = 0;
        const QByteArray buf = "213-Status follows\r\n details\r\n213 Done\r\n";
        const FtpReply r = parseReply(buf, &consumed);
        QVERIFY(r.complete);
        QCOMPARE(r.code, 213);
        QCOMPARE(r.text, QString("Done"));
        QCOMPARE(consumed, buf.size());
    }

    void parseMultiLineReplyIncompleteWaitsForTerminator() {
        int consumed = 0;
        const FtpReply r = parseReply("213-Status follows\r\n details\r\n", &consumed);
        QVERIFY(!r.complete);
        QCOMPARE(consumed, 0);
    }

    // Uma linha interna que começa com OUTRO código não termina a resposta.
    void parseMultiLineIgnoresForeignCode() {
        int consumed = 0;
        const QByteArray buf = "213-Start\r\n226 not the terminator\r\n213 End\r\n";
        const FtpReply r = parseReply(buf, &consumed);
        QVERIFY(r.complete);
        QCOMPARE(r.code, 213);
        QCOMPARE(r.text, QString("End"));
        QCOMPARE(consumed, buf.size());
    }

    void parseReplyLeavesTrailingBytes() {
        int consumed = 0;
        const FtpReply r = parseReply("220 Hi\r\n331 Next\r\n", &consumed);
        QVERIFY(r.complete);
        QCOMPARE(r.code, 220);
        QCOMPARE(consumed, 8);            // só a primeira resposta é consumida
    }

    // --- parsePasv ------------------------------------------------------
    void parsePasvCanonical() {
        const auto r = parsePasv("227 Entering Passive Mode (127,0,0,1,195,80).");
        QVERIFY(r.has_value());
        QCOMPARE(r->first, QString("127.0.0.1"));
        QCOMPARE(r->second, quint16(195 * 256 + 80));
    }

    void parsePasvWithoutTrailingDot() {
        const auto r = parsePasv("227 Entering Passive Mode (10,0,0,7,4,1)");
        QVERIFY(r.has_value());
        QCOMPARE(r->first, QString("10.0.0.7"));
        QCOMPARE(r->second, quint16(4 * 256 + 1));
    }

    void parsePasvMalformedReturnsNullopt() {
        QVERIFY(!parsePasv("227 Entering Passive Mode").has_value());
        QVERIFY(!parsePasv("227 (1,2,3)").has_value());
        QVERIFY(!parsePasv("227 (1,2,3,4,5,6,7)").has_value());
        QVERIFY(!parsePasv("227 (1,2,3,4,999,80)").has_value());   // octeto > 255
    }

    // --- parseMdtm ------------------------------------------------------
    void parseMdtmCanonical() {
        const auto r = parseMdtm("213 20260717123045");
        QVERIFY(r.has_value());
        QCOMPARE(r->toUTC(), QDateTime(QDate(2026, 7, 17), QTime(12, 30, 45), QTimeZone::UTC));
    }

    void parseMdtmMalformedReturnsNullopt() {
        QVERIFY(!parseMdtm("213 nonsense").has_value());
        QVERIFY(!parseMdtm("550 Not found").has_value());
        QVERIFY(!parseMdtm("213 2026071712").has_value());   // curto demais
    }

    // --- TestFtpServer (double) -----------------------------------------
    // O double precisa ser confiável antes de qualquer teste de cliente.
    void testServerSpeaksBasicFtp() {
        TestFtpServer srv(QByteArray("HELLO"));
        QVERIFY(srv.listen());

        QTcpSocket c;
        c.connectToHost("127.0.0.1", srv.port());
        QVERIFY(c.waitForConnected(3000));

        auto readLine = [&c]() -> QString {
            if (!c.canReadLine() && !QTest::qWaitFor([&] { return c.canReadLine(); }, 3000))
                return QString();
            return QString::fromUtf8(c.readLine()).trimmed();
        };
        auto cmd = [&c, &readLine](const QString& s) {
            c.write((s + "\r\n").toUtf8());
            c.flush();
            return readLine();
        };

        QVERIFY(readLine().startsWith("220"));
        QVERIFY(cmd("USER anonymous").startsWith("331"));
        QVERIFY(cmd("PASS x@y").startsWith("230"));
        QVERIFY(cmd("TYPE I").startsWith("200"));
        QCOMPARE(cmd("SIZE /f.bin"), QString("213 5"));
        QVERIFY(cmd("MDTM /f.bin").startsWith("213 2026"));
        QVERIFY(cmd("REST 1").startsWith("350"));
    }

    void testServerHonorsKnobs() {
        TestFtpServer srv(QByteArray("HELLO"));
        srv.setNoSize(true);
        srv.setNoRest(true);
        srv.requireAuth("bob", "secret");
        QVERIFY(srv.listen());

        QTcpSocket c;
        c.connectToHost("127.0.0.1", srv.port());
        QVERIFY(c.waitForConnected(3000));
        auto readLine = [&c]() -> QString {
            if (!c.canReadLine() && !QTest::qWaitFor([&] { return c.canReadLine(); }, 3000))
                return QString();
            return QString::fromUtf8(c.readLine()).trimmed();
        };
        auto cmd = [&c, &readLine](const QString& s) {
            c.write((s + "\r\n").toUtf8());
            c.flush();
            return readLine();
        };

        QVERIFY(readLine().startsWith("220"));
        QVERIFY(cmd("USER anonymous").startsWith("331"));
        QVERIFY(cmd("PASS x@y").startsWith("530"));      // credencial errada
        QVERIFY(cmd("USER bob").startsWith("331"));
        QVERIFY(cmd("PASS secret").startsWith("230"));
        QVERIFY(cmd("SIZE /f.bin").startsWith("502"));   // noSize
        QVERIFY(cmd("REST 1").startsWith("502"));        // noRest
    }

    // --- FtpControlChannel ----------------------------------------------
    void controlChannelLogsInAnonymously() {
        TestFtpServer srv(QByteArray("HELLO"));
        QVERIFY(srv.listen());
        FtpControlChannel ch;
        QSignalSpy in(&ch, &FtpControlChannel::loggedIn);
        ch.connectAndLogin(srv.url(), Credentials{}, 3000);
        QVERIFY(in.wait(3000));
    }

    void controlChannelLogsInWithCredentials() {
        TestFtpServer srv(QByteArray("HELLO"));
        srv.requireAuth("bob", "secret");
        QVERIFY(srv.listen());
        FtpControlChannel ch;
        QSignalSpy in(&ch, &FtpControlChannel::loggedIn);
        ch.connectAndLogin(srv.url(), Credentials{"bob", "secret"}, 3000);
        QVERIFY(in.wait(3000));
    }

    void controlChannelReportsAuthFailure() {
        TestFtpServer srv(QByteArray("HELLO"));
        srv.requireAuth("bob", "secret");
        QVERIFY(srv.listen());
        FtpControlChannel ch;
        QSignalSpy bad(&ch, &FtpControlChannel::failed);
        ch.connectAndLogin(srv.url(), Credentials{"bob", "wrong"}, 3000);
        QVERIFY(bad.wait(3000));
        QCOMPARE(bad.at(0).at(1).value<FtpErrorClass>(), FtpErrorClass::Auth);
    }

    void controlChannelSendsCommandAndParsesReply() {
        TestFtpServer srv(QByteArray("HELLO"));
        QVERIFY(srv.listen());
        FtpControlChannel ch;
        QSignalSpy in(&ch, &FtpControlChannel::loggedIn);
        QSignalSpy rep(&ch, &FtpControlChannel::replyReceived);
        ch.connectAndLogin(srv.url(), Credentials{}, 3000);
        QVERIFY(in.wait(3000));
        ch.sendCommand("SIZE /f.bin");
        QVERIFY(rep.wait(3000));
        QCOMPARE(rep.at(0).at(0).toInt(), 213);
        QCOMPARE(rep.at(0).at(1).toString(), QString("5"));
    }

    void controlChannelClassifiesTooManyConnectionsAsTransient() {
        TestFtpServer srv(QByteArray("HELLO"));
        srv.setMaxConnections(0);          // toda conexão leva 421
        QVERIFY(srv.listen());
        FtpControlChannel ch;
        QSignalSpy bad(&ch, &FtpControlChannel::failed);
        ch.connectAndLogin(srv.url(), Credentials{}, 3000);
        QVERIFY(bad.wait(3000));
        QCOMPARE(bad.at(0).at(1).value<FtpErrorClass>(), FtpErrorClass::Transient);
    }

    void controlChannelTimesOutOnDeadPort() {
        // Porta fechada: erro de conexão -> Transient (vale a pena tentar de novo).
        FtpControlChannel ch;
        QSignalSpy bad(&ch, &FtpControlChannel::failed);
        QUrl u; u.setScheme("ftp"); u.setHost("127.0.0.1"); u.setPort(1); u.setPath("/f.bin");
        ch.connectAndLogin(u, Credentials{}, 2000);
        QVERIFY(bad.wait(5000));
        QCOMPARE(bad.at(0).at(1).value<FtpErrorClass>(), FtpErrorClass::Transient);
    }

    // --- FtpProbe -------------------------------------------------------
    // Critério 4
    void ftpProbeHappyPath() {
        TestFtpServer srv(makeBody(5000));
        srv.setMdtm("20260717123045");
        QVERIFY(srv.listen());
        EngineConfig cfg;
        FtpProbe probe(cfg);
        QSignalSpy spy(&probe, &Probe::finished);
        probe.start(srv.url(), Credentials{}, HeaderList{});
        QVERIFY(spy.wait(5000));
        const auto r = qvariant_cast<ProbeResult>(spy.at(0).at(0));
        QVERIFY(r.ok);
        QVERIFY(r.supportsRange);
        QCOMPARE(r.totalBytes, qint64(5000));
        QCOMPARE(r.lastModified, QString("20260717123045"));
        QVERIFY(r.etag.isEmpty());          // FTP não tem ETag
        QVERIFY(!r.authRequired);
    }

    // Critério 5 (sem SIZE)
    void ftpProbeWithoutSizeReportsUnknownTotal() {
        TestFtpServer srv(makeBody(5000));
        srv.setNoSize(true);
        QVERIFY(srv.listen());
        EngineConfig cfg;
        FtpProbe probe(cfg);
        QSignalSpy spy(&probe, &Probe::finished);
        probe.start(srv.url(), Credentials{}, HeaderList{});
        QVERIFY(spy.wait(5000));
        const auto r = qvariant_cast<ProbeResult>(spy.at(0).at(0));
        QVERIFY(r.ok);
        QCOMPARE(r.totalBytes, qint64(-1));
        QVERIFY(!r.supportsRange);          // sem tamanho não há como segmentar
    }

    // Critério 5 (sem REST)
    void ftpProbeWithoutRestReportsNoRangeSupport() {
        TestFtpServer srv(makeBody(5000));
        srv.setNoRest(true);
        QVERIFY(srv.listen());
        EngineConfig cfg;
        FtpProbe probe(cfg);
        QSignalSpy spy(&probe, &Probe::finished);
        probe.start(srv.url(), Credentials{}, HeaderList{});
        QVERIFY(spy.wait(5000));
        const auto r = qvariant_cast<ProbeResult>(spy.at(0).at(0));
        QVERIFY(r.ok);
        QCOMPARE(r.totalBytes, qint64(5000));
        QVERIFY(!r.supportsRange);
    }

    void ftpProbeWithoutMdtmHasEmptyValidator() {
        TestFtpServer srv(makeBody(5000));
        srv.setNoMdtm(true);
        QVERIFY(srv.listen());
        EngineConfig cfg;
        FtpProbe probe(cfg);
        QSignalSpy spy(&probe, &Probe::finished);
        probe.start(srv.url(), Credentials{}, HeaderList{});
        QVERIFY(spy.wait(5000));
        const auto r = qvariant_cast<ProbeResult>(spy.at(0).at(0));
        QVERIFY(r.ok);
        QVERIFY(r.lastModified.isEmpty());
        QVERIFY(r.supportsRange);           // REST continua funcionando
    }

    // Critério 12 (auth pelo probe)
    void ftpProbeReportsAuthRequired() {
        TestFtpServer srv(makeBody(500));
        srv.requireAuth("bob", "secret");
        QVERIFY(srv.listen());
        EngineConfig cfg;
        FtpProbe probe(cfg);
        QSignalSpy spy(&probe, &Probe::finished);
        probe.start(srv.url(), Credentials{}, HeaderList{});      // anônimo -> 530
        QVERIFY(spy.wait(5000));
        const auto r = qvariant_cast<ProbeResult>(spy.at(0).at(0));
        QVERIFY(!r.ok);
        QVERIFY(r.authRequired);
    }

    void ftpProbeSucceedsWithCredentials() {
        TestFtpServer srv(makeBody(500));
        srv.requireAuth("bob", "secret");
        QVERIFY(srv.listen());
        EngineConfig cfg;
        FtpProbe probe(cfg);
        QSignalSpy spy(&probe, &Probe::finished);
        probe.start(srv.url(), Credentials{"bob", "secret"}, HeaderList{});
        QVERIFY(spy.wait(5000));
        const auto r = qvariant_cast<ProbeResult>(spy.at(0).at(0));
        QVERIFY(r.ok);
        QVERIFY(!r.authRequired);
        QCOMPARE(r.totalBytes, qint64(500));
    }

    void ftpTransportIsRegisteredForFtpScheme() {
        QTemporaryDir dir;
        EngineConfig cfg;
        DownloadManager mgr(cfg, dir.path());
        QVERIFY(mgr.transportFor(QUrl("ftp://h/f")) != nullptr);
    }

    // Critério 8: o worker corta no end, mesmo o servidor mandando até o fim.
    void ftpWorkerCutsAtSegmentEnd() {
        const QByteArray body = makeBody(10000);
        TestFtpServer srv(body);
        QVERIFY(srv.listen());
        QTemporaryDir dir;
        QFile f(dir.path() + "/out.bin");
        QVERIFY(f.open(QIODevice::ReadWrite));
        f.resize(body.size());

        EngineConfig cfg;
        FtpSegmentWorker w(&f, cfg);
        QSignalSpy done(&w, &SegmentSource::completed);

        Segment seg;                       // fatia do meio: [2000, 4999]
        seg.index = 0; seg.start = 2000; seg.current = 2000; seg.end = 4999;
        w.start(seg, srv.url(), QString(), Credentials{}, HeaderList{});
        QVERIFY(done.wait(5000));

        QCOMPARE(w.segment().current, qint64(5000));   // parou logo após o end
        f.seek(2000);
        QCOMPARE(f.read(3000), body.mid(2000, 3000));
        // E não escreveu além do end: o byte 5000 continua zerado.
        f.seek(5000);
        QCOMPARE(f.read(1), QByteArray(1, '\0'));
    }

    // Critério 9: fallback (end == -1) lê até o fim.
    void ftpWorkerFallbackReadsToEof() {
        const QByteArray body = makeBody(8000);
        TestFtpServer srv(body);
        QVERIFY(srv.listen());
        QTemporaryDir dir;
        QFile f(dir.path() + "/out.bin");
        QVERIFY(f.open(QIODevice::ReadWrite));

        EngineConfig cfg;
        FtpSegmentWorker w(&f, cfg);
        QSignalSpy done(&w, &SegmentSource::completed);

        Segment seg;
        seg.index = 0; seg.start = 0; seg.current = 0; seg.end = -1;
        w.start(seg, srv.url(), QString(), Credentials{}, HeaderList{});
        QVERIFY(done.wait(5000));

        f.flush();
        QCOMPARE(w.segment().end, qint64(7999));   // end preenchido no EOF
        f.seek(0);
        QCOMPARE(f.readAll(), body);
    }

    // Critério 7: download FTP multi-segmento completo, byte-idêntico.
    void ftpMultiSegmentDownloadIsByteIdentical() {
        const QByteArray body = makeBody(1 << 16);   // 64 KiB
        TestFtpServer srv(body);
        QVERIFY(srv.listen());
        QTemporaryDir dir;
        const QString dest = dir.path() + "/out.bin";

        EngineConfig cfg;
        cfg.minSegSize = 1024;                       // força 4 segmentos reais
        DownloadManager mgr(cfg, dir.path());
        const QUuid id = mgr.addDownload(srv.url(), dest);
        QVERIFY(!id.isNull());

        QVERIFY(QTest::qWaitFor([&]{
            DownloadTask* t = mgr.taskById(id);
            return t && t->state() == DownloadState::Completed;
        }, 15000));

        QFile f(dest);
        QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(f.readAll(), body);
        QVERIFY(srv.controlConnections() >= 4);      // um por segmento + o probe
    }

    // Critério 9 integrado: servidor sem REST -> um segmento, arquivo íntegro.
    void ftpFallbackDownloadIsByteIdentical() {
        const QByteArray body = makeBody(20000);
        TestFtpServer srv(body);
        srv.setNoRest(true);
        QVERIFY(srv.listen());
        QTemporaryDir dir;
        const QString dest = dir.path() + "/out.bin";

        EngineConfig cfg;
        DownloadManager mgr(cfg, dir.path());
        const QUuid id = mgr.addDownload(srv.url(), dest);

        QVERIFY(QTest::qWaitFor([&]{
            DownloadTask* t = mgr.taskById(id);
            return t && t->state() == DownloadState::Completed;
        }, 15000));

        QFile f(dest);
        QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(f.readAll(), body);
    }

    // Queda no meio da transferência -> retry -> completa, retomando do offset
    // já gravado (não do zero).
    void ftpWorkerRetriesAfterDataDrop() {
        const QByteArray body = makeBody(10000);
        TestFtpServer srv(body);
        srv.setDropAfter(3000);            // morre aos 3000 bytes...
        srv.setDropOnce(true);             // ...mas só na primeira vez
        QVERIFY(srv.listen());
        QTemporaryDir dir;
        QFile f(dir.path() + "/out.bin");
        QVERIFY(f.open(QIODevice::ReadWrite));
        f.resize(body.size());

        EngineConfig cfg;
        cfg.retryBackoffBaseMs = 50;
        FtpSegmentWorker w(&f, cfg);
        QSignalSpy done(&w, &SegmentSource::completed);

        Segment seg;
        seg.index = 0; seg.start = 0; seg.current = 0; seg.end = 9999;
        w.start(seg, srv.url(), QString(), Credentials{}, HeaderList{});

        QVERIFY(done.wait(15000));
        QCOMPARE(w.segment().current, qint64(10000));
        f.flush();
        f.seek(0);
        QCOMPARE(f.read(10000), body);     // sem buraco nem byte duplicado na emenda
        QVERIFY(srv.controlConnections() >= 2);   // houve mesmo uma segunda tentativa
    }

    // Retries esgotados -> desiste com failed, não trava.
    void ftpWorkerGivesUpAfterMaxRetries() {
        const QByteArray body = makeBody(10000);
        TestFtpServer srv(body);
        srv.setDropAfter(3000);            // toda transferência morre
        QVERIFY(srv.listen());
        QTemporaryDir dir;
        QFile f(dir.path() + "/out.bin");
        QVERIFY(f.open(QIODevice::ReadWrite));
        f.resize(body.size());

        EngineConfig cfg;
        cfg.retryBackoffBaseMs = 10;
        cfg.maxSegmentRetries  = 2;
        FtpSegmentWorker w(&f, cfg);
        QSignalSpy bad(&w, &SegmentSource::failed);

        Segment seg;
        seg.index = 0; seg.start = 0; seg.current = 0; seg.end = 9999;
        w.start(seg, srv.url(), QString(), Credentials{}, HeaderList{});

        QVERIFY(bad.wait(15000));
        QCOMPARE(bad.at(0).at(2).value<FailureKind>(), FailureKind::Fatal);
    }

    // --- Task 8: throttle do worker FTP via RateLimiter ------------------

    // Corpo grande o bastante para que o drain sob throttle abranja vários
    // ticks de 20ms - isso expõe corrupção/short-read espúrio se a leitura
    // parcial (grant<avail) ou o drain residual em onDataFinished() não
    // preservarem o corte no 'end' / a ordem dos bytes escritos.
    void ftpThrottledDownloadStillCompletesIntact() {
        const QByteArray big = makeBody(192 * 1024);
        TestFtpServer srv(big);
        QVERIFY(srv.listen());
        QTemporaryDir dir;
        const QString dest = dir.path() + "/cap.bin";

        EngineConfig cfg;
        cfg.minSegSize = 1024;                   // força múltiplos segmentos reais
        cfg.maxBytesPerSec = 64 * 1024;           // teto baixo, mas download deve terminar
        DownloadManager mgr(cfg, dir.path());
        const QUuid id = mgr.addDownload(srv.url(), dest);
        QVERIFY(!id.isNull());

        QVERIFY(QTest::qWaitFor([&]{
            DownloadTask* t = mgr.taskById(id);
            return t && t->state() == DownloadState::Completed;
        }, 20000));

        QFile f(dest);
        QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(f.readAll(), big);               // conteúdo íntegro sob throttle
    }

    // --- Task 9: resume FTP + validação por MDTM ------------------------

    // Critério 11: MDTM divergente -> restartRequired.
    void ftpWorkerRestartsOnMdtmMismatch() {
        const QByteArray body = makeBody(10000);
        TestFtpServer srv(body);
        srv.setMdtm("20260717120000");
        QVERIFY(srv.listen());
        QTemporaryDir dir;
        QFile f(dir.path() + "/out.bin");
        QVERIFY(f.open(QIODevice::ReadWrite));
        f.resize(body.size());

        EngineConfig cfg;
        FtpSegmentWorker w(&f, cfg);
        QSignalSpy restart(&w, &SegmentSource::restartRequired);

        Segment seg;
        seg.index = 0; seg.start = 0; seg.current = 500; seg.end = 9999;
        // validador do .meta != MDTM do servidor
        w.start(seg, srv.url(), QString("20260101000000"), Credentials{}, HeaderList{});
        QVERIFY(restart.wait(5000));
        QCOMPARE(restart.at(0).at(0).toInt(), 0);
    }

    // MDTM igual -> segue direto, sem restart.
    void ftpWorkerProceedsOnMdtmMatch() {
        const QByteArray body = makeBody(10000);
        TestFtpServer srv(body);
        srv.setMdtm("20260717120000");
        QVERIFY(srv.listen());
        QTemporaryDir dir;
        QFile f(dir.path() + "/out.bin");
        QVERIFY(f.open(QIODevice::ReadWrite));
        f.resize(body.size());

        EngineConfig cfg;
        FtpSegmentWorker w(&f, cfg);
        QSignalSpy done(&w, &SegmentSource::completed);
        QSignalSpy restart(&w, &SegmentSource::restartRequired);

        Segment seg;
        seg.index = 0; seg.start = 0; seg.current = 0; seg.end = 9999;
        w.start(seg, srv.url(), QString("20260717120000"), Credentials{}, HeaderList{});
        QVERIFY(done.wait(5000));
        QCOMPARE(restart.count(), 0);
    }

    // Validador vazio -> nem manda MDTM (spec §3.5: resume não-validado).
    void ftpWorkerSkipsMdtmWithoutValidator() {
        const QByteArray body = makeBody(5000);
        TestFtpServer srv(body);
        QVERIFY(srv.listen());
        QTemporaryDir dir;
        QFile f(dir.path() + "/out.bin");
        QVERIFY(f.open(QIODevice::ReadWrite));
        f.resize(body.size());

        EngineConfig cfg;
        FtpSegmentWorker w(&f, cfg);
        QSignalSpy done(&w, &SegmentSource::completed);
        QSignalSpy restart(&w, &SegmentSource::restartRequired);

        Segment seg;
        seg.index = 0; seg.start = 0; seg.current = 0; seg.end = 4999;
        w.start(seg, srv.url(), QString(), Credentials{}, HeaderList{});
        QVERIFY(done.wait(5000));
        QCOMPARE(restart.count(), 0);
    }

    // Critério 10: resume multi-segmento entre "sessões", MDTM inalterado.
    void ftpResumeAcrossSessionsWithValidMdtm() {
        // 4 MiB: corpo grande o bastante p/ que o download NÃO termine antes de
        // conseguirmos pausar. Com 64 KiB de localhost o teste completaria na
        // primeira sessão e não testaria resume nenhum.
        const QByteArray body = makeBody(4 << 20);
        TestFtpServer srv(body);
        srv.setMdtm("20260717120000");
        srv.setDropAfter(64 << 10);        // cada transferência morre aos 64 KiB...
        QVERIFY(srv.listen());
        QTemporaryDir dir;
        const QString dest = dir.path() + "/out.bin";

        EngineConfig cfg;
        cfg.minSegSize = 1024;

        // Sessão 1: baixa um pedaço e pausa. O dropAfter garante que a sessão 1
        // não consiga completar, tornando o pause determinístico.
        {
            DownloadManager mgr(cfg, dir.path());
            const QUuid id = mgr.addDownload(srv.url(), dest);
            qint64 got = 0;
            connect(&mgr, &DownloadManager::taskProgress, this,
                    [&got](const QUuid&, qint64 r, qint64) { got = r; });
            QVERIFY(QTest::qWaitFor([&]{ return got > 0; }, 10000));
            mgr.pause(id);
            QVERIFY(QTest::qWaitFor([&]{
                DownloadTask* t = mgr.taskById(id);
                return t && t->state() == DownloadState::Paused;
            }, 5000));
            QVERIFY(got < body.size());     // realmente parou no meio
        }

        srv.setDropAfter(-1);              // ...mas a sessão 2 pode completar

        // Sessão 2: recarrega do .meta e retoma até completar.
        {
            DownloadManager mgr(cfg, dir.path());
            mgr.loadSession();
            QCOMPARE(mgr.tasks().size(), 1);
            const QUuid id = mgr.tasks().first()->id();
            mgr.resume(id);
            QVERIFY(QTest::qWaitFor([&]{
                DownloadTask* t = mgr.taskById(id);
                return t && t->state() == DownloadState::Completed;
            }, 20000));
        }

        QFile f(dest);
        QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(f.readAll(), body);
    }

    // Critério 11 integrado: arquivo muda entre sessões -> descarta parcial.
    void ftpResumeWithChangedFileRestartsFromZero() {
        const QByteArray v1 = makeBody(4 << 20);
        TestFtpServer srv(v1);
        srv.setMdtm("20260717120000");
        srv.setDropAfter(64 << 10);        // sessão 1 não completa (ver teste acima)
        QVERIFY(srv.listen());
        QTemporaryDir dir;
        const QString dest = dir.path() + "/out.bin";

        EngineConfig cfg;
        cfg.minSegSize = 1024;

        {
            DownloadManager mgr(cfg, dir.path());
            const QUuid id = mgr.addDownload(srv.url(), dest);
            qint64 got = 0;
            connect(&mgr, &DownloadManager::taskProgress, this,
                    [&got](const QUuid&, qint64 r, qint64) { got = r; });
            QVERIFY(QTest::qWaitFor([&]{ return got > 0; }, 10000));
            mgr.pause(id);
            QVERIFY(QTest::qWaitFor([&]{
                DownloadTask* t = mgr.taskById(id);
                return t && t->state() == DownloadState::Paused;
            }, 5000));
            QVERIFY(got < v1.size());
        }

        // O arquivo mudou no servidor: mesmo tamanho, conteúdo e MDTM outros.
        QByteArray v2 = v1;
        for (int i = 0; i < v2.size(); ++i) v2[i] = char('z' - (i % 26));
        srv.setContent(v2);
        srv.setMdtm("20260718090000");
        srv.setDropAfter(-1);              // sessão 2 pode completar

        {
            DownloadManager mgr(cfg, dir.path());
            mgr.loadSession();
            const QUuid id = mgr.tasks().first()->id();
            mgr.resume(id);
            QVERIFY(QTest::qWaitFor([&]{
                DownloadTask* t = mgr.taskById(id);
                return t && t->state() == DownloadState::Completed;
            }, 20000));
        }

        // Resultado: v2 puro. Se o parcial de v1 tivesse sobrevivido, seria um
        // Frankenstein dos dois.
        QFile f(dest);
        QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(f.readAll(), v2);
    }

    // Critério 12: auth pelo probe (download novo).
    void ftpAuthRequiredFromProbePausesAndAsks() {
        TestFtpServer srv(makeBody(5000));
        srv.requireAuth("bob", "secret");
        QVERIFY(srv.listen());
        QTemporaryDir dir;
        const QString dest = dir.path() + "/out.bin";

        EngineConfig cfg;
        DownloadManager mgr(cfg, dir.path());
        QSignalSpy ask(&mgr, &DownloadManager::credentialsRequired);
        const QUuid id = mgr.addDownload(srv.url(), dest);

        QVERIFY(ask.wait(5000));
        QCOMPARE(ask.count(), 1);
        QCOMPARE(ask.at(0).at(0).value<QUuid>(), id);
        QCOMPARE(ask.at(0).at(1).toString(), QString("127.0.0.1"));
        QCOMPARE(mgr.taskById(id)->state(), DownloadState::Paused);

        mgr.provideCredentials(id, "bob", "secret");
        QVERIFY(QTest::qWaitFor([&]{
            DownloadTask* t = mgr.taskById(id);
            return t && t->state() == DownloadState::Completed;
        }, 20000));

        QFile f(dest);
        QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(f.readAll(), makeBody(5000));
    }

    // Critério 13: auth pelo worker (resume) — e UM único sinal p/ N segmentos.
    void ftpAuthRequiredFromWorkerAsksOnce() {
        const QByteArray body = makeBody(4 << 20);
        TestFtpServer srv(body);
        srv.setMdtm("20260717120000");
        srv.setDropAfter(64 << 10);
        QVERIFY(srv.listen());
        QTemporaryDir dir;
        const QString dest = dir.path() + "/out.bin";

        EngineConfig cfg;
        cfg.minSegSize = 1024;

        // Sessão 1: anônimo funciona; baixa um pouco e pausa. O dropAfter torna
        // o pause determinístico (senão o download completa antes).
        {
            DownloadManager mgr(cfg, dir.path());
            const QUuid id = mgr.addDownload(srv.url(), dest);
            qint64 got = 0;
            connect(&mgr, &DownloadManager::taskProgress, this,
                    [&got](const QUuid&, qint64 r, qint64) { got = r; });
            QVERIFY(QTest::qWaitFor([&]{ return got > 0; }, 10000));
            mgr.pause(id);
            QVERIFY(QTest::qWaitFor([&]{
                DownloadTask* t = mgr.taskById(id);
                return t && t->state() == DownloadState::Paused;
            }, 5000));
            QVERIFY(got < body.size());
        }

        // O servidor passa a exigir login. Sessão 2 não sonda (m_probed do
        // .meta): os N workers levam 530 juntos.
        srv.requireAuth("bob", "secret");
        srv.setDropAfter(-1);

        {
            DownloadManager mgr(cfg, dir.path());
            mgr.loadSession();
            const QUuid id = mgr.tasks().first()->id();
            QSignalSpy ask(&mgr, &DownloadManager::credentialsRequired);
            mgr.resume(id);

            QVERIFY(ask.wait(10000));
            QTest::qWait(500);                    // dá tempo p/ os outros workers falharem
            QCOMPARE(ask.count(), 1);             // UM diálogo, não N
            QCOMPARE(mgr.taskById(id)->state(), DownloadState::Paused);

            mgr.provideCredentials(id, "bob", "secret");
            QVERIFY(QTest::qWaitFor([&]{
                DownloadTask* t = mgr.taskById(id);
                return t && t->state() == DownloadState::Completed;
            }, 20000));
        }

        QFile f(dest);
        QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(f.readAll(), body);
    }

    // Critério 12: 550 (arquivo inexistente) é fatal — Error, sem retry e sem
    // diálogo de credenciais.
    void ftpMissingFileIsFatal() {
        TestFtpServer srv(makeBody(100));
        srv.setMissing(true);                     // 550 em SIZE/MDTM/RETR
        QVERIFY(srv.listen());
        QTemporaryDir dir;
        EngineConfig cfg;
        cfg.retryBackoffBaseMs = 10;
        DownloadManager mgr(cfg, dir.path());
        QSignalSpy ask(&mgr, &DownloadManager::credentialsRequired);

        const QUuid id = mgr.addDownload(srv.url(), dir.path() + "/out.bin");
        QVERIFY(QTest::qWaitFor([&]{
            DownloadTask* t = mgr.taskById(id);
            return t && t->state() == DownloadState::Error;
        }, 10000));
        QCOMPARE(ask.count(), 0);                 // nunca pediu credenciais
    }
};

QTEST_MAIN(TstFtp)
#include "tst_ftp.moc"
