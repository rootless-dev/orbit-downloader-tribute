# Orbit Downloader Tribute — Fase 1: Core HTTP Engine

**Data:** 2026-07-16
**Projeto:** `orbit-downloader-tribute`
**Escopo desta spec:** Fase 1 — motor de download HTTP segmentado, headless (sem GUI), com testes.

---

## 1. Contexto e visão do produto

Reimplementação em C++/Qt6 do gerenciador de downloads *Orbit Downloader*, replicando suas
funcionalidades e o layout da GUI clássica (toolbar, árvore de categorias, tabela de downloads,
abas Log/Progress/Properties com a grade de blocos de segmentos).

O produto completo está dividido em fases; **esta spec cobre apenas a Fase 1**, que entrega a
fundação: um motor de download que baixa um arquivo HTTP/HTTPS em múltiplos segmentos paralelos,
com pause/resume que sobrevive ao fechamento do app. As fases seguintes (GUI, FTP, scheduler,
clipboard, browser) terão specs próprias.

### Decisões já tomadas (brainstorming)

- **Motor:** HTTP+FTP multi-segmento (FTP fica para fase posterior; Fase 1 é HTTP/HTTPS).
- **Stack de rede:** híbrida — HTTP/HTTPS via `QNetworkAccessManager`; FTP futuro via `QTcpSocket`.
- **Plataforma:** macOS primeiro, código portable (Qt).
- **Build:** CMake + Qt 6.11, **C++20** (`CMAKE_CXX_STANDARD 20`, sem extensões).
- **Princípio:** o Core não depende de QtWidgets — só `QtCore` e `QtNetwork`.

---

## 2. Objetivo e critérios de sucesso da Fase 1

**Objetivo:** dado uma URL HTTP/HTTPS, baixar o arquivo dividindo-o em N segmentos paralelos,
reportando progresso, permitindo pausar/retomar e **retomar após reiniciar o processo**.

**Critérios de sucesso (todos verificáveis por teste automatizado contra servidor local):**

1. Baixa um arquivo conhecido e o conteúdo final bate byte-a-byte com o original.
2. Com servidor que suporta `Range`, usa N segmentos; o arquivo resultante é idêntico ao de 1 segmento.
3. Com servidor que **não** suporta `Range` (ou tamanho desconhecido), faz fallback para 1 conexão e
   ainda completa corretamente.
4. Pausar no meio e retomar produz arquivo idêntico (sem rebaixar bytes já obtidos).
5. Matar o processo no meio, recriar o Manager a partir do disco e retomar produz arquivo idêntico.
6. Um segmento cuja conexão cai no meio é re-tentado sozinho (backoff) enquanto os outros seguem, e o
   arquivo final ainda bate byte-a-byte.
7. Um segmento que falha além de `maxSegmentRetries` leva o task a `Error`; um `start()` posterior
   retoma e completa corretamente.
8. Erro não-recuperável (HTTP 404 / disco sem espaço) vai direto para `Error`, sem retries.
9. Conexão que trava (nenhum byte novo) é abortada por `idleTimeout` e cai no fluxo de retry.
10. Sinais de progresso (`bytesReceived`/`bytesTotal` por task e por segmento) são emitidos, coalescidos
    por `progressThrottle`.

---

## 3. Arquitetura do Core

Todos os tipos vivem em `src/core/` e dependem apenas de `QtCore`/`QtNetwork`.

### 3.1 Modelo de dados

```
DownloadTask
  id            : QUuid
  url           : QUrl
  destPath      : QString            // caminho final do arquivo
  totalBytes    : qint64             // -1 se desconhecido
  supportsRange : bool
  state         : State              // enum abaixo
  segments      : QVector<Segment>
  etag          : QString            // validador p/ If-Range (vazio se ausente)
  lastModified  : QString            // fallback de validador
  validated     : bool               // false = resume otimista sem validador (§3.4)
  error         : QString

Segment
  index   : int
  start   : qint64    // offset inicial (imutável)
  current : qint64    // próximo byte a escrever (avança conforme baixa)
  end     : qint64    // offset final inclusivo; -1 = até EOF (modo fallback)
  // baixado = current - start
  // concluído: se end >= 0, quando current > end; se end == -1 (fallback), no sinal finished/EOF
```

**Estados (`DownloadTask::State`):**
`Queued → Connecting → Downloading → Paused → Completed` e, de qualquer ativo, `→ Error`.
De `Paused`/`Error` pode voltar a `Queued` (retry/resume).

### 3.2 Componentes

- **`HttpProtocol`** — encapsula `QNetworkAccessManager`.
  - `probe(url)` → emite `probed(totalBytes, supportsRange, etag, lastModified, resolvedUrl)`.
    Implementado com um GET `Range: bytes=0-0`: resposta `206` + header `Content-Range` ⇒ suporta
    Range e revela o tamanho total; resposta `200` ⇒ não suporta Range (usa `Content-Length` como
    tamanho, se houver). Também captura `ETag`/`Last-Modified` para o resume. Assim que os headers
    chegam (`metaDataChanged`), **aborta o reply** — não baixamos o corpo no probe.
    (Preferimos GET a HEAD porque muitos servidores não respondem HEAD corretamente.)
  - `startSegment(segment, destFile)` → abre `QNetworkReply` com o header Range apropriado, conecta
    `readyRead`/`finished`/`errorOccurred`, escreve os dados no arquivo no offset `segment.current`.
    **Se `QFile::write` retornar erro** (disco cheio, permissão), aborta o reply e sinaliza erro
    não-recuperável (§3.5).
  - Segue redirects (`QNetworkRequest::followRedirectsAttribute`), respeitando o `resolvedUrl` do probe.

- **`DownloadTask`** (`QObject`) — orquestra os segmentos de UM download.
  - `start()`: se ainda não sondado, chama `probe`; ao receber resultado, calcula a divisão em
    segmentos (ver §3.3) e dispara `startSegment` para cada um.
  - Agrega progresso dos segmentos; emite `progress(received, total)` e `stateChanged(state)`.
  - `pause()`: aborta os replies ativos, persiste metadados, vai para `Paused`.
  - Ao concluir todos os segmentos: fecha o arquivo, vai para `Completed`, remove o sidecar `.meta`.

- **`DownloadManager`** (`QObject`) — dono da coleção de tasks.
  - `addDownload(url, destPath, segmentCount)` → cria e enfileira um `DownloadTask`.
  - Aplica `maxConcurrentDownloads`: mantém uma fila; promove `Queued → Downloading` conforme abre vaga.
  - `pauseAll()` / `resumeAll()`.
  - `remove(id, deleteFiles)`: aborta os segmentos ativos, tira o task da fila/coleção e persiste a
    lista. Se `deleteFiles` for `true`, apaga também o arquivo parcial e o `.meta`; se `false`
    (default), mantém o parcial no disco para eventual reuso.
  - Carrega/salva estado via `Persistence` (na inicialização, restaura tasks incompletos como `Paused`).
  - Reemite sinais agregados para a futura camada de GUI.

- **`Persistence`** — serialização.
  - **Sessão:** `downloads.json` na pasta de dados do app (`QStandardPaths::AppDataLocation`) com a
    lista de tasks (id, url, destPath, totalBytes, supportsRange, state, segmentCount).
  - **Sidecar por download:** `<destPath>.meta` (JSON) com o vetor de segmentos `[start,current,end]`,
    mais `etag`/`lastModified`/`validated`. É a fonte da verdade para resume.
    **Cadência de escrita:** um `QTimer` por task grava o `.meta` a cada **2 s** enquanto há
    progresso, e imediatamente em `pause()`, ao concluir um segmento, e em `Error`. Antes de gravar,
    o `QFile` do download é `flush()`ado para o SO, garantindo que os bytes no disco cubram os offsets
    persistidos (nunca registrar `current` além do que já foi escrito).
  - Escrita atômica (arquivo temporário + `rename`) para não corromper metadados se o processo morrer.
  - **Colisão de destino:** se `destPath` já existe e **não** há `.meta` correspondente, o Manager
    resolve um nome livre (`arquivo (1).ext`, `(2)`, …) antes de iniciar. Se existe `.meta`, trata-se
    de um download interrompido do próprio app e ele é **retomado**, não renomeado.

### 3.3 Mecânica de segmentação

- Se `supportsRange` e `totalBytes > 0`: divide `[0, totalBytes-1]` em `N` faixas contíguas de
  tamanho ~igual (última faixa absorve o resto). `N = min(segmentCount, teto(totalBytes / minSegSize))`
  com `minSegSize` (ex.: 1 MiB) para não criar segmentos minúsculos em arquivos pequenos.
- Senão: **1 segmento** `[0, -1]` (lê até EOF). Sem paralelismo, mas funciona.
- O arquivo é **pré-alocado** com o tamanho total (`QFile::resize`) quando conhecido, para que cada
  segmento escreva no seu offset com `seek` sem colisão.
- **Escrita:** cada segmento faz `file.seek(current); file.write(chunk); current += n`. Um único
  `QFile` compartilhado com `seek` por escrita (todos os replies rodam no mesmo event loop/thread,
  então não há concorrência real de I/O — sem locks necessários na Fase 1).

### 3.4 Resume (a parte crítica)

- No `start` de um task restaurado, `Persistence` lê o `.meta`; cada segmento retoma de `current`
  (header `Range: bytes=current-end`). Segmentos já com `current > end` são pulados.
- Guardamos `ETag`/`Last-Modified` no `.meta` e, ao retomar cada segmento, enviamos `If-Range` com
  esse validador junto do `Range`. Comportamento por resposta:
  - `206 Partial Content` ⇒ recurso inalterado, retoma normalmente.
  - `200 OK` ⇒ servidor ignorou o `If-Range` (recurso mudou ou não suporta) ⇒ **recomeça do zero**:
    zera todos os segmentos, re-pré-aloca e baixa de novo.
- **Sem validador disponível** (servidor não manda `ETag` nem `Last-Modified`): resume é *otimista* —
  retomamos pelos offsets do `.meta` sem garantia de integridade. Registramos `validated:false` no
  `.meta`; o CLI/GUI pode expor isso. Aceitável para a Fase 1 (a alternativa seria sempre recomeçar,
  o que anula o resume no caso comum de servidores simples).
- **Tamanho desconhecido** (sem `Content-Length`, modo 1-segmento append): **não é resumável** — ao
  retomar, o arquivo parcial é descartado e o download recomeça. Documentado como limitação.

### 3.5 Tratamento de erros, retry e timeouts

Distinguimos **falha de segmento** de **falha de task**:

- **Falha de segmento** — reply com erro de rede, conexão derrubada, `connectTimeout` ou `idleTimeout`
  estourado. O segmento (que já persistiu seu `current` no `.meta`) é **re-tentado individualmente**
  com backoff exponencial: espera `retryBackoffBase * 2^(tentativa-1)` e reabre o `Range` a partir do
  `current`. Os **demais segmentos continuam baixando** normalmente.
- **Falha de task** — só ocorre quando um segmento esgota `maxSegmentRetries`. Aí o task inteiro vai
  para `Error` (aborta os segmentos restantes, persiste estado) e expõe `error`. O usuário pode
  chamar `start()` de novo (retry manual), que retoma de onde parou via `.meta`.
- **Erros não-recuperáveis** (HTTP 4xx como 403/404, disco cheio, permissão negada na escrita) **não**
  são re-tentados: vão direto para `Error` com mensagem descritiva.

**Timeouts** são implementados com um `QTimer` por segmento, rearmado a cada `readyRead`
(idle) e um timer separado para a resposta inicial (connect). Ao estourar, aborta o reply — o que
cai no fluxo de falha de segmento acima.

### 3.6 Emissão de progresso (throttling)

`QNetworkReply::readyRead` dispara em altíssima frequência. Cada segmento acumula bytes internamente,
mas o `DownloadTask` só **emite `progress(received, total)` no máximo a cada `progressThrottle`
(200 ms)**, coalescendo atualizações via um `QTimer`. Emite também um sinal final imediato ao
concluir. Isso evita inundar a futura camada de GUI e mantém os testes determinísticos.

---

## 4. Concorrência

Fase 1 roda **tudo no event loop principal** (I/O assíncrono do Qt). Sem threads manuais:
`QNetworkReply` entrega dados via sinais, e a escrita em disco por chunk é barata o suficiente para
o alvo desta fase. Mover escrita para thread dedicada é uma otimização deixada para depois (anotada,
não implementada). Isso elimina toda uma classe de bugs de concorrência na fundação.

### 4.1 Configuração (`EngineConfig`)

Struct simples passada ao `DownloadManager` na construção; carregada de `settings.json`
(`QStandardPaths::AppConfigLocation`) se existir, senão usa os defaults. Não há UI de settings na
Fase 1 — o CLI aceita overrides por flag (`--segments`, `--max-concurrent`).

| Campo | Default | Uso |
|---|---|---|
| `maxConcurrentDownloads` | 3 | Máx. de tasks em `Downloading` ao mesmo tempo |
| `segmentCount` | 4 | Segmentos por download (teto; ver §3.3) |
| `minSegSize` | 1 MiB | Abaixo disso não fatia (evita segmentos minúsculos) |
| `maxSegmentRetries` | 5 | Tentativas por segmento antes de falhar o task (§3.5) |
| `retryBackoffBase` | 1 s | Base do backoff exponencial entre retries |
| `connectTimeout` | 30 s | Sem resposta inicial ⇒ falha do segmento |
| `idleTimeout` | 30 s | Sem nenhum byte novo nesse intervalo ⇒ falha do segmento |
| `progressThrottle` | 200 ms | Intervalo mínimo entre sinais `progress` por task (§3.6) |

---

## 5. Estrutura de arquivos (Fase 1)

```
orbit-downloader-tribute/
  CMakeLists.txt
  src/
    core/
      DownloadTypes.h        // enums, struct Segment
      DownloadTask.{h,cpp}
      HttpProtocol.{h,cpp}
      DownloadManager.{h,cpp}
      Persistence.{h,cpp}
    main.cpp                 // Fase 1: pequeno CLI de fumaça (baixa 1 URL) p/ ver o motor rodando
  tests/
    CMakeLists.txt
    tst_httpengine.cpp       // QtTest + QHttpServer local
  docs/design/specs/
```

Na Fase 1 `main.cpp` é um CLI mínimo (`orbit-cli <url> <destino>`) só para exercitar o motor de
ponta a ponta manualmente; ele é descartado/substituído quando a GUI entrar.

---

## 6. Estratégia de testes

Framework: **QtTest**. Servidor de teste: **`QHttpServer`** (módulo `qthttpserver`) subido dentro do
próprio teste, servindo um buffer de bytes determinístico.

O servidor de teste oferece rotas configuráveis para cobrir cada critério de sucesso:

- `/ranged` — respeita `Range`, responde `206` + `Content-Range` + `Accept-Ranges: bytes` + `ETag`.
- `/plain` — ignora `Range`, sempre `200` com corpo inteiro (testa fallback).
- `/nolength` — responde sem `Content-Length` (tamanho desconhecido; testa não-resumível).
- `/flaky?dropAt=N&failTimes=K` — fecha a conexão após N bytes nas primeiras K vezes, depois serve
  normal (testa retry de segmento + sucesso; e, com K > `maxSegmentRetries`, task → `Error`).
- `/stall` — envia headers e alguns bytes, depois "congela" sem fechar (testa `idleTimeout`).
- `/notfound` — responde `404` (testa erro não-recuperável).
- `/changed` — na retomada, devolve `200` com `ETag` diferente (testa `If-Range` → restart do zero).

Casos de teste mapeados 1:1 aos critérios de sucesso do §2. O teste de "resume após reiniciar
processo" é simulado destruindo o `DownloadManager` e recriando-o a partir do `downloads.json`/`.meta`
no mesmo diretório temporário, sem realmente matar o processo. Timeouts nos testes usam valores
reduzidos via `EngineConfig` para não deixar a suíte lenta.

**Testes não dependem da internet** — tudo contra `127.0.0.1`.

---

## 7. Build

`CMakeLists.txt` com `find_package(Qt6 REQUIRED COMPONENTS Core Network HttpServer Test)`.
Três alvos: a lib estática `orbitcore` (o Core), o executável `orbit-cli`, e o executável de teste
`tst_httpengine` registrado via `add_test`/`qt_add_test`. `CMAKE_PREFIX_PATH` aponta para o Qt do
Homebrew (`/opt/homebrew`).

---

## 8. Fora de escopo (fases futuras)

GUI (QtWidgets), FTP, scheduler, monitor de clipboard, drag & drop, integração com browser,
limite de banda, grade de blocos visual. Nada disso é implementado na Fase 1, mas o Core é desenhado
para não precisar de reescrita quando chegarem (sinais de progresso já pensados para alimentar models).
