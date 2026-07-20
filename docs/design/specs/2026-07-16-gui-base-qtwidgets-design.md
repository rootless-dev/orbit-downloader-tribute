# Orbit Downloader Tribute — Fase 2: GUI base (QtWidgets)

**Data:** 2026-07-16
**Projeto:** `orbit-downloader-tribute`
**Escopo desta spec:** Fase 2 — GUI QtWidgets com o layout do Orbit (toolbar, árvore de categorias,
tabela de downloads, abas Log/Progress/Properties), a grade de blocos de segmentos e o diálogo New.
Liga a GUI aos sinais já expostos pelo Core da Fase 1. **Sem** FTP, clipboard, drag & drop, scheduler
ou Preferences (fases 3/4).

---

## 1. Contexto e visão do produto

Reimplementação em C++/Qt6 do *Orbit Downloader*, replicando o layout clássico da GUI. A Fase 1
entregou o Core headless (`orbitcore`): motor HTTP/HTTPS multi-segmento com pause/resume que sobrevive
ao fechamento do processo, exposto por `DownloadManager`/`DownloadTask` com sinais de progresso.

**Esta spec cobre a Fase 2:** dar rosto ao Core. Uma `MainWindow` QtWidgets com o layout do Orbit,
uma tabela de downloads dirigida por um `QAbstractTableModel`, a árvore de categorias à esquerda como
filtro, as abas inferiores Log/Progress/Properties, e o recurso-assinatura — a **grade densa de blocos
coloridos** de segmentos na aba Progress. Inclui o diálogo **New** (colar URL + escolher pasta).

### Decisões já tomadas (brainstorming desta fase)

- **Alvo separado `orbit-gui`** que linka em `orbitcore`. O Core **continua sem depender de QtWidgets**
  (só `QtCore`/`QtNetwork`) — a GUI é a única consumidora de `QtWidgets`.
- **Controle por item:** estender o `DownloadManager` com `pause(id)`/`resume(id)`/`taskById(id)`
  roteados pelo `pump()` e pelo cap de concorrência. A GUI **nunca** chama `DownloadTask::pause()`
  direto (evita furar o cap — o mesmo bug CRITICAL corrigido na Fase 1).
- **Árvore lateral:** filtros funcionais por estado (`Downloading`/`Completed`) + tipo derivado por
  extensão do arquivo (`Movie`/`Software`/`Music`/`Others`). Sem persistência de categoria (Fase 4).
- **Diálogo New:** URL (pré-preenche do clipboard se for `http[s]`) + pasta destino; nome derivado
  da URL. Sem escolha de categoria (Fase 3/4).
- **Grade de progresso:** grade densa 2D (muitos blocos pequenos em várias linhas), cada bloco = faixa
  de bytes; cor por segmento dono / pendente / erro. É a mais fiel ao Orbit.
- **Speed/ETA:** o Core não tem timestamps. A GUI amostra os bytes recebidos a 1 Hz e calcula
  velocidade/ETA por linha.
- **Build:** CMake + Qt 6.11, C++20. Testes headless via `QT_QPA_PLATFORM=offscreen`.

---

## 2. Objetivo e critérios de sucesso da Fase 2

**Objetivo:** operar o Core inteiramente pela GUI — adicionar um download por URL, vê-lo progredir na
tabela com velocidade/ETA, pausá-lo e retomá-lo pela toolbar, filtrá-lo pela árvore, e ver a grade de
blocos por segmento evoluir na aba Progress.

**Critérios de sucesso.** Os itens de **lógica** (1–7) são verificáveis por teste automatizado
(QtTest, `offscreen`); os itens de **integração visual** (8–10) são verificados manualmente com o app
rodando (documentado, não automatizado — ver §7).

1. **`GridGeometry` (função pura):** dado `totalBytes`, o vetor de `Segment` e uma contagem de células,
   produz a lista `célula → estado/cor` correta. Casos: 1 segmento; N segmentos; arquivo completo;
   nada baixado; segmento em erro; `totalBytes` desconhecido (-1).
2. **`SpeedSampler`:** a partir de amostras `(bytesRecebidos, instante)`, calcula velocidade (bytes/s)
   e ETA; velocidade 0 e ETA indefinido quando não há progresso; ETA cai a zero ao completar.
3. **`DownloadTableModel`:** `rowCount()`/`columnCount()`/`data()` refletem as tasks; um
   `taskProgress` emite `dataChanged` na linha certa (colunas Progress/Speed/Time Left); um
   `taskStateChanged` atualiza a coluna Status; `addDownload` insere linha (`rowsInserted`); `remove`
   retira linha (`rowsRemoved`).
4. **Filtro de categoria:** o predicado do proxy classifica corretamente por estado
   (Downloading/Completed) e por extensão (Movie/Software/Music/Others), e o nó `All Downloads` não
   filtra nada.
5. **Derivação de nome:** dada uma URL (com querystring, com/sem nome de arquivo, percent-encoded), o
   diálogo New deriva o nome de arquivo correto e monta o `destPath` na pasta escolhida.
6. **Roteamento de controle no manager:** `pause(id)` leva a task ativa a `Paused`; `resume(id)`
   re-enfileira e passa pelo `pump()` respeitando `maxConcurrentDownloads` (uma task retomada não
   ultrapassa o cap); `pause(id)`/`resume(id)` com id inexistente são no-ops seguros.
7. **Smoke de construção:** `MainWindow` é construída e destruída sem crash sob `offscreen`, com o
   model, a árvore, as abas e a grade presentes.
8. *(manual)* Adicionar uma URL real pela toolbar/diálogo New baixa o arquivo; a tabela mostra
   progresso, velocidade e ETA ao vivo; ao completar, a linha vai para `Completed` e some do filtro
   `Downloading`.
9. *(manual)* Pausar e retomar pela toolbar funciona sobre a linha selecionada; o arquivo final bate.
10. *(manual)* A aba Progress desenha a grade densa e ela evolui conforme os segmentos avançam;
    Properties mostra url/destino/tamanho/segmentos; Log acumula os eventos de ciclo de vida.

---

## 3. Arquitetura da GUI

Todo código novo vive em `src/gui/` e é o único a depender de `QtWidgets`. A GUI **consome** o Core;
a única mudança no Core é a extensão do `DownloadManager` (§3.2).

### 3.1 Componentes

```
main_gui.cpp          // QApplication; cria EngineConfig, DownloadManager, MainWindow; loadSession()
MainWindow            // QMainWindow: toolbar, splitter (árvore | tabela), abas inferiores; fiação
DownloadTableModel    // QAbstractTableModel: linhas = tasks; colunas Name/Size/Progress/Status/Speed/Time Left
CategoryFilterProxy   // QSortFilterProxyModel: filtra a tabela pelo nó ativo da árvore
CategoryTree          // QTreeWidget (lateral): emite o filtro selecionado
ProgressGridWidget    // QWidget: pinta a grade densa a partir de segments() da task selecionada
GridGeometry          // funções PURAS (sem QtWidgets de UI): mapeia (bytes, segments, nCélulas) → células/cores
SpeedSampler          // amostra bytes/tempo → velocidade + ETA (por task)
FileType              // funções PURAS: extensão → categoria (Movie/Software/Music/Others)
NewDownloadDialog     // QDialog: URL + pasta destino → (QUrl, destPath)
```

**Princípio de testabilidade:** toda lógica não-visual é extraída para unidades puras
(`GridGeometry`, `SpeedSampler`, `FileType`, e a derivação de nome do diálogo). `paintEvent` e o
layout de widgets ficam finos, apenas desenhando/montando o resultado dessas unidades. Isso mantém o
TDD viável sem depender de renderização real.

### 3.2 Extensão do Core (`DownloadManager`)

Única alteração no Core nesta fase. Novos métodos públicos:

```cpp
DownloadTask* taskById(const QUuid& id) const;   // busca em m_tasks; nullptr se ausente
void          pause(const QUuid& id);            // se a task existe e está ativa/enfileirada: task->pause()
void          resume(const QUuid& id);           // task->requeue() + pump()  → respeita o cap de concorrência
```

- `pause(id)`: localiza a task; se estiver `Downloading`/`Connecting`/`Queued`, chama `task->pause()`.
  No-op para id inexistente ou já `Completed`/`Paused`.
- `resume(id)`: localiza a task; se `Paused`/`Error`, chama `task->requeue()` (volta a `Queued`) e
  então `pump()` — que promove `Queued → Downloading` só até `maxConcurrentDownloads`. **Nunca** chama
  `task->start()` direto, para não estourar o cap.
- `taskById(id)`: usado pela GUI para ler `record()`/`segments()`/assinar sinais por task.

`remove(id, deleteFiles)`, `pauseAll()`, `resumeAll()`, `addDownload()`, `loadSession()`, `tasks()` e
os sinais `taskProgress`/`taskStateChanged` já existem e são reusados como estão.

### 3.3 `DownloadTableModel` (QAbstractTableModel)

- **Linhas:** uma por `DownloadTask*` (via `mgr->tasks()`), na ordem de inserção.
- **Colunas:** `Name` | `Size` | `Progress` | `Status` | `Speed` | `Time Left`.
  - `Name` — `QFileInfo(record().destPath).fileName()`.
  - `Size` — `record().totalBytes` formatado (`-1` → "—" / "Unknown").
  - `Progress` — `received/total` como % (papel `Qt::DisplayRole` texto "42%"; papel custom para a
    barra, ver abaixo).
  - `Status` — `record().state` mapeado para texto ("Downloading", "Paused", "Completed", "Error", …).
  - `Speed` — do `SpeedSampler` da linha ("1.2 MB/s"); vazio se não estiver baixando.
  - `Time Left` — ETA do `SpeedSampler` ("00:37"); vazio/"—" se indefinido.
- **Barra de progresso na célula:** a coluna Progress expõe o valor 0–100 num papel custom
  (`ProgressRole`) e um `QStyledItemDelegate` desenha a barra. (Delegate é código de UI fino; a
  aritmética de % é pura.)
- **Fonte de atualização:**
  - `mgr::taskProgress(id, received, total)` → acha a linha por id, atualiza cache de bytes, emite
    `dataChanged` nas colunas Progress (e o `SpeedSampler` é alimentado — ver §3.6).
  - `mgr::taskStateChanged(id, state)` → atualiza Status; ao chegar a `Completed`/`Error`, zera o
    sampler daquela linha.
  - Inserção/remoção: como o Core não sinaliza add/remove, o `MainWindow` chama métodos explícitos do
    model (`appendTask(DownloadTask*)` após `addDownload`, `removeTaskById(id)` após `remove`) que
    disparam `beginInsertRows`/`beginRemoveRows`. `addDownload` retorna o `QUuid`, de onde a GUI
    obtém o `DownloadTask*` via `taskById`.
- **Mapa id→linha:** mantido incrementalmente para achar a linha em O(1) nos sinais.
- **Papéis custom para o proxy:** o model expõe, na coluna 0, papéis de dados brutos que o
  `CategoryFilterProxy` consome sem parsear texto: `StateRole` (o `DownloadState`) e `CategoryRole`
  (o `FileType::Category` derivado do nome). Assim o proxy filtra por valor, não por string exibida.
- **Ordem de inicialização (`main_gui`):** `DownloadManager` é criado, `loadSession()` é chamado
  **antes** de construir o model, para que o ctor do model já leia todas as tasks restauradas de
  `mgr->tasks()`. (Se a ordem se inverter, as linhas restauradas não apareceriam.)

### 3.4 Árvore de categorias e filtro

- `CategoryTree` (`QTreeWidget`) com a hierarquia:
  ```
  All Downloads
    Downloading      (state ∈ {Queued, Connecting, Downloading, Paused})
    Completed        (state == Completed)
    Movie            (extensão de vídeo)
    Software         (extensão de instalador/binário)
    Music            (extensão de áudio)
    Others           (qualquer outra)
  ```
  `Error` aparece sob `Downloading` (não-completo) — é um download ativo que falhou, ainda visível ali.
- Ao selecionar um nó, emite um `enum CategoryFilter`. `CategoryFilterProxy::filterAcceptsRow`
  consulta o `DownloadTask*` da linha (via o model-fonte) e aplica o predicado.
- `FileType::categorize(fileName)` (puro) devolve `Movie/Software/Music/Others` por extensão. Tabelas
  de extensão iniciais (case-insensitive):
  - **Movie:** `mp4 mkv avi mov wmv flv webm m4v mpg mpeg`
  - **Software:** `dmg pkg exe msi deb rpm appimage app zip 7z rar tar gz iso`
  - **Music:** `mp3 flac aac wav ogg m4a wma opus`
  - **Others:** o resto.
  *(Nota consciente: `zip/rar/…` caem em Software por serem majoritariamente distribuição de
  software; é uma heurística, não classificação semântica. Ajustável.)*

### 3.5 `ProgressGridWidget` e `GridGeometry`

- **`GridGeometry` (puro):** entrada = `totalBytes`, `QVector<Segment>`, e `nCells` (nº de células que
  cabem na área atual). Saída = `QVector<CellState>` de tamanho `nCells`, onde cada célula mapeia a
  faixa `[i*totalBytes/nCells, (i+1)*totalBytes/nCells)` e recebe um estado:
  - **Posse da célula:** cada célula pertence ao segmento que contém o seu **byte inicial**
    (`i*totalBytes/nCells`). Como `nCells` costuma exceder o nº de segmentos, uma célula pode cair
    exatamente sobre a fronteira de dois segmentos; a regra do byte inicial a atribui a exatamente um,
    sem ambiguidade.
  - `Downloaded(segmentIndex)` — o `current` do segmento dono cobre o **fim** da faixa da célula
    (`current >= (i+1)*totalBytes/nCells`), i.e. a faixa da célula está inteiramente baixada.
  - `Pending` — faixa ainda não baixada.
  - `Error` — a task está em `Error` e a faixa pertence a um segmento incompleto.
  - Se `totalBytes <= 0` (desconhecido): grade "indeterminada" — todas `Pending` (o widget pode mostrar
    um padrão neutro). Documentado.
- **Cor por segmento:** paleta cíclica indexada por `segmentIndex` (ex.: 4–8 tons distintos), para
  reproduzir os blocos coloridos por segmento do Orbit. `Pending` = cinza; `Error` = vermelho.
- **`ProgressGridWidget`:** guarda o `DownloadTask*` selecionado; em `paintEvent` calcula `nCells` a
  partir do tamanho atual (célula ~8–10 px, várias linhas), chama `GridGeometry` e pinta cada célula.
  Assina `segmentProgress(index, offset)` da task e chama `update()` com **throttle** (um `QTimer`
  ~100 ms) para não repintar a cada chunk. Ao trocar de task selecionada, desconecta a anterior.
- **Vinculação:** o `MainWindow` conecta a seleção da tabela → `ProgressGridWidget::setTask(task)` e
  também alimenta Properties e realça a linha.

### 3.6 `SpeedSampler` e o relógio de 1 Hz

- O Core emite bytes mas não velocidade. O `DownloadTableModel` roda um `QTimer` de **1 Hz**; a cada
  tick, para cada linha ativa, lê os bytes acumulados (do cache alimentado por `taskProgress`) e passa
  ao `SpeedSampler` daquela linha junto do instante atual.
- **`SpeedSampler` (puro/testável):** recebe amostras `(bytes, tMs)` — o tempo é injetado (parâmetro),
  não lido de relógio interno, para o teste ser determinístico. Mantém uma média móvel curta (janela
  ~3–5 s) e expõe `bytesPerSec()` e `etaSeconds(totalBytes)`; ETA indefinido se velocidade ~0 ou total
  desconhecido. Em produção o model injeta `QElapsedTimer::elapsed()`.
- No tick, o model emite `dataChanged` nas colunas Speed/Time Left das linhas ativas.

### 3.7 Abas inferiores

Um `QTabWidget` na parte de baixo, seguindo o Orbit:

- **Log** — `QPlainTextEdit` somente-leitura, **global**: o `MainWindow` escuta `taskStateChanged`
  (e o add/remove que ele mesmo dispara) e anexa linhas com timestamp
  ("`[hh:mm:ss] arquivo.zip → Downloading`"). O Core não emite texto de log; o Log é sintetizado dos
  eventos de ciclo de vida na GUI.
- **Progress** — o `ProgressGridWidget` da task selecionada (§3.5).
- **Properties** — `QFormLayout`/read-only mostrando `record()` da task selecionada: URL, destino,
  tamanho, nº de segmentos, suporte a Range, estado.

Sem seleção, as abas Progress/Properties ficam vazias/desabilitadas.

### 3.8 Toolbar e diálogo New

- **Toolbar** (`QToolBar`) com ações no estilo Orbit: **New**, **Start**, **Pause**, **Delete**,
  **Pause All**, **Resume All**. *(Scheduler e Preferences aparecem desabilitados/ocultos — Fase 4.)*
  - **New** → abre `NewDownloadDialog`; no accept, `mgr->addDownload(url, dest)` + `model->appendTask`.
  - **Start** → `mgr->resume(selId)`; **Pause** → `mgr->pause(selId)`;
    **Delete** → confirma e `mgr->remove(selId, deleteFiles)` + `model->removeTaskById`.
  - **Pause All**/**Resume All** → `mgr->pauseAll()`/`resumeAll()`.
  - Ações que dependem de seleção ficam desabilitadas quando não há linha selecionada.
- **`NewDownloadDialog`:** campo URL (pré-preenchido com o clipboard se `http[s]`), campo pasta
  (default `QStandardPaths::DownloadLocation`, botão "…" abre `QFileDialog::getExistingDirectory`),
  e um rótulo somente-leitura com o **nome derivado**. `deriveFileName(QUrl)` (puro) e
  `destPath = dir + "/" + name`. Valida: URL não-vazia e com esquema http/https.

---

## 4. Fluxo de dados (resumo)

```
NewDownloadDialog ──(url,dest)──▶ DownloadManager::addDownload ──▶ QUuid
                                          │
MainWindow: taskById(id) ─▶ model->appendTask(task)  (beginInsertRows)
                                          │
Core sinais:  taskProgress(id,r,t) ─▶ model: cache bytes + dataChanged(Progress)
              taskStateChanged(id,s)─▶ model: Status;  MainWindow: Log
              task::segmentProgress ─▶ ProgressGridWidget::update() (throttle)
model QTimer 1Hz ─▶ SpeedSampler ─▶ dataChanged(Speed, Time Left)
CategoryTree seleção ─▶ CategoryFilterProxy::invalidateFilter
tabela seleção ─▶ ProgressGridWidget::setTask + Properties + estado das ações
```

---

## 5. Estrutura de arquivos (Fase 2)

```
orbit-downloader-tribute/
  src/
    core/
      DownloadManager.{h,cpp}     // + pause(id)/resume(id)/taskById(id)
    gui/
      CMakeLists.txt
      main_gui.cpp
      MainWindow.{h,cpp}
      DownloadTableModel.{h,cpp}
      CategoryFilterProxy.{h,cpp}
      CategoryTree.{h,cpp}
      ProgressGridWidget.{h,cpp}
      GridGeometry.{h,cpp}        // puro
      SpeedSampler.{h,cpp}        // puro
      FileType.{h,cpp}            // puro
      NewDownloadDialog.{h,cpp}
  tests/
    CMakeLists.txt                // + alvo tst_gui
    tst_gui.cpp                   // QtTest headless (offscreen)
```

`orbit-cli` (Fase 1) permanece. `orbit-gui` é um novo executável.

---

## 6. Build

- `CMakeLists.txt` raiz: adicionar `Widgets` aos componentes Qt
  (`find_package(Qt6 REQUIRED COMPONENTS Core Network Widgets HttpServer Test)`).
- `src/gui/CMakeLists.txt`: define o executável **`orbit-gui`** com `qt_standard_project_setup()`
  (`AUTOMOC` ligado para os `Q_OBJECT`), linkando `orbitcore` + `Qt6::Widgets`.
- Núcleos puros (`GridGeometry`, `SpeedSampler`, `FileType`) compilados numa pequena lib
  **`orbitgui_logic`** (sem `QtWidgets`, só `QtCore` para `QVector`/`QString`/`QUrl`), para o teste
  linkar sem arrastar a UI.
- O alvo de teste `tst_gui` linka `orbitgui_logic` + `orbitcore` + `Qt6::Test` (+ `Widgets` para o
  smoke da `MainWindow`).

---

## 7. Estratégia de testes

Framework **QtTest**, headless com `QT_QPA_PLATFORM=offscreen` (registrado no CTest via env).

**Automatizados (mapeados aos critérios 1–7 do §2):**

- `GridGeometry`: mapeamento faixa→célula e cor por estado — 1 seg, N segs, completo, vazio, erro,
  total desconhecido; e a invariância "nº de células baixadas cresce monotonicamente com o progresso".
- `SpeedSampler`: velocidade a partir de amostras injetadas; ETA correto; zero/indefinido nos limites;
  ETA→0 ao completar.
- `DownloadTableModel`: `rowCount`/`data`; `dataChanged` na linha certa ao receber `taskProgress`
  (usando um `DownloadManager` real contra o `TestServer` da Fase 1, ou um duplo de teste que emite os
  sinais); `appendTask`/`removeTaskById` disparam `rowsInserted`/`rowsRemoved`.
- `CategoryFilterProxy`/`FileType`: classificação por estado e extensão; `All Downloads` aceita tudo.
- `deriveFileName`: variações de URL.
- `DownloadManager::pause/resume/taskById`: contra o `TestServer`, verifica transição de estado e que
  `resume` respeita `maxConcurrentDownloads` (subir 3 tasks, pausar/retomar, o cap nunca é excedido —
  reusa a checagem de concorrência da Fase 1).
- Smoke `MainWindow`: constrói e destrói sob `offscreen`.

**Manuais (critérios 8–10):** um roteiro curto no relatório da fase (baixar uma URL real, pausar/
retomar, olhar a grade/Properties/Log). Não automatizado — renderização e rede real ficam fora do CI.

**Reuso:** o `TestServer` (`QHttpServer`) da Fase 1 é reaproveitado para dirigir o Core nos testes de
model/manager, mantendo tudo offline (`127.0.0.1`).

---

## 8. Fora de escopo (fases futuras)

- **Fase 3:** FTP (`QTcpSocket`), monitor de clipboard, drag & drop na janela, escolha de pasta/
  categoria mais rica no New.
- **Fase 4:** Scheduler (`QTimer`), diálogo Preferences (edita `EngineConfig`, carrega/salva
  `settings.json`), limite de banda, **persistência de categoria** (na Fase 2 a categoria é derivada
  por extensão, não armazenada).
- **Fase 5:** integração com browser.

Nada acima é implementado na Fase 2. A GUI é desenhada para acomodá-los (o model já isola a fonte de
dados; a árvore já tem os nós de categoria; a toolbar já reserva Scheduler/Preferences).

---

## 9. Riscos e mitigações

| Risco | Mitigação |
|---|---|
| Testar QtWidgets no CI (sem display) | `QT_QPA_PLATFORM=offscreen`; lógica pesada extraída para unidades puras (`GridGeometry`/`SpeedSampler`/`FileType`) testadas sem widget. |
| Repintura da grade a cada chunk trava a UI | `segmentProgress` só agenda `update()` via `QTimer` throttle (~100 ms). |
| `dataChanged` a cada `taskProgress` inunda a view | Progresso já vem coalescido do Core (`progressThrottle`); Speed/ETA atualizam só no tick de 1 Hz. |
| Velocidade "pula" (bursts do throttle do Core) | Média móvel curta no `SpeedSampler` (janela de alguns segundos). |
| GUI furar o cap de concorrência chamando task direto | Proibido por design: todo controle passa por `DownloadManager::pause/resume` → `pump()`. |
| Nome de arquivo derivado colide/vazio | `deriveFileName` cai num default ("download") quando a URL não tem nome; o Core já resolve colisão de destino (`resolveUniquePath`, Fase 1 §3.2). |
