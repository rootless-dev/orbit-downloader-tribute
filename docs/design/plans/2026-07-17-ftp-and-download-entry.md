# Fase 3 — Entrada de downloads + FTP: Implementation Plan

**Goal:** Abstrair o transporte do Core, entregar FTP multi-segmento sobre `QTcpSocket` com resume e
autenticação, e alimentar a fila pelos gestos do Orbit clássico (clipboard, drag & drop, New).

**Architecture:** O `DownloadTask` deixa de conhecer `QNetworkAccessManager` e passa a receber um
`Transport*` — interface dona do probe e do worker de segmento. `HttpTransport` embrulha o que já
existe; `FtpTransport` traz `FtpProbe` + `FtpSegmentWorker`, ambos sobre um `FtpControlChannel`
compartilhado. A GUI ganha entrada de URLs (clipboard/drop/New) e o diálogo de credenciais.

**Tech Stack:** C++20, Qt 6.11 (Core, Network, Widgets, Test, HttpServer), CMake, QtTest.

**Spec:** `docs/design/specs/2026-07-17-ftp-and-download-entry-design.md`

## Global Constraints

- **Commits:** Conventional Commits, mensagem em inglês, **sem** coautoria.
- C++20. Qt 6.11 via Homebrew (`/opt/homebrew`).
- `orbitcore` **não pode** depender de QtWidgets — só `Qt6::Core` e `Qt6::Network`.
- Configurar build: `cmake -S . -B build -DCMAKE_PREFIX_PATH=/opt/homebrew`
- Build: `cmake --build build -j`
- Suíte inteira: `ctest --test-dir build --output-on-failure`
- **Zero warnings novos** nos arquivos tocados.
- Testes 100% offline. Nenhum teste pode tocar a rede real.
- **Gate permanente:** `tst_download` (27 casos) passa **sem mudança de expectativa**. Só ajuste
  mecânico de construtor é permitido. Mudou expectativa = a refatoração quebrou algo, pare e reporte.
- As contagens de casos em "Expected: PASS, N casos" são **indicativas**. O que vale é: todos verdes,
  e nenhum caso pré-existente mudou de expectativa. Não force um número.

---

## Estrutura de arquivos

| Arquivo | Responsabilidade | Tarefa |
|---|---|---|
| `src/core/Transport.h` | `Credentials`, `FailureKind`, `Probe`, `SegmentSource`, `Transport` | 1 |
| `src/core/HttpTransport.{h,cpp}` | `HttpTransport` (dono do `QNAM`) | 1 |
| `src/core/HttpProbe.{h,cpp}` | passa a implementar `Probe` | 1 |
| `src/core/SegmentWorker.{h,cpp}` | passa a implementar `SegmentSource`; `FailureKind` no `failed` | 1 |
| `src/core/DownloadTask.{h,cpp}` | `Transport*` no lugar de `QNAM*` | 1 |
| `src/core/DownloadManager.{h,cpp}` | registry `scheme → Transport` | 1, 2 |
| `tests/FakeTransport.{h,cpp}` | transport sintético sem rede | 2 |
| `tests/tst_transport.cpp` | critérios 1, 2 | 2 |
| `src/core/DownloadTask.cpp` | mitigação §9.1 (`deleteLater` + `m_restarting`) | 3 |
| `src/core/FtpReply.{h,cpp}` | `parseReply`, `parsePasv`, `parseMdtm` (puros) | 4 |
| `tests/tst_ftp.cpp` | critérios 4–13 | 4–10 |
| `tests/TestFtpServer.{h,cpp}` | servidor FTP mínimo com knobs | 5 |
| `src/core/FtpControlChannel.{h,cpp}` | socket de controle: conectar, login, comando→resposta | 6 |
| `src/core/FtpProbe.{h,cpp}` | `SIZE`/`MDTM`/`REST 1` → `ProbeResult` | 7 |
| `src/core/FtpTransport.{h,cpp}` | fábrica FTP | 7 |
| `src/core/FtpSegmentWorker.{h,cpp}` | `PASV`/`REST`/`RETR`, corte no `end`, `MDTM`, auth | 8, 9, 10 |
| `src/gui/NewDownloadDialog.{h,cpp}` | `isValidDownloadUrl`, rótulo de tipo | 11 |
| `src/gui/DropTargets.{h,cpp}` | `extractUrls` (puro) | 12 |
| `src/gui/ClipboardWatcher.{h,cpp}` | `shouldOffer` (puro) + monitor | 13 |
| `src/gui/CredentialsDialog.{h,cpp}` | diálogo user/senha | 14 |
| `src/gui/MainWindow.{h,cpp}` | drop, menu Tools, diálogo de credenciais | 12, 13, 14 |

**Refinamento sobre a spec:** a spec §4 lista `FtpProbe` e `FtpSegmentWorker` como peças
independentes, mas ambas precisam de conectar + `USER`/`PASS` + `TYPE I` + parsing de resposta. O
`FtpControlChannel` (Tarefa 6) existe para não duplicar isso duas vezes.

---

## Task 1: Abstração `Transport` + migração do Core

**Files:**
- Create: `src/core/Transport.h`, `src/core/HttpTransport.h`, `src/core/HttpTransport.cpp`
- Modify: `src/core/HttpProbe.h`, `src/core/HttpProbe.cpp`, `src/core/SegmentWorker.h`,
  `src/core/SegmentWorker.cpp`, `src/core/DownloadTask.h`, `src/core/DownloadTask.cpp`,
  `src/core/DownloadManager.h`, `src/core/DownloadManager.cpp`, `src/core/CMakeLists.txt`
- Test: `tests/tst_download.cpp` (só ajuste mecânico de construtor)

**Interfaces:**
- Consumes: nada (primeira tarefa)
- Produces: `Credentials{QString user, pass}`, `enum class FailureKind{Fatal, AuthRequired}`,
  `class Probe : QObject` com `virtual void start(const QUrl&, const Credentials&)` e sinal
  `finished(const ProbeResult&)`; `class SegmentSource : QObject` com
  `virtual void start(const Segment&, const QUrl&, const QString& validator, const Credentials&)`,
  `virtual void stop()`, `virtual Segment segment() const` e sinais
  `progressed(int,qint64)`/`completed(int)`/`failed(int, const QString&, FailureKind)`/`restartRequired(int)`;
  `class Transport` com `createProbe(QObject*)` e `createWorker(QFile*, const EngineConfig&, QObject*)`;
  `HttpTransport` (construtor `HttpTransport(QObject* namParent)`);
  `DownloadTask(Transport*, const EngineConfig&, QObject*)`;
  `DownloadManager::transportFor(const QUrl&) const → Transport*`.

**Contexto para quem implementa:** `DownloadTask` hoje faz `new HttpProbe(m_nam, this)` em
`start()` (`DownloadTask.cpp:34`) e `new SegmentWorker(m_nam, m_file, m_cfg, this)` em
`spawnWorker()` (`DownloadTask.cpp:118`). `DownloadManager` cria o `QNAM` no construtor
(`DownloadManager.cpp:9`) e o passa em `addDownload` (`:30`) e `loadSession` (`:178`). Esta tarefa
troca **só isso** — nenhuma lógica muda.

- [ ] **Step 1: Criar `src/core/Transport.h`**

```cpp
#pragma once
#include "DownloadTypes.h"
#include <QObject>
#include <QString>
#include <QUrl>
class QFile;

// Credenciais de protocolo. Vazio = anônimo. Trafega por parâmetro (nunca no
// Transport, que é compartilhado por todas as tasks) porque credenciais vindas
// do diálogo vivem só em memória, na DownloadTask. HttpTransport ignora por ora.
struct Credentials {
    QString user;
    QString pass;
    bool isEmpty() const { return user.isEmpty() && pass.isEmpty(); }
};

// `failed` só é emitido quando o worker DESISTE; o retry recuperável é interno
// ao worker. Logo só existem dois desfechos que atravessam a interface: erro
// fatal, ou "pare e peça credenciais". AuthRequired existe porque no resume o
// probe não roda e o 530 chega pelo worker (spec §3.5/§3.6).
enum class FailureKind { Fatal, AuthRequired };

class Probe : public QObject {
    Q_OBJECT
public:
    explicit Probe(QObject* parent = nullptr) : QObject(parent) {}
    virtual void start(const QUrl& url, const Credentials& creds) = 0;
signals:
    void finished(const ProbeResult& result);
};

class SegmentSource : public QObject {
    Q_OBJECT
public:
    explicit SegmentSource(QObject* parent = nullptr) : QObject(parent) {}
    virtual void start(const Segment& seg, const QUrl& url,
                       const QString& validator, const Credentials& creds) = 0;
    virtual void stop() = 0;
    virtual Segment segment() const = 0;
signals:
    void progressed(int index, qint64 currentOffset);
    void completed(int index);
    void failed(int index, const QString& error, FailureKind kind);
    void restartRequired(int index);
};

// Não é QObject: sem sinais, sem filhos. Os objetos que cria recebem o parent
// passado (a DownloadTask), preservando a árvore de ownership atual.
class Transport {
public:
    virtual ~Transport() = default;
    virtual Probe*         createProbe(QObject* parent) = 0;
    virtual SegmentSource* createWorker(QFile* file, const EngineConfig& cfg, QObject* parent) = 0;
};

// Necessário p/ QSignalSpy inspecionar o argumento de `failed` nos testes.
Q_DECLARE_METATYPE(FailureKind)
```

- [ ] **Step 2: `HttpProbe` implementa `Probe`**

Em `src/core/HttpProbe.h`, trocar a declaração da classe:

```cpp
#pragma once
#include "Transport.h"
class QNetworkAccessManager;
class QNetworkReply;

class HttpProbe : public Probe {
    Q_OBJECT
public:
    explicit HttpProbe(QNetworkAccessManager* nam, QObject* parent = nullptr);
    void start(const QUrl& url, const Credentials& creds) override;
private:
    void onMetaDataChanged();
    void onErrorOccurred();
    QNetworkAccessManager* m_nam;
    QNetworkReply*         m_reply = nullptr;
    bool                   m_done  = false;
};
```

Em `src/core/HttpProbe.cpp`, ajustar construtor e assinatura:

```cpp
HttpProbe::HttpProbe(QNetworkAccessManager* nam, QObject* parent)
    : Probe(parent), m_nam(nam) {
    qRegisterMetaType<ProbeResult>("ProbeResult");
}

void HttpProbe::start(const QUrl& url, const Credentials& creds) {
    Q_UNUSED(creds);   // HTTP basic-auth não está no escopo da Fase 3
    // ...resto do corpo permanece IDÊNTICO...
}
```

O `signals: void finished(const ProbeResult&)` sai de `HttpProbe.h` — agora vem de `Probe`.

- [ ] **Step 3: `SegmentWorker` implementa `SegmentSource`**

`src/core/SegmentWorker.h`:

```cpp
#pragma once
#include "Transport.h"
#include <QNetworkReply>
class QNetworkAccessManager;
class QFile;
class QTimer;

class SegmentWorker : public SegmentSource {
    Q_OBJECT
public:
    SegmentWorker(QNetworkAccessManager* nam, QFile* file, const EngineConfig& cfg,
                  QObject* parent = nullptr);
    void start(const Segment& seg, const QUrl& url,
               const QString& validator, const Credentials& creds) override;
    void stop() override;
    Segment segment() const override { return m_seg; }
private:
    // ...membros idênticos aos atuais...
};
```

Os quatro `signals` saem do header (vêm de `SegmentSource`). Em `SegmentWorker.cpp`:

```cpp
SegmentWorker::SegmentWorker(QNetworkAccessManager* nam, QFile* file,
                             const EngineConfig& cfg, QObject* parent)
    : SegmentSource(parent), m_nam(nam), m_file(file), m_cfg(cfg) {}

void SegmentWorker::start(const Segment& seg, const QUrl& url,
                          const QString& validator, const Credentials& creds) {
    Q_UNUSED(creds);
    m_seg = seg;
    m_url = url;
    m_validator = validator;
    // ...resto idêntico...
}
```

Os três `emit failed(...)` ganham o `FailureKind`. Mapeamento **exato** do comportamento atual —
`isRecoverable()` já filtrou o que era retry, então tudo que chega em `failed` hoje é desistência:

```cpp
// SegmentWorker.cpp:69  (write error)
emit failed(m_seg.index, "write error: " + m_file->errorString(), FailureKind::Fatal);

// SegmentWorker.cpp:128 (non-recoverable network error)
// AuthenticationRequiredError vira AuthRequired; 403/404 seguem Fatal.
const bool isAuth = (err == QNetworkReply::AuthenticationRequiredError);
emit failed(m_seg.index, "non-recoverable network error: " + m_reply->errorString(),
            isAuth ? FailureKind::AuthRequired : FailureKind::Fatal);

// SegmentWorker.cpp:181 (retries exhausted)
emit failed(m_seg.index, "retries exhausted", FailureKind::Fatal);
```

- [ ] **Step 4: Criar `src/core/HttpTransport.h` e `.cpp`**

```cpp
// HttpTransport.h
#pragma once
#include "Transport.h"
class QNetworkAccessManager;
class QObject;

class HttpTransport : public Transport {
public:
    // namParent é o QObject dono do QNetworkAccessManager (tipicamente o
    // DownloadManager) — o QNAM segue na árvore de ownership do Qt, como hoje.
    explicit HttpTransport(QObject* namParent);
    Probe*         createProbe(QObject* parent) override;
    SegmentSource* createWorker(QFile* file, const EngineConfig& cfg, QObject* parent) override;
private:
    QNetworkAccessManager* m_nam;
};
```

```cpp
// HttpTransport.cpp
#include "HttpTransport.h"
#include "HttpProbe.h"
#include "SegmentWorker.h"
#include <QNetworkAccessManager>

HttpTransport::HttpTransport(QObject* namParent)
    : m_nam(new QNetworkAccessManager(namParent)) {}

Probe* HttpTransport::createProbe(QObject* parent) {
    return new HttpProbe(m_nam, parent);
}

SegmentSource* HttpTransport::createWorker(QFile* file, const EngineConfig& cfg, QObject* parent) {
    return new SegmentWorker(m_nam, file, cfg, parent);
}
```

- [ ] **Step 5: `DownloadTask` recebe `Transport*`**

`src/core/DownloadTask.h`: trocar `#include <QNetworkAccessManager>`-forward por
`#include "Transport.h"`, remover `class QNetworkAccessManager;`/`class HttpProbe;`/`class SegmentWorker;`,
e:

```cpp
DownloadTask(Transport* transport, const EngineConfig& cfg, QObject* parent = nullptr);
// ...
void onSegmentFailed(int index, const QString& error, FailureKind kind);
// membros:
Transport*              m_transport;          // era QNetworkAccessManager* m_nam
QVector<SegmentSource*> m_workers;            // era QVector<SegmentWorker*>
```

`src/core/DownloadTask.cpp`:

```cpp
DownloadTask::DownloadTask(Transport* transport, const EngineConfig& cfg, QObject* parent)
    : QObject(parent), m_transport(transport), m_cfg(cfg) {}

void DownloadTask::start() {
    setState(DownloadState::Connecting);
    if (!m_probed) {
        Probe* probe = m_transport->createProbe(this);
        connect(probe, &Probe::finished, this, [this, probe](const ProbeResult& r) {
            probe->deleteLater();
            onProbed(r);
        });
        probe->start(m_url, Credentials{});      // credenciais entram na Tarefa 10
    } else {
        beginSegments();
    }
}

void DownloadTask::spawnWorker(const Segment& seg) {
    SegmentSource* w = m_transport->createWorker(m_file, m_cfg, this);
    m_workers.append(w);
    connect(w, &SegmentSource::progressed, this, [this](int idx, qint64 cur) {
        // ...corpo IDÊNTICO ao atual...
    });
    connect(w, &SegmentSource::completed,       this, &DownloadTask::onSegmentCompleted);
    connect(w, &SegmentSource::failed,          this, &DownloadTask::onSegmentFailed);
    connect(w, &SegmentSource::restartRequired, this, &DownloadTask::onRestartRequired);
    const QString validator = m_validated ? (!m_etag.isEmpty() ? m_etag : m_lastModified) : QString();
    w->start(seg, m_url, validator, Credentials{});   // credenciais entram na Tarefa 10
}

void DownloadTask::onSegmentFailed(int index, const QString& error, FailureKind kind) {
    Q_UNUSED(index); Q_UNUSED(error); Q_UNUSED(kind);   // kind é tratado na Tarefa 10
    // ...resto IDÊNTICO ao atual...
}
```

Remover `#include "HttpProbe.h"`, `#include "SegmentWorker.h"` e `#include <QNetworkAccessManager>`.

- [ ] **Step 6: `DownloadManager` com registry**

`src/core/DownloadManager.h`:

```cpp
#pragma once
#include "DownloadTypes.h"
#include "DownloadTask.h"
#include "Transport.h"
#include <QObject>
#include <QVector>
#include <QHash>
#include <memory>

class DownloadManager : public QObject {
    Q_OBJECT
public:
    DownloadManager(const EngineConfig& cfg, const QString& dataDir, QObject* parent = nullptr);
    QUuid addDownload(const QUrl& url, const QString& destPath);
    // ...resto igual...
    Transport* transportFor(const QUrl& url) const;   // nullptr se esquema desconhecido
private:
    // ...
    // era: QNetworkAccessManager* m_nam
    QHash<QString, Transport*>              m_transports;   // scheme -> transport (não-dono)
    std::vector<std::unique_ptr<Transport>> m_owned;        // dono de verdade
};
```

Adicionar `#include <vector>` ao header.

`src/core/DownloadManager.cpp`:

Os transports são donos via `m_owned`; o `QHash` guarda ponteiros crus, então `http` e `https`
compartilham **um único** `HttpTransport` (e portanto um único `QNAM`, como hoje).

```cpp
#include "HttpTransport.h"

DownloadManager::DownloadManager(const EngineConfig& cfg, const QString& dataDir, QObject* parent)
    : QObject(parent), m_cfg(cfg), m_dataDir(dataDir) {
    auto http = std::make_unique<HttpTransport>(this);
    m_transports.insert("http",  http.get());
    m_transports.insert("https", http.get());
    m_owned.push_back(std::move(http));
    QDir().mkpath(m_dataDir);
}

Transport* DownloadManager::transportFor(const QUrl& url) const {
    return m_transports.value(url.scheme().toLower(), nullptr);
}
```

Os membros correspondentes, no header (substituem o `QHash<QString, std::unique_ptr<Transport>>`
esboçado acima):

```cpp
    QHash<QString, Transport*>              m_transports;   // scheme -> transport (não-dono)
    std::vector<std::unique_ptr<Transport>> m_owned;        // dono de verdade
```

`addDownload` e `loadSession`:

```cpp
QUuid DownloadManager::addDownload(const QUrl& url, const QString& destPath) {
    Transport* tr = transportFor(url);
    if (!tr) return QUuid();          // esquema desconhecido: nada criado (spec critério 2)
    const QString finalPath = Persistence::resolveUniquePath(destPath);
    auto* t = new DownloadTask(tr, m_cfg, this);
    // ...resto idêntico...
}

void DownloadManager::loadSession() {
    const auto recs = Persistence::readSession(sessionPath());
    for (const auto& rec : recs) {
        if (rec.state == DownloadState::Completed) continue;
        Transport* tr = transportFor(rec.url);
        if (!tr) continue;            // registro órfão de esquema desconhecido: ignora
        QVector<Segment> segs; QString etag, lm; bool validated = false;
        Persistence::readMeta(rec.destPath, segs, etag, lm, validated);
        auto* t = new DownloadTask(tr, m_cfg, this);
        // ...resto idêntico...
    }
}
```

Remover `#include <QNetworkAccessManager>` e o membro `m_nam`.

- [ ] **Step 7: Registrar os novos fontes no CMake**

`src/core/CMakeLists.txt` — adicionar às fontes de `orbitcore`:

```cmake
  Transport.h
  HttpTransport.cpp
```

- [ ] **Step 8: Ajustar `tests/tst_download.cpp` (mecânico apenas)**

Os testes que constroem `HttpProbe`, `SegmentWorker` ou `DownloadTask` direto com um `QNAM`
precisam do ajuste mínimo. Padrões:

```cpp
// HttpProbe: start() ganha o segundo parâmetro
probe.start(srv.url("/ranged"), Credentials{});

// SegmentWorker: start() ganha o quarto parâmetro
w.start(seg, srv.url("/ranged"), QString(), Credentials{});

// DownloadTask construído direto: passa a precisar de um Transport
HttpTransport tr(&nam_owner_qobject);
DownloadTask task(&tr, cfg);

// QSignalSpy sobre `failed` continua funcionando; se algum teste inspeciona
// spy.at(0).size(), agora são 3 argumentos, não 2.
```

Adicionar `#include "Transport.h"` e `#include "HttpTransport.h"` no topo do arquivo.

**Nada mais pode mudar.** Se um caso precisar de nova expectativa, PARE e reporte — significa que a
refatoração alterou comportamento.

- [ ] **Step 9: Build**

Run: `cmake --build build -j`
Expected: compila sem warnings novos.

- [ ] **Step 10: Rodar o gate**

Run: `ctest --test-dir build --output-on-failure`
Expected: **5/5 verde**, `tst_download` com 27 casos passando.

- [ ] **Step 11: Sem commit**

Deixe no working tree. Reporte: arquivos tocados, saída do `ctest`, e qualquer expectativa de teste
que você tenha precisado mudar (idealmente: nenhuma).

---

## Task 2: `FakeTransport` + testes da abstração

**Files:**
- Create: `tests/FakeTransport.h`, `tests/FakeTransport.cpp`, `tests/tst_transport.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `Transport`, `Probe`, `SegmentSource`, `Credentials`, `FailureKind` (Task 1);
  `DownloadManager::addDownload`, `DownloadManager::transportFor` (Task 1).
- Produces: `FakeTransport` com `setProbeResult(ProbeResult)`, `setBody(QByteArray)`,
  `lastCredentials() → Credentials`, `probeCount() → int` — reusado nas Tasks 3 e 10.

**Objetivo:** provar que a abstração não vaza HTTP (critério 1) e que o registry rejeita esquema
desconhecido (critério 2).

- [ ] **Step 1: Escrever `tests/FakeTransport.h`**

```cpp
#pragma once
#include "Transport.h"
#include <QByteArray>
#include <QFile>
#include <QTimer>

// Probe sintético: devolve um ProbeResult programado, assincronamente.
class FakeProbe : public Probe {
    Q_OBJECT
public:
    FakeProbe(ProbeResult r, Credentials* sink, int* count, QObject* parent = nullptr)
        : Probe(parent), m_r(r), m_sink(sink), m_count(count) {}
    void start(const QUrl& url, const Credentials& creds) override {
        Q_UNUSED(url);
        if (m_sink)  *m_sink = creds;
        if (m_count) ++(*m_count);
        QTimer::singleShot(0, this, [this] { emit finished(m_r); });
    }
private:
    ProbeResult  m_r;
    Credentials* m_sink;
    int*         m_count;
};

// Worker sintético: escreve a fatia [start, end] do corpo programado e completa.
class FakeWorker : public SegmentSource {
    Q_OBJECT
public:
    FakeWorker(QByteArray body, QFile* file, QObject* parent = nullptr)
        : SegmentSource(parent), m_body(body), m_file(file) {}
    void start(const Segment& seg, const QUrl& url,
               const QString& validator, const Credentials& creds) override {
        Q_UNUSED(url); Q_UNUSED(validator); Q_UNUSED(creds);
        m_seg = seg;
        QTimer::singleShot(0, this, [this] {
            if (m_stopped) return;
            const qint64 end = (m_seg.end >= 0) ? m_seg.end : m_body.size() - 1;
            const qint64 n   = end - m_seg.current + 1;
            if (n > 0) {
                m_file->seek(m_seg.current);
                m_file->write(m_body.constData() + m_seg.current, n);
                m_seg.current += n;
                emit progressed(m_seg.index, m_seg.current);
            }
            if (m_seg.end < 0) m_seg.end = m_seg.current - 1;
            emit completed(m_seg.index);
        });
    }
    void stop() override { m_stopped = true; }
    Segment segment() const override { return m_seg; }
private:
    QByteArray m_body;
    QFile*     m_file;
    Segment    m_seg;
    bool       m_stopped = false;
};

class FakeTransport : public Transport {
public:
    Probe*         createProbe(QObject* parent) override;
    SegmentSource* createWorker(QFile* file, const EngineConfig& cfg, QObject* parent) override;
    void setProbeResult(const ProbeResult& r) { m_probeResult = r; }
    void setBody(const QByteArray& b)         { m_body = b; }
    Credentials lastCredentials() const       { return m_lastCreds; }
    int         probeCount() const            { return m_probeCount; }
private:
    ProbeResult m_probeResult;
    QByteArray  m_body;
    Credentials m_lastCreds;
    int         m_probeCount = 0;
};
```

```cpp
// tests/FakeTransport.cpp
#include "FakeTransport.h"

Probe* FakeTransport::createProbe(QObject* parent) {
    return new FakeProbe(m_probeResult, &m_lastCreds, &m_probeCount, parent);
}

SegmentSource* FakeTransport::createWorker(QFile* file, const EngineConfig& cfg, QObject* parent) {
    Q_UNUSED(cfg);
    return new FakeWorker(m_body, file, parent);
}
```

- [ ] **Step 2: Escrever `tests/tst_transport.cpp` (testes que falham)**

```cpp
#include <QtTest>
#include <QSignalSpy>
#include <QTemporaryDir>
#include "FakeTransport.h"
#include "DownloadTask.h"
#include "DownloadManager.h"

static QByteArray makeBody(int n) {
    QByteArray b; b.resize(n);
    for (int i = 0; i < n; ++i) b[i] = char('A' + (i % 26));
    return b;
}

class TstTransport : public QObject {
    Q_OBJECT
private slots:

    // Critério 1: um Transport sem rede leva a task de Queued a Completed.
    void fakeTransportDrivesTaskToCompletion() {
        const QByteArray body = makeBody(4096);
        QTemporaryDir dir;
        const QString dest = dir.path() + "/out.bin";

        FakeTransport tr;
        tr.setBody(body);
        ProbeResult r;
        r.ok = true; r.totalBytes = body.size(); r.supportsRange = true; r.etag = "\"v1\"";
        tr.setProbeResult(r);

        EngineConfig cfg;
        DownloadTask task(&tr, cfg);
        task.init(QUuid::createUuid(), QUrl("fake://host/f.bin"), dest, 4);
        QSignalSpy spy(&task, &DownloadTask::stateChanged);
        task.start();

        QVERIFY(QTest::qWaitFor([&]{ return task.state() == DownloadState::Completed; }, 3000));
        QFile f(dest);
        QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(f.readAll(), body);
    }

    // Critério 2: esquema desconhecido -> QUuid nulo, nenhuma task criada.
    void unknownSchemeIsRejected() {
        QTemporaryDir dir;
        EngineConfig cfg;
        DownloadManager mgr(cfg, dir.path());
        const QUuid id = mgr.addDownload(QUrl("gopher://host/f.bin"), dir.path() + "/f.bin");
        QVERIFY(id.isNull());
        QCOMPARE(mgr.tasks().size(), 0);
    }

    // Critério 2: http/https resolvem; ftp ainda não (chega na Task 7).
    void registryResolvesHttpSchemes() {
        QTemporaryDir dir;
        EngineConfig cfg;
        DownloadManager mgr(cfg, dir.path());
        QVERIFY(mgr.transportFor(QUrl("http://h/f"))  != nullptr);
        QVERIFY(mgr.transportFor(QUrl("https://h/f")) != nullptr);
        QVERIFY(mgr.transportFor(QUrl("HTTP://h/f"))  != nullptr);   // case-insensitive
        QVERIFY(mgr.transportFor(QUrl("gopher://h/f")) == nullptr);
    }
};

QTEST_MAIN(TstTransport)
#include "tst_transport.moc"
```

- [ ] **Step 3: Registrar no CMake**

`tests/CMakeLists.txt`:

```cmake
add_executable(tst_transport tst_transport.cpp FakeTransport.cpp)
target_link_libraries(tst_transport PRIVATE orbitcore Qt6::Test)
add_test(NAME tst_transport COMMAND tst_transport)
```

- [ ] **Step 4: Rodar e ver falhar**

Run: `cmake --build build -j && ctest --test-dir build --output-on-failure -R tst_transport`
Expected: FALHA. `unknownSchemeIsRejected` falha se a Task 1 não tiver feito o guard de esquema.
Se **todos** passarem de primeira, ótimo — a Task 1 já os satisfaz; registre isso no relatório.

- [ ] **Step 5: Corrigir o que faltar na Task 1**

Se algum teste falhar, o defeito está no código da Task 1 (não invente API nova): tipicamente o
`if (!tr) return QUuid();` em `addDownload` ou o `.toLower()` em `transportFor`.

- [ ] **Step 6: Rodar a suíte inteira**

Run: `ctest --test-dir build --output-on-failure`
Expected: **6/6 verde**.

- [ ] **Step 7: Sem commit** — deixe no working tree e reporte.

---

## Task 3: Mitigação §9.1 — `restartRequired` não pode destruir o emissor

**Files:**
- Modify: `src/core/DownloadTask.h`, `src/core/DownloadTask.cpp:161-169`
- Test: `tests/tst_transport.cpp`

**Interfaces:**
- Consumes: `FakeTransport`/`FakeWorker` (Task 2), `SegmentSource::restartRequired` (Task 1).
- Produces: `DownloadTask::m_restarting` (guarda interna). Nenhuma API pública nova.

**O bug (spec §9.1):** `DownloadTask::onRestartRequired` faz `qDeleteAll(m_workers)`, destruindo o
worker que está **dentro do próprio `emit restartRequired(...)`** — um objeto cujo frame de pilha
ainda vai retornar. Hoje sobrevive por acidente: `SegmentWorker::onReadyRead` faz `return`
imediatamente após o emit, sem tocar em membro nenhum. A Fase 3 destrói esse acidente: cada
`FtpSegmentWorker` faz sua própria checagem de `MDTM`, então N workers emitem `restartRequired` na
mesma volta do event loop — o primeiro já apagou `m_workers`, e os demais emitem de objetos
destruídos.

Esta tarefa **toca o Core da Fase 1**. É mudança de mecanismo de destruição, não de comportamento
observável: `tst_download` continua sendo a rede de segurança e **não pode mudar de expectativa**.

- [ ] **Step 1: Escrever o teste que falha**

Adicionar a `tests/FakeTransport.h` um worker que reproduz o cenário — emite `restartRequired` e
**continua tocando membros depois do emit**, que é exatamente o que o FTP vai fazer:

```cpp
// Worker que pede restart enquanto houver validador, e baixa normalmente
// quando não houver. (Não precisa de flag "já reiniciei": onRestartRequired
// limpa o validador antes de respawnar, então a segunda rodada chega aqui com
// validator vazio — é assim que o restart é finito, spec §3.5.)
// Depois do emit, escreve em m_touched: se o objeto tiver sido destruído
// durante o emit, isto é use-after-free (ASAN pega; sem ASAN, corrompe).
class RestartingWorker : public SegmentSource {
    Q_OBJECT
public:
    RestartingWorker(QByteArray body, QFile* file, QObject* parent = nullptr)
        : SegmentSource(parent), m_body(body), m_file(file) {}
    void start(const Segment& seg, const QUrl& url,
               const QString& validator, const Credentials& creds) override {
        Q_UNUSED(url); Q_UNUSED(creds);
        m_seg = seg;
        m_askRestart = !validator.isEmpty();
        QTimer::singleShot(0, this, [this] {
            if (m_stopped) return;
            if (m_askRestart) {
                emit restartRequired(m_seg.index);
                m_touched = 42;          // <-- use-after-free se fomos destruídos no emit
                return;
            }
            const qint64 end = (m_seg.end >= 0) ? m_seg.end : m_body.size() - 1;
            const qint64 n   = end - m_seg.current + 1;
            if (n > 0) {
                m_file->seek(m_seg.current);
                m_file->write(m_body.constData() + m_seg.current, n);
                m_seg.current += n;
                emit progressed(m_seg.index, m_seg.current);
            }
            if (m_seg.end < 0) m_seg.end = m_seg.current - 1;
            emit completed(m_seg.index);
        });
    }
    void stop() override { m_stopped = true; }
    Segment segment() const override { return m_seg; }
private:
    QByteArray m_body;
    QFile*     m_file;
    Segment    m_seg;
    bool       m_askRestart = false;
    bool       m_stopped    = false;
    int        m_touched    = 0;
};
```

E um transport que os fabrica (adicionar a `FakeTransport`):

```cpp
// em FakeTransport.h, na classe FakeTransport:
    void setRestartOnce(bool on) { m_restartOnce = on; }
private:
    bool m_restartOnce = false;
```

```cpp
// em FakeTransport.cpp:
SegmentSource* FakeTransport::createWorker(QFile* file, const EngineConfig& cfg, QObject* parent) {
    Q_UNUSED(cfg);
    if (m_restartOnce) return new RestartingWorker(m_body, file, parent);
    return new FakeWorker(m_body, file, parent);
}
```

O teste, em `tests/tst_transport.cpp`:

```cpp
    // Spec §9.1: N workers emitindo restartRequired na mesma volta do event
    // loop. O primeiro dispara onRestartRequired, que hoje faz qDeleteAll() —
    // destruindo os outros (e o próprio emissor) durante a emissão.
    void simultaneousRestartsDoNotDestroySender() {
        const QByteArray body = makeBody(1 << 16);
        QTemporaryDir dir;
        const QString dest = dir.path() + "/out.bin";

        FakeTransport tr;
        tr.setBody(body);
        tr.setRestartOnce(true);
        ProbeResult r;
        r.ok = true; r.totalBytes = body.size(); r.supportsRange = true;
        r.lastModified = "20260717000000";        // validador não-vazio -> pede restart
        tr.setProbeResult(r);

        EngineConfig cfg;
        DownloadTask task(&tr, cfg);
        task.init(QUuid::createUuid(), QUrl("fake://host/f.bin"), dest, 4);
        task.start();

        // Sem a correção: crash / corrupção sob ASAN.
        // Com a correção: os 4 workers reiniciam e o download completa do zero.
        QVERIFY(QTest::qWaitFor([&]{ return task.state() == DownloadState::Completed; }, 5000));
        QFile f(dest);
        QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(f.readAll(), body);
    }
```

- [ ] **Step 2: Rodar e ver falhar**

Run: `cmake --build build -j && ctest --test-dir build --output-on-failure -R tst_transport`
Expected: FALHA (crash, hang, ou arquivo diferente). Para evidência forte, rode uma vez com ASAN:

```bash
cmake -S . -B build-asan -DCMAKE_PREFIX_PATH=/opt/homebrew \
      -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS="-fsanitize=address -g"
cmake --build build-asan -j --target tst_transport
./build-asan/tests/tst_transport simultaneousRestartsDoNotDestroySender
```
Expected: `heap-use-after-free`. Registre a saída no relatório — é a prova de que o bug é real.

- [ ] **Step 3: Implementar a correção**

`src/core/DownloadTask.h` — adicionar o membro:

```cpp
    bool m_restarting = false;
```

`src/core/DownloadTask.cpp` — substituir `onRestartRequired` inteiro:

```cpp
void DownloadTask::onRestartRequired(int index) {
    Q_UNUSED(index);
    // Guarda: com N segmentos, cada worker detecta a mudança do recurso por
    // conta própria (HTTP: 200 no lugar de 206; FTP: MDTM divergente, spec
    // §3.5) e todos emitem restartRequired na mesma volta do event loop. Só a
    // primeira emissão vale; as seguintes chegariam com m_workers já esvaziado.
    if (m_restarting) return;
    m_restarting = true;

    // deleteLater(), nunca delete/qDeleteAll: o emissor deste sinal está entre
    // os workers e seu frame de pilha ainda vai retornar. Destruí-lo aqui é
    // use-after-free (spec §9.1). stop() já o torna inerte imediatamente; a
    // destruição fica para o retorno ao event loop.
    for (auto* w : m_workers) { w->stop(); w->deleteLater(); }
    m_workers.clear();

    for (auto& s : m_segments) s.current = s.start;   // reset to zero
    m_validated = false;                               // don't send If-Range again
    m_etag.clear(); m_lastModified.clear();
    beginSegments();
    m_restarting = false;
}
```

> **Por que `m_restarting = false` no fim e não uma flag permanente:** um segundo restart legítimo
> (o recurso muda de novo, mais tarde) precisa funcionar. A guarda cobre a rodada, não a vida da task.
> `beginSegments()` limpa o validador antes de respawnar, então os workers da segunda rodada não
> pedem restart de novo — o restart é sempre finito (spec §3.5).

- [ ] **Step 4: Rodar e ver passar**

Run: `ctest --test-dir build --output-on-failure -R tst_transport`
Expected: PASS, 4 casos.

Run (ASAN): `cmake --build build-asan -j --target tst_transport && ./build-asan/tests/tst_transport`
Expected: sem `heap-use-after-free`.

- [ ] **Step 5: Rodar o gate de regressão**

Run: `ctest --test-dir build --output-on-failure`
Expected: **6/6 verde**, `tst_download` 27 casos **sem mudança de expectativa**.

- [ ] **Step 6: Sem commit** — deixe no working tree. Reporte a saída do ASAN antes e depois.

---

## Task 4: `FtpReply` — parsers puros

**Files:**
- Create: `src/core/FtpReply.h`, `src/core/FtpReply.cpp`, `tests/tst_ftp.cpp`
- Modify: `src/core/CMakeLists.txt`, `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: nada.
- Produces:
  - `struct FtpReply { int code = 0; QString text; bool complete = false; }`
  - `FtpReply parseReply(const QByteArray& buf, int* consumed)` — `complete=false` se o buffer ainda
    não tem uma resposta inteira; `*consumed` = bytes a descartar do buffer quando `complete`.
  - `std::optional<QPair<QString, quint16>> parsePasv(const QString& line)`
  - `std::optional<QDateTime> parseMdtm(const QString& line)`

**Contexto FTP (RFC 959) para quem não conhece o protocolo:** uma resposta é `NNN <texto>\r\n`. Se o
4º caractere for `-` em vez de espaço (`213-Info`), a resposta é **multi-linha** e só termina quando
aparecer uma linha começando com o **mesmo código** seguido de **espaço**. Exemplo:

```
213-Status follows\r\n
 file details here\r\n
213 End of status\r\n
```

- [ ] **Step 1: Escrever `tests/tst_ftp.cpp` (testes que falham)**

```cpp
#include <QtTest>
#include "FtpReply.h"

class TstFtp : public QObject {
    Q_OBJECT
private slots:

    // --- parseReply -----------------------------------------------------
    void parseSingleLineReply() {
        int consumed = 0;
        const FtpReply r = parseReply("220 Welcome\r\n", &consumed);
        QVERIFY(r.complete);
        QCOMPARE(r.code, 220);
        QCOMPARE(r.text, QString("Welcome"));
        QCOMPARE(consumed, 13);
    }

    void parseIncompleteReplyIsNotComplete() {
        int consumed = 0;
        const FtpReply r = parseReply("220 Welco", &consumed);
        QVERIFY(!r.complete);
        QCOMPARE(consumed, 0);
    }

    void parseMultiLineReply() {
        int consumed = 0;
        const QByteArray buf = "213-Status follows\r\n details\r\n213 Done\r\n";
        const FtpReply r = parseReply(buf, &consumed);
        QVERIFY(r.complete);
        QCOMPARE(r.code, 213);
        QCOMPARE(r.text, QString("Done"));
        QCOMPARE(consumed, buf.size());
    }

    void parseMultiLineReplyIncompleteWaitsForTerminator() {
        int consumed = 0;
        const FtpReply r = parseReply("213-Status follows\r\n details\r\n", &consumed);
        QVERIFY(!r.complete);
        QCOMPARE(consumed, 0);
    }

    // Uma linha interna que começa com OUTRO código não termina a resposta.
    void parseMultiLineIgnoresForeignCode() {
        int consumed = 0;
        const QByteArray buf = "213-Start\r\n226 not the terminator\r\n213 End\r\n";
        const FtpReply r = parseReply(buf, &consumed);
        QVERIFY(r.complete);
        QCOMPARE(r.code, 213);
        QCOMPARE(r.text, QString("End"));
        QCOMPARE(consumed, buf.size());
    }

    void parseReplyLeavesTrailingBytes() {
        int consumed = 0;
        const FtpReply r = parseReply("220 Hi\r\n331 Next\r\n", &consumed);
        QVERIFY(r.complete);
        QCOMPARE(r.code, 220);
        QCOMPARE(consumed, 8);            // só a primeira resposta é consumida
    }

    // --- parsePasv ------------------------------------------------------
    void parsePasvCanonical() {
        const auto r = parsePasv("227 Entering Passive Mode (127,0,0,1,195,80).");
        QVERIFY(r.has_value());
        QCOMPARE(r->first, QString("127.0.0.1"));
        QCOMPARE(r->second, quint16(195 * 256 + 80));
    }

    void parsePasvWithoutTrailingDot() {
        const auto r = parsePasv("227 Entering Passive Mode (10,0,0,7,4,1)");
        QVERIFY(r.has_value());
        QCOMPARE(r->first, QString("10.0.0.7"));
        QCOMPARE(r->second, quint16(4 * 256 + 1));
    }

    void parsePasvMalformedReturnsNullopt() {
        QVERIFY(!parsePasv("227 Entering Passive Mode").has_value());
        QVERIFY(!parsePasv("227 (1,2,3)").has_value());
        QVERIFY(!parsePasv("227 (1,2,3,4,5,6,7)").has_value());
        QVERIFY(!parsePasv("227 (1,2,3,4,999,80)").has_value());   // octeto > 255
    }

    // --- parseMdtm ------------------------------------------------------
    void parseMdtmCanonical() {
        const auto r = parseMdtm("213 20260717123045");
        QVERIFY(r.has_value());
        QCOMPARE(r->toUTC(), QDateTime(QDate(2026, 7, 17), QTime(12, 30, 45), QTimeZone::UTC));
    }

    void parseMdtmMalformedReturnsNullopt() {
        QVERIFY(!parseMdtm("213 nonsense").has_value());
        QVERIFY(!parseMdtm("550 Not found").has_value());
        QVERIFY(!parseMdtm("213 2026071712").has_value());   // curto demais
    }
};

QTEST_MAIN(TstFtp)
#include "tst_ftp.moc"
```

- [ ] **Step 2: Registrar no CMake e ver falhar**

`tests/CMakeLists.txt`:

```cmake
add_executable(tst_ftp tst_ftp.cpp)
target_link_libraries(tst_ftp PRIVATE orbitcore Qt6::Test)
add_test(NAME tst_ftp COMMAND tst_ftp)
```

Run: `cmake --build build -j`
Expected: FALHA — `FtpReply.h: No such file or directory`.

- [ ] **Step 3: Implementar `src/core/FtpReply.h`**

```cpp
#pragma once
#include <QByteArray>
#include <QDateTime>
#include <QPair>
#include <QString>
#include <optional>

// Uma resposta FTP (RFC 959): "NNN <texto>\r\n", ou multi-linha
// "NNN-<texto>\r\n ... \r\nNNN <texto>\r\n".
struct FtpReply {
    int     code     = 0;
    QString text;               // texto da linha FINAL
    bool    complete = false;   // false = o buffer ainda não tem uma resposta inteira
};

// Extrai a primeira resposta completa de `buf`. Quando complete, *consumed é o
// número de bytes a descartar do buffer; quando não, *consumed = 0 e o chamador
// espera mais dados.
FtpReply parseReply(const QByteArray& buf, int* consumed);

// "227 Entering Passive Mode (h1,h2,h3,h4,p1,p2)" -> ("h1.h2.h3.h4", p1*256+p2)
std::optional<QPair<QString, quint16>> parsePasv(const QString& line);

// "213 YYYYMMDDHHMMSS" -> QDateTime (UTC)
std::optional<QDateTime> parseMdtm(const QString& line);
```

- [ ] **Step 4: Implementar `src/core/FtpReply.cpp`**

```cpp
#include "FtpReply.h"
#include <QRegularExpression>
#include <QStringList>

static bool isCodeLine(const QByteArray& line, int code, char sep) {
    // "NNN<sep>..." com NNN == code
    if (line.size() < 4) return false;
    bool ok = false;
    const int c = line.left(3).toInt(&ok);
    return ok && c == code && line[3] == sep;
}

FtpReply parseReply(const QByteArray& buf, int* consumed) {
    if (consumed) *consumed = 0;
    FtpReply r;

    const int firstEol = buf.indexOf("\r\n");
    if (firstEol < 0) return r;                 // nem uma linha inteira ainda
    const QByteArray firstLine = buf.left(firstEol);
    if (firstLine.size() < 4) return r;

    bool ok = false;
    const int code = firstLine.left(3).toInt(&ok);
    if (!ok) return r;

    // Resposta de uma linha: "NNN texto"
    if (firstLine[3] == ' ') {
        r.code     = code;
        r.text     = QString::fromUtf8(firstLine.mid(4)).trimmed();
        r.complete = true;
        if (consumed) *consumed = firstEol + 2;
        return r;
    }

    // Multi-linha: "NNN-..." termina numa linha "NNN <texto>"
    if (firstLine[3] != '-') return r;
    int pos = firstEol + 2;
    while (true) {
        const int eol = buf.indexOf("\r\n", pos);
        if (eol < 0) return r;                  // terminador ainda não chegou
        const QByteArray line = buf.mid(pos, eol - pos);
        if (isCodeLine(line, code, ' ')) {
            r.code     = code;
            r.text     = QString::fromUtf8(line.mid(4)).trimmed();
            r.complete = true;
            if (consumed) *consumed = eol + 2;
            return r;
        }
        pos = eol + 2;
    }
}

std::optional<QPair<QString, quint16>> parsePasv(const QString& line) {
    static const QRegularExpression re(
        R"(\((\d{1,3}),(\d{1,3}),(\d{1,3}),(\d{1,3}),(\d{1,3}),(\d{1,3})\))");
    const auto m = re.match(line);
    if (!m.hasMatch()) return std::nullopt;

    int v[6];
    for (int i = 0; i < 6; ++i) {
        bool ok = false;
        v[i] = m.captured(i + 1).toInt(&ok);
        if (!ok || v[i] < 0 || v[i] > 255) return std::nullopt;
    }
    const QString host = QString("%1.%2.%3.%4").arg(v[0]).arg(v[1]).arg(v[2]).arg(v[3]);
    const quint16 port = quint16(v[4] * 256 + v[5]);
    return QPair<QString, quint16>(host, port);
}

std::optional<QDateTime> parseMdtm(const QString& line) {
    static const QRegularExpression re(R"(^213\s+(\d{14}))");
    const auto m = re.match(line.trimmed());
    if (!m.hasMatch()) return std::nullopt;
    QDateTime dt = QDateTime::fromString(m.captured(1), "yyyyMMddHHmmss");
    if (!dt.isValid()) return std::nullopt;
    dt.setTimeZone(QTimeZone::UTC);            // MDTM é sempre UTC (RFC 3659)
    return dt;
}
```

> **Nota sobre `parsePasvMalformedReturnsNullopt`:** o caso `"227 (1,2,3,4,5,6,7)"` casa a regex nos
> **seis primeiros** números e retornaria um valor. Se o teste falhar, ancore a regex exigindo `)`
> logo após o 6º grupo — o `\)` já faz isso, então `(1,2,3,4,5,6,7)` **não** casa (o 7º número
> impede o `\)`). Verifique; se casar por outro motivo, ajuste a regex, não o teste.

- [ ] **Step 5: Registrar no CMake do core**

`src/core/CMakeLists.txt` — adicionar às fontes de `orbitcore`:

```cmake
  FtpReply.cpp
```

- [ ] **Step 6: Rodar e ver passar**

Run: `cmake --build build -j && ctest --test-dir build --output-on-failure -R tst_ftp`
Expected: PASS, 12 casos.

- [ ] **Step 7: Suíte inteira**

Run: `ctest --test-dir build --output-on-failure`
Expected: **7/7 verde**.

- [ ] **Step 8: Sem commit** — deixe no working tree e reporte.

---

## Task 5: `TestFtpServer`

**Files:**
- Create: `tests/TestFtpServer.h`, `tests/TestFtpServer.cpp`
- Modify: `tests/CMakeLists.txt`, `tests/tst_ftp.cpp`

**Interfaces:**
- Consumes: nada do projeto (usa `QTcpServer`/`QTcpSocket` direto).
- Produces: `class TestFtpServer` com:
  - `explicit TestFtpServer(QByteArray content)`
  - `bool listen()` — 127.0.0.1, porta efêmera
  - `quint16 port() const`
  - `QUrl url(const QString& path = "/f.bin") const` — monta `ftp://127.0.0.1:PORT/f.bin`
  - `void setContent(QByteArray)` / `void setMdtm(const QString&)` (`"YYYYMMDDHHMMSS"`)
  - `void setNoSize(bool)` / `void setNoMdtm(bool)` / `void setNoRest(bool)`
  - `void requireAuth(const QString& user, const QString& pass)`
  - `void setDropAfter(qint64 bytes)` — fecha o socket de dados após N bytes
  - `void setDropOnce(bool)` — o `dropAfter` vale só na **primeira** transferência; as seguintes
    completam (permite testar retry **bem-sucedido**, não só desistência)
  - `void setMissing(bool)` — `550` em `SIZE`/`MDTM`/`RETR` (arquivo inexistente)
  - `void setMaxConnections(int n)` — `421` nas conexões de controle extras
  - `void setRestFailsAt(int nth)` — `REST` responde `502` a partir da n-ésima conexão
  - `int  controlConnections() const`

**Objetivo:** manter a suíte 100% offline (spec §5). Este é um **test double**, não um servidor FTP
de verdade — implemente só o subset abaixo e nada mais.

- [ ] **Step 1: Escrever `tests/TestFtpServer.h`**

```cpp
#pragma once
#include <QByteArray>
#include <QObject>
#include <QString>
#include <QTcpServer>
#include <QUrl>

class QTcpSocket;

// Servidor FTP mínimo, in-process, para testes offline. Fala só o subset que o
// cliente usa: USER, PASS, TYPE, SIZE, MDTM, PASV, REST, RETR, QUIT.
class TestFtpServer : public QObject {
    Q_OBJECT
public:
    explicit TestFtpServer(QByteArray content, QObject* parent = nullptr);

    bool    listen();
    quint16 port() const { return m_server.serverPort(); }
    QUrl    url(const QString& path = "/f.bin") const;

    void setContent(const QByteArray& c) { m_content = c; }
    void setMdtm(const QString& v)       { m_mdtm = v; }
    void setNoSize(bool on)              { m_noSize = on; }
    void setNoMdtm(bool on)              { m_noMdtm = on; }
    void setNoRest(bool on)              { m_noRest = on; }
    void requireAuth(const QString& u, const QString& p) { m_user = u; m_pass = p; }
    void setDropAfter(qint64 bytes)      { m_dropAfter = bytes; }
    void setDropOnce(bool on)            { m_dropOnce = on; }
    void setMissing(bool on)             { m_missing = on; }
    void setMaxConnections(int n)        { m_maxConn = n; }
    void setRestFailsAt(int nth)         { m_restFailsAt = nth; }
    int  controlConnections() const      { return m_connCount; }

private:
    struct Session;
    void onNewConnection();
    void onLine(Session* s, const QByteArray& line);
    void startTransfer(Session* s);

    QTcpServer m_server;
    QByteArray m_content;
    QString    m_mdtm = "20260717120000";
    bool       m_noSize = false, m_noMdtm = false, m_noRest = false;
    QString    m_user, m_pass;           // vazio = anônimo aceito
    qint64     m_dropAfter = -1;
    bool       m_dropOnce = false;
    bool       m_dropped  = false;       // já dropou uma vez? (p/ m_dropOnce)
    bool       m_missing  = false;
    int        m_maxConn = -1;
    int        m_restFailsAt = -1;
    int        m_connCount = 0;
};
```

- [ ] **Step 2: Escrever `tests/TestFtpServer.cpp`**

```cpp
#include "TestFtpServer.h"
#include <QTcpSocket>
#include <QTimer>
#include <QHostAddress>

struct TestFtpServer::Session {
    QTcpSocket* ctrl = nullptr;
    QByteArray  buf;
    QString     user;
    bool        loggedIn = false;
    qint64      rest = 0;
    QTcpServer* pasv = nullptr;     // listener passivo desta sessão
    QTcpSocket* data = nullptr;
    int         index = 0;          // n-ésima conexão de controle (1-based)
};

TestFtpServer::TestFtpServer(QByteArray content, QObject* parent)
    : QObject(parent), m_content(std::move(content)) {
    connect(&m_server, &QTcpServer::newConnection, this, &TestFtpServer::onNewConnection);
}

bool TestFtpServer::listen() {
    return m_server.listen(QHostAddress::LocalHost, 0);
}

QUrl TestFtpServer::url(const QString& path) const {
    QUrl u;
    u.setScheme("ftp");
    u.setHost("127.0.0.1");
    u.setPort(port());
    u.setPath(path);
    return u;
}

static void send(QTcpSocket* s, const QString& line) {
    s->write((line + "\r\n").toUtf8());
    s->flush();
}

void TestFtpServer::onNewConnection() {
    while (QTcpSocket* c = m_server.nextPendingConnection()) {
        ++m_connCount;
        auto* s = new Session;
        s->ctrl  = c;
        s->index = m_connCount;

        if (m_maxConn > 0 && m_connCount > m_maxConn) {
            send(c, "421 Too many connections");
            c->disconnectFromHost();
            delete s;
            continue;
        }

        connect(c, &QTcpSocket::readyRead, this, [this, s] {
            s->buf += s->ctrl->readAll();
            int eol;
            while ((eol = s->buf.indexOf("\r\n")) >= 0) {
                const QByteArray line = s->buf.left(eol);
                s->buf.remove(0, eol + 2);
                onLine(s, line);
            }
        });
        connect(c, &QTcpSocket::disconnected, this, [s] {
            if (s->pasv) s->pasv->deleteLater();
            if (s->data) s->data->deleteLater();
            s->ctrl->deleteLater();
            delete s;
        });

        send(c, "220 TestFtpServer ready");
    }
}

void TestFtpServer::onLine(Session* s, const QByteArray& line) {
    const QString l   = QString::fromUtf8(line).trimmed();
    const QString cmd = l.section(' ', 0, 0).toUpper();
    const QString arg = l.section(' ', 1);

    if (cmd == "USER") {
        s->user = arg;
        send(s->ctrl, "331 Password required");
    } else if (cmd == "PASS") {
        const bool needAuth = !m_user.isEmpty();
        if (!needAuth) {
            s->loggedIn = true;
            send(s->ctrl, "230 Logged in");
        } else if (s->user == m_user && arg == m_pass) {
            s->loggedIn = true;
            send(s->ctrl, "230 Logged in");
        } else {
            send(s->ctrl, "530 Authentication required");
        }
    } else if (!s->loggedIn) {
        send(s->ctrl, "530 Please login");
    } else if (cmd == "TYPE") {
        send(s->ctrl, "200 Type set");
    } else if (cmd == "SIZE") {
        if (m_missing)     send(s->ctrl, "550 No such file");
        else if (m_noSize) send(s->ctrl, "502 SIZE not implemented");
        else               send(s->ctrl, QString("213 %1").arg(m_content.size()));
    } else if (cmd == "MDTM") {
        if (m_missing)     send(s->ctrl, "550 No such file");
        else if (m_noMdtm) send(s->ctrl, "502 MDTM not implemented");
        else               send(s->ctrl, QString("213 %1").arg(m_mdtm));
    } else if (cmd == "PASV") {
        if (s->pasv) { s->pasv->deleteLater(); s->pasv = nullptr; }
        s->pasv = new QTcpServer(this);
        if (!s->pasv->listen(QHostAddress::LocalHost, 0)) {
            send(s->ctrl, "425 Can't open data connection");
            return;
        }
        const quint16 p = s->pasv->serverPort();
        send(s->ctrl, QString("227 Entering Passive Mode (127,0,0,1,%1,%2).")
                          .arg(p / 256).arg(p % 256));
    } else if (cmd == "REST") {
        const bool fails = m_noRest ||
                           (m_restFailsAt > 0 && s->index >= m_restFailsAt);
        if (fails) { send(s->ctrl, "502 REST not implemented"); return; }
        s->rest = arg.toLongLong();
        send(s->ctrl, QString("350 Restarting at %1").arg(s->rest));
    } else if (cmd == "RETR") {
        if (m_missing) { send(s->ctrl, "550 No such file"); return; }
        if (!s->pasv)  { send(s->ctrl, "425 Use PASV first"); return; }
        startTransfer(s);
    } else if (cmd == "QUIT") {
        send(s->ctrl, "221 Bye");
        s->ctrl->disconnectFromHost();
    } else {
        send(s->ctrl, "500 Unknown command");
    }
}

void TestFtpServer::startTransfer(Session* s) {
    send(s->ctrl, "150 Opening data connection");

    auto deliver = [this, s] {
        s->data = s->pasv->nextPendingConnection();
        if (!s->data) return;

        QByteArray payload = m_content.mid(s->rest);   // FTP manda do REST ATÉ O FIM

        // dropAfter: mata a transferência no meio. Com dropOnce, só a PRIMEIRA
        // morre — as seguintes completam, o que permite testar um retry
        // BEM-SUCEDIDO (e não só a desistência após esgotar retries).
        const bool doDrop = (m_dropAfter >= 0) && payload.size() > m_dropAfter &&
                            (!m_dropOnce || !m_dropped);
        if (doDrop) {
            m_dropped = true;
            payload = payload.left(m_dropAfter);
            s->data->write(payload);
            s->data->flush();
            s->data->abort();                          // queda no meio: sem 226
            s->rest = 0;
            return;
        }
        s->data->write(payload);
        s->data->flush();
        s->data->disconnectFromHost();
        send(s->ctrl, "226 Transfer complete");
        s->rest = 0;
    };

    if (s->pasv->hasPendingConnections()) deliver();
    else connect(s->pasv, &QTcpServer::newConnection, this, deliver, Qt::SingleShotConnection);
}
```

> **Ponto crítico do double:** `startTransfer` manda `m_content.mid(s->rest)` — **do offset até o
> fim**, sempre. É exatamente o comportamento real do FTP e a razão de o cliente precisar cortar
> sozinho no `end` (spec §3.3, critério 8). Não "melhore" isso mandando só a fatia pedida: destruiria
> o teste que mais importa.

- [ ] **Step 3: Escrever o teste de fumaça do próprio double**

Em `tests/tst_ftp.cpp`, adicionar no topo `#include "TestFtpServer.h"`, `#include <QTcpSocket>` e:

```cpp
    // O double precisa ser confiável antes de qualquer teste de cliente.
    void testServerSpeaksBasicFtp() {
        TestFtpServer srv(QByteArray("HELLO"));
        QVERIFY(srv.listen());

        QTcpSocket c;
        c.connectToHost("127.0.0.1", srv.port());
        QVERIFY(c.waitForConnected(3000));

        auto readLine = [&c]() -> QString {
            if (!c.canReadLine()) QVERIFY2(c.waitForReadyRead(3000), "no reply");
            return QString::fromUtf8(c.readLine()).trimmed();
        };
        auto cmd = [&c, &readLine](const QString& s) {
            c.write((s + "\r\n").toUtf8());
            c.flush();
            return readLine();
        };

        QVERIFY(readLine().startsWith("220"));
        QVERIFY(cmd("USER anonymous").startsWith("331"));
        QVERIFY(cmd("PASS x@y").startsWith("230"));
        QVERIFY(cmd("TYPE I").startsWith("200"));
        QCOMPARE(cmd("SIZE /f.bin"), QString("213 5"));
        QVERIFY(cmd("MDTM /f.bin").startsWith("213 2026"));
        QVERIFY(cmd("REST 1").startsWith("350"));
    }

    void testServerHonorsKnobs() {
        TestFtpServer srv(QByteArray("HELLO"));
        srv.setNoSize(true);
        srv.setNoRest(true);
        srv.requireAuth("bob", "secret");
        QVERIFY(srv.listen());

        QTcpSocket c;
        c.connectToHost("127.0.0.1", srv.port());
        QVERIFY(c.waitForConnected(3000));
        auto readLine = [&c]() -> QString {
            if (!c.canReadLine()) QVERIFY2(c.waitForReadyRead(3000), "no reply");
            return QString::fromUtf8(c.readLine()).trimmed();
        };
        auto cmd = [&c, &readLine](const QString& s) {
            c.write((s + "\r\n").toUtf8());
            c.flush();
            return readLine();
        };

        QVERIFY(readLine().startsWith("220"));
        QVERIFY(cmd("USER anonymous").startsWith("331"));
        QVERIFY(cmd("PASS x@y").startsWith("530"));      // credencial errada
        QVERIFY(cmd("USER bob").startsWith("331"));
        QVERIFY(cmd("PASS secret").startsWith("230"));
        QVERIFY(cmd("SIZE /f.bin").startsWith("502"));   // noSize
        QVERIFY(cmd("REST 1").startsWith("502"));        // noRest
    }
```

- [ ] **Step 4: Registrar no CMake**

`tests/CMakeLists.txt` — trocar a linha do `tst_ftp`:

```cmake
add_executable(tst_ftp tst_ftp.cpp TestFtpServer.cpp)
target_link_libraries(tst_ftp PRIVATE orbitcore Qt6::Test Qt6::Network)
add_test(NAME tst_ftp COMMAND tst_ftp)
```

- [ ] **Step 5: Rodar**

Run: `cmake --build build -j && ctest --test-dir build --output-on-failure -R tst_ftp`
Expected: PASS, 14 casos.

- [ ] **Step 6: Suíte inteira**

Run: `ctest --test-dir build --output-on-failure`
Expected: **7/7 verde**.

- [ ] **Step 7: Sem commit** — deixe no working tree e reporte.

---

## Task 6: `FtpControlChannel`

**Files:**
- Create: `src/core/FtpControlChannel.h`, `src/core/FtpControlChannel.cpp`
- Modify: `src/core/CMakeLists.txt`, `tests/tst_ftp.cpp`

**Interfaces:**
- Consumes: `parseReply` (Task 4), `Credentials` (Task 1), `TestFtpServer` (Task 5).
- Produces:
  - `enum class FtpErrorClass { Transient, Fatal, Auth }`
  - `class FtpControlChannel : public QObject` com:
    - `explicit FtpControlChannel(QObject* parent = nullptr)`
    - `void connectAndLogin(const QUrl& url, const Credentials& creds, int connectTimeoutMs)`
    - `void sendCommand(const QString& cmd)` → resposta via `replyReceived`
    - `void close()`
    - sinais: `loggedIn()`, `replyReceived(int code, const QString& text)`,
      `failed(const QString& error, FtpErrorClass cls)`

**Por que existe (refinamento sobre a spec §4):** `FtpProbe` (Task 7) e `FtpSegmentWorker` (Task 8)
precisam **ambos** de conectar + `USER`/`PASS` + `TYPE I` + parsing incremental de respostas. Sem esta
peça, isso seria escrito duas vezes.

**Regras de classificação (spec §3.3):**

| Resposta | Classe |
|---|---|
| `421`, `425`, `426`, socket caiu, timeout | `Transient` |
| `530` | `Auth` |
| `550` e demais `5xx` | `Fatal` |

- [ ] **Step 1: Escrever os testes que falham**

Em `tests/tst_ftp.cpp`, adicionar `#include "FtpControlChannel.h"` e:

```cpp
    void controlChannelLogsInAnonymously() {
        TestFtpServer srv(QByteArray("HELLO"));
        QVERIFY(srv.listen());
        FtpControlChannel ch;
        QSignalSpy in(&ch, &FtpControlChannel::loggedIn);
        ch.connectAndLogin(srv.url(), Credentials{}, 3000);
        QVERIFY(in.wait(3000));
    }

    void controlChannelLogsInWithCredentials() {
        TestFtpServer srv(QByteArray("HELLO"));
        srv.requireAuth("bob", "secret");
        QVERIFY(srv.listen());
        FtpControlChannel ch;
        QSignalSpy in(&ch, &FtpControlChannel::loggedIn);
        ch.connectAndLogin(srv.url(), Credentials{"bob", "secret"}, 3000);
        QVERIFY(in.wait(3000));
    }

    void controlChannelReportsAuthFailure() {
        TestFtpServer srv(QByteArray("HELLO"));
        srv.requireAuth("bob", "secret");
        QVERIFY(srv.listen());
        FtpControlChannel ch;
        QSignalSpy bad(&ch, &FtpControlChannel::failed);
        ch.connectAndLogin(srv.url(), Credentials{"bob", "wrong"}, 3000);
        QVERIFY(bad.wait(3000));
        QCOMPARE(bad.at(0).at(1).value<FtpErrorClass>(), FtpErrorClass::Auth);
    }

    void controlChannelSendsCommandAndParsesReply() {
        TestFtpServer srv(QByteArray("HELLO"));
        QVERIFY(srv.listen());
        FtpControlChannel ch;
        QSignalSpy in(&ch, &FtpControlChannel::loggedIn);
        QSignalSpy rep(&ch, &FtpControlChannel::replyReceived);
        ch.connectAndLogin(srv.url(), Credentials{}, 3000);
        QVERIFY(in.wait(3000));
        ch.sendCommand("SIZE /f.bin");
        QVERIFY(rep.wait(3000));
        QCOMPARE(rep.at(0).at(0).toInt(), 213);
        QCOMPARE(rep.at(0).at(1).toString(), QString("5"));
    }

    void controlChannelClassifiesTooManyConnectionsAsTransient() {
        TestFtpServer srv(QByteArray("HELLO"));
        srv.setMaxConnections(0);          // toda conexão leva 421
        QVERIFY(srv.listen());
        FtpControlChannel ch;
        QSignalSpy bad(&ch, &FtpControlChannel::failed);
        ch.connectAndLogin(srv.url(), Credentials{}, 3000);
        QVERIFY(bad.wait(3000));
        QCOMPARE(bad.at(0).at(1).value<FtpErrorClass>(), FtpErrorClass::Transient);
    }

    void controlChannelTimesOutOnDeadPort() {
        // Porta fechada: erro de conexão -> Transient (vale a pena tentar de novo).
        FtpControlChannel ch;
        QSignalSpy bad(&ch, &FtpControlChannel::failed);
        QUrl u; u.setScheme("ftp"); u.setHost("127.0.0.1"); u.setPort(1); u.setPath("/f.bin");
        ch.connectAndLogin(u, Credentials{}, 2000);
        QVERIFY(bad.wait(5000));
        QCOMPARE(bad.at(0).at(1).value<FtpErrorClass>(), FtpErrorClass::Transient);
    }
```

- [ ] **Step 2: Rodar e ver falhar**

Run: `cmake --build build -j`
Expected: FALHA — `FtpControlChannel.h: No such file or directory`.

- [ ] **Step 3: Escrever `src/core/FtpControlChannel.h`**

```cpp
#pragma once
#include "Transport.h"          // Credentials
#include <QObject>
#include <QQueue>
#include <QUrl>
class QTcpSocket;
class QTimer;

enum class FtpErrorClass { Transient, Fatal, Auth };

// Socket de controle FTP: conecta, faz login (USER/PASS/TYPE I) e serializa
// comandos -> respostas. Compartilhado por FtpProbe e FtpSegmentWorker.
class FtpControlChannel : public QObject {
    Q_OBJECT
public:
    explicit FtpControlChannel(QObject* parent = nullptr);

    void connectAndLogin(const QUrl& url, const Credentials& creds, int connectTimeoutMs);
    void sendCommand(const QString& cmd);
    void close();

signals:
    void loggedIn();
    void replyReceived(int code, const QString& text);
    void failed(const QString& error, FtpErrorClass cls);

private:
    enum class Phase { Idle, Greeting, User, Pass, Type, Ready };
    void onConnected();
    void onReadyRead();
    void onSocketError();
    void onTimeout();
    void handleReply(int code, const QString& text);
    void write(const QString& cmd);
    void fail(const QString& error, FtpErrorClass cls);
    static FtpErrorClass classify(int code);

    QTcpSocket* m_sock = nullptr;
    QTimer*     m_timer = nullptr;
    QByteArray  m_buf;
    QUrl        m_url;
    Credentials m_creds;
    Phase       m_phase = Phase::Idle;
    int         m_timeoutMs = 30000;
    bool        m_dead = false;
};

Q_DECLARE_METATYPE(FtpErrorClass)
```

- [ ] **Step 4: Escrever `src/core/FtpControlChannel.cpp`**

```cpp
#include "FtpControlChannel.h"
#include "FtpReply.h"
#include <QTcpSocket>
#include <QTimer>

FtpControlChannel::FtpControlChannel(QObject* parent) : QObject(parent) {
    qRegisterMetaType<FtpErrorClass>("FtpErrorClass");
}

void FtpControlChannel::connectAndLogin(const QUrl& url, const Credentials& creds,
                                        int connectTimeoutMs) {
    m_url       = url;
    m_creds     = creds;
    m_timeoutMs = connectTimeoutMs;
    m_phase     = Phase::Greeting;
    m_dead      = false;
    m_buf.clear();

    m_sock = new QTcpSocket(this);
    connect(m_sock, &QTcpSocket::connected,    this, &FtpControlChannel::onConnected);
    connect(m_sock, &QTcpSocket::readyRead,    this, &FtpControlChannel::onReadyRead);
    connect(m_sock, &QTcpSocket::errorOccurred, this, &FtpControlChannel::onSocketError);

    m_timer = new QTimer(this);
    m_timer->setSingleShot(true);
    connect(m_timer, &QTimer::timeout, this, &FtpControlChannel::onTimeout);
    m_timer->start(m_timeoutMs);

    m_sock->connectToHost(m_url.host(), quint16(m_url.port(21)));
}

void FtpControlChannel::onConnected() {
    // Nada a fazer: o servidor fala primeiro (220). O timer segue armado até o
    // greeting chegar.
}

void FtpControlChannel::write(const QString& cmd) {
    if (!m_sock) return;
    m_sock->write((cmd + "\r\n").toUtf8());
    m_sock->flush();
    if (m_timer) m_timer->start(m_timeoutMs);   // re-arma a cada comando
}

void FtpControlChannel::onReadyRead() {
    if (m_dead || !m_sock) return;
    m_buf += m_sock->readAll();
    while (true) {
        int consumed = 0;
        const FtpReply r = parseReply(m_buf, &consumed);
        if (!r.complete) break;
        m_buf.remove(0, consumed);
        handleReply(r.code, r.text);
        if (m_dead) return;
    }
}

FtpErrorClass FtpControlChannel::classify(int code) {
    switch (code) {
        case 421: case 425: case 426: return FtpErrorClass::Transient;
        case 530:                     return FtpErrorClass::Auth;
        default:                      return FtpErrorClass::Fatal;
    }
}

void FtpControlChannel::handleReply(int code, const QString& text) {
    if (m_timer) m_timer->stop();

    switch (m_phase) {
        case Phase::Greeting:
            if (code != 220) { fail(QString("%1 %2").arg(code).arg(text), classify(code)); return; }
            m_phase = Phase::User;
            write(m_creds.user.isEmpty() ? "USER anonymous"
                                         : "USER " + m_creds.user);
            return;

        case Phase::User:
            if (code == 230) { m_phase = Phase::Type; write("TYPE I"); return; }   // sem senha
            if (code != 331) { fail(QString("%1 %2").arg(code).arg(text), classify(code)); return; }
            m_phase = Phase::Pass;
            write(m_creds.pass.isEmpty() ? "PASS orbit@tribute"
                                         : "PASS " + m_creds.pass);
            return;

        case Phase::Pass:
            if (code != 230) { fail(QString("%1 %2").arg(code).arg(text), classify(code)); return; }
            m_phase = Phase::Type;
            write("TYPE I");
            return;

        case Phase::Type:
            if (code != 200) { fail(QString("%1 %2").arg(code).arg(text), classify(code)); return; }
            m_phase = Phase::Ready;
            emit loggedIn();
            return;

        case Phase::Ready:
            emit replyReceived(code, text);
            return;

        case Phase::Idle:
            return;
    }
}

void FtpControlChannel::sendCommand(const QString& cmd) {
    if (m_phase != Phase::Ready || m_dead) return;
    write(cmd);
}

void FtpControlChannel::onSocketError() {
    if (m_dead || !m_sock) return;
    // Erro de socket (conexão recusada, reset, host inacessível) é sempre
    // transitório do ponto de vista do worker: vale um retry com backoff.
    fail("socket: " + m_sock->errorString(), FtpErrorClass::Transient);
}

void FtpControlChannel::onTimeout() {
    if (m_dead) return;
    fail("timeout", FtpErrorClass::Transient);
}

void FtpControlChannel::fail(const QString& error, FtpErrorClass cls) {
    if (m_dead) return;
    m_dead = true;
    if (m_timer) m_timer->stop();
    emit failed(error, cls);
}

void FtpControlChannel::close() {
    m_dead  = true;
    m_phase = Phase::Idle;
    if (m_timer) m_timer->stop();
    if (m_sock) {
        m_sock->disconnect(this);       // nada de sinais depois do close
        m_sock->abort();
        m_sock->deleteLater();
        m_sock = nullptr;
    }
}
```

> **`m_sock->disconnect(this)` antes do `abort()` em `close()`** é a mesma disciplina de
> `SegmentWorker::onTimeout` (`SegmentWorker.cpp:172`): abortar um socket dispara sinais que
> chegariam depois, encontrando estado já reciclado.

- [ ] **Step 5: Registrar no CMake**

`src/core/CMakeLists.txt`:

```cmake
  FtpControlChannel.cpp
```

- [ ] **Step 6: Rodar e ver passar**

Run: `cmake --build build -j && ctest --test-dir build --output-on-failure -R tst_ftp`
Expected: PASS, 20 casos.

- [ ] **Step 7: Suíte inteira**

Run: `ctest --test-dir build --output-on-failure`
Expected: **7/7 verde**.

- [ ] **Step 8: Sem commit** — deixe no working tree e reporte.

---

## Task 7: `FtpProbe` + `FtpTransport`

**Files:**
- Create: `src/core/FtpProbe.h`, `src/core/FtpProbe.cpp`, `src/core/FtpTransport.h`,
  `src/core/FtpTransport.cpp`
- Modify: `src/core/DownloadTypes.h`, `src/core/DownloadManager.cpp`, `src/core/CMakeLists.txt`,
  `tests/tst_ftp.cpp`

**Interfaces:**
- Consumes: `FtpControlChannel`, `FtpErrorClass` (Task 6); `parseMdtm` (Task 4); `Probe`,
  `Transport`, `ProbeResult` (Task 1).
- Produces: `ProbeResult::authRequired` (novo campo, default `false`);
  `class FtpProbe : public Probe` (construtor `FtpProbe(const EngineConfig&, QObject*)`);
  `class FtpTransport : public Transport` (construtor sem argumentos). `createWorker` do
  `FtpTransport` devolve `nullptr` até a Task 8 — nenhum teste desta tarefa o chama.

**Sequência (spec §3.2):** `login` → `SIZE path` → `MDTM path` → `REST 1` → `REST 0` → `QUIT`.
`SIZE` falho → `totalBytes = -1`; `MDTM` falho → validador vazio; `REST 1` sem `350` →
`supportsRange = false`. `530` → `ProbeResult{ok=false, authRequired=true}`. `550` no `SIZE` →
`ok=false` fatal.

- [ ] **Step 1: Escrever os testes que falham (critérios 4, 5, 12-probe, 6-integrado)**

Em `tests/tst_ftp.cpp`, adicionar `#include "FtpProbe.h"` e:

```cpp
    // Critério 4
    void ftpProbeHappyPath() {
        TestFtpServer srv(makeBody(5000));
        srv.setMdtm("20260717123045");
        QVERIFY(srv.listen());
        EngineConfig cfg;
        FtpProbe probe(cfg);
        QSignalSpy spy(&probe, &Probe::finished);
        probe.start(srv.url(), Credentials{});
        QVERIFY(spy.wait(5000));
        const auto r = qvariant_cast<ProbeResult>(spy.at(0).at(0));
        QVERIFY(r.ok);
        QVERIFY(r.supportsRange);
        QCOMPARE(r.totalBytes, qint64(5000));
        QCOMPARE(r.lastModified, QString("20260717123045"));
        QVERIFY(r.etag.isEmpty());          // FTP não tem ETag
        QVERIFY(!r.authRequired);
    }

    // Critério 5 (sem SIZE)
    void ftpProbeWithoutSizeReportsUnknownTotal() {
        TestFtpServer srv(makeBody(5000));
        srv.setNoSize(true);
        QVERIFY(srv.listen());
        EngineConfig cfg;
        FtpProbe probe(cfg);
        QSignalSpy spy(&probe, &Probe::finished);
        probe.start(srv.url(), Credentials{});
        QVERIFY(spy.wait(5000));
        const auto r = qvariant_cast<ProbeResult>(spy.at(0).at(0));
        QVERIFY(r.ok);
        QCOMPARE(r.totalBytes, qint64(-1));
        QVERIFY(!r.supportsRange);          // sem tamanho não há como segmentar
    }

    // Critério 5 (sem REST)
    void ftpProbeWithoutRestReportsNoRangeSupport() {
        TestFtpServer srv(makeBody(5000));
        srv.setNoRest(true);
        QVERIFY(srv.listen());
        EngineConfig cfg;
        FtpProbe probe(cfg);
        QSignalSpy spy(&probe, &Probe::finished);
        probe.start(srv.url(), Credentials{});
        QVERIFY(spy.wait(5000));
        const auto r = qvariant_cast<ProbeResult>(spy.at(0).at(0));
        QVERIFY(r.ok);
        QCOMPARE(r.totalBytes, qint64(5000));
        QVERIFY(!r.supportsRange);
    }

    void ftpProbeWithoutMdtmHasEmptyValidator() {
        TestFtpServer srv(makeBody(5000));
        srv.setNoMdtm(true);
        QVERIFY(srv.listen());
        EngineConfig cfg;
        FtpProbe probe(cfg);
        QSignalSpy spy(&probe, &Probe::finished);
        probe.start(srv.url(), Credentials{});
        QVERIFY(spy.wait(5000));
        const auto r = qvariant_cast<ProbeResult>(spy.at(0).at(0));
        QVERIFY(r.ok);
        QVERIFY(r.lastModified.isEmpty());
        QVERIFY(r.supportsRange);           // REST continua funcionando
    }

    // Critério 12 (auth pelo probe)
    void ftpProbeReportsAuthRequired() {
        TestFtpServer srv(makeBody(500));
        srv.requireAuth("bob", "secret");
        QVERIFY(srv.listen());
        EngineConfig cfg;
        FtpProbe probe(cfg);
        QSignalSpy spy(&probe, &Probe::finished);
        probe.start(srv.url(), Credentials{});      // anônimo -> 530
        QVERIFY(spy.wait(5000));
        const auto r = qvariant_cast<ProbeResult>(spy.at(0).at(0));
        QVERIFY(!r.ok);
        QVERIFY(r.authRequired);
    }

    void ftpProbeSucceedsWithCredentials() {
        TestFtpServer srv(makeBody(500));
        srv.requireAuth("bob", "secret");
        QVERIFY(srv.listen());
        EngineConfig cfg;
        FtpProbe probe(cfg);
        QSignalSpy spy(&probe, &Probe::finished);
        probe.start(srv.url(), Credentials{"bob", "secret"});
        QVERIFY(spy.wait(5000));
        const auto r = qvariant_cast<ProbeResult>(spy.at(0).at(0));
        QVERIFY(r.ok);
        QVERIFY(!r.authRequired);
        QCOMPARE(r.totalBytes, qint64(500));
    }

    void ftpTransportIsRegisteredForFtpScheme() {
        QTemporaryDir dir;
        EngineConfig cfg;
        DownloadManager mgr(cfg, dir.path());
        QVERIFY(mgr.transportFor(QUrl("ftp://h/f")) != nullptr);
    }
```

Adicionar no topo de `tst_ftp.cpp`: `#include <QSignalSpy>`, `#include <QTemporaryDir>`,
`#include "DownloadManager.h"`, e o helper `makeBody` (mesmo de `tst_download.cpp`):

```cpp
static QByteArray makeBody(int n) {
    QByteArray b; b.resize(n);
    for (int i = 0; i < n; ++i) b[i] = char('A' + (i % 26));
    return b;
}
```

- [ ] **Step 2: Rodar e ver falhar**

Run: `cmake --build build -j`
Expected: FALHA — `FtpProbe.h: No such file or directory`.

- [ ] **Step 3: Adicionar `authRequired` ao `ProbeResult`**

`src/core/DownloadTypes.h`:

```cpp
struct ProbeResult {
    bool    ok            = false;
    qint64  totalBytes    = -1;
    bool    supportsRange = false;
    QString etag;
    QString lastModified;
    QUrl    resolvedUrl;
    QString error;
    bool    authRequired  = false;   // 530: o Core vai pedir credenciais à GUI (spec §3.6)
};
```

- [ ] **Step 4: Escrever `src/core/FtpProbe.h`**

```cpp
#pragma once
#include "Transport.h"
class FtpControlChannel;

class FtpProbe : public Probe {
    Q_OBJECT
public:
    explicit FtpProbe(const EngineConfig& cfg, QObject* parent = nullptr);
    void start(const QUrl& url, const Credentials& creds) override;
private:
    enum class Step { Size, Mdtm, RestProbe, RestReset };
    void onLoggedIn();
    void onReply(int code, const QString& text);
    void finishOk();
    // O tratamento de `failed` do canal é uma lambda no .cpp (precisa do tipo
    // FtpErrorClass, que só o .cpp inclui) — por isso não há slot p/ ele aqui.

    EngineConfig       m_cfg;
    FtpControlChannel* m_ch = nullptr;
    QUrl               m_url;
    ProbeResult        m_r;
    Step               m_step = Step::Size;
    bool               m_done = false;
};
```

- [ ] **Step 5: Escrever `src/core/FtpProbe.cpp`**

```cpp
#include "FtpProbe.h"
#include "FtpControlChannel.h"
#include "FtpReply.h"

FtpProbe::FtpProbe(const EngineConfig& cfg, QObject* parent)
    : Probe(parent), m_cfg(cfg) {
    qRegisterMetaType<ProbeResult>("ProbeResult");
}

void FtpProbe::start(const QUrl& url, const Credentials& creds) {
    m_url = url;
    m_ch  = new FtpControlChannel(this);
    connect(m_ch, &FtpControlChannel::loggedIn,      this, &FtpProbe::onLoggedIn);
    connect(m_ch, &FtpControlChannel::replyReceived, this, &FtpProbe::onReply);
    connect(m_ch, &FtpControlChannel::failed, this,
            [this](const QString& e, FtpErrorClass cls) {
        if (m_done) return;
        m_done = true;
        m_r.ok           = false;
        m_r.error        = e;
        m_r.authRequired = (cls == FtpErrorClass::Auth);
        m_ch->close();
        emit finished(m_r);
    });
    m_ch->connectAndLogin(url, creds, m_cfg.connectTimeoutMs);
}

void FtpProbe::onLoggedIn() {
    m_r.resolvedUrl = m_url;
    m_step = Step::Size;
    m_ch->sendCommand("SIZE " + m_url.path());
}

void FtpProbe::onReply(int code, const QString& text) {
    if (m_done) return;
    switch (m_step) {
        case Step::Size:
            // 213 <n> = tamanho. Qualquer outra coisa (502/550) = tamanho
            // desconhecido; seguimos assim mesmo e o download cai em fallback.
            if (code == 213) {
                bool ok = false;
                const qint64 n = text.trimmed().toLongLong(&ok);
                if (ok) m_r.totalBytes = n;
            }
            m_step = Step::Mdtm;
            m_ch->sendCommand("MDTM " + m_url.path());
            return;

        case Step::Mdtm:
            // Guardamos a STRING crua (não o QDateTime): é ela que vai para o
            // .meta como validador e que o worker vai comparar (spec §3.5).
            if (code == 213 && parseMdtm(QString("213 %1").arg(text)).has_value())
                m_r.lastModified = text.trimmed();
            m_step = Step::RestProbe;
            m_ch->sendCommand("REST 1");
            return;

        case Step::RestProbe:
            // 350 = REST suportado. Sem tamanho conhecido não há o que
            // segmentar, então supportsRange exige os dois.
            m_r.supportsRange = (code == 350) && (m_r.totalBytes > 0);
            if (code == 350) {
                m_step = Step::RestReset;
                m_ch->sendCommand("REST 0");   // não deixa offset pendente na sessão
                return;
            }
            finishOk();
            return;

        case Step::RestReset:
            finishOk();
            return;
    }
}

void FtpProbe::finishOk() {
    if (m_done) return;
    m_done  = true;
    m_r.ok  = true;
    m_ch->sendCommand("QUIT");
    m_ch->close();
    emit finished(m_r);
}
```

- [ ] **Step 6: Escrever `src/core/FtpTransport.h` e `.cpp`**

```cpp
// FtpTransport.h
#pragma once
#include "Transport.h"

class FtpTransport : public Transport {
public:
    FtpTransport() = default;
    Probe*         createProbe(QObject* parent) override;
    SegmentSource* createWorker(QFile* file, const EngineConfig& cfg, QObject* parent) override;
private:
    EngineConfig m_probeCfg;   // só p/ timeouts do probe; o worker recebe o cfg da task
};
```

```cpp
// FtpTransport.cpp
#include "FtpTransport.h"
#include "FtpProbe.h"

Probe* FtpTransport::createProbe(QObject* parent) {
    return new FtpProbe(m_probeCfg, parent);
}

SegmentSource* FtpTransport::createWorker(QFile* file, const EngineConfig& cfg, QObject* parent) {
    Q_UNUSED(file); Q_UNUSED(cfg); Q_UNUSED(parent);
    return nullptr;   // Task 8
}
```

- [ ] **Step 7: Registrar `ftp` no registry**

`src/core/DownloadManager.cpp`, no construtor, junto dos outros:

```cpp
#include "FtpTransport.h"
// ...
    m_transports.insert("ftp", std::unique_ptr<Transport>(new FtpTransport()));
```

- [ ] **Step 8: CMake**

`src/core/CMakeLists.txt`:

```cmake
  FtpProbe.cpp
  FtpTransport.cpp
```

- [ ] **Step 9: Rodar e ver passar**

Run: `cmake --build build -j && ctest --test-dir build --output-on-failure -R tst_ftp`
Expected: PASS, 27 casos.

- [ ] **Step 10: Suíte inteira**

Run: `ctest --test-dir build --output-on-failure`
Expected: **7/7 verde**.

- [ ] **Step 11: Sem commit** — deixe no working tree e reporte.

---

## Task 8: `FtpSegmentWorker` — download, corte no `end`, multi-segmento

**Files:**
- Create: `src/core/FtpSegmentWorker.h`, `src/core/FtpSegmentWorker.cpp`
- Modify: `src/core/FtpTransport.cpp`, `src/core/CMakeLists.txt`, `tests/tst_ftp.cpp`

**Interfaces:**
- Consumes: `FtpControlChannel`, `FtpErrorClass` (Task 6); `parsePasv` (Task 4); `SegmentSource`,
  `FailureKind` (Task 1).
- Produces: `class FtpSegmentWorker : public SegmentSource`, construtor
  `FtpSegmentWorker(QFile* file, const EngineConfig& cfg, QObject* parent = nullptr)`.
  `FtpTransport::createWorker` passa a devolvê-lo.

**A lógica que não tem paralelo no HTTP (spec §3.3, critério 8):** o servidor manda do `REST` **até o
fim do arquivo** — não sabe parar num byte. Todo worker com `end >= 0` trunca o excedente, emite
`completed` e fecha. Só o worker de fallback (`end == -1`) lê até o `226`.

Nesta tarefa o worker **ainda não** faz `MDTM` (Task 9) nem `AuthRequired` (Task 10).

- [ ] **Step 1: Escrever os testes que falham (critérios 7, 8, 9)**

Em `tests/tst_ftp.cpp`, adicionar `#include "FtpSegmentWorker.h"`, `#include <QTemporaryDir>`,
`#include <QFile>` e:

```cpp
    // Critério 8: o worker corta no end, mesmo o servidor mandando até o fim.
    void ftpWorkerCutsAtSegmentEnd() {
        const QByteArray body = makeBody(10000);
        TestFtpServer srv(body);
        QVERIFY(srv.listen());
        QTemporaryDir dir;
        QFile f(dir.path() + "/out.bin");
        QVERIFY(f.open(QIODevice::ReadWrite));
        f.resize(body.size());

        EngineConfig cfg;
        FtpSegmentWorker w(&f, cfg);
        QSignalSpy done(&w, &SegmentSource::completed);

        Segment seg;                       // fatia do meio: [2000, 4999]
        seg.index = 0; seg.start = 2000; seg.current = 2000; seg.end = 4999;
        w.start(seg, srv.url(), QString(), Credentials{});
        QVERIFY(done.wait(5000));

        QCOMPARE(w.segment().current, qint64(5000));   // parou logo após o end
        f.seek(2000);
        QCOMPARE(f.read(3000), body.mid(2000, 3000));
        // E não escreveu além do end: o byte 5000 continua zerado.
        f.seek(5000);
        QCOMPARE(f.read(1), QByteArray(1, '\0'));
    }

    // Critério 9: fallback (end == -1) lê até o fim.
    void ftpWorkerFallbackReadsToEof() {
        const QByteArray body = makeBody(8000);
        TestFtpServer srv(body);
        QVERIFY(srv.listen());
        QTemporaryDir dir;
        QFile f(dir.path() + "/out.bin");
        QVERIFY(f.open(QIODevice::ReadWrite));

        EngineConfig cfg;
        FtpSegmentWorker w(&f, cfg);
        QSignalSpy done(&w, &SegmentSource::completed);

        Segment seg;
        seg.index = 0; seg.start = 0; seg.current = 0; seg.end = -1;
        w.start(seg, srv.url(), QString(), Credentials{});
        QVERIFY(done.wait(5000));

        f.flush();
        QCOMPARE(w.segment().end, qint64(7999));   // end preenchido no EOF
        f.seek(0);
        QCOMPARE(f.readAll(), body);
    }

    // Critério 7: download FTP multi-segmento completo, byte-idêntico.
    void ftpMultiSegmentDownloadIsByteIdentical() {
        const QByteArray body = makeBody(1 << 16);   // 64 KiB
        TestFtpServer srv(body);
        QVERIFY(srv.listen());
        QTemporaryDir dir;
        const QString dest = dir.path() + "/out.bin";

        EngineConfig cfg;
        cfg.minSegSize = 1024;                       // força 4 segmentos reais
        DownloadManager mgr(cfg, dir.path());
        const QUuid id = mgr.addDownload(srv.url(), dest);
        QVERIFY(!id.isNull());

        QVERIFY(QTest::qWaitFor([&]{
            DownloadTask* t = mgr.taskById(id);
            return t && t->state() == DownloadState::Completed;
        }, 15000));

        QFile f(dest);
        QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(f.readAll(), body);
        QVERIFY(srv.controlConnections() >= 4);      // um por segmento + o probe
    }

    // Critério 9 integrado: servidor sem REST -> um segmento, arquivo íntegro.
    void ftpFallbackDownloadIsByteIdentical() {
        const QByteArray body = makeBody(20000);
        TestFtpServer srv(body);
        srv.setNoRest(true);
        QVERIFY(srv.listen());
        QTemporaryDir dir;
        const QString dest = dir.path() + "/out.bin";

        EngineConfig cfg;
        DownloadManager mgr(cfg, dir.path());
        const QUuid id = mgr.addDownload(srv.url(), dest);

        QVERIFY(QTest::qWaitFor([&]{
            DownloadTask* t = mgr.taskById(id);
            return t && t->state() == DownloadState::Completed;
        }, 15000));

        QFile f(dest);
        QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(f.readAll(), body);
    }

    // Queda no meio da transferência -> retry -> completa, retomando do offset
    // já gravado (não do zero).
    void ftpWorkerRetriesAfterDataDrop() {
        const QByteArray body = makeBody(10000);
        TestFtpServer srv(body);
        srv.setDropAfter(3000);            // morre aos 3000 bytes...
        srv.setDropOnce(true);             // ...mas só na primeira vez
        QVERIFY(srv.listen());
        QTemporaryDir dir;
        QFile f(dir.path() + "/out.bin");
        QVERIFY(f.open(QIODevice::ReadWrite));
        f.resize(body.size());

        EngineConfig cfg;
        cfg.retryBackoffBaseMs = 50;
        FtpSegmentWorker w(&f, cfg);
        QSignalSpy done(&w, &SegmentSource::completed);

        Segment seg;
        seg.index = 0; seg.start = 0; seg.current = 0; seg.end = 9999;
        w.start(seg, srv.url(), QString(), Credentials{});

        QVERIFY(done.wait(15000));
        QCOMPARE(w.segment().current, qint64(10000));
        f.flush();
        f.seek(0);
        QCOMPARE(f.read(10000), body);     // sem buraco nem byte duplicado na emenda
        QVERIFY(srv.controlConnections() >= 2);   // houve mesmo uma segunda tentativa
    }

    // Retries esgotados -> desiste com failed, não trava.
    void ftpWorkerGivesUpAfterMaxRetries() {
        const QByteArray body = makeBody(10000);
        TestFtpServer srv(body);
        srv.setDropAfter(3000);            // toda transferência morre
        QVERIFY(srv.listen());
        QTemporaryDir dir;
        QFile f(dir.path() + "/out.bin");
        QVERIFY(f.open(QIODevice::ReadWrite));
        f.resize(body.size());

        EngineConfig cfg;
        cfg.retryBackoffBaseMs = 10;
        cfg.maxSegmentRetries  = 2;
        FtpSegmentWorker w(&f, cfg);
        QSignalSpy bad(&w, &SegmentSource::failed);

        Segment seg;
        seg.index = 0; seg.start = 0; seg.current = 0; seg.end = 9999;
        w.start(seg, srv.url(), QString(), Credentials{});

        QVERIFY(bad.wait(15000));
        QCOMPARE(bad.at(0).at(2).value<FailureKind>(), FailureKind::Fatal);
    }
```

- [ ] **Step 2: Rodar e ver falhar**

Run: `cmake --build build -j`
Expected: FALHA — `FtpSegmentWorker.h: No such file or directory`.

- [ ] **Step 3: Escrever `src/core/FtpSegmentWorker.h`**

```cpp
#pragma once
#include "FtpControlChannel.h"
#include "Transport.h"
#include <QUrl>
class QFile;
class QTcpSocket;
class QTimer;

class FtpSegmentWorker : public SegmentSource {
    Q_OBJECT
public:
    FtpSegmentWorker(QFile* file, const EngineConfig& cfg, QObject* parent = nullptr);
    void start(const Segment& seg, const QUrl& url,
               const QString& validator, const Credentials& creds) override;
    void stop() override;
    Segment segment() const override { return m_seg; }

private:
    enum class Step { Pasv, Rest, Retr, Transferring };
    void openAttempt();
    void onLoggedIn();
    void onReply(int code, const QString& text);
    void onControlFailed(const QString& error, FtpErrorClass cls);
    void onDataReadyRead();
    void onDataFinished();
    void finishSegment();
    void scheduleRetry(const QString& why);
    void armIdleTimer(int ms);
    void onTimeout();
    void teardown();

    QFile*             m_file;
    EngineConfig       m_cfg;
    Segment            m_seg;
    QUrl               m_url;
    QString            m_validator;
    Credentials        m_creds;
    FtpControlChannel* m_ch   = nullptr;
    QTcpSocket*        m_data = nullptr;
    Step               m_step = Step::Pasv;
    bool               m_stopped = false;
    bool               m_finished = false;
    int                m_attempt = 0;
    QTimer*            m_retryTimer = nullptr;
    QTimer*            m_idleTimer  = nullptr;
};
```

- [ ] **Step 4: Escrever `src/core/FtpSegmentWorker.cpp`**

```cpp
#include "FtpSegmentWorker.h"
#include "FtpReply.h"
#include <QFile>
#include <QTcpSocket>
#include <QTimer>

FtpSegmentWorker::FtpSegmentWorker(QFile* file, const EngineConfig& cfg, QObject* parent)
    : SegmentSource(parent), m_file(file), m_cfg(cfg) {}

void FtpSegmentWorker::start(const Segment& seg, const QUrl& url,
                             const QString& validator, const Credentials& creds) {
    m_seg       = seg;
    m_url       = url;
    m_validator = validator;
    m_creds     = creds;
    m_stopped   = false;
    m_finished  = false;
    m_attempt   = 0;
    openAttempt();
}

void FtpSegmentWorker::openAttempt() {
    if (m_stopped) return;
    if (m_seg.isComplete()) { emit completed(m_seg.index); return; }

    teardown();                       // limpa qualquer tentativa anterior
    m_step = Step::Pasv;
    m_ch   = new FtpControlChannel(this);
    connect(m_ch, &FtpControlChannel::loggedIn,      this, &FtpSegmentWorker::onLoggedIn);
    connect(m_ch, &FtpControlChannel::replyReceived, this, &FtpSegmentWorker::onReply);
    connect(m_ch, &FtpControlChannel::failed,        this, &FtpSegmentWorker::onControlFailed);
    m_ch->connectAndLogin(m_url, m_creds, m_cfg.connectTimeoutMs);
    armIdleTimer(m_cfg.connectTimeoutMs);
}

void FtpSegmentWorker::onLoggedIn() {
    if (m_stopped) return;
    m_step = Step::Pasv;
    m_ch->sendCommand("PASV");
}

void FtpSegmentWorker::onReply(int code, const QString& text) {
    if (m_stopped || m_finished) return;

    switch (m_step) {
        case Step::Pasv: {
            if (code != 227) { scheduleRetry(QString("PASV: %1 %2").arg(code).arg(text)); return; }
            const auto hp = parsePasv(QString("227 %1").arg(text));
            if (!hp) { scheduleRetry("PASV: resposta malformada"); return; }

            m_data = new QTcpSocket(this);
            connect(m_data, &QTcpSocket::readyRead,    this, &FtpSegmentWorker::onDataReadyRead);
            connect(m_data, &QTcpSocket::disconnected, this, &FtpSegmentWorker::onDataFinished);
            m_data->connectToHost(hp->first, hp->second);

            // REST antes do RETR. Offset 0 também manda REST 0: inofensivo e
            // mantém um único caminho de código.
            m_step = Step::Rest;
            m_ch->sendCommand(QString("REST %1").arg(m_seg.current));
            return;
        }

        case Step::Rest:
            // REST recusado embora o .meta diga supportsRange: o servidor mudou
            // de comportamento. Não dá p/ retomar, mas dá p/ recomeçar do zero
            // (spec §3.5) — restartRequired, não failed.
            if (code != 350) { emit restartRequired(m_seg.index); return; }
            m_step = Step::Retr;
            m_ch->sendCommand("RETR " + m_url.path());
            return;

        case Step::Retr:
            if (code != 150 && code != 125) {
                if (code == 550) {
                    emit failed(m_seg.index, QString("RETR: %1 %2").arg(code).arg(text),
                                FailureKind::Fatal);
                    return;
                }
                scheduleRetry(QString("RETR: %1 %2").arg(code).arg(text));
                return;
            }
            m_step = Step::Transferring;
            armIdleTimer(m_cfg.idleTimeoutMs);
            return;

        case Step::Transferring:
            // 226 = transferência completa (relevante só p/ o fallback, que não
            // tem end conhecido). Segmentos com end já terminaram no corte.
            if (code == 226 && m_seg.end < 0) finishSegment();
            return;
    }
}

void FtpSegmentWorker::onDataReadyRead() {
    if (m_stopped || m_finished || !m_data) return;
    armIdleTimer(m_cfg.idleTimeoutMs);

    QByteArray chunk = m_data->readAll();
    if (chunk.isEmpty()) return;

    // O CORTE (spec §3.3): o servidor manda do REST até o fim do arquivo — não
    // sabe parar num byte. Quem corta somos nós.
    if (m_seg.end >= 0) {
        const qint64 room = m_seg.end - m_seg.current + 1;
        if (room <= 0) { finishSegment(); return; }
        if (chunk.size() > room) chunk.truncate(int(room));
    }

    m_file->seek(m_seg.current);
    const qint64 written = m_file->write(chunk);
    if (written < 0) {
        emit failed(m_seg.index, "write error: " + m_file->errorString(), FailureKind::Fatal);
        m_finished = true;
        teardown();
        return;
    }
    m_seg.current += written;
    emit progressed(m_seg.index, m_seg.current);

    if (m_seg.end >= 0 && m_seg.current > m_seg.end) finishSegment();
}

void FtpSegmentWorker::onDataFinished() {
    if (m_stopped || m_finished) return;
    // Socket de dados fechou. Fallback: isso é EOF = sucesso. Segmento com end:
    // fechou antes de completarmos = leitura curta, tenta de novo do offset atual.
    if (m_seg.end < 0) { finishSegment(); return; }
    if (m_seg.current <= m_seg.end) scheduleRetry("short read");
}

void FtpSegmentWorker::finishSegment() {
    if (m_finished) return;
    m_finished = true;
    m_file->flush();
    if (m_seg.end < 0) m_seg.end = m_seg.current - 1;   // fallback: end vira o EOF
    teardown();
    emit completed(m_seg.index);
}

void FtpSegmentWorker::onControlFailed(const QString& error, FtpErrorClass cls) {
    if (m_stopped || m_finished) return;
    switch (cls) {
        case FtpErrorClass::Transient: scheduleRetry(error); return;
        case FtpErrorClass::Auth:
            // Tratado na Task 10; por ora, fatal.
            emit failed(m_seg.index, error, FailureKind::Fatal);
            m_finished = true;
            teardown();
            return;
        case FtpErrorClass::Fatal:
            emit failed(m_seg.index, error, FailureKind::Fatal);
            m_finished = true;
            teardown();
            return;
    }
}

void FtpSegmentWorker::armIdleTimer(int ms) {
    if (!m_idleTimer) {
        m_idleTimer = new QTimer(this);
        m_idleTimer->setSingleShot(true);
        connect(m_idleTimer, &QTimer::timeout, this, &FtpSegmentWorker::onTimeout);
    }
    m_idleTimer->start(ms);
}

void FtpSegmentWorker::onTimeout() {
    if (m_stopped || m_finished) return;
    scheduleRetry("timeout");
}

void FtpSegmentWorker::scheduleRetry(const QString& why) {
    Q_UNUSED(why);
    teardown();
    if (m_attempt >= m_cfg.maxSegmentRetries) {
        emit failed(m_seg.index, "retries exhausted", FailureKind::Fatal);
        m_finished = true;
        return;
    }
    ++m_attempt;
    const int delay = m_cfg.retryBackoffBaseMs * (1 << (m_attempt - 1));
    if (!m_retryTimer) {
        m_retryTimer = new QTimer(this);
        m_retryTimer->setSingleShot(true);
        connect(m_retryTimer, &QTimer::timeout, this, &FtpSegmentWorker::openAttempt);
    }
    m_retryTimer->start(delay);
}

// Derruba a tentativa atual sem tocar em m_seg (o offset avançado é o ponto de
// partida do próximo retry). Desconecta antes de abortar: abortar dispara
// sinais que chegariam depois, encontrando estado já reciclado — mesma
// disciplina de SegmentWorker::onTimeout (SegmentWorker.cpp:172).
void FtpSegmentWorker::teardown() {
    if (m_idleTimer) m_idleTimer->stop();
    if (m_data) {
        m_data->disconnect(this);
        m_data->abort();
        m_data->deleteLater();
        m_data = nullptr;
    }
    if (m_ch) {
        m_ch->disconnect(this);
        m_ch->close();
        m_ch->deleteLater();
        m_ch = nullptr;
    }
}

void FtpSegmentWorker::stop() {
    m_stopped = true;
    if (m_retryTimer) m_retryTimer->stop();
    teardown();
}
```

- [ ] **Step 5: `FtpTransport` passa a criar o worker**

`src/core/FtpTransport.cpp`:

```cpp
#include "FtpSegmentWorker.h"
// ...
SegmentSource* FtpTransport::createWorker(QFile* file, const EngineConfig& cfg, QObject* parent) {
    return new FtpSegmentWorker(file, cfg, parent);
}
```

- [ ] **Step 6: CMake**

`src/core/CMakeLists.txt`:

```cmake
  FtpSegmentWorker.cpp
```

- [ ] **Step 7: Rodar e ver passar**

Run: `cmake --build build -j && ctest --test-dir build --output-on-failure -R tst_ftp`
Expected: PASS, 32 casos.

- [ ] **Step 8: Suíte inteira**

Run: `ctest --test-dir build --output-on-failure`
Expected: **7/7 verde**.

- [ ] **Step 9: Sem commit** — deixe no working tree e reporte.

---

## Task 9: Resume FTP + validação por `MDTM`

**Files:**
- Modify: `src/core/FtpSegmentWorker.h`, `src/core/FtpSegmentWorker.cpp`, `tests/tst_ftp.cpp`

**Interfaces:**
- Consumes: `FtpSegmentWorker` (Task 8), `parseMdtm` (Task 4), `TestFtpServer::setMdtm`/`setContent`
  (Task 5), `SegmentSource::restartRequired` (Task 1), guarda `m_restarting` (Task 3).
- Produces: nenhuma API nova — só o passo `Mdtm` na máquina de estados do worker.

**A regra (spec §3.5):** no resume o probe **não roda** (`restore()` marca `m_probed = true`), e FTP
não tem `If-Range`. Então o worker compara `MDTM` com o validador que recebeu, **na mesma condição em
que o HTTP manda `If-Range`**: validador não-vazio. Divergiu → `restartRequired`, que zera tudo e
limpa o validador — a segunda passada não checa `MDTM` e o restart é finito.

- [ ] **Step 1: Escrever os testes que falham (critérios 10, 11)**

Em `tests/tst_ftp.cpp`:

```cpp
    // Critério 11: MDTM divergente -> restartRequired.
    void ftpWorkerRestartsOnMdtmMismatch() {
        const QByteArray body = makeBody(10000);
        TestFtpServer srv(body);
        srv.setMdtm("20260717120000");
        QVERIFY(srv.listen());
        QTemporaryDir dir;
        QFile f(dir.path() + "/out.bin");
        QVERIFY(f.open(QIODevice::ReadWrite));
        f.resize(body.size());

        EngineConfig cfg;
        FtpSegmentWorker w(&f, cfg);
        QSignalSpy restart(&w, &SegmentSource::restartRequired);

        Segment seg;
        seg.index = 0; seg.start = 0; seg.current = 500; seg.end = 9999;
        // validador do .meta != MDTM do servidor
        w.start(seg, srv.url(), QString("20260101000000"), Credentials{});
        QVERIFY(restart.wait(5000));
        QCOMPARE(restart.at(0).at(0).toInt(), 0);
    }

    // MDTM igual -> segue direto, sem restart.
    void ftpWorkerProceedsOnMdtmMatch() {
        const QByteArray body = makeBody(10000);
        TestFtpServer srv(body);
        srv.setMdtm("20260717120000");
        QVERIFY(srv.listen());
        QTemporaryDir dir;
        QFile f(dir.path() + "/out.bin");
        QVERIFY(f.open(QIODevice::ReadWrite));
        f.resize(body.size());

        EngineConfig cfg;
        FtpSegmentWorker w(&f, cfg);
        QSignalSpy done(&w, &SegmentSource::completed);
        QSignalSpy restart(&w, &SegmentSource::restartRequired);

        Segment seg;
        seg.index = 0; seg.start = 0; seg.current = 0; seg.end = 9999;
        w.start(seg, srv.url(), QString("20260717120000"), Credentials{});
        QVERIFY(done.wait(5000));
        QCOMPARE(restart.count(), 0);
    }

    // Validador vazio -> nem manda MDTM (spec §3.5: resume não-validado).
    void ftpWorkerSkipsMdtmWithoutValidator() {
        const QByteArray body = makeBody(5000);
        TestFtpServer srv(body);
        QVERIFY(srv.listen());
        QTemporaryDir dir;
        QFile f(dir.path() + "/out.bin");
        QVERIFY(f.open(QIODevice::ReadWrite));
        f.resize(body.size());

        EngineConfig cfg;
        FtpSegmentWorker w(&f, cfg);
        QSignalSpy done(&w, &SegmentSource::completed);
        QSignalSpy restart(&w, &SegmentSource::restartRequired);

        Segment seg;
        seg.index = 0; seg.start = 0; seg.current = 0; seg.end = 4999;
        w.start(seg, srv.url(), QString(), Credentials{});
        QVERIFY(done.wait(5000));
        QCOMPARE(restart.count(), 0);
    }

    // Critério 10: resume multi-segmento entre "sessões", MDTM inalterado.
    void ftpResumeAcrossSessionsWithValidMdtm() {
        // 4 MiB: corpo grande o bastante p/ que o download NÃO termine antes de
        // conseguirmos pausar. Com 64 KiB de localhost o teste completaria na
        // primeira sessão e não testaria resume nenhum.
        const QByteArray body = makeBody(4 << 20);
        TestFtpServer srv(body);
        srv.setMdtm("20260717120000");
        srv.setDropAfter(64 << 10);        // cada transferência morre aos 64 KiB...
        QVERIFY(srv.listen());
        QTemporaryDir dir;
        const QString dest = dir.path() + "/out.bin";

        EngineConfig cfg;
        cfg.minSegSize = 1024;

        // Sessão 1: baixa um pedaço e pausa. O dropAfter garante que a sessão 1
        // não consiga completar, tornando o pause determinístico.
        {
            DownloadManager mgr(cfg, dir.path());
            const QUuid id = mgr.addDownload(srv.url(), dest);
            qint64 got = 0;
            connect(&mgr, &DownloadManager::taskProgress, this,
                    [&got](const QUuid&, qint64 r, qint64) { got = r; });
            QVERIFY(QTest::qWaitFor([&]{ return got > 0; }, 10000));
            mgr.pause(id);
            QVERIFY(QTest::qWaitFor([&]{
                DownloadTask* t = mgr.taskById(id);
                return t && t->state() == DownloadState::Paused;
            }, 5000));
            QVERIFY(got < body.size());     // realmente parou no meio
        }

        srv.setDropAfter(-1);              // ...mas a sessão 2 pode completar

        // Sessão 2: recarrega do .meta e retoma até completar.
        {
            DownloadManager mgr(cfg, dir.path());
            mgr.loadSession();
            QCOMPARE(mgr.tasks().size(), 1);
            const QUuid id = mgr.tasks().first()->id();
            mgr.resume(id);
            QVERIFY(QTest::qWaitFor([&]{
                DownloadTask* t = mgr.taskById(id);
                return t && t->state() == DownloadState::Completed;
            }, 20000));
        }

        QFile f(dest);
        QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(f.readAll(), body);
    }

    // Critério 11 integrado: arquivo muda entre sessões -> descarta parcial.
    void ftpResumeWithChangedFileRestartsFromZero() {
        const QByteArray v1 = makeBody(4 << 20);
        TestFtpServer srv(v1);
        srv.setMdtm("20260717120000");
        srv.setDropAfter(64 << 10);        // sessão 1 não completa (ver teste acima)
        QVERIFY(srv.listen());
        QTemporaryDir dir;
        const QString dest = dir.path() + "/out.bin";

        EngineConfig cfg;
        cfg.minSegSize = 1024;

        {
            DownloadManager mgr(cfg, dir.path());
            const QUuid id = mgr.addDownload(srv.url(), dest);
            qint64 got = 0;
            connect(&mgr, &DownloadManager::taskProgress, this,
                    [&got](const QUuid&, qint64 r, qint64) { got = r; });
            QVERIFY(QTest::qWaitFor([&]{ return got > 0; }, 10000));
            mgr.pause(id);
            QVERIFY(QTest::qWaitFor([&]{
                DownloadTask* t = mgr.taskById(id);
                return t && t->state() == DownloadState::Paused;
            }, 5000));
            QVERIFY(got < v1.size());
        }

        // O arquivo mudou no servidor: mesmo tamanho, conteúdo e MDTM outros.
        QByteArray v2 = v1;
        for (int i = 0; i < v2.size(); ++i) v2[i] = char('z' - (i % 26));
        srv.setContent(v2);
        srv.setMdtm("20260718090000");
        srv.setDropAfter(-1);              // sessão 2 pode completar

        {
            DownloadManager mgr(cfg, dir.path());
            mgr.loadSession();
            const QUuid id = mgr.tasks().first()->id();
            mgr.resume(id);
            QVERIFY(QTest::qWaitFor([&]{
                DownloadTask* t = mgr.taskById(id);
                return t && t->state() == DownloadState::Completed;
            }, 20000));
        }

        // Resultado: v2 puro. Se o parcial de v1 tivesse sobrevivido, seria um
        // Frankenstein dos dois.
        QFile f(dest);
        QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(f.readAll(), v2);
    }
```

- [ ] **Step 2: Rodar e ver falhar**

Run: `cmake --build build -j && ctest --test-dir build --output-on-failure -R tst_ftp`
Expected: FALHA — `ftpWorkerRestartsOnMdtmMismatch` timeout (o worker ainda não manda `MDTM`) e
`ftpResumeWithChangedFileRestartsFromZero` com arquivo misturado.

- [ ] **Step 3: Adicionar o passo `Mdtm` à máquina de estados**

`src/core/FtpSegmentWorker.h` — trocar o enum:

```cpp
    enum class Step { Mdtm, Pasv, Rest, Retr, Transferring };
```

`src/core/FtpSegmentWorker.cpp` — em `onLoggedIn`, checar o validador antes do `PASV`:

```cpp
void FtpSegmentWorker::onLoggedIn() {
    if (m_stopped) return;
    // FTP não tem If-Range. Quando há validador (mesma condição em que o HTTP
    // mandaria If-Range), verificamos explicitamente que o arquivo não mudou —
    // um round-trip a mais, que é o preço do protocolo (spec §3.5).
    if (!m_validator.isEmpty()) {
        m_step = Step::Mdtm;
        m_ch->sendCommand("MDTM " + m_url.path());
        return;
    }
    m_step = Step::Pasv;
    m_ch->sendCommand("PASV");
}
```

E no `onReply`, adicionar o case antes do `Pasv`:

```cpp
        case Step::Mdtm: {
            // Servidor sem MDTM (502): não dá p/ validar. Seguimos — é a mesma
            // exposição do HTTP sem ETag/Last-Modified (spec §3.5).
            if (code != 213) {
                m_step = Step::Pasv;
                m_ch->sendCommand("PASV");
                return;
            }
            if (text.trimmed() != m_validator) {
                // O arquivo mudou: o parcial no disco não presta.
                // restartRequired (não failed): dá p/ recomeçar do zero.
                emit restartRequired(m_seg.index);
                return;
            }
            m_step = Step::Pasv;
            m_ch->sendCommand("PASV");
            return;
        }
```

> **Comparação de string crua, não de `QDateTime`:** o validador que veio do `.meta` é exatamente a
> string que o `FtpProbe` gravou (Task 7, `Step::Mdtm`). Comparar strings evita ida-e-volta de
> parsing e diferenças de timezone. O `parseMdtm` continua servindo para validar **formato** no probe.

> **Depois do `emit restartRequired`, retorne imediatamente e não toque em membros.** A `DownloadTask`
> vai chamar `stop()` + `deleteLater()` neste worker de dentro do emit (Task 3).

- [ ] **Step 4: Rodar e ver passar**

Run: `cmake --build build -j && ctest --test-dir build --output-on-failure -R tst_ftp`
Expected: PASS, 37 casos.

- [ ] **Step 5: Suíte inteira + ASAN**

Run: `ctest --test-dir build --output-on-failure`
Expected: **7/7 verde**.

Run: `cmake --build build-asan -j --target tst_ftp && ./build-asan/tests/tst_ftp`
Expected: sem `heap-use-after-free` — este é o cenário real do §9.1 (N workers pedindo restart).

- [ ] **Step 6: Sem commit** — deixe no working tree e reporte.

---

## Task 10: Credenciais fim-a-fim (Core)

**Files:**
- Modify: `src/core/FtpSegmentWorker.cpp`, `src/core/DownloadTask.h`, `src/core/DownloadTask.cpp`,
  `src/core/DownloadManager.h`, `src/core/DownloadManager.cpp`, `tests/tst_ftp.cpp`

**Interfaces:**
- Consumes: `FailureKind::AuthRequired`, `Credentials` (Task 1); `ProbeResult::authRequired`
  (Task 7); `FtpErrorClass::Auth` (Task 6).
- Produces:
  - `DownloadTask`: sinal `credentialsRequired(const QUuid& id, const QString& host)`;
    `void setCredentials(const Credentials&)`; membros `m_creds`, `m_awaitingCredentials`.
  - `DownloadManager`: sinal `credentialsRequired(const QUuid& id, const QString& host)`;
    `void provideCredentials(const QUuid& id, const QString& user, const QString& pass)`.

**Os dois pontos de entrada (spec §3.6):** download novo → `530` no **probe**; resume → `530` no
**worker** (porque `m_probed == true` e o probe não roda). Ambos convergem para `Paused` +
`credentialsRequired` **emitido uma vez** (guarda `m_awaitingCredentials`) — sem a guarda, 4 segmentos
= 4 diálogos.

- [ ] **Step 1: Escrever os testes que falham (critérios 12, 13)**

```cpp
    // Critério 12: auth pelo probe (download novo).
    void ftpAuthRequiredFromProbePausesAndAsks() {
        TestFtpServer srv(makeBody(5000));
        srv.requireAuth("bob", "secret");
        QVERIFY(srv.listen());
        QTemporaryDir dir;
        const QString dest = dir.path() + "/out.bin";

        EngineConfig cfg;
        DownloadManager mgr(cfg, dir.path());
        QSignalSpy ask(&mgr, &DownloadManager::credentialsRequired);
        const QUuid id = mgr.addDownload(srv.url(), dest);

        QVERIFY(ask.wait(5000));
        QCOMPARE(ask.count(), 1);
        QCOMPARE(ask.at(0).at(0).value<QUuid>(), id);
        QCOMPARE(ask.at(0).at(1).toString(), QString("127.0.0.1"));
        QCOMPARE(mgr.taskById(id)->state(), DownloadState::Paused);

        mgr.provideCredentials(id, "bob", "secret");
        QVERIFY(QTest::qWaitFor([&]{
            DownloadTask* t = mgr.taskById(id);
            return t && t->state() == DownloadState::Completed;
        }, 20000));

        QFile f(dest);
        QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(f.readAll(), makeBody(5000));
    }

    // Critério 13: auth pelo worker (resume) — e UM único sinal p/ N segmentos.
    void ftpAuthRequiredFromWorkerAsksOnce() {
        const QByteArray body = makeBody(4 << 20);
        TestFtpServer srv(body);
        srv.setMdtm("20260717120000");
        srv.setDropAfter(64 << 10);
        QVERIFY(srv.listen());
        QTemporaryDir dir;
        const QString dest = dir.path() + "/out.bin";

        EngineConfig cfg;
        cfg.minSegSize = 1024;

        // Sessão 1: anônimo funciona; baixa um pouco e pausa. O dropAfter torna
        // o pause determinístico (senão o download completa antes).
        {
            DownloadManager mgr(cfg, dir.path());
            const QUuid id = mgr.addDownload(srv.url(), dest);
            qint64 got = 0;
            connect(&mgr, &DownloadManager::taskProgress, this,
                    [&got](const QUuid&, qint64 r, qint64) { got = r; });
            QVERIFY(QTest::qWaitFor([&]{ return got > 0; }, 10000));
            mgr.pause(id);
            QVERIFY(QTest::qWaitFor([&]{
                DownloadTask* t = mgr.taskById(id);
                return t && t->state() == DownloadState::Paused;
            }, 5000));
            QVERIFY(got < body.size());
        }

        // O servidor passa a exigir login. Sessão 2 não sonda (m_probed do
        // .meta): os N workers levam 530 juntos.
        srv.requireAuth("bob", "secret");
        srv.setDropAfter(-1);

        {
            DownloadManager mgr(cfg, dir.path());
            mgr.loadSession();
            const QUuid id = mgr.tasks().first()->id();
            QSignalSpy ask(&mgr, &DownloadManager::credentialsRequired);
            mgr.resume(id);

            QVERIFY(ask.wait(10000));
            QTest::qWait(500);                    // dá tempo p/ os outros workers falharem
            QCOMPARE(ask.count(), 1);             // UM diálogo, não N
            QCOMPARE(mgr.taskById(id)->state(), DownloadState::Paused);

            mgr.provideCredentials(id, "bob", "secret");
            QVERIFY(QTest::qWaitFor([&]{
                DownloadTask* t = mgr.taskById(id);
                return t && t->state() == DownloadState::Completed;
            }, 20000));
        }

        QFile f(dest);
        QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(f.readAll(), body);
    }

    // Critério 12: 550 (arquivo inexistente) é fatal — Error, sem retry e sem
    // diálogo de credenciais.
    void ftpMissingFileIsFatal() {
        TestFtpServer srv(makeBody(100));
        srv.setMissing(true);                     // 550 em SIZE/MDTM/RETR
        QVERIFY(srv.listen());
        QTemporaryDir dir;
        EngineConfig cfg;
        cfg.retryBackoffBaseMs = 10;
        DownloadManager mgr(cfg, dir.path());
        QSignalSpy ask(&mgr, &DownloadManager::credentialsRequired);

        const QUuid id = mgr.addDownload(srv.url(), dir.path() + "/out.bin");
        QVERIFY(QTest::qWaitFor([&]{
            DownloadTask* t = mgr.taskById(id);
            return t && t->state() == DownloadState::Error;
        }, 10000));
        QCOMPARE(ask.count(), 0);                 // nunca pediu credenciais
    }
```

> **Onde o `550` é detectado:** com `setMissing`, o `SIZE` responde `550`. O `FtpProbe` (Task 7) trata
> qualquer coisa != `213` no `Step::Size` como "tamanho desconhecido" e **segue** — então quem produz
> o `Error` é o `RETR` no worker (`FtpSegmentWorker::onReply`, `Step::Retr`, `code == 550` →
> `FailureKind::Fatal`). Se este teste falhar com a task presa em `Downloading` ou em retry infinito,
> o defeito está lá, não no teste.

- [ ] **Step 2: Rodar e ver falhar**

Run: `cmake --build build -j`
Expected: FALHA — `DownloadManager::credentialsRequired` não existe.

- [ ] **Step 3: `DownloadTask` guarda credenciais e pede quando falta**

`src/core/DownloadTask.h`:

```cpp
public:
    void setCredentials(const Credentials& c);
signals:
    void credentialsRequired(const QUuid& id, const QString& host);
private:
    void askForCredentials();
    Credentials m_creds;
    bool        m_awaitingCredentials = false;
```

`src/core/DownloadTask.cpp`:

```cpp
void DownloadTask::setCredentials(const Credentials& c) {
    m_creds = c;
    m_awaitingCredentials = false;    // nova rodada: pode perguntar de novo se falhar
}

// Para tudo, persiste o progresso e pede credenciais UMA vez. Com N segmentos,
// cada worker leva 530 e chama isto — só a primeira chamada emite (spec §3.6).
void DownloadTask::askForCredentials() {
    if (m_awaitingCredentials) return;
    m_awaitingCredentials = true;
    pause();                                        // para workers, escreve .meta, -> Paused
    emit credentialsRequired(m_id, m_url.host());
}
```

`start()` e `spawnWorker()` passam as credenciais reais:

```cpp
    probe->start(m_url, m_creds);        // era Credentials{}
    // ...
    w->start(seg, m_url, validator, m_creds);   // era Credentials{}
```

`onProbed` — ponto de entrada (a):

```cpp
void DownloadTask::onProbed(const ProbeResult& r) {
    if (!r.ok) {
        if (r.authRequired) { askForCredentials(); return; }   // spec §3.6 (a)
        setState(DownloadState::Error);
        return;
    }
    // ...resto idêntico...
}
```

`onSegmentFailed` — ponto de entrada (b):

```cpp
void DownloadTask::onSegmentFailed(int index, const QString& error, FailureKind kind) {
    Q_UNUSED(index); Q_UNUSED(error);
    if (kind == FailureKind::AuthRequired) { askForCredentials(); return; }   // spec §3.6 (b)
    // ...resto IDÊNTICO ao atual (para timers, para workers, writeMeta, Error)...
}
```

> **Por que `askForCredentials` chama `pause()` e não `setState(Paused)`:** `pause()` já para os
> workers, faz flush do progresso e escreve o `.meta`. Sem parar os workers, os outros N-1 seguiriam
> tentando e levando `530`.

- [ ] **Step 4: `FtpSegmentWorker` reporta `AuthRequired`**

`src/core/FtpSegmentWorker.cpp`, em `onControlFailed` — trocar o `case Auth`:

```cpp
        case FtpErrorClass::Auth:
            // 530 no resume: o probe não rodou (m_probed do .meta), então é
            // por aqui que o pedido de credenciais nasce (spec §3.6 (b)).
            emit failed(m_seg.index, error, FailureKind::AuthRequired);
            m_finished = true;
            teardown();
            return;
```

- [ ] **Step 5: `DownloadManager` repassa e injeta**

`src/core/DownloadManager.h`:

```cpp
public:
    void provideCredentials(const QUuid& id, const QString& user, const QString& pass);
signals:
    void credentialsRequired(const QUuid& id, const QString& host);
```

`src/core/DownloadManager.cpp` — em `wire()`:

```cpp
    connect(t, &DownloadTask::credentialsRequired, this,
            [this](const QUuid& id, const QString& host) {
        emit credentialsRequired(id, host);
    });
```

E o método:

```cpp
// Credenciais vivem SÓ em memória, nunca no .meta (spec §3.6): senha em texto
// puro no disco não. Depois de recarregar a sessão, a app pergunta de novo.
void DownloadManager::provideCredentials(const QUuid& id, const QString& user, const QString& pass) {
    DownloadTask* t = taskById(id);
    if (!t) return;
    t->setCredentials(Credentials{user, pass});
    resume(id);                 // requeue + pump: respeita o cap de concorrência
}
```

- [ ] **Step 6: Rodar e ver passar**

Run: `cmake --build build -j && ctest --test-dir build --output-on-failure -R tst_ftp`
Expected: PASS, 40 casos.

- [ ] **Step 7: Suíte inteira**

Run: `ctest --test-dir build --output-on-failure`
Expected: **7/7 verde**. `tst_download` 27 casos **sem mudança de expectativa**.

- [ ] **Step 8: Sem commit** — deixe no working tree e reporte. Diga explicitamente se manteve,
      reescreveu ou apagou o `ftpMissingFileIsFatal`.

---

## Task 11: `isValidDownloadUrl` + rótulo de tipo no New

**Files:**
- Modify: `src/gui/NewDownloadDialog.h`, `src/gui/NewDownloadDialog.cpp`, `tests/tst_gui.cpp`

**Interfaces:**
- Consumes: `FileType` (`src/gui/FileType.h`, Fase 2).
- Produces: `static bool NewDownloadDialog::isValidDownloadUrl(const QUrl& u)` — substitui
  `isValidHttpUrl`. Usado nas Tasks 12 e 13.

- [ ] **Step 1: Escrever os testes que falham (critério 14, parte)**

Em `tests/tst_gui.cpp`, achar os casos que exercitam `isValidHttpUrl` e **substituí-los** por:

```cpp
    void validDownloadUrlAcceptsHttpHttpsFtp() {
        QVERIFY(NewDownloadDialog::isValidDownloadUrl(QUrl("http://h/f.bin")));
        QVERIFY(NewDownloadDialog::isValidDownloadUrl(QUrl("https://h/f.bin")));
        QVERIFY(NewDownloadDialog::isValidDownloadUrl(QUrl("ftp://h/f.bin")));
        QVERIFY(NewDownloadDialog::isValidDownloadUrl(QUrl("FTP://h/f.bin")));   // case-insensitive
        QVERIFY(NewDownloadDialog::isValidDownloadUrl(QUrl("ftp://u:p@h/f.bin")));
    }

    void validDownloadUrlRejectsOthers() {
        QVERIFY(!NewDownloadDialog::isValidDownloadUrl(QUrl("gopher://h/f")));
        QVERIFY(!NewDownloadDialog::isValidDownloadUrl(QUrl("file:///tmp/f")));
        QVERIFY(!NewDownloadDialog::isValidDownloadUrl(QUrl("not a url")));
        QVERIFY(!NewDownloadDialog::isValidDownloadUrl(QUrl("http://")));        // sem host
        QVERIFY(!NewDownloadDialog::isValidDownloadUrl(QUrl()));
    }
```

- [ ] **Step 2: Rodar e ver falhar**

Run: `cmake --build build -j`
Expected: FALHA — `isValidDownloadUrl` não existe.

- [ ] **Step 3: Implementar — a regra mora em `orbitgui_logic`, o diálogo delega**

A regra de esquema é usada por três consumidores (diálogo, drop, clipboard), e dois deles precisam
ser testáveis **sem** QtWidgets. Então ela nasce como função livre em `orbitgui_logic`, e
`NewDownloadDialog::isValidDownloadUrl` vira um encaminhamento — uma regra, um lugar.

Criar `src/gui/DropTargets.h` (a Task 12 acrescenta `extractUrls` a este mesmo arquivo):

```cpp
#pragma once
#include <QUrl>

// Esquemas que o motor sabe baixar (spec §3.7). Sem QtWidgets: testável
// headless e reusável pelo drop (Task 12) e pelo clipboard (Task 13).
bool isDownloadableScheme(const QUrl& u);
```

Criar `src/gui/DropTargets.cpp`:

```cpp
#include "DropTargets.h"

bool isDownloadableScheme(const QUrl& u) {
    if (!u.isValid() || u.host().isEmpty()) return false;
    const QString s = u.scheme().toLower();
    return s == "http" || s == "https" || s == "ftp";
}
```

`src/gui/CMakeLists.txt` — em **`orbitgui_logic`**:

```cmake
    DropTargets.cpp
```

`src/gui/NewDownloadDialog.h` — trocar a declaração:

```cpp
    static bool isValidDownloadUrl(const QUrl& u);   // era isValidHttpUrl
```

`src/gui/NewDownloadDialog.cpp` — substituir `isValidHttpUrl` por:

```cpp
#include "DropTargets.h"

bool NewDownloadDialog::isValidDownloadUrl(const QUrl& u) {
    return isDownloadableScheme(u);
}
```

Atualizar **todos** os pontos de uso (o prefill do clipboard e a validação do botão OK). Rodar
`grep -rn "isValidHttpUrl" src tests` ao final e não deixar nenhum — inclusive os 4 casos em
`tests/tst_gui.cpp:498-501`, substituídos no Step 1.

- [ ] **Step 4: Rótulo de tipo + prefill no construtor**

`src/gui/NewDownloadDialog.h`:

```cpp
public:
    // prefill: URL vinda de um drop ou do clipboard (Tasks 12/13). Vazia = o
    // comportamento atual (tenta o clipboard).
    explicit NewDownloadDialog(QWidget* parent = nullptr, const QUrl& prefill = QUrl());
private:
    QLabel* m_type;
```

`src/gui/NewDownloadDialog.cpp` — o construtor passa a receber `prefill`, e a linha do clipboard
(hoje `if (isValidHttpUrl(QUrl(clip))) m_url->setText(clip);`) vira:

```cpp
NewDownloadDialog::NewDownloadDialog(QWidget* parent, const QUrl& prefill) : QDialog(parent) {
    setWindowTitle("New Download");
    m_url  = new QLineEdit(this);
    m_dir  = new QLineEdit(QStandardPaths::writableLocation(QStandardPaths::DownloadLocation), this);
    m_name = new QLabel("—", this);
    m_type = new QLabel("—", this);

    // Prefill explícito (drop/clipboard) tem prioridade; senão, tenta o
    // clipboard como a Fase 2 fazia.
    if (isValidDownloadUrl(prefill)) {
        m_url->setText(prefill.toString());
    } else {
        const QString clip = QApplication::clipboard()->text().trimmed();
        if (isValidDownloadUrl(QUrl(clip))) m_url->setText(clip);
    }
    // ...resto do construtor idêntico, mais a linha do form abaixo...
```

Adicionar a linha ao form, logo após `form->addRow("File:", m_name);`:

```cpp
    form->addRow("Type:", m_type);
```

E `refreshName()` passa a atualizar os dois:

```cpp
void NewDownloadDialog::refreshName() {
    const QString name = deriveFileName(url());
    m_name->setText(name);
    // Rótulo somente-leitura: a categoria continua DERIVADA da extensão
    // (decisão da Fase 2, mantida na Fase 3). Isto informa, não escolhe.
    m_type->setText(FileType::displayName(FileType::categorize(name)));
}
```

Adicionar `#include "FileType.h"` ao `.cpp`.

Trocar também a validação do botão OK: `if (isValidHttpUrl(url())) accept();` →
`if (isValidDownloadUrl(url())) accept();`.

- [ ] **Step 5: Rodar e ver passar**

Run: `cmake --build build -j && ctest --test-dir build --output-on-failure -R tst_gui`
Expected: PASS.

- [ ] **Step 6: Suíte inteira**

Run: `ctest --test-dir build --output-on-failure`
Expected: **7/7 verde**.

- [ ] **Step 7: Sem commit** — deixe no working tree e reporte.

---

## Task 12: `extractUrls` + drag & drop na janela

**Files:**
- Modify: `src/gui/DropTargets.h`, `src/gui/DropTargets.cpp` (criados na Task 11),
  `src/gui/MainWindow.h`, `src/gui/MainWindow.cpp`, `tests/tst_gui.cpp`

**Interfaces:**
- Consumes: `isDownloadableScheme` (Task 11); `NewDownloadDialog(parent, prefill)` (Task 11);
  `deriveFileName` (`src/gui/UrlName.h`, Fase 2); `DownloadManager::addDownload`.
- Produces: `QList<QUrl> extractUrls(const QMimeData* mime)` em `DropTargets.h`;
  `MainWindow::defaultDir()`, `MainWindow::addUrlViaDialog(const QUrl&)`,
  `MainWindow::enqueue(const QUrl&, const QString&)` (privados; usados pela Task 13).

**Regra (spec §3.7):** aceita `text/uri-list` e `text/plain`. Uma URL → New pré-preenchido. Várias →
enfileira todas na pasta padrão. Nenhuma URL baixável → drop rejeitado, sem diálogo de erro.

`extractUrls` é **puro** e vai em `orbitgui_logic` (sem QtWidgets) — `QMimeData` é `QtCore`.

- [ ] **Step 1: Escrever os testes que falham (critério 14, parte)**

Em `tests/tst_gui.cpp`, adicionar `#include "DropTargets.h"`, `#include <QMimeData>` e:

```cpp
    void extractUrlsFromUriList() {
        QMimeData m;
        m.setUrls({QUrl("http://h/a.bin"), QUrl("ftp://h/b.bin")});
        const auto urls = extractUrls(&m);
        QCOMPARE(urls.size(), 2);
        QCOMPARE(urls.at(0), QUrl("http://h/a.bin"));
        QCOMPARE(urls.at(1), QUrl("ftp://h/b.bin"));
    }

    void extractUrlsFiltersUnsupportedSchemes() {
        QMimeData m;
        m.setUrls({QUrl("http://h/a.bin"), QUrl("file:///tmp/x"), QUrl("gopher://h/c")});
        const auto urls = extractUrls(&m);
        QCOMPARE(urls.size(), 1);
        QCOMPARE(urls.at(0), QUrl("http://h/a.bin"));
    }

    void extractUrlsFromPlainText() {
        QMimeData m;
        m.setText("olha esse link http://h/a.bin no meio do texto");
        const auto urls = extractUrls(&m);
        QCOMPARE(urls.size(), 1);
        QCOMPARE(urls.at(0), QUrl("http://h/a.bin"));
    }

    void extractUrlsFromMultiLineText() {
        QMimeData m;
        m.setText("http://h/a.bin\nftp://h/b.bin\nlixo\nhttps://h/c.bin");
        const auto urls = extractUrls(&m);
        QCOMPARE(urls.size(), 3);
    }

    void extractUrlsDeduplicates() {
        QMimeData m;
        m.setText("http://h/a.bin http://h/a.bin");
        const auto urls = extractUrls(&m);
        QCOMPARE(urls.size(), 1);
    }

    void extractUrlsEmptyWhenNothingDownloadable() {
        QMimeData m;
        m.setText("só texto, nenhum link");
        QVERIFY(extractUrls(&m).isEmpty());

        QMimeData m2;
        QVERIFY(extractUrls(&m2).isEmpty());
        QVERIFY(extractUrls(nullptr).isEmpty());
    }
```

- [ ] **Step 2: Rodar e ver falhar**

Run: `cmake --build build -j`
Expected: FALHA — `DropTargets.h: No such file or directory`.

- [ ] **Step 3: Acrescentar `extractUrls` a `src/gui/DropTargets.h`**

```cpp
#pragma once
#include <QList>
#include <QUrl>
class QMimeData;

bool isDownloadableScheme(const QUrl& u);       // Task 11

// Extrai as URLs baixáveis de um drop. Aceita text/uri-list e text/plain.
// Puro e sem QtWidgets: testável headless. Preserva a ordem e remove
// duplicatas. Lista vazia = drop deve ser rejeitado.
QList<QUrl> extractUrls(const QMimeData* mime);
```

- [ ] **Step 4: Implementar `extractUrls` em `src/gui/DropTargets.cpp`**

```cpp
#include "DropTargets.h"
#include <QMimeData>
#include <QRegularExpression>
#include <QSet>

static void appendIfNew(QList<QUrl>& out, QSet<QString>& seen, const QUrl& u) {
    if (!isDownloadableScheme(u)) return;
    const QString key = u.toString();
    if (seen.contains(key)) return;
    seen.insert(key);
    out.append(u);
}

QList<QUrl> extractUrls(const QMimeData* mime) {
    QList<QUrl>   out;
    QSet<QString> seen;
    if (!mime) return out;

    if (mime->hasUrls())
        for (const QUrl& u : mime->urls()) appendIfNew(out, seen, u);

    if (mime->hasText()) {
        // Varre o texto por tokens que pareçam URL. Arrastar uma SELEÇÃO do
        // navegador cai aqui (não em uri-list), e o texto pode ter lixo em volta.
        static const QRegularExpression re(R"((?:https?|ftp)://[^\s<>"']+)",
                                           QRegularExpression::CaseInsensitiveOption);
        auto it = re.globalMatch(mime->text());
        while (it.hasNext()) appendIfNew(out, seen, QUrl(it.next().captured(0)));
    }
    return out;
}
```

> **Por que `extractUrls` varre prosa e o clipboard (Task 13) não:** arrastar uma *seleção* do
> navegador entrega texto com lixo em volta — é o gesto normal. Copiar um parágrafo com link, não:
> lá a URL tem que estar limpa, senão o monitor dispara ao copiar qualquer texto.

- [ ] **Step 5: Rodar e ver passar**

Run: `cmake --build build -j && ctest --test-dir build --output-on-failure -R tst_gui`
Expected: PASS.

- [ ] **Step 6: Ligar o drop na `MainWindow`**

`src/gui/MainWindow.h`:

```cpp
protected:
    void dragEnterEvent(QDragEnterEvent* e) override;
    void dropEvent(QDropEvent* e) override;
private:
    QString defaultDir() const;
    void    addUrlViaDialog(const QUrl& prefill);
    void    enqueue(const QUrl& url, const QString& dir);
    QString m_lastDir;      // pasta padrão: última escolhida NESTA sessão (spec §3.7)
```

`src/gui/MainWindow.cpp` — no construtor: `setAcceptDrops(true);` e:

```cpp
#include "DropTargets.h"
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QStandardPaths>

// Pasta padrão (Auto e drop múltiplo): última escolhida no New nesta sessão,
// ou Downloads na primeira vez. Em memória — persistir config é da Fase 4.
QString MainWindow::defaultDir() const {
    if (!m_lastDir.isEmpty()) return m_lastDir;
    return QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
}

void MainWindow::dragEnterEvent(QDragEnterEvent* e) {
    if (!extractUrls(e->mimeData()).isEmpty()) e->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent* e) {
    const auto urls = extractUrls(e->mimeData());
    if (urls.isEmpty()) return;               // rejeitado, sem diálogo de erro
    e->acceptProposedAction();

    // Uma URL: o New pré-preenchido, p/ escolher pasta. Várias: perguntar N
    // vezes seria insuportável — vão todas p/ a pasta padrão (spec §3.7).
    if (urls.size() == 1) { addUrlViaDialog(urls.first()); return; }
    for (const QUrl& u : urls) enqueue(u, defaultDir());
}

void MainWindow::enqueue(const QUrl& url, const QString& dir) {
    m_mgr->addDownload(url, QDir(dir).filePath(deriveFileName(url)));   // UrlName.h (Fase 2)
}
```

Adicionar `#include "UrlName.h"` e `#include <QDir>`.

`onNew()` hoje constrói o `NewDownloadDialog`, chama `exec()` e, se aceito, faz `addDownload`.
Extraia esse corpo para `addUrlViaDialog(prefill)` e deixe `onNew()` como uma chamada. O ponto novo é
guardar `m_lastDir`:

```cpp
void MainWindow::onNew() { addUrlViaDialog(QUrl()); }

void MainWindow::addUrlViaDialog(const QUrl& prefill) {
    NewDownloadDialog d(this, prefill);
    if (d.exec() != QDialog::Accepted) return;
    // A pasta escolhida vira a padrão desta sessão (Auto e drop múltiplo usam).
    m_lastDir = QFileInfo(d.destPath()).absolutePath();
    m_mgr->addDownload(d.url(), d.destPath());
}
```

Adicionar `#include <QFileInfo>`.

- [ ] **Step 7: Suíte inteira**

Run: `cmake --build build -j && ctest --test-dir build --output-on-failure`
Expected: **7/7 verde**. `orbit-gui` sobe headless:

Run: `QT_QPA_PLATFORM=offscreen timeout 5 ./build/src/gui/orbit-gui; echo "exit=$?"`
Expected: sobe sem crash (`exit=124` do timeout é o esperado).

- [ ] **Step 8: Sem commit** — deixe no working tree e reporte.

---

## Task 13: `ClipboardWatcher` + menu Tools

**Files:**
- Create: `src/gui/ClipboardWatcher.h`, `src/gui/ClipboardWatcher.cpp`
- Modify: `src/gui/MainWindow.h`, `src/gui/MainWindow.cpp`, `src/gui/CMakeLists.txt`,
  `tests/tst_gui.cpp`

**Interfaces:**
- Consumes: `isDownloadableScheme` / `NewDownloadDialog::isValidDownloadUrl` (Tasks 11/12);
  `MainWindow::addUrlViaDialog`, `MainWindow::enqueue`, `MainWindow::defaultDir` (Task 12).
- Produces:
  - `std::optional<QUrl> shouldOffer(const QString& text, const QUrl& lastOffered, bool selfCopied)`
    (puro, em `ClipboardWatcher.h`)
  - `enum class ClipboardMode { Off, Ask, Auto, Notify }`
  - `class ClipboardWatcher : public QObject` com `setMode(ClipboardMode)`,
    `void markSelfCopy()`, sinal `urlDetected(const QUrl&)`

**Regra (spec §3.7):** 4 modos, `Tools → Clipboard monitor`, rádio, padrão **Off**, **sem persistir**.
Filtros: só `http`/`https`/`ftp`; ignora o que a própria app copiou; ignora repetição imediata da
última URL oferecida.

- [ ] **Step 1: Escrever os testes que falham (critério 14, parte)**

```cpp
    void shouldOfferAcceptsDownloadableUrl() {
        const auto r = shouldOffer("http://h/a.bin", QUrl(), false);
        QVERIFY(r.has_value());
        QCOMPARE(*r, QUrl("http://h/a.bin"));
    }

    void shouldOfferAcceptsFtp() {
        QVERIFY(shouldOffer("ftp://h/a.bin", QUrl(), false).has_value());
    }

    void shouldOfferTrimsWhitespace() {
        const auto r = shouldOffer("  http://h/a.bin \n", QUrl(), false);
        QVERIFY(r.has_value());
        QCOMPARE(*r, QUrl("http://h/a.bin"));
    }

    void shouldOfferRejectsNonUrl() {
        QVERIFY(!shouldOffer("bom dia", QUrl(), false).has_value());
        QVERIFY(!shouldOffer("", QUrl(), false).has_value());
        QVERIFY(!shouldOffer("file:///tmp/x", QUrl(), false).has_value());
    }

    void shouldOfferRejectsSelfCopy() {
        QVERIFY(!shouldOffer("http://h/a.bin", QUrl(), true).has_value());
    }

    void shouldOfferRejectsImmediateRepeat() {
        QVERIFY(!shouldOffer("http://h/a.bin", QUrl("http://h/a.bin"), false).has_value());
    }

    void shouldOfferAcceptsDifferentUrlAfterPrevious() {
        QVERIFY(shouldOffer("http://h/b.bin", QUrl("http://h/a.bin"), false).has_value());
    }

    // Texto com lixo em volta NÃO conta: o clipboard tem que ser a URL. (Drop
    // é que varre texto — spec §3.7.)
    void shouldOfferRejectsUrlBuriedInProse() {
        QVERIFY(!shouldOffer("veja http://h/a.bin agora", QUrl(), false).has_value());
    }
```

> **Decisão registrada:** varrer prosa no clipboard dispararia o diálogo ao copiar qualquer parágrafo
> com link — irritante. O drop varre (é o gesto de arrastar seleção); o clipboard exige a URL limpa.
> O teste `shouldOfferRejectsUrlBuriedInProse` trava essa decisão.

- [ ] **Step 2: Rodar e ver falhar**

Run: `cmake --build build -j`
Expected: FALHA — `ClipboardWatcher.h: No such file or directory`.

- [ ] **Step 3: Implementar `src/gui/ClipboardWatcher.h`**

```cpp
#pragma once
#include <QObject>
#include <QUrl>
#include <optional>

enum class ClipboardMode { Off, Ask, Auto, Notify };

// Decide se um texto copiado merece virar oferta de download. Puro: sem
// clipboard, sem widgets, testável direto.
//   - só http/https/ftp
//   - ignora o que a própria app copiou (selfCopied)
//   - ignora repetição imediata da última URL oferecida (lastOffered)
//   - exige a URL limpa (prosa com link no meio não conta)
std::optional<QUrl> shouldOffer(const QString& text, const QUrl& lastOffered, bool selfCopied);

class ClipboardWatcher : public QObject {
    Q_OBJECT
public:
    explicit ClipboardWatcher(QObject* parent = nullptr);
    void setMode(ClipboardMode m) { m_mode = m; }
    ClipboardMode mode() const    { return m_mode; }
    void markSelfCopy();          // chame ANTES de a app escrever no clipboard
signals:
    void urlDetected(const QUrl& url);
private:
    void onClipboardChanged();
    ClipboardMode m_mode = ClipboardMode::Off;   // padrão Off (spec §3.7)
    QUrl          m_lastOffered;
    bool          m_selfCopied = false;
};
```

- [ ] **Step 4: Implementar `src/gui/ClipboardWatcher.cpp`**

```cpp
#include "ClipboardWatcher.h"
#include "DropTargets.h"        // isDownloadableScheme
#include <QClipboard>
#include <QGuiApplication>

std::optional<QUrl> shouldOffer(const QString& text, const QUrl& lastOffered, bool selfCopied) {
    if (selfCopied) return std::nullopt;
    const QString t = text.trimmed();
    if (t.isEmpty()) return std::nullopt;
    const QUrl u(t);
    if (!isDownloadableScheme(u)) return std::nullopt;
    if (lastOffered.isValid() && u == lastOffered) return std::nullopt;
    return u;
}

ClipboardWatcher::ClipboardWatcher(QObject* parent) : QObject(parent) {
    connect(QGuiApplication::clipboard(), &QClipboard::dataChanged,
            this, &ClipboardWatcher::onClipboardChanged);
}

void ClipboardWatcher::markSelfCopy() { m_selfCopied = true; }

void ClipboardWatcher::onClipboardChanged() {
    const bool self = m_selfCopied;
    m_selfCopied = false;                       // consome a marca
    if (m_mode == ClipboardMode::Off) return;

    const auto u = shouldOffer(QGuiApplication::clipboard()->text(), m_lastOffered, self);
    if (!u) return;
    m_lastOffered = *u;
    emit urlDetected(*u);
}
```

- [ ] **Step 5: CMake**

`src/gui/CMakeLists.txt` — `ClipboardWatcher.cpp` em **`orbitgui`** (usa `QGuiApplication`), não em
`orbitgui_logic`. Se isso quebrar os testes puros de `shouldOffer` em `tst_gui` (que linka
`orbitgui`), não quebra — `tst_gui` já linka `orbitgui`.

- [ ] **Step 6: Menu Tools na `MainWindow`**

`src/gui/MainWindow.h`:

```cpp
private slots:
    void onClipboardUrl(const QUrl& url);
private:
    ClipboardWatcher* m_clip;
```

`src/gui/MainWindow.cpp` — no construtor, depois da toolbar:

```cpp
#include "ClipboardWatcher.h"
#include <QActionGroup>
#include <QMenuBar>
#include <QStatusBar>

    m_clip = new ClipboardWatcher(this);
    connect(m_clip, &ClipboardWatcher::urlDetected, this, &MainWindow::onClipboardUrl);

    // Menu Tools -> Clipboard monitor: rádio de 4, padrão Off, SEM persistir
    // (settings.json é da Fase 4 — spec §3.7).
    QMenu* tools = menuBar()->addMenu(tr("&Tools"));
    QMenu* clip  = tools->addMenu(tr("&Clipboard monitor"));
    auto*  group = new QActionGroup(this);
    group->setExclusive(true);

    struct { const char* label; ClipboardMode mode; } items[] = {
        { QT_TR_NOOP("Off"),                  ClipboardMode::Off    },
        { QT_TR_NOOP("Ask (open New dialog)"), ClipboardMode::Ask    },
        { QT_TR_NOOP("Add automatically"),     ClipboardMode::Auto   },
        { QT_TR_NOOP("Notify in status bar"),  ClipboardMode::Notify },
    };
    for (const auto& it : items) {
        QAction* a = clip->addAction(tr(it.label));
        a->setCheckable(true);
        a->setChecked(it.mode == ClipboardMode::Off);      // padrão
        group->addAction(a);
        const ClipboardMode m = it.mode;
        connect(a, &QAction::triggered, this, [this, m] { m_clip->setMode(m); });
    }
```

E o slot:

```cpp
void MainWindow::onClipboardUrl(const QUrl& url) {
    switch (m_clip->mode()) {
        case ClipboardMode::Off:
            return;
        case ClipboardMode::Ask:
            addUrlViaDialog(url);
            return;
        case ClipboardMode::Auto:
            enqueue(url, defaultDir());
            return;
        case ClipboardMode::Notify:
            showLinkNotification(url);
            return;
    }
}

// Notificação CLICÁVEL na barra de status (spec §3.7). QStatusBar::showMessage
// só mostra texto morto — não serve. Um QLabel com rich text emite linkActivated
// quando clicado; ele se remove sozinho após 8s ou ao ser usado.
void MainWindow::showLinkNotification(const QUrl& url) {
    if (m_notice) { statusBar()->removeWidget(m_notice); m_notice->deleteLater(); }

    m_notice = new QLabel(tr("Link detected: <a href=\"#\">%1</a>")
                              .arg(url.toString().toHtmlEscaped()), this);
    m_notice->setTextFormat(Qt::RichText);
    statusBar()->addWidget(m_notice);
    m_notice->show();

    connect(m_notice, &QLabel::linkActivated, this, [this, url] {
        clearLinkNotification();
        addUrlViaDialog(url);
    });
    QTimer::singleShot(8000, this, &MainWindow::clearLinkNotification);
}

void MainWindow::clearLinkNotification() {
    if (!m_notice) return;
    statusBar()->removeWidget(m_notice);
    m_notice->deleteLater();
    m_notice = nullptr;
}
```

Os membros correspondentes em `src/gui/MainWindow.h`:

```cpp
private:
    void    showLinkNotification(const QUrl& url);
    void    clearLinkNotification();
    QLabel* m_notice = nullptr;      // notificação clicável do modo Notify (ou nullptr)
```

Adicionar `#include <QTimer>` ao `.cpp`.

> **`QTimer::singleShot` com `this` como contexto** garante que o timer morra junto com a janela — o
> lambda não pode disparar sobre uma `MainWindow` já destruída.

- [ ] **Step 7: Rodar e ver passar**

Run: `cmake --build build -j && ctest --test-dir build --output-on-failure -R tst_gui`
Expected: PASS.

- [ ] **Step 8: Suíte inteira**

Run: `ctest --test-dir build --output-on-failure`
Expected: **7/7 verde**.

- [ ] **Step 9: Sem commit** — deixe no working tree e reporte. Diga como implementou o Notify.

---

## Task 14: `CredentialsDialog` + ligação na GUI

**Files:**
- Create: `src/gui/CredentialsDialog.h`, `src/gui/CredentialsDialog.cpp`
- Modify: `src/gui/MainWindow.h`, `src/gui/MainWindow.cpp`, `src/gui/CMakeLists.txt`,
  `tests/tst_gui.cpp`

**Interfaces:**
- Consumes: `DownloadManager::credentialsRequired`, `DownloadManager::provideCredentials` (Task 10).
- Produces: `class CredentialsDialog : public QDialog` com
  `explicit CredentialsDialog(const QString& host, QWidget* parent = nullptr)`,
  `QString user() const`, `QString pass() const`.

- [ ] **Step 1: Escrever o teste que falha**

```cpp
    void credentialsDialogReturnsTypedValues() {
        CredentialsDialog d("ftp.example.org");
        // Os campos são achados por objectName — defina-os na implementação.
        auto* user = d.findChild<QLineEdit*>("userEdit");
        auto* pass = d.findChild<QLineEdit*>("passEdit");
        QVERIFY(user);
        QVERIFY(pass);
        QTest::keyClicks(user, "bob");
        QTest::keyClicks(pass, "secret");
        QCOMPARE(d.user(), QString("bob"));
        QCOMPARE(d.pass(), QString("secret"));
        QCOMPARE(pass->echoMode(), QLineEdit::Password);   // senha não aparece na tela
    }

    void credentialsDialogShowsHost() {
        CredentialsDialog d("ftp.example.org");
        bool found = false;
        for (auto* l : d.findChildren<QLabel*>())
            if (l->text().contains("ftp.example.org")) { found = true; break; }
        QVERIFY2(found, "o diálogo precisa dizer a QUEM está entregando a senha");
    }
```

Adicionar `#include "CredentialsDialog.h"`, `#include <QLineEdit>`, `#include <QLabel>` em
`tests/tst_gui.cpp`.

- [ ] **Step 2: Rodar e ver falhar**

Run: `cmake --build build -j`
Expected: FALHA — `CredentialsDialog.h: No such file or directory`.

- [ ] **Step 3: Implementar `src/gui/CredentialsDialog.h`**

```cpp
#pragma once
#include <QDialog>
class QLineEdit;

// Pede user/senha para um host FTP. As credenciais vivem SÓ em memória
// (spec §3.6): nada aqui é persistido.
class CredentialsDialog : public QDialog {
    Q_OBJECT
public:
    explicit CredentialsDialog(const QString& host, QWidget* parent = nullptr);
    QString user() const;
    QString pass() const;
private:
    QLineEdit* m_user;
    QLineEdit* m_pass;
};
```

- [ ] **Step 4: Implementar `src/gui/CredentialsDialog.cpp`**

```cpp
#include "CredentialsDialog.h"
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>

CredentialsDialog::CredentialsDialog(const QString& host, QWidget* parent)
    : QDialog(parent) {
    setWindowTitle(tr("Authentication required"));

    auto* form = new QFormLayout(this);
    form->addRow(new QLabel(tr("The server %1 requires a login.").arg(host), this));

    m_user = new QLineEdit(this);
    m_user->setObjectName("userEdit");
    form->addRow(tr("User:"), m_user);

    m_pass = new QLineEdit(this);
    m_pass->setObjectName("passEdit");
    m_pass->setEchoMode(QLineEdit::Password);
    form->addRow(tr("Password:"), m_pass);

    auto* box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(box, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);
    form->addRow(box);
}

QString CredentialsDialog::user() const { return m_user->text(); }
QString CredentialsDialog::pass() const { return m_pass->text(); }
```

- [ ] **Step 5: CMake**

`src/gui/CMakeLists.txt` — em `orbitgui`:

```cmake
    CredentialsDialog.cpp
```

- [ ] **Step 6: Ligar na `MainWindow`**

`src/gui/MainWindow.h`:

```cpp
private slots:
    void onCredentialsRequired(const QUuid& id, const QString& host);
```

`src/gui/MainWindow.cpp` — no construtor:

```cpp
    connect(m_mgr, &DownloadManager::credentialsRequired,
            this, &MainWindow::onCredentialsRequired);
```

E o slot:

```cpp
void MainWindow::onCredentialsRequired(const QUuid& id, const QString& host) {
    CredentialsDialog d(host, this);
    if (d.exec() != QDialog::Accepted) return;   // cancelou: a task fica Paused,
                                                 // o botão Start tenta de novo (spec §3.6)
    m_mgr->provideCredentials(id, d.user(), d.pass());
}
```

- [ ] **Step 7: Rodar e ver passar**

Run: `cmake --build build -j && ctest --test-dir build --output-on-failure -R tst_gui`
Expected: PASS.

- [ ] **Step 8: Suíte inteira + smoke da GUI**

Run: `ctest --test-dir build --output-on-failure`
Expected: **7/7 verde**, zero warnings novos.

Run: `QT_QPA_PLATFORM=offscreen timeout 5 ./build/src/gui/orbit-gui; echo "exit=$?"`
Expected: sobe sem crash (`exit=124` do timeout é o esperado).

- [ ] **Step 9: Sem commit** — deixe no working tree e reporte.

---

## Verificação manual (critérios 15–16)

Com `orbit-gui` rodando, contra um servidor FTP público real:

1. Baixar um arquivo FTP: progride, grade de blocos evolui por segmento, arquivo íntegro.
2. Pausar e retomar no meio: retoma dos offsets, arquivo final íntegro.
3. Servidor com auth: diálogo aparece, credenciais corretas completam o download.
4. Os 4 modos de clipboard fazem o que dizem.
5. Arrastar um link do navegador → New pré-preenchido; arrastar vários → todos enfileirados.

**Atenção ao §9.2 da spec:** 4 segmentos × 3 downloads = 24 sockets contra servidores que costumam
limitar a 2–5 por IP. Se o item 1 vier cheio de retry, é o risco previsto se materializando — a
resposta é um cap de segmentos por protocolo, deliberadamente adiado até haver evidência.
