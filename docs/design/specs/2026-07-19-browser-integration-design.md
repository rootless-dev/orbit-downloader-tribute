# Fase 5 — Integração com browser (design)

> Spec de design. Fase 5 do `ROADMAP.md`. Reimplementa a peça de "integração com
> browser" do Orbit original: uma extensão de navegador que entrega downloads ao
> gerenciador. Escopo desta fase: **interceptar todos os downloads** do Chrome/Chromium
> e enfileirá-los no app, encaminhando o contexto de sessão (cookies/headers).
> Data: 2026-07-19.

---

## 1. Objetivo e resumo

O app hoje recebe downloads por diálogo New, monitor de clipboard e drag & drop —
todos desaguando em `MainWindow::enqueue(url, dir)` → `DownloadManager::addDownload`.
Esta fase adiciona um **quarto caminho de entrada**: uma extensão Chrome/Chromium que
intercepta os downloads iniciados no navegador, cancela-os no Chrome e os repassa ao
app rodando, via um **endpoint HTTP local em loopback**.

Comportamento central escolhido: **interceptar todos os downloads** (não apenas um menu
de contexto opt-in). Como muitos downloads dependem da sessão do navegador, a extensão
**encaminha cookies + `Referer` + `User-Agent`** junto da URL, e o app os reenvia — o que
exige uma pequena mudança no Core (headers por-download).

Quando o app recebe um download da extensão, ele **adiciona automaticamente** (a intenção
do usuário já é explícita — ele clicou em baixar) e **notifica pela bandeja**
(`QSystemTrayIcon` já existente). Sem diálogo, sem interrupção.

## 2. Decisões fechadas no brainstorming (não re-decidir sem motivo)

| Tema | Decisão |
|---|---|
| **Comportamento** | Interceptar **todos** os downloads do navegador (cancela no Chrome, entrega ao app). |
| **Canal IPC** | **Servidor HTTP local** (`QTcpServer` em `127.0.0.1:porta`); a extensão faz `fetch(POST)`. Casa com o event loop do Qt (sem threads) e com o "socket endpoint" do roadmap. |
| **Sessão/cookies** | **Encaminhar cookies + `Referer` + `User-Agent`**. Requer headers por-download no Core. |
| **Navegadores** | **Chrome/Chromium MV3 primeiro** (Edge/Brave/Chromium). Firefox/Safari em fase futura. |
| **Filtros** | **Só liga/desliga global.** Sem filtro de tamanho/tipo/domínio (futuro, YAGNI). |
| **UX no app** | **Adiciona automaticamente + notifica** pela bandeja. Sem diálogo por item. |
| **Segurança do endpoint** | **Checar `Origin` (`chrome-extension://<id-fixo>`) + token compartilhado** no header. Defesa em profundidade. |
| **Servidor app-side** | **`QTcpServer` + parser HTTP mínimo próprio.** Lógica de parse/autorização como **função pura testável** (padrão do `shouldOffer`); zero dependência nova; mesmo estilo do FTP hand-rolled. |
| **Sentido do fluxo** | **Um sentido só** (extensão → app). Sem app → extensão. A extensão mostra a própria notificação de sucesso. |
| **Sync de config** | Nenhum. Token e porta colados à mão na página de opções da extensão. |

## 3. Componentes

1. **Extensão Chrome/Chromium MV3** — deliverable externo em JS (fora da lib C++). Service
   worker que intercepta downloads e os repassa; página de opções.
2. **`BrowserBridge`** (novo, app-side, **apenas QtNetwork/QtCore**, sem QtWidgets) — dono do
   `QTcpServer` em loopback; recebe o POST, autoriza, e **emite `downloadRequested`**.
3. **Fio de ligação na `MainWindow`** — conecta `downloadRequested` ao caminho de enfileirar
   (auto-add + notificação na bandeja) e cria/derruba o bridge conforme a configuração.
4. **Mudança no Core** — `addDownload` aceita headers por-download; `HttpTransport` os aplica.
5. **Preferences + Settings** — seção Browser (habilitar/porta/token) persistida em `settings.json`.

## 4. Protocolo (loopback HTTP)

Tudo em `127.0.0.1` (apenas loopback; o `QTcpServer` faz bind em `QHostAddress::LocalHost`).

### 4.1. Requisição de adicionar download

```
POST /add HTTP/1.1
Host: 127.0.0.1:8697
Origin: chrome-extension://<id-fixo-da-extensão>
Content-Type: application/json
X-Orbit-Token: <token de 32 hex>

{
  "url":       "https://host/path/file.zip",
  "filename":  "file.zip",
  "referrer":  "https://host/page",
  "userAgent": "Mozilla/5.0 ...",
  "cookie":    "sid=abc; theme=dark"
}
```

Campos do corpo (JSON):
- `url` (obrigatório) — a URL do download. Só `http`/`https` são aceitos nesta fase.
- `filename` (opcional) — nome sugerido pelo navegador; usado como preferência de destino.
- `referrer` (opcional) — vira header `Referer` no request do app.
- `userAgent` (opcional) — vira header `User-Agent` no request do app (sobrepõe o
  `EngineConfig.userAgent` **apenas para esta task**).
- `cookie` (opcional) — string pronta do header `Cookie` (`name=val; name2=val2`),
  montada pela extensão via `chrome.cookies.getAll`.

### 4.2. Respostas

| Situação | Status | Corpo |
|---|---|---|
| Aceito e enfileirado | `200` | `{"ok":true}` |
| Token ausente ou incorreto | `401` | `{"ok":false,"error":"unauthorized"}` |
| `Origin` inválido | `403` | `{"ok":false,"error":"forbidden"}` |
| JSON malformado, `url` ausente ou esquema não suportado | `400` | `{"ok":false,"error":"bad_request"}` |
| Rota/método desconhecido | `404` | `{"ok":false,"error":"not_found"}` |

Toda resposta (inclusive erros) carrega os headers CORS da §4.3 para que o `fetch` da
extensão consiga ler o status (senão o Chrome esconde a resposta atrás de erro de CORS).
O `Access-Control-Allow-Origin` sempre ecoa a **constante permitida** (não o `Origin`
recebido). Assim a extensão legítima lê até o `403`; um origin não-permitido não bate com
o ACAO e recebe erro de CORS do próprio Chrome — o que é o comportamento desejado.

### 4.3. Preflight (CORS + Private Network Access)

Uma extensão com `host_permissions` cobrindo o loopback **pode fazer o `fetch` sem
preflight de CORS** (o Chrome dá privilégio elevado a hosts permitidos e pula a checagem
CORS — inclusive pode **não enviar `Origin`**; ver §6.1). Porém o Chrome aplica **Private
Network Access (PNA)** a requests que atingem loopback e **pode exigir um preflight `OPTIONS`
com o header PNA**, independente de CORS. Além disso, um site público que tentar `fetch` ao
loopback **sempre** cai no preflight PNA e é barrado ali.

Portanto o servidor **sempre** trata `OPTIONS /add` corretamente (cinto e suspensório): se
o preflight ocorrer, ele passa; se não ocorrer, o handler simplesmente nunca é chamado.

`OPTIONS /add` responde `204 No Content` com:
```
Access-Control-Allow-Origin: chrome-extension://<id-fixo>
Access-Control-Allow-Methods: POST, OPTIONS
Access-Control-Allow-Headers: Content-Type, X-Orbit-Token
Access-Control-Allow-Private-Network: true
Access-Control-Max-Age: 600
Vary: Origin
```

O `Access-Control-Allow-Origin` **ecoa exatamente** o `chrome-extension://<id-fixo>`
permitido (não `*`, que não combina com credenciais/headers customizados e é menos seguro).

## 5. Extensão Chrome/Chromium (MV3)

Deliverable em `extension/chrome/` (JS + `manifest.json`), fora da lib C++.

### 5.1. `manifest.json`

- `manifest_version: 3`.
- `permissions`: `downloads`, `cookies`, `notifications`, `storage`.
- `host_permissions`: `<all_urls>` — necessário para ler cookies de qualquer site
  (`chrome.cookies.getAll`) e para `fetch` ao loopback.
- **`key` fixado** no manifesto → **ID de extensão estável** entre máquinas/instalações
  unpacked. Isso permite ao app **allowlistar um `Origin` conhecido e fixo** sem exigir
  configuração de ID pelo usuário. **Gerar o par de chaves, fixar o `key` no manifesto e
  registrar o ID resultante** (como constante no app e nesta spec) é uma tarefa do plano —
  o ID concreto só existe após gerar a chave.
- `background.service_worker`: o script de interceptação.
- `options_page` (ou `options_ui`): a página de opções.

### 5.2. Interceptação (service worker)

Listener em `chrome.downloads.onCreated(downloadItem)`:

1. **Gate**: prosseguir apenas se
   - a extensão está **habilitada** **e o token está configurado** (não-vazio) em
     `chrome.storage.local`. Sem token, **não intercepta e não notifica** — o navegador
     baixa normalmente (evita churn de cancel/erase e falsos "Orbit indisponível" antes do
     setup);
   - a URL efetiva (**`downloadItem.finalUrl`**, com fallback para `.url`) começa com
     `http://` ou `https://`;
   - a URL **não** é `blob:`, `data:`, `filesystem:` nem `chrome-extension://` (não
     re-baixáveis pelo app — deixa o navegador cuidar).
2. **Coletar contexto** (usando a URL efetiva `finalUrl`):
   - `url` — `finalUrl` (pós-redirect): é a que o app realmente vai buscar, e a que casa
     com os cookies coletados;
   - `filename` — de `downloadItem.filename` (pode vir vazio no `onCreated`; cair para
     derivação da URL);
   - `referrer` — `downloadItem.referrer`;
   - `userAgent` — `navigator.userAgent`;
   - `cookie` — `chrome.cookies.getAll({ url: finalUrl })` juntado em `name=val; ...`. A
     **API `chrome.cookies` enxerga cookies `HttpOnly`** (ao contrário de `document.cookie`)
     — é por isso que a usamos: os cookies de sessão costumam ser `HttpOnly`.
3. **POST** para `http://127.0.0.1:<porta>/add` com `X-Orbit-Token` e o corpo JSON.
4. **Sucesso (`200`)** → `chrome.downloads.cancel(id)` seguido de `chrome.downloads.erase({id})`;
   notificação discreta "Enviado ao Orbit".
5. **Falha de rede (app desligado) ou não-`200`** → **não cancela**: deixa o navegador
   concluir o download normalmente, e mostra **uma** notificação ("Orbit indisponível —
   o navegador vai baixar"). **Nenhum download é perdido.**

> **Nota de corrida (documentada, aceita):** `onCreated` dispara *depois* do download
> começar; um `.crdownload` parcial pode existir por instantes. `cancel` + `erase` limpam.
> Aceitável para esta fase.

A decisão pura "interceptar esta URL?" fica isolada numa função (`shouldIntercept(url, enabled)`)
para clareza e verificação manual; não entra no harness C++ (é JS).

### 5.3. Página de opções

Campos, persistidos em `chrome.storage.local`:
- **Habilitar interceptação** (checkbox; default **desligado** — o usuário opta por ligar).
- **Porta** (default `8697`).
- **Token** (campo de texto; o usuário cola o token gerado pelo app nas Preferences).

## 6. App-side: `BrowserBridge`

Novo par `BrowserBridge.{h,cpp}` + `BrowserBridgeProtocol.h` na lib **`orbitgui_logic`** (a lib
"pura", sem QtWidgets — hoje linka `orbitcore` + `Qt6::Core`; somamos **`Qt6::Network`** para o
`QTcpServer`). Isso mantém a testabilidade headless: `tst_browserbridge` linka `orbitgui_logic`
(como `tst_settings`/`tst_scheduler` já fazem), sem arrastar QtWidgets.

### 6.1. Funções puras (testáveis sem socket)

Em um header dedicado (ex. `BrowserBridgeProtocol.h`):

- `struct AddRequest { QString method, path, origin, token; QByteArray body; bool complete; };`
- `AddRequest parseAddRequest(const QByteArray& rawHttp)` — parse mínimo de request-line +
  headers + corpo. Tolerante a espaços/caixa de header; retorna `complete=false` se ainda
  não chegou o corpo inteiro (ver §6.2).
- `struct DownloadPayload { QUrl url; QString filename, referrer, userAgent, cookie; bool valid; };`
- `DownloadPayload parseBody(const QByteArray& json)` — valida `url` (http/https), extrai campos.
- `enum class AuthResult { Ok, Unauthorized, Forbidden };`
- `AuthResult authorize(const AddRequest& req, const QString& expectedToken, const QString& allowedOrigin)`
  com semântica:
  1. **`Forbidden`** se o request **traz** `Origin` **e** ele **difere** do permitido
     (bloqueia outra extensão / página web que mande um `Origin` conhecido-ruim);
  2. senão **`Unauthorized`** se o token está ausente ou incorreto;
  3. senão **`Ok`**.

  > **Por que o token é o portão duro, não o `Origin`:** requisições de uma extensão com
  > `host_permissions` sobre o loopback **podem chegar sem header `Origin`** (§4.3). Se
  > exigíssemos `Origin` idêntico, rejeitaríamos a extensão legítima. Então `Origin`
  > **ausente é aceito** (desde que o token confira); `Origin` **presente-e-divergente é
  > recusado**. Uma página web hostil sempre carrega o próprio `Origin` (o navegador anexa
  > em cross-origin) e não conhece o token — barrada de qualquer forma. O `key` fixado é
  > **público** (está no repositório), então o `Origin` não é segredo; **o token é a
  > credencial de verdade**.

  Comparação de token em **tempo constante** (evita timing-leak; `QMessageAuthenticationCode`
  ou comparação byte-a-byte sem short-circuit).

Estas funções concentram toda a lógica sensível e são o alvo dos testes unitários.

### 6.2. Camada de socket

- `QTcpServer` com bind em `QHostAddress::LocalHost` na porta configurada.
- **Buffer por-conexão**: cada `QTcpSocket` tem seu próprio acumulador (ex. `QHash<QTcpSocket*,
  QByteArray>`, limpo em `disconnected`), pois vários downloads simultâneos abrem conexões
  concorrentes. Acumula bytes até ter os headers completos (`\r\n\r\n`); lê **`Content-Length`**
  e espera o corpo inteiro (o `fetch` com corpo JSON sempre manda `Content-Length`; **não**
  suportamos `Transfer-Encoding: chunked` → `400`).
- **Guarda de tamanho**: **rejeita** (413 + fecha) requests acima de um teto (ex. 64 KiB — o
  header `cookie` pode ser grande, mas não absurdo), contando desde o começo do buffer para
  não acumular indefinidamente.
- Trata `OPTIONS /add` → `204` com headers da §4.3. Trata `POST /add` → autoriza (§6.1),
  parseia o corpo, e em `Ok` **emite `downloadRequested(url, headers, filename)`** e responde `200`.
- Método `bool start(quint16 port)` → `false` se o bind falhar (**porta ocupada**); `void stop()`.
  Um método `quint16 port()` / estado de erro alimenta o rótulo de status das Preferences.

### 6.3. Sinal exposto

```cpp
signals:
    void downloadRequested(const QUrl& url,
                           const QList<QPair<QByteArray,QByteArray>>& headers,
                           const QString& suggestedFilename);
```
Os headers já vêm montados (`Cookie`, `Referer`, `User-Agent`) a partir do payload.

## 7. App-side: ligação na `MainWindow`

- Na inicialização (e ao mudar as Preferences), se `browser.enabled`:
  - cria o `BrowserBridge`, chama `start(port)`. Se falhar (porta ocupada), **não trava**:
    marca estado de erro e mostra a mensagem nas Preferences.
  - conecta `downloadRequested` a um slot que:
    1. deriva o destino: pasta padrão (`defaultDir()`) + `suggestedFilename` (ou derivado da URL);
    2. chama `m_mgr->addDownload(url, destPath, headers)`;
    3. `m_model->appendTask(...)`;
    4. notifica pela bandeja ("Novo download do navegador: <nome>").
- Ao desabilitar, `stop()` e destrói o bridge.

## 8. Preferences + Settings

### 8.1. UI (nova seção "Browser" na `PreferencesDialog`)

- Checkbox **Habilitar integração com o navegador**.
- Campo **Porta** (default `8697`).
- Campo **Token** (read-only) + botões **Copiar** e **Regerar**.
  - Token gerado no primeiro enable (ou via "Regerar") com **`QRandomGenerator::system()`**,
    32 dígitos hex.
- Rótulo de status: "Escutando em 127.0.0.1:<porta>" ou o erro de bind (porta ocupada).

### 8.2. Persistência (`settings.json`, via `SettingsIo`)

Novo bloco:
```json
"browser": { "enabled": false, "port": 8697, "token": "" }
```
Defaults tolerantes; **preserva chaves desconhecidas** (padrão já estabelecido do `SettingsIo`).
`SettingsIo::load/save` ganham o bloco `browser`.

## 9. Mudança no Core (headers por-download)

Segue **o mesmo molde do `Credentials`** (que já trafega por parâmetro até
`Probe::start`/`SegmentSource::start`, com HTTP ignorando por ora — ver `Transport.h`).

- **Tipo**: `using HeaderList = QList<QPair<QByteArray, QByteArray>>;` em `DownloadTypes.h`.
- **Assinatura**: `QUuid addDownload(const QUrl& url, const QString& destPath,`
  `const HeaderList& extraHeaders = {})`. Parâmetro opcional → todos os call-sites existentes
  (CLI, New, clipboard, drag & drop) seguem compilando sem mudança.
- **Threading (decisão concreta, não deixar o implementador escolher):**
  - `DownloadTask` ganha membro `HeaderList m_extraHeaders`. `init(...)` recebe um parâmetro
    `extraHeaders` a mais (o `addDownload` repassa); `restore(...)` o preenche a partir do
    `DownloadRecord` (resume). `DownloadTask::record()` inclui `m_extraHeaders` no registro
    devolvido, então `saveSession()` já o persiste.
  - `Probe::start` e `SegmentSource::start` ganham um parâmetro final
    `const HeaderList& extraHeaders`. A `DownloadTask` passa `m_extraHeaders` (assim como já
    passa `m_creds`). Implementações a atualizar: `HttpProbe`, `SegmentWorker` (worker HTTP),
    `FtpProbe`, `FtpSegmentWorker`, e os fakes (`FakeProbe`, `FakeWorker`, `RestartingWorker`
    — já fazem `Q_UNUSED(creds)`, então só somam `Q_UNUSED(extraHeaders)`).
- **`HttpProbe` / `SegmentWorker`**: **depois** de setar o `User-Agent` default
  (`m_cfg.userAgent`), aplicam `req.setRawHeader(h.first, h.second)` para cada header extra.
  Como `setRawHeader` **substitui** o valor do mesmo header, um `User-Agent` vindo do browser
  **vence** o default automaticamente — sem header duplicado. O `SegmentWorker` guarda os
  headers num membro para reaplicá-los em cada `openRequest()` (retry/resume de segmento).
- **`FtpProbe` / `FtpSegmentWorker`**: **ignoram** (`Q_UNUSED`) — interceptação é http/https.
- **Persistência/resume**: `DownloadRecord` ganha `HeaderList extraHeaders`.
  `Persistence::writeSession`/`readSession` serializam como array de objetos
  `{ "name": "...", "value": "..." }` em `downloads.json`. `readMeta`/`writeMeta` **não**
  mudam (headers vivem no registro de sessão, não no `.meta` de segmentos).
  - **Tradeoff de privacidade documentado:** diferente das **credenciais** (que ficam **só em
    memória**, nunca em disco), os **cookies são persistidos em texto puro** em
    `downloads.json` para o resume funcionar. É um arquivo local por-usuário no app-data
    (mesmo lugar onde as URLs já ficam). Decisão consciente desta fase; criptografar fica
    como follow-up.
  - **Limitação documentada**: cookies expiram. Um resume muito depois da pausa pode falhar
    autenticação (mesma classe da staleness do clipboard). Aceito nesta fase.

## 10. Segurança e casos de borda

- **Página web maliciosa sondando a porta** → barrada pelo **token** (que ela não conhece) e,
  se mandar `Origin`, pelo mismatch; além disso o **preflight PNA** do Chrome já bloqueia
  sites públicos → loopback antes mesmo de chegar ao app.
- **Modelo de ameaça (explícito):** o alvo é impedir **páginas web** e **outras extensões** de
  injetar downloads. Um **processo local rodando como o mesmo usuário** está **fora do modelo**
  — ele pode ler `settings.json` (onde o token vive) e o próprio app-data, exatamente como o
  gerenciador. Não tentamos nos defender do mesmo-usuário-local (seria impossível e é a mesma
  fronteira de confiança do app).
- **Guard de tamanho** no request inteiro (§6.2) e, em particular, no header `cookie`.
- **`blob:`/`data:`/`filesystem:`/stream/DRM** → não interceptados (a extensão os pula); ficam
  no navegador.
- **App desligado** → a extensão não cancela; o navegador conclui + notifica (§5.2.5).
- **Porta ocupada** → bridge desligado, erro nas Preferences, app segue (§7).
- **Downloads simultâneos** → cada `onCreated` é tratado independentemente; `resolveUniquePath`
  (já existente) evita sobrescrita de arquivo.
- **Token não configurado** → a **extensão** nem intercepta (§5.2.1); se ainda assim algo
  chegar ao bridge sem token, ele responde `401`.

## 11. Estratégia de testes

### 11.1. Unit — puro, sem socket (`tst_browserbridge`, novo)
- `parseAddRequest`: request bem-formado; header faltando; caixa/espaços variados; corpo
  incompleto (`complete=false`); request acima do teto.
- `parseBody`: JSON válido; `url` ausente; esquema não-http (rejeita); campos opcionais ausentes.
- `authorize` (semântica §6.1): `Origin` permitido + token ok → `Ok`; **`Origin` ausente** +
  token ok → **`Ok`** (caso real da extensão com host-permission); `Origin` presente-e-errado
  → `Forbidden` (mesmo com token ok); token ausente/errado (com `Origin` ok ou ausente) →
  `Unauthorized`; comparação de token em tempo constante.

### 11.2. Integração — loopback real
- Sobe o `BrowserBridge` em porta efêmera; cliente `QTcpSocket`/`QNetworkAccessManager`:
  - `POST /add` válido (com `Origin` permitido) → `downloadRequested` emitido (`QSignalSpy`)
    com url/headers/filename corretos, resposta `200`.
  - `POST /add` **sem `Origin`** + token ok → `200` + sinal (caso da extensão host-permission).
  - token ruim → `401`, sem sinal.
  - `Origin` presente-e-errado → `403`, sem sinal.
  - request acima do teto → `413`/fechado, sem sinal.
  - `OPTIONS /add` → `204` com os headers CORS/PNA exatos da §4.3.

### 11.3. Core (`tst_download`, estendido)
- `addDownload` com `extraHeaders` → o `TestServer` confere que os requests de segmento
  chegaram com `Cookie`/`Referer`/`User-Agent`.
- **Gate**: os 27 casos existentes de `tst_download` continuam **sem mudança de expectativa**
  (só adicionamos casos).

### 11.4. Extensão — E2E manual (fora do harness C++)
Passos documentados (não automatizáveis nesta fase):
1. Carregar unpacked no Chrome; habilitar; colar token e porta.
2. Baixar um arquivo público → aparece e progride no app; notificação da bandeja.
3. Baixar arquivo **atrás de login** (com cookie) → funciona no app.
4. App **desligado** → o navegador conclui + notifica "Orbit indisponível".
5. `blob:`/preview → o navegador cuida, app não interfere.

## 12. Escopo / não-metas (esta fase)

- **Só Chrome/Chromium MV3** (Edge/Brave/Chromium). Firefox/Safari em fase futura.
- **Só liga/desliga global.** Sem filtro de tamanho/tipo/domínio (futuro).
- **Um sentido só** (extensão → app). Sem sync de config nem status app → extensão além da
  notificação própria da extensão.
- **FTP não se aplica** (interceptação é http/https).
- **Grab / captura de mídia (RTMP/FLV)** continua **fora** (roadmap).
- **Publicação na Chrome Web Store** fora de escopo (instalação unpacked/dev).

## 13. Entregáveis

- Lib/alvo do app: `BrowserBridge.{h,cpp}` + `BrowserBridgeProtocol.h` (funções puras).
- Mudança no Core: `addDownload` com headers; propagação em `HttpTransport`; persistência.
- UI: seção Browser na `PreferencesDialog`; bloco `browser` no `SettingsIo`.
- Extensão: `extension/chrome/` (`manifest.json`, service worker, página de opções).
- Testes: `tst_browserbridge` (unit + integração loopback); casos novos em `tst_download`.
- Docs: passos de E2E manual (§11.4) e como carregar a extensão unpacked.
