# Orbit Downloader Tribute — Nome de arquivo via Content-Disposition

**Data:** 2026-07-17
**Projeto:** `orbit-downloader-tribute`
**Escopo desta spec:** detectar o nome real do arquivo a partir do header HTTP `Content-Disposition`
durante a entrada de um download HTTP(S), preenchendo o campo de nome no diálogo New. Inclui um
utilitário de parsing puro no Core, a extensão do `HttpProbe`/`ProbeResult` para capturar o header, e
o disparo de probe assíncrono (com debounce) no `NewDownloadDialog`. **Fora de escopo:** re-verificação
durante o download, FTP, seguir a página HTML de confirmação do Google Drive, e qualquer persistência.

---

## 1. Contexto e problema

Hoje o nome do arquivo de destino é decidido **exclusivamente a partir da URL**, na GUI, pela função
`deriveFileName()` em `src/gui/UrlName.cpp`:

```cpp
QString deriveFileName(const QUrl& url) {
    const QString path = url.path(QUrl::FullyDecoded);
    const QString name = QFileInfo(path).fileName();
    return name.isEmpty() ? QStringLiteral("download") : name;
}
```

Para URLs cujo nome real vem só no header HTTP `Content-Disposition` (típico de endpoints tipo
`.../download?id=...`), o path não carrega o nome. Caso concreto que motivou esta spec — um audiobook
`.m4a` do Google Drive:

```
https://drive.usercontent.google.com/download?id=12tYR...&export=download&confirm=t&...
```

O path é `/download`, então `QFileInfo::fileName()` devolve `"download"` e o arquivo é salvo com esse
nome, sem extensão. O nome correto (ex.: `Audiobook.m4a`) chega apenas em:

```
Content-Disposition: attachment; filename="Audiobook.m4a"
```

Header que o projeto **não lê em lugar nenhum** hoje. O `HttpProbe` (`src/core/HttpProbe.cpp`) já lê
`ETag`, `Last-Modified`, `Content-Range` e `Content-Length` da resposta, mas ignora
`Content-Disposition`.

### Decisões já tomadas (brainstorming)

- **Resolver ao colar a URL no diálogo**, não durante o download. Um probe assíncrono busca o nome e
  preenche o campo do `NewDownloadDialog` antes do usuário confirmar. Descartado: resolver só durante o
  download (nome só apareceria correto na lista, depois do fato).
- **Sem rede de segurança durante o download.** A resolução acontece apenas no diálogo. Se o probe
  falhar ali, vale o nome derivado da URL (que o usuário vê e pode editar). O caminho de download
  **não** é tocado. Descartado: re-verificar `Content-Disposition` no `SegmentWorker`/`DownloadTask`.
- **Reusar o `HttpProbe` existente** (já manda `Range: bytes=0-0` e lê headers), em vez de criar um
  probe novo. Ganha um campo `suggestedFileName`.
- **`filename*=` (RFC 5987/6266) tem prioridade sobre `filename=`** — é o formato que preserva acentos.
- **Sanitização basename-only** — descartar tudo antes de `/` ou `\` bloqueia path traversal vindo de
  servidor malicioso.
- **Gatilho por debounce (~400 ms)** enquanto o usuário digita/cola, assíncrono, sem travar a UI.
- **Escopo HTTP(S) apenas.** FTP continua derivando o nome do path (já funciona).

---

## 2. Objetivo e critérios de sucesso

**Objetivo:** ao inserir uma URL HTTP(S) cujo nome real está no `Content-Disposition`, o diálogo New
passa a exibir o nome correto (com extensão e acentos), e o arquivo é salvo com esse nome — sem que o
usuário precise renomear manualmente.

Critérios **1–8** são verificáveis por teste automatizado (QtTest); **9** é de integração e verificado
manualmente (ver §7).

### Parsing (Core, função pura)

1. `parseContentDisposition("attachment; filename=\"Audiobook.m4a\"")` → `"Audiobook.m4a"`.
2. `parseContentDisposition("attachment; filename*=UTF-8''Cora%C3%A7%C3%A3o.m4a")` → `"Coração.m4a"`
   (percent-decode com o charset declarado).
3. Com os dois presentes, o `filename*=` vence: `filename="x.m4a"; filename*=UTF-8''y.m4a` → `"y.m4a"`.
4. Sanitização basename-only: `filename="../../etc/passwd"` → `"passwd"`.
5. Caracteres inválidos de nome de arquivo (`/ \ : * ? " < > |` e controles) são removidos/neutralizados;
   header sem `filename`/`filename*` (`inline`), vazio ou malformado → **string vazia** (nunca crasha).

### Probe (Core)

6. `ProbeResult` ganha `suggestedFileName`; dado um servidor de teste que responde com
   `Content-Disposition`, `HttpProbe` preenche esse campo. Sem o header, o campo fica vazio.
7. **Regressão zero:** os testes atuais do Core seguem passando sem alteração de expectativa (o novo
   campo é aditivo).

### Diálogo (GUI)

8. No `NewDownloadDialog`: (a) quando o probe devolve um nome, o campo de nome é preenchido com ele e
   `destPath()` reflete esse nome; (b) quando o usuário editou o campo manualmente, o probe **não**
   sobrescreve e `destPath()` usa o nome editado; (c) quando o probe falha/vazio, o fallback
   `deriveFileName(url)` continua valendo — os testes existentes de `tst_gui.cpp` (linhas 89–94) passam
   sem alteração. `destPath()` passa a derivar de `m_name->text()`, não da URL.

### Integração (manual)

9. Colar a URL do Google Drive acima no diálogo New preenche o nome com o `.m4a` real; ao confirmar, o
   arquivo é salvo com esse nome na pasta de destino.

---

## 3. Arquitetura

```
NewDownloadDialog (GUI)
  │  usuário digita/cola URL
  │  QTimer debounce (~400 ms) → URL http/https válida?
  ▼
  dispara probe assíncrono ─────────►  HttpProbe (Core, reusado)
  │                                       │ Range: bytes=0-0
  │                                       │ onMetaDataChanged():
  │                                       │   rawHeader("Content-Disposition")
  │                                       ▼
  │                              parseContentDisposition()  (novo, Core, puro)
  │                                       │ filename* > filename, sanitiza
  │                                       ▼
  │  ◄─── ProbeResult { suggestedFileName, totalBytes, ... }
  │
  ├─ nome não-vazio E usuário não editou → preenche campo + mostra tamanho
  └─ vazio / erro / timeout             → mantém deriveFileName(url)
```

Peças e responsabilidades:

| Unidade | Camada | Responsabilidade | Depende de |
|---|---|---|---|
| `parseContentDisposition()` | Core (novo) | header bruto → nome limpo e seguro; sem I/O | só `QString`/Qt core |
| `HttpProbe` / `ProbeResult` | Core (estende) | ler o header, chamar o parser, expor `suggestedFileName` | `QNetworkReply`, parser |
| `NewDownloadDialog` | GUI (estende) | debounce, disparar probe, preencher campo respeitando edição | `HttpProbe`, `QNetworkAccessManager`, `deriveFileName` |

> **Nota de dependência (achado da auto-revisão):** construir um `HttpProbe` exige um
> `QNetworkAccessManager`, e o `NewDownloadDialog` não tem um hoje — o NAM vive no Core, dentro do
> `HttpTransport`, dono do `DownloadManager`. Para não acoplar a GUI aos internos do transporte do
> Core, **o diálogo passa a possuir seu próprio `QNetworkAccessManager`** (parenteado nele, um probe
> one-shot é barato). Alternativa considerada e descartada: encanar o NAM do Core até o diálogo via
> `MainWindow` — aumenta o acoplamento sem ganho. A costura fina (o diálogo cria o NAM vs. recebe um
> por injeção para teste) é resolvida em §8.

### 3.1 `parseContentDisposition()`

Assinatura sugerida (arquivo novo `src/core/ContentDisposition.{h,cpp}`; nome final pode seguir a
convenção do diretório):

```cpp
// Extrai um nome de arquivo seguro do valor de um header Content-Disposition.
// Retorna string vazia quando não há filename utilizável (o chamador então usa o fallback da URL).
QString parseContentDisposition(const QString& headerValue);
```

Algoritmo:

1. Tokenizar os parâmetros do header por `;`, respeitando aspas em `filename="..."`.
2. Se houver `filename*` (RFC 5987, formato `charset'lang'valor-percent-encoded`): separar o `charset`,
   percent-decodificar o valor com esse charset (UTF-8 no caso comum; se o charset for desconhecido,
   tratar como UTF-8) e usar como nome candidato. `filename*` tem prioridade.
3. Senão, se houver `filename=`: remover aspas envolventes e usar como candidato.
4. Senão: retornar vazio.
5. **Sanitizar o candidato (sempre):**
   - basename-only: `QFileInfo(candidate).fileName()` após normalizar `\` → `/`, descartando qualquer
     componente de path (bloqueia `../../...`);
   - remover caracteres inválidos/controle (`/ \ : * ? " < > |` e `\x00–\x1F`);
   - `trim`; se o resultado ficar vazio, retornar vazio.

Função pura, sem rede nem disco → coberta por testes unitários (critérios 1–5).

### 3.2 `HttpProbe` / `ProbeResult`

- `ProbeResult` (`src/core/DownloadTypes.h`) ganha `QString suggestedFileName;` (default vazio).
- Em `HttpProbe::onMetaDataChanged()` (`src/core/HttpProbe.cpp:22`), a leitura é **independente do
  status** (o header aparece tanto em 200 quanto em 206), então fica junto de `etag`/`lastModified`,
  **antes** do bloco `if (status == 206) … else if (status == 200) …`:
  ```cpp
  r.etag         = QString::fromUtf8(m_reply->rawHeader("ETag"));
  r.lastModified = QString::fromUtf8(m_reply->rawHeader("Last-Modified"));
  const QByteArray cd = m_reply->rawHeader("Content-Disposition");   // novo
  if (!cd.isEmpty())                                                 // novo
      r.suggestedFileName = parseContentDisposition(QString::fromUtf8(cd));
  ```
- Nenhum outro consumidor do `ProbeResult` (ex.: `DownloadTask::onProbed`) é alterado — o campo é
  aditivo e ignorado por quem não o usa. Isso preserva o critério 7 (regressão zero).

### 3.3 `NewDownloadDialog`

**Mudança estrutural (achado da auto-revisão):** hoje `m_name` é um `QLabel` somente-leitura
(`src/gui/NewDownloadDialog.h:22`) e `destPath()` **re-deriva o nome da URL** — `QDir(...).filePath(
deriveFileName(url()))` (`src/gui/NewDownloadDialog.cpp:77`), ignorando qualquer nome exibido. Para o
fluxo aprovado ("usuário vê/edita o nome"), duas coisas precisam mudar:

1. **`m_name` passa de `QLabel` para `QLineEdit`** (editável). Realiza a edição prevista no brainstorming
   e dá sentido à proteção de edição manual. É um desvio consciente do estado da Fase 2, que deixou o
   nome como rótulo somente-leitura.
2. **`destPath()` passa a usar o nome do campo**, não a re-derivar da URL:
   `QDir(m_dir->text()).filePath(m_name->text())`. Sem isso, o nome resolvido nunca chegaria ao arquivo
   — este era o bug central do rascunho.

- **Estado novo:** um `QTimer` de debounce; a instância de `HttpProbe` do probe corrente; um
  `QNetworkAccessManager` próprio do diálogo (ver nota em §3); uma flag `m_nameEditedByUser`
  (`true` quando o usuário edita o campo de nome manualmente); a URL que originou o probe em voo.
- **Gatilho:** ao mudar o texto da URL, (re)inicia o `QTimer` (~400 ms) **e** reseta
  `m_nameEditedByUser = false` (nova URL ⇒ novo nome sugerido é bem-vindo). Ao disparar, se a URL for
  http/https válida, cancela qualquer probe anterior em voo e inicia um novo. URLs FTP ou inválidas não
  disparam probe.
- **`refreshName()`** (o slot atual de `textChanged` da URL) continua preenchendo o campo com
  `deriveFileName(url())` imediatamente (fallback visível instantâneo) e atualizando o rótulo de tipo.
- **Retorno do probe (assíncrono):**
  - descartar o retorno se a URL que o originou não é mais a URL atual (probe obsoleto);
  - sucesso com `suggestedFileName` não-vazio **e** `m_nameEditedByUser == false` → escreve o nome no
    campo (`m_name->setText(...)`) e atualiza o rótulo de tipo; atualiza o tamanho exibido a partir de
    `totalBytes` quando disponível;
  - caso contrário → não mexe no campo (mantém o `deriveFileName(url)` já colocado).
- **Proteção de edição:** ao editar `m_name` manualmente, `m_nameEditedByUser = true`. O rótulo de tipo
  (`m_type`) passa a acompanhar o texto de `m_name` (via `textChanged`), já que o nome agora pode mudar
  sem a URL mudar.
- **Cuidado:** preencher `m_name` programaticamente dispara seu `textChanged`; o handler de edição não
  deve marcar `m_nameEditedByUser` nesses casos (usar um guard/flag de "atualização programática" ou
  `QSignalBlocker` ao setar o texto pelo probe/refreshName).

---

## 4. Fluxo de dados e ciclo de vida do probe

- **Concorrência de probes:** só um probe relevante por vez. Ao iniciar um novo (URL mudou), o anterior
  é abortado/descartado; um retorno de probe obsoleto (de uma URL que não é mais a atual) é ignorado —
  guardar a URL que originou o probe e comparar no retorno.
- **Timeout:** o probe tem teto curto (ex.: 5 s via `QTimer`); ao estourar, aborta e trata como
  "sem nome" (mantém o fallback). Evita o campo preso em "detectando…".
- **Estado visual:** enquanto o probe corre, o campo já mostra `deriveFileName(url)` (comportamento
  atual); opcionalmente um placeholder/label discreto "detectando nome…". Sem spinner bloqueante.
- **Ciclo de vida:** o `HttpProbe` corrente é filho do diálogo (parenteado) e destruído com ele; um
  probe em voo ao fechar o diálogo é abortado no destrutor. Nenhum sinal deve tocar o campo depois do
  diálogo fechar.

---

## 5. Tratamento de erros e casos de borda

| Situação | Comportamento |
|---|---|
| Sem rede / host inacessível | probe falha silenciosamente → fallback `deriveFileName` |
| Resposta sem `Content-Disposition` | `suggestedFileName` vazio → fallback |
| `Content-Disposition: inline` (sem filename) | parser retorna vazio → fallback |
| Header malformado | parser retorna vazio, nunca crasha → fallback |
| `filename` com path traversal | sanitizado para basename → nome seguro |
| Usuário editou o nome antes do probe voltar | edição preservada, probe não sobrescreve |
| URL trocada antes do probe voltar | retorno obsoleto ignorado (comparação por URL) |
| Timeout do probe | aborta → fallback |
| URL FTP | não dispara probe; nome vem do path (inalterado) |
| Google Drive respondendo página HTML de confirmação | **fora de escopo**; a URL do caso já traz `confirm=t`. Se o probe vier com `Content-Disposition` de HTML, o nome pode sair errado — aceito por ora, documentado aqui |

---

## 6. Testes

Seguindo TDD: os testes do parser são escritos antes da implementação dele.

**Unitários — `parseContentDisposition()` (novo `tst_contentdisposition` ou dentro de `tst_download`):**
cobre os critérios 1–5 — `filename=` com/sem aspas, `filename*=` com acentos (percent-decode), ambos
presentes (estendido vence), path traversal → basename, caracteres inválidos, header vazio/`inline`/
malformado → vazio.

**Core — `HttpProbe`:** se `tests/` já tiver infra de servidor HTTP local (o padrão do
`TestFtpServer`/`QTcpServer` mencionado na Fase 3 sugere que sim), subir um servidor que responde com
`Content-Disposition` e afirmar `ProbeResult.suggestedFileName` (critério 6). Se não houver infra HTTP
reutilizável, este ponto fica coberto pelos unitários do parser + verificação manual (critério 9), e a
criação da infra HTTP de teste **não** entra no escopo desta spec.

**GUI — `tst_gui.cpp`:** critério 8 — probe devolve nome ⇒ campo preenchido; usuário editou ⇒ não
sobrescreve; probe falha ⇒ fallback (os casos 89–94 seguem passando). Para não depender de rede, o
teste injeta o resultado do probe (ver §8) em vez de bater num servidor real.

---

## 7. Verificação manual (critério 9)

1. Compilar `orbit-gui`.
2. Abrir o diálogo New, colar a URL do Google Drive do §1.
3. Após ~1 s, o campo de nome deve exibir o `.m4a` real (não `download`).
4. Confirmar; conferir que o arquivo salvo na pasta de destino tem o nome e a extensão corretos.
5. Repetir com uma URL http comum cujo path já tem nome (ex.: `.../arquivo.zip`) e confirmar que o
   comportamento não regrediu.

---

## 8. Riscos e pontos de atenção

- **Testabilidade da GUI sem rede:** o `NewDownloadDialog` não deve instanciar um `HttpProbe` concreto
  de forma rígida se isso impedir o teste. Preferir uma costura que permita injetar o resultado do
  probe no teste (ex.: um slot público `onProbeFinished(const ProbeResult&)` que o teste chama com um
  `ProbeResult` sintético, exercendo o preenchimento do campo e a proteção de edição sem tocar a rede;
  ou receber o `QNetworkAccessManager`/uma factory de probe por injeção no construtor). A decisão fina
  fica para o plano, mas o **requisito** é: o critério 8 tem de ser testável sem rede real. O NAM
  próprio do diálogo (§3) deve ser injetável para que o teste não abra sockets.
- **Charset do `filename*`:** o comum é UTF-8. Charsets exóticos (ex.: `ISO-8859-1`) são raros; tratar
  desconhecido como UTF-8 é aceitável e não quebra o caso do Drive.
- **Latência percebida:** debounce de ~400 ms + timeout de ~5 s equilibram "nome aparece rápido" contra
  "não dispara a cada tecla". Valores podem ser ajustados no plano.

---

## 9. Fora de escopo (explícito)

- Re-verificar `Content-Disposition` durante o download (rede de segurança) — decisão do brainstorming.
- FTP (nome continua vindo do path).
- Seguir a página HTML de confirmação de vírus do Google Drive para arquivos grandes.
- Qualquer persistência de configuração.

---

## 10. Correções aplicadas na auto-revisão

Gaps encontrados conferindo o código real e já corrigidos acima. Registrados aqui para revisão
consciente — o item 1 é uma decisão de UX que vale um sign-off explícito:

1. **`m_name` vira `QLineEdit` (era `QLabel` somente-leitura).** O fluxo aprovado pressupõe que o
   usuário vê **e edita** o nome; o widget atual não permite editar. É um desvio consciente do estado da
   Fase 2. *Se você preferir manter o nome não-editável (só auto-preenchido, sem edição manual), diga —
   aí a proteção de edição sai do escopo e o campo continua um rótulo.*
2. **`destPath()` deixa de re-derivar da URL** e passa a usar o nome do campo
   (`m_name->text()`). Sem isso, o nome resolvido nunca chegaria ao arquivo salvo — era o bug central do
   rascunho, que afirmava (errado) que `destPath()` não mudaria.
3. **Dependência do `QNetworkAccessManager`:** o diálogo não tinha um; agora possui o seu (injetável
   para teste), evitando acoplar a GUI aos internos do `HttpTransport` do Core.
4. **Leitura do `Content-Disposition` independente do status HTTP** (200 e 206), posicionada antes do
   bloco de status no `HttpProbe`.
5. **Guard contra falso-positivo de edição:** setar o nome programaticamente (probe/`refreshName`)
   dispara `textChanged`; o handler não deve marcar "editado pelo usuário" nesses casos.
