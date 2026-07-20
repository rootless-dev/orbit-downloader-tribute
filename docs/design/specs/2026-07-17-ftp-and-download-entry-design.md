# Orbit Downloader Tribute — Fase 3: Entrada de downloads + FTP

**Data:** 2026-07-17
**Projeto:** `orbit-downloader-tribute`
**Escopo desta spec:** Fase 3 — abstração de transporte no Core, `FtpProtocol` multi-segmento sobre
`QTcpSocket` (SIZE, MDTM, REST, PASV, RETR), monitor de área de transferência, drag & drop na janela
e diálogo New estendido (pasta + tipo detectado). **Sem** scheduler, Preferences, `settings.json`,
categorias persistidas, FTPS ou importação de listas (fases 4/5 ou fora de escopo).

---

## 1. Contexto

A Fase 1 entregou o Core headless (`orbitcore`): motor HTTP/HTTPS multi-segmento com pause/resume que
sobrevive ao fechamento do processo. A Fase 2 deu-lhe rosto: `orbit-gui` com o layout do Orbit, a
grade de blocos por segmento e o diálogo New. Ambas estão commitadas em `develop` (`4ba183d`).

**Esta spec cobre a Fase 3**, que tem duas frentes de tamanhos muito diferentes:

- **FTP** — a maior parte do trabalho. O Core hoje é acoplado a HTTP **na estrutura**, não só na
  implementação: `DownloadTask` recebe um `QNetworkAccessManager*` no construtor, `SegmentWorker`
  fala `QNetworkReply` diretamente, `HttpProbe` é concreto. FTP não encaixa sem uma abstração de
  transporte.
- **Entrada de downloads** — clipboard, drag & drop e New estendido. Só GUI, sem tocar no Core.

Optou-se por **uma spec única** cobrindo as duas frentes, como o ROADMAP previa.

### Decisões já tomadas (brainstorming desta fase)

- **Abstração `Transport`** (uma interface dona do probe **e** do worker), com registry
  `scheme → Transport*` no `DownloadManager`. O `DownloadTask` perde o `QNetworkAccessManager*` e
  passa a receber um `Transport*`. Descartadas: duas interfaces independentes (`IProbe`/`ISegmentSource`
  — permitiriam misturar protocolos) e um `FtpDownloadTask` paralelo (duplicaria a parte mais delicada
  do projeto).
- **FTP multi-segmento com fallback para single-connection**, espelhando o que o HTTP já faz via
  `supportsRange`. Sem `SIZE` ou sem `REST` → uma conexão só, do início.
- **Autenticação:** anônimo, `user:senha` na URL, **e diálogo de credenciais** quando o servidor
  recusa (`530`). Sem FTPS (`AUTH TLS`) — fora de escopo.
- **Credenciais do diálogo ficam só em memória**, nunca no `.meta`. Senha em texto puro no disco não.
- **Monitor de clipboard com 4 modos** (Off/Ask/Auto/Notify) em `Tools → Clipboard monitor`, rádio,
  padrão **Off**, **sem persistência** (`settings.json` é da Fase 4; `QSettings` criaria um segundo
  mecanismo de config convivendo com o futuro).
- **Categoria segue derivada** (estado + extensão, como na Fase 2). O New **não** escolhe categoria —
  ganha apenas um rótulo somente-leitura do tipo detectado. Categoria persistida é da Fase 4.
  Isto é um **desvio consciente** da letra do ROADMAP ("diálogo New com escolha de pasta/categoria").
- **Drag & drop aceita links e texto** (`text/uri-list`, `text/plain`). Arquivos `.txt`/`.html` com
  listas de URLs são feature própria — fora de escopo.
- **`TestFtpServer` mínimo sobre `QTcpServer`** para manter a suíte offline. Sem ele, os caminhos mais
  arriscados do FTP (fallback, resume) ficariam sem cobertura.

---

## 2. Objetivo e critérios de sucesso

**Objetivo:** baixar de servidores FTP com a mesma aceleração multi-segmento, grade de blocos e
pause/resume que o HTTP já tem, e alimentar a fila de downloads pelos gestos naturais do Orbit
clássico — copiar um link, arrastar um link, ou colar no diálogo New.

Critérios **1–14** são verificáveis por teste automatizado (QtTest, `offscreen`); **15–16** são de
integração e verificados manualmente com o app rodando (ver §7).

### Core / transporte

1. **`FakeTransport` dirige o `DownloadTask` sem rede:** um `Transport` de teste (probe programável +
   worker que escreve bytes sintéticos) leva uma task de `Queued` a `Completed`, provando que a
   abstração não vaza HTTP.
2. **Registry por esquema:** `DownloadManager` resolve `http`/`https` → `HttpTransport` e `ftp` →
   `FtpTransport`; esquema desconhecido faz `addDownload` retornar um `QUuid` nulo sem criar task nem
   `.meta` (a GUI já barra isso antes, via `isValidDownloadUrl` — esta é a defesa do Core).
3. **Regressão zero no HTTP:** `tst_download` (27 casos) passa **sem alteração de expectativa** — só
   o ajuste mecânico de construtor. Mudança de comportamento aqui = refatoração quebrou algo.

### FTP

4. **`FtpProbe` feliz:** contra o `TestFtpServer`, `SIZE` → `totalBytes`, `MDTM` → `lastModified`,
   `REST 1` → `350` → `supportsRange == true`.
5. **`FtpProbe` sem `SIZE` ou sem `REST`:** `supportsRange == false` (e `totalBytes == -1` no caso do
   `SIZE`), levando ao fallback single-connection.
6. **Parse do `227`:** `227 Entering Passive Mode (h1,h2,h3,h4,p1,p2)` → host/porta corretos, em
   função pura. Casos: formato canônico, com texto extra, malformado → erro.
7. **Download FTP multi-segmento:** N segmentos, arquivo resultante **byte-idêntico** ao original.
8. **Corte no `end`:** o worker de um segmento não-final para de escrever ao passar do seu `end`,
   fecha o socket de dados e não contamina a fatia seguinte. O servidor manda até o fim do arquivo —
   quem corta somos nós. Caso dedicado, é a lógica sem paralelo no HTTP.
9. **Fallback single-connection:** servidor sem `REST` → um segmento, do zero, arquivo íntegro.
10. **Resume FTP válido (multi-segmento):** matar e recarregar a sessão com `MDTM` inalterado →
    retoma dos offsets do `.meta`, arquivo final íntegro. **Só vale no caminho `REST`:** resume de um
    download em fallback (`end == -1`) recomeça do zero — regra herdada da Fase 1 §3.4
    (`beginSegments` zera segmentos `end < 0` e trunca o arquivo), não um bug novo.
11. **Resume FTP com `MDTM` divergente:** o worker compara `MDTM` com o validador do `.meta` e emite
    `restartRequired` → descarta o parcial e recomeça do zero. FTP não tem `If-Range`; a verificação é
    explícita e fica **no worker**, não no probe (§3.5).
12. **Auth pelo probe (download novo):** `530` → `Paused` + `credentialsRequired(id, host)`;
    `provideCredentials(id, user, pass)` → download completa. `550` → `Error` (fatal, sem retry).
13. **Auth pelo worker (resume):** task restaurada do `.meta` não sonda (`m_probed == true`), então o
    `530` chega por `failed(..., AuthRequired)` → mesmo `Paused` + `credentialsRequired`. Com N
    segmentos levando `530`, o sinal é emitido **uma única vez** (guarda `m_awaitingCredentials`) —
    senão são N diálogos.

### GUI

14. **Unidades puras:** `shouldOffer(text, lastOffered, selfCopied)` (só http/https/ftp; ignora cópia
    própria; ignora repetição imediata), `extractUrls(mimeData)` (uri-list e text/plain; drop sem URL
    baixável → lista vazia) e `isValidDownloadUrl` (aceita ftp; rejeita esquema desconhecido).
15. **Manual:** os 4 modos de clipboard fazem o que dizem (Off nada; Ask abre New pré-preenchido;
    Auto enfileira; Notify mostra mensagem clicável na barra de status).
16. **Manual:** arrastar um link do navegador abre o New pré-preenchido; arrastar vários enfileira
    todos; um download FTP real progride com grade de blocos, pausa e retoma.

---

## 3. Arquitetura

### 3.1 A abstração `Transport`

O que o `DownloadTask` realmente pede ao mundo, hoje, e que FTP precisa fornecer igual:

- **sondar** uma URL → `ProbeResult` (total, suporta range, validador)
- **criar um worker por segmento** que escreve num `QFile` e emite
  `progressed`/`completed`/`failed`/`restartRequired`

```cpp
// src/core/Transport.h  — sem QtWidgets, sem QNAM
struct Credentials { QString user, pass; };   // vazio = anônimo; HttpTransport ignora (por ora)

enum class FailureKind { Fatal, AuthRequired };   // `failed` só é emitido quando o worker desiste;
                                                  // o retry recuperável é interno ao worker

class Probe : public QObject {          // interface do que HttpProbe já é
    Q_OBJECT
public:
    virtual void start(const QUrl& url, const Credentials& creds) = 0;
signals:
    void finished(const ProbeResult& result);
};

class SegmentSource : public QObject {  // interface do que SegmentWorker já é
    Q_OBJECT
public:
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

class Transport {
public:
    virtual ~Transport() = default;
    virtual Probe*         createProbe(QObject* parent) = 0;
    virtual SegmentSource* createWorker(QFile* file, const EngineConfig& cfg, QObject* parent) = 0;
};
```

**Ownership:** o `DownloadManager` é dono dos `Transport` (`std::unique_ptr` no registry) e do
`QNetworkAccessManager`, que passa a ser construído **dentro** do `HttpTransport`. `Transport` não é
`QObject` — não tem sinais nem filhos; os objetos que ele cria recebem o `parent` passado (a
`DownloadTask`), preservando a árvore de ownership atual. Transports são compartilhados por todas as
tasks e por isso **não guardam credenciais** — elas trafegam por parâmetro.

Implementações: `HttpTransport(QNetworkAccessManager*)` → `HttpProbe` + `HttpSegmentWorker`
(`SegmentWorker` renomeado); `FtpTransport()` → `FtpProbe` + `FtpSegmentWorker`.

O `DownloadTask` mantém **toda** a lógica atual — máquina de estados, segmentação, resume, `.meta`,
progresso, cap de concorrência — intocada. Muda só de quem recebe as peças:

```cpp
DownloadTask(Transport* transport, const EngineConfig& cfg, QObject* parent = nullptr);
```

`DownloadManager` mantém o registry e injeta:

```cpp
Transport* transportFor(const QUrl& url) const;   // scheme → Transport*, nullptr se desconhecido
```

`FtpCredentials { QString user, pass; }` (vazio = anônimo) trafega como parâmetro porque credenciais
de diálogo vivem só em memória, na `DownloadTask` — nunca no `Transport`, que é compartilhado entre
todas as tasks.

O `failed` ganha um `FailureKind`. Hoje o `SegmentWorker` classifica erro internamente via
`isRecoverable(QNetworkReply::NetworkError)` — um enum HTTP que não sobrevive à abstração; a decisão
sobe para o worker concreto, e o que atravessa a interface é o **efeito** (desistir por erro fatal vs.
desistir pedindo credenciais), não a causa. `AuthRequired` existe porque, no resume, o `530` chega
pelo worker e não pelo probe (§3.5/§3.6) — sem ele, uma sessão FTP autenticada recarregada iria direto
a `Error`, sem diálogo.

### 3.2 `FtpProbe`

Uma conexão de controle, sequência linear:

```
conectar → 220 → USER <u|anonymous> → 331 → PASS <p|orbit@tribute>
        → 230 → TYPE I → 200
        → SIZE <path>  → 213 <n>            → totalBytes    (falha → totalBytes = -1)
        → MDTM <path>  → 213 YYYYMMDDHHMMSS → lastModified  (falha → validador vazio)
        → REST 1       → 350                → supportsRange (qualquer outra resposta → false)
        → REST 0 → QUIT
```

**O probe só roda em download novo.** No resume, `restore()` marca `m_probed = true` e `start()` vai
direto a `beginSegments()` — os workers assumem `totalBytes`/`supportsRange` do `.meta`. Daí a
validação e a auth também precisarem existir no worker (§3.5, §3.6).

- `530` em qualquer ponto do login → `ProbeResult{ok=false, authRequired=true}`.
- `550` no `SIZE` (arquivo inexistente) → `ProbeResult{ok=false, error="550 ..."}`, fatal.
- **FTP não tem ETag.** O validador é o `MDTM`, guardado no campo `lastModified` que o `.meta` já tem.
  O campo `etag` fica vazio para FTP.
- `ProbeResult` ganha `bool authRequired = false`.

### 3.3 `FtpSegmentWorker`

Um por segmento, com **conexão de controle própria**:

```
conectar → login → TYPE I
        → [se validator não-vazio] MDTM <path> → 213 → compara → divergente: restartRequired (§3.5)
        → PASV → 227 (h1,h2,h3,h4,p1,p2) → abrir socket de dados
        → REST <seg.current> → 350          → falhou: restartRequired, não failed (§3.5)
        → RETR <path> → 150
        → ler socket de dados, escrever em file->seek(current)
```

**O ponto novo — corte no `end`.** FTP não sabe parar num byte: o servidor manda até o fim do arquivo.
Cada worker cujo `end >= 0` trunca o que exceder o `end`, emite `completed`, fecha o socket de dados e
encerra a conexão de controle (sem esperar `226`). O worker do último segmento (`end == -1`, fallback)
lê até o `226`. Esta é a única lógica sem paralelo no HTTP — daí o critério 8.

**Classificação de erro:**

| Resposta | Tratamento |
|---|---|
| `421` (service not available / too many connections), `425` (can't open data connection), `426` (transfer aborted), queda de socket, timeout | recuperável → retry com backoff (`maxSegmentRetries`) |
| `530` (auth), `550` (não existe / sem permissão) | fatal → `failed(..., fatal=true)`, sem retry |

**Limite de conexões por IP:** N segmentos = N conexões de controle + N de dados. Se o servidor recusar
as extras (`421`), isso cai como recuperável e o backoff resolve sozinho — segmentos que terminam
liberam vaga. Sem knob novo no `EngineConfig`.

**Timeouts** reusam `connectTimeoutMs` e `idleTimeoutMs`; o `FtpSegmentWorker` arma os mesmos timers
que o `HttpSegmentWorker`.

### 3.4 Parser de respostas (unidades puras)

Isoladas em `src/core/FtpReply.{h,cpp}`, sem socket, testáveis direto:

```cpp
struct FtpReply { int code = 0; QString text; bool complete = false; };
FtpReply parseReply(const QByteArray& buf, int* consumed);   // trata multi-linha "213-...\r\n213 ..."
std::optional<QPair<QString,quint16>> parsePasv(const QString& line227);
std::optional<QDateTime> parseMdtm(const QString& line213);
```

### 3.5 Resume FTP — validação no worker, não no probe

**No resume o probe não roda.** `DownloadTask::restore()` faz `m_probed = !segs.isEmpty()`, e
`start()` só sonda quando `!m_probed` — uma task restaurada com segmentos vai direto para
`beginSegments()`. Além disso, `DownloadRecord` **não guarda validador**: `etag`/`lastModified` vivem
no `.meta` (`Persistence::readMeta`). Portanto não existe ponto de comparação "SIZE/MDTM vs. record"
no `loadSession`.

O HTTP resolve isso **dentro do worker**, sem round-trip extra: manda `If-Range: <validador>` e, se o
recurso mudou, o servidor responde `200` em vez de `206` — `SegmentWorker::onReadyRead` detecta
(`m_expectPartial && status == 200`) e emite `restartRequired`, que faz o `DownloadTask` zerar todos
os segmentos, limpar validadores e recomeçar.

FTP não tem `If-Range`, então o `FtpSegmentWorker` faz a mesma verificação **explicitamente**, na
mesma condição em que o HTTP manda `If-Range` (validador não-vazio):

```
login → TYPE I
     → se validator não-vazio:  MDTM <path>  →  compara com validator
                                  divergente  →  emit restartRequired(index)   [não é erro]
     → PASV → REST → RETR ...
```

Isto reusa **exatamente** o caminho `restartRequired` que já existe e é testado. Três consequências
que precisam estar escritas:

- **Custo:** um round-trip `MDTM` por worker por tentativa. É o preço de o FTP não ter `If-Range`.
  Só ocorre quando há validador — igual à condição do HTTP.
- **Sem loop de restart:** `DownloadTask::onRestartRequired` limpa `m_validated`, `m_etag` e
  `m_lastModified` antes de re-chamar `beginSegments()`. Na segunda passada o validador está vazio,
  o worker pula o `MDTM` e baixa do zero. O restart é sempre finito.
- **Validador vazio → sem verificação.** Servidor sem `MDTM` significa resume não-validado: se o
  arquivo mudou entre sessões, o resultado é um arquivo corrompido silenciosamente. É a mesma
  exposição que o HTTP sem `ETag`/`Last-Modified` já tem hoje (Fase 1), não uma regressão nova.

**Se o `REST` falhar no worker** (servidor mudou de comportamento desde o probe que gravou
`supportsRange = true` no `.meta`), o worker emite `restartRequired`, não `failed`: não dá para
retomar, mas dá para recomeçar.

**Fallback FTP não retoma.** Segmento com `end < 0` cai na regra da Fase 1 (`beginSegments`, §3.4):
`current` volta a `start` e o arquivo é truncado. Resume de download FTP em fallback **recomeça do
zero**, por design herdado — ver critérios 9 e 10.

**Nenhuma mudança em `Persistence`.** O `.meta` já carrega `etag`/`lastModified`/`validated`; FTP
deixa `etag` vazio e usa `lastModified` para o `MDTM`. `spawnWorker` já escolhe
`etag.isEmpty() ? lastModified : etag` como validador — FTP cai naturalmente no `MDTM`.

### 3.6 Fluxo de credenciais (GUI ↔ Core)

O único caminho em que o Core **pergunta** algo à GUI. Sinais, sem QtWidgets.

**Dois pontos de entrada**, não um — porque no resume o probe não roda (§3.5):

```
(a) download novo:  FtpProbe: 530  →  ProbeResult{ok=false, authRequired=true}  →  onProbed
(b) resume:         FtpSegmentWorker: 530  →  failed(idx, msg, AuthRequired)    →  onSegmentFailed
```

Ambos convergem para o mesmo tratamento na `DownloadTask`:

```
para workers  →  writeMeta  →  state = Paused  →  emit credentialsRequired(id, host)   [uma vez]
DownloadManager (repassa via wire()) → MainWindow → diálogo user/senha
DownloadManager::provideCredentials(id, user, pass) → task guarda em memória → resume(id)
```

**Guarda contra diálogo duplicado:** no caso (b) há N workers e cada um leva `530`, ou seja N ×
`failed(AuthRequired)`. A `DownloadTask` mantém `m_awaitingCredentials` e emite
`credentialsRequired` **uma única vez** por rodada; `provideCredentials` limpa a flag. Sem isso, um
FTP autenticado com 4 segmentos abre 4 diálogos.

`onProbed` hoje é `if (!r.ok) { setState(Error); return; }` e `onSegmentFailed` vai incondicionalmente
a `Error` — ambos ganham o desvio de `AuthRequired` antes desse fallback.

Reuso de `Paused` é deliberado: `DownloadState::AuthRequired` obrigaria a mexer no model, na árvore e
nos testes da Fase 2 por ganho cosmético. Cancelar o diálogo deixa a task `Paused`, e o botão Start
tenta de novo. Após recarregar a sessão as credenciais somem (não são persistidas): o próximo Start
pergunta de novo — e é justamente o caminho (b).

### 3.7 Entrada de downloads (GUI)

**Diálogo New:** `isValidHttpUrl` → `isValidDownloadUrl` (aceita `http`/`https`/`ftp`, mensagem
específica por rejeição). Ganha rótulo somente-leitura do tipo detectado, reusando `FileType`. Pasta e
preview do nome seguem como estão. Sem escolha de categoria.

**`ClipboardWatcher`** (`QObject`, liga em `QGuiApplication::clipboard()->dataChanged`) com a decisão
numa função pura:

```cpp
std::optional<QUrl> shouldOffer(const QString& text, const QUrl& lastOffered, bool selfCopied);
```

Modos em `Tools → Clipboard monitor` (rádio, padrão **Off**, sem persistir):

| Modo | Comportamento |
|---|---|
| Off | nada |
| Ask | abre o New pré-preenchido |
| Auto | enfileira direto na pasta padrão |
| Notify | `QStatusBar` com mensagem clicável ("Link detectado — baixar?") |

**Drag & drop:** `MainWindow` aceita `text/uri-list` e `text/plain`. Parsing puro:

```cpp
QList<QUrl> extractUrls(const QMimeData* mime);
```

Uma URL → New pré-preenchido. Várias → enfileira todas na pasta padrão (perguntar N vezes seria
insuportável). Nenhuma URL baixável → drop rejeitado, sem diálogo de erro.

**Pasta padrão** (Auto e drop múltiplo) = último diretório escolhido no New nesta sessão, ou
`QStandardPaths::DownloadLocation` na primeira vez. Em memória, coerente com não persistir config
nesta fase.

---

## 4. Estrutura de arquivos

```
src/core/Transport.h            → Probe, SegmentSource, Transport, Credentials,
                                  FailureKind (interfaces)                         [novo]
src/core/HttpTransport.{h,cpp}  → HttpTransport (dono do QNAM)                     [novo]
src/core/FtpTransport.{h,cpp}   → FtpTransport                                     [novo]
src/core/FtpReply.{h,cpp}       → parseReply / parsePasv / parseMdtm (puros)       [novo]
src/core/FtpProbe.{h,cpp}       → sondagem FTP                                     [novo]
src/core/FtpSegmentWorker.{h,cpp} → transferência FTP por segmento                 [novo]
src/core/SegmentWorker.{h,cpp}  → renomeado HttpSegmentWorker, implementa SegmentSource
src/core/HttpProbe.{h,cpp}      → implementa Probe
src/core/DownloadTask.{h,cpp}   → Transport* no lugar de QNAM*; credenciais + m_awaitingCredentials
                                  em memória; desvio AuthRequired em onProbed/onSegmentFailed;
                                  onRestartRequired com deleteLater + guarda (§9.1)
src/core/DownloadManager.{h,cpp}→ registry de transports (unique_ptr); provideCredentials;
                                  credentialsRequired repassado por wire()
src/core/DownloadTypes.h        → ProbeResult::authRequired
src/core/Persistence.{h,cpp}    → SEM MUDANÇAS (o .meta já carrega o validador; FTP usa
                                  lastModified para o MDTM e deixa etag vazio)
src/gui/ClipboardWatcher.{h,cpp}→ monitor + shouldOffer (puro)                      [novo]
src/gui/DropTargets.{h,cpp}     → extractUrls (puro)                                [novo]
src/gui/CredentialsDialog.{h,cpp} → user/senha                                      [novo]
src/gui/NewDownloadDialog.{h,cpp} → isValidDownloadUrl; rótulo de tipo
src/gui/MainWindow.{h,cpp}      → menu Tools; dropEvent; diálogo de credenciais
tests/TestFtpServer.{h,cpp}     → servidor FTP mínimo com knobs                     [novo]
tests/FakeTransport.{h,cpp}     → transport sintético, sem rede                     [novo]
tests/tst_ftp.cpp               → critérios 4–12                                    [novo]
```

---

## 5. `TestFtpServer`

Sobre `QTcpServer` (~200 linhas), falando só o subset que o cliente usa: `USER`, `PASS`, `TYPE`,
`SIZE`, `MDTM`, `PASV`, `REST`, `RETR`, `QUIT`. Knobs para os casos que carregam o risco:

| Knob | Simula |
|---|---|
| `noSize` | `SIZE` responde `502` → `totalBytes == -1` |
| `noRest` | `REST` responde `502` → fallback single-connection |
| `requireAuth(user, pass)` | `530` para anônimo/credencial errada |
| `dropAfter(n)` | fecha o socket de dados após n bytes → retry |
| `maxConnections(n)` | `421` nas conexões extras |
| `noMdtm` | `MDTM` responde `502` → validador vazio, resume não-validado |
| `setMdtm(v)` / `setContent(bytes)` | muda o `MDTM` (e o conteúdo) entre sessões → critério 11: o worker vê divergência e dispara `restartRequired` |
| `restFailsAt(n)` | `REST` responde `502` a partir da n-ésima conexão → critério do `restartRequired` por `REST` recusado |

Mesma filosofia do `TestServer` (`QHttpServer`) da Fase 1: suíte 100% offline, determinística.

---

## 6. Ordem de implementação sugerida

A refatoração de transporte é pré-requisito de tudo em FTP e mexe no motor estável — vai primeiro, com
a suíte HTTP como rede de segurança. A frente de GUI é independente e pode ser feita em paralelo.

1. `Transport`/`Probe`/`SegmentSource` + `HttpTransport` + `FakeTransport`; `DownloadTask` e
   `DownloadManager` migrados. **Gate: `tst_download` verde sem mudar expectativa.**
2. Mitigação do §9.1 (`deleteLater` + guarda `m_restarting`), com teste de N segmentos restartando na
   mesma volta do event loop. Vai **antes** do FTP: é o FTP que torna esse caminho comum, e é mais
   barato consertá-lo com só a suíte HTTP em cima.
3. `FtpReply` (puros) → `FtpProbe` + `TestFtpServer` → `FtpSegmentWorker` (multi-seg, corte no `end`)
   → fallback → resume/`MDTM` → auth (probe e worker).
4. GUI: `isValidDownloadUrl` → `extractUrls`/drop → `ClipboardWatcher` + menu Tools →
   `CredentialsDialog`.

---

## 7. Verificação manual (não automatizável)

Com o app rodando, contra um servidor FTP público real:

1. Baixar um arquivo FTP: progride, grade de blocos evolui por segmento, arquivo íntegro.
2. Pausar e retomar no meio: retoma dos offsets, arquivo final íntegro.
3. Servidor com auth: diálogo aparece, credenciais corretas completam o download.
4. Os 4 modos de clipboard fazem o que dizem.
5. Arrastar um link do navegador → New pré-preenchido; arrastar vários → todos enfileirados.

---

## 8. Fora de escopo (explícito)

| Item | Onde vive |
|---|---|
| FTPS (`AUTH TLS`), `QSslSocket` | fora de escopo |
| Persistir modo de clipboard / `settings.json` / Preferences | Fase 4 |
| Categoria escolhida e persistida | Fase 4 |
| Scheduler, limite de banda | Fase 4 |
| Importar lista de URLs de `.txt`/`.html` | feature própria, não planejada |
| Integração com browser | Fase 5 |
| Listagem de diretórios FTP (`LIST`/`NLST`), upload | fora de escopo |
| P2SP/P2P, Grab de streaming | fora de escopo permanente |

---

## 9. Riscos conhecidos

### 9.1 `onRestartRequired` destrói o worker que está emitindo (latente, pré-existente)

`DownloadTask::onRestartRequired` faz `qDeleteAll(m_workers)` — o que inclui o worker **dentro da
chamada de `emit restartRequired(...)`**, ou seja, destrói um objeto cujo frame de pilha ainda vai
retornar. Hoje isso não explode por um acidente feliz: `SegmentWorker::onReadyRead` faz `return`
imediatamente após o emit, sem tocar em membro nenhum.

FTP aumenta muito a exposição:

- **N restarts quase simultâneos.** Cada `FtpSegmentWorker` faz sua própria checagem de `MDTM`
  (§3.5), então um arquivo alterado no servidor dispara `restartRequired` em todos os N workers na
  mesma volta do event loop. O primeiro já apagou `m_workers` — os demais emitem de objetos
  destruídos.
- **O emit do FTP não estará na última linha.** A máquina de estados do worker FTP tem sockets de
  controle e de dados para fechar; a disciplina "não toque em nada depois do emit" é muito mais
  difícil de manter do que num `readyRead`.

**Mitigação (parte da tarefa do `FtpSegmentWorker`):** trocar `qDeleteAll(m_workers)` por
`w->stop()` + `w->deleteLater()` e adicionar uma guarda `m_restarting` na `DownloadTask` para que os
`restartRequired` seguintes da mesma rodada sejam no-op. `deleteLater()` adia a destruição para depois
do retorno ao event loop, que é exatamente a garantia que falta hoje.

Isto **toca o Core da Fase 1** e portanto viola em espírito o gate "`tst_download` sem mudança de
expectativa" (§6) — mas não na letra: é mudança de mecanismo de destruição, sem mudança de
comportamento observável, e a suíte HTTP existente é a rede de segurança que prova isso. Merece
tarefa própria, com um teste de N segmentos restartando juntos.

### 9.2 Contagem de conexões

N segmentos = 2N sockets (controle + dados) por download, × `maxConcurrentDownloads`. Com os padrões
(4 × 3) são 24 sockets contra servidores que frequentemente limitam a 2–5 por IP. O `421` é tratado
como recuperável e o backoff resolve (§3.3), mas **o caminho comum contra servidor público real vai
ser cheio de retry**. Se a verificação manual (§7) mostrar isso como problema prático, a resposta é um
cap de segmentos por protocolo no `EngineConfig` — deliberadamente **não** incluído agora para não
adicionar knob antes de haver evidência.
