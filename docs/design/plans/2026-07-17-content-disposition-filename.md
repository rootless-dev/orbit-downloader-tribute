# Content-Disposition Filename Implementation Plan

**Goal:** Detectar o nome real de um download HTTP(S) a partir do header `Content-Disposition` e preenchê-lo no diálogo New, para que o arquivo seja salvo com o nome/extensão corretos.

**Architecture:** Um parser puro no Core (`parseContentDisposition`) alimenta um novo campo `ProbeResult::suggestedFileName`, lido pelo `HttpProbe` existente. O `NewDownloadDialog` ganha um campo de nome editável, dispara um probe assíncrono (debounce) ao inserir a URL e preenche o campo respeitando edição manual. `destPath()` passa a usar o nome do campo. Só HTTP(S); FTP inalterado.

**Tech Stack:** C++/Qt6 (Core, Network, Widgets, HttpServer, Test), CMake, QtTest.

**Spec:** `docs/design/specs/2026-07-17-content-disposition-filename-design.md`

## Global Constraints

- Qt6 componentes já usados: `Core Network Widgets HttpServer Test` (raiz `CMakeLists.txt:13`). Nenhuma dependência nova.
- Build: `cmake -S . -B build -DCMAKE_PREFIX_PATH=/opt/homebrew` && `cmake --build build`.
- Testes: `ctest --test-dir build --output-on-failure` (ou `-R <nome>` para um só).
- Testes de GUI rodam com `QT_QPA_PLATFORM=offscreen` (já configurado em `tests/CMakeLists.txt:24`).
- Branch de trabalho: `develop`.
- Commits: Conventional Commits, mensagem em inglês, **sem co-autoria**.
- `filename*=` (RFC 5987) tem prioridade sobre `filename=`. Sanitização basename-only (bloqueia path traversal). Sem tocar no caminho de download.

---

### Task 1: Parser puro `parseContentDisposition()` (Core)

**Files:**
- Create: `src/core/ContentDisposition.h`
- Create: `src/core/ContentDisposition.cpp`
- Modify: `src/core/CMakeLists.txt` (adicionar `ContentDisposition.cpp` à lib `orbitcore`)
- Create: `tests/tst_contentdisposition.cpp`
- Modify: `tests/CMakeLists.txt` (novo executável de teste)

**Interfaces:**
- Consumes: nada.
- Produces: `QString parseContentDisposition(const QString& headerValue);` — recebe o **valor** do header (sem o nome `Content-Disposition:`); devolve um nome de arquivo limpo e seguro, ou string vazia quando não há filename utilizável.

- [ ] **Step 1: Escrever o teste que falha**

Create `tests/tst_contentdisposition.cpp`:

```cpp
#include <QtTest>
#include "ContentDisposition.h"

class TstContentDisposition : public QObject {
    Q_OBJECT
private slots:
    void plain_filename() {
        QCOMPARE(parseContentDisposition("attachment; filename=\"Audiobook.m4a\""),
                 QString("Audiobook.m4a"));
    }
    void plain_filename_unquoted() {
        QCOMPARE(parseContentDisposition("attachment; filename=song.mp3"),
                 QString("song.mp3"));
    }
    void extended_utf8_decodes_accents() {
        QCOMPARE(parseContentDisposition("attachment; filename*=UTF-8''Cora%C3%A7%C3%A3o.m4a"),
                 QString("Coração.m4a"));
    }
    void extended_wins_over_plain() {
        QCOMPARE(parseContentDisposition("attachment; filename=\"x.m4a\"; filename*=UTF-8''y.m4a"),
                 QString("y.m4a"));
    }
    void strips_path_traversal_to_basename() {
        QCOMPARE(parseContentDisposition("attachment; filename=\"../../etc/passwd\""),
                 QString("passwd"));
    }
    void strips_backslash_path() {
        QCOMPARE(parseContentDisposition("attachment; filename=\"..\\\\..\\\\secret.txt\""),
                 QString("secret.txt"));
    }
    void inline_without_filename_is_empty() {
        QCOMPARE(parseContentDisposition("inline"), QString());
    }
    void empty_and_malformed_are_empty() {
        QCOMPARE(parseContentDisposition(""), QString());
        QCOMPARE(parseContentDisposition("garbage;;;="), QString());
    }
};

QTEST_MAIN(TstContentDisposition)
#include "tst_contentdisposition.moc"
```

Add to `tests/CMakeLists.txt` (após o bloco `tst_transport`, linha 19):

```cmake
add_executable(tst_contentdisposition tst_contentdisposition.cpp)
target_link_libraries(tst_contentdisposition PRIVATE orbitcore Qt6::Test)
add_test(NAME tst_contentdisposition COMMAND tst_contentdisposition)
```

- [ ] **Step 2: Rodar e ver falhar (não compila — header ausente)**

Run: `cmake -S . -B build -DCMAKE_PREFIX_PATH=/opt/homebrew && cmake --build build --target tst_contentdisposition`
Expected: FAIL de compilação — `ContentDisposition.h` não existe / `parseContentDisposition` não definido.

- [ ] **Step 3: Implementar o parser**

Create `src/core/ContentDisposition.h`:

```cpp
#pragma once
#include <QString>

// Extrai um nome de arquivo seguro do VALOR de um header Content-Disposition.
// Regras: filename* (RFC 5987) vence filename; sanitização basename-only.
// Retorna string vazia quando não há filename utilizável (o chamador então
// usa o fallback derivado da URL).
QString parseContentDisposition(const QString& headerValue);
```

Create `src/core/ContentDisposition.cpp`:

```cpp
#include "ContentDisposition.h"
#include <QFileInfo>
#include <QStringList>
#include <QUrl>

namespace {

QString sanitizeFileName(QString name) {
    name.replace(QLatin1Char('\\'), QLatin1Char('/'));
    name = QFileInfo(name).fileName();          // só o basename -> bloqueia ../../
    static const QString invalid = QStringLiteral("/\\:*?\"<>|");
    QString out;
    out.reserve(name.size());
    for (const QChar c : name) {
        if (c.unicode() < 0x20) continue;       // caracteres de controle
        if (invalid.contains(c)) continue;
        out.append(c);
    }
    return out.trimmed();
}

// Divide "a=1; b=\"x;y\"; c=2" por ';' respeitando aspas duplas.
QStringList splitParams(const QString& s) {
    QStringList parts;
    QString cur;
    bool inQuotes = false;
    for (const QChar c : s) {
        if (c == QLatin1Char('"')) { inQuotes = !inQuotes; cur.append(c); }
        else if (c == QLatin1Char(';') && !inQuotes) { parts.append(cur); cur.clear(); }
        else cur.append(c);
    }
    parts.append(cur);
    return parts;
}

// filename*=  ->  charset'lang'valor-percent-encoded
QString decodeExtended(const QString& v) {
    const int q1 = v.indexOf(QLatin1Char('\''));
    if (q1 < 0) return QString();
    const int q2 = v.indexOf(QLatin1Char('\''), q1 + 1);
    if (q2 < 0) return QString();
    const QString encoded = v.mid(q2 + 1);
    // fromPercentEncoding decodifica os bytes como UTF-8 (charset comum). Um
    // charset exótico cairia em replacement chars — aceito pela spec.
    return QUrl::fromPercentEncoding(encoded.toUtf8());
}

QString stripQuotes(QString v) {
    v = v.trimmed();
    if (v.size() >= 2 && v.startsWith(QLatin1Char('"')) && v.endsWith(QLatin1Char('"')))
        v = v.mid(1, v.size() - 2);
    v.replace(QStringLiteral("\\\""), QStringLiteral("\""));   // desescapa \"
    return v;
}

} // namespace

QString parseContentDisposition(const QString& headerValue) {
    QString extended, plain;
    const QStringList params = splitParams(headerValue);
    for (const QString& raw : params) {
        const QString p = raw.trimmed();
        const int eq = p.indexOf(QLatin1Char('='));
        if (eq < 0) continue;
        const QString key = p.left(eq).trimmed().toLower();
        const QString val = p.mid(eq + 1);
        if (key == QLatin1String("filename*"))     extended = decodeExtended(val.trimmed());
        else if (key == QLatin1String("filename")) plain    = stripQuotes(val);
    }
    return sanitizeFileName(!extended.isEmpty() ? extended : plain);
}
```

Add `ContentDisposition.cpp` to `src/core/CMakeLists.txt` na lista de fontes da `orbitcore` (após `Segmentation.cpp`):

```cmake
  Segmentation.cpp
  ContentDisposition.cpp
```

- [ ] **Step 4: Rodar e ver passar**

Run: `cmake --build build --target tst_contentdisposition && ctest --test-dir build -R tst_contentdisposition --output-on-failure`
Expected: PASS (8 casos).

- [ ] **Step 5: Commit**

```bash
git add src/core/ContentDisposition.h src/core/ContentDisposition.cpp src/core/CMakeLists.txt tests/tst_contentdisposition.cpp tests/CMakeLists.txt
git commit -m "feat(core): parse filename from Content-Disposition header"
```

---

### Task 2: `HttpProbe` captura `Content-Disposition` → `ProbeResult::suggestedFileName`

**Files:**
- Modify: `src/core/DownloadTypes.h:36` (novo campo em `ProbeResult`)
- Modify: `src/core/HttpProbe.cpp:22-30` (ler o header e chamar o parser)
- Modify: `tests/TestServer.cpp` (nova rota `/named` que devolve o header)
- Modify: `tests/tst_download.cpp` (novo teste de probe)

**Interfaces:**
- Consumes: `parseContentDisposition()` (Task 1).
- Produces: `ProbeResult::suggestedFileName` (`QString`, default vazio). Preenchido pelo `HttpProbe` quando a resposta traz `Content-Disposition`.

- [ ] **Step 1: Escrever o teste que falha**

Em `tests/TestServer.cpp`, adicionar uma rota dentro de `TestServer::listen()` (após a rota `/plain`, linha 56):

```cpp
    m_http.route("/named", [this](const QHttpServerRequest& req) {
        qint64 s = 0, e = -1;
        QByteArray b = m_body;
        Resp::StatusCode code = Resp::StatusCode::Ok;
        QHttpHeaders headers;
        if (parseRange(req.value("Range"), s, e)) {
            b = partial(s, e < 0 ? m_body.size() - 1 : e);
            code = Resp::StatusCode::PartialContent;
            headers.append(QHttpHeaders::WellKnownHeader::ContentRange,
                QString("bytes %1-%2/%3").arg(s).arg(s + b.size() - 1).arg(m_body.size()).toUtf8());
        }
        Resp r("application/octet-stream", b, code);
        headers.append("Content-Disposition", "attachment; filename=\"Audiobook.m4a\"");
        r.setHeaders(std::move(headers));
        return r;
    });
```

Em `tests/tst_download.cpp`, adicionar um teste após `probePlainReportsNoRange()` (linha 83):

```cpp
    void probeReadsContentDispositionFilename() {
        TestServer srv(m_body);
        QVERIFY(srv.listen());
        QNetworkAccessManager nam;
        HttpProbe probe(&nam);
        QSignalSpy spy(&probe, &HttpProbe::finished);
        probe.start(srv.url("/named"), Credentials{});
        QVERIFY(spy.wait(3000));
        const auto res = qvariant_cast<ProbeResult>(spy.at(0).at(0));
        QVERIFY(res.ok);
        QCOMPARE(res.suggestedFileName, QString("Audiobook.m4a"));
    }

    void probeWithoutContentDispositionHasEmptyName() {
        TestServer srv(m_body);
        QVERIFY(srv.listen());
        QNetworkAccessManager nam;
        HttpProbe probe(&nam);
        QSignalSpy spy(&probe, &HttpProbe::finished);
        probe.start(srv.url("/ranged"), Credentials{});
        QVERIFY(spy.wait(3000));
        const auto res = qvariant_cast<ProbeResult>(spy.at(0).at(0));
        QVERIFY(res.suggestedFileName.isEmpty());
    }
```

- [ ] **Step 2: Rodar e ver falhar**

Run: `cmake --build build --target tst_download`
Expected: FAIL de compilação — `ProbeResult` não tem membro `suggestedFileName`.

- [ ] **Step 3: Implementar**

Em `src/core/DownloadTypes.h`, adicionar o campo em `ProbeResult` (após `resolvedUrl`, linha 36):

```cpp
    QUrl    resolvedUrl;
    QString suggestedFileName;        // do Content-Disposition; vazio se ausente
```

Em `src/core/HttpProbe.cpp`, incluir o header no topo (após linha 4):

```cpp
#include "ContentDisposition.h"
```

E ler o header em `onMetaDataChanged()`, **antes** do bloco de status (após a linha 29, `r.lastModified = ...`):

```cpp
    r.lastModified = QString::fromUtf8(m_reply->rawHeader("Last-Modified"));
    const QByteArray cd = m_reply->rawHeader("Content-Disposition");
    if (!cd.isEmpty())
        r.suggestedFileName = parseContentDisposition(QString::fromUtf8(cd));
```

- [ ] **Step 4: Rodar e ver passar**

Run: `cmake --build build --target tst_download && ctest --test-dir build -R tst_download --output-on-failure`
Expected: PASS — os 2 novos casos passam e os 27 existentes seguem passando (regressão zero).

- [ ] **Step 5: Commit**

```bash
git add src/core/DownloadTypes.h src/core/HttpProbe.cpp tests/TestServer.cpp tests/tst_download.cpp
git commit -m "feat(core): expose suggested filename from HTTP probe"
```

---

### Task 3: `NewDownloadDialog` — campo de nome editável + `destPath()` do campo

**Files:**
- Modify: `src/gui/NewDownloadDialog.h:22` (`m_name` `QLabel*` → `QLineEdit*`)
- Modify: `src/gui/NewDownloadDialog.cpp` (widget, `objectName`s, `refreshName`, `destPath`)
- Modify: `tests/tst_gui.cpp:518-535` (teste existente passa a localizar campos por `objectName`; novo teste de `destPath` a partir do campo)

**Interfaces:**
- Consumes: nada de Tasks anteriores (mudança estrutural na GUI).
- Produces: `m_name` é um `QLineEdit` com `objectName("fileNameEdit")`; `m_url` → `objectName("urlEdit")`; `m_dir` → `objectName("dirEdit")`. `destPath()` deriva de `m_name->text()`. Helper privado `void updateTypeLabel();` (atualiza `m_type` a partir de `m_name->text()`), reutilizado na Task 4.

- [ ] **Step 1: Escrever/ajustar os testes que falham**

Em `tests/tst_gui.cpp`, **substituir** o corpo de `dialog_destpath_joins_dir_and_name_without_double_slash()` (linhas 518-535) por uma versão que localiza os campos por `objectName` (robusto à ordem dos filhos) e adicionar um teste novo:

```cpp
    void dialog_destpath_joins_dir_and_name_without_double_slash() {
        NewDownloadDialog dlg;
        auto* urlEdit  = dlg.findChild<QLineEdit*>("urlEdit");
        auto* dirEdit  = dlg.findChild<QLineEdit*>("dirEdit");
        QVERIFY(urlEdit && dirEdit);

        urlEdit->setText("https://x.com/file.zip");

        dirEdit->setText("/tmp/orbit-test-dir");
        QCOMPARE(dlg.destPath(), QString("/tmp/orbit-test-dir/file.zip"));

        dirEdit->setText("/tmp/orbit-test-dir/");   // barra final não pode duplicar
        QCOMPARE(dlg.destPath(), QString("/tmp/orbit-test-dir/file.zip"));
    }

    void dialog_destpath_uses_edited_name_field() {
        NewDownloadDialog dlg;
        auto* urlEdit  = dlg.findChild<QLineEdit*>("urlEdit");
        auto* dirEdit  = dlg.findChild<QLineEdit*>("dirEdit");
        auto* nameEdit = dlg.findChild<QLineEdit*>("fileNameEdit");
        QVERIFY(urlEdit && dirEdit && nameEdit);

        urlEdit->setText("https://x.com/download");   // path dá "download"
        QCOMPARE(nameEdit->text(), QString("download"));

        dirEdit->setText("/tmp/orbit-test-dir");
        nameEdit->setText("Audiobook.m4a");            // usuário renomeia
        QCOMPARE(dlg.destPath(), QString("/tmp/orbit-test-dir/Audiobook.m4a"));
    }
```

- [ ] **Step 2: Rodar e ver falhar**

Run: `cmake --build build --target tst_gui`
Expected: FAIL de compilação (`m_name` ainda é `QLabel`, `findChild<QLineEdit*>("fileNameEdit")` retornaria nullptr em runtime; e o novo teste espera `destPath` do campo). Após compilar, `dialog_destpath_uses_edited_name_field` falha.

- [ ] **Step 3: Converter o widget e ajustar `destPath`/`refreshName`**

Em `src/gui/NewDownloadDialog.h`: trocar o tipo de `m_name` e declarar o helper:

```cpp
private slots:
    void chooseDir();
    void refreshName();
private:
    void updateTypeLabel();          // m_type a partir de m_name->text()
    QLineEdit* m_url;
    QLineEdit* m_dir;
    QLineEdit* m_name;               // era QLabel*
    QLabel*    m_type;
```

Em `src/gui/NewDownloadDialog.cpp`:

Trocar a criação de `m_name` (linha 25) e nomear os campos (logo após criar cada um):

```cpp
    m_url  = new QLineEdit(this);
    m_url->setObjectName("urlEdit");
    m_dir  = new QLineEdit(QStandardPaths::writableLocation(QStandardPaths::DownloadLocation), this);
    m_dir->setObjectName("dirEdit");
    m_name = new QLineEdit(this);
    m_name->setObjectName("fileNameEdit");
    m_type = new QLabel("—", this);
```

Substituir `refreshName()` (linhas 68-74) e ajustar `destPath()` (linha 77):

```cpp
void NewDownloadDialog::refreshName() {
    m_name->setText(deriveFileName(url()));   // fallback visível imediato
    updateTypeLabel();
}

void NewDownloadDialog::updateTypeLabel() {
    m_type->setText(FileType::displayName(FileType::categorize(m_name->text())));
}

QUrl    NewDownloadDialog::url() const      { return QUrl(m_url->text().trimmed()); }
QString NewDownloadDialog::destPath() const { return QDir(m_dir->text()).filePath(m_name->text()); }
```

- [ ] **Step 4: Rodar e ver passar**

Run: `cmake --build build --target tst_gui && ctest --test-dir build -R tst_gui --output-on-failure`
Expected: PASS — os dois testes de `destPath` passam e os demais casos de `tst_gui` seguem passando (incluindo `urlname_derives_filename`, que não usa o diálogo).

- [ ] **Step 5: Commit**

```bash
git add src/gui/NewDownloadDialog.h src/gui/NewDownloadDialog.cpp tests/tst_gui.cpp
git commit -m "feat(gui): make download name field editable and honor it in destPath"
```

---

### Task 4: Probe assíncrono no diálogo (debounce + preenchimento + proteção de edição)

**Files:**
- Modify: `src/gui/NewDownloadDialog.h` (NAM próprio, timer, flag, slot público `applyProbeResult`)
- Modify: `src/gui/NewDownloadDialog.cpp` (wiring do probe, debounce, guardas)
- Modify: `tests/tst_gui.cpp` (testes de preenchimento, proteção de edição e probe obsoleto — sem rede)

**Interfaces:**
- Consumes: `ProbeResult::suggestedFileName` (Task 2); `updateTypeLabel()`, `objectName`s (Task 3); `HttpProbe`, `QNetworkAccessManager` (Core/Qt).
- Produces: slot público `void applyProbeResult(const QUrl& probedUrl, const ProbeResult& res);` — aplica o nome sugerido ao campo se `probedUrl` ainda é a URL atual, o nome não é vazio e o usuário não editou. É o ponto de teste sem rede.

- [ ] **Step 1: Escrever os testes que falham (sem rede)**

Em `tests/tst_gui.cpp`, adicionar (incluir `<QTest>`/`QTest::keyClicks` já disponível via `<QtTest>`):

```cpp
    void dialog_probe_fills_name_when_not_edited() {
        NewDownloadDialog dlg;
        auto* urlEdit  = dlg.findChild<QLineEdit*>("urlEdit");
        auto* nameEdit = dlg.findChild<QLineEdit*>("fileNameEdit");
        QVERIFY(urlEdit && nameEdit);

        urlEdit->setText("https://drive/download?id=1");
        QCOMPARE(nameEdit->text(), QString("download"));      // fallback da URL

        ProbeResult r; r.ok = true; r.suggestedFileName = "Audiobook.m4a";
        dlg.applyProbeResult(QUrl("https://drive/download?id=1"), r);

        QCOMPARE(nameEdit->text(), QString("Audiobook.m4a")); // preenchido
    }

    void dialog_probe_does_not_overwrite_user_edit() {
        NewDownloadDialog dlg;
        auto* urlEdit  = dlg.findChild<QLineEdit*>("urlEdit");
        auto* nameEdit = dlg.findChild<QLineEdit*>("fileNameEdit");
        QVERIFY(urlEdit && nameEdit);

        urlEdit->setText("https://drive/download?id=1");
        nameEdit->clear();
        QTest::keyClicks(nameEdit, "meu-nome.m4a");           // edição do usuário (textEdited)

        ProbeResult r; r.ok = true; r.suggestedFileName = "Audiobook.m4a";
        dlg.applyProbeResult(QUrl("https://drive/download?id=1"), r);

        QCOMPARE(nameEdit->text(), QString("meu-nome.m4a"));  // preservado
    }

    void dialog_probe_ignores_stale_result() {
        NewDownloadDialog dlg;
        auto* urlEdit  = dlg.findChild<QLineEdit*>("urlEdit");
        auto* nameEdit = dlg.findChild<QLineEdit*>("fileNameEdit");
        QVERIFY(urlEdit && nameEdit);

        urlEdit->setText("https://drive/download?id=NEW");    // nome = "download"
        ProbeResult r; r.ok = true; r.suggestedFileName = "obsoleto.m4a";
        dlg.applyProbeResult(QUrl("https://drive/download?id=OLD"), r);   // URL antiga

        QCOMPARE(nameEdit->text(), QString("download"));      // ignorado
    }

    void dialog_probe_empty_name_keeps_fallback() {
        NewDownloadDialog dlg;
        auto* urlEdit  = dlg.findChild<QLineEdit*>("urlEdit");
        auto* nameEdit = dlg.findChild<QLineEdit*>("fileNameEdit");
        QVERIFY(urlEdit && nameEdit);

        urlEdit->setText("https://x.com/file.zip");
        ProbeResult r; r.ok = true; r.suggestedFileName = "";  // sem header
        dlg.applyProbeResult(QUrl("https://x.com/file.zip"), r);

        QCOMPARE(nameEdit->text(), QString("file.zip"));       // fallback mantido
    }
```

- [ ] **Step 2: Rodar e ver falhar**

Run: `cmake --build build --target tst_gui`
Expected: FAIL de compilação — `applyProbeResult` não existe.

- [ ] **Step 3: Implementar o wiring do probe**

Em `src/gui/NewDownloadDialog.h`, adicionar includes/forward-decls e membros:

```cpp
#pragma once
#include <QDialog>
#include <QUrl>
#include "DownloadTypes.h"          // ProbeResult
class QLineEdit;
class QLabel;
class QTimer;
class QNetworkAccessManager;
class HttpProbe;

class NewDownloadDialog : public QDialog {
    Q_OBJECT
public:
    explicit NewDownloadDialog(QWidget* parent = nullptr, const QUrl& prefill = QUrl());
    QUrl        url() const;
    QString     destPath() const;
    static bool isValidDownloadUrl(const QUrl& u);
public slots:
    // Aplica o nome sugerido se `probedUrl` ainda é a URL atual, o nome não é
    // vazio e o usuário não editou o campo. Ponto de teste sem rede.
    void applyProbeResult(const QUrl& probedUrl, const ProbeResult& res);
private slots:
    void chooseDir();
    void refreshName();
    void onUrlChanged();
    void startProbe();
private:
    void updateTypeLabel();
    QLineEdit* m_url;
    QLineEdit* m_dir;
    QLineEdit* m_name;
    QLabel*    m_type;
    QNetworkAccessManager* m_nam       = nullptr;
    QTimer*                m_debounce  = nullptr;
    HttpProbe*             m_probe     = nullptr;
    QUrl                   m_probeUrl;
    bool                   m_nameEdited = false;
};
```

Em `src/gui/NewDownloadDialog.cpp`, adicionar includes:

```cpp
#include "HttpProbe.h"
#include <QNetworkAccessManager>
#include <QTimer>
```

No construtor, após criar os campos e antes do `refreshName()` final (linha 60), configurar NAM, timer e conexões. **Trocar** a conexão atual de `m_url` (linha 39) por `onUrlChanged` e conectar a edição manual do nome:

```cpp
    m_nam = new QNetworkAccessManager(this);
    m_debounce = new QTimer(this);
    m_debounce->setSingleShot(true);
    m_debounce->setInterval(400);
    connect(m_debounce, &QTimer::timeout, this, &NewDownloadDialog::startProbe);

    connect(m_url,  &QLineEdit::textChanged, this, &NewDownloadDialog::onUrlChanged);
    connect(m_name, &QLineEdit::textEdited,  this, [this]{ m_nameEdited = true; });
```

E, no **fim** do construtor (após o `refreshName()` da linha 60), disparar um probe para uma URL de prefill/clipboard já presente — senão o fluxo "abrir New com link no clipboard" nunca sondaria:

```cpp
    refreshName();
    if (isValidDownloadUrl(url())) m_debounce->start();   // prefill/clipboard também sonda
```

> Nota: `QLineEdit::textEdited` só dispara em edição do usuário, **não** em `setText()` programático — por isso preencher o campo pelo probe/`refreshName` não marca `m_nameEdited`. Não é preciso `QSignalBlocker`. O prefill (linhas 30-35) roda **antes** das conexões, então não aciona `onUrlChanged`; por isso o kick explícito acima.

Implementar os slots (após `refreshName`/`updateTypeLabel` da Task 3):

```cpp
void NewDownloadDialog::onUrlChanged() {
    m_nameEdited = false;          // URL nova => nome sugerido é bem-vindo
    refreshName();                 // fallback visível imediato
    m_debounce->start();           // (re)agenda o probe
}

void NewDownloadDialog::startProbe() {
    const QUrl u = url();
    if (!isValidDownloadUrl(u) || u.scheme().compare("ftp", Qt::CaseInsensitive) == 0)
        return;                    // só HTTP(S)
    if (m_probe) { m_probe->deleteLater(); m_probe = nullptr; }
    m_probeUrl = u;
    m_probe = new HttpProbe(m_nam, this);
    connect(m_probe, &HttpProbe::finished, this,
            [this, u](const ProbeResult& r){ applyProbeResult(u, r); });
    m_probe->start(u, Credentials{});
}

void NewDownloadDialog::applyProbeResult(const QUrl& probedUrl, const ProbeResult& res) {
    if (probedUrl != url()) return;               // probe obsoleto
    if (!res.ok || res.suggestedFileName.isEmpty()) return;
    if (m_nameEdited) return;                      // respeita edição manual
    m_name->setText(res.suggestedFileName);        // setText -> não dispara textEdited
    updateTypeLabel();
}
```

> `isValidDownloadUrl` aceita `http/https/ftp`; a guarda extra exclui `ftp` do probe. `Credentials{}` vem de `Transport.h` (via `HttpProbe.h`).

- [ ] **Step 4: Rodar e ver passar**

Run: `cmake --build build --target tst_gui && ctest --test-dir build -R tst_gui --output-on-failure`
Expected: PASS — os 4 novos casos passam; os testes das Tasks 3 e os pré-existentes seguem passando.

- [ ] **Step 5: Rodar a suíte inteira (regressão)**

Run: `ctest --test-dir build --output-on-failure`
Expected: PASS — todas as suítes (`tst_contentdisposition`, `tst_download`, `tst_gui`, `tst_ftp`, etc.).

- [ ] **Step 6: Commit**

```bash
git add src/gui/NewDownloadDialog.h src/gui/NewDownloadDialog.cpp tests/tst_gui.cpp
git commit -m "feat(gui): auto-detect filename via debounced probe on URL entry"
```

---

### Task 5: Verificação manual (critério 9 da spec)

**Files:** nenhum (verificação de integração com o app rodando).

- [ ] **Step 1: Compilar o app**

Run: `cmake --build build --target orbit-gui`
Expected: build OK.

- [ ] **Step 2: Testar o caso real do Google Drive**

1. Rodar `./build/src/gui/orbit-gui` (ou caminho equivalente do binário).
2. Abrir o diálogo New, colar a URL do audiobook:
   `https://drive.usercontent.google.com/download?id=12tYRqA310DhuWYAuppAR3y1sXAXV2w7N&export=download&authuser=0&confirm=t&uuid=...`
3. Após ~1 s, o campo **File** deve exibir o nome real com extensão `.m4a` (não `download`).
4. Confirmar o download e conferir que o arquivo salvo na pasta de destino tem o nome/extensão corretos.
5. Repetir com uma URL http comum com nome no path (ex.: `.../arquivo.zip`) e confirmar que o comportamento **não** regrediu.

- [ ] **Step 3: Registrar o resultado**

Anotar no PR/commit final que a verificação manual passou (ou abrir tarefa se o Drive responder a página HTML de confirmação — fora de escopo, ver spec §9).

---

## Self-Review

**Cobertura da spec:**
- §3.1 parser (`filename*` prioridade, sanitização basename) → Task 1 (8 casos cobrem os critérios 1–5).
- §3.2 `HttpProbe`/`ProbeResult` → Task 2 (critérios 6, 7; regressão zero verificada no Step 4).
- §3.3 campo editável + `destPath` do campo → Task 3.
- §3.3/§4 debounce, proteção de edição, probe obsoleto, timeout/fallback → Task 4 (critério 8). Nota: o **timeout** de 5 s do probe live não é coberto por teste unitário (exigiria rede); o fallback por nome vazio/erro é coberto por `applyProbeResult` com `res.ok=false`/nome vazio. O `QTimer` de timeout do probe pode ser adicionado em `startProbe`/`applyProbeResult` se desejado, mas a spec o trata como refinamento — mantido fora do caminho testado; o caminho de erro do `HttpProbe` (`onErrorOccurred`) já produz `res.ok=false`, que `applyProbeResult` ignora.
- §3 nota de dependência (NAM próprio do diálogo) → Task 4 (`m_nam` membro).
- §5 casos de borda → Task 4 (obsoleto, nome vazio, edição) + Task 1 (traversal, malformado).
- §6 testes → Tasks 1, 2, 3, 4. §7 verificação manual → Task 5.

**Gaps conscientes:**
1. **Timeout do probe (spec §4):** não implementado como `QTimer` dedicado nesta versão — o `HttpProbe` não expõe cancelamento externo e seu `onErrorOccurred` já cobre falha de rede via `res.ok=false`. Se o probe ficar pendente indefinidamente, o campo permanece no fallback da URL (já visível), então o impacto é só "o nome não é auto-preenchido". Aceitável; anotado para eventual refinamento.
2. **Exibição do tamanho no diálogo (spec §3.3, "atualiza o tamanho a partir de `totalBytes`"):** o diálogo atual não tem um rótulo de tamanho; adicioná-lo seria escopo de UI extra não coberto pela spec §2 (critérios). Deixado de fora — o valor está em `ProbeResult.totalBytes` e pode ser exibido numa iteração futura sem retrabalho.

**Placeholders:** nenhum — todo passo tem código/comando concreto.

**Consistência de tipos:** `parseContentDisposition(const QString&)→QString` (Task 1) usado em Task 2; `ProbeResult::suggestedFileName` (Task 2) usado em Task 4; `applyProbeResult(const QUrl&, const ProbeResult&)`, `updateTypeLabel()`, `objectName`s consistentes entre Tasks 3 e 4.
