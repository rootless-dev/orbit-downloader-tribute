#pragma once
#include "DownloadTypes.h"
#include "ClipboardWatcher.h"   // ClipboardMode
#include <QString>
#include <QTime>
#include <QJsonObject>

enum class Recurrence { Once, Daily };

struct BrowserPrefs {
    bool    enabled = false;
    quint16 port    = 8697;
    QString token;
};

struct UiPrefs {
    QString       defaultDownloadDir;                 // vazio => boot resolve p/ Downloads
    ClipboardMode clipboardMode = ClipboardMode::Off;
};

struct SchedulerConfig {
    bool       enabled      = false;
    QTime      start        = QTime(8, 0);
    QTime      stop         = QTime(18, 0);
    Recurrence recurrence   = Recurrence::Daily;
    bool       quitWhenDone = false;
};

struct AppSettings {
    EngineConfig    engine;
    UiPrefs         ui;
    SchedulerConfig scheduler;
    BrowserPrefs    browser;
};

namespace SettingsIo {
    AppSettings fromJson(const QJsonObject& root, const EngineConfig& defaults);
    QJsonObject toJson(const AppSettings& s, const QJsonObject& previousRoot);
    AppSettings load(const QString& path, const EngineConfig& defaults);
    void        save(const QString& path, const AppSettings& s);
}
