#pragma once
#include "DownloadTypes.h"
#include "ClipboardWatcher.h"   // ClipboardMode
#include <QString>
#include <QTime>
#include <QJsonObject>

enum class Recurrence { Once, Daily };

enum class ThemePref { System, Light, Dark };

struct BrowserPrefs {
    bool    enabled = false;
    quint16 port    = 8697;
    QString token;
};

struct UiPrefs {
    QString       defaultDownloadDir;                 // vazio => boot resolve p/ Downloads
    ClipboardMode clipboardMode = ClipboardMode::Off;
    ThemePref     theme         = ThemePref::System;
    bool          startAtLogin        = false;        // intent: launch hidden at login
    bool          closeToTrayHintShown = false;       // one-time "still running in tray" balloon
};

struct SchedulerConfig {
    bool       enabled      = false;
    QTime      start        = QTime(8, 0);
    QTime      stop         = QTime(18, 0);
    Recurrence recurrence   = Recurrence::Daily;
    bool       quitWhenDone = false;
    bool operator==(const SchedulerConfig&) const = default;
};

struct BitTorrentPrefs {
    int               maxPeersPerTorrent = 50;
    ResumeVerifyMode  verify             = ResumeVerifyMode::TrustBitfield;
    PieceStrategy     defaultStrategy    = PieceStrategy::RarestFirst;
    quint16           listenPort         = 6881;
    bool operator==(const BitTorrentPrefs&) const = default;   // != synthesized (C++20)
};

// Task 17: mainline DHT toggle + UDP port, gating DownloadManager's real,
// internet-bootstrapping DhtNode join (see DownloadManager::startDhtBootstrap()).
struct DhtSettings {
    bool    enabled = true;
    quint16 port    = 6881;
    bool operator==(const DhtSettings&) const = default;   // != synthesized (C++20)
};

struct AppSettings {
    EngineConfig     engine;
    UiPrefs          ui;
    SchedulerConfig  scheduler;
    BrowserPrefs     browser;
    BitTorrentPrefs  bittorrent;
    DhtSettings      dht;
};

namespace SettingsIo {
    AppSettings fromJson(const QJsonObject& root, const EngineConfig& defaults);
    QJsonObject toJson(const AppSettings& s, const QJsonObject& previousRoot);
    AppSettings load(const QString& path, const EngineConfig& defaults);
    void        save(const QString& path, const AppSettings& s);
}
