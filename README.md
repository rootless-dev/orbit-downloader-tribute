# Orbit Downloader Tribute

Reimplementação em **C++20 / Qt 6** do clássico gerenciador de downloads **Orbit Downloader**
(Windows XP), replicando as funcionalidades e o layout da GUI original: toolbar, árvore de categorias,
tabela de downloads e a característica **grade de blocos coloridos** que mostra o progresso de cada
segmento do arquivo.

É um tributo/reimplementação independente — não afiliado ao Orbit Downloader original.

> **Aviso:** projeto pessoal em desenvolvimento ativo. macOS é a plataforma primária; o código é
> portável (Qt) para Windows/Linux em fases futuras.

---

## O que ele faz hoje

- **Downloads multi-segmento** (acelerador clássico) para **HTTP/HTTPS** e **FTP**, com pausa/retomada
  que sobrevive ao fechamento do app.
- **Detecção automática do nome do arquivo** a partir do header `Content-Disposition` — arquivos de
  URLs tipo `.../download?id=...` (ex.: Google Drive) são salvos com o nome e a extensão corretos, não
  como `download`.
- **User-Agent configurável** (padrão `curl/8.7.1`) — destrava servidores que **bloqueiam** o
  User-Agent de navegador (ex.: alguns links protegidos por Cloudflare) e liberam clientes tipo curl.
- **Limite de banda global** — um teto único de velocidade compartilhado por todos os downloads.
- **Scheduler:** iniciar/pausar a fila por horário (recorrência **diária** ou **única**) e, opcional,
  **fechar o app** quando tudo concluir — diálogo próprio com botão no toolbar.
- **Preferences** (abas **General** + **Advanced**): downloads simultâneos, segmentos por download,
  velocidade máxima, pasta padrão, modo do monitor de clipboard, User-Agent e ajustes avançados
  (timeouts, retries, backoff…).
- **Configuração persistida em `settings.json`** — as preferências, o modo de clipboard e a pasta
  padrão sobrevivem ao fechamento do app.
- **GUI no estilo Orbit:** barra de menus (**File / Edit / View / Tools / Help**, nativa no macOS),
  toolbar (New, Start, Pause, Delete, Scheduler, Preferences…), árvore de categorias
  (Downloading / Completed / Movie / Software / Music / Others), tabela de downloads e abas
  **Log / Progress / Properties** com a grade de blocos por segmento.
- **Formas de adicionar downloads:** diálogo New (colar URL), monitor de área de transferência,
  drag & drop de links na janela e **extensão de navegador** (Chrome/Chromium) que **intercepta**
  os downloads e os entrega ao app (ver [Browser integration](#browser-integration)).

### Em construção / próximas fases

- **P2P/P2SP:** fora do escopo **por enquanto**, mas planejado para entrar em breve.
- **Instalação assistida da extensão** — hoje ela é carregada manualmente como "unpacked".

Fora de escopo (provavelmente permanente): captura de streaming.

---

## Arquitetura em uma olhada

| Componente | O que é |
|---|---|
| `orbitcore` | Biblioteca estática do **motor de download** (HTTP + FTP), sem dependência de interface gráfica — testável de forma isolada (headless). |
| `orbit-gui` | Aplicativo com a interface QtWidgets, montado sobre o `orbitcore`. |
| `orbit-cli` | Driver de linha de comando usado para testes ponta-a-ponta do motor. |

Toda a rede roda no **event loop principal** do Qt (I/O assíncrono, sem threads na fundação):
HTTP/HTTPS via `QNetworkAccessManager`, FTP via `QTcpSocket`. HTTP e FTP compartilham a mesma máquina
de estados (segmentação, resume, cap de concorrência) através de uma abstração `Transport`.

---

## Browser integration

O Orbit oferece uma **extensão do Chrome/Chromium** que **intercepta os downloads do navegador** e os entrega ao aplicativo. A extensão cancela o download no navegador **antes** de o Chrome abrir a caixa "Salvar como", e o Orbit assume — reaproveitando os **cookies da sessão** (downloads logados funcionam) e salvando com o **nome real** que o servidor informa (via `Content-Disposition`), não como `download`.

### Como habilitar

1. **No aplicativo Orbit:**
   - Abra **Preferences** (aba **Browser**)
   - Marque **Enable browser bridge**
   - Copie o token que aparecer (uma chave de segurança única)
   - Anote a porta (padrão: **8697**)

2. **No navegador:**
   - Navegue até `chrome://extensions`
   - Ative o **Developer mode** (canto superior direito)
   - Clique em **Load unpacked** e selecione a pasta `extension/chrome/` do projeto
   - A extensão aparecerá com um ícone do Orbit
   - Clique com o botão direito no ícone da extensão → **Options**
   - Cole o token e a porta copiados acima
   - Marque **Intercept downloads** e clique **Save**

Para detalhes de compilação e estrutura da extensão, veja [`extension/chrome/README.md`](extension/chrome/README.md).

**Nota de segurança:** o endpoint escuta apenas em `127.0.0.1` (loopback) e exige um **token** — páginas web e outras extensões não conseguem injetar downloads. (Um processo local rodando como o mesmo usuário pode ler o token do `settings.json`; isso está fora do modelo de ameaça, é a mesma fronteira de confiança do próprio app.)

**Nota (MV3):** o *service worker* da extensão é não-persistente. No **primeiríssimo** download logo após o navegador acordar o worker, a caixa "Salvar como" pode aparecer uma vez; os downloads seguintes interceptam sem caixa.

### Browser integration — manual E2E

Os seguintes testes **não podem ser automáticos** e devem ser executados manualmente uma vez por release:

1. Carregar a extensão unpacked; habilitar em Preferences; colar token + porta nas opções.
2. Fazer download de um arquivo público → aparece no app com **nome correto** (não `download`), progride com barra e blocos coloridos, tray notification aparece, e o navegador **não abre** a caixa "Salvar como".
3. Fazer download de um arquivo com autenticação (ex.: usando cookie de sessão) → funciona no app.
4. Sair/fechar o app → a extensão **re-baixa pelo navegador** (sem caixa) e notifica "Orbit not reachable" — nenhum download é perdido.
5. Fazer download de um `blob:`/preview file → o navegador maneja, app não interfere.

---

## Build, testes e execução

> Todos os comandos são executados **a partir da raiz do projeto**:
> ```bash
> cd orbit-downloader-tribute
> ```

### Requisitos

- **macOS** com [Homebrew](https://brew.sh)
- **CMake** e **Qt 6.11** instalados via Homebrew (Qt fica em `/opt/homebrew`)
  ```bash
  brew install cmake qt
  ```

### Compilar

O projeto usa **CMake**, que gera tudo dentro da pasta `build/`. São dois passos — **configurar**
(só na primeira vez, ou quando arquivos novos entram no projeto) e **compilar** (sempre que o código
muda):

```bash
# Passo A — configurar (repita só se der erro de "arquivo/alvo não encontrado")
cmake -S . -B build -DCMAKE_PREFIX_PATH=/opt/homebrew

# Passo B — compilar tudo
cmake --build build
```

No dia a dia, normalmente só o **Passo B** é necessário.

### Rodar os testes

```bash
# Todos os testes
ctest --test-dir build --output-on-failure
```

No fim aparece algo como `100% tests passed, 0 tests failed out of 13`.

Para rodar **um** teste específico, use `-R` com o nome:

```bash
ctest --test-dir build -R tst_gui --output-on-failure
```

Suítes disponíveis: `tst_smoke`, `tst_segmentation`, `tst_persistence`, `tst_download`,
`tst_transport`, `tst_contentdisposition`, `tst_gui`, `tst_ftp`, `tst_settings`, `tst_ratelimiter`,
`tst_scheduler`, `tst_logger`, `tst_browserbridge`. Os testes são offline (usam um servidor
HTTP/FTP/loopback em processo), então não dependem de rede.

### Abrir o aplicativo

```bash
./build/src/gui/orbit-gui
```

(Existe também `./build/src/cli/orbit-cli` para uso via linha de comando.)

### Atalho do dia a dia

Compilar e, se compilar, rodar todos os testes de uma vez:

```bash
cmake --build build && ctest --test-dir build --output-on-failure
```

Se algum dia quiser recomeçar o build do zero, basta apagar a pasta `build/` e rodar o **Passo A**
novamente — nenhum código-fonte é perdido, só o resultado compilado.

---

## Estrutura do projeto

```
src/core/    biblioteca orbitcore (motor de download, sem GUI)
src/gui/     aplicativo orbit-gui (QtWidgets)
src/cli/     driver orbit-cli
tests/       suíte de testes (QtTest) + servidores de teste em processo
docs/        specs e planos de implementação por fase
```

---

## Licença

Projeto pessoal. Tributo/reimplementação independente, não afiliado ao Orbit Downloader original nem
aos seus detentores de marca.
