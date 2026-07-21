#include "Settings.h"
#include "EngineConfigJson.h"
#include "Persistence.h"

static QString clipToStr(ClipboardMode m) {
    switch (m) {
        case ClipboardMode::Ask:    return "Ask";
        case ClipboardMode::Auto:   return "Auto";
        case ClipboardMode::Notify: return "Notify";
        case ClipboardMode::Off:    default: return "Off";
    }
}
static ClipboardMode clipFromStr(const QString& s) {
    if (s == "Ask")    return ClipboardMode::Ask;
    if (s == "Auto")   return ClipboardMode::Auto;
    if (s == "Notify") return ClipboardMode::Notify;
    return ClipboardMode::Off;
}
static QString themeToStr(ThemePref t) {
    switch (t) {
        case ThemePref::Light: return "light";
        case ThemePref::Dark:  return "dark";
        case ThemePref::System: default: return "system";
    }
}
static ThemePref themeFromStr(const QString& s) {
    if (s == "light") return ThemePref::Light;
    if (s == "dark")  return ThemePref::Dark;
    return ThemePref::System;
}
static QTime timeOr(const QString& s, QTime def) {
    const QTime t = QTime::fromString(s, "HH:mm");
    return t.isValid() ? t : def;
}

namespace SettingsIo {

AppSettings fromJson(const QJsonObject& root, const EngineConfig& defaults) {
    AppSettings s;
    s.engine = engineConfigFromJson(root.value("engine").toObject(), defaults);

    const QJsonObject ui = root.value("ui").toObject();
    s.ui.defaultDownloadDir = ui.value("defaultDownloadDir").toString();
    s.ui.clipboardMode      = clipFromStr(ui.value("clipboardMode").toString());
    s.ui.theme              = themeFromStr(ui.value("theme").toString());
    s.ui.startAtLogin        = ui.value("startAtLogin").toBool(false);
    s.ui.closeToTrayHintShown = ui.value("closeToTrayHintShown").toBool(false);

    const QJsonObject sc = root.value("scheduler").toObject();
    s.scheduler.enabled      = sc.value("enabled").toBool(false);
    s.scheduler.start        = timeOr(sc.value("startTime").toString(), QTime(8, 0));
    s.scheduler.stop         = timeOr(sc.value("stopTime").toString(),  QTime(18, 0));
    s.scheduler.recurrence   = (sc.value("recurrence").toString() == "once")
                                   ? Recurrence::Once : Recurrence::Daily;
    s.scheduler.quitWhenDone = sc.value("quitWhenDone").toBool(false);

    const QJsonObject br = root.value("browser").toObject();
    s.browser.enabled = br.value("enabled").toBool(false);
    s.browser.port    = quint16(br.value("port").toInt(8697));
    s.browser.token   = br.value("token").toString();
    return s;
}

QJsonObject toJson(const AppSettings& s, const QJsonObject& prev) {
    QJsonObject root = prev;   // preserva chaves de topo desconhecidas
    root["version"]  = 1;
    root["engine"]   = engineConfigToJson(s.engine);
    root["ui"]       = QJsonObject{
        {"defaultDownloadDir", s.ui.defaultDownloadDir},
        {"clipboardMode",      clipToStr(s.ui.clipboardMode)},
        {"theme",              themeToStr(s.ui.theme)},
        {"startAtLogin",       s.ui.startAtLogin},
        {"closeToTrayHintShown", s.ui.closeToTrayHintShown}};
    root["scheduler"] = QJsonObject{
        {"enabled",      s.scheduler.enabled},
        {"startTime",    s.scheduler.start.toString("HH:mm")},
        {"stopTime",     s.scheduler.stop.toString("HH:mm")},
        {"recurrence",   s.scheduler.recurrence == Recurrence::Daily ? "daily" : "once"},
        {"quitWhenDone", s.scheduler.quitWhenDone}};
    root["browser"] = QJsonObject{
        {"enabled", s.browser.enabled},
        {"port",    int(s.browser.port)},
        {"token",   s.browser.token}};
    return root;
}

AppSettings load(const QString& path, const EngineConfig& defaults) {
    return fromJson(Persistence::readJsonObject(path), defaults);
}

void save(const QString& path, const AppSettings& s) {
    Persistence::writeJsonObject(path, toJson(s, Persistence::readJsonObject(path)));
}

} // namespace SettingsIo
