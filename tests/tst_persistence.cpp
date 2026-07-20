#include <QtTest>
#include <QTemporaryDir>
#include <QJsonObject>
#include <QFile>
#include "Persistence.h"

class TstPersistence : public QObject {
    Q_OBJECT
private slots:
    void metaRoundTrip() {
        QTemporaryDir dir;
        const QString dest = dir.filePath("file.bin");
        QVector<Segment> segs = {{0,0,120,499}, {1,500,500,999}};
        QVERIFY(Persistence::writeMeta(dest, segs, "\"abc\"", "Mon", true));
        QVERIFY(QFile::exists(Persistence::metaPath(dest)));

        QVector<Segment> back; QString etag, lm; bool validated=false;
        QVERIFY(Persistence::readMeta(dest, back, etag, lm, validated));
        QCOMPARE(back.size(), 2);
        QCOMPARE(back[0].current, 120LL);
        QCOMPARE(back[1].end, 999LL);
        QCOMPARE(etag, QString("\"abc\""));
        QCOMPARE(validated, true);
    }
    void removeMetaDeletesFile() {
        QTemporaryDir dir;
        const QString dest = dir.filePath("f.bin");
        Persistence::writeMeta(dest, {{0,0,0,9}}, "", "", false);
        Persistence::removeMeta(dest);
        QVERIFY(!QFile::exists(Persistence::metaPath(dest)));
    }
    void sessionRoundTrip() {
        QTemporaryDir dir;
        const QString path = dir.filePath("downloads.json");
        DownloadRecord r;
        r.id = QUuid::createUuid();
        r.url = QUrl("http://x/y.bin");
        r.destPath = "/tmp/y.bin";
        r.totalBytes = 1000; r.supportsRange = true;
        r.state = DownloadState::Paused; r.segmentCount = 4;
        QVERIFY(Persistence::writeSession(path, {r}));

        auto back = Persistence::readSession(path);
        QCOMPARE(back.size(), 1);
        QCOMPARE(back[0].id, r.id);
        QCOMPARE(back[0].url, r.url);
        QCOMPARE(back[0].totalBytes, 1000LL);
        QCOMPARE(back[0].state, DownloadState::Paused);
    }
    void resolveUniqueAvoidsCollision() {
        QTemporaryDir dir;
        const QString a = dir.filePath("movie.flv");
        QFile(a).open(QIODevice::WriteOnly);          // create it
        const QString got = Persistence::resolveUniquePath(a);
        QCOMPARE(got, dir.filePath("movie (1).flv"));
    }
    void resolveUniqueReturnsSameWhenFree() {
        QTemporaryDir dir;
        const QString a = dir.filePath("free.bin");
        QCOMPARE(Persistence::resolveUniquePath(a), a);
    }
    void atomicWriteReplaces() {
        QTemporaryDir dir;
        const QString p = dir.filePath("x.txt");
        QVERIFY(Persistence::writeFileAtomic(p, "one"));
        QVERIFY(Persistence::writeFileAtomic(p, "two"));
        QFile f(p); f.open(QIODevice::ReadOnly);
        QCOMPARE(f.readAll(), QByteArray("two"));
    }
    void sessionRoundTripsPriorityAndCancelled() {
        QTemporaryDir dir;
        const QString path = dir.path() + "/downloads.json";
        DownloadRecord r;
        r.id = QUuid::createUuid();
        r.url = QUrl("http://example.com/f.bin");
        r.destPath = dir.path() + "/f.bin";
        r.state = DownloadState::Cancelled;
        r.priority = Priority::High;
        QVERIFY(Persistence::writeSession(path, {r}));
        const auto back = Persistence::readSession(path);
        QCOMPARE(back.size(), 1);
        QCOMPARE(back[0].state, DownloadState::Cancelled);
        QCOMPARE(back[0].priority, Priority::High);
    }

    void legacySessionDefaultsPriorityToNormal() {
        QTemporaryDir dir;
        const QString path = dir.path() + "/downloads.json";
        // JSON sem o campo "priority" (formato antigo):
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(R"([{"id":"11111111-1111-1111-1111-111111111111","url":"http://x/f",)"
                R"("destPath":"/tmp/f","totalBytes":10,"supportsRange":true,"state":3,"segmentCount":4}])");
        f.close();
        const auto back = Persistence::readSession(path);
        QCOMPARE(back.size(), 1);
        QCOMPARE(back[0].priority, Priority::Normal);
    }

    void sessionRoundTripsExtraHeaders() {
        QTemporaryDir dir;
        const QString path = dir.filePath("dl.json");
        DownloadRecord rec;
        rec.id       = QUuid::createUuid();
        rec.url      = QUrl("https://example.com/f.zip");
        rec.destPath = "/tmp/f.zip";
        rec.state    = DownloadState::Paused;
        rec.extraHeaders = {
            {QByteArray("Cookie"),     QByteArray("sid=abc; t=1")},
            {QByteArray("Referer"),    QByteArray("https://example.com/p")},
            {QByteArray("User-Agent"), QByteArray("Mozilla/5.0")},
        };
        QVERIFY(Persistence::writeSession(path, {rec}));
        const auto back = Persistence::readSession(path);
        QCOMPARE(back.size(), 1);
        QCOMPARE(back[0].extraHeaders, rec.extraHeaders);
    }

    void jsonObjectRoundTripAndTolerance() {
        QTemporaryDir dir;
        const QString path = dir.filePath("settings.json");
        // ausente -> objeto vazio
        QVERIFY(Persistence::readJsonObject(path).isEmpty());
        // round-trip
        QJsonObject o{{"a", 1}, {"b", QJsonObject{{"c", true}}}, {"keep", "x"}};
        QVERIFY(Persistence::writeJsonObject(path, o));
        const QJsonObject back = Persistence::readJsonObject(path);
        QCOMPARE(back.value("a").toInt(), 1);
        QCOMPARE(back.value("b").toObject().value("c").toBool(), true);
        QCOMPARE(back.value("keep").toString(), QString("x"));
        // corrompido -> objeto vazio (não crasha)
        QFile f(path); QVERIFY(f.open(QIODevice::WriteOnly)); f.write("{not json"); f.close();
        QVERIFY(Persistence::readJsonObject(path).isEmpty());
    }
};

QTEST_MAIN(TstPersistence)
#include "tst_persistence.moc"
