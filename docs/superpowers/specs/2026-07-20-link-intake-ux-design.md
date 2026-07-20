# Link-Intake UX Improvements — Design

Date: 2026-07-20
Status: Approved (pending spec review)

## Summary

Four related improvements to how the app receives and starts downloads:

1. **Extension reconnection (retry with backoff)** — the browser extension currently does a
   single `fetch` to the app and, if the app is momentarily unreachable, the download escapes
   back to the browser. Make `handoff()` retry with backoff before falling back.
2. **Received links open the New dialog** — instead of silently auto-starting, a link arriving
   from the extension / clipboard (Ask) / single-URL drag brings the window to the foreground
   and opens the New Download dialog.
3. **Background download while the dialog is open** — the download starts immediately in the
   background so that by the time the user finishes configuring, a good part is already
   fetched. On confirm, the running download is retargeted (live) to the chosen name/folder,
   preserving downloaded bytes.
4. **Responsive New dialog layout** — the dialog's fields don't grow with the window; make them
   expand.

## Background / Current State

- **Extension** (`extension/chrome/background.js`): `handoff()` does one `fetch` POST to
  `http://127.0.0.1:<port>/add` with `X-Orbit-Token`. On any failure it notifies and falls back
  to `chrome.downloads.download({url})` (the download escapes to the browser). No retry, no
  backoff, no persistent connection. MV3 service worker has no `alarms` permission.
- **App bridge** (`src/gui/BrowserBridge.cpp`): a per-request HTTP server on `127.0.0.1:<port>`;
  `POST /add` → `emit downloadRequested(...)`. Stateless per request. (Unchanged by this work.)
- **Receiving a link** (`src/gui/MainWindow.cpp`):
  - `onBrowserDownload` (extension): sanitizes the name, builds `dest` in the default folder,
    calls `addDownload(url, dest, headers, provisionalName=true)` — **enqueues and starts
    immediately, no dialog**, then shows a tray notification.
  - `addUrlViaDialog(prefill)`: opens `NewDownloadDialog` modally; on Accept calls
    `addDownload(d.url(), d.destPath())` (provisionalName **false** — user's name wins).
  - `onNew()` → `addUrlViaDialog(QUrl())` (empty, manual entry).
  - `dropEvent`: 1 URL → `addUrlViaDialog(url)`; many → `enqueue` (direct).
  - `onClipboardUrl`: Ask → `addUrlViaDialog`; Auto → `enqueue`; Notify → clickable →
    `addUrlViaDialog`; Off → nothing.
- **New dialog** (`src/gui/NewDownloadDialog.cpp`): a `QFormLayout` with URL / Save to / File /
  Type rows. It runs its own async `HttpProbe` (Range `bytes=0-0`, aborts the body) to fill the
  filename from Content-Disposition. It does **not** create a `DownloadTask` — the download is
  born only after Accept. No `setFieldGrowthPolicy` call, so fields stay at size hint (don't grow).
- **Core** (`src/core/DownloadManager.cpp`): the partial is written directly to `destPath` (no
  `.part` suffix) plus a `.meta` sidecar. Relevant API:
  - `addDownload(url, destPath, extraHeaders, provisionalName)` → `QUuid`; calls `pump()` so it
    starts if a slot is free.
  - `pause(id)` — safely holds even a `Queued` task (`DownloadTask::pause()` skips the `.meta`
    write when `m_segments` is empty).
  - `moveFiles(id, newDir)` — moves file + `.meta` to `newDir` **keeping the basename**, and
    **refuses while Downloading/Connecting**.
  - `cancel(id)` — discards partial + `.meta`, task → `Cancelled`.
  - `taskById(id)`, `setPriority`, `remove(id, deleteFiles)`.
  - `Persistence::metaPath(destPath)`, `Persistence::resolveUniquePath(destPath)`.
  - `DownloadTask::setDestPath(path)` closes/discards the open `QFile` object and swaps the
    stored path (safe only when the download is not actively writing).
- Renaming a partial's basename after it started has **no existing API** (`moveFiles` keeps the
  name). This is the one new core capability required.

## Design

### Part A — Extension retry with backoff

In `extension/chrome/background.js`, `handoff()` wraps the POST to `/add` in a bounded retry
loop instead of a single attempt:

- Up to **3 attempts** with exponential backoff: ~**500 ms → 1500 ms → 4500 ms** between tries
  (total worst case ~6.5 s including request time).
- A retry is triggered by a thrown `fetch` (app unreachable / connection refused) — i.e. the
  connection-level failures that today drop straight to the browser fallback. An HTTP response
  that is reachable-but-refused (e.g. 401/403 auth, or 200 ok) is **not** retried: reaching the
  server means the connection works, so the existing "Orbit refused" / "Sent to Orbit" handling
  applies unchanged.
- Only after all attempts throw does the fail-safe run (browser `chrome.downloads.download`
  passthrough), exactly as today.
- **MV3 caveat:** the total retry window is kept to a few seconds so the service worker is
  unlikely to be suspended mid-backoff. This is a best-effort improvement, not a durable queue;
  a worker killed mid-retry still falls back (documented in a comment).

No new permission, no new app endpoint. Retry parameters live in named constants at the top of
`background.js` (e.g. `HANDOFF_MAX_ATTEMPTS`, `HANDOFF_BACKOFF_MS`).

### Part B — Responsive New dialog layout

In `NewDownloadDialog.cpp`, after building the `QFormLayout`:

- `form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);` so the URL / Save-to / File
  fields expand with the window.
- Set a sensible `minimumWidth` on the dialog (e.g. `setMinimumWidth(480)`) so the default size
  isn't cramped. No `setFixedSize` (the dialog stays resizable).

This is independent of Parts A and C.

### Part C — Received links open the dialog + background download + live retarget

#### C.1 New core method: `retarget`

Add `bool DownloadManager::retarget(const QUuid& id, const QString& newDestPath);` to
`DownloadManager` (`.h` + `.cpp`). Semantics:

```
retarget(id, newDestPath):
  t = taskById(id); if (!t) return false
  finalPath = resolveUniquePath(newDestPath)      // avoid clobbering an existing file
  if (finalPath == t->destPath) return true        // nothing to do
  wasActive = (state == Downloading || Connecting)
  if (wasActive) pause(id)                          // safe hold; stops workers/writes
  oldPath = t->destPath
  if (QFile::exists(oldPath)) {
      if (!moveFileTo(oldPath, finalPath)) { if (wasActive) resume(id); return false }
      renameMetaIfPresent(oldPath, finalPath)       // Persistence::metaPath both sides
  }
  t->setDestPath(finalPath)
  saveSession()
  if (wasActive) resume(id)                          // continues from downloaded bytes
  return true
```

- Handles **rename + move in one step** (unlike `moveFiles`, which only changes directory).
- Works whether the task is Downloading, Connecting, Queued, Paused, or **Completed** (for
  Completed, `pause`/`resume` are effectively no-ops and it just moves the finished file).
- If no bytes exist on disk yet (task created but nothing written), it only updates the path.
- Returns `false` on IO failure, leaving the task resumed at its old path.

This is the headless-testable heart of Part C.

#### C.2 GUI flow: `receiveLink`

Introduce a single MainWindow path for a link that arrives with a URL, replacing the direct
enqueue in `onBrowserDownload` and the plain dialog in the Ask/single-drag flows:

The flow is split into a thin modal driver (`receiveLink`) and a pure-decision reconciler
(`reconcileReceivedLink`) so the reconciliation logic is unit-testable without running the modal
event loop:

```
receiveLink(url, headers):
  provDest = QDir(defaultDir()).filePath(deriveFileName(url))
  if (m_dialogOpen):                                                // don't stack dialogs
      id = addDownload(url, provDest, headers, provisionalName=true) + appendTask + tray; return
  id = addDownload(url, provDest, headers, provisionalName=true)    // starts in background
  raise(); activateWindow()                                          // bring app to foreground
  dlg = NewDownloadDialog(url, this); prefill folder/name
  m_dialogOpen = true
  accepted = (dlg.exec() == Accepted)
  m_dialogOpen = false
  reconcileReceivedLink(id, url, headers, accepted, dlg.url(), dlg.destPath())

reconcileReceivedLink(id, origUrl, headers, accepted, chosenUrl, chosenDest):
  if (!accepted): cancel(id); return                                // Cancel → discard partial + .meta
  if (chosenUrl != origUrl):                                        // user changed the URL
      cancel(id)                                                     // discard partial + .meta
      addDownload(chosenUrl, chosenDest, headers, provisionalName=false)  // restart, user's name wins
      return
  if (chosenDest != taskById(id)->destPath()): retarget(id, chosenDest)  // live retarget, keep bytes
  taskById(id)->clearProvisionalName()                              // user confirmed the name; CD must not override
```

Wiring:
- `onBrowserDownload(url, headers, filename)` → `receiveLink(url, headers)` (drop the direct
  enqueue + tray path for the single-link case; the "already-open" branch keeps the tray path).
- `onClipboardUrl` **Ask** and **Notify-click** → `receiveLink(url, {})`.
- `dropEvent` single URL → `receiveLink(url, {})`. Multiple URLs → `enqueue` each (unchanged).
- `onClipboardUrl` **Auto** and **Off** → unchanged (Auto enqueues directly; Off ignores).
- `onNew()` (manual, empty URL) → **unchanged**: dialog first, download only on Accept (no
  background download while the user is still typing a URL).

Notes:
- Because `NewDownloadDialog::exec()` runs the event loop, the background task's network events
  keep processing while the dialog is open — the download genuinely progresses. Modal is fine;
  no need to make the dialog non-modal.
- "Already open" is tracked with a simple guard flag/member on MainWindow set around `exec()`.
- `clearProvisionalName()` is a small new `DownloadTask` accessor so that, once the user has
  confirmed a name in the dialog, a late Content-Disposition probe result can't rename the file
  out from under them. (Today `provisionalName` is set only via `init`; a public clear is needed.)
- The extension carries `extraHeaders` (cookies/referer); these are passed through `addDownload`
  as today and are unaffected by retarget.

## Testing

### Core (headless, TDD) — `tests/tst_download.cpp`
- `retargetRenamesAndMovesPartial`: start a partial download, `retarget` to a different dir +
  basename; assert the partial file and `.meta` moved to the new path, `destPath` updated, and
  the download resumes and completes to the new path (bytes preserved, final file byte-identical).
- `retargetNoOpWhenSamePath`: `retarget` to the current path returns true and doesn't disturb the
  download.
- `retargetOnCompletedMovesFinishedFile`: complete a download, then `retarget`; the finished file
  ends up at the new path.
- `retargetResolvesUniqueOnCollision`: retarget onto an existing file name resolves to a unique
  path rather than clobbering.

### GUI — `tests/tst_gui.cpp`
The tests target `reconcileReceivedLink` (the pure decision path) rather than the modal
`receiveLink`, so no `exec()` is needed headless:
- `reconcileAcceptWithChangedDestRetargets`: create a background task via `addDownload`, call
  `reconcileReceivedLink(accepted=true, chosenDest≠taskDest)`; assert the task moved to the chosen
  path and its `provisionalName` is cleared.
- `reconcileAcceptUnchangedKeepsTask`: accept with the same URL/dest; assert the task is untouched
  and still at its path, `provisionalName` cleared.
- `reconcileCancelDiscardsPartial`: `accepted=false`; assert the task is cancelled and its file is
  removed.
- `reconcileChangedUrlRestarts`: accept with a different URL; assert the original task is cancelled
  and a new task exists at the chosen dest for the new URL.
- `secondLinkWhileDialogOpenEnqueuesDirectly`: with `m_dialogOpen` set (via a test hook), a
  `receiveLink` call enqueues directly and does not construct a dialog (task count grows by one,
  no dialog shown).
- Layout: assert `NewDownloadDialog`'s form uses `AllNonFixedFieldsGrow` (via
  `fieldGrowthPolicy()`), guarding Part B.

### Extension — manual + static
- `node --check extension/chrome/background.js` stays green.
- Manual E2E: stop the app, trigger a download, confirm the extension retries (visible via its
  notifications / console) and succeeds once the app is back within the retry window; if the app
  stays down, it falls back to the browser as before.

## Out of Scope

- A persistent connection / heartbeat / `chrome.alarms` health-check and a `/ping` app endpoint
  (the user chose retry-with-backoff only for now).
- Reusing downloaded bytes across a **changed URL** (a different URL discards and restarts).
- Any change to the bridge server's per-request lifecycle or CORS.
- Deduplicating a link that is already downloading (receiving the same URL twice starts a second
  task, as today).

## Files Touched

- `extension/chrome/background.js` (Part A)
- `src/gui/NewDownloadDialog.cpp` (Part B; possibly `.h` for a test hook)
- `src/core/DownloadManager.h`, `src/core/DownloadManager.cpp` (Part C.1 — `retarget`)
- `src/core/DownloadTask.h`, `src/core/DownloadTask.cpp` (Part C.2 — `clearProvisionalName`)
- `src/gui/MainWindow.h`, `src/gui/MainWindow.cpp` (Part C.2 — `receiveLink`, wiring, guard)
- `tests/tst_download.cpp` (core retarget tests)
- `tests/tst_gui.cpp` (GUI flow + layout tests)
