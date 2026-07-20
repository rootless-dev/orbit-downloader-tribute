# Browser Integration (Fase 5) Implementation Plan

**Goal:** Interceptar todos os downloads do Chrome/Chromium via uma extensão MV3 e entregá-los ao app por um endpoint HTTP local em loopback, encaminhando cookies + `Referer` + `User-Agent` para que downloads logados funcionem.

**Architecture:** Uma extensão MV3 cancela cada download no navegador e faz `POST` ao `BrowserBridge` (um `QTcpServer` em `127.0.0.1`). O bridge autoriza por token (+ `Origin`), parseia o pedido e emite `downloadRequested`, que a `MainWindow` enfileira no `DownloadManager` (auto-add + notificação na bandeja). O Core ganha **headers por-download** que o `HttpProbe`/`SegmentWorker` aplicam no `QNetworkRequest`, no mesmo molde do `Credentials` já existente.

**Tech Stack:** C++20, Qt 6.11 (Core, Network, Widgets, Test), CMake, QtTest. Extensão: JavaScript (Manifest V3, `chrome.downloads`/`chrome.cookies`/`chrome.storage`/`chrome.notifications`).

## Global Constraints

- **C++20**; Qt 6.11 via Homebrew (`/opt/homebrew`). Configurar: `cmake -S . -B build -DCMAKE_PREFIX_PATH=/opt/homebrew`. Testar: `ctest --test-dir build --output-on-failure`.
- **`orbitcore` não depende de QtWidgets** (só Core+Network). **`orbitgui_logic`** é a lib "pura" sem QtWidgets.
- **Tudo no event loop principal** — sem threads/mutex.
- **Gate de regressão:** todos os casos **existentes** de `tst_download` continuam passando **sem mudança de expectativa** (só adicionar casos novos — não editar os antigos). Toda a suíte (`ctest`) verde antes de cada commit.
- **Mensagens de commit em inglês, Conventional Commits.** Nunca `Co-Authored-By`.
- **Spec de referência:** `docs/design/specs/2026-07-19-browser-integration-design.md` (as §N abaixo remetem a ela).
- **Tipo compartilhado:** `using HeaderList = QList<QPair<QByteArray, QByteArray>>;` (definido na Task 1, usado em todo o plano).
- **Escopo:** Chrome/Chromium MV3; só liga/desliga global; um sentido (extensão→app); FTP não se aplica.

---

## File Structure

**Core (`src/core/`):**
- `DownloadTypes.h` (modify) — `HeaderList` typedef.
- `Persistence.h`/`Persistence.cpp` (modify) — `DownloadRecord.extraHeaders` + (de)serialização na sessão.
- `Transport.h` (modify) — `Probe::start`/`SegmentSource::start` ganham `const HeaderList&`.
- `HttpProbe.h`/`.cpp`, `SegmentWorker.h`/`.cpp` (modify) — aplicam headers no request.
- `FtpProbe.h`/`.cpp`, `FtpSegmentWorker.h`/`.cpp` (modify) — assinatura nova, `Q_UNUSED`.
- `DownloadTask.h`/`.cpp` (modify) — `m_extraHeaders`; `init`/`restore`/`record`; repassa no `start`.
- `DownloadManager.h`/`.cpp` (modify) — `addDownload` com `extraHeaders`.

**GUI logic (`src/gui/`, lib `orbitgui_logic`):**
- `BrowserBridgeProtocol.h`/`.cpp` (create) — funções puras + `generateBridgeToken` + `kExtensionOrigin`.
- `BrowserBridge.h`/`.cpp` (create) — `QTcpServer` loopback.
- `Settings.h`/`.cpp` (modify) — `BrowserPrefs` + (de)serialização.

**GUI widgets (`src/gui/`, lib `orbitgui`):**
- `PreferencesDialog.h`/`.cpp` (modify) — seção Browser.
- `MainWindow.h`/`.cpp` (modify) — cria/religa o bridge; slot de enfileirar + bandeja.

**Build:**
- `src/gui/CMakeLists.txt` (modify) — `BrowserBridge*` em `orbitgui_logic` + `Qt6::Network`.
- `tests/CMakeLists.txt` (modify) — alvo `tst_browserbridge`.

**Tests (`tests/`):**
- `tst_persistence.cpp` (modify) — round-trip de `extraHeaders`.
- `FakeTransport.h` (modify) — assinaturas novas + captura de headers.
- `TestServer.h`/`.cpp` (modify) — captura Cookie/Referer.
- `tst_download.cpp` (modify) — headers chegam ao wire.
- `tst_settings.cpp` (modify) — bloco `browser`.
- `tst_browserbridge.cpp` (create) — unit + integração loopback.
- `tst_gui.cpp` (modify) — wiring do bridge na MainWindow.

**Extension (`extension/chrome/`):**
- `manifest.json`, `background.js`, `options.html`, `options.js` (create) + `README.md` de instalação.

**Docs:**
- `README.md` (modify) — carregar a extensão + E2E manual (§11.4).

---

## Task 1: Core — `HeaderList` + persistência de headers na sessão

**Files:**
- Modify: `src/core/DownloadTypes.h`
- Modify: `src/core/Persistence.h`, `src/core/Persistence.cpp`
- Test: `tests/tst_persistence.cpp`

**Interfaces:**
- Produces: `using HeaderList = QList<QPair<QByteArray, QByteArray>>;`; `DownloadRecord::extraHeaders` (campo `HeaderList`); round-trip em `writeSession`/`readSession`.

- [ ] **Step 1: Escrever o teste que falha** — em `tests/tst_persistence.cpp`, novo slot (registrar no bloco de slots privados existente):

```cpp
void sessionRoundTripsExtraHeaders() {
    const QString path = m_tmp.filePath("dl.json");
    DownloadRecord rec;
    rec.id       = QUuid::createUuid();
    rec.url      = QUrl("https://example.com/f.zip");
    rec.destPath = "/tmp/f.zip";
    rec.state    = DownloadState::Paused;
    rec.extraHeaders = {
        {QByteArray("Cookie"),     QByteArray("sid=abc; t=1")},
        {QByteArray("Referer"),    QByteArray("https://example.com/p")},
        {QByteArray("User-Agent"), QByteArray("Mozilla/5.0")},
    };
    QVERIFY(Persistence::writeSession(path, {rec}));
    const auto back = Persistence::readSession(path);
    QCOMPARE(back.size(), 1);
    QCOMPARE(back[0].extraHeaders, rec.extraHeaders);
}
```

(`m_tmp` é o `QTemporaryDir` já usado no arquivo; confirme o nome do membro e o padrão dos slots antes de colar.)

- [ ] **Step 2: Rodar e ver falhar**

Run: `cmake --build build --target tst_persistence && ctest --test-dir build -R tst_persistence --output-on-failure`
Expected: FALHA na compilação (`extraHeaders` não existe em `DownloadRecord`).

- [ ] **Step 3: Adicionar o typedef** — em `src/core/DownloadTypes.h`, logo após os `#include`:

```cpp
using HeaderList = QList<QPair<QByteArray, QByteArray>>;
```

- [ ] **Step 4: Adicionar o campo ao registro** — em `src/core/Persistence.h`, dentro de `struct DownloadRecord` (após `Priority priority`):

```cpp
    HeaderList    extraHeaders;   // headers por-download (cookies/referer/UA do browser)
```

- [ ] **Step 5: Serializar/desserializar** — em `src/core/Persistence.cpp`, no ponto onde cada `DownloadRecord` vira `QJsonObject` (dentro de `writeSession`), acrescente ao objeto do registro:

```cpp
    QJsonArray hdrs;
    for (const auto& h : rec.extraHeaders)
        hdrs.append(QJsonObject{{"name",  QString::fromUtf8(h.first)},
                                {"value", QString::fromUtf8(h.second)}});
    obj["headers"] = hdrs;   // `obj` é o QJsonObject do registro; ajuste o nome à variável local
```

E na leitura (dentro de `readSession`, onde o `QJsonObject` vira `DownloadRecord`):

```cpp
    for (const QJsonValue& v : obj.value("headers").toArray()) {
        const QJsonObject h = v.toObject();
        rec.extraHeaders.append({h.value("name").toString().toUtf8(),
                                 h.value("value").toString().toUtf8()});
    }
```

Inclua `#include <QJsonArray>` no topo do `.cpp` se ainda não houver.

- [ ] **Step 6: Rodar e ver passar**

Run: `cmake --build build --target tst_persistence && ctest --test-dir build -R tst_persistence --output-on-failure`
Expected: PASS.

- [ ] **Step 7: Commit**

```bash
git add src/core/DownloadTypes.h src/core/Persistence.h src/core/Persistence.cpp tests/tst_persistence.cpp
git commit -m "feat(core): persist per-download headers in the session record"
```

---

## Task 2: Core — encaminhar `extraHeaders` pelas interfaces (refactor sem comportamento novo)

**Files:**
- Modify: `src/core/Transport.h`, `src/core/HttpProbe.h`/`.cpp`, `src/core/SegmentWorker.h`/`.cpp`, `src/core/FtpProbe.h`/`.cpp`, `src/core/FtpSegmentWorker.h`/`.cpp`
- Modify: `src/core/DownloadTask.h`/`.cpp`, `src/core/DownloadManager.h`/`.cpp`
- Modify: `tests/FakeTransport.h`

**Interfaces:**
- Consumes: `HeaderList` (Task 1).
- Produces: `Probe::start(const QUrl&, const Credentials&, const HeaderList&)`; `SegmentSource::start(const Segment&, const QUrl&, const QString&, const Credentials&, const HeaderList&)`; `DownloadManager::addDownload(const QUrl&, const QString&, const HeaderList& = {})`; `DownloadTask::init(..., const HeaderList& extraHeaders = {})`.

Este é um refactor de assinatura: as implementações **recebem mas ainda não aplicam** os headers HTTP (isso é a Task 3). O gate é a suíte inteira continuar verde.

- [ ] **Step 1: Estender as interfaces** — em `src/core/Transport.h`:

```cpp
    virtual void start(const QUrl& url, const Credentials& creds,
                       const HeaderList& extraHeaders) = 0;          // Probe
```
```cpp
    virtual void start(const Segment& seg, const QUrl& url,
                       const QString& validator, const Credentials& creds,
                       const HeaderList& extraHeaders) = 0;          // SegmentSource
```

- [ ] **Step 2: Atualizar `HttpProbe`** — em `HttpProbe.h` a assinatura; em `HttpProbe.cpp`:

```cpp
void HttpProbe::start(const QUrl& url, const Credentials& creds,
                      const HeaderList& extraHeaders) {
    Q_UNUSED(creds);
    Q_UNUSED(extraHeaders);   // aplicado na Task 3
    QNetworkRequest req(url);
    // ... resto inalterado ...
}
```

- [ ] **Step 3: Atualizar `SegmentWorker`** — em `SegmentWorker.h` a assinatura + membro `HeaderList m_extraHeaders;`; em `SegmentWorker.cpp`:

```cpp
void SegmentWorker::start(const Segment& seg, const QUrl& url,
                          const QString& validator, const Credentials& creds,
                          const HeaderList& extraHeaders) {
    Q_UNUSED(creds);
    m_extraHeaders = extraHeaders;   // guardado; aplicado em openRequest() na Task 3
    m_seg = seg;
    // ... resto inalterado ...
}
```

- [ ] **Step 4: Atualizar FTP** — em `FtpProbe.h`/`.cpp` e `FtpSegmentWorker.h`/`.cpp`, novas assinaturas com `Q_UNUSED(extraHeaders);` no início de cada `start`.

- [ ] **Step 5: Atualizar os fakes** — em `tests/FakeTransport.h`, as três `start` (`FakeProbe`, `FakeWorker`, `RestartingWorker`) ganham `const HeaderList& extraHeaders` e `Q_UNUSED(extraHeaders);`.

- [ ] **Step 6: Encaminhar na `DownloadTask`** — em `DownloadTask.h`: membro `HeaderList m_extraHeaders;` e `init(const QUuid&, const QUrl&, const QString&, int, const HeaderList& extraHeaders = {})`. Em `DownloadTask.cpp`:
  - em `init(...)`: `m_extraHeaders = extraHeaders;`
  - em `restore(...)`: `m_extraHeaders = rec.extraHeaders;`
  - em `record()`: `rec.extraHeaders = m_extraHeaders;` (antes do `return`)
  - na linha `probe->start(m_url, m_creds);` → `probe->start(m_url, m_creds, m_extraHeaders);`
  - na linha `w->start(seg, m_url, validator, m_creds);` → `w->start(seg, m_url, validator, m_creds, m_extraHeaders);`

- [ ] **Step 7: `addDownload`** — em `DownloadManager.h`: `QUuid addDownload(const QUrl& url, const QString& destPath, const HeaderList& extraHeaders = {});`. Em `DownloadManager.cpp`:

```cpp
QUuid DownloadManager::addDownload(const QUrl& url, const QString& destPath,
                                   const HeaderList& extraHeaders) {
    Transport* tr = transportFor(url);
    if (!tr) return QUuid();
    const QString finalPath = Persistence::resolveUniquePath(destPath);
    auto* t = new DownloadTask(tr, m_cfg, &m_limiter, this);
    t->init(QUuid::createUuid(), url, finalPath, m_cfg.segmentCount, extraHeaders);
    wire(t);
    t->setLogger(m_logger);
    m_tasks.append(t);
    saveSession();
    pump();
    return t->id();
}
```

- [ ] **Step 8: Rodar a suíte inteira (gate)**

Run: `cmake -S . -B build -DCMAKE_PREFIX_PATH=/opt/homebrew && cmake --build build && ctest --test-dir build --output-on-failure`
Expected: TODOS verdes, incluindo `tst_download` sem mudança de expectativa (só passamos lista de headers vazia — comportamento idêntico ao de antes).

- [ ] **Step 9: Commit**

```bash
git add src/core tests/FakeTransport.h
git commit -m "refactor(core): thread per-download headers through the transport interface"
```

---

## Task 3: Core — aplicar headers no request HTTP (probe + segmentos)

**Files:**
- Modify: `src/core/HttpProbe.cpp`, `src/core/SegmentWorker.cpp`
- Modify: `tests/TestServer.h`, `tests/TestServer.cpp`
- Test: `tests/tst_download.cpp`

**Interfaces:**
- Consumes: `HttpProbe::start(...extraHeaders)`, `SegmentWorker` `m_extraHeaders` (Task 2).
- Produces: comportamento — Cookie/Referer/User-Agent extra vão ao wire; `User-Agent` extra substitui o default. `TestServer::cookiesSeen()`/`referersSeen()`.

- [ ] **Step 1: Estender o `TestServer` para capturar headers** — em `TestServer.h`, membros + getters ao lado de `m_uaSeen`:

```cpp
    QStringList cookiesSeen()  const { return QStringList(m_cookiesSeen.begin(),  m_cookiesSeen.end()); }
    QStringList referersSeen() const { return QStringList(m_referersSeen.begin(), m_referersSeen.end()); }
private:
    QSet<QString> m_cookiesSeen;
    QSet<QString> m_referersSeen;
```
Em `TestServer.cpp`, no handler da rota `/ranged` (onde já grava `m_uaSeen`), grave também, quando não-vazios, `request.value("Cookie")` e `request.value("Referer")` (siga o mesmo padrão de captura do User-Agent já presente ali).

- [ ] **Step 2: Escrever o teste que falha** — em `tests/tst_download.cpp`, novo slot:

```cpp
void extraHeadersReachTheWire() {
    TestServer server(makeBody(3 * 1024 * 1024));   // helper de corpo já usado no arquivo
    QVERIFY(server.listen());
    DownloadManager mgr(EngineConfig{}, m_dir.path());   // padrão do arquivo p/ dataDir
    const HeaderList hdrs = {
        {QByteArray("Cookie"),     QByteArray("sid=zzz")},
        {QByteArray("Referer"),    QByteArray("https://ref.example/p")},
        {QByteArray("User-Agent"), QByteArray("BrowserUA/9")},
    };
    const QString dest = m_dir.filePath("out.bin");
    mgr.addDownload(server.url("/ranged"), dest, hdrs);
    QTRY_VERIFY_WITH_TIMEOUT(fileExistsWithSize(dest, 3 * 1024 * 1024), 10000);
    QVERIFY(server.cookiesSeen().contains("sid=zzz"));
    QVERIFY(server.referersSeen().contains("https://ref.example/p"));
    QVERIFY(server.userAgentsSeen().contains("BrowserUA/9"));      // override do default
    QVERIFY(!server.userAgentsSeen().contains("curl/8.7.1"));       // default NÃO usado
}
```

(Adapte `makeBody`/`fileExistsWithSize`/`m_dir` aos helpers reais do arquivo — reuse os já existentes.)

- [ ] **Step 3: Rodar e ver falhar**

Run: `cmake --build build --target tst_download && ctest --test-dir build -R tst_download --output-on-failure`
Expected: FALHA (headers extra não chegam; `cookiesSeen` vazio; UA default ainda presente).

- [ ] **Step 4: Aplicar no `HttpProbe`** — em `HttpProbe::start`, remover o `Q_UNUSED(extraHeaders)` e, **após** `req.setRawHeader("User-Agent", m_userAgent);`:

```cpp
    for (const auto& h : extraHeaders) req.setRawHeader(h.first, h.second);
```

- [ ] **Step 5: Aplicar no `SegmentWorker`** — em `SegmentWorker::openRequest`, **após** `req.setRawHeader("User-Agent", m_cfg.userAgent.toUtf8());`:

```cpp
    for (const auto& h : m_extraHeaders) req.setRawHeader(h.first, h.second);
```

- [ ] **Step 6: Rodar e ver passar (+ gate)**

Run: `cmake --build build --target tst_download && ctest --test-dir build -R tst_download --output-on-failure`
Expected: PASS no caso novo **e** em todos os já existentes (sem regressão).

- [ ] **Step 7: Commit**

```bash
git add src/core/HttpProbe.cpp src/core/SegmentWorker.cpp tests/TestServer.h tests/TestServer.cpp tests/tst_download.cpp
git commit -m "feat(core): send per-download Cookie/Referer/User-Agent on HTTP requests"
```

---

## Task 4: GUI logic — `BrowserPrefs` no `settings.json`

**Files:**
- Modify: `src/gui/Settings.h`, `src/gui/Settings.cpp`
- Test: `tests/tst_settings.cpp`

**Interfaces:**
- Produces: `struct BrowserPrefs { bool enabled=false; quint16 port=8697; QString token; };` + `AppSettings::browser`; round-trip em `SettingsIo::fromJson`/`toJson` preservando chaves desconhecidas.

- [ ] **Step 1: Teste que falha** — em `tests/tst_settings.cpp`:

```cpp
void browserBlockRoundTrips() {
    QJsonObject root;
    root["browser"] = QJsonObject{{"enabled", true}, {"port", 9000},
                                  {"token", "deadbeef"}};
    root["mystery"] = "keep-me";                 // chave desconhecida
    const AppSettings s = SettingsIo::fromJson(root, EngineConfig{});
    QCOMPARE(s.browser.enabled, true);
    QCOMPARE(s.browser.port, quint16(9000));
    QCOMPARE(s.browser.token, QString("deadbeef"));
    const QJsonObject out = SettingsIo::toJson(s, root);
    QCOMPARE(out.value("browser").toObject().value("port").toInt(), 9000);
    QCOMPARE(out.value("mystery").toString(), QString("keep-me"));   // preservada
}

void browserDefaultsAreTolerant() {
    const AppSettings s = SettingsIo::fromJson(QJsonObject{}, EngineConfig{});
    QCOMPARE(s.browser.enabled, false);
    QCOMPARE(s.browser.port, quint16(8697));
    QVERIFY(s.browser.token.isEmpty());
}
```

- [ ] **Step 2: Rodar e ver falhar**

Run: `cmake --build build --target tst_settings && ctest --test-dir build -R tst_settings --output-on-failure`
Expected: FALHA (`browser` não existe em `AppSettings`).

- [ ] **Step 3: Struct + campo** — em `src/gui/Settings.h`, antes de `struct AppSettings`:

```cpp
struct BrowserPrefs {
    bool    enabled = false;
    quint16 port    = 8697;
    QString token;
};
```
E dentro de `AppSettings`: `BrowserPrefs browser;`.

- [ ] **Step 4: (De)serializar** — em `src/gui/Settings.cpp`, em `fromJson` (antes do `return s;`):

```cpp
    const QJsonObject br = root.value("browser").toObject();
    s.browser.enabled = br.value("enabled").toBool(false);
    s.browser.port    = quint16(br.value("port").toInt(8697));
    s.browser.token   = br.value("token").toString();
```
Em `toJson` (antes do `return root;`):

```cpp
    root["browser"] = QJsonObject{
        {"enabled", s.browser.enabled},
        {"port",    int(s.browser.port)},
        {"token",   s.browser.token}};
```

- [ ] **Step 5: Rodar e ver passar**

Run: `cmake --build build --target tst_settings && ctest --test-dir build -R tst_settings --output-on-failure`
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add src/gui/Settings.h src/gui/Settings.cpp tests/tst_settings.cpp
git commit -m "feat(gui): persist browser-bridge prefs (enabled/port/token) in settings.json"
```

---

## Task 5: GUI logic — protocolo puro do bridge + identidade da extensão

**Files:**
- Create: `src/gui/BrowserBridgeProtocol.h`, `src/gui/BrowserBridgeProtocol.cpp`
- Create: `tests/tst_browserbridge.cpp`
- Modify: `src/gui/CMakeLists.txt`, `tests/CMakeLists.txt`

**Interfaces:**
- Produces: `parseAddRequest`, `parseBody`, `headersFromPayload`, `authorize`, `generateBridgeToken`, `AddRequest`, `DownloadPayload`, `AuthResult`, e a constante `kExtensionOrigin`.

- [ ] **Step 1: Gerar a chave e o ID da extensão** — a extensão precisa de um **ID estável**; o app allowlista `chrome-extension://<ID>`. Gere um par de chaves e derive o ID (rode e **anote o ID de saída**):

```bash
# chave privada (guardar fora do git; só a pública vai pro manifesto)
openssl genrsa 2048 | openssl pkcs8 -topk8 -nocrypt -out extension.pem
# valor do campo "key" do manifest.json (base64 da SPKI DER):
openssl rsa -in extension.pem -pubout -outform DER 2>/dev/null | openssl base64 -A ; echo
# ID da extensão (hash da SPKI DER mapeado p/ a-p):
openssl rsa -in extension.pem -pubout -outform DER 2>/dev/null | openssl dgst -sha256 -binary \
 | head -c16 | xxd -p -c1 | awk '{printf "%c", 97 + strtonum("0x"$1)/16 ; printf "%c", 97 + strtonum("0x"$1)%16}' ; echo
```
Guarde `extension.pem` num local seguro (não versionar) e anote o **ID** (32 letras a–p) e a **key base64**. O `key` entra no `manifest.json` na Task 9.

- [ ] **Step 2: Escrever os testes que falham** — `tests/tst_browserbridge.cpp`:

```cpp
#include <QtTest/QtTest>
#include "BrowserBridgeProtocol.h"

class TestBrowserBridge : public QObject {
    Q_OBJECT
private slots:
    void parsesWellFormedPost() {
        const QByteArray raw =
            "POST /add HTTP/1.1\r\nHost: 127.0.0.1:8697\r\n"
            "Origin: chrome-extension://abc\r\nX-Orbit-Token: tok\r\n"
            "Content-Type: application/json\r\nContent-Length: 2\r\n\r\n{}";
        const AddRequest r = parseAddRequest(raw);
        QVERIFY(r.headersComplete && r.bodyComplete);
        QCOMPARE(r.method, QString("POST"));
        QCOMPARE(r.path, QString("/add"));
        QCOMPARE(r.origin, QString("chrome-extension://abc"));
        QCOMPARE(r.token, QString("tok"));
        QCOMPARE(r.body, QByteArray("{}"));
    }
    void reportsIncompleteBody() {
        const QByteArray raw =
            "POST /add HTTP/1.1\r\nContent-Length: 10\r\n\r\n{}";   // só 2 dos 10
        const AddRequest r = parseAddRequest(raw);
        QVERIFY(r.headersComplete);
        QVERIFY(!r.bodyComplete);
    }
    void parsesBodyUrlAndFields() {
        const DownloadPayload p = parseBody(
            R"({"url":"https://h/f.zip","filename":"f.zip","referrer":"https://h/p",)"
            R"("userAgent":"UA","cookie":"a=1"})");
        QVERIFY(p.valid);
        QCOMPARE(p.url, QUrl("https://h/f.zip"));
        QCOMPARE(p.filename, QString("f.zip"));
        const HeaderList h = headersFromPayload(p);
        QVERIFY(h.contains({QByteArray("Cookie"), QByteArray("a=1")}));
        QVERIFY(h.contains({QByteArray("Referer"), QByteArray("https://h/p")}));
        QVERIFY(h.contains({QByteArray("User-Agent"), QByteArray("UA")}));
    }
    void rejectsNonHttpUrl() {
        QVERIFY(!parseBody(R"({"url":"file:///etc/passwd"})").valid);
        QVERIFY(!parseBody(R"({"foo":1})").valid);          // sem url
    }
    void authorizeAcceptsMatchingOriginAndToken() {
        AddRequest r; r.origin = "chrome-extension://x"; r.token = "s3cret";
        QCOMPARE(authorize(r, "s3cret", "chrome-extension://x"), AuthResult::Ok);
    }
    void authorizeAcceptsAbsentOrigin() {                    // extensão host-permission
        AddRequest r; r.origin = ""; r.token = "s3cret";
        QCOMPARE(authorize(r, "s3cret", "chrome-extension://x"), AuthResult::Ok);
    }
    void authorizeRejectsWrongOrigin() {
        AddRequest r; r.origin = "https://evil.test"; r.token = "s3cret";
        QCOMPARE(authorize(r, "s3cret", "chrome-extension://x"), AuthResult::Forbidden);
    }
    void authorizeRejectsBadToken() {
        AddRequest r; r.origin = "chrome-extension://x"; r.token = "nope";
        QCOMPARE(authorize(r, "s3cret", "chrome-extension://x"), AuthResult::Unauthorized);
        AddRequest empty; empty.token = "anything";
        QCOMPARE(authorize(empty, "", "chrome-extension://x"), AuthResult::Unauthorized);
    }
    void tokenIs32Hex() {
        const QString t = generateBridgeToken();
        QCOMPARE(t.size(), 32);
        QVERIFY(QRegularExpression("^[0-9a-f]{32}$").match(t).hasMatch());
    }
};
QTEST_MAIN(TestBrowserBridge)
#include "tst_browserbridge.moc"
```

- [ ] **Step 3: Criar `BrowserBridgeProtocol.h`**:

```cpp
#pragma once
#include "DownloadTypes.h"   // HeaderList (orbitcore)
#include <QByteArray>
#include <QString>
#include <QUrl>

// Origin permitido = a nossa extensão (ID fixado pela `key` do manifesto, Task 5/9).
// PÚBLICO (está no repo): não é segredo — o token é a credencial real (ver spec §6.1).
inline const QString kExtensionOrigin =
    QStringLiteral("chrome-extension://REPLACE_WITH_ID_FROM_TASK5_STEP1");

struct AddRequest {
    QString    method;
    QString    path;
    QString    origin;                 // vazio se ausente
    QString    token;                  // X-Orbit-Token; vazio se ausente
    qint64     contentLength = -1;
    QByteArray body;
    bool       headersComplete = false;
    bool       bodyComplete    = false;
};
AddRequest parseAddRequest(const QByteArray& raw);

struct DownloadPayload {
    QUrl    url;
    QString filename, referrer, userAgent, cookie;
    bool    valid = false;             // url http/https presente
};
DownloadPayload parseBody(const QByteArray& json);
HeaderList headersFromPayload(const DownloadPayload& p);

enum class AuthResult { Ok, Unauthorized, Forbidden };
AuthResult authorize(const AddRequest& req, const QString& expectedToken,
                     const QString& allowedOrigin);

QString generateBridgeToken();         // 32 hex, QRandomGenerator::system()
```
Substitua `REPLACE_WITH_ID_FROM_TASK5_STEP1` pelo ID anotado no Step 1.

- [ ] **Step 4: Criar `BrowserBridgeProtocol.cpp`**:

```cpp
#include "BrowserBridgeProtocol.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>

static QString headerValue(const QByteArray& headerBlock, const char* name) {
    for (const QByteArray& line : headerBlock.split('\n')) {
        const int colon = line.indexOf(':');
        if (colon < 0) continue;
        if (line.left(colon).trimmed().toLower() == QByteArray(name).toLower())
            return QString::fromUtf8(line.mid(colon + 1).trimmed());
    }
    return QString();
}

AddRequest parseAddRequest(const QByteArray& raw) {
    AddRequest r;
    const int sep = raw.indexOf("\r\n\r\n");
    if (sep < 0) return r;                 // headers incompletos
    r.headersComplete = true;
    const QByteArray head = raw.left(sep);
    const int firstEol = head.indexOf("\r\n");
    const QByteArray requestLine = firstEol < 0 ? head : head.left(firstEol);
    const QList<QByteArray> parts = requestLine.split(' ');
    if (parts.size() >= 2) { r.method = QString::fromUtf8(parts[0]);
                             r.path   = QString::fromUtf8(parts[1]); }
    const QByteArray headers = firstEol < 0 ? QByteArray() : head.mid(firstEol + 2);
    r.origin = headerValue(headers, "Origin");
    r.token  = headerValue(headers, "X-Orbit-Token");
    const QString cl = headerValue(headers, "Content-Length");
    r.contentLength = cl.isEmpty() ? -1 : cl.toLongLong();
    r.body = raw.mid(sep + 4);
    r.bodyComplete = (r.contentLength >= 0)
        ? (r.body.size() >= r.contentLength)
        : true;                            // sem corpo declarado: nada a esperar
    if (r.contentLength >= 0) r.body = r.body.left(r.contentLength);
    return r;
}

DownloadPayload parseBody(const QByteArray& json) {
    DownloadPayload p;
    const QJsonObject o = QJsonDocument::fromJson(json).object();
    const QUrl u(o.value("url").toString());
    const QString scheme = u.scheme().toLower();
    if (!u.isValid() || (scheme != "http" && scheme != "https")) return p;
    p.url       = u;
    p.filename  = o.value("filename").toString();
    p.referrer  = o.value("referrer").toString();
    p.userAgent = o.value("userAgent").toString();
    p.cookie    = o.value("cookie").toString();
    p.valid     = true;
    return p;
}

HeaderList headersFromPayload(const DownloadPayload& p) {
    HeaderList h;
    if (!p.cookie.isEmpty())    h.append({"Cookie",     p.cookie.toUtf8()});
    if (!p.referrer.isEmpty())  h.append({"Referer",    p.referrer.toUtf8()});
    if (!p.userAgent.isEmpty()) h.append({"User-Agent", p.userAgent.toUtf8()});
    return h;
}

static bool constantTimeEquals(const QByteArray& a, const QByteArray& b) {
    if (a.size() != b.size()) return false;
    quint8 diff = 0;
    for (int i = 0; i < a.size(); ++i)
        diff |= quint8(a[i]) ^ quint8(b[i]);
    return diff == 0;
}

AuthResult authorize(const AddRequest& req, const QString& expectedToken,
                     const QString& allowedOrigin) {
    if (!req.origin.isEmpty() && req.origin != allowedOrigin)
        return AuthResult::Forbidden;
    if (expectedToken.isEmpty() ||
        !constantTimeEquals(req.token.toUtf8(), expectedToken.toUtf8()))
        return AuthResult::Unauthorized;
    return AuthResult::Ok;
}

QString generateBridgeToken() {
    quint32 buf[4];
    QRandomGenerator::system()->fillRange(buf);
    return QString::fromLatin1(
        QByteArray(reinterpret_cast<const char*>(buf), sizeof(buf)).toHex());
}
```

- [ ] **Step 5: CMake** — em `src/gui/CMakeLists.txt`, adicionar `BrowserBridgeProtocol.cpp` à lib `orbitgui_logic` e trocar a linha de link para incluir `Qt6::Network`:

```cmake
add_library(orbitgui_logic STATIC
    FileType.cpp
    UrlName.cpp
    GridGeometry.cpp
    SpeedSampler.cpp
    DropTargets.cpp
    Settings.cpp
    Scheduler.cpp
    BrowserBridgeProtocol.cpp
)
target_link_libraries(orbitgui_logic PUBLIC orbitcore Qt6::Core Qt6::Network)
```
Em `tests/CMakeLists.txt`:

```cmake
add_executable(tst_browserbridge tst_browserbridge.cpp)
target_link_libraries(tst_browserbridge PRIVATE orbitgui_logic Qt6::Test Qt6::Network)
add_test(NAME tst_browserbridge COMMAND tst_browserbridge)
```

- [ ] **Step 6: Rodar e ver passar**

Run: `cmake -S . -B build -DCMAKE_PREFIX_PATH=/opt/homebrew && cmake --build build --target tst_browserbridge && ctest --test-dir build -R tst_browserbridge --output-on-failure`
Expected: PASS em todos os slots.

- [ ] **Step 7: Commit**

```bash
git add src/gui/BrowserBridgeProtocol.h src/gui/BrowserBridgeProtocol.cpp src/gui/CMakeLists.txt tests/CMakeLists.txt tests/tst_browserbridge.cpp
git commit -m "feat(gui): pure HTTP request parser and authorizer for the browser bridge"
```

---

## Task 6: GUI logic — `BrowserBridge` (servidor loopback)

**Files:**
- Create: `src/gui/BrowserBridge.h`, `src/gui/BrowserBridge.cpp`
- Modify: `src/gui/CMakeLists.txt`
- Test: `tests/tst_browserbridge.cpp`

**Interfaces:**
- Consumes: as funções puras da Task 5.
- Produces: `BrowserBridge` com `bool start(quint16 port, const QString& token, const QString& allowedOrigin)`, `void stop()`, `bool listening() const`, `quint16 port() const`, sinal `downloadRequested(const QUrl&, const HeaderList&, const QString&)`.

- [ ] **Step 1: Teste de integração que falha** — acrescentar ao `tst_browserbridge.cpp` (novos slots; inclua `#include "BrowserBridge.h"`, `<QTcpSocket>`, `<QSignalSpy>`):

```cpp
    void loopbackAcceptsValidPost() {
        BrowserBridge b;
        QVERIFY(b.start(0, "tok", kExtensionOrigin));   // porta 0 = efêmera
        QSignalSpy spy(&b, &BrowserBridge::downloadRequested);
        const QByteArray body = R"({"url":"https://h/f.zip","cookie":"a=1"})";
        QTcpSocket s; s.connectToHost(QHostAddress::LocalHost, b.port());
        QVERIFY(s.waitForConnected(2000));
        const QByteArray req = "POST /add HTTP/1.1\r\nHost: x\r\n"
            "Origin: " + kExtensionOrigin.toUtf8() + "\r\nX-Orbit-Token: tok\r\n"
            "Content-Length: " + QByteArray::number(body.size()) + "\r\n\r\n" + body;
        s.write(req); QVERIFY(s.waitForBytesWritten(2000));
        QVERIFY(spy.wait(2000));
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(0).toUrl(), QUrl("https://h/f.zip"));
        QVERIFY(s.waitForReadyRead(2000));
        QVERIFY(s.readAll().startsWith("HTTP/1.1 200"));
    }
    void loopbackRejectsBadToken() {
        BrowserBridge b; QVERIFY(b.start(0, "tok", kExtensionOrigin));
        QSignalSpy spy(&b, &BrowserBridge::downloadRequested);
        const QByteArray body = R"({"url":"https://h/f.zip"})";
        QTcpSocket s; s.connectToHost(QHostAddress::LocalHost, b.port());
        QVERIFY(s.waitForConnected(2000));
        s.write("POST /add HTTP/1.1\r\nX-Orbit-Token: WRONG\r\nContent-Length: "
                + QByteArray::number(body.size()) + "\r\n\r\n" + body);
        QVERIFY(s.waitForReadyRead(2000));
        QVERIFY(s.readAll().startsWith("HTTP/1.1 401"));
        QCOMPARE(spy.count(), 0);
    }
    void loopbackPreflightHasCorsAndPna() {
        BrowserBridge b; QVERIFY(b.start(0, "tok", kExtensionOrigin));
        QTcpSocket s; s.connectToHost(QHostAddress::LocalHost, b.port());
        QVERIFY(s.waitForConnected(2000));
        s.write("OPTIONS /add HTTP/1.1\r\nOrigin: " + kExtensionOrigin.toUtf8()
                + "\r\nAccess-Control-Request-Private-Network: true\r\n\r\n");
        QVERIFY(s.waitForReadyRead(2000));
        const QByteArray resp = s.readAll();
        QVERIFY(resp.startsWith("HTTP/1.1 204"));
        QVERIFY(resp.contains("Access-Control-Allow-Private-Network: true"));
        QVERIFY(resp.contains("Access-Control-Allow-Origin: " + kExtensionOrigin.toUtf8()));
    }
```

- [ ] **Step 2: Rodar e ver falhar**

Run: `cmake --build build --target tst_browserbridge`
Expected: FALHA de compilação (`BrowserBridge` não existe).

- [ ] **Step 3: Criar `BrowserBridge.h`**:

```cpp
#pragma once
#include "BrowserBridgeProtocol.h"
#include "DownloadTypes.h"
#include <QObject>
#include <QHash>
class QTcpServer;
class QTcpSocket;

class BrowserBridge : public QObject {
    Q_OBJECT
public:
    explicit BrowserBridge(QObject* parent = nullptr);
    bool    start(quint16 port, const QString& token, const QString& allowedOrigin);
    void    stop();
    bool    listening() const;
    quint16 port() const;
signals:
    void downloadRequested(const QUrl& url, const HeaderList& headers,
                           const QString& suggestedFilename);
private:
    void onNewConnection();
    void onReadyRead(QTcpSocket* sock);
    void sendJson(QTcpSocket* sock, int status, const char* reason, const QByteArray& body);
    void sendPreflight(QTcpSocket* sock);
    QByteArray corsBlock() const;

    QTcpServer* m_server = nullptr;
    QString     m_token, m_allowedOrigin;
    QHash<QTcpSocket*, QByteArray> m_buffers;
    static constexpr int kMaxRequestBytes = 64 * 1024;
};
```

- [ ] **Step 4: Criar `BrowserBridge.cpp`**:

```cpp
#include "BrowserBridge.h"
#include <QTcpServer>
#include <QTcpSocket>

BrowserBridge::BrowserBridge(QObject* parent) : QObject(parent) {}

bool BrowserBridge::start(quint16 port, const QString& token, const QString& allowedOrigin) {
    stop();
    m_token = token;
    m_allowedOrigin = allowedOrigin;
    m_server = new QTcpServer(this);
    connect(m_server, &QTcpServer::newConnection, this, &BrowserBridge::onNewConnection);
    if (!m_server->listen(QHostAddress::LocalHost, port)) {
        delete m_server; m_server = nullptr;
        return false;                       // porta ocupada
    }
    return true;
}

void BrowserBridge::stop() {
    m_buffers.clear();
    if (m_server) { m_server->deleteLater(); m_server = nullptr; }
}

bool    BrowserBridge::listening() const { return m_server && m_server->isListening(); }
quint16 BrowserBridge::port() const      { return m_server ? m_server->serverPort() : 0; }

void BrowserBridge::onNewConnection() {
    while (QTcpSocket* sock = m_server->nextPendingConnection()) {
        connect(sock, &QTcpSocket::readyRead, this, [this, sock] { onReadyRead(sock); });
        connect(sock, &QTcpSocket::disconnected, this, [this, sock] {
            m_buffers.remove(sock); sock->deleteLater();
        });
    }
}

void BrowserBridge::onReadyRead(QTcpSocket* sock) {
    QByteArray& buf = m_buffers[sock];
    buf += sock->readAll();
    if (buf.size() > kMaxRequestBytes) {                 // guarda de tamanho
        sendJson(sock, 413, "Payload Too Large", R"({"ok":false,"error":"too_large"})");
        return;
    }
    const AddRequest req = parseAddRequest(buf);
    if (!req.headersComplete) return;                    // aguarda mais bytes
    if (req.method == "OPTIONS") { sendPreflight(sock); return; }
    if (req.method != "POST" || req.path != "/add") {
        sendJson(sock, 404, "Not Found", R"({"ok":false,"error":"not_found"})"); return;
    }
    if (!req.bodyComplete) return;                       // aguarda o corpo inteiro

    switch (authorize(req, m_token, m_allowedOrigin)) {
        case AuthResult::Forbidden:
            sendJson(sock, 403, "Forbidden", R"({"ok":false,"error":"forbidden"})"); return;
        case AuthResult::Unauthorized:
            sendJson(sock, 401, "Unauthorized", R"({"ok":false,"error":"unauthorized"})"); return;
        case AuthResult::Ok: break;
    }
    const DownloadPayload p = parseBody(req.body);
    if (!p.valid) {
        sendJson(sock, 400, "Bad Request", R"({"ok":false,"error":"bad_request"})"); return;
    }
    emit downloadRequested(p.url, headersFromPayload(p), p.filename);
    sendJson(sock, 200, "OK", R"({"ok":true})");
}

QByteArray BrowserBridge::corsBlock() const {
    return "Access-Control-Allow-Origin: " + m_allowedOrigin.toUtf8() + "\r\n"
           "Access-Control-Allow-Methods: POST, OPTIONS\r\n"
           "Access-Control-Allow-Headers: Content-Type, X-Orbit-Token\r\n"
           "Access-Control-Allow-Private-Network: true\r\n"
           "Access-Control-Max-Age: 600\r\nVary: Origin\r\n";
}

void BrowserBridge::sendPreflight(QTcpSocket* sock) {
    sock->write("HTTP/1.1 204 No Content\r\n" + corsBlock() + "Content-Length: 0\r\n\r\n");
    sock->flush();
    m_buffers.remove(sock);
    sock->disconnectFromHost();
}

void BrowserBridge::sendJson(QTcpSocket* sock, int status, const char* reason,
                             const QByteArray& body) {
    sock->write("HTTP/1.1 " + QByteArray::number(status) + " " + reason + "\r\n"
                + corsBlock() + "Content-Type: application/json\r\n"
                "Content-Length: " + QByteArray::number(body.size()) + "\r\n\r\n" + body);
    sock->flush();
    m_buffers.remove(sock);
    sock->disconnectFromHost();
}
```

- [ ] **Step 5: CMake** — adicionar `BrowserBridge.cpp` à lib `orbitgui_logic` (mesma lista da Task 5).

- [ ] **Step 6: Rodar e ver passar**

Run: `cmake -S . -B build -DCMAKE_PREFIX_PATH=/opt/homebrew && cmake --build build --target tst_browserbridge && ctest --test-dir build -R tst_browserbridge --output-on-failure`
Expected: PASS (unit + os 3 de integração).

- [ ] **Step 7: Commit**

```bash
git add src/gui/BrowserBridge.h src/gui/BrowserBridge.cpp src/gui/CMakeLists.txt tests/tst_browserbridge.cpp
git commit -m "feat(gui): loopback QTcpServer bridge emitting downloadRequested"
```

---

## Task 7: GUI — seção Browser nas Preferences

**Files:**
- Modify: `src/gui/PreferencesDialog.h`, `src/gui/PreferencesDialog.cpp`
- Test: `tests/tst_gui.cpp`

**Interfaces:**
- Consumes: `AppSettings::browser` (Task 4), `generateBridgeToken` (Task 5).
- Produces: a `PreferencesDialog` lê/escreve `m_base.browser`; hook de teste `setBrowserEnabledForTest(bool)` e `browserResultForTest()`.

- [ ] **Step 1: Teste que falha** — em `tests/tst_gui.cpp`:

```cpp
void preferencesRoundTripsBrowserPrefs() {
    AppSettings in;
    in.browser.enabled = false;
    in.browser.port    = 8697;
    PreferencesDialog dlg(in);
    dlg.setBrowserEnabledForTest(true);        // liga → deve gerar token não-vazio
    const AppSettings out = dlg.result();
    QVERIFY(out.browser.enabled);
    QCOMPARE(out.browser.port, quint16(8697));
    QVERIFY(!out.browser.token.isEmpty());     // gerado ao habilitar
}
```

- [ ] **Step 2: Rodar e ver falhar**

Run: `cmake --build build --target tst_gui && ctest --test-dir build -R tst_gui --output-on-failure`
Expected: FALHA (hook/seção inexistentes).

- [ ] **Step 3: Header** — em `PreferencesDialog.h`, membros e hook:

```cpp
    void setBrowserEnabledForTest(bool on);
    // ...
    QCheckBox* m_brEnabled = nullptr;
    QSpinBox*  m_brPort    = nullptr;
    QLineEdit* m_brToken   = nullptr;   // read-only
    QString    m_brTokenValue;          // token atual (persistido no result())
```
Adicione `class QCheckBox;` aos forward-declares.

- [ ] **Step 4: Construir a seção** — em `PreferencesDialog.cpp` (no ctor, uma nova aba/`QGroupBox` "Browser"), criar checkbox/port/token(read-only) + botões **Copy**/**Regenerate** + um `QLabel` de dica estática (ex.: `tr("The app listens on 127.0.0.1:<port>. Paste this token into the browser extension's options.")`). Inicializar de `current.browser`. Lógica:
  - ao **habilitar** com token vazio, ou clicar **Regenerate**: `m_brTokenValue = generateBridgeToken(); m_brToken->setText(m_brTokenValue);`
  - **Copy**: `QApplication::clipboard()->setText(m_brTokenValue);`
  - `m_base.browser` preserva o token vindo do `current` quando já existir.
  Em `result()`, preencher:

```cpp
    r.browser.enabled = m_brEnabled->isChecked();
    r.browser.port    = quint16(m_brPort->value());
    r.browser.token   = m_brTokenValue;
```
E o hook:

```cpp
void PreferencesDialog::setBrowserEnabledForTest(bool on) {
    m_brEnabled->setChecked(on);
    if (on && m_brTokenValue.isEmpty()) {           // mesmo caminho do toggle real
        m_brTokenValue = generateBridgeToken();
        m_brToken->setText(m_brTokenValue);
    }
}
```
Ligue o `toggled` do checkbox à mesma geração de token, para o caminho de UI real.

- [ ] **Step 5: Rodar e ver passar**

Run: `cmake --build build --target tst_gui && ctest --test-dir build -R tst_gui --output-on-failure`
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add src/gui/PreferencesDialog.h src/gui/PreferencesDialog.cpp tests/tst_gui.cpp
git commit -m "feat(gui): Browser section in Preferences with token generation"
```

---

## Task 8: GUI — ligar o bridge na `MainWindow`

**Files:**
- Modify: `src/gui/MainWindow.h`, `src/gui/MainWindow.cpp`
- Test: `tests/tst_gui.cpp`

**Interfaces:**
- Consumes: `BrowserBridge` (Task 6), `AppSettings::browser` (Task 4), `kExtensionOrigin` (Task 5), `DownloadManager::addDownload(..., headers)` (Task 2).
- Produces: `MainWindow` cria/religa o bridge em `applySettings`; slot `onBrowserDownload(url, headers, filename)`; hook `emitBrowserDownloadForTest(url, headers, filename)`.

- [ ] **Step 1: Teste que falha** — em `tests/tst_gui.cpp`:

```cpp
void browserDownloadEnqueuesWithHeaders() {
    DownloadManager mgr(EngineConfig{}, m_dir.path());   // padrão do arquivo
    DownloadTableModel model(&mgr);
    MainWindow w(&mgr, &model, nullptr);
    const int before = model.rowCount();
    const HeaderList h = {{QByteArray("Cookie"), QByteArray("k=v")}};
    w.emitBrowserDownloadForTest(QUrl("https://h/big.iso"), h, "big.iso");
    QCOMPARE(model.rowCount(), before + 1);
    // o header foi parar na task criada
    const auto tasks = mgr.tasks();
    QVERIFY(!tasks.isEmpty());
    QVERIFY(tasks.last()->record().extraHeaders.contains(
        {QByteArray("Cookie"), QByteArray("k=v")}));
}
```

- [ ] **Step 2: Rodar e ver falhar**

Run: `cmake --build build --target tst_gui && ctest --test-dir build -R tst_gui --output-on-failure`
Expected: FALHA (hook/slot inexistentes).

- [ ] **Step 3: Header** — em `MainWindow.h`: `#include "DownloadTypes.h"`, forward `class BrowserBridge;`, e:

```cpp
    void emitBrowserDownloadForTest(const QUrl& url, const HeaderList& headers,
                                    const QString& filename) { onBrowserDownload(url, headers, filename); }
private slots:
    void onBrowserDownload(const QUrl& url, const HeaderList& headers, const QString& filename);
private:
    void applyBrowserBridge(const BrowserPrefs& b);
    BrowserBridge* m_bridge = nullptr;
```

- [ ] **Step 4: Implementar** — em `MainWindow.cpp` (`#include "BrowserBridge.h"`, `#include "UrlName.h"`):

```cpp
void MainWindow::onBrowserDownload(const QUrl& url, const HeaderList& headers,
                                   const QString& filename) {
    const QString name = filename.isEmpty() ? deriveFileName(url) : filename;
    const QString dest = QDir(defaultDir()).filePath(name);
    const QUuid id = m_mgr->addDownload(url, dest, headers);
    if (id.isNull()) return;                     // esquema não suportado
    m_model->appendTask(m_mgr->taskById(id));
    if (m_tray)
        m_tray->showMessage(tr("Orbit"), tr("New download from browser: %1").arg(name),
                            QSystemTrayIcon::Information, 5000);
}

void MainWindow::applyBrowserBridge(const BrowserPrefs& b) {
    if (!m_bridge) {
        m_bridge = new BrowserBridge(this);
        connect(m_bridge, &BrowserBridge::downloadRequested,
                this, &MainWindow::onBrowserDownload);
    }
    m_bridge->stop();
    if (b.enabled && !b.token.isEmpty()) {
        if (!m_bridge->start(b.port, b.token, kExtensionOrigin) && m_tray) {
            // porta ocupada (spec §7): não trava; avisa e segue
            m_tray->showMessage(tr("Orbit"),
                tr("Browser bridge could not bind port %1 (in use).").arg(b.port),
                QSystemTrayIcon::Warning, 5000);
        }
    }
}
```
`#include "BrowserBridgeProtocol.h"` para `kExtensionOrigin`. Chamar `applyBrowserBridge(m_settings.browser);` no fim de `applySettings(...)`.

- [ ] **Step 5: Rodar e ver passar (+ suíte)**

Run: `cmake --build build --target tst_gui && ctest --test-dir build --output-on-failure`
Expected: PASS no caso novo e suíte inteira verde.

- [ ] **Step 6: Commit**

```bash
git add src/gui/MainWindow.h src/gui/MainWindow.cpp tests/tst_gui.cpp
git commit -m "feat(gui): wire the browser bridge into MainWindow with tray notification"
```

---

## Task 9: Extensão Chrome/Chromium (MV3)

**Files:**
- Create: `extension/chrome/manifest.json`, `background.js`, `options.html`, `options.js`, `README.md`

**Interfaces:**
- Consumes: o endpoint `POST /add` do `BrowserBridge` (Task 6) e o ID/`key` da Task 5 Step 1.
- Produces: extensão carregável unpacked que intercepta downloads e faz `POST` ao app.

> Não há teste C++ para a extensão (é JS). A verificação é o **E2E manual** (Task 10 / spec §11.4). Cada arquivo abaixo é conteúdo completo, sem placeholders além do `key`/porta que vêm da Task 5.

- [ ] **Step 1: `manifest.json`** (troque `<KEY_BASE64>` pela `key` da Task 5 Step 1):

```json
{
  "manifest_version": 3,
  "name": "Orbit Downloader Tribute",
  "version": "1.0",
  "description": "Send browser downloads to the Orbit Downloader Tribute app.",
  "key": "<KEY_BASE64>",
  "permissions": ["downloads", "cookies", "notifications", "storage"],
  "host_permissions": ["<all_urls>"],
  "background": { "service_worker": "background.js" },
  "options_page": "options.html"
}
```

- [ ] **Step 2: `background.js`**:

```javascript
const DEFAULTS = { enabled: false, port: 8697, token: "" };

function cfg() { return chrome.storage.local.get(DEFAULTS); }

function shouldIntercept(url, enabled, token) {
  if (!enabled || !token) return false;
  return /^https?:\/\//i.test(url);   // http/https só; blob/data/filesystem caem fora
}

async function cookieHeader(url) {
  try {
    const cookies = await chrome.cookies.getAll({ url });
    return cookies.map(c => `${c.name}=${c.value}`).join("; ");
  } catch (e) { return ""; }
}

function notify(msg) {
  chrome.notifications.create({ type: "basic", iconUrl: "options.html",
    title: "Orbit", message: msg });
}

chrome.downloads.onCreated.addListener(async (item) => {
  const c = await cfg();
  const url = item.finalUrl || item.url;
  if (!shouldIntercept(url, c.enabled, c.token)) return;

  const payload = {
    url,
    filename: item.filename || "",
    referrer: item.referrer || "",
    userAgent: navigator.userAgent,
    cookie: await cookieHeader(url),
  };
  try {
    const resp = await fetch(`http://127.0.0.1:${c.port}/add`, {
      method: "POST",
      headers: { "Content-Type": "application/json", "X-Orbit-Token": c.token },
      body: JSON.stringify(payload),
    });
    if (resp.ok) {
      await chrome.downloads.cancel(item.id);
      await chrome.downloads.erase({ id: item.id });
      notify("Sent to Orbit");
    } else {
      notify("Orbit refused the download — browser will handle it");
    }
  } catch (e) {
    // app desligado: NÃO cancela; o navegador conclui normalmente
    notify("Orbit not reachable — browser will handle it");
  }
});
```

- [ ] **Step 3: `options.html`**:

```html
<!doctype html>
<meta charset="utf-8">
<title>Orbit Tribute — Options</title>
<body>
  <h1>Orbit Downloader Tribute</h1>
  <label><input type="checkbox" id="enabled"> Intercept browser downloads</label><br>
  <label>Port: <input type="number" id="port" min="1" max="65535"></label><br>
  <label>Token: <input type="text" id="token" size="40" placeholder="paste from app Preferences"></label><br>
  <button id="save">Save</button>
  <span id="status"></span>
  <script src="options.js"></script>
</body>
```

- [ ] **Step 4: `options.js`**:

```javascript
const DEFAULTS = { enabled: false, port: 8697, token: "" };
const $ = id => document.getElementById(id);

chrome.storage.local.get(DEFAULTS).then(c => {
  $("enabled").checked = c.enabled;
  $("port").value = c.port;
  $("token").value = c.token;
});

$("save").addEventListener("click", async () => {
  await chrome.storage.local.set({
    enabled: $("enabled").checked,
    port: parseInt($("port").value, 10) || 8697,
    token: $("token").value.trim(),
  });
  $("status").textContent = "Saved.";
  setTimeout(() => ($("status").textContent = ""), 1500);
});
```

- [ ] **Step 5: `extension/chrome/README.md`** — instruções de carregar unpacked:

```markdown
# Orbit Tribute — Chrome extension

1. Open `chrome://extensions`, enable **Developer mode**.
2. **Load unpacked** → select this `extension/chrome/` folder.
3. In the Orbit app: Preferences → Browser → enable, copy the **token**.
4. In the extension **Options**: paste the token, confirm the **port** (default 8697), tick **Intercept**, Save.
5. Download anything → it should appear in the Orbit app instead of the browser.
```

- [ ] **Step 6: Commit**

```bash
git add extension/chrome
git commit -m "feat(extension): MV3 Chrome extension intercepting downloads to the app"
```

---

## Task 10: Docs — README + passos de E2E manual

**Files:**
- Modify: `README.md`

**Interfaces:**
- Consumes: tudo acima.

- [ ] **Step 1: Seção "Browser integration"** — em `README.md`, adicionar uma seção explicando: (a) habilitar em Preferences → Browser e copiar o token; (b) carregar a extensão unpacked (apontar para `extension/chrome/README.md`); (c) que o endpoint é loopback-only e protegido por token.

- [ ] **Step 2: Checklist de E2E manual** (§11.4 da spec) — adicionar ao README:

```markdown
### Browser integration — manual E2E
1. Load the extension unpacked; enable it; paste token + port.
2. Download a public file → appears and progresses in the app; tray notification.
3. Download a login-gated file (with a session cookie) → works in the app.
4. Quit the app → the browser completes the download + shows "Orbit not reachable".
5. A `blob:`/preview download → browser handles it, app does not interfere.
```

- [ ] **Step 3: Rodar a suíte inteira uma última vez (gate final)**

Run: `cmake -S . -B build -DCMAKE_PREFIX_PATH=/opt/homebrew && cmake --build build && ctest --test-dir build --output-on-failure`
Expected: TODOS os testes verdes (incluindo `tst_browserbridge` novo e `tst_download` sem regressão).

- [ ] **Step 4: Commit**

```bash
git add README.md
git commit -m "docs: document browser integration and its manual E2E checklist"
```

---

## Notas de implementação (para quem executa)

- **Ordem importa:** Task 5 Step 1 gera o **ID da extensão**, consumido como `kExtensionOrigin` (Task 5/8) e como `key` no `manifest.json` (Task 9). Não pule.
- **`item.finalUrl`** (pós-redirect) é a URL certa para buscar e para casar com os cookies — não use `item.url` quando `finalUrl` existir.
- **`setRawHeader` substitui** o valor do mesmo header: por isso o `User-Agent` do browser vence o default sem duplicar.
- **App desligado = sem perda:** a extensão só cancela o download do navegador depois de um `200`. Qualquer falha deixa o navegador concluir.
- **Modelo de ameaça:** o token (não o `Origin`) é a credencial. Um processo local do mesmo usuário está fora do modelo (spec §10).
- **Desvio consciente do §8.1:** o `PreferencesDialog` é modal e não é dono do `BrowserBridge`, então mostra um **rótulo estático** de dica (porta/token). O **status vivo de bind** (sucesso/porta-ocupada) é surfado pela `MainWindow` via bandeja em `applyBrowserBridge` (Task 8), em vez de um rótulo dinâmico no diálogo.
