#include <QCoreApplication>
#include <QCommandLineParser>
#include <QStandardPaths>
#include <QTextStream>
#include "DownloadManager.h"

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName("orbit-cli");

    QCommandLineParser p;
    p.addPositionalArgument("url", "URL to download");
    p.addPositionalArgument("dest", "Destination file path");
    QCommandLineOption segs("segments", "Segments per download", "N", "4");
    QCommandLineOption maxc("max-concurrent", "Max concurrent downloads", "M", "3");
    p.addOption(segs); p.addOption(maxc);
    p.addHelpOption();
    p.process(app);

    const auto args = p.positionalArguments();
    QTextStream err(stderr);
    if (args.size() < 2) { err << "usage: orbit-cli <url> <dest>\n"; return 2; }

    EngineConfig cfg;
    cfg.segmentCount = p.value(segs).toInt();
    cfg.maxConcurrentDownloads = p.value(maxc).toInt();

    const QString dataDir =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/orbit-cli";
    DownloadManager mgr(cfg, dataDir);

    QTextStream out(stdout);
    QObject::connect(&mgr, &DownloadManager::taskProgress, &app,
        [&out](const QUuid&, qint64 r, qint64 t) {
            if (t > 0) out << QString("\r%1%  (%2/%3 bytes)")
                                 .arg(100.0 * r / t, 0, 'f', 1).arg(r).arg(t) << Qt::flush;
        });
    QObject::connect(&mgr, &DownloadManager::taskStateChanged, &app,
        [&out, &app](const QUuid&, DownloadState s) {
            if (s == DownloadState::Completed) { out << "\nDone.\n"; app.quit(); }
            else if (s == DownloadState::Error) { out << "\nError.\n"; QCoreApplication::exit(1); }
        });

    mgr.addDownload(QUrl(args[0]), args[1]);
    return app.exec();
}
