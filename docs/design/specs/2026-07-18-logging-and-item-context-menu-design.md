# Spec — Logging detalhado + Menu de contexto do item

> Data: 2026-07-18 · Branch base: `develop` · Fase: extensão da Fase 4 (GUI).
> Objetivo: (1) logs detalhados por download e um log de aplicação, gravados em
> disco; (2) menu de contexto (botão direito) na tabela de downloads com
> Start/Stop/Cancel/Delete/Move/Open/Open-folder/Priority — incluindo recursos
> novos no core (estado `Cancelled`, `Priority`, `moveFiles`); (3) recolorir a
> grade de progresso no padrão do Orbit clássico (fundo claro; cinza/azul/laranja
> por estado, não mais arco-íris por segmento). Extras deste ciclo: coluna
> Priority na tabela, double-click (abrir/iniciar), notificação de conclusão e
> "Clear completed".

---

## 1. Contexto

Hoje o log é um único `QPlainTextEdit` global (`MainWindow::m_log`) que recebe
uma linha por mudança de estado (`MainWindow::onStateChanged`). A tabela de
downloads (`QTableView` + `DownloadTableModel` via `CategoryFilterProxy`) não tem
menu de contexto. O core (`orbitcore`, só QtCore+Network, testável headless)
expõe `pause/resume/remove(deleteFiles)/pauseAll/resumeAll`, mas **não** tem
conceito de cancelamento distinto, de prioridade, nem de mover arquivos.

Dados do app ficam em `AppDataLocation/orbit-gui/` (onde vive `downloads.json`).
Logs irão para `AppDataLocation/orbit-gui/logs/`.

---

## 2. Feature A — Logging

### 2.1 `Logger` (novo, no core)

Classe `Logger : public QObject` em `src/core/Logger.{h,cpp}` (só QtCore). Vive
no core porque é lá que estão os eventos de nível de segmento.

Responsabilidades:

- Escrever em `<dataDir>/logs/`:
  - `app.log` — eventos globais da aplicação.
  - **Um arquivo por download**, nomeado `<basename>-<id-curto>.log`, onde
    `basename` é o nome do arquivo de destino sanitizado (sem extensão de path
    perigosa) e `id-curto` são os primeiros 8 hex do `QUuid` (garante unicidade
    e legibilidade). O caminho do log é derivável de forma determinística a
    partir de `(destPath, id)`.
- Formato de linha (uma linha por evento):
  `YYYY-MM-DD HH:mm:ss.zzz [LEVEL] mensagem`
  Níveis: `DEBUG`, `INFO`, `WARN`, `ERROR`.
- API:
  - `void logApp(Level, const QString& msg)` → grava em `app.log` e emite sinal.
  - `void logTask(const QUuid& id, const QString& destPath, Level, const QString& msg)`
    → grava no arquivo do download e emite sinal.
  - `signals: void lineLogged(const QUuid& id, const QString& formattedLine)` —
    `id` nulo para linhas de `app.log`. Permite à GUI mostrar ao vivo sem reler.
  - `QString taskLogPath(const QUuid& id, const QString& destPath) const` —
    caminho do arquivo de log de um download (usado pela GUI para carregar o
    conteúdo já existente ao selecionar o item).
  - `QString logsDir() const`.
- **Rotação por tamanho** somente no `app.log`: ao ultrapassar 5 MB, renomeia
  para `app.log.1` (substituindo o `.1` anterior) e recomeça `app.log`. Arquivos
  por-download não rotacionam (tamanho limitado pela vida do download).
- I/O: append incremental (mantém `QFile` aberto por handle em cache pequeno, ou
  abre/append/fecha por linha — decisão de implementação; correção acima de
  performance nesta fase). Falha de escrita nunca aborta o download: loga o erro
  em `app.log` (best-effort) e segue.

### 2.2 Instrumentação do core

O `Logger` é injetado no `DownloadManager` (construtor) e propagado às
`DownloadTask`/`SegmentWorker`. Eventos mínimos a registrar (nível de segmento):

- Probe HTTP/FTP: URL, método, status code, `supportsRange`, tamanho total.
- Segmentação: número de segmentos, ranges de cada um.
- Por segmento: início, retry (com motivo/causa: timeout, status, reset), bytes
  concluídos, erro non-recoverable (403/404/auth).
- Credenciais solicitadas (host).
- Throttle de banda ativo (limite aplicado).
- Resume: sessão carregada, offset por segmento; fallback single-connection.
- Toda transição de estado (`stateChanged`) → `INFO` (ou `ERROR` no estado Error,
  com a mensagem de erro).

Nível padrão gravado: tudo (DEBUG incluso). Não há filtro de verbosidade
configurável neste ciclo (decisão explícita — pode virar Preferência depois).

### 2.3 Log na GUI

- A aba inferior **"Log"** deixa de ser global e passa a mostrar **o log do
  download selecionado**:
  - Ao mudar a seleção (`onSelectionChanged`): limpa o `QPlainTextEdit`, carrega
    o conteúdo atual de `logger->taskLogPath(id, destPath)` (se existir) e passa
    a anexar as novas linhas recebidas via `Logger::lineLogged` **filtradas por
    aquele `id`**.
  - Sem seleção: mostra `—`.
- **Tools → Application Log**: abre a pasta `logs/` no gerenciador de arquivos
  (`QDesktopServices::openUrl(QUrl::fromLocalFile(logger->logsDir()))`). Sem
  janela dedicada.
- Eventos de nível de app disparados pela GUI (startup, aplicação de settings,
  scheduler acionado) chamam `logger->logApp(...)` → vão para `app.log`.
- O `MainWindow::onStateChanged` atual (que hoje escreve no log global) é
  substituído por este mecanismo; nenhuma linha "global" some — vira `app.log`.

---

## 3. Feature B — Menu de contexto + mudanças no core

### 3.1 Novo estado `Cancelled` (core)

- Adicionar `DownloadState::Cancelled` ao enum (`DownloadTypes.h`).
- `DownloadManager::cancel(const QUuid& id)`:
  - Aborta transferências ativas do item.
  - Apaga o arquivo parcial (o próprio `destPath`, pré-alocado ao tamanho total
    em `beginSegments` — não existe arquivo `.part` separado) e o `.meta`.
  - Marca o registro como `Cancelled` (o item **permanece** na lista/tabela).
  - Persiste a sessão.
- Semântica de retomada: `resume`/`Start` de um item `Cancelled` **recomeça do
  zero** (novo probe + nova segmentação), pois não há parcial nem `.meta`.
- Persistência: `downloads.json` passa a serializar o estado `Cancelled` (itens
  cancelados sobrevivem ao restart, mostrados como Cancelled).

### 3.2 Prioridade (core)

- Enum `Priority { High, Normal, Low }` (padrão `Normal`) — em `DownloadTypes.h`.
- Campo `priority` no registro do download (`DownloadRecord`/`DownloadTask`),
  **persistido** em `downloads.json` (default `Normal` para registros antigos
  sem o campo — retrocompatível).
- `DownloadManager::setPriority(const QUuid& id, Priority p)` — atualiza e
  persiste; não altera downloads já ativos, só a ordem de promoção da fila.
- `pump()` passa a promover os `Queued` **em ordem de prioridade** (High antes de
  Normal antes de Low); empate mantém a ordem de inserção (estável).

### 3.3 Mover arquivos (core)

- `DownloadManager::moveFiles(const QUuid& id, const QString& newDir)`:
  - **Pré-condição:** só permitido quando o item **não está ativo**
    (Paused/Completed/Cancelled/Error/Queued) — a GUI desabilita a ação quando
    ativo; o core valida e é no-op/erro se chamado em item ativo.
  - Move o arquivo de destino (`destPath` — final ou parcial pré-alocado) e o
    `.meta` para `newDir`, preservando o nome; atualiza `destPath` no registro;
    persiste.
  - Colisão de nome no destino: resolve com `resolveUniquePath` (mesma política
    já usada em `addDownload`).

### 3.4 Menu de contexto na GUI

- `m_table->setContextMenuPolicy(Qt::CustomContextMenu)` + slot
  `onTableContextMenu(const QPoint&)` que monta um `QMenu` para o item sob o
  cursor (usa a seleção atual; tabela permanece **seleção única**).
- Ações e regra de habilitação por estado do item:

  | Ação        | Habilitada quando o estado é…                                  |
  |-------------|----------------------------------------------------------------|
  | **Start**   | Queued, Paused, Cancelled, Error (não: Downloading, Completed)  |
  | **Stop**    | Connecting, Downloading (não os demais)                        |
  | **Cancel**  | Queued, Connecting, Downloading, Paused, Error (não: Completed, Cancelled) |
  | **Delete…** | sempre                                                          |
  | **Move…**   | não-ativo: Paused, Completed, Cancelled, Error, Queued          |
  | **Open**    | Completed (o arquivo final existe no disco)                     |
  | **Open containing folder** | sempre que a pasta de `destPath` existe (pós-add) |
  | **Priority ▸ High/Normal/Low** | sempre (o nível atual aparece marcado)      |

- **Delete…**: diálogo (`QMessageBox` com checkbox, ou pequeno `QDialog`) com a
  opção **"Também apagar os arquivos do disco"**. Confirma → `remove(id,
  deleteFiles)`; o arquivo de log do item também é removido quando o item sai da
  lista.
- **Move…**: `QFileDialog::getExistingDirectory` → `moveFiles(id, dir)`.
- **Open**: abre o arquivo baixado com o app padrão do SO
  (`QDesktopServices::openUrl(QUrl::fromLocalFile(destPath))`). Habilitado só em
  Completed (evita abrir parcial corrompido).
- **Open containing folder**: revela o arquivo no gerenciador de arquivos. No
  macOS, seleciona o item no Finder (`open -R <path>` via `QProcess`, ou
  `QDesktopServices::openUrl` da pasta como fallback portável).
- **Priority**: submenu com três ações marcáveis (exclusivas) → `setPriority`.
- As mesmas operações Start/Stop/Delete continuam disponíveis pela toolbar; o
  menu de contexto reusa os slots onde fizer sentido.

### 3.5 Coluna de prioridade + double-click + Clear completed

- **Coluna Priority na tabela:** novo valor no enum `Column` de
  `DownloadTableModel` (`Priority`), header `"Priority"`, exibindo
  `High`/`Normal`/`Low` do registro. Torna visível a prioridade definida pelo
  menu. Ordenação por essa coluna não é requisito deste ciclo.
- **Double-click no item** (`QTableView::doubleClicked`):
  - Estado `Completed` → **Open** (abre o arquivo, mesma ação do menu).
  - Estado não-ativo (Paused/Queued/Cancelled/Error) → **Start**.
  - Estado ativo (Connecting/Downloading) → no-op (ou foca a aba Progress).
- **Clear completed:** ação no menu Edit (e opcionalmente toolbar) que remove da
  **lista** todos os itens em estado `Completed` (via `remove(id,
  deleteFiles=false)` para cada um) — não apaga arquivos do disco. Loga a
  operação no `app.log`.

### 3.6 Notificação de conclusão

- Ao um download transitar para `Completed`, exibir uma **notificação do
  sistema** "Download concluído: `<nome>`" via `QSystemTrayIcon::showMessage`
  (reusa o ícone de bandeja/menu bar já existente da Fase 4; se ausente, cria um
  `QSystemTrayIcon` mínimo para as notificações).
- A notificação é **clicável**: conectar `QSystemTrayIcon::messageClicked` para
  abrir o arquivo do último download concluído (`Open`). Como o sinal não
  identifica qual mensagem foi clicada, guardar o `id`/`destPath` do último
  concluído e abrir esse.
- Sem dependência de plataforma além do que o Qt já provê; em sistemas sem
  bandeja disponível, a notificação é simplesmente omitida (best-effort).

---

## 4. Feature C — Grade de progresso no padrão Orbit

Hoje `ProgressGridWidget`/`GridGeometry` pintam cada segmento com uma cor de uma
paleta de 8 cores (`segColor`) sobre fundo escuro `#1e1e1e`. Trocar para o
esquema do Orbit clássico: **fundo claro** e **três estados de tile** por cor
(não por segmento).

### 4.1 Estados e cores

- **Pending** (não baixado): cinza claro com leve relevo. Fill `#dcdcdc`, borda
  `#c0c0c0`.
- **Downloaded** (baixado): azul/ciano do Orbit. Fill `~#5b9bd5` (borda um tom
  mais escura).
- **Active** (cabeça de escrita de um segmento em andamento): laranja `~#f7941e`.
  É **um tile por segmento ativo** — a célula que contém a posição `current` do
  segmento enquanto o download roda e a célula ainda não está completa.
- **Error**: mantém vermelho (`#ef4444`), agora com fundo claro.
- Fundo do widget: claro (`#ffffff` ou `#f0f0f0`), no lugar de `#1e1e1e`.

Valores de cor são aproximações do Orbit e podem ser afinados na implementação;
o importante é **não** haver mais o arco-íris por segmento — todo tile baixado
tem a mesma cor.

### 4.2 Mudança em `computeCells` (`GridGeometry`)

- Adicionar `CellKind::Active` ao enum.
- Marcar como `Active` a célula cujo intervalo `[cellStart, cellEnd)` contém a
  posição `current` de um segmento que ainda está baixando (owner não completo)
  **e** o estado do download é ativo (Connecting/Downloading). Fora disso, a
  regra atual permanece (Downloaded quando `current >= cellEnd`, senão Pending /
  Error).
- `segmentIndex` deixa de determinar cor (fica só informativo); `segColor` é
  removido de `ProgressGridWidget`.

---

## 5. Persistência / retrocompatibilidade

`downloads.json` ganha dois campos por download: `state` passa a poder ser
`Cancelled`; novo campo `priority` (`"High"|"Normal"|"Low"`, ausente ⇒ Normal).
Ler um `downloads.json` antigo (sem `priority`) não pode falhar.

---

## 6. Testes (TDD)

### Core (QtTest headless)
- **Cancel:** iniciar → cancelar → estado vira `Cancelled`, `destPath`/`.meta`
  apagados; Start subsequente recomeça do zero; `Cancelled` sobrevive a
  save/load da sessão.
- **Priority:** com `maxConcurrent` reduzido, enfileirar itens com prioridades
  diferentes → `pump()` promove na ordem High→Normal→Low; empate estável;
  round-trip de `priority` em `downloads.json`; default Normal ao ler JSON legado.
- **Move:** `moveFiles` move `destPath`/`.meta`, atualiza `destPath`, resolve
  colisão; recusado/no-op em item ativo.
- **Logger:** `logApp`/`logTask` gravam linha no arquivo certo com nível e
  timestamp no formato; `lineLogged` emitido com `id` correto; `taskLogPath`
  determinístico; rotação do `app.log` ao exceder o limite.

### GUI (`tst_gui`)
- Menu de contexto: cada ação habilita/desabilita conforme o estado (tabela por
  estado acima).
- Delete: diálogo com o checkbox; caminho deleteFiles=true vs false.
- Log tab: trocar a seleção troca o conteúdo exibido; nova linha via
  `lineLogged` do item selecionado aparece; linha de outro item não aparece.
- Grade (`computeCells`): a célula que contém o `current` de um segmento ativo
  vira `Active`; células já baixadas viram `Downloaded` (sem depender do índice
  do segmento); sem segmento ativo não há `Active`.
- Coluna Priority: `data()` retorna o texto correto por linha; muda ao chamar
  `setPriority`.
- Double-click: em item Completed dispara Open; em item parado dispara Start; em
  item ativo é no-op.
- Clear completed: remove só os itens `Completed` da lista, preserva os demais e
  não apaga arquivos.

---

## 7. Fora de escopo (deste ciclo)

- Filtro de verbosidade de log configurável (Preferências).
- Seleção múltipla na tabela / ações em lote.
- Mover com o download ativo (pausa-move-retoma automático).
- Mover para categoria lógica (árvore lateral) — "Mover" aqui é só de pasta no
  disco.
- Reordenar itens manualmente na lista (a ordem da fila é decidida por
  prioridade, não por drag manual).
