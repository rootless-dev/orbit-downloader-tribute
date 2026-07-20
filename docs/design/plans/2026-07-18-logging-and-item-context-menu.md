# Logging detalhado + Menu de contexto do item — Implementation Plan

**Goal:** Adicionar logging detalhado (app.log + arquivo por download, nível de segmento), um menu de contexto na tabela de downloads (Start/Stop/Cancel/Delete/Move/Open/Open-folder/Priority), e recolorir a grade de progresso no padrão do Orbit clássico.

**Architecture:** O logging nasce no core (`orbitcore`, só QtCore) numa classe `Logger` que grava arquivos e emite um sinal por linha; a GUI lê o arquivo do download selecionado e recebe linhas ao vivo. O core ganha o estado `Cancelled`, um campo `Priority` persistido e `moveFiles`; o `pump()` passa a promover a fila por prioridade. A GUI ganha o menu de contexto, a coluna Priority, double-click, "Clear completed" e notificação de conclusão. A grade troca a paleta por-segmento por cores por-estado sobre fundo claro.

**Tech Stack:** C++20, Qt 6.11 (Core, Network, Widgets, Test), CMake, QtTest (headless, `QT_QPA_PLATFORM=offscreen` para a GUI).

## Global Constraints

- **C++20**; `CMAKE_CXX_EXTENSIONS OFF`; `AUTOMOC ON` (copiado de `CMakeLists.txt`).
- **`orbitcore` só depende de `Qt6::Core` e `Qt6::Network`** — nunca de QtWidgets. `Logger` vive no core e usa apenas QtCore.
- **Zero warnings.** Adicionar `DownloadState::Cancelled` torna não-exaustivo todo `switch`/array indexado por `DownloadState` sem `default`. Toda tarefa que toque o enum DEVE manter todos esses pontos exaustivos (ver Task 2).
- **Não existe arquivo `.part`.** O download é escrito direto no `destPath` (pré-alocado ao tamanho total em `DownloadTask::beginSegments`). O "arquivo parcial" é o próprio `destPath`; o sidecar é `destPath + ".meta"` (`Persistence::metaPath`).
- **Persistência retrocompatível.** Ler um `downloads.json` antigo (sem `priority`) não pode falhar; `state` é serializado como `int` (`Cancelled` deve ser o ÚLTIMO enumerador = 6, para não deslocar os valores existentes 0–5).
- **Commits:** mensagens em inglês, Conventional Commits, **sem** coautoria (Co-Authored-By).
- **Regra TDD:** teste que falha → roda e vê falhar → implementação mínima → roda e vê passar → commit.
- **Build/test:** `cmake --build build` e `ctest --test-dir build --output-on-failure`. (O diretório `build/` já existe.)

---

## Estrutura de arquivos

**Core (`src/core/`):**
- Create: `Logger.h`, `Logger.cpp` — logging (app.log + por-download), sinal `lineLogged`, rotação.
- Modify: `DownloadTypes.h` — `DownloadState::Cancelled`, `enum class Priority`, helpers `stateName`/`priorityToString`/`priorityFromString`.
- Modify: `Persistence.h` (campo `priority` em `DownloadRecord`), `Persistence.cpp` (serialização).
- Modify: `DownloadTask.h/.cpp` — `cancel()`, `priority()/setPriority()`, `setDestPath()`, `setLogger()`, instrumentação, `record()/restore()`.
- Modify: `DownloadManager.h/.cpp` — ctor recebe `Logger*`; `cancel()`, `setPriority()`, `moveFiles()`, `pump()` por prioridade, instrumentação de estado.
- Modify: `CMakeLists.txt` (core) — acrescentar `Logger.cpp`.

**GUI logic (`src/gui/`, lib `orbitgui_logic`):**
- Modify: `GridGeometry.h/.cpp` — `CellKind::Active` + detecção da célula-cabeça.

**GUI widgets (`src/gui/`, lib `orbitgui`):**
- Modify: `ProgressGridWidget.cpp` — cores do Orbit (fundo claro; cinza/azul/laranja/vermelho).
- Modify: `DownloadTableModel.h/.cpp` — coluna `Priority`; `stateText` com `Cancelled`.
- Modify: `MainWindow.h/.cpp` — `Logger*`, aba Log por-download, Tools→Application Log, menu de contexto, double-click, Clear completed, notificação.
- Modify: `main_gui.cpp` — cria o `Logger`, injeta no manager e na janela.

**Tests (`tests/`):**
- Create: `tst_logger.cpp` + entrada no `tests/CMakeLists.txt`.
- Modify: `tst_persistence.cpp` (round-trip de `priority`/`Cancelled`).
- Modify: `tst_download.cpp` (cancel, priority-ordering, move, logging).
- Modify: `tst_gui.cpp` (grid Active, coluna Priority, menu enable/disable, double-click, clear completed).

---

## Task 1: `Logger` no core

**Files:**
- Create: `src/core/Logger.h`, `src/core/Logger.cpp`
- Modify: `src/core/CMakeLists.txt` (adicionar `Logger.cpp`)
- Test: `tests/tst_logger.cpp` (novo) + `tests/CMakeLists.txt`

**Interfaces:**
- Produces:
  - `enum class LogLevel { Debug, Info, Warn, Error };`
  - `class Logger : QObject` com:
    - `explicit Logger(const QString& dataDir, QObject* parent = nullptr);`
    - `void logApp(LogLevel, const QString& msg);`
    - `void logTask(const QUuid& id, const QString& destPath, LogLevel, const QString& msg);`
    - `QString logsDir() const;`
    - `QString taskLogPath(const QUuid& id, const QString& destPath) const;`
    - `signals: void lineLogged(const QUuid& id, const QString& formattedLine);`

- [ ] **Step 1: Escrever o teste que falha** — `tests/tst_logger.cpp`

```cpp
#include <QtTest>
#include <QTemporaryDir>
#include <QSignalSpy>
#include <QFile>
#include "Logger.h"

static QString readAll(const QString& path) {
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
}

class TstLogger : public QObject {
    Q_OBJECT
private slots:
    void appLogWritesFormattedLine() {
        QTemporaryDir dir;
        Logger log(dir.path());
        QSignalSpy spy(&log, &Logger::lineLogged);
        log.logApp(LogLevel::Info, "hello");
        const QString content = readAll(log.logsDir() + "/app.log");
        QVERIFY(content.contains("[INFO] hello"));
        QVERIFY(content.contains("-"));                       // tem data
        QCOMPARE(spy.count(), 1);
        QVERIFY(spy.at(0).at(0).value<QUuid>().isNull());     // app => id nulo
    }

    void taskLogGoesToPerDownloadFile() {
        QTemporaryDir dir;
        Logger log(dir.path());
        const QUuid id = QUuid::createUuid();
        const QString dest = "/tmp/movie.flv";
        QSignalSpy spy(&log, &Logger::lineLogged);
        log.logTask(id, dest, LogLevel::Warn, "retry seg 2");
        const QString path = log.taskLogPath(id, dest);
        QVERIFY(path.endsWith(".log"));
        QVERIFY(path.contains("movie"));
        QVERIFY(readAll(path).contains("[WARN] retry seg 2"));
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(0).value<QUuid>(), id);
    }

    void taskLogPathIsDeterministic() {
        QTemporaryDir dir;
        Logger log(dir.path());
        const QUuid id = QUuid::createUuid();
        QCOMPARE(log.taskLogPath(id, "/a/movie.flv"), log.taskLogPath(id, "/a/movie.flv"));
    }

    void appLogRotatesWhenTooBig() {
        QTemporaryDir dir;
        Logger log(dir.path());
        for (int i = 0; i < 60000; ++i) log.logApp(LogLevel::Debug, QString(200, 'x'));
        QVERIFY(QFile::exists(log.logsDir() + "/app.log.1"));   // rotacionou
    }
};

QTEST_MAIN(TstLogger)
#include "tst_logger.moc"
```

- [ ] **Step 2: Registrar o teste no CMake** — em `tests/CMakeLists.txt`, ao final:

```cmake
add_executable(tst_logger tst_logger.cpp)
target_link_libraries(tst_logger PRIVATE orbitcore Qt6::Test)
add_test(NAME tst_logger COMMAND tst_logger)
```

- [ ] **Step 3: Rodar e ver falhar (não compila: `Logger.h` não existe)**

Run: `cmake -S . -B build && cmake --build build --target tst_logger 2>&1 | tail -5`
Expected: erro de compilação `Logger.h: No such file or directory`.

- [ ] **Step 4: Implementar `Logger`** — `src/core/Logger.h`

```cpp
#pragma once
#include <QObject>
#include <QString>
#include <QUuid>

enum class LogLevel { Debug, Info, Warn, Error };

// Logging do core: grava um app.log global + um arquivo por download em
// <dataDir>/logs/, e emite lineLogged por linha para a GUI mostrar ao vivo.
class Logger : public QObject {
    Q_OBJECT
public:
    explicit Logger(const QString& dataDir, QObject* parent = nullptr);

    void logApp(LogLevel level, const QString& msg);
    void logTask(const QUuid& id, const QString& destPath, LogLevel level, const QString& msg);

    QString logsDir() const { return m_logsDir; }
    QString taskLogPath(const QUuid& id, const QString& destPath) const;

signals:
    void lineLogged(const QUuid& id, const QString& formattedLine);

private:
    static QString levelStr(LogLevel l);
    QString formatLine(LogLevel level, const QString& msg) const;
    void    appendToFile(const QString& path, const QString& line);

    QString m_logsDir;
    qint64  m_appRotateBytes = 5LL * 1024 * 1024;
};
```

- [ ] **Step 5: Implementar `Logger`** — `src/core/Logger.cpp`

```cpp
#include "Logger.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QDateTime>

Logger::Logger(const QString& dataDir, QObject* parent)
    : QObject(parent), m_logsDir(dataDir + "/logs") {
    QDir().mkpath(m_logsDir);
}

QString Logger::levelStr(LogLevel l) {
    switch (l) {
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info:  return "INFO";
        case LogLevel::Warn:  return "WARN";
        case LogLevel::Error: return "ERROR";
    }
    return "INFO";
}

QString Logger::formatLine(LogLevel level, const QString& msg) const {
    const QString ts = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss.zzz");
    return QString("%1 [%2] %3").arg(ts, levelStr(level), msg);
}

void Logger::appendToFile(const QString& path, const QString& line) {
    QFile f(path);
    if (!f.open(QIODevice::Append | QIODevice::Text)) return;   // best-effort
    f.write(line.toUtf8());
    f.write("\n");
}

// Sanitiza o basename do destino e anexa os 8 primeiros hex do id -> único e legível.
QString Logger::taskLogPath(const QUuid& id, const QString& destPath) const {
    QString base = QFileInfo(destPath).completeBaseName();
    base.replace(QRegularExpression("[^A-Za-z0-9._-]"), "_");
    if (base.isEmpty()) base = "download";
    const QString shortId = id.toString(QUuid::WithoutBraces).left(8);
    return m_logsDir + "/" + base + "-" + shortId + ".log";
}

void Logger::logApp(LogLevel level, const QString& msg) {
    const QString appLog = m_logsDir + "/app.log";
    if (QFileInfo(appLog).size() >= m_appRotateBytes) {
        QFile::remove(appLog + ".1");
        QFile::rename(appLog, appLog + ".1");
    }
    const QString line = formatLine(level, msg);
    appendToFile(appLog, line);
    emit lineLogged(QUuid(), line);
}

void Logger::logTask(const QUuid& id, const QString& destPath, LogLevel level, const QString& msg) {
    const QString line = formatLine(level, msg);
    appendToFile(taskLogPath(id, destPath), line);
    emit lineLogged(id, line);
}
```

Adicionar o include no topo do `.cpp`: `#include <QRegularExpression>`.

- [ ] **Step 6: Adicionar ao build do core** — em `src/core/CMakeLists.txt`, na lista de fontes de `orbitcore`, após `RateLimiter.cpp`:

```cmake
  RateLimiter.cpp
  Logger.cpp
```

- [ ] **Step 7: Rodar e ver passar**

Run: `cmake --build build --target tst_logger && ctest --test-dir build -R tst_logger --output-on-failure`
Expected: PASS (4/4).

- [ ] **Step 8: Commit**

```bash
git add src/core/Logger.h src/core/Logger.cpp src/core/CMakeLists.txt tests/tst_logger.cpp tests/CMakeLists.txt
git commit -m "feat(core): add Logger with app.log, per-download logs, and rotation"
```

---

## Task 2: Estado `Cancelled` + enum `Priority` + persistência

**Files:**
- Modify: `src/core/DownloadTypes.h`
- Modify: `src/core/Persistence.h:5-13` (struct `DownloadRecord`), `src/core/Persistence.cpp:60-93` (write/readSession)
- Modify: `src/gui/DownloadTableModel.cpp:73-83` (`stateText` — exaustivo)
- Modify: `src/gui/MainWindow.cpp:284` (array `names[]` — não pode indexar fora)
- Test: `tests/tst_persistence.cpp`

**Interfaces:**
- Produces:
  - `DownloadState::Cancelled` (último enumerador, valor 6).
  - `enum class Priority { High, Normal, Low };`
  - `inline const char* stateName(DownloadState);`
  - `inline QString priorityToString(Priority);` / `inline Priority priorityFromString(const QString&);`
  - `DownloadRecord::priority` (default `Priority::Normal`).

- [ ] **Step 1: Escrever o teste que falha** — acrescentar em `tests/tst_persistence.cpp` (dentro da classe de teste; siga o padrão de `QTemporaryDir`/`writeSession`/`readSession` já usado no arquivo):

```cpp
    void sessionRoundTripsPriorityAndCancelled() {
        QTemporaryDir dir;
        const QString path = dir.path() + "/downloads.json";
        DownloadRecord r;
        r.id = QUuid::createUuid();
        r.url = QUrl("http://example.com/f.bin");
        r.destPath = dir.path() + "/f.bin";
        r.state = DownloadState::Cancelled;
        r.priority = Priority::High;
        QVERIFY(Persistence::writeSession(path, {r}));
        const auto back = Persistence::readSession(path);
        QCOMPARE(back.size(), 1);
        QCOMPARE(back[0].state, DownloadState::Cancelled);
        QCOMPARE(back[0].priority, Priority::High);
    }

    void legacySessionDefaultsPriorityToNormal() {
        QTemporaryDir dir;
        const QString path = dir.path() + "/downloads.json";
        // JSON sem o campo "priority" (formato antigo):
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(R"([{"id":"11111111-1111-1111-1111-111111111111","url":"http://x/f",)"
                R"("destPath":"/tmp/f","totalBytes":10,"supportsRange":true,"state":3,"segmentCount":4}])");
        f.close();
        const auto back = Persistence::readSession(path);
        QCOMPARE(back.size(), 1);
        QCOMPARE(back[0].priority, Priority::Normal);
    }
```

Garanta os includes `<QTemporaryDir>` e `<QFile>` no topo de `tst_persistence.cpp` (adicione se faltarem).

- [ ] **Step 2: Rodar e ver falhar**

Run: `cmake --build build --target tst_persistence 2>&1 | tail -5`
Expected: erro de compilação (`priority` não é membro de `DownloadRecord`; `Priority` não declarado).

- [ ] **Step 3: Estender o enum e helpers** — em `src/core/DownloadTypes.h`, trocar a linha 8 e adicionar helpers logo abaixo:

```cpp
enum class DownloadState { Queued, Connecting, Downloading, Paused, Completed, Error, Cancelled };
enum class Priority { High, Normal, Low };

inline const char* stateName(DownloadState s) {
    switch (s) {
        case DownloadState::Queued:      return "Queued";
        case DownloadState::Connecting:  return "Connecting";
        case DownloadState::Downloading: return "Downloading";
        case DownloadState::Paused:      return "Paused";
        case DownloadState::Completed:   return "Completed";
        case DownloadState::Error:       return "Error";
        case DownloadState::Cancelled:   return "Cancelled";
    }
    return "Unknown";
}

inline QString priorityToString(Priority p) {
    switch (p) {
        case Priority::High:   return QStringLiteral("High");
        case Priority::Low:    return QStringLiteral("Low");
        case Priority::Normal: return QStringLiteral("Normal");
    }
    return QStringLiteral("Normal");
}

inline Priority priorityFromString(const QString& s) {
    if (s == QLatin1String("High")) return Priority::High;
    if (s == QLatin1String("Low"))  return Priority::Low;
    return Priority::Normal;
}
```

- [ ] **Step 4: Campo `priority` no registro** — em `src/core/Persistence.h`, dentro de `struct DownloadRecord` (após `segmentCount`):

```cpp
    int           segmentCount  = 4;
    Priority      priority      = Priority::Normal;
```

- [ ] **Step 5: Serializar** — em `src/core/Persistence.cpp`, em `writeSession` acrescentar a chave no `QJsonObject` (após `segmentCount`):

```cpp
            {"segmentCount", r.segmentCount},
            {"priority", priorityToString(r.priority)}});
```

E em `readSession`, após `r.segmentCount = ...`:

```cpp
        r.segmentCount  = o.value("segmentCount").toInt();
        r.priority      = priorityFromString(o.value("priority").toString("Normal"));
```

- [ ] **Step 6: Manter switches exaustivos (zero warnings)** — em `src/gui/DownloadTableModel.cpp`, na função `stateText` (linha ~73), adicionar antes do fecho do switch:

```cpp
        case DownloadState::Completed:   return "Completed";
        case DownloadState::Error:       return "Error";
        case DownloadState::Cancelled:   return "Cancelled";
    }
```

E em `src/gui/MainWindow.cpp` na `onStateChanged` (linha ~284), incluir o novo rótulo no array (evita índice fora do vetor quando `state==6`):

```cpp
    static const char* names[] = {"Queued","Connecting","Downloading","Paused","Completed","Error","Cancelled"};
```

- [ ] **Step 7: Rodar e ver passar (e sem warnings)**

Run: `cmake --build build 2>&1 | grep -i warning; ctest --test-dir build -R tst_persistence --output-on-failure`
Expected: nenhuma linha de warning; tst_persistence PASS.

- [ ] **Step 8: Commit**

```bash
git add src/core/DownloadTypes.h src/core/Persistence.h src/core/Persistence.cpp src/gui/DownloadTableModel.cpp src/gui/MainWindow.cpp tests/tst_persistence.cpp
git commit -m "feat(core): add Cancelled state and persisted Priority to the session"
```

---

## Task 3: `cancel()` na task e no manager

**Files:**
- Modify: `src/core/DownloadTask.h`, `src/core/DownloadTask.cpp`
- Modify: `src/core/DownloadManager.h`, `src/core/DownloadManager.cpp`
- Test: `tests/tst_download.cpp`

**Interfaces:**
- Consumes: `DownloadState::Cancelled`, `Persistence::removeMeta`, `Persistence::metaPath` (Task 2).
- Produces:
  - `void DownloadTask::cancel();`
  - `void DownloadTask::setDestPath(const QString&);` (usado também na Task 5)
  - `void DownloadManager::cancel(const QUuid& id);`
  - `resume()`/`requeue()` passam a aceitar `Cancelled` como ponto de partida (recomeça do zero).

- [ ] **Step 1: Escrever o teste que falha** — em `tests/tst_download.cpp`, novo slot (usa os helpers `makeTempDir`, `waitForState`, `readFile` já presentes no arquivo):

```cpp
    void cancelDiscardsPartialAndKeepsItemCancelled() {
        const QByteArray big = makeBody(300000);
        TestServer srv(big);
        QVERIFY(srv.listen());
        const QString dir = makeTempDir();
        EngineConfig cfg;
        cfg.maxBytesPerSec = 50000;                 // throttle: segura em Downloading
        DownloadManager mgr(cfg, dir);
        const QUuid id = mgr.addDownload(srv.url("/ranged"), dir + "/movie.bin");
        QVERIFY(waitForState(mgr, id, DownloadState::Downloading, 5000));
        QVERIFY(QFile::exists(dir + "/movie.bin"));                 // pré-alocado ao iniciar
        mgr.cancel(id);
        DownloadTask* t = mgr.taskById(id);
        QVERIFY(t);
        QCOMPARE(t->state(), DownloadState::Cancelled);            // fica na lista
        QVERIFY(!QFile::exists(dir + "/movie.bin"));               // parcial apagado
        QVERIFY(!QFile::exists(Persistence::metaPath(dir + "/movie.bin")));
    }

    void startFromCancelledRestartsFromZero() {
        const QByteArray big = makeBody(300000);
        TestServer srv(big);
        QVERIFY(srv.listen());
        const QString dir = makeTempDir();
        EngineConfig cfg;
        cfg.maxBytesPerSec = 50000;
        DownloadManager mgr(cfg, dir);
        const QUuid id = mgr.addDownload(srv.url("/ranged"), dir + "/m.bin");
        QVERIFY(waitForState(mgr, id, DownloadState::Downloading, 5000));
        mgr.cancel(id);
        QCOMPARE(mgr.taskById(id)->state(), DownloadState::Cancelled);
        mgr.resume(id);                                            // Start de Cancelled
        QVERIFY(waitForState(mgr, id, DownloadState::Completed, 15000));  // throttle ~6s
        QCOMPARE(readFile(dir + "/m.bin"), big);                   // baixou tudo de novo
    }
```

Garanta `#include "Persistence.h"` no topo de `tst_download.cpp` (adicione se faltar).

- [ ] **Step 2: Rodar e ver falhar**

Run: `cmake --build build --target tst_download 2>&1 | tail -5`
Expected: erro de compilação (`cancel` não é membro de `DownloadManager`).

- [ ] **Step 3: `cancel()`/`setDestPath()` na task** — em `src/core/DownloadTask.h`, declarar (após `requeue();`):

```cpp
    void requeue();
    void cancel();
    void setDestPath(const QString& path);
    Priority priority() const { return m_priority; }
    void     setPriority(Priority p) { m_priority = p; }
```

E o membro (na seção private, junto aos outros campos):

```cpp
    Priority               m_priority = Priority::Normal;
```

- [ ] **Step 4: Implementar na task** — em `src/core/DownloadTask.cpp`, após `requeue()`:

```cpp
void DownloadTask::cancel() {
    for (auto* w : m_workers) { w->stop(); w->deleteLater(); }
    m_workers.clear();
    if (m_metaTimer) m_metaTimer->stop();
    if (m_progressTimer) m_progressTimer->stop();
    m_progressPending = false;
    if (m_file) { m_file->close(); delete m_file; m_file = nullptr; }
    QFile::remove(m_destPath);                 // parcial = próprio destPath
    Persistence::removeMeta(m_destPath);
    m_segments.clear();                        // -> Start recomeça do zero
    m_completedCount = 0;
    m_probed = false;
    m_totalBytes = -1;
    setState(DownloadState::Cancelled);
}

void DownloadTask::setDestPath(const QString& path) {
    if (m_file) { m_file->close(); delete m_file; m_file = nullptr; }
    m_destPath = path;
}
```

Ajustar `record()` (final do arquivo) para incluir a prioridade:

```cpp
    r.state = m_state; r.segmentCount = m_segmentCount;
    r.priority = m_priority;
    return r;
```

Ajustar `restore()` para preservar `Cancelled` e a prioridade:

```cpp
    m_probed = !segs.isEmpty();
    m_priority = rec.priority;
    m_state = (rec.state == DownloadState::Cancelled) ? DownloadState::Cancelled
                                                      : DownloadState::Paused;
```

- [ ] **Step 5: `cancel()` no manager + resume/requeue aceitam Cancelled** — em `src/core/DownloadManager.h`, declarar (após `resume(...)`):

```cpp
    void  cancel(const QUuid& id);
```

Em `src/core/DownloadManager.cpp`, adicionar:

```cpp
void DownloadManager::cancel(const QUuid& id) {
    DownloadTask* t = taskById(id);
    if (!t) return;
    if (t->state() == DownloadState::Completed || t->state() == DownloadState::Cancelled)
        return;                        // nada a cancelar
    t->cancel();
    saveSession();
    pump();                            // um slot pode ter liberado
}
```

Em `resume()` (linha ~152), ampliar a condição para incluir `Cancelled`:

```cpp
    if (t->state() == DownloadState::Paused || t->state() == DownloadState::Error ||
        t->state() == DownloadState::Cancelled) {
        t->requeue();
        pump();
    }
```

Em `DownloadTask::requeue()` (linha ~235), ampliar o guard:

```cpp
    if (m_state == DownloadState::Paused || m_state == DownloadState::Error ||
        m_state == DownloadState::Cancelled)
        setState(DownloadState::Queued);
```

- [ ] **Step 6: Rodar e ver passar**

Run: `cmake --build build --target tst_download && ctest --test-dir build -R tst_download --output-on-failure`
Expected: PASS (inclusive os dois novos casos).

- [ ] **Step 7: Commit**

```bash
git add src/core/DownloadTask.h src/core/DownloadTask.cpp src/core/DownloadManager.h src/core/DownloadManager.cpp tests/tst_download.cpp
git commit -m "feat(core): add cancel() that discards partial and restarts from Cancelled"
```

---

## Task 4: `setPriority()` + `pump()` por prioridade

**Files:**
- Modify: `src/core/DownloadManager.h`, `src/core/DownloadManager.cpp:59-88` (`pump`)
- Test: `tests/tst_download.cpp`

**Interfaces:**
- Consumes: `DownloadTask::priority()/setPriority()` (Task 3), `enum class Priority` (Task 2).
- Produces: `void DownloadManager::setPriority(const QUuid& id, Priority p);` e um `pump()` que promove a fila em ordem High→Normal→Low (estável).

- [ ] **Step 1: Escrever o teste que falha** — em `tests/tst_download.cpp`:

```cpp
    void pumpPromotesByPriority() {
        const QByteArray big = makeBody(300000);
        TestServer srv(big);
        QVERIFY(srv.listen());
        const QString dir = makeTempDir();
        EngineConfig cfg;
        cfg.maxConcurrentDownloads = 1;         // 1 ativo por vez -> ordem importa
        cfg.maxBytesPerSec = 50000;             // 'a' segura Downloading enquanto ajustamos
        DownloadManager mgr(cfg, dir);
        const QUuid a = mgr.addDownload(srv.url("/ranged"), dir + "/a.bin");
        QVERIFY(waitForState(mgr, a, DownloadState::Downloading, 5000));
        const QUuid b = mgr.addDownload(srv.url("/ranged"), dir + "/b.bin");   // Queued
        const QUuid c = mgr.addDownload(srv.url("/ranged"), dir + "/c.bin");   // Queued
        mgr.setPriority(b, Priority::Low);
        mgr.setPriority(c, Priority::High);
        // Registra a ORDEM em que b e c entram em Downloading.
        QSignalSpy spy(&mgr, &DownloadManager::taskStateChanged);
        // Deixa a fila drenar (a -> c -> b).
        QVERIFY(waitForState(mgr, b, DownloadState::Completed, 20000));
        QVERIFY(waitForState(mgr, c, DownloadState::Completed, 20000));
        // Procura o primeiro Downloading de b e de c no histórico do spy.
        int idxC = -1, idxB = -1;
        for (int i = 0; i < spy.count(); ++i) {
            const QUuid id = spy.at(i).at(0).value<QUuid>();
            const auto st = spy.at(i).at(1).value<DownloadState>();
            if (st != DownloadState::Downloading) continue;
            if (id == c && idxC < 0) idxC = i;
            if (id == b && idxB < 0) idxB = i;
        }
        QVERIFY(idxC >= 0 && idxB >= 0);
        QVERIFY2(idxC < idxB, "High-priority c must be promoted before Low-priority b");
    }
```

> `DownloadState` precisa estar registrado no sistema de metatipos para o `QSignalSpy.value<DownloadState>()`. O sinal `taskStateChanged(const QUuid&, DownloadState)` já é usado com `QSignalSpy` em outros testes deste arquivo, então o registro já existe; se um erro de metatype aparecer, acrescente `qRegisterMetaType<DownloadState>("DownloadState");` no `initTestCase()`.

- [ ] **Step 2: Rodar e ver falhar**

Run: `cmake --build build --target tst_download 2>&1 | tail -5`
Expected: erro de compilação (`setPriority` não é membro de `DownloadManager`).

- [ ] **Step 3: Declarar e implementar** — em `src/core/DownloadManager.h` (após `cancel(...)`):

```cpp
    void  setPriority(const QUuid& id, Priority p);
```

Em `src/core/DownloadManager.cpp`, incluir `<algorithm>` no topo e adicionar:

```cpp
void DownloadManager::setPriority(const QUuid& id, Priority p) {
    DownloadTask* t = taskById(id);
    if (!t) return;
    t->setPriority(p);
    saveSession();
    pump();                 // a reordenação pode mudar quem é promovido a seguir
}
```

Reescrever o laço de promoção em `pump()` (mantendo o guard `m_inPump` e o comentário existente sobre reentrância). Substituir o `for (auto* t : m_tasks) { ... }` (linhas ~80-87) por:

```cpp
    // Promove os Queued em ordem de prioridade (High->Normal->Low), estável
    // dentro de cada nível (std::stable_sort preserva a ordem de inserção).
    QVector<DownloadTask*> queued;
    for (auto* t : m_tasks)
        if (t->state() == DownloadState::Queued) queued.append(t);
    std::stable_sort(queued.begin(), queued.end(),
        [](DownloadTask* x, DownloadTask* y){ return int(x->priority()) < int(y->priority()); });

    for (auto* t : queued) {
        int active = 0;
        for (auto* u : m_tasks)
            if (u->state() == DownloadState::Downloading || u->state() == DownloadState::Connecting)
                ++active;
        if (active >= m_cfg.maxConcurrentDownloads) break;
        t->start();
    }
```

- [ ] **Step 4: Rodar e ver passar**

Run: `cmake --build build --target tst_download && ctest --test-dir build -R tst_download --output-on-failure`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/core/DownloadManager.h src/core/DownloadManager.cpp tests/tst_download.cpp
git commit -m "feat(core): promote queued downloads by priority in pump()"
```

---

## Task 5: `moveFiles()`

**Files:**
- Modify: `src/core/DownloadManager.h`, `src/core/DownloadManager.cpp`
- Test: `tests/tst_download.cpp`

**Interfaces:**
- Consumes: `DownloadTask::setDestPath` (Task 3), `Persistence::metaPath`, `Persistence::resolveUniquePath`.
- Produces: `bool DownloadManager::moveFiles(const QUuid& id, const QString& newDir);` (retorna `false` e é no-op se o item está ativo — Downloading/Connecting).

- [ ] **Step 1: Escrever o teste que falha** — em `tests/tst_download.cpp`:

```cpp
    void moveFilesRelocatesAndUpdatesDest() {
        TestServer srv(m_body);
        QVERIFY(srv.listen());
        const QString dir = makeTempDir();
        const QString dest2 = makeTempDir();          // pasta destino do move
        EngineConfig cfg;
        DownloadManager mgr(cfg, dir);
        const QUuid id = mgr.addDownload(srv.url("/ranged"), dir + "/movie.bin");
        QVERIFY(waitForState(mgr, id, DownloadState::Completed, 5000));
        QVERIFY(mgr.moveFiles(id, dest2));
        QVERIFY(!QFile::exists(dir + "/movie.bin"));            // saiu da origem
        QVERIFY(QFile::exists(dest2 + "/movie.bin"));           // chegou ao destino
        QCOMPARE(mgr.taskById(id)->record().destPath, dest2 + "/movie.bin");
    }

    void moveFilesRefusedWhileActive() {
        const QByteArray big = makeBody(300000);
        TestServer srv(big);
        QVERIFY(srv.listen());
        const QString dir = makeTempDir();
        const QString dest2 = makeTempDir();
        EngineConfig cfg;
        cfg.maxBytesPerSec = 50000;                            // segura em Downloading
        DownloadManager mgr(cfg, dir);
        const QUuid id = mgr.addDownload(srv.url("/ranged"), dir + "/m.bin");
        QVERIFY(waitForState(mgr, id, DownloadState::Downloading, 5000));
        QCOMPARE(mgr.moveFiles(id, dest2), false);            // recusado enquanto ativo
        QVERIFY(QFile::exists(dir + "/m.bin"));               // não moveu
    }
```

- [ ] **Step 2: Rodar e ver falhar**

Run: `cmake --build build --target tst_download 2>&1 | tail -5`
Expected: erro de compilação (`moveFiles` não é membro).

- [ ] **Step 3: Declarar e implementar** — em `src/core/DownloadManager.h` (após `setPriority(...)`):

```cpp
    bool  moveFiles(const QUuid& id, const QString& newDir);
```

Em `src/core/DownloadManager.cpp` (garantir `#include <QFileInfo>`; `<QDir>` e `<QFile>` já estão incluídos):

```cpp
bool DownloadManager::moveFiles(const QUuid& id, const QString& newDir) {
    DownloadTask* t = taskById(id);
    if (!t) return false;
    const DownloadState s = t->state();
    if (s == DownloadState::Downloading || s == DownloadState::Connecting)
        return false;                          // só com download parado
    const QString oldPath  = t->record().destPath;
    const QString fileName = QFileInfo(oldPath).fileName();
    QString newPath = Persistence::resolveUniquePath(QDir(newDir).filePath(fileName));
    if (QFileInfo::exists(oldPath) && !QFile::rename(oldPath, newPath))
        return false;
    const QString oldMeta = Persistence::metaPath(oldPath);
    if (QFileInfo::exists(oldMeta))
        QFile::rename(oldMeta, Persistence::metaPath(newPath));
    t->setDestPath(newPath);
    saveSession();
    return true;
}
```

- [ ] **Step 4: Rodar e ver passar**

Run: `cmake --build build --target tst_download && ctest --test-dir build -R tst_download --output-on-failure`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/core/DownloadManager.h src/core/DownloadManager.cpp tests/tst_download.cpp
git commit -m "feat(core): add moveFiles() to relocate a stopped download"
```

---

## Task 6: Instrumentação de logging no core

**Files:**
- Modify: `src/core/DownloadManager.h`, `src/core/DownloadManager.cpp` (ctor recebe `Logger*`; loga transições)
- Modify: `src/core/DownloadTask.h`, `src/core/DownloadTask.cpp` (`setLogger` + logs de probe/segmento/credenciais)
- Test: `tests/tst_download.cpp`

**Interfaces:**
- Consumes: `Logger` (Task 1).
- Produces:
  - `DownloadManager(const EngineConfig&, const QString& dataDir, Logger* logger = nullptr, QObject* parent = nullptr);`
  - `void DownloadTask::setLogger(Logger* l);`
  - Cada transição de estado gera uma linha no log daquele download; probe/segmentação/falha/credenciais também.

- [ ] **Step 1: Escrever o teste que falha** — em `tests/tst_download.cpp` (inclua `#include "Logger.h"` no topo):

```cpp
    void completedDownloadWritesTaskLog() {
        TestServer srv(m_body);
        QVERIFY(srv.listen());
        const QString dir = makeTempDir();
        EngineConfig cfg;
        Logger logger(dir);
        DownloadManager mgr(cfg, dir, &logger);
        const QUuid id = mgr.addDownload(srv.url("/ranged"), dir + "/movie.bin");
        QVERIFY(waitForState(mgr, id, DownloadState::Completed, 5000));
        const QString logContent = readFile(logger.taskLogPath(id, dir + "/movie.bin"));
        QVERIFY(logContent.contains("Downloading"));    // logou a transição
        QVERIFY(logContent.contains("Completed"));
    }
```

- [ ] **Step 2: Rodar e ver falhar**

Run: `cmake --build build --target tst_download 2>&1 | tail -5`
Expected: erro de compilação (ctor de `DownloadManager` não aceita `Logger*`).

- [ ] **Step 3: Ctor do manager recebe `Logger*`** — em `src/core/DownloadManager.h`: incluir `class Logger;` (forward) no topo (após os includes) e trocar a assinatura do ctor + adicionar membro:

```cpp
    DownloadManager(const EngineConfig& cfg, const QString& dataDir,
                    Logger* logger = nullptr, QObject* parent = nullptr);
```
```cpp
    Logger*                 m_logger = nullptr;   // não-dono; pode ser nullptr
```

Em `src/core/DownloadManager.cpp`: `#include "Logger.h"`; atualizar a definição do ctor:

```cpp
DownloadManager::DownloadManager(const EngineConfig& cfg, const QString& dataDir,
                                 Logger* logger, QObject* parent)
    : QObject(parent), m_cfg(cfg), m_dataDir(dataDir), m_logger(logger) {
```

Em `addDownload`, após `wire(t);` e antes de `m_tasks.append(t)`, propagar o logger:

```cpp
    wire(t);
    t->setLogger(m_logger);
```

E em `loadSession`, após `wire(t);`:

```cpp
        wire(t);
        t->setLogger(m_logger);
```

No handler de `stateChanged` dentro de `wire()` (linha ~33), logar a transição:

```cpp
    connect(t, &DownloadTask::stateChanged, this, [this, t](DownloadState s) {
        if (m_logger)
            m_logger->logTask(t->id(), t->record().destPath,
                              s == DownloadState::Error ? LogLevel::Error : LogLevel::Info,
                              QStringLiteral("state -> %1").arg(stateName(s)));
        emit taskStateChanged(t->id(), s);
        saveSession();
        if (s == DownloadState::Completed || s == DownloadState::Error ||
            s == DownloadState::Paused)
            pump();
    });
```

- [ ] **Step 4: `setLogger` + logs de detalhe na task** — em `src/core/DownloadTask.h`: `class Logger;` (forward, após os `class QFile;`/`class RateLimiter;`), declarar `void setLogger(Logger* l) { m_logger = l; }` na seção public e o membro `Logger* m_logger = nullptr;` na private. Adicionar um helper privado:

```cpp
    void logLine(LogLevel level, const QString& msg);
```

Em `src/core/DownloadTask.cpp`: `#include "Logger.h"` e implementar:

```cpp
void DownloadTask::logLine(LogLevel level, const QString& msg) {
    if (m_logger) m_logger->logTask(m_id, m_destPath, level, msg);
}
```

Instrumentar (linhas exatas de referência entre parênteses):
- Em `onProbed` (ok, após `m_segments = computeSegments(...)`):
  ```cpp
  logLine(LogLevel::Info, QStringLiteral("probe ok: total=%1 range=%2 segments=%3")
              .arg(m_totalBytes).arg(m_supportsRange ? "yes" : "no").arg(m_segments.size()));
  ```
  e no ramo de erro (`if (!r.ok)`, após `m_error = r.error;`):
  ```cpp
  logLine(LogLevel::Error, QStringLiteral("probe failed: %1").arg(r.error));
  ```
- Em `beginSegments`, logo após `setState(DownloadState::Downloading);`:
  ```cpp
  for (const auto& seg : m_segments)
      logLine(LogLevel::Debug, QStringLiteral("segment %1 [%2..%3]")
                  .arg(seg.index).arg(seg.start).arg(seg.end));
  ```
- Em `onSegmentCompleted` (após `++m_completedCount;`):
  ```cpp
  logLine(LogLevel::Debug, QStringLiteral("segment %1 complete").arg(index));
  ```
- Em `onSegmentFailed` (após `m_error = error;`):
  ```cpp
  logLine(LogLevel::Warn, QStringLiteral("segment %1 failed: %2").arg(index).arg(error));
  ```
- Em `onRestartRequired` (após `if (m_restarting) return;` e `m_restarting = true;`):
  ```cpp
  logLine(LogLevel::Warn, QStringLiteral("resource changed, restarting from zero"));
  ```
- Em `askForCredentials` (após `m_awaitingCredentials = true;`):
  ```cpp
  logLine(LogLevel::Info, QStringLiteral("credentials required for host %1").arg(m_url.host()));
  ```

- [ ] **Step 5: Rodar e ver passar**

Run: `cmake --build build --target tst_download && ctest --test-dir build -R tst_download --output-on-failure`
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add src/core/DownloadManager.h src/core/DownloadManager.cpp src/core/DownloadTask.h src/core/DownloadTask.cpp tests/tst_download.cpp
git commit -m "feat(core): instrument downloads with segment-level logging"
```

---

## Task 7: `CellKind::Active` em `GridGeometry`

**Files:**
- Modify: `src/gui/GridGeometry.h`, `src/gui/GridGeometry.cpp`
- Test: `tests/tst_gui.cpp`

**Interfaces:**
- Produces: `CellKind::Active` — a célula que contém a posição `current` de um segmento ainda não completo, quando o download está ativo (Connecting/Downloading).

- [ ] **Step 1: Escrever o teste que falha** — em `tests/tst_gui.cpp`, novo slot (inclua `#include "GridGeometry.h"` se faltar):

```cpp
    void computeCellsMarksActiveHead() {
        // 1 segmento de 0..99, current no meio, download ativo -> a célula do
        // 'current' é Active; as anteriores Downloaded; as seguintes Pending.
        QVector<Segment> segs;
        Segment s; s.index = 0; s.start = 0; s.end = 99; s.current = 50;
        segs.append(s);
        const auto cells = computeCells(100, segs, DownloadState::Downloading, 10);
        QCOMPARE(cells.size(), 10);
        QCOMPARE(cells[0].kind, CellKind::Downloaded);     // [0..9] < current
        QCOMPARE(cells[5].kind, CellKind::Active);         // contém current=50
        QCOMPARE(cells[9].kind, CellKind::Pending);        // [90..99] > current
    }

    void computeCellsNoActiveWhenPaused() {
        QVector<Segment> segs;
        Segment s; s.index = 0; s.start = 0; s.end = 99; s.current = 50;
        segs.append(s);
        const auto cells = computeCells(100, segs, DownloadState::Paused, 10);
        for (const auto& c : cells) QVERIFY(c.kind != CellKind::Active);
    }
```

- [ ] **Step 2: Rodar e ver falhar**

Run: `cmake --build build --target tst_gui 2>&1 | tail -5`
Expected: erro de compilação (`CellKind::Active` não existe).

- [ ] **Step 3: Adicionar `Active` ao enum** — `src/gui/GridGeometry.h`:

```cpp
enum class CellKind { Pending, Downloaded, Error, Active };
```

- [ ] **Step 4: Detectar a célula-cabeça** — em `src/gui/GridGeometry.cpp`, no laço de `computeCells`, o bloco atual obtém `owner` e computa `downloaded`. Calcule também o `current` do segmento dono uma única vez e substitua o bloco de decisão (a partir de `if (downloaded) {`, linhas ~27-33) por:

```cpp
        qint64 cur = -1;
        for (const Segment& s : segments)
            if (s.index == owner) { cur = s.current; break; }
        const bool active = (state == DownloadState::Downloading ||
                             state == DownloadState::Connecting);
        if (downloaded) {
            cells[i].kind = CellKind::Downloaded;
            cells[i].segmentIndex = owner;
        } else if (active && owner >= 0 && cellStart <= cur && cur < cellEnd) {
            cells[i].kind = CellKind::Active;         // célula que contém o current
            cells[i].segmentIndex = owner;
        } else if (state == DownloadState::Error) {
            cells[i].kind = CellKind::Error;
        } else {
            cells[i].kind = CellKind::Pending;
        }
```

- [ ] **Step 5: Rodar e ver passar**

Run: `cmake --build build --target tst_gui && ctest --test-dir build -R tst_gui --output-on-failure`
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add src/gui/GridGeometry.h src/gui/GridGeometry.cpp tests/tst_gui.cpp
git commit -m "feat(gui): mark the active write-head cell in the progress grid"
```

---

## Task 8: Cores do Orbit em `ProgressGridWidget`

**Files:**
- Modify: `src/gui/ProgressGridWidget.cpp:8-14` (remover `segColor`) e `:80-99` (paintEvent)

**Interfaces:**
- Consumes: `CellKind::Active` (Task 7).
- Produces: grade com fundo claro e cores por estado (sem paleta por-segmento). Mudança visual — verificação por build + execução do app.

- [ ] **Step 1: Remover a paleta por-segmento** — em `src/gui/ProgressGridWidget.cpp`, apagar a função `segColor` inteira (linhas 8-14).

- [ ] **Step 2: Fundo claro** — trocar a linha `p.fillRect(rect(), QColor("#1e1e1e"));` (linha ~82) por:

```cpp
    p.fillRect(rect(), QColor("#ffffff"));
```

- [ ] **Step 3: Cores por estado** — no `paintEvent`, substituir o `switch (cells[i].kind)` (linhas ~92-96) por:

```cpp
        QColor c;
        switch (cells[i].kind) {
            case CellKind::Downloaded: c = QColor("#5b9bd5"); break;   // azul Orbit
            case CellKind::Active:     c = QColor("#f7941e"); break;   // laranja (cabeça)
            case CellKind::Error:      c = QColor("#ef4444"); break;
            case CellKind::Pending:    c = QColor("#dcdcdc"); break;   // cinza claro
        }
        p.fillRect(cx, cy, kCellPx - 1, kCellPx - 1, c);
```

- [ ] **Step 4: Build limpo (sem warnings; o switch cobre os 4 CellKind)**

Run: `cmake --build build 2>&1 | grep -i warning; echo done`
Expected: apenas `done` (nenhum warning).

- [ ] **Step 5: Verificação visual** — rodar o app, adicionar um download grande e abrir a aba Progress:

Run: `./build/src/gui/orbit-gui` (encerre com Ctrl-C após conferir)
Expected: grade em fundo claro; tiles baixados em azul, cabeça de cada segmento em laranja, pendentes em cinza. Sem o arco-íris antigo.

- [ ] **Step 6: Commit**

```bash
git add src/gui/ProgressGridWidget.cpp
git commit -m "feat(gui): recolor progress grid to the classic Orbit scheme"
```

---

## Task 9: Wiring do `Logger` na GUI + aba Log por-download + Application Log

**Files:**
- Modify: `src/gui/main_gui.cpp` (cria o `Logger`, injeta no manager e na janela)
- Modify: `src/gui/MainWindow.h`, `src/gui/MainWindow.cpp` (guarda `Logger*`, aba Log por seleção, Tools→Application Log)

**Interfaces:**
- Consumes: `Logger` (Task 1), ctor de `DownloadManager` com `Logger*` (Task 6).
- Produces: `MainWindow(DownloadManager*, DownloadTableModel*, Logger*, QWidget*)`. A aba "Log" mostra o log do item selecionado; Tools→Application Log abre `logs/`.

- [ ] **Step 1: Criar o Logger no `main_gui.cpp`** — em `src/gui/main_gui.cpp`, após `const AppSettings settings = ...;`:

```cpp
    Logger logger(dataDir);
    DownloadManager mgr(settings.engine, dataDir, &logger);
    mgr.loadSession();
    DownloadTableModel model(&mgr);

    MainWindow w(&mgr, &model, &logger);
```

Trocar a construção antiga de `mgr`/`w` por essa. Incluir `#include "Logger.h"` no topo.

- [ ] **Step 2: `MainWindow` guarda o `Logger*`** — em `src/gui/MainWindow.h`: `class Logger;` (forward), trocar a assinatura do ctor:

```cpp
    MainWindow(DownloadManager* mgr, DownloadTableModel* model,
               Logger* logger, QWidget* parent = nullptr);
```
adicionar membros:
```cpp
    Logger*  m_logger = nullptr;
    QUuid    m_logShownId;     // download cujo log está na aba
```
e um slot:
```cpp
    void onLogLine(const QUuid& id, const QString& line);
```

- [ ] **Step 3: Ctor + wiring** — em `src/gui/MainWindow.cpp`, atualizar a assinatura e a lista de init:

```cpp
MainWindow::MainWindow(DownloadManager* mgr, DownloadTableModel* model,
                       Logger* logger, QWidget* parent)
    : QMainWindow(parent), m_mgr(mgr), m_model(model), m_logger(logger) {
```

Incluir `#include "Logger.h"`. Após criar `m_log` (linha ~86), conectar o sinal do logger:

```cpp
    if (m_logger)
        connect(m_logger, &Logger::lineLogged, this, &MainWindow::onLogLine);
```

- [ ] **Step 4: Aba Log segue a seleção** — em `onSelectionChanged` (linha ~270), ao final da função, carregar o arquivo do item selecionado:

```cpp
    // Log por-download: recarrega o arquivo do item selecionado e passa a
    // anexar as novas linhas dele (ver onLogLine).
    m_logShownId = id;
    m_log->clear();
    if (t && m_logger) {
        QFile f(m_logger->taskLogPath(id, t->record().destPath));
        if (f.open(QIODevice::ReadOnly))
            m_log->setPlainText(QString::fromUtf8(f.readAll()));
    }
```

Garanta `#include <QFile>` no `.cpp` (adicione se faltar).

E implementar o slot (append ao vivo só do item mostrado):

```cpp
void MainWindow::onLogLine(const QUuid& id, const QString& line) {
    if (id == m_logShownId && !id.isNull())
        m_log->appendPlainText(line);
}
```

- [ ] **Step 5: Substituir o log global antigo** — a antiga `onStateChanged` (linhas ~281-291) montava uma `line` e a escrevia no `m_log` global. O log agora vem do `Logger` (per-download) e o evento de app vai para `app.log`. Substituir a função inteira por (note: sem a variável `line`, que ficaria sem uso e viraria warning):

```cpp
void MainWindow::onStateChanged(const QUuid& id, int state) {
    DownloadTask* t = m_mgr->taskById(id);
    const QString name = t ? QFileInfo(t->record().destPath).fileName() : id.toString();
    static const char* names[] = {"Queued","Connecting","Downloading","Paused",
                                  "Completed","Error","Cancelled"};
    if (m_logger)
        m_logger->logApp(state == int(DownloadState::Error) ? LogLevel::Error : LogLevel::Info,
                         QString("%1 -> %2").arg(name, names[state]));
    maybeQuitWhenDone();
}
```

(A notificação de conclusão é acrescentada a esta função na Task 12.)

- [ ] **Step 6: Tools → Application Log** — localizar a construção do menu Tools (em `MainWindow.cpp`, onde `tools->addAction(aSched)`/`aPrefs` são adicionados, linha ~198). Antes desses, inserir:

```cpp
    QAction* aAppLog = tools->addAction(tr("Application Log"));
    connect(aAppLog, &QAction::triggered, this, [this]{
        if (m_logger)
            QDesktopServices::openUrl(QUrl::fromLocalFile(m_logger->logsDir()));
    });
    tools->addSeparator();
```

(`QDesktopServices` e `QUrl` já são usados no arquivo.)

- [ ] **Step 7: Atualizar chamadas ao ctor nos testes** — em `tests/tst_gui.cpp`, toda construção `MainWindow w(&mgr, &model)` precisa do novo parâmetro. Adicionar um `Logger` no escopo do teste e passar `&logger` (ex.: `Logger logger(dir); MainWindow w(&mgr, &model, &logger);`). Ajuste cada ocorrência. Inclua `#include "Logger.h"`.

- [ ] **Step 8: Build + testes**

Run: `cmake --build build && ctest --test-dir build --output-on-failure`
Expected: tudo PASS; sem warnings.

- [ ] **Step 9: Commit**

```bash
git add src/gui/main_gui.cpp src/gui/MainWindow.h src/gui/MainWindow.cpp tests/tst_gui.cpp
git commit -m "feat(gui): per-download log tab and Tools > Application Log"
```

---

## Task 10: Coluna `Priority` na tabela

**Files:**
- Modify: `src/gui/DownloadTableModel.h:16` (enum `Column`), `src/gui/DownloadTableModel.cpp` (`data`, `headerData`)
- Test: `tests/tst_gui.cpp`

**Interfaces:**
- Consumes: `DownloadTask::priority()` (Task 3), `priorityToString` (Task 2).
- Produces: nova coluna `Priority` exibindo `High/Normal/Low`; método público `void DownloadTableModel::refreshRow(const QUuid& id);` para a view repintar a linha após uma mudança de prioridade (usado na Task 11).

- [ ] **Step 1: Escrever o teste que falha** — em `tests/tst_gui.cpp` (segue o padrão de construir `DownloadManager`+`DownloadTableModel`; use `mgr.setPriority` da Task 4):

```cpp
    void tableExposesPriorityColumn() {
        const QString dir = QDir::tempPath() + "/orbit_prio_" +
                            QUuid::createUuid().toString(QUuid::WithoutBraces);
        QDir().mkpath(dir);
        EngineConfig cfg;
        Logger logger(dir);
        DownloadManager mgr(cfg, dir, &logger);
        const QUuid id = mgr.addDownload(QUrl("http://x/f.bin"), dir + "/f.bin");
        DownloadTableModel model(&mgr);
        QCOMPARE(model.columnCount(), int(DownloadTableModel::ColumnCount));
        const int col = DownloadTableModel::Priority;
        const int row = 0;
        QCOMPARE(model.data(model.index(row, col)).toString(), QString("Normal"));
        mgr.setPriority(id, Priority::High);
        QCOMPARE(model.data(model.index(row, col)).toString(), QString("High"));
    }
```

- [ ] **Step 2: Rodar e ver falhar**

Run: `cmake --build build --target tst_gui 2>&1 | tail -5`
Expected: erro de compilação (`DownloadTableModel::Priority` não existe).

- [ ] **Step 3: Adicionar a coluna** — em `src/gui/DownloadTableModel.h`:

```cpp
    enum Column { Name, Size, Progress, Status, Speed, TimeLeft, Priority, ColumnCount };
```

- [ ] **Step 4: `data` e `headerData`** — em `src/gui/DownloadTableModel.cpp`, no `switch (ix.column())` de `data`, adicionar antes do fecho:

```cpp
        case Priority: return priorityToString(r.task->priority());
```

E em `headerData`, estender o array:

```cpp
    static const char* h[] = {"Name","Size","Progress","Status","Speed","Time Left","Priority"};
```

- [ ] **Step 5: `refreshRow` para repintar após mudança de prioridade** — em `src/gui/DownloadTableModel.h`, declarar na seção public:

```cpp
    void refreshRow(const QUuid& id);
```

Em `src/gui/DownloadTableModel.cpp`, implementar:

```cpp
void DownloadTableModel::refreshRow(const QUuid& id) {
    const int row = rowForId(id);
    if (row < 0) return;
    emit dataChanged(index(row, 0), index(row, ColumnCount - 1));
}
```

- [ ] **Step 6: Rodar e ver passar**

Run: `cmake --build build --target tst_gui && ctest --test-dir build -R tst_gui --output-on-failure`
Expected: PASS.

- [ ] **Step 7: Commit**

```bash
git add src/gui/DownloadTableModel.h src/gui/DownloadTableModel.cpp tests/tst_gui.cpp
git commit -m "feat(gui): show download priority as a table column"
```

---

## Task 11: Menu de contexto na tabela

**Files:**
- Modify: `src/gui/MainWindow.h`, `src/gui/MainWindow.cpp`
- Test: `tests/tst_gui.cpp`

**Interfaces:**
- Consumes: `m_mgr->cancel/setPriority/moveFiles/remove/pause/resume` (Tasks 3–6), `selectedId()` (existente).
- Produces:
  - slot `void onTableContextMenu(const QPoint& pos);` que monta o `QMenu` inline e habilita/desabilita as ações por estado.
  - handlers `void onCancel(); void onMove(); void onOpen(); void onOpenFolder();`.
  - header header-only `src/gui/ContextMenuRules.h` com as regras puras testáveis `ctxCanStart/ctxCanStop/ctxCanCancel/ctxCanMove/ctxCanOpen`.

- [ ] **Step 1: Escrever o teste que falha (regra de habilitação, sem exec de menu)** — a lógica de enable/disable é pura; extraia-a para uma função livre testável em `MainWindow.cpp` e teste-a. Em `tests/tst_gui.cpp`:

```cpp
    void contextMenuEnableRules() {
        using S = DownloadState;
        // Start: habilitado em Queued/Paused/Cancelled/Error; não em Downloading/Completed.
        QVERIFY(ctxCanStart(S::Paused));
        QVERIFY(ctxCanStart(S::Cancelled));
        QVERIFY(!ctxCanStart(S::Downloading));
        QVERIFY(!ctxCanStart(S::Completed));
        // Stop: só Connecting/Downloading.
        QVERIFY(ctxCanStop(S::Downloading));
        QVERIFY(!ctxCanStop(S::Paused));
        // Cancel: não em Completed/Cancelled.
        QVERIFY(ctxCanCancel(S::Downloading));
        QVERIFY(!ctxCanCancel(S::Completed));
        // Move: só não-ativo.
        QVERIFY(ctxCanMove(S::Paused));
        QVERIFY(!ctxCanMove(S::Downloading));
        // Open: só Completed.
        QVERIFY(ctxCanOpen(S::Completed));
        QVERIFY(!ctxCanOpen(S::Paused));
    }
```

Declare os protótipos no topo de `tst_gui.cpp` (definidos como funções livres no header abaixo):

```cpp
#include "ContextMenuRules.h"
```

- [ ] **Step 2: Rodar e ver falhar**

Run: `cmake --build build --target tst_gui 2>&1 | tail -5`
Expected: erro (`ContextMenuRules.h` não existe).

- [ ] **Step 3: Regras puras (header testável)** — Create `src/gui/ContextMenuRules.h`:

```cpp
#pragma once
#include "DownloadTypes.h"

// Regras de habilitação do menu de contexto (spec §3.4). Puras -> testáveis.
inline bool ctxCanStart(DownloadState s) {
    return s == DownloadState::Queued || s == DownloadState::Paused ||
           s == DownloadState::Cancelled || s == DownloadState::Error;
}
inline bool ctxCanStop(DownloadState s) {
    return s == DownloadState::Connecting || s == DownloadState::Downloading;
}
inline bool ctxCanCancel(DownloadState s) {
    return s != DownloadState::Completed && s != DownloadState::Cancelled;
}
inline bool ctxCanMove(DownloadState s) {
    return s != DownloadState::Downloading && s != DownloadState::Connecting;
}
inline bool ctxCanOpen(DownloadState s) {
    return s == DownloadState::Completed;
}
```

(`orbitgui_logic` já expõe `${CMAKE_CURRENT_SOURCE_DIR}` no include path, então `tst_gui` — que linka `orbitgui` → `orbitgui_logic` — enxerga o header. Header-only: nada a adicionar no CMake.)

- [ ] **Step 4: Slot do menu + ações** — em `src/gui/MainWindow.h`, declarar o slot `void onTableContextMenu(const QPoint& pos);` e os handlers `void onCancel(); void onMove(); void onOpen(); void onOpenFolder();`.

Em `src/gui/MainWindow.cpp`, no ctor (após configurar `m_table`, linha ~82) habilitar o menu:

```cpp
    m_table->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_table, &QTableView::customContextMenuRequested,
            this, &MainWindow::onTableContextMenu);
```

Incluir `#include <QMenu>` e `#include "ContextMenuRules.h"`. Implementar:

```cpp
void MainWindow::onTableContextMenu(const QPoint& pos) {
    const QModelIndex ix = m_table->indexAt(pos);
    if (!ix.isValid()) return;
    m_table->setCurrentIndex(ix);                 // seleção reflete a linha clicada;
                                                  // selectedId() (usado pelos handlers) segue-a
    const QUuid id = selectedId();
    DownloadTask* t = id.isNull() ? nullptr : m_mgr->taskById(id);
    if (!t) return;
    const DownloadState s = t->state();

    QMenu menu(this);
    QAction* aStart  = menu.addAction(tr("Start"));
    QAction* aStop   = menu.addAction(tr("Stop"));
    QAction* aCancel = menu.addAction(tr("Cancel"));
    menu.addSeparator();
    QAction* aDelete = menu.addAction(tr("Delete..."));
    QAction* aMove   = menu.addAction(tr("Move..."));
    menu.addSeparator();
    QAction* aOpen   = menu.addAction(tr("Open"));
    QAction* aFolder = menu.addAction(tr("Open containing folder"));
    menu.addSeparator();
    QMenu* prio = menu.addMenu(tr("Priority"));
    QAction* pHigh = prio->addAction(tr("High"));
    QAction* pNorm = prio->addAction(tr("Normal"));
    QAction* pLow  = prio->addAction(tr("Low"));
    for (auto* a : {pHigh, pNorm, pLow}) a->setCheckable(true);
    pHigh->setChecked(t->priority() == Priority::High);
    pNorm->setChecked(t->priority() == Priority::Normal);
    pLow ->setChecked(t->priority() == Priority::Low);

    aStart->setEnabled(ctxCanStart(s));
    aStop->setEnabled(ctxCanStop(s));
    aCancel->setEnabled(ctxCanCancel(s));
    aMove->setEnabled(ctxCanMove(s));
    aOpen->setEnabled(ctxCanOpen(s));

    connect(aStart,  &QAction::triggered, this, &MainWindow::onStart);
    connect(aStop,   &QAction::triggered, this, &MainWindow::onPause);
    connect(aCancel, &QAction::triggered, this, &MainWindow::onCancel);
    connect(aDelete, &QAction::triggered, this, &MainWindow::onDelete);
    connect(aMove,   &QAction::triggered, this, &MainWindow::onMove);
    connect(aOpen,   &QAction::triggered, this, &MainWindow::onOpen);
    connect(aFolder, &QAction::triggered, this, &MainWindow::onOpenFolder);
    connect(pHigh, &QAction::triggered, this, [this, id]{ m_mgr->setPriority(id, Priority::High);   m_model->refreshRow(id); });
    connect(pNorm, &QAction::triggered, this, [this, id]{ m_mgr->setPriority(id, Priority::Normal); m_model->refreshRow(id); });
    connect(pLow,  &QAction::triggered, this, [this, id]{ m_mgr->setPriority(id, Priority::Low);    m_model->refreshRow(id); });

    menu.exec(m_table->viewport()->mapToGlobal(pos));
}

void MainWindow::onCancel() {
    const QUuid id = selectedId();
    if (id.isNull()) return;
    const auto btn = QMessageBox::question(this, tr("Cancel"),
        tr("Cancel this download? The partial file will be discarded."));
    if (btn == QMessageBox::Yes) m_mgr->cancel(id);
}

void MainWindow::onMove() {
    const QUuid id = selectedId();
    if (id.isNull()) return;
    const QString dir = QFileDialog::getExistingDirectory(this, tr("Move to folder"));
    if (dir.isEmpty()) return;
    if (!m_mgr->moveFiles(id, dir))
        QMessageBox::warning(this, tr("Move"), tr("Could not move the files."));
}

void MainWindow::onOpen() {
    const QUuid id = selectedId();
    DownloadTask* t = id.isNull() ? nullptr : m_mgr->taskById(id);
    if (t) QDesktopServices::openUrl(QUrl::fromLocalFile(t->record().destPath));
}

void MainWindow::onOpenFolder() {
    const QUuid id = selectedId();
    DownloadTask* t = id.isNull() ? nullptr : m_mgr->taskById(id);
    if (!t) return;
    const QString path = t->record().destPath;
#ifdef Q_OS_MACOS
    QProcess::startDetached("open", {"-R", path});     // revela e seleciona no Finder
#else
    QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
#endif
}
```

Incluir `#include <QFileDialog>` e `#include <QProcess>`.

- [ ] **Step 5: Delete com checkbox de apagar arquivos** — substituir `onDelete()` (linha ~260) por uma versão com checkbox:

```cpp
void MainWindow::onDelete() {
    const QUuid id = selectedId();
    if (id.isNull()) return;
    QMessageBox box(this);
    box.setWindowTitle(tr("Delete"));
    box.setText(tr("Remove this download?"));
    box.setStandardButtons(QMessageBox::Yes | QMessageBox::No);
    QCheckBox* chk = new QCheckBox(tr("Also delete the files from disk"), &box);
    box.setCheckBox(chk);
    if (box.exec() != QMessageBox::Yes) return;
    m_mgr->remove(id, /*deleteFiles=*/chk->isChecked());
    m_model->removeTaskById(id);
}
```

Incluir `#include <QCheckBox>`.

- [ ] **Step 6: Rodar e ver passar**

Run: `cmake --build build --target tst_gui && ctest --test-dir build -R tst_gui --output-on-failure`
Expected: PASS (as regras puras). Build da GUI sem warnings.

- [ ] **Step 7: Verificação manual** — rodar `./build/src/gui/orbit-gui`, clicar com o botão direito num item e conferir ações/estados; testar Delete com e sem o checkbox, Move, Open, Open folder, Priority.

- [ ] **Step 8: Commit**

```bash
git add src/gui/ContextMenuRules.h src/gui/MainWindow.h src/gui/MainWindow.cpp tests/tst_gui.cpp
git commit -m "feat(gui): right-click context menu with start/stop/cancel/delete/move/open/priority"
```

---

## Task 12: Double-click + Clear completed + notificação de conclusão

**Files:**
- Modify: `src/gui/MainWindow.h`, `src/gui/MainWindow.cpp`
- Test: `tests/tst_gui.cpp`

**Interfaces:**
- Consumes: `onOpen`/`onStart` (Task 11), `m_mgr->remove` (existente), `ctxCanStart` (Task 11).
- Produces:
  - slot `void onItemDoubleClicked(const QModelIndex&);`
  - slot/helper `void clearCompleted();` + ação de menu.
  - notificação via `QSystemTrayIcon` ao concluir.
  - helper puro `static DownloadState/decisão` — testar a decisão do double-click sem UI (ver Step 1).

- [ ] **Step 1: Escrever o teste que falha** — a decisão do double-click é pura: extraia `ctxCanStart`/`ctxCanOpen` já existentes (Task 11). Teste o `clearCompleted` de fato (efeito no manager/modelo). Em `tests/tst_gui.cpp`:

```cpp
    void clearCompletedRemovesOnlyCompleted() {
        const QString dir = QDir::tempPath() + "/orbit_clear_" +
                            QUuid::createUuid().toString(QUuid::WithoutBraces);
        QDir().mkpath(dir);
        EngineConfig cfg;
        Logger logger(dir);
        DownloadManager mgr(cfg, dir, &logger);
        // Um "completed" simulado não é trivial sem rede; use o helper de
        // remoção diretamente: adiciona 2, marca 1 como Completed via download real.
        TestServer srv(makeBodyGui(2000));
        QVERIFY(srv.listen());
        const QUuid done = mgr.addDownload(srv.url("/ranged"), dir + "/done.bin");
        QVERIFY(QTest::qWaitFor([&]{ auto* t = mgr.taskById(done);
                    return t && t->state() == DownloadState::Completed; }, 5000));
        const QUuid queued = mgr.addDownload(QUrl("http://x/never"), dir + "/keep.bin");
        Q_UNUSED(queued);
        DownloadTableModel model(&mgr);
        MainWindow w(&mgr, &model, &logger);
        w.clearCompletedForTest();
        // 'done' saiu; 'keep' permanece.
        QVERIFY(mgr.taskById(done) == nullptr);
        QVERIFY(mgr.taskById(queued) != nullptr);
        QVERIFY(QFile::exists(dir + "/done.bin"));   // arquivos NÃO apagados
    }
```

> Adicione um helper `makeBodyGui(int)` no topo de `tst_gui.cpp` (igual ao `makeBody` de `tst_download.cpp`) se ainda não existir, e o hook de teste `clearCompletedForTest()` (Step 4).

- [ ] **Step 2: Rodar e ver falhar**

Run: `cmake --build build --target tst_gui 2>&1 | tail -5`
Expected: erro (`clearCompletedForTest` não existe).

- [ ] **Step 3: Double-click** — em `src/gui/MainWindow.h` declarar `void onItemDoubleClicked(const QModelIndex& ix);`. No ctor (`MainWindow.cpp`, após conectar `currentRowChanged`, linha ~82):

```cpp
    connect(m_table, &QTableView::doubleClicked,
            this, &MainWindow::onItemDoubleClicked);
```

Implementar:

```cpp
void MainWindow::onItemDoubleClicked(const QModelIndex&) {
    const QUuid id = selectedId();
    DownloadTask* t = id.isNull() ? nullptr : m_mgr->taskById(id);
    if (!t) return;
    if (t->state() == DownloadState::Completed) onOpen();
    else if (ctxCanStart(t->state()))           onStart();
    // ativo: no-op
}
```

- [ ] **Step 4: Clear completed** — em `src/gui/MainWindow.h` declarar `void clearCompleted();` (private) e o hook `void clearCompletedForTest() { clearCompleted(); }` (public, junto aos demais hooks de teste). Implementar:

```cpp
void MainWindow::clearCompleted() {
    QVector<QUuid> done;
    for (DownloadTask* t : m_mgr->tasks())
        if (t->state() == DownloadState::Completed) done.append(t->id());
    for (const QUuid& id : done) {
        m_mgr->remove(id, /*deleteFiles=*/false);
        m_model->removeTaskById(id);
    }
    if (m_logger)
        m_logger->logApp(LogLevel::Info, QString("Cleared %1 completed download(s)").arg(done.size()));
}
```

Adicionar a ação no menu Edit (onde `edit->addAction(aDelete)` etc., linha ~158):

```cpp
    QAction* aClearDone = edit->addAction(tr("Clear Completed"));
    connect(aClearDone, &QAction::triggered, this, &MainWindow::clearCompleted);
```

- [ ] **Step 5: Notificação de conclusão** — em `src/gui/MainWindow.h` adicionar membros:

```cpp
    QSystemTrayIcon* m_tray = nullptr;
    QUuid            m_lastCompletedId;
    QString          m_lastCompletedPath;
```

No ctor (perto do final, antes de `m_clip`), criar a bandeja e conectar o clique:

```cpp
    m_tray = new QSystemTrayIcon(this);
    m_tray->setIcon(qApp->windowIcon());
    m_tray->show();
    connect(m_tray, &QSystemTrayIcon::messageClicked, this, [this]{
        if (!m_lastCompletedPath.isEmpty())
            QDesktopServices::openUrl(QUrl::fromLocalFile(m_lastCompletedPath));
    });
```

Em `onStateChanged` (após montar `name`), ao concluir:

```cpp
    if (state == int(DownloadState::Completed) && t && m_tray) {
        m_lastCompletedId = id;
        m_lastCompletedPath = t->record().destPath;
        m_tray->showMessage(tr("Download complete"), name,
                            QSystemTrayIcon::Information, 5000);
    }
```

Incluir `#include <QSystemTrayIcon>`.

- [ ] **Step 6: Rodar e ver passar**

Run: `cmake --build build --target tst_gui && ctest --test-dir build --output-on-failure`
Expected: PASS (toda a suíte); sem warnings.

- [ ] **Step 7: Verificação manual** — `./build/src/gui/orbit-gui`: double-click num concluído abre o arquivo; num parado inicia; concluir um download mostra a notificação; Edit→Clear Completed limpa os concluídos.

- [ ] **Step 8: Commit**

```bash
git add src/gui/MainWindow.h src/gui/MainWindow.cpp tests/tst_gui.cpp
git commit -m "feat(gui): double-click open/start, Clear Completed, and completion notifications"
```

---

## Fechamento

- [ ] **Suíte completa + zero warnings**

Run: `cmake --build build 2>&1 | grep -i warning; ctest --test-dir build --output-on-failure`
Expected: nenhuma linha de warning; todos os testes PASS.

- [ ] **Verificação end-to-end no app** (skill `verify` / `run`): baixar um arquivo grande, exercitar menu de contexto (start/stop/cancel/delete±arquivos/move/open/open-folder/priority), conferir a grade recolorida, a coluna Priority, a aba Log por-download, Tools→Application Log, double-click, Clear Completed e a notificação de conclusão.
