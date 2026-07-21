#include <QtTest>
#include <QTemporaryDir>
#include <QFile>
#include "AutostartService.h"

class TstAutostart : public QObject {
    Q_OBJECT
#ifdef Q_OS_MACOS
private slots:
    void disabledByDefault() {
        QTemporaryDir dir;
        const QString plist = dir.filePath("agent.plist");
        MacAutostartService s(plist, "/Applications/Orbit.app/Contents/MacOS/Orbit",
                              /*useLaunchctl=*/false);
        QVERIFY(!s.isEnabled());
    }
    void enableWritesPlistWithFlag() {
        QTemporaryDir dir;
        const QString plist = dir.filePath("agent.plist");
        const QString exec  = "/Applications/Orbit.app/Contents/MacOS/Orbit";
        MacAutostartService s(plist, exec, /*useLaunchctl=*/false);
        QVERIFY(s.setEnabled(true));
        QVERIFY(s.isEnabled());
        QFile f(plist);
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QString xml = QString::fromUtf8(f.readAll());
        QVERIFY(xml.contains("dev.rootless.orbit-downloader-tribute"));  // Label
        QVERIFY(xml.contains(exec));                                     // ProgramArguments[0]
        QVERIFY(xml.contains("--background"));                           // ProgramArguments[1]
        QVERIFY(xml.contains("RunAtLoad"));
    }
    void disableRemovesPlist() {
        QTemporaryDir dir;
        const QString plist = dir.filePath("agent.plist");
        MacAutostartService s(plist, "/x/Orbit", /*useLaunchctl=*/false);
        QVERIFY(s.setEnabled(true));
        QVERIFY(s.isEnabled());
        QVERIFY(s.setEnabled(false));
        QVERIFY(!s.isEnabled());
        QVERIFY(!QFile::exists(plist));
    }
#endif
    void factoryNeverNull() {
        QVERIFY(AutostartService::create() != nullptr);   // no-op off-macOS, Mac impl on-macOS
    }
};
QTEST_APPLESS_MAIN(TstAutostart)
#include "tst_autostart.moc"
