#include "AutostartService.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QSaveFile>

namespace {
// No-op used on platforms without an autostart backend yet (Windows/Linux).
class NoopAutostartService : public AutostartService {
public:
    bool isEnabled() const override { return false; }
    bool setEnabled(bool) override { return false; }
};
}

#ifdef Q_OS_MACOS
#include <QProcess>

MacAutostartService::MacAutostartService(QString plistPath, QString execPath, bool useLaunchctl)
    : m_plistPath(std::move(plistPath)), m_execPath(std::move(execPath)),
      m_useLaunchctl(useLaunchctl) {}

bool MacAutostartService::isEnabled() const {
    return QFile::exists(m_plistPath);
}

QString MacAutostartService::buildPlistXml() const {
    return QStringLiteral(
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
        "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
        "<plist version=\"1.0\">\n"
        "<dict>\n"
        "    <key>Label</key>\n"
        "    <string>%1</string>\n"
        "    <key>ProgramArguments</key>\n"
        "    <array>\n"
        "        <string>%2</string>\n"
        "        <string>--background</string>\n"
        "    </array>\n"
        "    <key>RunAtLoad</key>\n"
        "    <true/>\n"
        "</dict>\n"
        "</plist>\n")
        .arg(QLatin1String(kAutostartLabel), m_execPath.toHtmlEscaped());
}

bool MacAutostartService::setEnabled(bool on) {
    if (on) {
        QDir().mkpath(QFileInfo(m_plistPath).absolutePath());
        QSaveFile f(m_plistPath);
        if (!f.open(QIODevice::WriteOnly)) return false;
        f.write(buildPlistXml().toUtf8());
        return f.commit();
        // Deliberately no `launchctl load`: RunAtLoad would spawn a --background
        // process immediately. The agent takes effect at next login.
    }
    // Disable: best-effort unload (only meaningful if it was loaded), then remove.
    if (m_useLaunchctl && QFile::exists(m_plistPath))
        QProcess::execute("launchctl", {"unload", "-w", m_plistPath});
    if (QFile::exists(m_plistPath))
        return QFile::remove(m_plistPath);
    return true;
}
#endif  // Q_OS_MACOS

std::unique_ptr<AutostartService> AutostartService::create() {
#ifdef Q_OS_MACOS
    const QString plist = QDir::homePath()
        + "/Library/LaunchAgents/" + QLatin1String(kAutostartLabel) + ".plist";
    return std::make_unique<MacAutostartService>(plist,
                                                 QCoreApplication::applicationFilePath());
#else
    return std::make_unique<NoopAutostartService>();
#endif
}
