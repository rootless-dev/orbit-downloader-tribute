# Orbit Downloader Tribute

A **C++20 / Qt 6** reimplementation of the classic **Orbit Downloader** download manager
(Windows XP), reproducing the original's features and GUI layout: toolbar, category tree,
download table, and the signature **colored block grid** that shows the progress of each
file segment.

It is an independent tribute/reimplementation — not affiliated with the original Orbit Downloader.

> **Notice:** personal project under active development. macOS is the primary platform; the code is
> portable (Qt) to Windows/Linux in future phases.

---

## What it does today

- **Multi-segment downloads** (the classic accelerator) for **HTTP/HTTPS** and **FTP**, with
  pause/resume that survives closing the app.
- **Automatic filename detection** from the `Content-Disposition` header — files from URLs like
  `.../download?id=...` (e.g. Google Drive) are saved with the correct name and extension, not as
  `download`.
- **Configurable User-Agent** (default `curl/8.7.1`) — unblocks servers that **reject** the
  browser User-Agent (e.g. some Cloudflare-protected links) but allow curl-like clients.
- **Global bandwidth limit** — a single shared speed cap across all downloads.
- **Scheduler:** start/pause the queue on a schedule (**daily** or **one-time** recurrence) and,
  optionally, **quit the app** when everything finishes — its own dialog with a toolbar button.
- **Preferences** (**General** + **Advanced** tabs): concurrent downloads, segments per download,
  maximum speed, default folder, clipboard-monitor mode, User-Agent, and advanced tuning
  (timeouts, retries, backoff…).
- **Configuration persisted in `settings.json`** — preferences, clipboard mode, and the default
  folder survive closing the app.
- **Orbit-style GUI:** menu bar (**File / Edit / View / Tools / Help**, native on macOS),
  toolbar (New, Start, Pause, Delete, Scheduler, Preferences…), category tree
  (Downloading / Completed / Movie / Software / Music / Others), download table, and
  **Log / Progress / Properties** tabs with the per-segment block grid.
- **Ways to add downloads:** New dialog (paste a URL), clipboard monitor, drag & drop of links
  onto the window, and a **browser extension** (Chrome/Chromium) that **intercepts** downloads and
  hands them to the app (see [Browser integration](#browser-integration)).

### In progress / upcoming phases

- **P2P/P2SP:** out of scope **for now**, but planned to arrive soon.
- **Assisted extension install** — today it is loaded manually as "unpacked".

Out of scope (likely permanent): streaming capture.

---

## Architecture at a glance

| Component | What it is |
|---|---|
| `orbitcore` | Static library for the **download engine** (HTTP + FTP), with no GUI dependency — testable in isolation (headless). |
| `orbit-gui` | The QtWidgets application, built on top of `orbitcore`. |
| `orbit-cli` | A command-line driver used for end-to-end testing of the engine. |

All networking runs on Qt's **main event loop** (asynchronous I/O, no threads at the foundation):
HTTP/HTTPS via `QNetworkAccessManager`, FTP via `QTcpSocket`. HTTP and FTP share the same state
machine (segmentation, resume, concurrency cap) through a `Transport` abstraction.

---

## Browser integration

Orbit ships a **Chrome/Chromium extension** that **intercepts browser downloads** and hands them to
the application. The extension cancels the download in the browser **before** Chrome opens the "Save
as" dialog, and Orbit takes over — reusing the **session cookies** (logged-in downloads work) and
saving with the **real name** the server reports (via `Content-Disposition`), not as `download`.

### How to enable

1. **In the Orbit app:**
   - Open **Preferences** (the **Browser** tab)
   - Check **Enable browser bridge**
   - Copy the token that appears (a unique security key)
   - Note the port (default: **8697**)

2. **In the browser:**
   - Go to `chrome://extensions`
   - Turn on **Developer mode** (top-right corner)
   - Click **Load unpacked** and select the project's `extension/chrome/` folder
   - The extension shows up with an Orbit icon
   - Right-click the extension icon → **Options**
   - Paste the token and port copied above
   - Check **Intercept downloads** and click **Save**

For build details and the extension's structure, see [`extension/chrome/README.md`](extension/chrome/README.md).

**Security note:** the endpoint listens only on `127.0.0.1` (loopback) and requires a **token** — web
pages and other extensions cannot inject downloads. (A local process running as the same user can
read the token from `settings.json`; that is outside the threat model — it is the same trust boundary
as the app itself.)

**Note (MV3):** the extension's *service worker* is non-persistent. On the **very first** download
right after the browser wakes the worker, the "Save as" dialog may appear once; subsequent downloads
intercept without a dialog.

### Browser integration — manual E2E

The following tests **cannot be automated** and should be run manually once per release:

1. Load the unpacked extension; enable it in Preferences; paste the token + port into the options.
2. Download a public file → it appears in the app with the **correct name** (not `download`),
   progresses with a bar and colored blocks, a tray notification appears, and the browser **does not
   open** the "Save as" dialog.
3. Download an authenticated file (e.g. using a session cookie) → it works in the app.
4. Quit/close the app → the extension **re-downloads through the browser** (no dialog) and notifies
   "Orbit not reachable" — no download is lost.
5. Download a `blob:`/preview file → the browser handles it, the app does not interfere.

---

## Build, test, and run

> All commands are run **from the project root**:
> ```bash
> cd orbit-downloader-tribute
> ```

### Requirements

- **macOS** with [Homebrew](https://brew.sh)
- **CMake** and **Qt 6.11** installed via Homebrew (Qt lives in `/opt/homebrew`)
  ```bash
  brew install cmake qt
  ```

### Build

The project uses **CMake**, which generates everything inside the `build/` folder. There are two
steps — **configure** (only the first time, or when new files are added to the project) and **build**
(whenever the code changes):

```bash
# Step A — configure (repeat only if you get a "file/target not found" error)
cmake -S . -B build -DCMAKE_PREFIX_PATH=/opt/homebrew

# Step B — build everything
cmake --build build
```

Day to day, usually only **Step B** is needed.

### Run the tests

```bash
# All tests
ctest --test-dir build --output-on-failure
```

At the end you'll see something like `100% tests passed, 0 tests failed out of 13`.

To run a **single** test, use `-R` with its name:

```bash
ctest --test-dir build -R tst_gui --output-on-failure
```

Available suites: `tst_smoke`, `tst_segmentation`, `tst_persistence`, `tst_download`,
`tst_transport`, `tst_contentdisposition`, `tst_gui`, `tst_ftp`, `tst_settings`, `tst_ratelimiter`,
`tst_scheduler`, `tst_logger`, `tst_browserbridge`. The tests are offline (they use an in-process
HTTP/FTP/loopback server), so they don't depend on the network.

### Launch the app

```bash
./build/src/gui/orbit-gui
```

(There is also `./build/src/cli/orbit-cli` for command-line use.)

### Everyday shortcut

Build and, if it builds, run all tests at once:

```bash
cmake --build build && ctest --test-dir build --output-on-failure
```

If you ever want to start the build from scratch, just delete the `build/` folder and run **Step A**
again — no source code is lost, only the compiled output.

---

## Project structure

```
src/core/    orbitcore library (download engine, no GUI)
src/gui/     orbit-gui application (QtWidgets)
src/cli/     orbit-cli driver
tests/       test suite (QtTest) + in-process test servers
docs/        per-phase specs and implementation plans
```

---

## License

Personal project. An independent tribute/reimplementation, not affiliated with the original Orbit
Downloader or its trademark holders.
