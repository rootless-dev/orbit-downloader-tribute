#include <QtTest>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTime>
#include "EngineConfigJson.h"
#include "Persistence.h"
#include "Settings.h"

class TstSettings : public QObject {
    Q_OBJECT
private slots:
    void engineConfigRoundTrip() {
        EngineConfig c;
        c.maxConcurrentDownloads = 7; c.segmentCount = 2; c.maxBytesPerSec = 500000;
        c.userAgent = "x/1";
        const EngineConfig back = engineConfigFromJson(engineConfigToJson(c), EngineConfig{});
        QCOMPARE(back.maxConcurrentDownloads, 7);
        QCOMPARE(back.segmentCount, 2);
        QCOMPARE(back.maxBytesPerSec, qint64(500000));
        QCOMPARE(back.userAgent, QString("x/1"));
    }
    void engineConfigMissingKeysFallBackToDefaults() {
        EngineConfig def; def.segmentCount = 9; def.userAgent = "def/9";
        const EngineConfig c = engineConfigFromJson(QJsonObject{{"maxConcurrentDownloads", 2}}, def);
        QCOMPARE(c.maxConcurrentDownloads, 2);   // presente
        QCOMPARE(c.segmentCount, 9);             // ausente -> default
        QCOMPARE(c.userAgent, QString("def/9")); // ausente -> default
    }
    void appSettingsRoundTripThroughFile() {
        QTemporaryDir dir;
        const QString path = dir.filePath("settings.json");
        AppSettings s;
        s.engine.segmentCount = 6;
        s.ui.defaultDownloadDir = "/tmp/dl";
        s.ui.clipboardMode = ClipboardMode::Auto;
        s.scheduler.enabled = true;
        s.scheduler.start = QTime(9, 30);
        s.scheduler.stop  = QTime(21, 0);
        s.scheduler.recurrence = Recurrence::Once;
        s.scheduler.quitWhenDone = true;
        SettingsIo::save(path, s);

        const AppSettings back = SettingsIo::load(path, EngineConfig{});
        QCOMPARE(back.engine.segmentCount, 6);
        QCOMPARE(back.ui.defaultDownloadDir, QString("/tmp/dl"));
        QVERIFY(back.ui.clipboardMode == ClipboardMode::Auto);
        QVERIFY(back.scheduler.enabled);
        QCOMPARE(back.scheduler.start, QTime(9, 30));
        QCOMPARE(back.scheduler.stop, QTime(21, 0));
        QVERIFY(back.scheduler.recurrence == Recurrence::Once);
        QVERIFY(back.scheduler.quitWhenDone);
    }
    void missingFileGivesDefaults() {
        const AppSettings s = SettingsIo::load("/no/such/settings.json", EngineConfig{});
        QCOMPARE(s.engine.segmentCount, EngineConfig{}.segmentCount);
        QVERIFY(s.ui.clipboardMode == ClipboardMode::Off);
        QVERIFY(!s.scheduler.enabled);
    }
    void unknownTopLevelKeysPreservedOnSave() {
        QTemporaryDir dir;
        const QString path = dir.filePath("settings.json");
        Persistence::writeJsonObject(path, QJsonObject{{"futureFeature", 42}});
        SettingsIo::save(path, SettingsIo::load(path, EngineConfig{}));
        QCOMPARE(Persistence::readJsonObject(path).value("futureFeature").toInt(), 42);
    }
    void clipboardModeStringMapping() {
        QTemporaryDir dir; const QString path = dir.filePath("s.json");
        AppSettings s; s.ui.clipboardMode = ClipboardMode::Notify;
        SettingsIo::save(path, s);
        QCOMPARE(Persistence::readJsonObject(path).value("ui").toObject()
                     .value("clipboardMode").toString(), QString("Notify"));
    }
    void browserBlockRoundTrips() {
        QJsonObject root;
        root["browser"] = QJsonObject{{"enabled", true}, {"port", 9000},
                                      {"token", "deadbeef"}};
        root["mystery"] = "keep-me";                 // chave desconhecida
        const AppSettings s = SettingsIo::fromJson(root, EngineConfig{});
        QCOMPARE(s.browser.enabled, true);
        QCOMPARE(s.browser.port, quint16(9000));
        QCOMPARE(s.browser.token, QString("deadbeef"));
        const QJsonObject out = SettingsIo::toJson(s, root);
        QCOMPARE(out.value("browser").toObject().value("port").toInt(), 9000);
        QCOMPARE(out.value("mystery").toString(), QString("keep-me"));   // preservada
    }
    void browserDefaultsAreTolerant() {
        const AppSettings s = SettingsIo::fromJson(QJsonObject{}, EngineConfig{});
        QCOMPARE(s.browser.enabled, false);
        QCOMPARE(s.browser.port, quint16(8697));
        QVERIFY(s.browser.token.isEmpty());
    }
};

QTEST_MAIN(TstSettings)
#include "tst_settings.moc"
