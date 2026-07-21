#pragma once
#include <QString>
#include <QtGlobal>
#include <memory>

inline constexpr char kAutostartLabel[] = "dev.rootless.orbit-downloader-tribute";

// Enables/disables "start the app at login". Ground truth lives on disk
// (a LaunchAgent plist on macOS), so isEnabled() reflects reality even if the
// file was changed out of band.
class AutostartService {
public:
    virtual ~AutostartService() = default;
    virtual bool isEnabled() const = 0;
    virtual bool setEnabled(bool on) = 0;
    static std::unique_ptr<AutostartService> create();   // platform factory
};

#ifdef Q_OS_MACOS
class MacAutostartService : public AutostartService {
public:
    // plistPath: ~/Library/LaunchAgents/<label>.plist in production.
    // execPath : absolute path to the running binary (bundle inner binary is fine).
    // useLaunchctl: false in tests to stay hermetic (no launchd side effects).
    MacAutostartService(QString plistPath, QString execPath, bool useLaunchctl = true);
    bool isEnabled() const override;
    bool setEnabled(bool on) override;
private:
    QString buildPlistXml() const;
    QString m_plistPath;
    QString m_execPath;
    bool    m_useLaunchctl;
};
#endif
