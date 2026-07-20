# Fase 4 — Settings + Preferences + Scheduler + Limite de banda — Implementation Plan

**Goal:** Persistir configuração em `settings.json`, adicionar o diálogo Preferences (General + Advanced), um limite de banda global (token bucket) e um Scheduler global (janela de horário + auto-quit).

**Architecture:** Config vive no Core (`EngineConfig` + serialização JSON headless); a GUI monta o `settings.json` completo (`engine`/`ui`/`scheduler`) por cima, via unidades puras testáveis (`SettingsIo`, `Scheduler`) e o `RateLimiter` no Core. Cada unidade tem uma responsabilidade e é testada isolada com tempo/caminho injetáveis, no padrão QtTest offline do projeto.

**Tech Stack:** C++20, Qt 6.11 (Core, Network, Widgets, Test), CMake, QtTest.

## Global Constraints

- **C++20**, Qt 6.11 via Homebrew (`/opt/homebrew`). Configurar: `cmake -S . -B build -DCMAKE_PREFIX_PATH=/opt/homebrew`. Compilar: `cmake --build build`.
- **`orbitcore` não depende de QtWidgets** — só `Qt6::Core` + `Qt6::Network`. Nada de tipos de GUI (`ClipboardMode`, `QTime` de scheduler, widgets) entra no Core.
- **Toda a rede no event loop principal**, I/O assíncrono. Sem threads/mutex.
- **Testes offline** (QtTest + servidores em processo). `QTEST_MAIN(...)` ao fim de cada suíte.
- **Gate de não-regressão:** `tst_download` (27 casos) roda **sem mudança de expectativa** ao fim de cada tarefa do Core. `RateLimiter` com teto `0` não altera tempos/bytes.
- **Regras de commit:** **sem** `Co-Authored-By`; mensagens em inglês, Conventional Commits (`feat:`/`fix:`/`refactor:`/`test:`).
- **Pré-requisito:** a feature de User-Agent (hoje no working tree: UA `curl/8.7.1` + `DownloadTask::error()` + Log com erro, testes `requestsCarryCurlUserAgent`/`probeErrorIsExposedOnTask`) deve estar **commitada** antes da Task 1. A Task 1 a promove para config.

---

## File Structure

**Core (`orbitcore`) — modificados/criados:**
- `src/core/DownloadTypes.h` — `EngineConfig` ganha `maxBytesPerSec` e `userAgent`.
- `src/core/Transport.h` — `createProbe` ganha `const EngineConfig&`; `createWorker` ganha `RateLimiter*`.
- `src/core/HttpProbe.{h,cpp}`, `HttpTransport.{h,cpp}`, `FtpProbe`/`FtpTransport.{h,cpp}` — assinaturas.
- `src/core/SegmentWorker.{h,cpp}`, `FtpSegmentWorker.{h,cpp}` — UA por config + throttle.
- `src/core/DownloadTask.{h,cpp}` — recebe/repassa `RateLimiter*`; passa `m_cfg` ao probe.
- `src/core/DownloadManager.{h,cpp}` — dono do `RateLimiter`; `setConfig()`.
- `src/core/HttpUserAgent.h` — **removido** (constante some).
- `src/core/Persistence.{h,cpp}` — `readJsonObject`/`writeJsonObject`.
- `src/core/EngineConfigJson.{h,cpp}` — **novo**: `engineConfigToJson`/`engineConfigFromJson`.
- `src/core/RateLimiter.{h,cpp}` — **novo**: token bucket, tempo injetável.

**GUI (`orbitgui_logic` puro / `orbitgui` widgets):**
- `src/gui/Settings.{h,cpp}` — **novo** (`orbitgui_logic`): `UiPrefs`, `SchedulerConfig`, `Recurrence`, `AppSettings`, `SettingsIo`.
- `src/gui/Scheduler.{h,cpp}` — **novo** (`orbitgui_logic`): `SchedAction`, `Scheduler`.
- `src/gui/PreferencesDialog.{h,cpp}` — **novo** (`orbitgui`).
- `src/gui/MainWindow.{h,cpp}` — ação Preferences, `applySettings`, quit-when-done, persistir modo clipboard/pasta.
- `src/gui/main_gui.cpp` — carregar settings no boot, `Scheduler` + `QTimer`.

**Tests:**
- `tests/tst_persistence.cpp` — estende (JSON object round-trip).
- `tests/tst_settings.cpp` — **novo** (link `orbitgui_logic`): `engineConfigJson` + `SettingsIo`.
- `tests/tst_ratelimiter.cpp` — **novo** (link `orbitcore`).
- `tests/tst_scheduler.cpp` — **novo** (link `orbitgui_logic`).
- `tests/tst_download.cpp` — estende (UA por config; setConfig live cap).
- `tests/tst_gui.cpp` — estende (PreferencesDialog `result()`; boot aplica settings).
- `tests/CMakeLists.txt` — registra `tst_settings`, `tst_ratelimiter`, `tst_scheduler`.

---

## Task 1: `EngineConfig` ganha `maxBytesPerSec` + `userAgent`; UA vira config

**Files:**
- Modify: `src/core/DownloadTypes.h:19-28`
- Modify: `src/core/Transport.h:52`
- Modify: `src/core/HttpProbe.h`, `src/core/HttpProbe.cpp`
- Modify: `src/core/HttpTransport.h`, `src/core/HttpTransport.cpp`
- Modify: `src/core/FtpTransport.h:7`, `src/core/FtpTransport.cpp:5`
- Modify: `src/core/SegmentWorker.cpp:38`
- Modify: `src/core/DownloadTask.cpp:32`
- Delete: `src/core/HttpUserAgent.h`
- Modify: `src/core/CMakeLists.txt` (remover `HttpUserAgent.h` se listado — não está, mas confira)
- Test: `tests/tst_download.cpp` (`requestsCarryCurlUserAgent`)

**Interfaces:**
- Produces: `EngineConfig.maxBytesPerSec` (qint64, default 0), `EngineConfig.userAgent` (QString, default `"curl/8.7.1"`); `Transport::createProbe(const EngineConfig&, QObject*)`.

- [ ] **Step 1: Editar o teste de UA para usar config**

Em `tests/tst_download.cpp`, substitua o corpo de `requestsCarryCurlUserAgent` para setar um UA custom e afirmá-lo:

```cpp
    void requestsCarryCurlUserAgent() {
        // O UA é configurável via EngineConfig; probe E segmentos usam o mesmo.
        TestServer srv(m_body);
        QVERIFY(srv.listen());
        QTemporaryDir dir;
        const QString dest = dir.filePath("ua.bin");
        QNetworkAccessManager nam;
        EngineConfig cfg; cfg.segmentCount = 4; cfg.minSegSize = 1;
        cfg.userAgent = "orbit-test/1.0";
        HttpTransport tr(&nam);
        DownloadTask task(&tr, cfg);
        task.init(QUuid::createUuid(), srv.url("/ranged"), dest, 4);
        task.start();
        QTRY_COMPARE_WITH_TIMEOUT(task.state(), DownloadState::Completed, 5000);
        QCOMPARE(srv.userAgentsSeen(), QStringList{"orbit-test/1.0"});
    }
```

- [ ] **Step 2: Rodar o teste e ver falhar**

Run: `cmake --build build && ctest --test-dir build -R tst_download --output-on-failure`
Expected: FAIL — `userAgentsSeen()` retorna `{"curl/8.7.1"}` (constante), não `{"orbit-test/1.0"}`.

- [ ] **Step 3: Adicionar os campos ao `EngineConfig`**

`src/core/DownloadTypes.h`, dentro de `struct EngineConfig`, após `progressThrottleMs`:

```cpp
    int    progressThrottleMs     = 200;
    qint64 maxBytesPerSec         = 0;              // 0 = ilimitado (teto GLOBAL)
    QString userAgent             = "curl/8.7.1";  // enviado em probe + segmentos HTTP
```

- [ ] **Step 4: Threadar o UA pelo probe e worker**

`src/core/Transport.h:52` — `createProbe` passa a receber a config:

```cpp
    virtual Probe*         createProbe(const EngineConfig& cfg, QObject* parent) = 0;
```

`src/core/HttpProbe.h` — construtor guarda o UA:

```cpp
    explicit HttpProbe(QNetworkAccessManager* nam, QByteArray userAgent, QObject* parent = nullptr);
private:
    // ...
    QByteArray             m_userAgent;
```

`src/core/HttpProbe.cpp` — remover `#include "HttpUserAgent.h"`, guardar UA e usá-lo:

```cpp
HttpProbe::HttpProbe(QNetworkAccessManager* nam, QByteArray userAgent, QObject* parent)
    : Probe(parent), m_nam(nam), m_userAgent(std::move(userAgent)) {
    qRegisterMetaType<ProbeResult>("ProbeResult");
}
```
e em `start()` trocar `req.setRawHeader("User-Agent", kHttpUserAgent);` por
`req.setRawHeader("User-Agent", m_userAgent);`.

`src/core/HttpTransport.h`/`.cpp` — `createProbe` repassa o UA:

```cpp
Probe* HttpTransport::createProbe(const EngineConfig& cfg, QObject* parent) {
    return new HttpProbe(m_nam, cfg.userAgent.toUtf8(), parent);
}
```
(atualize a declaração no `.h` para `Probe* createProbe(const EngineConfig& cfg, QObject* parent) override;`)

`src/core/FtpTransport.h`/`.cpp` — mesma assinatura nova; FTP ignora o UA:

```cpp
Probe* FtpTransport::createProbe(const EngineConfig& cfg, QObject* parent) {
    Q_UNUSED(cfg);   // FTP não tem cabeçalho User-Agent
    return new FtpProbe(/* args existentes */ parent);
}
```
(preserve os argumentos que o `FtpProbe` já recebia; só adicione o `const EngineConfig& cfg` à assinatura e o `Q_UNUSED`.)

`src/core/SegmentWorker.cpp:38` — remover `#include "HttpUserAgent.h"` e trocar
`req.setRawHeader("User-Agent", kHttpUserAgent);` por
`req.setRawHeader("User-Agent", m_cfg.userAgent.toUtf8());`.

`src/core/DownloadTask.cpp:32` — passar a config ao criar o probe:

```cpp
        Probe* probe = m_transport->createProbe(m_cfg, this);
```

Por fim, remova o arquivo `src/core/HttpUserAgent.h`.

- [ ] **Step 5: Rodar os testes e ver passar (+ gate)**

Run: `cmake --build build && ctest --test-dir build -R "tst_download|tst_ftp|tst_transport" --output-on-failure`
Expected: PASS. `tst_download` 27 casos (o de UA agora afirma `orbit-test/1.0`); `tst_ftp`/`tst_transport` intactos.

- [ ] **Step 6: Commit** (após autorização)

```bash
git add src/core tests/tst_download.cpp
git commit -m "refactor(core): make HTTP User-Agent configurable via EngineConfig"
```

---

## Task 2: `Persistence::readJsonObject` / `writeJsonObject`

**Files:**
- Modify: `src/core/Persistence.h:14-25`
- Modify: `src/core/Persistence.cpp`
- Test: `tests/tst_persistence.cpp`

**Interfaces:**
- Produces: `QJsonObject Persistence::readJsonObject(const QString& path)` (objeto vazio se ausente/corrompido/não-objeto); `bool Persistence::writeJsonObject(const QString& path, const QJsonObject& obj)` (escrita atômica, JSON indentado).

- [ ] **Step 1: Escrever o teste que falha**

Em `tests/tst_persistence.cpp`, adicionar um slot:

```cpp
    void jsonObjectRoundTripAndTolerance() {
        QTemporaryDir dir;
        const QString path = dir.filePath("settings.json");
        // ausente -> objeto vazio
        QVERIFY(Persistence::readJsonObject(path).isEmpty());
        // round-trip
        QJsonObject o{{"a", 1}, {"b", QJsonObject{{"c", true}}}, {"keep", "x"}};
        QVERIFY(Persistence::writeJsonObject(path, o));
        const QJsonObject back = Persistence::readJsonObject(path);
        QCOMPARE(back.value("a").toInt(), 1);
        QCOMPARE(back.value("b").toObject().value("c").toBool(), true);
        QCOMPARE(back.value("keep").toString(), QString("x"));
        // corrompido -> objeto vazio (não crasha)
        QFile f(path); QVERIFY(f.open(QIODevice::WriteOnly)); f.write("{not json"); f.close();
        QVERIFY(Persistence::readJsonObject(path).isEmpty());
    }
```

Adicione `#include <QJsonObject>` e `#include <QFile>` ao topo do arquivo se ainda não houver.

- [ ] **Step 2: Rodar e ver falhar**

Run: `cmake --build build 2>&1 | head -20`
Expected: FAIL de compilação — `readJsonObject`/`writeJsonObject` não declarados.

- [ ] **Step 3: Implementar**

`src/core/Persistence.h`, dentro do `namespace Persistence`, após `readSession`:

```cpp
    QJsonObject readJsonObject(const QString& path);
    bool        writeJsonObject(const QString& path, const QJsonObject& obj);
```
Adicione `#include <QJsonObject>` no topo do `.h`.

`src/core/Persistence.cpp`, dentro do namespace:

```cpp
QJsonObject readJsonObject(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    const auto doc = QJsonDocument::fromJson(f.readAll());
    return doc.isObject() ? doc.object() : QJsonObject{};
}

bool writeJsonObject(const QString& path, const QJsonObject& obj) {
    return writeFileAtomic(path, QJsonDocument(obj).toJson(QJsonDocument::Indented));
}
```

- [ ] **Step 4: Rodar e ver passar**

Run: `cmake --build build && ctest --test-dir build -R tst_persistence --output-on-failure`
Expected: PASS.

- [ ] **Step 5: Commit** (após autorização)

```bash
git add src/core/Persistence.h src/core/Persistence.cpp tests/tst_persistence.cpp
git commit -m "feat(core): add generic JSON object read/write to Persistence"
```

---

## Task 3: `engineConfigToJson` / `engineConfigFromJson`

**Files:**
- Create: `src/core/EngineConfigJson.h`, `src/core/EngineConfigJson.cpp`
- Modify: `src/core/CMakeLists.txt` (adicionar `EngineConfigJson.cpp`)
- Create: `tests/tst_settings.cpp`
- Modify: `tests/CMakeLists.txt` (registrar `tst_settings`)

**Interfaces:**
- Consumes: `EngineConfig` (Task 1).
- Produces: `QJsonObject engineConfigToJson(const EngineConfig&)`; `EngineConfig engineConfigFromJson(const QJsonObject&, const EngineConfig& defaults)` (cada chave ausente/tipo errado cai para `defaults`).

- [ ] **Step 1: Criar a suíte `tst_settings` com o teste que falha**

`tests/tst_settings.cpp`:

```cpp
#include <QtTest>
#include <QJsonObject>
#include "EngineConfigJson.h"

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
};

QTEST_MAIN(TstSettings)
#include "tst_settings.moc"
```

Registrar em `tests/CMakeLists.txt` (após `tst_ftp`):

```cmake
add_executable(tst_settings tst_settings.cpp)
target_link_libraries(tst_settings PRIVATE orbitgui_logic Qt6::Test)
add_test(NAME tst_settings COMMAND tst_settings)
```
(link `orbitgui_logic` porque as Tasks 4+ adicionam testes de `SettingsIo` a esta mesma suíte; `orbitgui_logic` já expõe o `orbitcore` publicamente.)

- [ ] **Step 2: Rodar e ver falhar**

Run: `cmake -S . -B build -DCMAKE_PREFIX_PATH=/opt/homebrew && cmake --build build 2>&1 | head -20`
Expected: FAIL de compilação — `EngineConfigJson.h` inexistente.

- [ ] **Step 3: Implementar**

`src/core/EngineConfigJson.h`:

```cpp
#pragma once
#include "DownloadTypes.h"
#include <QJsonObject>

QJsonObject  engineConfigToJson(const EngineConfig& c);
EngineConfig engineConfigFromJson(const QJsonObject& o, const EngineConfig& defaults);
```

`src/core/EngineConfigJson.cpp`:

```cpp
#include "EngineConfigJson.h"

static int getInt(const QJsonObject& o, const char* k, int def) {
    const auto v = o.value(QLatin1String(k));
    return v.isDouble() ? v.toInt(def) : def;
}
static qint64 getI64(const QJsonObject& o, const char* k, qint64 def) {
    const auto v = o.value(QLatin1String(k));
    return v.isDouble() ? qint64(v.toDouble(double(def))) : def;
}
static QString getStr(const QJsonObject& o, const char* k, const QString& def) {
    const auto v = o.value(QLatin1String(k));
    return v.isString() ? v.toString() : def;
}

QJsonObject engineConfigToJson(const EngineConfig& c) {
    return QJsonObject{
        {"maxConcurrentDownloads", c.maxConcurrentDownloads},
        {"segmentCount",           c.segmentCount},
        {"minSegSize",             double(c.minSegSize)},
        {"maxSegmentRetries",      c.maxSegmentRetries},
        {"retryBackoffBaseMs",     c.retryBackoffBaseMs},
        {"connectTimeoutMs",       c.connectTimeoutMs},
        {"idleTimeoutMs",          c.idleTimeoutMs},
        {"progressThrottleMs",     c.progressThrottleMs},
        {"maxBytesPerSec",         double(c.maxBytesPerSec)},
        {"userAgent",              c.userAgent}};
}

EngineConfig engineConfigFromJson(const QJsonObject& o, const EngineConfig& d) {
    EngineConfig c;
    c.maxConcurrentDownloads = getInt(o, "maxConcurrentDownloads", d.maxConcurrentDownloads);
    c.segmentCount           = getInt(o, "segmentCount",           d.segmentCount);
    c.minSegSize             = getI64(o, "minSegSize",             d.minSegSize);
    c.maxSegmentRetries      = getInt(o, "maxSegmentRetries",      d.maxSegmentRetries);
    c.retryBackoffBaseMs     = getInt(o, "retryBackoffBaseMs",     d.retryBackoffBaseMs);
    c.connectTimeoutMs       = getInt(o, "connectTimeoutMs",       d.connectTimeoutMs);
    c.idleTimeoutMs          = getInt(o, "idleTimeoutMs",          d.idleTimeoutMs);
    c.progressThrottleMs     = getInt(o, "progressThrottleMs",     d.progressThrottleMs);
    c.maxBytesPerSec         = getI64(o, "maxBytesPerSec",         d.maxBytesPerSec);
    c.userAgent              = getStr(o, "userAgent",              d.userAgent);
    return c;
}
```

Adicionar `EngineConfigJson.cpp` à lista de fontes do `orbitcore` em `src/core/CMakeLists.txt`.

- [ ] **Step 4: Rodar e ver passar**

Run: `cmake -S . -B build -DCMAKE_PREFIX_PATH=/opt/homebrew && cmake --build build && ctest --test-dir build -R tst_settings --output-on-failure`
Expected: PASS (2 casos).

- [ ] **Step 5: Commit** (após autorização)

```bash
git add src/core/EngineConfigJson.h src/core/EngineConfigJson.cpp src/core/CMakeLists.txt tests/tst_settings.cpp tests/CMakeLists.txt
git commit -m "feat(core): serialize EngineConfig to/from JSON with default fallback"
```

---

## Task 4: `SettingsIo` — `settings.json` completo (engine/ui/scheduler)

**Files:**
- Create: `src/gui/Settings.h`, `src/gui/Settings.cpp`
- Modify: `src/gui/CMakeLists.txt` (adicionar `Settings.cpp` a `orbitgui_logic`)
- Test: `tests/tst_settings.cpp`

**Interfaces:**
- Consumes: `engineConfigFromJson/ToJson` (Task 3), `Persistence::readJsonObject/writeJsonObject` (Task 2), `ClipboardMode` (de `ClipboardWatcher.h`).
- Produces: `Recurrence` (Once/Daily), `UiPrefs`, `SchedulerConfig`, `AppSettings`; `SettingsIo::fromJson/toJson/load/save`.

- [ ] **Step 1: Escrever os testes que falham**

Adicionar a `tests/tst_settings.cpp` os includes `#include "Settings.h"`, `#include <QTemporaryDir>`, `#include <QTime>` e os slots:

```cpp
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
```

Adicionar `#include "Persistence.h"` ao arquivo de teste.

- [ ] **Step 2: Rodar e ver falhar**

Run: `cmake --build build 2>&1 | head -20`
Expected: FAIL de compilação — `Settings.h` inexistente.

- [ ] **Step 3: Implementar**

`src/gui/Settings.h`:

```cpp
#pragma once
#include "DownloadTypes.h"
#include "ClipboardWatcher.h"   // ClipboardMode
#include <QString>
#include <QTime>
#include <QJsonObject>

enum class Recurrence { Once, Daily };

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
};

namespace SettingsIo {
    AppSettings fromJson(const QJsonObject& root, const EngineConfig& defaults);
    QJsonObject toJson(const AppSettings& s, const QJsonObject& previousRoot);
    AppSettings load(const QString& path, const EngineConfig& defaults);
    void        save(const QString& path, const AppSettings& s);
}
```

`src/gui/Settings.cpp`:

```cpp
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

    const QJsonObject sc = root.value("scheduler").toObject();
    s.scheduler.enabled      = sc.value("enabled").toBool(false);
    s.scheduler.start        = timeOr(sc.value("startTime").toString(), QTime(8, 0));
    s.scheduler.stop         = timeOr(sc.value("stopTime").toString(),  QTime(18, 0));
    s.scheduler.recurrence   = (sc.value("recurrence").toString() == "once")
                                   ? Recurrence::Once : Recurrence::Daily;
    s.scheduler.quitWhenDone = sc.value("quitWhenDone").toBool(false);
    return s;
}

QJsonObject toJson(const AppSettings& s, const QJsonObject& prev) {
    QJsonObject root = prev;   // preserva chaves de topo desconhecidas
    root["version"]  = 1;
    root["engine"]   = engineConfigToJson(s.engine);
    root["ui"]       = QJsonObject{
        {"defaultDownloadDir", s.ui.defaultDownloadDir},
        {"clipboardMode",      clipToStr(s.ui.clipboardMode)}};
    root["scheduler"] = QJsonObject{
        {"enabled",      s.scheduler.enabled},
        {"startTime",    s.scheduler.start.toString("HH:mm")},
        {"stopTime",     s.scheduler.stop.toString("HH:mm")},
        {"recurrence",   s.scheduler.recurrence == Recurrence::Daily ? "daily" : "once"},
        {"quitWhenDone", s.scheduler.quitWhenDone}};
    return root;
}

AppSettings load(const QString& path, const EngineConfig& defaults) {
    return fromJson(Persistence::readJsonObject(path), defaults);
}

void save(const QString& path, const AppSettings& s) {
    Persistence::writeJsonObject(path, toJson(s, Persistence::readJsonObject(path)));
}

} // namespace SettingsIo
```

Adicionar `Settings.cpp` à lista de fontes de `orbitgui_logic` em `src/gui/CMakeLists.txt`.

- [ ] **Step 4: Rodar e ver passar**

Run: `cmake -S . -B build -DCMAKE_PREFIX_PATH=/opt/homebrew && cmake --build build && ctest --test-dir build -R tst_settings --output-on-failure`
Expected: PASS (6 casos).

- [ ] **Step 5: Commit** (após autorização)

```bash
git add src/gui/Settings.h src/gui/Settings.cpp src/gui/CMakeLists.txt tests/tst_settings.cpp
git commit -m "feat(gui): add SettingsIo for settings.json (engine/ui/scheduler)"
```

---

## Task 5: `RateLimiter` (token bucket, tempo injetável)

**Files:**
- Create: `src/core/RateLimiter.h`, `src/core/RateLimiter.cpp`
- Modify: `src/core/CMakeLists.txt`
- Create: `tests/tst_ratelimiter.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Produces: `RateLimiter::setRate(qint64 bytesPerSec)` (0 = ilimitado); `qint64 RateLimiter::take(qint64 want, qint64 nowMs)` (0..want; ilimitado devolve `want`).

- [ ] **Step 1: Escrever os testes que falham**

`tests/tst_ratelimiter.cpp`:

```cpp
#include <QtTest>
#include "RateLimiter.h"

class TstRateLimiter : public QObject {
    Q_OBJECT
private slots:
    void unlimitedGrantsEverything() {
        RateLimiter r;                 // taxa default 0
        QCOMPARE(r.take(1'000'000, 0), qint64(1'000'000));
        QCOMPARE(r.take(1'000'000, 0), qint64(1'000'000));
    }
    void cappedByBurstThenRefills() {
        RateLimiter r; r.setRate(1000); // 1000 B/s, burst = 1000
        QCOMPARE(r.take(5000, 0), qint64(1000));  // prime cheio -> concede 1000
        QCOMPARE(r.take(5000, 0), qint64(0));     // sem tokens no mesmo instante
        QCOMPARE(r.take(5000, 1000), qint64(1000));// +1s -> +1000 (cap no burst)
    }
    void partialRefillProportionalToElapsed() {
        RateLimiter r; r.setRate(1000);
        QCOMPARE(r.take(5000, 0), qint64(1000));   // esvazia
        QCOMPARE(r.take(5000, 500), qint64(500));  // 0,5s -> 500 tokens
    }
    void grantsNoMoreThanRequested() {
        RateLimiter r; r.setRate(1000);
        QCOMPARE(r.take(300, 0), qint64(300));     // pediu menos que o bucket
    }
    void settingRateZeroAfterCapReturnsUnlimited() {
        RateLimiter r; r.setRate(1000); r.take(1000, 0);
        r.setRate(0);
        QCOMPARE(r.take(9999, 0), qint64(9999));
    }
};

QTEST_MAIN(TstRateLimiter)
#include "tst_ratelimiter.moc"
```

Registrar em `tests/CMakeLists.txt`:

```cmake
add_executable(tst_ratelimiter tst_ratelimiter.cpp)
target_link_libraries(tst_ratelimiter PRIVATE orbitcore Qt6::Test)
add_test(NAME tst_ratelimiter COMMAND tst_ratelimiter)
```

- [ ] **Step 2: Rodar e ver falhar**

Run: `cmake -S . -B build -DCMAKE_PREFIX_PATH=/opt/homebrew && cmake --build build 2>&1 | head -20`
Expected: FAIL de compilação — `RateLimiter.h` inexistente.

- [ ] **Step 3: Implementar**

`src/core/RateLimiter.h`:

```cpp
#pragma once
#include <QtGlobal>

// Token bucket global. Tempo injetável (nowMs) para teste determinístico, no
// mesmo espírito do SpeedSampler. take() nunca bloqueia: devolve o quanto pode
// conceder agora (0..want). Taxa 0 = ilimitado (bypass, custo zero).
class RateLimiter {
public:
    void   setRate(qint64 bytesPerSec);          // 0 = ilimitado
    qint64 take(qint64 want, qint64 nowMs);
private:
    qint64 m_ratePerSec = 0;
    double m_tokens      = 0.0;
    double m_burst       = 0.0;   // capacidade = 1s de taxa
    qint64 m_lastMs      = -1;
};
```

`src/core/RateLimiter.cpp`:

```cpp
#include "RateLimiter.h"

void RateLimiter::setRate(qint64 bytesPerSec) {
    m_ratePerSec = bytesPerSec > 0 ? bytesPerSec : 0;
    m_burst      = double(m_ratePerSec);           // burst = 1s
    if (m_tokens > m_burst) m_tokens = m_burst;
}

qint64 RateLimiter::take(qint64 want, qint64 nowMs) {
    if (m_ratePerSec <= 0) return want;            // ilimitado
    if (want <= 0) return 0;
    if (m_lastMs < 0) { m_lastMs = nowMs; m_tokens = m_burst; }  // prime cheio
    const qint64 elapsed = nowMs - m_lastMs;
    if (elapsed > 0) {
        m_tokens = qMin(m_burst, m_tokens + double(m_ratePerSec) * double(elapsed) / 1000.0);
        m_lastMs = nowMs;
    }
    qint64 grant = qint64(qMin(m_tokens, double(want)));
    if (grant < 0) grant = 0;
    m_tokens -= double(grant);
    return grant;
}
```

Adicionar `RateLimiter.cpp` a `orbitcore` em `src/core/CMakeLists.txt`.

- [ ] **Step 4: Rodar e ver passar**

Run: `cmake -S . -B build -DCMAKE_PREFIX_PATH=/opt/homebrew && cmake --build build && ctest --test-dir build -R tst_ratelimiter --output-on-failure`
Expected: PASS (5 casos).

- [ ] **Step 5: Commit** (após autorização)

```bash
git add src/core/RateLimiter.h src/core/RateLimiter.cpp src/core/CMakeLists.txt tests/tst_ratelimiter.cpp tests/CMakeLists.txt
git commit -m "feat(core): add token-bucket RateLimiter with injectable time"
```

---

## Task 6: `DownloadManager` dono do `RateLimiter` + `setConfig()` + threading do limiter

**Files:**
- Modify: `src/core/Transport.h:53` (createWorker ganha `RateLimiter*`)
- Modify: `src/core/HttpTransport.{h,cpp}`, `src/core/FtpTransport.{h,cpp}`
- Modify: `src/core/SegmentWorker.{h,cpp}`, `src/core/FtpSegmentWorker.{h,cpp}` (ctor guarda `RateLimiter*`, sem usar ainda)
- Modify: `src/core/DownloadTask.{h,cpp}` (ctor recebe `RateLimiter*`; repassa a `createWorker`)
- Modify: `src/core/DownloadManager.{h,cpp}` (membro `RateLimiter`; `setConfig`)
- Test: `tests/tst_download.cpp`

**Interfaces:**
- Consumes: `RateLimiter` (Task 5).
- Produces: `Transport::createWorker(QFile*, const EngineConfig&, RateLimiter*, QObject*)`; `DownloadTask(Transport*, const EngineConfig&, RateLimiter*, QObject*)`; `DownloadManager::setConfig(const EngineConfig&)` (banda + cap ao vivo; resto p/ próximos downloads).

- [ ] **Step 1: Escrever o teste que falha (cap ao vivo)**

Em `tests/tst_download.cpp`, adicionar um slot que sobe o cap em runtime e vê um `Queued` promover. Use a rota lenta existente do `TestServer` (se não houver, use `/ranged` com corpo grande e `segmentCount=1`):

```cpp
    void setConfigRaisesConcurrencyCapLive() {
        TestServer srv(m_body);
        QVERIFY(srv.listen());
        QTemporaryDir dir;
        EngineConfig cfg; cfg.maxConcurrentDownloads = 1; cfg.segmentCount = 1; cfg.minSegSize = 1;
        DownloadManager mgr(cfg, dir.path());
        const QUuid a = mgr.addDownload(srv.url("/ranged"), dir.filePath("a.bin"));
        const QUuid b = mgr.addDownload(srv.url("/ranged"), dir.filePath("b.bin"));
        Q_UNUSED(a);
        // com cap=1, 'b' fica Queued (ou completa muito depois); subir o cap deve
        // permitir ambos concluírem.
        EngineConfig hi = cfg; hi.maxConcurrentDownloads = 4;
        mgr.setConfig(hi);
        DownloadTask* tb = mgr.taskById(b);
        QVERIFY(tb);
        QTRY_COMPARE_WITH_TIMEOUT(tb->state(), DownloadState::Completed, 5000);
    }
```

- [ ] **Step 2: Rodar e ver falhar**

Run: `cmake --build build 2>&1 | head -20`
Expected: FAIL de compilação — `setConfig` não declarado.

- [ ] **Step 3: Threadar o `RateLimiter*` e implementar `setConfig`**

`src/core/Transport.h:53`:

```cpp
    virtual SegmentSource* createWorker(QFile* file, const EngineConfig& cfg,
                                        RateLimiter* limiter, QObject* parent) = 0;
```
Adicionar `class RateLimiter;` como forward-declaration no topo do `Transport.h`.

`src/core/HttpTransport.{h,cpp}` e `src/core/FtpTransport.{h,cpp}`: atualizar a assinatura de `createWorker` para incluir `RateLimiter* limiter` e repassá-lo ao construtor do worker:

```cpp
// HttpTransport.cpp
SegmentSource* HttpTransport::createWorker(QFile* file, const EngineConfig& cfg,
                                           RateLimiter* limiter, QObject* parent) {
    return new SegmentWorker(m_nam, file, cfg, limiter, parent);
}
```
```cpp
// FtpTransport.cpp
SegmentSource* FtpTransport::createWorker(QFile* file, const EngineConfig& cfg,
                                          RateLimiter* limiter, QObject* parent) {
    return new FtpSegmentWorker(file, cfg, limiter, parent);
}
```
(inclua `#include "RateLimiter.h"` onde necessário; atualize os dois `.h`.)

`src/core/SegmentWorker.h` — construtor e membro:

```cpp
    SegmentWorker(QNetworkAccessManager* nam, QFile* file, const EngineConfig& cfg,
                  RateLimiter* limiter, QObject* parent = nullptr);
    // ...
    RateLimiter* m_limiter = nullptr;
```
Adicionar `class RateLimiter;` ao topo. `src/core/SegmentWorker.cpp` — guardar (ainda sem usar):

```cpp
SegmentWorker::SegmentWorker(QNetworkAccessManager* nam, QFile* file,
                             const EngineConfig& cfg, RateLimiter* limiter, QObject* parent)
    : SegmentSource(parent), m_nam(nam), m_file(file), m_cfg(cfg), m_limiter(limiter) {}
```

`src/core/FtpSegmentWorker.h`/`.cpp` — igual: ctor ganha `RateLimiter* limiter`, membro `RateLimiter* m_limiter = nullptr;`, guardado no ctor (sem uso ainda).

`src/core/DownloadTask.h` — ctor e membro:

```cpp
    DownloadTask(Transport* transport, const EngineConfig& cfg,
                 RateLimiter* limiter, QObject* parent = nullptr);
    // ...
    RateLimiter* m_limiter;
```
Adicionar `class RateLimiter;` ao topo. `src/core/DownloadTask.cpp`:

```cpp
DownloadTask::DownloadTask(Transport* transport, const EngineConfig& cfg,
                           RateLimiter* limiter, QObject* parent)
    : QObject(parent), m_transport(transport), m_cfg(cfg), m_limiter(limiter) {}
```
e na criação do worker (linha ~121):

```cpp
    SegmentSource* w = m_transport->createWorker(m_file, m_cfg, m_limiter, this);
```

> **Compatibilidade dos testes existentes:** `tst_download`/`tst_transport`/`tst_ftp` que constroem `DownloadTask(&tr, cfg)` passam a `DownloadTask(&tr, cfg, nullptr)`. Como `parent` tem default, adote `DownloadTask(Transport*, const EngineConfig&, RateLimiter* = nullptr, QObject* = nullptr)` — assim as chamadas existentes de 2 argumentos continuam compilando e passam `limiter = nullptr` (worker roda ilimitado). Faça o mesmo default (`RateLimiter* limiter = nullptr`) nos ctors de `SegmentWorker`/`FtpSegmentWorker` **apenas se** houver testes que os instanciam diretamente; caso contrário mantenha sem default.

`src/core/DownloadManager.h` — membro e método:

```cpp
    void  setConfig(const EngineConfig& cfg);
    // ...
private:
    // ... após m_cfg:
    RateLimiter m_limiter;   // teto global de banda, consultado pelos workers
```
Adicionar `#include "RateLimiter.h"`.

`src/core/DownloadManager.cpp`:
- No construtor, inicializar a taxa: após a lista de init, no corpo, `m_limiter.setRate(cfg.maxBytesPerSec);`.
- Ao criar cada `DownloadTask` (linhas ~49 e ~208), passar `&m_limiter`:
  `auto* t = new DownloadTask(tr, m_cfg, &m_limiter, this);`
- Implementar `setConfig`:

```cpp
void DownloadManager::setConfig(const EngineConfig& cfg) {
    m_cfg = cfg;                             // vale para PRÓXIMOS downloads
    m_limiter.setRate(cfg.maxBytesPerSec);   // banda: ao vivo
    pump();                                  // cap de concorrência: ao vivo
}
```

- [ ] **Step 4: Rodar o teste novo e o gate**

Run: `cmake -S . -B build -DCMAKE_PREFIX_PATH=/opt/homebrew && cmake --build build && ctest --test-dir build -R "tst_download|tst_transport|tst_ftp" --output-on-failure`
Expected: PASS. `tst_download` agora com 28 casos (o gate original de 27 permanece **sem mudança de expectativa**; o novo caso é aditivo).

- [ ] **Step 5: Commit** (após autorização)

```bash
git add src/core
git commit -m "feat(core): DownloadManager owns global RateLimiter and setConfig() (live cap/rate)"
```

---

## Task 7: Throttle do worker HTTP via `RateLimiter`

**Files:**
- Modify: `src/core/SegmentWorker.h` (declarar `scheduleDrain`, `QTimer* m_drainTimer`)
- Modify: `src/core/SegmentWorker.cpp` (`onReadyRead` consulta o limiter)
- Test: verificação de não-corrupção com teto baixo (`tests/tst_download.cpp`)

**Interfaces:**
- Consumes: `RateLimiter::take` (Task 5), `m_limiter` (Task 6).

- [ ] **Step 1: Escrever o teste que falha (download com teto baixo completa byte-idêntico)**

Em `tests/tst_download.cpp`:

```cpp
    void throttledDownloadStillCompletesIntact() {
        TestServer srv(m_body);
        QVERIFY(srv.listen());
        QTemporaryDir dir;
        const QString dest = dir.filePath("cap.bin");
        EngineConfig cfg; cfg.segmentCount = 4; cfg.minSegSize = 1;
        cfg.maxBytesPerSec = 4096;               // teto baixo, mas download deve terminar
        DownloadManager mgr(cfg, dir.path());
        const QUuid id = mgr.addDownload(srv.url("/ranged"), dest);
        DownloadTask* t = mgr.taskById(id);
        QVERIFY(t);
        QTRY_COMPARE_WITH_TIMEOUT(t->state(), DownloadState::Completed, 15000);
        QFile f(dest); QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(f.readAll(), m_body);           // conteúdo íntegro sob throttle
    }
```

- [ ] **Step 2: Rodar e ver falhar/passar-por-acidente**

Run: `cmake --build build && ctest --test-dir build -R tst_download --output-on-failure`
Expected: sem o throttle, o teto é ignorado e o teste PASSA trivialmente (não prova throttle) — ele existe como **gate de não-corrupção** para o Step 3. Prossiga ao Step 3; o valor do teste é garantir que a leitura parcial + drain não corrompe o arquivo.

- [ ] **Step 3: Implementar o throttle em `onReadyRead`**

`src/core/SegmentWorker.h` — declarar no `private:`:

```cpp
    void    scheduleDrain();
    QTimer* m_drainTimer = nullptr;
```

`src/core/SegmentWorker.cpp` — adicionar `#include "RateLimiter.h"`, `#include <QDateTime>` e reescrever `onReadyRead` (linhas 58-78) para ler só o concedido:

```cpp
void SegmentWorker::onReadyRead() {
    if (m_stopped || !m_reply) return;
    armIdleTimer(m_cfg.idleTimeoutMs);
    const int status = m_reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (m_expectPartial && status == 200) {   // servidor ignorou If-Range
        m_reply->abort();
        emit restartRequired(m_seg.index);
        return;
    }
    const qint64 avail = m_reply->bytesAvailable();
    if (avail <= 0) return;
    qint64 grant = avail;
    if (m_limiter) {
        grant = m_limiter->take(avail, QDateTime::currentMSecsSinceEpoch());
        if (grant <= 0) { scheduleDrain(); return; }   // sem tokens: tentar de novo em breve
    }
    const QByteArray chunk = m_reply->read(grant);
    if (chunk.isEmpty()) return;
    m_file->seek(m_seg.current);
    const qint64 written = m_file->write(chunk);
    if (written < 0) {
        m_reply->abort();
        emit failed(m_seg.index, "write error: " + m_file->errorString(), FailureKind::Fatal);
        return;
    }
    m_seg.current += written;
    emit progressed(m_seg.index, m_seg.current);
    if (m_reply->bytesAvailable() > 0) scheduleDrain();  // restou dado sob throttle
}

void SegmentWorker::scheduleDrain() {
    if (m_stopped) return;
    if (!m_drainTimer) {
        m_drainTimer = new QTimer(this);
        m_drainTimer->setSingleShot(true);
        connect(m_drainTimer, &QTimer::timeout, this, &SegmentWorker::onReadyRead);
    }
    if (!m_drainTimer->isActive()) m_drainTimer->start(20);   // ~20ms até repor tokens
}
```

Em `stop()` (onde `m_retryTimer`/`m_idleTimer` são parados), pare também o drain:
`if (m_drainTimer) m_drainTimer->stop();`

> **Nota:** com `m_limiter == nullptr` (testes diretos) ou `maxBytesPerSec == 0`, `take` devolve `avail` e o caminho é idêntico ao anterior (lê tudo de uma vez), preservando o comportamento dos 27 casos do gate.

- [ ] **Step 4: Rodar o gate + o novo teste**

Run: `cmake --build build && ctest --test-dir build -R "tst_download|tst_transport" --output-on-failure`
Expected: PASS. Gate de 27 casos sem mudança; `throttledDownloadStillCompletesIntact` verde (arquivo íntegro).

- [ ] **Step 5: Commit** (após autorização)

```bash
git add src/core/SegmentWorker.h src/core/SegmentWorker.cpp tests/tst_download.cpp
git commit -m "feat(core): throttle HTTP segment reads through the global RateLimiter"
```

---

## Task 8: Throttle do worker FTP via `RateLimiter`

**Files:**
- Modify: `src/core/FtpSegmentWorker.h` (declarar `scheduleDrain`, `QTimer* m_drainTimer`)
- Modify: `src/core/FtpSegmentWorker.cpp` (`onDataReadyRead` consulta o limiter)
- Test: `tests/tst_ftp.cpp` (download FTP com teto baixo completa íntegro)

**Interfaces:**
- Consumes: `RateLimiter::take`, `m_limiter` (Task 6).

- [ ] **Step 1: Escrever o teste que falha (gate de não-corrupção FTP)**

Em `tests/tst_ftp.cpp`, espelhando um teste de download FTP existente mas com `cfg.maxBytesPerSec` baixo, afirmando o corpo íntegro ao fim. Use o mesmo `TestFtpServer` e o mesmo padrão `QTRY_COMPARE_WITH_TIMEOUT(..., DownloadState::Completed, ...)` já usado na suíte; após completar, abra o destino e `QCOMPARE(f.readAll(), <corpo esperado>)`. (Copie a estrutura de um teste de sucesso já presente no arquivo, trocando só `cfg.maxBytesPerSec` para `4096` e adicionando a verificação de conteúdo.)

- [ ] **Step 2: Rodar e observar**

Run: `cmake --build build && ctest --test-dir build -R tst_ftp --output-on-failure`
Expected: passa trivialmente sem throttle (teto ignorado) — serve de gate para o Step 3, garantindo que a leitura parcial + drain não corrompe o arquivo FTP.

- [ ] **Step 3: Implementar o throttle em `onDataReadyRead`**

`src/core/FtpSegmentWorker.h` — no `private:`:

```cpp
    void    scheduleDrain();
    QTimer* m_drainTimer = nullptr;
```

`src/core/FtpSegmentWorker.cpp` — adicionar `#include "RateLimiter.h"`, `#include <QDateTime>` e reescrever `onDataReadyRead` (a partir da linha ~130) para ler só o concedido:

```cpp
void FtpSegmentWorker::onDataReadyRead() {
    if (m_stopped || m_finished || !m_data) return;
    armIdleTimer(m_cfg.idleTimeoutMs);
    const qint64 avail = m_data->bytesAvailable();
    if (avail <= 0) return;
    qint64 grant = avail;
    if (m_limiter) {
        grant = m_limiter->take(avail, QDateTime::currentMSecsSinceEpoch());
        if (grant <= 0) { scheduleDrain(); return; }
    }
    // corte no 'end' do segmento (servidor manda do REST até o fim): não passar do limite
    if (m_seg.end >= 0) {
        const qint64 remaining = m_seg.end - m_seg.current + 1;
        if (remaining <= 0) { finishSegment(); return; }
        grant = qMin(grant, remaining);
    }
    QByteArray chunk = m_data->read(grant);
    if (chunk.isEmpty()) return;
    m_file->seek(m_seg.current);
    const qint64 written = m_file->write(chunk);
    if (written < 0) {
        emit failed(m_seg.index, "write error: " + m_file->errorString(), FailureKind::Fatal);
        return;
    }
    m_seg.current += written;
    emit progressed(m_seg.index, m_seg.current);
    if (m_seg.end >= 0 && m_seg.current > m_seg.end) { finishSegment(); return; }
    if (m_data->bytesAvailable() > 0) scheduleDrain();
}

void FtpSegmentWorker::scheduleDrain() {
    if (m_stopped || m_finished) return;
    if (!m_drainTimer) {
        m_drainTimer = new QTimer(this);
        m_drainTimer->setSingleShot(true);
        connect(m_drainTimer, &QTimer::timeout, this, &FtpSegmentWorker::onDataReadyRead);
    }
    if (!m_drainTimer->isActive()) m_drainTimer->start(20);
}
```

> **IMPORTANTE — preserve a lógica de corte atual:** o `onDataReadyRead` existente já contém a regra de corte no `end` e a finalização do segmento (linhas ~130-158). O snippet acima **reproduz** essa regra com leitura limitada; ao aplicar, confira contra o corte atual do arquivo e mantenha exatamente a mesma condição de fim (`finishSegment()`), só trocando `readAll()` por `read(grant)` e o cálculo do `grant`. Em `teardown()`/`stop()`, pare o `m_drainTimer` (`if (m_drainTimer) m_drainTimer->stop();`).

- [ ] **Step 4: Rodar o gate FTP + ASAN**

Run: `cmake --build build && ctest --test-dir build -R "tst_ftp|tst_transport" --output-on-failure`
Expected: PASS, corpo íntegro sob throttle. (Se houver build ASAN configurado, rode-o em `tst_ftp` como as fases anteriores fizeram.)

- [ ] **Step 5: Commit** (após autorização)

```bash
git add src/core/FtpSegmentWorker.h src/core/FtpSegmentWorker.cpp tests/tst_ftp.cpp
git commit -m "feat(core): throttle FTP segment reads through the global RateLimiter"
```

---

## Task 9: `Scheduler` (janela de horário, recorrência, tempo injetável)

**Files:**
- Create: `src/gui/Scheduler.h`, `src/gui/Scheduler.cpp`
- Modify: `src/gui/CMakeLists.txt` (adicionar `Scheduler.cpp` a `orbitgui_logic`)
- Create: `tests/tst_scheduler.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `SchedulerConfig`, `Recurrence` (Task 4, em `Settings.h`).
- Produces: `SchedAction` (None/StartAll/StopAll); `Scheduler::setConfig(const SchedulerConfig&)`; `SchedAction Scheduler::tick(const QDateTime& now)` (edge-trigger, sem repique no mesmo dia/janela).

- [ ] **Step 1: Escrever os testes que falham**

`tests/tst_scheduler.cpp`:

```cpp
#include <QtTest>
#include "Scheduler.h"

static QDateTime at(int y,int mo,int d,int h,int mi) {
    return QDateTime(QDate(y,mo,d), QTime(h,mi));
}

class TstScheduler : public QObject {
    Q_OBJECT
private slots:
    void disabledNeverFires() {
        Scheduler s; SchedulerConfig c; c.enabled = false; s.setConfig(c);
        QVERIFY(s.tick(at(2026,7,18,8,0)) == SchedAction::None);
    }
    void dailyFiresStartThenStop() {
        Scheduler s; SchedulerConfig c;
        c.enabled = true; c.start = QTime(8,0); c.stop = QTime(18,0);
        c.recurrence = Recurrence::Daily; s.setConfig(c);
        QVERIFY(s.tick(at(2026,7,18,7,59)) == SchedAction::None);
        QVERIFY(s.tick(at(2026,7,18,8,0))  == SchedAction::StartAll);
        QVERIFY(s.tick(at(2026,7,18,9,0))  == SchedAction::None);   // sem repique na janela
        QVERIFY(s.tick(at(2026,7,18,18,0)) == SchedAction::StopAll);
        QVERIFY(s.tick(at(2026,7,18,19,0)) == SchedAction::None);
        QVERIFY(s.tick(at(2026,7,19,8,0))  == SchedAction::StartAll); // rearma no dia seguinte
    }
    void launchMidWindowStartsImmediately() {
        Scheduler s; SchedulerConfig c;
        c.enabled = true; c.start = QTime(8,0); c.stop = QTime(18,0); s.setConfig(c);
        QVERIFY(s.tick(at(2026,7,18,10,0)) == SchedAction::StartAll);
    }
    void launchAfterWindowDoesNotFire() {
        Scheduler s; SchedulerConfig c;
        c.enabled = true; c.start = QTime(8,0); c.stop = QTime(18,0); s.setConfig(c);
        QVERIFY(s.tick(at(2026,7,18,20,0)) == SchedAction::None);   // nem Start nem Stop
    }
    void onceDisarmsAfterStop() {
        Scheduler s; SchedulerConfig c;
        c.enabled = true; c.start = QTime(8,0); c.stop = QTime(18,0);
        c.recurrence = Recurrence::Once; s.setConfig(c);
        QVERIFY(s.tick(at(2026,7,18,8,0))  == SchedAction::StartAll);
        QVERIFY(s.tick(at(2026,7,18,18,0)) == SchedAction::StopAll);
        QVERIFY(s.tick(at(2026,7,19,8,0))  == SchedAction::None);   // não rearma
    }
};

QTEST_MAIN(TstScheduler)
#include "tst_scheduler.moc"
```

Registrar em `tests/CMakeLists.txt`:

```cmake
add_executable(tst_scheduler tst_scheduler.cpp)
target_link_libraries(tst_scheduler PRIVATE orbitgui_logic Qt6::Test)
add_test(NAME tst_scheduler COMMAND tst_scheduler)
```

- [ ] **Step 2: Rodar e ver falhar**

Run: `cmake -S . -B build -DCMAKE_PREFIX_PATH=/opt/homebrew && cmake --build build 2>&1 | head -20`
Expected: FAIL de compilação — `Scheduler.h` inexistente.

- [ ] **Step 3: Implementar**

`src/gui/Scheduler.h`:

```cpp
#pragma once
#include "Settings.h"   // SchedulerConfig, Recurrence
#include <QDateTime>

enum class SchedAction { None, StartAll, StopAll };

// Puro: decide a ação devida por tick, com tempo injetado. Edge-trigger — dispara
// StartAll ao entrar na janela e StopAll após o fim, uma vez por dia (Daily) ou
// uma única vez (Once).
class Scheduler {
public:
    void        setConfig(const SchedulerConfig& c) { m_cfg = c; }
    SchedAction tick(const QDateTime& now);
private:
    SchedulerConfig m_cfg;
    QDate           m_lastStartDate;
    QDate           m_lastStopDate;
};
```

`src/gui/Scheduler.cpp`:

```cpp
#include "Scheduler.h"

SchedAction Scheduler::tick(const QDateTime& now) {
    if (!m_cfg.enabled) return SchedAction::None;
    const QDate today = now.date();
    const QTime t     = now.time();
    const bool inWindow = (t >= m_cfg.start && t < m_cfg.stop);

    if (inWindow && m_lastStartDate != today) {
        m_lastStartDate = today;
        return SchedAction::StartAll;
    }
    if (!inWindow && t >= m_cfg.stop
        && m_lastStartDate == today && m_lastStopDate != today) {
        m_lastStopDate = today;
        if (m_cfg.recurrence == Recurrence::Once) m_cfg.enabled = false;
        return SchedAction::StopAll;
    }
    return SchedAction::None;
}
```

Adicionar `Scheduler.cpp` a `orbitgui_logic` em `src/gui/CMakeLists.txt`.

- [ ] **Step 4: Rodar e ver passar**

Run: `cmake -S . -B build -DCMAKE_PREFIX_PATH=/opt/homebrew && cmake --build build && ctest --test-dir build -R tst_scheduler --output-on-failure`
Expected: PASS (5 casos).

- [ ] **Step 5: Commit** (após autorização)

```bash
git add src/gui/Scheduler.h src/gui/Scheduler.cpp src/gui/CMakeLists.txt tests/tst_scheduler.cpp tests/CMakeLists.txt
git commit -m "feat(gui): add pure Scheduler (window/recurrence, injectable time)"
```

---

## Task 10: `PreferencesDialog` (abas General + Advanced)

**Files:**
- Create: `src/gui/PreferencesDialog.h`, `src/gui/PreferencesDialog.cpp`
- Modify: `src/gui/CMakeLists.txt` (adicionar `PreferencesDialog.cpp` a `orbitgui`)
- Test: `tests/tst_gui.cpp`

**Interfaces:**
- Consumes: `AppSettings`, `UiPrefs` (Task 4).
- Produces: `PreferencesDialog(const AppSettings& current, QWidget* parent)`; `AppSettings PreferencesDialog::result() const`.

- [ ] **Step 1: Escrever o teste que falha**

Em `tests/tst_gui.cpp`, adicionar `#include "PreferencesDialog.h"` e um slot que constrói o diálogo, mexe nos widgets e lê `result()`:

```cpp
    void preferences_result_reflects_widgets() {
        AppSettings in;
        in.engine.maxConcurrentDownloads = 3;
        in.engine.userAgent = "curl/8.7.1";
        PreferencesDialog dlg(in);
        dlg.setConcurrentForTest(5);
        dlg.setMaxKBpsForTest(250);              // 250 KB/s
        dlg.setUserAgentCustomForTest("orbit/9");
        const AppSettings out = dlg.result();
        QCOMPARE(out.engine.maxConcurrentDownloads, 5);
        QCOMPARE(out.engine.maxBytesPerSec, qint64(250 * 1024));
        QCOMPARE(out.engine.userAgent, QString("orbit/9"));
    }
```

- [ ] **Step 2: Rodar e ver falhar**

Run: `cmake --build build 2>&1 | head -20`
Expected: FAIL de compilação — `PreferencesDialog.h` inexistente.

- [ ] **Step 3: Implementar**

`src/gui/PreferencesDialog.h`:

```cpp
#pragma once
#include "Settings.h"
#include <QDialog>
class QSpinBox;
class QLineEdit;
class QComboBox;

class PreferencesDialog : public QDialog {
    Q_OBJECT
public:
    explicit PreferencesDialog(const AppSettings& current, QWidget* parent = nullptr);
    AppSettings result() const;

    // Hooks de teste (sem depender de exec()):
    void setConcurrentForTest(int n);
    void setMaxKBpsForTest(int kbps);
    void setUserAgentCustomForTest(const QString& ua);

private:
    AppSettings m_base;                 // preserva campos não editados (timeouts avançados etc.)
    // General
    QSpinBox*  m_concurrent = nullptr;
    QSpinBox*  m_segments   = nullptr;
    QSpinBox*  m_maxKBps    = nullptr;  // 0 = ilimitado
    QLineEdit* m_dir        = nullptr;
    QComboBox* m_clipMode   = nullptr;
    QComboBox* m_uaPreset   = nullptr;
    QLineEdit* m_uaCustom   = nullptr;
    // Advanced
    QSpinBox*  m_connectMs  = nullptr;
    QSpinBox*  m_idleMs     = nullptr;
    QSpinBox*  m_retries    = nullptr;
    QSpinBox*  m_backoffMs  = nullptr;
    QSpinBox*  m_minSegKB   = nullptr;
    QSpinBox*  m_throttleMs = nullptr;
};
```

`src/gui/PreferencesDialog.cpp`:

```cpp
#include "PreferencesDialog.h"
#include <QTabWidget>
#include <QFormLayout>
#include <QVBoxLayout>
#include <QDialogButtonBox>
#include <QSpinBox>
#include <QLineEdit>
#include <QComboBox>
#include <QLabel>
#include <QPushButton>
#include <QFileDialog>

static const char* kChromeUA =
    "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/126.0.0.0 Safari/537.36";
static const char* kCurlUA = "curl/8.7.1";

PreferencesDialog::PreferencesDialog(const AppSettings& current, QWidget* parent)
    : QDialog(parent), m_base(current) {
    setWindowTitle(tr("Preferences"));
    auto* tabs = new QTabWidget(this);

    // ---- General ----
    auto* gen = new QWidget(this);
    auto* gf  = new QFormLayout(gen);
    m_concurrent = new QSpinBox(gen); m_concurrent->setRange(1, 32);
    m_concurrent->setValue(current.engine.maxConcurrentDownloads);
    m_segments = new QSpinBox(gen); m_segments->setRange(1, 32);
    m_segments->setValue(current.engine.segmentCount);
    m_maxKBps = new QSpinBox(gen); m_maxKBps->setRange(0, 1'000'000);
    m_maxKBps->setSuffix(" KB/s"); m_maxKBps->setSpecialValueText(tr("Unlimited"));
    m_maxKBps->setValue(int(current.engine.maxBytesPerSec / 1024));
    auto* dirRow = new QWidget(gen); auto* dl = new QHBoxLayout(dirRow);
    dl->setContentsMargins(0,0,0,0);
    m_dir = new QLineEdit(current.ui.defaultDownloadDir, dirRow);
    auto* browse = new QPushButton(tr("Browse…"), dirRow);
    dl->addWidget(m_dir); dl->addWidget(browse);
    connect(browse, &QPushButton::clicked, this, [this]{
        const QString d = QFileDialog::getExistingDirectory(this, tr("Default download folder"), m_dir->text());
        if (!d.isEmpty()) m_dir->setText(d);
    });
    m_clipMode = new QComboBox(gen);
    m_clipMode->addItem("Off",    int(ClipboardMode::Off));
    m_clipMode->addItem("Ask",    int(ClipboardMode::Ask));
    m_clipMode->addItem("Auto",   int(ClipboardMode::Auto));
    m_clipMode->addItem("Notify", int(ClipboardMode::Notify));
    m_clipMode->setCurrentIndex(m_clipMode->findData(int(current.ui.clipboardMode)));
    m_uaPreset = new QComboBox(gen);
    m_uaPreset->addItem(tr("curl (curl/8.7.1)"), kCurlUA);
    m_uaPreset->addItem(tr("Chrome"),            kChromeUA);
    m_uaPreset->addItem(tr("Custom…"),           QString());
    m_uaCustom = new QLineEdit(gen);
    // seleciona preset conforme o valor atual; senão, Custom
    if (current.engine.userAgent == kCurlUA)        m_uaPreset->setCurrentIndex(0);
    else if (current.engine.userAgent == kChromeUA) m_uaPreset->setCurrentIndex(1);
    else { m_uaPreset->setCurrentIndex(2); m_uaCustom->setText(current.engine.userAgent); }
    auto syncUa = [this]{ m_uaCustom->setEnabled(m_uaPreset->currentData().toString().isEmpty()); };
    connect(m_uaPreset, &QComboBox::currentIndexChanged, this, [syncUa](int){ syncUa(); });
    syncUa();

    gf->addRow(tr("Simultaneous downloads:"), m_concurrent);
    gf->addRow(tr("Segments per download:"),  m_segments);
    gf->addRow(tr("Max speed:"),              m_maxKBps);
    gf->addRow(tr("Default folder:"),         dirRow);
    gf->addRow(tr("Clipboard monitor:"),      m_clipMode);
    gf->addRow(tr("User-Agent:"),             m_uaPreset);
    gf->addRow(QString(),                     m_uaCustom);
    tabs->addTab(gen, tr("General"));

    // ---- Advanced ----
    auto* adv = new QWidget(this);
    auto* af  = new QFormLayout(adv);
    auto mkSpin = [adv](int lo, int hi, int val){ auto* s=new QSpinBox(adv); s->setRange(lo,hi); s->setValue(val); return s; };
    m_connectMs  = mkSpin(1000, 600000, current.engine.connectTimeoutMs);
    m_idleMs     = mkSpin(1000, 600000, current.engine.idleTimeoutMs);
    m_retries    = mkSpin(0, 100,       current.engine.maxSegmentRetries);
    m_backoffMs  = mkSpin(0, 60000,     current.engine.retryBackoffBaseMs);
    m_minSegKB   = mkSpin(1, 1'000'000, int(current.engine.minSegSize / 1024));
    m_throttleMs = mkSpin(0, 5000,      current.engine.progressThrottleMs);
    af->addRow(tr("Connect timeout (ms):"), m_connectMs);
    af->addRow(tr("Idle timeout (ms):"),    m_idleMs);
    af->addRow(tr("Max segment retries:"),  m_retries);
    af->addRow(tr("Retry backoff base (ms):"), m_backoffMs);
    af->addRow(tr("Min segment size (KB):"), m_minSegKB);
    af->addRow(tr("Progress throttle (ms):"), m_throttleMs);
    tabs->addTab(adv, tr("Advanced"));

    auto* note = new QLabel(
        tr("Speed and simultaneous-download limits apply immediately; other changes "
           "apply to new downloads."), this);
    note->setWordWrap(true);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* lay = new QVBoxLayout(this);
    lay->addWidget(tabs);
    lay->addWidget(note);
    lay->addWidget(buttons);
}

AppSettings PreferencesDialog::result() const {
    AppSettings s = m_base;   // mantém o que não é editável aqui
    s.engine.maxConcurrentDownloads = m_concurrent->value();
    s.engine.segmentCount           = m_segments->value();
    s.engine.maxBytesPerSec         = qint64(m_maxKBps->value()) * 1024;
    s.engine.connectTimeoutMs       = m_connectMs->value();
    s.engine.idleTimeoutMs          = m_idleMs->value();
    s.engine.maxSegmentRetries      = m_retries->value();
    s.engine.retryBackoffBaseMs     = m_backoffMs->value();
    s.engine.minSegSize             = qint64(m_minSegKB->value()) * 1024;
    s.engine.progressThrottleMs     = m_throttleMs->value();
    const QString preset = m_uaPreset->currentData().toString();
    s.engine.userAgent = preset.isEmpty() ? m_uaCustom->text() : preset;
    s.ui.defaultDownloadDir = m_dir->text();
    s.ui.clipboardMode = ClipboardMode(m_clipMode->currentData().toInt());
    return s;
}

void PreferencesDialog::setConcurrentForTest(int n)              { m_concurrent->setValue(n); }
void PreferencesDialog::setMaxKBpsForTest(int kbps)             { m_maxKBps->setValue(kbps); }
void PreferencesDialog::setUserAgentCustomForTest(const QString& ua) {
    m_uaPreset->setCurrentIndex(2);   // Custom…
    m_uaCustom->setText(ua);
}
```

Adicionar `#include <QHBoxLayout>` ao `.cpp`. Registrar `PreferencesDialog.cpp` em `orbitgui` no `src/gui/CMakeLists.txt`.

- [ ] **Step 4: Rodar e ver passar**

Run: `cmake -S . -B build -DCMAKE_PREFIX_PATH=/opt/homebrew && cmake --build build && ctest --test-dir build -R tst_gui --output-on-failure`
Expected: PASS (o novo caso + todos os anteriores).

- [ ] **Step 5: Commit** (após autorização)

```bash
git add src/gui/PreferencesDialog.h src/gui/PreferencesDialog.cpp src/gui/CMakeLists.txt tests/tst_gui.cpp
git commit -m "feat(gui): add PreferencesDialog (General + Advanced)"
```

---

## Task 11: Fiar Preferences no `MainWindow` (ação + aplicar + persistir)

**Files:**
- Modify: `src/gui/MainWindow.h` (membros `m_settings`, `m_settingsPath`, `m_clipGroup`; slot `onPreferences`; método `applySettings`)
- Modify: `src/gui/MainWindow.cpp` (ação toolbar; `onPreferences`; `applySettings`; usar `defaultDir` das settings)
- Test: `tests/tst_gui.cpp`

**Interfaces:**
- Consumes: `PreferencesDialog` (Task 10), `DownloadManager::setConfig` (Task 6), `SettingsIo::save` (Task 4).
- Produces: `MainWindow::applySettings(const AppSettings&, const QString& settingsPath)`.

- [ ] **Step 1: Escrever o teste que falha**

Em `tests/tst_gui.cpp`, adicionar um slot que aplica settings à janela e verifica que a pasta padrão passou a vir delas:

```cpp
    void mainwindow_applysettings_sets_default_dir_and_clip_mode() {
        QTemporaryDir dir;
        EngineConfig cfg; DownloadManager mgr(cfg, dir.path());
        DownloadTableModel model(&mgr);
        MainWindow w(&mgr, &model);
        AppSettings s;
        s.ui.defaultDownloadDir = dir.path();
        s.ui.clipboardMode = ClipboardMode::Auto;
        w.applySettings(s, dir.filePath("settings.json"));
        QCOMPARE(w.defaultDirForTest(), dir.path());
        QVERIFY(w.clipModeForTest() == ClipboardMode::Auto);
    }
```

(Adicione ao `MainWindow.h`/`.cpp` os hooks de teste `QString defaultDirForTest() const { return defaultDir(); }` e `ClipboardMode clipModeForTest() const { return m_clip->mode(); }`.)

- [ ] **Step 2: Rodar e ver falhar**

Run: `cmake --build build 2>&1 | head -20`
Expected: FAIL de compilação — `applySettings`/hooks inexistentes.

- [ ] **Step 3: Implementar**

`src/gui/MainWindow.h` — adicionar includes/forward e membros/slots:

```cpp
#include "Settings.h"       // AppSettings
class QActionGroup;
// ...
public:
    void applySettings(const AppSettings& s, const QString& settingsPath);
    QString      defaultDirForTest() const { return defaultDir(); }
    ClipboardMode clipModeForTest() const;
private slots:
    void onPreferences();
private:
    // ... após m_lastDir:
    AppSettings  m_settings;
    QString      m_settingsPath;
    QActionGroup* m_clipGroup = nullptr;   // p/ refletir o modo persistido no menu
```

`src/gui/MainWindow.cpp`:
- No ctor, após criar `aResumeAll`, adicionar a ação Preferences:

```cpp
    auto* aPrefs = tb->addAction("Preferences");
    connect(aPrefs, &QAction::triggered, this, &MainWindow::onPreferences);
```
- No bloco do menu Tools → Clipboard monitor (linhas ~128-145), **guardar** o `QActionGroup` em `m_clipGroup` (onde hoje é uma variável local `group`, atribua também `m_clipGroup = group;`) e ponha `a->setData(int(m));` em cada ação do rádio, para `applySettings` poder marcar a correta.
- Trocar `defaultDir()` para preferir a pasta das settings quando ainda não houve escolha nesta sessão:

```cpp
QString MainWindow::defaultDir() const {
    if (!m_lastDir.isEmpty()) return m_lastDir;
    if (!m_settings.ui.defaultDownloadDir.isEmpty()) return m_settings.ui.defaultDownloadDir;
    // ... fallback existente (QStandardPaths Downloads) permanece abaixo ...
}
```
- Adicionar os métodos:

```cpp
ClipboardMode MainWindow::clipModeForTest() const { return m_clip->mode(); }

void MainWindow::applySettings(const AppSettings& s, const QString& settingsPath) {
    m_settings     = s;
    m_settingsPath = settingsPath;
    m_clip->setMode(s.ui.clipboardMode);
    if (m_clipGroup) {
        for (QAction* a : m_clipGroup->actions())
            if (a->data().toInt() == int(s.ui.clipboardMode)) { a->setChecked(true); break; }
    }
    if (!s.ui.defaultDownloadDir.isEmpty()) m_lastDir = s.ui.defaultDownloadDir;
}

void MainWindow::onPreferences() {
    PreferencesDialog dlg(m_settings, this);
    if (dlg.exec() != QDialog::Accepted) return;
    m_settings = dlg.result();
    m_mgr->setConfig(m_settings.engine);
    m_clip->setMode(m_settings.ui.clipboardMode);
    if (m_clipGroup) {
        for (QAction* a : m_clipGroup->actions())
            if (a->data().toInt() == int(m_settings.ui.clipboardMode)) { a->setChecked(true); break; }
    }
    if (!m_settings.ui.defaultDownloadDir.isEmpty()) m_lastDir = m_settings.ui.defaultDownloadDir;
    if (!m_settingsPath.isEmpty()) SettingsIo::save(m_settingsPath, m_settings);
}
```
Adicionar os includes `#include "PreferencesDialog.h"` e `#include <QActionGroup>` ao `.cpp`.

- [ ] **Step 4: Rodar e ver passar**

Run: `cmake --build build && ctest --test-dir build -R tst_gui --output-on-failure`
Expected: PASS.

- [ ] **Step 5: Commit** (após autorização)

```bash
git add src/gui/MainWindow.h src/gui/MainWindow.cpp tests/tst_gui.cpp
git commit -m "feat(gui): wire Preferences action, applySettings and live setConfig into MainWindow"
```

---

## Task 12: Boot — carregar settings, `Scheduler` + `QTimer`, quit-when-done

**Files:**
- Modify: `src/gui/main_gui.cpp` (load settings antes do manager; `applySettings`)
- Modify: `src/gui/MainWindow.h` (membros `Scheduler`, `QTimer*`; slot de tick; guarda quit)
- Modify: `src/gui/MainWindow.cpp` (iniciar timer em `applySettings`; roteia ações; quit-when-done)
- Test: `tests/tst_gui.cpp`

**Interfaces:**
- Consumes: `SettingsIo::load` (Task 4), `Scheduler` (Task 9), `DownloadManager::pauseAll/resumeAll`.

- [ ] **Step 1: Escrever o teste que falha (roteamento de ação do scheduler)**

Como o `tick` real usa relógio de parede, teste o **roteamento** expondo um gancho que aplica uma `SchedAction` diretamente:

```cpp
    void mainwindow_scheduler_action_routes_to_manager() {
        QTemporaryDir dir;
        EngineConfig cfg; DownloadManager mgr(cfg, dir.path());
        DownloadTableModel model(&mgr);
        MainWindow w(&mgr, &model);
        TestServer srv(m_body); QVERIFY(srv.listen());
        const QUuid id = mgr.addDownload(srv.url("/ranged"), dir.filePath("s.bin"));
        mgr.pause(id);
        QTRY_COMPARE_WITH_TIMEOUT(mgr.taskById(id)->state(), DownloadState::Paused, 5000);
        w.applySchedActionForTest(SchedAction::StartAll);   // roteia p/ resumeAll()
        QTRY_VERIFY_WITH_TIMEOUT(mgr.taskById(id)->state() != DownloadState::Paused, 5000);
    }
```
(`m_body`/`TestServer` já disponíveis na suíte `tst_gui`.)

- [ ] **Step 2: Rodar e ver falhar**

Run: `cmake --build build 2>&1 | head -20`
Expected: FAIL de compilação — `applySchedActionForTest`/`Scheduler` inexistentes no MainWindow.

- [ ] **Step 3: Implementar**

`src/gui/MainWindow.h` — includes e membros:

```cpp
#include "Scheduler.h"
class QTimer;
// ...
public:
    void applySchedActionForTest(SchedAction a) { routeSchedAction(a); }
private:
    void routeSchedAction(SchedAction a);
    void maybeQuitWhenDone();
    Scheduler m_scheduler;
    QTimer*   m_schedTimer = nullptr;
```

`src/gui/MainWindow.cpp`:
- Em `applySettings`, após configurar clipboard/dir, configurar o scheduler e (re)iniciar o timer:

```cpp
    m_scheduler.setConfig(s.scheduler);
    if (!m_schedTimer) {
        m_schedTimer = new QTimer(this);
        connect(m_schedTimer, &QTimer::timeout, this, [this]{
            routeSchedAction(m_scheduler.tick(QDateTime::currentDateTime()));
        });
    }
    m_schedTimer->start(30'000);   // tick a cada 30s
```
- Implementar o roteamento e o quit:

```cpp
void MainWindow::routeSchedAction(SchedAction a) {
    if (a == SchedAction::StartAll) m_mgr->resumeAll();
    else if (a == SchedAction::StopAll) m_mgr->pauseAll();
}

void MainWindow::maybeQuitWhenDone() {
    if (!m_settings.scheduler.quitWhenDone) return;
    const auto tasks = m_mgr->tasks();
    if (tasks.isEmpty()) return;                       // não fecha app vazio
    for (DownloadTask* t : tasks)
        if (t->state() != DownloadState::Completed) return;
    qApp->quit();
}
```
- Chamar `maybeQuitWhenDone()` ao fim de `onStateChanged(...)` (após o append no Log). Adicionar `#include <QApplication>`, `#include <QTimer>`, `#include <QDateTime>` ao `.cpp`.

`src/gui/main_gui.cpp` — carregar settings antes do manager e aplicá-las:

```cpp
int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationName("orbit-gui");

    const QString dataDir =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/orbit-gui";
    const QString settingsPath = dataDir + "/settings.json";
    const AppSettings settings = SettingsIo::load(settingsPath, EngineConfig{});

    DownloadManager mgr(settings.engine, dataDir);
    mgr.loadSession();
    DownloadTableModel model(&mgr);

    MainWindow w(&mgr, &model);
    w.applySettings(settings, settingsPath);
    w.setWindowTitle("Orbit Downloader Tribute");
    w.resize(960, 640);
    w.show();
    return app.exec();
}
```
Adicionar `#include "Settings.h"` ao `main_gui.cpp`.

- [ ] **Step 4: Rodar e ver passar (+ suíte GUI e gate)**

Run: `cmake --build build && ctest --test-dir build -R "tst_gui|tst_download" --output-on-failure`
Expected: PASS. `orbit-gui` sobe headless sem crash:
`QT_QPA_PLATFORM=offscreen ./build/src/gui/orbit-gui &` seguido de `sleep 1 && kill %1` — sem erros no stderr.

- [ ] **Step 5: Commit** (após autorização)

```bash
git add src/gui/MainWindow.h src/gui/MainWindow.cpp src/gui/main_gui.cpp tests/tst_gui.cpp
git commit -m "feat(gui): load settings at boot, drive Scheduler via QTimer, quit-when-done"
```

---

## Task 13: Persistir modo de clipboard alterado pelo menu Tools

**Files:**
- Modify: `src/gui/MainWindow.cpp` (lambda do rádio de clipboard salva settings)
- Test: `tests/tst_gui.cpp`

**Interfaces:**
- Consumes: `SettingsIo::save` (Task 4), `m_settings`/`m_settingsPath` (Task 11).

- [ ] **Step 1: Escrever o teste que falha**

Em `tests/tst_gui.cpp`, um slot que muda o modo via o hook e verifica o `settings.json`:

```cpp
    void mainwindow_clipboard_mode_change_persists() {
        QTemporaryDir dir;
        EngineConfig cfg; DownloadManager mgr(cfg, dir.path());
        DownloadTableModel model(&mgr);
        MainWindow w(&mgr, &model);
        const QString path = dir.filePath("settings.json");
        w.applySettings(AppSettings{}, path);
        w.setClipboardModeForTest(ClipboardMode::Notify);   // simula clique no rádio
        const AppSettings back = SettingsIo::load(path, EngineConfig{});
        QVERIFY(back.ui.clipboardMode == ClipboardMode::Notify);
    }
```

- [ ] **Step 2: Rodar e ver falhar**

Run: `cmake --build build 2>&1 | head -20`
Expected: FAIL de compilação — `setClipboardModeForTest` inexistente.

- [ ] **Step 3: Implementar**

`src/gui/MainWindow.h` — declarar um método que centraliza a troca de modo + persistência, e o hook de teste:

```cpp
public:
    void setClipboardModeForTest(ClipboardMode m) { setClipboardMode(m); }
private:
    void setClipboardMode(ClipboardMode m);
```

`src/gui/MainWindow.cpp` — implementar e usar na lambda do rádio. Trocar o corpo da lambda do menu (hoje `[this, m] { m_clip->setMode(m); }`) por `[this, m] { setClipboardMode(m); }` e adicionar:

```cpp
void MainWindow::setClipboardMode(ClipboardMode m) {
    m_clip->setMode(m);
    m_settings.ui.clipboardMode = m;
    if (!m_settingsPath.isEmpty()) SettingsIo::save(m_settingsPath, m_settings);
}
```

- [ ] **Step 4: Rodar e ver passar**

Run: `cmake --build build && ctest --test-dir build -R tst_gui --output-on-failure`
Expected: PASS.

- [ ] **Step 5: Rodar a suíte inteira (gate final)**

Run: `cmake --build build && ctest --test-dir build --output-on-failure`
Expected: `100% tests passed` — `tst_smoke`, `tst_segmentation`, `tst_persistence`, `tst_download`, `tst_transport`, `tst_contentdisposition`, `tst_gui`, `tst_ftp`, `tst_settings`, `tst_ratelimiter`, `tst_scheduler` (11 suítes). `tst_download` mantém os 27 casos originais **sem mudança de expectativa** (mais os aditivos).

- [ ] **Step 6: Commit** (após autorização)

```bash
git add src/gui/MainWindow.h src/gui/MainWindow.cpp tests/tst_gui.cpp
git commit -m "feat(gui): persist clipboard monitor mode to settings.json"
```

---

## Verificação manual humana (pós-implementação — critérios de aceite finais, §5.1 da spec)

Não automatizável; feita manualmente após a suíte verde:

1. **Limite de banda real:** Preferences → Max speed 500 KB/s → baixar arquivo grande → velocidade agregada estabiliza perto do teto; `Unlimited` volta ao normal.
2. **Scheduler:** start/stop poucos minutos à frente → a fila inicia e pausa nos horários; `Daily` dispara de novo no dia seguinte; `quitWhenDone` fecha o app ao concluir tudo.
3. **Persistência:** mudar knobs, fechar e reabrir → voltam; idem modo de clipboard (menu Tools) e pasta padrão.
4. **User-Agent:** preset `curl/8.7.1` baixa de servidor que bloqueia UA de browser (akirabox); trocar para `Chrome` muda o comportamento.

---

## Notas de execução

- **Ordem:** as Tasks 1–3 e 5 são independentes entre si (Core puro); 4 depende de 2,3; 6 depende de 1,5; 7,8 dependem de 6; 9 depende de 4; 10 depende de 4; 11 depende de 6,10; 12 depende de 4,9,11; 13 depende de 11,12. Executar em ordem numérica satisfaz todas as dependências.
- **Gate a cada tarefa do Core:** rodar `tst_download` e confirmar os 27 casos originais intactos.
- **Reconfigurar o CMake** (`cmake -S . -B build ...`) sempre que um arquivo novo entra (Tasks 3, 4, 5, 9, 10 e os novos testes) — o Passo A do README.

---

## Extensão pós-plano: UI do Scheduler (Tasks 14–15)

Durante a revisão final identificou-se que o plano original não previa uma UI para configurar o
`SchedulerConfig` — o scheduler era dirigido só por `settings.json` editado à mão (a spec §3.6 só
definiu Preferences = General + Advanced). Optou-se por fechar a lacuna ainda na Fase 4, com um
**diálogo Scheduler próprio** (botão no toolbar, como o Orbit original).

### Task 14 — `SchedulerDialog`
`QDialog` dedicado (em `orbitgui`): `Enable schedule` (checkbox), `Start`/`Stop` (`QTimeEdit` HH:mm),
recorrência `Daily`/`Once` (rádios em `QButtonGroup`), `Quit when all downloads finish` (checkbox).
`result() → SchedulerConfig`, hooks de teste (sem `exec()`). Espelha o `PreferencesDialog`.
Arquivos: `src/gui/SchedulerDialog.{h,cpp}`, teste em `tst_gui`.

### Task 15 — Fiação + pontas soltas
- **Toolbar `Scheduler`** (antes de Preferences) → `onScheduler()` abre o diálogo, aplica e persiste.
- **`applySchedulerConfig()`** — helper único usado pelo boot (`applySettings`, sem persistir) e pelo
  diálogo (`onScheduler`, persiste). Faz um **tick imediato** para uma janela já ativa iniciar sem
  esperar os 30 s.
- **`Scheduler::setConfig` re-arma** o estado de disparo (`m_lastStartDate`/`m_lastStopDate` = `QDate()`)
  — fecha o Minor de staleness da Task 9 (uma reconfiguração no mesmo dia reavalia a janela).
- **Hardening:** rádios do `SchedulerDialog` agrupados em `QButtonGroup` explícito.
Arquivos: `src/gui/Scheduler.h`, `src/gui/SchedulerDialog.cpp`, `src/gui/MainWindow.{h,cpp}`,
testes em `tst_scheduler` e `tst_gui`.

Suíte final: **11/11**, launch headless limpo. Scheduler agora 100% configurável pela GUI.
