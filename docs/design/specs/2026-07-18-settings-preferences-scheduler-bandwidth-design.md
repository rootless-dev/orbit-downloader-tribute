# Orbit Downloader Tribute — Fase 4: Settings + Preferences + Scheduler + Limite de banda

**Data:** 2026-07-18
**Projeto:** `orbit-downloader-tribute`
**Escopo desta spec:** Fase 4 — persistência de configuração em `settings.json`, diálogo
**Preferences** (abas General + Advanced), **limite de banda global**, **Scheduler global** (janela de
horário com recorrência + auto-quit ao concluir), e persistência do **modo de clipboard** e da **pasta
de destino padrão**. **Sem** categorias atribuíveis (seguem derivadas), ações de sistema no scheduler
(shutdown/sleep/hibernar), limite de banda por download ou agendamento por item (fases futuras / fora
de escopo).

---

## 1. Contexto

As Fases 1–3 estão commitadas em `develop`:

- **Fase 1** — Core headless (`orbitcore`): motor HTTP/HTTPS multi-segmento, pause/resume que
  sobrevive ao fechamento do processo.
- **Fase 2** — `orbit-gui`: layout do Orbit, grade de blocos por segmento, diálogo New.
- **Fase 3** — abstração `Transport`, FTP multi-segmento (`FtpProtocol` sobre `QTcpSocket`), monitor de
  clipboard, drag & drop, New estendido.

Depois delas veio uma mini-feature de **detecção de nome via `Content-Disposition`** (commitada) e
uma feature de **User-Agent** — hoje o UA é o constante `kHttpUserAgent = "curl/8.7.1"`
(`src/core/HttpUserAgent.h`), lido em `HttpProbe.cpp` e `SegmentWorker.cpp`.

**O que já existe e NÃO é trabalho desta fase:**

- **Persistência de sessão de downloads** já funciona: `Persistence::writeSession/readSession`
  (JSON de `DownloadRecord` em `downloads.json`), e `main_gui.cpp` chama `mgr.loadSession()` no boot,
  restaurando downloads em andamento. O ROADMAP lista "persistência de sessão" na Fase 4, mas isso já
  foi entregue nas fases anteriores.

**O que a Fase 4 realmente adiciona** são quatro subsistemas independentes: persistência de
**configuração** (o `settings.json` que a Fase 1 deferiu — ver o comentário em `MainWindow.cpp:127`),
o diálogo **Preferences**, o **limite de banda** e o **Scheduler**. Optou-se por **uma spec única**
cobrindo os quatro, como o ROADMAP previa.

### Pré-requisito: feature de User-Agent commitada

O Preferences expõe o **User-Agent** como knob, então a Fase 4 **promove** o constante
`kHttpUserAgent` para um campo `EngineConfig.userAgent`. A spec assume que a feature de UA (hoje no
working tree, não commitada) já foi commitada antes do início da implementação. A primeira
tarefa do plano pode absorver essa promoção se o UA ainda não tiver aterrissado, mas o caminho
preferido é: **o UA é commitado → Fase 4 refatora constante → `EngineConfig.userAgent`.**

### Decisões já tomadas (brainstorming desta fase)

- **Config vive no Core, em `settings.json` próprio** (escrito à mão via `QJsonDocument`, ao lado do
  `downloads.json`), seguindo o padrão do `Persistence.cpp` atual — não `QSettings`. Testável headless
  com caminho injetável, igual ao `sessionPath()`. Descartados: `QSettings` nativo (tira a config do
  motor do `orbitcore`, dependente de plataforma, menos testável) e híbrido (dois mecanismos).
- **Limite de banda GLOBAL** — um único teto de bytes/s compartilhado por todos os downloads/segmentos
  ativos, via token bucket no `DownloadManager`. `0 = ilimitado`. Descartados: por download e
  global+override (YAGNI / complexidade de dois níveis).
- **Scheduler GLOBAL com auto-quit** — iniciar a fila às `HH:MM`, pausar tudo às `HH:MM`, recorrência
  `once`/`daily`, opção de fechar o app quando todos concluírem. **Sem** ações de SO (shutdown/sleep —
  arriscadas e cheias de permissão no macOS). Descartados: só-janela (sem auto-quit) e por item.
- **Preferences com abas General + Advanced.** General: básicos + User-Agent. Advanced: timeouts,
  retries, backoff, minSegSize, throttle.
- **Categorias seguem derivadas** (estado + extensão) — nada persistido por download. Evita um
  subsistema inteiro (campo no record + UI de atribuição + migração). YAGNI até haver necessidade real.

---

## 2. Arquitetura — unidades isoladas e testáveis

Cada unidade tem um propósito, uma interface e pode ser entendida/testada isolada.

| Unidade | Camada | Responsabilidade |
|---|---|---|
| `EngineConfig` (+2 campos) | Core | ganha `maxBytesPerSec` (qint64, 0=ilimitado) e `userAgent` (QString, default `"curl/8.7.1"`) |
| `Persistence::readJsonObject/writeJsonObject` | Core | I/O de `QJsonObject` genérico, escrita atômica, puro (mesmo padrão do `writeSession`) |
| `engineConfigToJson/engineConfigFromJson` | Core | (de)serializa **só** o bloco `engine`; chaves ausentes caem para os defaults passados |
| `RateLimiter` | Core | token bucket global; `take(want, now) → grantedBytes`; tempo injetável; `0 = bypass` |
| `DownloadManager::setConfig()` | Core | aplica config nova em runtime (banda e cap ao vivo; resto vale p/ próximos downloads) |
| `SettingsIo` | GUI (lógica pura, `orbitgui_logic`) | `QJsonObject` ⇄ `AppSettings{engine, ui, scheduler}`; preserva chaves desconhecidas no round-trip |
| `Scheduler` | GUI (lógica pura, `orbitgui_logic`) | guarda `SchedulerConfig`; `tick(now) → Action{None/StartAll/StopAll}`; recorrência; tempo injetável |
| `PreferencesDialog` | GUI (widgets, `orbitgui`) | abas General+Advanced; edita cópia de trabalho; OK → aplica + salva |
| `MainWindow` (fiação) | GUI | carrega settings no boot; `QTimer` → `Scheduler`; ação Preferences; salva ao mudar |

### 2.1 Camadas e dependências

- **Core** conhece só o bloco `engine` (é a única config que o motor headless precisa; o `orbit-cli`
  também poderá carregá-la). Não conhece `ClipboardMode` (tipo da GUI) nem `SchedulerConfig`.
- **GUI** monta o `settings.json` completo: lê o arquivo como um `QJsonObject`, entrega o sub-objeto
  `engine` ao (de)serializador do Core e parseia `ui`/`scheduler` ela mesma. Ao salvar, reconstrói o
  objeto raiz preservando chaves desconhecidas e regrava.

O I/O de arquivo genérico (`readJsonObject`/`writeJsonObject`) fica no Core (`Persistence.cpp`) por
ser puro e já haver ali a mesma mecânica de escrita atômica do `writeSession`.

---

## 3. Detalhe por subsistema

### 3.1 `settings.json` — schema e regras

Caminho: `m_dataDir + "/settings.json"` (ao lado do `downloads.json`).

```json
{
  "version": 1,
  "engine": {
    "maxConcurrentDownloads": 3, "segmentCount": 4, "minSegSize": 1048576,
    "maxSegmentRetries": 5, "retryBackoffBaseMs": 1000,
    "connectTimeoutMs": 30000, "idleTimeoutMs": 30000, "progressThrottleMs": 200,
    "maxBytesPerSec": 0, "userAgent": "curl/8.7.1"
  },
  "ui":        { "defaultDownloadDir": "~/Downloads", "clipboardMode": "Off" },
  "scheduler": { "enabled": false, "startTime": "08:00", "stopTime": "18:00",
                 "recurrence": "daily", "quitWhenDone": false }
}
```

Regras:

- **Arquivo ausente** → `AppSettings` inteiramente default (não é erro; é o primeiro boot).
- **JSON corrompido / raiz não-objeto** → default + log; nunca crasha.
- **Chave individual ausente ou de tipo errado** → cai para o default daquele campo (parse tolerante,
  campo a campo). Um `settings.json` de versão futura com campos a mais não deve zerar os conhecidos.
- **Chaves desconhecidas preservadas:** ao regravar, o objeto raiz é lido, só as seções conhecidas são
  sobrescritas, e o resto é mantido. (Round-trip do objeto inteiro, não reconstrução do zero.)
- `clipboardMode`: string `"Off"|"Ask"|"Auto"|"Notify"` ⇄ `enum class ClipboardMode`; valor
  desconhecido → `Off`.
- `defaultDownloadDir`: `~` expandido na leitura para o home real; vazio → `QStandardPaths` Downloads.

### 3.2 `EngineConfig` — dois campos novos

```cpp
struct EngineConfig {
    // ... campos existentes ...
    qint64  maxBytesPerSec = 0;              // 0 = ilimitado (teto GLOBAL)
    QString userAgent      = "curl/8.7.1";   // enviado em probe + segmentos HTTP
};
```

`userAgent` substitui o `kHttpUserAgent`. `HttpProbe` e `SegmentWorker` passam a ler do `EngineConfig`
que já lhes chega (mesma via pela qual os outros campos de config já os alcançam). O header
`HttpUserAgent.h` deixa de conter o constante; se sobrar só o default, pode virar o valor inicial do
campo no `DownloadTypes.h` e o arquivo é removido.

### 3.3 Limite de banda — `RateLimiter` (unidade de maior risco)

Token bucket global, dono no `DownloadManager`:

```cpp
class RateLimiter {
public:
    void   setRate(qint64 bytesPerSec);              // 0 = ilimitado
    qint64 take(qint64 want, qint64 nowMs);          // devolve bytes concedidos (0..want)
    // reabastece proporcional ao tempo decorrido desde a última chamada; cap de burst ~= 1s de taxa
};
```

- **Tempo injetável** (`nowMs`), como o `SpeedSampler` da Fase 2 — testável sem relógio real.
- `bytesPerSec == 0` → `take` devolve sempre `want` (bypass total; custo zero no caminho ilimitado).
- **Integração nos workers:** no `readyRead`, o worker pede ao limiter até o tamanho disponível no
  reply e lê **apenas o concedido**; o excedente fica no buffer do socket. Backpressure real via
  `QNetworkReply::setReadBufferSize` (HTTP) / leitura parcial do `QTcpSocket` (FTP). Um `QTimer` do
  `DownloadManager` (ex.: 50 ms) acorda os workers com dados pendentes conforme os tokens repõem.
- **Validação isolada primeiro:** o `RateLimiter` é implementado e testado por completo (TDD) antes de
  tocar nos workers. O throttling real do transporte entra atrás desse teste.

Semântica: o teto é **agregado** — a soma das taxas de todos os segmentos ativos respeita
`maxBytesPerSec`. A divisão entre segmentos emerge de todos consultarem o mesmo bucket (não há cota
por segmento).

### 3.4 `DownloadManager::setConfig(const EngineConfig&)`

Hoje a config é capturada em `m_cfg` no construtor e repassada a cada `DownloadTask` na criação. O
setter novo atualiza `m_cfg` e distingue:

- **Aplica ao vivo:** `maxBytesPerSec` (chama `RateLimiter::setRate`) e `maxConcurrentDownloads`
  (re-`pump()` para promover/estabilizar dentro do novo cap).
- **Vale só para próximos downloads:** `segmentCount`, timeouts, retries, backoff, `minSegSize`,
  `progressThrottleMs`, `userAgent` — downloads já ativos mantêm o que capturaram. (Documentado no
  rodapé do Preferences para o usuário não se surpreender.)

Sem tentativa de re-segmentar downloads em andamento (fora de escopo).

### 3.5 `SettingsIo` (GUI, lógica pura)

```cpp
struct UiPrefs        { QString defaultDownloadDir; ClipboardMode clipboardMode = ClipboardMode::Off; };
struct SchedulerConfig{ bool enabled=false; QTime start{8,0}; QTime stop{18,0};
                        Recurrence recurrence=Recurrence::Daily; bool quitWhenDone=false; };
struct AppSettings    { EngineConfig engine; UiPrefs ui; SchedulerConfig scheduler; };

namespace SettingsIo {
    AppSettings fromJson(const QJsonObject& root, const EngineConfig& defaults);
    QJsonObject toJson(const AppSettings&, const QJsonObject& previousRoot); // preserva desconhecidas
    AppSettings load(const QString& path, const EngineConfig& defaults);      // usa Persistence::readJsonObject
    void        save(const QString& path, const AppSettings&);                // read-modify-write do root
}
```

Puro e testável em `orbitgui_logic` (sem widgets). Usa o (de)serializador de `engine` do Core para o
bloco do motor.

### 3.6 `PreferencesDialog` (GUI, widgets)

`QDialog` com `QTabWidget`:

- **General:** downloads simultâneos (spin) · segmentos por download (spin) · velocidade máx em KB/s
  (spin, 0=ilimitado) · pasta de destino padrão (linha + botão Browse) · modo clipboard (combo
  Off/Ask/Auto/Notify) · **User-Agent** (combo com presets `curl/8.7.1` | `Chrome` | `Custom…`, com
  campo de texto habilitado no Custom).
- **Advanced:** connect timeout · idle timeout · max retries · backoff base (ms) · min segment size
  (KB) · progress throttle (ms).
- Edita uma **cópia de trabalho** de `AppSettings`. **OK** → `DownloadManager::setConfig()` +
  aplica `ClipboardMode` no `ClipboardWatcher` + atualiza a pasta padrão do New + `SettingsIo::save`.
  **Cancel** → descarta. Rodapé com a nota "algumas mudanças só valem para novos downloads".

Aberto por toolbar (**Preferences**) e/ou menu.

### 3.7 `Scheduler` (GUI, lógica pura)

```cpp
enum class Recurrence { Once, Daily };
enum class SchedAction { None, StartAll, StopAll };

class Scheduler {
public:
    void        setConfig(const SchedulerConfig&);
    SchedAction tick(const QDateTime& now);   // idempotente entre disparos; sem repique
};
```

- `tick` retorna `StartAll` no primeiro tick em que `now` cruza `startTime`, `StopAll` idem para
  `stopTime`, e `None` no resto. Guarda estado do último disparo para **não repetir** enquanto dentro
  da mesma janela.
- `Once`: dispara uma vez e desarma (`enabled → false` após a janela). `Daily`: rearma a cada dia.
- Tempo injetável → testável sem relógio real.
- **Fiação no `MainWindow`:** `QTimer` de ~30 s chama `scheduler.tick(QDateTime::currentDateTime())`;
  `StartAll → mgr.resumeAll()`, `StopAll → mgr.pauseAll()`. `quitWhenDone`: ao receber
  `taskStateChanged(Completed)`, se **todas** as tasks estão `Completed`, `qApp->quit()` (com o guard
  de já ter havido ao menos um download; não fecha app vazio).

---

## 4. Fiação no boot (`MainWindow` / `main_gui.cpp`)

O `settingsPath` deriva do **mesmo `dataDir`** que o `DownloadManager` usa para o `downloads.json`
(`dataDir + "/settings.json"`) — hoje o `main_gui.cpp` já conhece esse `dataDir` ao construir o
manager, então o boot passa a lê-lo antes de construir o manager (o load não depende do manager).

Ordem no boot:

1. `SettingsIo::load(settingsPath, EngineConfig{})` → `AppSettings`.
2. Construir `DownloadManager` com `settings.engine` (em vez do `EngineConfig{}` default de hoje).
3. `mgr.loadSession()` (como já é).
4. Aplicar `settings.ui` (modo clipboard no `ClipboardWatcher`, pasta padrão no New).
5. Aplicar `settings.scheduler` no `Scheduler`; iniciar o `QTimer`.

Persistência: salvar `settings.json` sempre que o Preferences confirma, quando o modo de clipboard
muda pelo menu `Tools`, e quando a config do scheduler muda. (Não é preciso salvar a cada tick.)

---

## 5. Testes (QtTest, offline, no padrão do projeto)

- **`tst_settings`** (novo): round-trip `write→read` idêntico; arquivo ausente → default; JSON
  corrompido → default; campo ausente/tipo errado → default daquele campo; **chaves desconhecidas
  preservadas** na regravação; `clipboardMode` string ⇄ enum; `~` expandido.
- **`tst_ratelimiter`** (novo): concessão de tokens ao longo de tempo injetado; `0 = ilimitado`
  (concede tudo); burst limitado; taxa média converge ao teto.
- **`tst_scheduler`** (novo): `tick` produz `StartAll`/`StopAll` nos cruzamentos corretos; sem repique
  dentro da janela; `Once` desarma, `Daily` rearma no dia seguinte.
- **`tst_gui`** (estende): `PreferencesDialog` OK produz o `EngineConfig`/`UiPrefs` esperado e persiste;
  boot aplica `settings.engine` ao manager e `settings.ui` à GUI; preset de UA "Custom" grava o texto.
- **`tst_download`** (gate): 27 casos **sem mudança de expectativa** — o motor não regride. O
  `RateLimiter` com teto `0` não deve alterar tempos/bytes dos testes existentes.

### 5.1 Verificação manual humana (não automatizável — critérios de aceite finais)

1. **Limite de banda real:** definir teto (ex.: 500 KB/s) no Preferences, baixar arquivo grande, ver a
   velocidade agregada estabilizar perto do teto; `0` volta a ilimitado.
2. **Scheduler:** configurar start/stop poucos minutos à frente, ver a fila iniciar e pausar nos
   horários; `Daily` dispara de novo no dia seguinte; `quitWhenDone` fecha o app ao concluir tudo.
3. **Persistência:** mudar knobs no Preferences, fechar e reabrir o app, confirmar que voltam; idem
   modo de clipboard e pasta padrão.
4. **User-Agent:** com preset `curl/8.7.1`, baixar de servidor que bloqueia UA de browser (ex.:
   akirabox) e confirmar sucesso; trocar para `Chrome` e observar a diferença de comportamento.

---

## 6. Fora de escopo desta fase

- Categorias atribuíveis/persistidas (seguem derivadas de estado+extensão).
- Ações de sistema no scheduler (shutdown/sleep/hibernar, desconectar).
- Limite de banda por download e agendamento por item.
- Persistência de geometria de janela/splitter (candidata a follow-up menor; não bloqueia a fase).
- Integração com browser (Fase 5) e P2P/P2SP (fase futura).

---

## 7. Riscos e dívidas

- **Throttling real do transporte (§3.3)** é o ponto mais incerto: o comportamento de
  `setReadBufferSize` + repique por `QTimer` sob `QNetworkReply`/`QTcpSocket` pode não bater 1:1 com o
  modelo do `RateLimiter`. Mitigação: `RateLimiter` isolado e testado antes; teto real validado no E2E
  manual (§5.1.1), não em teste automatizado.
- **`setConfig()` ao vivo (§3.4):** só banda e cap mudam em downloads ativos; o resto vale para os
  próximos — precisa estar claro na UI para não confundir.
- **UA como pré-requisito (§1):** a Fase 4 assume a feature de UA commitada. Se ainda não estiver, a
  primeira tarefa do plano promove o constante para `EngineConfig.userAgent` antes de tudo.
- **Preservação de chaves desconhecidas (§3.1):** o `save` precisa fazer read-modify-write do objeto
  raiz; um `save` que reconstrói do zero perderia campos de versões futuras.
