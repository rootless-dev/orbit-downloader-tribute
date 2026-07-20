#include <QtTest>
#include <QTemporaryDir>
#include <QSignalSpy>
#include <QFile>
#include "Logger.h"

static QString readAll(const QString& path) {
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
}

class TstLogger : public QObject {
    Q_OBJECT
private slots:
    void appLogWritesFormattedLine() {
        QTemporaryDir dir;
        Logger log(dir.path());
        QSignalSpy spy(&log, &Logger::lineLogged);
        log.logApp(LogLevel::Info, "hello");
        const QString content = readAll(log.logsDir() + "/app.log");
        QVERIFY(content.contains("[INFO] hello"));
        QVERIFY(content.contains("-"));                       // tem data
        QCOMPARE(spy.count(), 1);
        QVERIFY(spy.at(0).at(0).value<QUuid>().isNull());     // app => id nulo
    }

    void taskLogGoesToPerDownloadFile() {
        QTemporaryDir dir;
        Logger log(dir.path());
        const QUuid id = QUuid::createUuid();
        const QString dest = "/tmp/movie.flv";
        QSignalSpy spy(&log, &Logger::lineLogged);
        log.logTask(id, dest, LogLevel::Warn, "retry seg 2");
        const QString path = log.taskLogPath(id, dest);
        QVERIFY(path.endsWith(".log"));
        QVERIFY(path.contains("movie"));
        QVERIFY(readAll(path).contains("[WARN] retry seg 2"));
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(0).value<QUuid>(), id);
    }

    void taskLogPathIsDeterministic() {
        QTemporaryDir dir;
        Logger log(dir.path());
        const QUuid id = QUuid::createUuid();
        QCOMPARE(log.taskLogPath(id, "/a/movie.flv"), log.taskLogPath(id, "/a/movie.flv"));
    }

    void appLogRotatesWhenTooBig() {
        QTemporaryDir dir;
        Logger log(dir.path());
        for (int i = 0; i < 60000; ++i) log.logApp(LogLevel::Debug, QString(200, 'x'));
        QVERIFY(QFile::exists(log.logsDir() + "/app.log.1"));   // rotacionou
    }
};

QTEST_MAIN(TstLogger)
#include "tst_logger.moc"
