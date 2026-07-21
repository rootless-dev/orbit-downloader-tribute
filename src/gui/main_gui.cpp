#include <QApplication>
#include <QStandardPaths>
#include <QCryptographicHash>
#include <QIcon>
#include <QFileInfo>
#include "AutostartService.h"
#include "BootOptions.h"
#include "DownloadManager.h"
#include "DownloadTableModel.h"
#include "Logger.h"
#include "MainWindow.h"
#include "Settings.h"
#include "SingleInstanceGuard.h"

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationName("orbit-gui");
    QApplication::setQuitOnLastWindowClosed(false);   // live in the tray, not tied to the window

    // Marca janela + bandeja com o ícone do Orbit. No macOS o .icns vive em
    // Contents/Resources do bundle; fora dele, tenta o asset ao lado do binário.
    // Se nada carregar, a MainWindow ainda garante um ícone de bandeja não-nulo.
    for (const QString& cand : {
             QCoreApplication::applicationDirPath() + "/../Resources/Orbit.icns",
             QCoreApplication::applicationDirPath() + "/Orbit.icns"}) {
        if (QFileInfo::exists(cand)) {
            const QIcon appIcon(cand);
            if (!appIcon.isNull()) { QApplication::setWindowIcon(appIcon); break; }
        }
    }

    const bool hidden = shouldStartHidden(app.arguments());

    const QString dataDir =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/orbit-gui";

    // Single instance: a name namespaced by the data dir so parallel installs don't clash.
    const QString guardName = "orbit-gui-"
        + QString::fromLatin1(QCryptographicHash::hash(dataDir.toUtf8(),
                              QCryptographicHash::Sha1).toHex().left(12));
    SingleInstanceGuard guard(guardName);
    if (!guard.tryBecomePrimary(hidden ? kPingMessage : kShowMessage))
        return 0;   // another instance handles it (and shows itself if we were foreground)

    const QString settingsPath = dataDir + "/settings.json";
    AppSettings settings = SettingsIo::load(settingsPath, EngineConfig{});

    Logger logger(dataDir);
    DownloadManager mgr(settings.engine, dataDir, &logger);
    mgr.loadSession();                    // restore tasks BEFORE building the model
    DownloadTableModel model(&mgr);       // ctor seeds rows from mgr.tasks()

    auto autostart = AutostartService::create();
    // Reconcile persisted intent with on-disk reality (plist deleted out of band, etc.).
    if (settings.ui.startAtLogin != autostart->isEnabled())
        autostart->setEnabled(settings.ui.startAtLogin);

    MainWindow w(&mgr, &model, &logger);
    w.setAutostartService(autostart.get());
    w.applySettings(settings, settingsPath);
    w.setWindowTitle("Orbit Downloader Tribute");
    w.resize(960, 640);
    if (!hidden) w.show();                // autostart (--background) stays hidden in the tray

    QObject::connect(&guard, &SingleInstanceGuard::showRequested,
                     &w, &MainWindow::showAndRaise);
    return app.exec();
}
