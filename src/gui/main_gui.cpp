#include <QApplication>
#include <QStandardPaths>
#include "DownloadManager.h"
#include "DownloadTableModel.h"
#include "Logger.h"
#include "MainWindow.h"
#include "Settings.h"

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationName("orbit-gui");

    const QString dataDir =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/orbit-gui";
    const QString settingsPath = dataDir + "/settings.json";
    const AppSettings settings = SettingsIo::load(settingsPath, EngineConfig{});

    Logger logger(dataDir);
    DownloadManager mgr(settings.engine, dataDir, &logger);
    mgr.loadSession();                    // restore tasks BEFORE building the model
    DownloadTableModel model(&mgr);       // ctor seeds rows from mgr.tasks()

    MainWindow w(&mgr, &model, &logger);
    w.applySettings(settings, settingsPath);
    w.setWindowTitle("Orbit Downloader Tribute");
    w.resize(960, 640);
    w.show();
    return app.exec();
}
