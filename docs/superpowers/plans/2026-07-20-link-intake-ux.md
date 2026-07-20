# Link-Intake UX Improvements — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the browser extension retry on transient failures, make received links open the New dialog while the download runs in the background (retargeting live on confirm), and make the New dialog layout responsive.

**Architecture:** A new headless-testable core method `DownloadManager::retarget` moves+renames a partial download (pausing/resuming an active one). The GUI gains a `receiveLink` flow (create background task → foreground dialog → `reconcileReceivedLink`) reused by extension/clipboard/drag. The extension gets a bounded retry-with-backoff in `handoff()`. The New dialog gets `AllNonFixedFieldsGrow`.

**Tech Stack:** C++/Qt6 (6.11.1), Qt Widgets, QtTest, CMake; MV3 JavaScript (extension).

## Global Constraints

- **Do not commit without the user's explicit authorization.** Leave all work in the working tree; commits happen later in a batch. The controller captures per-task snapshots for review — implementers must NOT run `git commit`.
- Commit messages (when eventually made): English, Conventional Commits, no co-authorship trailer.
- `retarget` models on the existing `DownloadManager::moveFiles` (`src/core/DownloadManager.cpp:202-223`) but changes dir AND basename, pauses/resumes an active download, and handles Completed / no-bytes-yet.
- Extension retry: bounded (3 attempts, ~500/1500/4500 ms), retried ONLY on a thrown `fetch` (unreachable); a reachable HTTP response (ok or refused) is not retried. Parameters in named constants. Keep total window a few seconds (MV3 worker suspension) and document the caveat.
- `deriveFileName(url)` comes from `UrlName.h` (already used in `MainWindow.cpp`).
- Cancel / changed-URL fully removes the background task via `m_mgr->remove(id, /*deleteFiles=*/true)` + `m_model->removeTaskById(id)` (cleaner than leaving a `Cancelled` row — a deliberate refinement over the spec's `cancel(id)` wording).
- `NewDownloadDialog::exec()` is modal and cannot run headless; GUI tests target the non-modal seams (`beginBackgroundLink` / `reconcileReceivedLink`) via test hooks, mirroring the existing `emitBrowserDownloadForTest` pattern.

---

### Task 1: Core `retarget` + provisional-name accessors

**Files:**
- Modify: `src/core/DownloadManager.h` (declare `retarget`)
- Modify: `src/core/DownloadManager.cpp` (implement `retarget`)
- Modify: `src/core/DownloadTask.h` (add `clearProvisionalName()` + `provisionalName()` getter)
- Modify: `src/core/DownloadTask.cpp` (implement the two)
- Test: `tests/tst_download.cpp`

**Interfaces:**
- Produces: `bool DownloadManager::retarget(const QUuid& id, const QString& newDestPath);`
- Produces: `void DownloadTask::clearProvisionalName();` and `bool DownloadTask::provisionalName() const;`
- Consumes (existing): `taskById`, `pause`, `resume`, `saveSession`, `Persistence::resolveUniquePath/metaPath`, `DownloadTask::setDestPath`, `record().destPath`.

- [ ] **Step 1: Write the failing tests**

Add these slots to `tests/tst_download.cpp` (next to `provisionalNameAdoptsContentDisposition`; they reuse the file's existing `TestServer`, `waitForState`, `m_body`, and `makeBody`). Add `#include <QDir>`/`#include <QFileInfo>` only if not already present.

```cpp
    void retargetOnCompletedMovesAndRenames() {
        TestServer srv(m_body);
        QVERIFY(srv.listen());
        QTemporaryDir dir;
        DownloadManager mgr(EngineConfig{}, dir.path());
        const QUuid id = mgr.addDownload(srv.url("/ranged"), dir.filePath("orig.bin"));
        QVERIFY(waitForState(mgr, id, DownloadState::Completed, 5000));
        QTemporaryDir dir2;
        const QString newPath = QDir(dir2.path()).filePath("renamed.bin");
        QVERIFY(mgr.retarget(id, newPath));
        QCOMPARE(mgr.taskById(id)->record().destPath, newPath);
        QVERIFY(QFile::exists(newPath));
        QVERIFY(!QFile::exists(dir.filePath("orig.bin")));
        QFile f(newPath); QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(f.readAll(), m_body);
    }
    void retargetNoOpWhenSamePath() {
        TestServer srv(m_body);
        QVERIFY(srv.listen());
        QTemporaryDir dir;
        DownloadManager mgr(EngineConfig{}, dir.path());
        const QString p = dir.filePath("keep.bin");
        const QUuid id = mgr.addDownload(srv.url("/ranged"), p);
        QVERIFY(waitForState(mgr, id, DownloadState::Completed, 5000));
        QVERIFY(mgr.retarget(id, p));                       // same path -> true, undisturbed
        QCOMPARE(mgr.taskById(id)->record().destPath, p);
        QVERIFY(QFile::exists(p));
    }
    void retargetResolvesUniqueOnCollision() {
        TestServer srv(m_body);
        QVERIFY(srv.listen());
        QTemporaryDir dir;
        DownloadManager mgr(EngineConfig{}, dir.path());
        const QUuid id = mgr.addDownload(srv.url("/ranged"), dir.filePath("a.bin"));
        QVERIFY(waitForState(mgr, id, DownloadState::Completed, 5000));
        QTemporaryDir dir2;
        const QString taken = QDir(dir2.path()).filePath("b.bin");
        { QFile pre(taken); QVERIFY(pre.open(QIODevice::WriteOnly)); pre.write("x"); }
        QVERIFY(mgr.retarget(id, taken));                   // collides -> "b (1).bin"
        const QString got = mgr.taskById(id)->record().destPath;
        QVERIFY(got != taken);
        QVERIFY(QFile::exists(got));
    }
    void retargetActiveDownloadPreservesBytes() {
        SKIP_IF_CI_TIMING();                                // mid-flight observation; see issue #1
        const QByteArray big = makeBody(2 * 1024 * 1024);
        TestServer srv(big);
        QVERIFY(srv.listen());
        QTemporaryDir dir;
        DownloadManager mgr(EngineConfig{}, dir.path());
        const QUuid id = mgr.addDownload(srv.url("/ranged"), dir.filePath("live.bin"));
        QVERIFY(waitForState(mgr, id, DownloadState::Downloading, 5000));
        QTemporaryDir dir2;
        const QString newPath = QDir(dir2.path()).filePath("moved.bin");
        QVERIFY(mgr.retarget(id, newPath));                 // pauses, moves partial+.meta, resumes
        QVERIFY(waitForState(mgr, id, DownloadState::Completed, 10000));
        QCOMPARE(mgr.taskById(id)->record().destPath, newPath);
        QFile f(newPath); QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(f.readAll(), big);
        QVERIFY(!QFile::exists(dir.filePath("live.bin")));
    }
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build build --target tst_download && ctest --test-dir build -R tst_download -V`
Expected: FAIL to compile (`retarget` not declared).

- [ ] **Step 3: Declare and implement `retarget`**

In `src/core/DownloadManager.h`, after the `moveFiles` declaration (line 32):

```cpp
    bool  retarget(const QUuid& id, const QString& newDestPath);
```

In `src/core/DownloadManager.cpp`, after `moveFiles` (after line 223):

```cpp
// Retarget a download to a new FULL path (directory and/or basename), preserving
// bytes already fetched. Unlike moveFiles (dir-only, keeps basename, refuses while
// active), retarget pauses an active download, renames+moves the partial + .meta,
// updates the path, and resumes. Safe for Completed/Paused/Queued (pause/resume are
// no-ops there). Returns false on IO failure, leaving the task resumed at old path.
bool DownloadManager::retarget(const QUuid& id, const QString& newDestPath) {
    DownloadTask* t = taskById(id);
    if (!t) return false;
    const QString oldPath = t->record().destPath;
    if (newDestPath == oldPath) return true;               // no change requested
    const QString finalPath = Persistence::resolveUniquePath(newDestPath);
    const DownloadState s = t->state();
    const bool wasActive = (s == DownloadState::Downloading || s == DownloadState::Connecting);
    if (wasActive) pause(id);                              // safe hold; stops workers/writes
    if (QFileInfo::exists(oldPath) && !QFile::rename(oldPath, finalPath)) {
        if (wasActive) resume(id);
        return false;
    }
    const QString oldMeta = Persistence::metaPath(oldPath);
    if (QFileInfo::exists(oldMeta))
        QFile::rename(oldMeta, Persistence::metaPath(finalPath));
    t->setDestPath(finalPath);
    saveSession();
    if (wasActive) resume(id);
    return true;
}
```

Confirm `Persistence.h` is already included in `DownloadManager.cpp` (it is — used by `moveFiles`). No new include needed.

- [ ] **Step 4: Add the provisional-name accessors**

In `src/core/DownloadTask.h`, in the public section (near `setDestPath`, line 26):

```cpp
    void clearProvisionalName();               // user confirmed the name; stop CD override
    bool provisionalName() const { return m_provisionalName; }
```

In `src/core/DownloadTask.cpp`, next to `setDestPath` (after line 304):

```cpp
void DownloadTask::clearProvisionalName() {
    m_provisionalName = false;
}
```

- [ ] **Step 5: Run the tests to verify they pass**

Run: `cmake --build build --target tst_download && ctest --test-dir build -R tst_download -V`
Expected: PASS. `retargetActiveDownloadPreservesBytes` runs locally; on CI with `ORBIT_SKIP_TIMING_TESTS` it skips. The other three run everywhere.

- [ ] **Step 6: Commit** (ask for authorization first — leave in working tree otherwise)

```bash
git add src/core/DownloadManager.h src/core/DownloadManager.cpp src/core/DownloadTask.h src/core/DownloadTask.cpp tests/tst_download.cpp
git commit -m "feat(core): add DownloadManager::retarget to rename+move a partial download"
```

---

### Task 2: Extension retry with backoff

**Files:**
- Modify: `extension/chrome/background.js` (`handoff`)

**Interfaces:**
- Self-contained; no app-side change.

- [ ] **Step 1: Verify the current file parses**

Run: `node --check extension/chrome/background.js`
Expected: exits 0 (baseline).

- [ ] **Step 2: Add retry constants and a sleep helper**

In `extension/chrome/background.js`, near the top (after the `DEFAULTS` line, line 14):

```javascript
// Retry the handoff a few times before falling back, so a download doesn't
// escape to the browser just because Orbit was briefly unreachable (e.g. a
// restart). Bounded and short on purpose: MV3 service workers can be suspended
// during a long await, so this is best-effort, not a durable queue — a worker
// killed mid-retry still falls back.
const HANDOFF_MAX_ATTEMPTS = 3;
const HANDOFF_BACKOFF_MS = [500, 1500, 4500];   // delay BEFORE attempt i (i>0)
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
```

- [ ] **Step 3: Rewrite `handoff` to retry on unreachable**

Replace the whole `handoff` function (lines 71-96) with:

```javascript
async function handoff(url, referrer, filename) {
  const payload = {
    url,
    filename,
    referrer,
    userAgent: navigator.userAgent,
    cookie: await cookieHeader(url),
  };
  for (let attempt = 0; attempt < HANDOFF_MAX_ATTEMPTS; attempt++) {
    if (attempt > 0) await sleep(HANDOFF_BACKOFF_MS[attempt - 1]);
    try {
      const resp = await fetch(`http://127.0.0.1:${cfg.port}/add`, {
        method: "POST",
        headers: { "Content-Type": "application/json", "X-Orbit-Token": cfg.token },
        body: JSON.stringify(payload),
      });
      // Reachable: the connection works, so don't retry regardless of status.
      if (resp.ok) { notify("Sent to Orbit"); return; }
      notify("Orbit refused the download — falling back to the browser");
      break;
    } catch (e) {
      // Unreachable (app down / connection refused): retry with backoff.
      if (attempt < HANDOFF_MAX_ATTEMPTS - 1) continue;
      notify("Orbit not reachable — falling back to the browser");
    }
  }

  // Fail-safe: we already cancelled, so re-issue the download ourselves. No
  // `saveAs`, so no dialog. Mark it passthrough so onCreated doesn't re-grab it.
  passthrough.add(url);
  chrome.downloads.download({ url }).catch(() => {});
  setTimeout(() => passthrough.delete(url), 10000);
}
```

- [ ] **Step 4: Verify it parses and review the control flow**

Run: `node --check extension/chrome/background.js`
Expected: exits 0. Re-read the function: a thrown fetch on attempts 0/1 → `continue` (retry after backoff); on the last attempt → notify + fall through to fail-safe. A reachable `resp` (ok or not) never retries. There is no JS test runner in this repo, so this is verified by `node --check` + manual E2E (stop app → trigger download → confirm it retries and lands once the app is back within the window).

- [ ] **Step 5: Commit** (ask for authorization first)

```bash
git add extension/chrome/background.js
git commit -m "feat(extension): retry handoff with backoff before browser fallback"
```

---

### Task 3: Responsive New dialog layout

**Files:**
- Modify: `src/gui/NewDownloadDialog.cpp` (form growth policy + min width)
- Test: `tests/tst_gui.cpp`

**Interfaces:**
- No API change; a test reads `QFormLayout::fieldGrowthPolicy()`.

- [ ] **Step 1: Write the failing test**

Add to `tests/tst_gui.cpp` (near the other dialog tests). It finds the dialog's `QFormLayout` and asserts the growth policy. Add `#include <QFormLayout>` to the test includes if absent.

```cpp
    void newDialogFieldsGrow() {
        NewDownloadDialog d(nullptr, QUrl("https://h/x.bin"));
        auto* form = d.findChild<QFormLayout*>();
        QVERIFY(form != nullptr);
        QCOMPARE(form->fieldGrowthPolicy(), QFormLayout::AllNonFixedFieldsGrow);
        QVERIFY(d.minimumWidth() >= 480);
    }
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build build --target tst_gui && ctest --test-dir build -R tst_gui -V`
Expected: FAIL — default policy is not `AllNonFixedFieldsGrow` and `minimumWidth()` is < 480.

- [ ] **Step 3: Set the growth policy and minimum width**

In `src/gui/NewDownloadDialog.cpp`, right after `QFormLayout* form = new QFormLayout(this);` (line 63):

```cpp
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);   // fields expand with the window
    setMinimumWidth(480);
```

- [ ] **Step 4: Run the test to verify it passes**

Run: `cmake --build build --target tst_gui && ctest --test-dir build -R tst_gui -V`
Expected: PASS.

- [ ] **Step 5: Commit** (ask for authorization first)

```bash
git add src/gui/NewDownloadDialog.cpp tests/tst_gui.cpp
git commit -m "fix(new-dialog): let fields grow with the window"
```

---

### Task 4: `receiveLink` flow + background download + reconcile

**Files:**
- Modify: `src/gui/MainWindow.h` (declare `receiveLink`, `beginBackgroundLink`, `reconcileReceivedLink`, `m_dialogOpen`, test hooks)
- Modify: `src/gui/MainWindow.cpp` (implement + rewire extension/clipboard/drag)
- Test: `tests/tst_gui.cpp`

**Interfaces:**
- Consumes: `DownloadManager::retarget`, `DownloadTask::clearProvisionalName/provisionalName` (Task 1); existing `addDownload`, `remove`, `taskById`, `DownloadTableModel::appendTask/removeTaskById`.
- Produces (private): `QUuid beginBackgroundLink(const QUrl&, const HeaderList&);`, `void receiveLink(const QUrl&, const HeaderList&);`, `void reconcileReceivedLink(const QUuid&, const QUrl& origUrl, const HeaderList&, bool accepted, const QUrl& chosenUrl, const QString& chosenDest);`, member `bool m_dialogOpen = false;`
- Produces (test hooks): `beginBackgroundLinkForTest`, `reconcileReceivedLinkForTest`, `setDialogOpenForTest`.

- [ ] **Step 1: Write the failing tests**

Add to `tests/tst_gui.cpp` (near `browserDownloadEnqueuesWithHeaders`). These use fake URLs (no server): the task is created but never actually fetches, so `retarget` with no file on disk just updates the path — enough to verify the decision logic.

```cpp
    void reconcileAcceptChangedDestRetargets() {
        QTemporaryDir dir;
        DownloadManager mgr(EngineConfig{}, dir.path());
        DownloadTableModel model(&mgr);
        MainWindow w(&mgr, &model, nullptr);
        const QUrl url("https://h/file.bin");
        const QUuid id = w.beginBackgroundLinkForTest(url, {});
        const QString newDest = QDir(dir.path()).filePath("chosen.bin");
        w.reconcileReceivedLinkForTest(id, url, {}, /*accepted=*/true, url, newDest);
        QCOMPARE(mgr.taskById(id)->record().destPath, newDest);
        QVERIFY(!mgr.taskById(id)->provisionalName());          // cleared on confirm
    }
    void reconcileCancelDiscardsTask() {
        QTemporaryDir dir;
        DownloadManager mgr(EngineConfig{}, dir.path());
        DownloadTableModel model(&mgr);
        MainWindow w(&mgr, &model, nullptr);
        const QUrl url("https://h/file.bin");
        const int before = model.rowCount();
        const QUuid id = w.beginBackgroundLinkForTest(url, {});
        QCOMPARE(model.rowCount(), before + 1);
        w.reconcileReceivedLinkForTest(id, url, {}, /*accepted=*/false, url, QString());
        QVERIFY(mgr.taskById(id) == nullptr);                  // fully removed
        QCOMPARE(model.rowCount(), before);
    }
    void reconcileChangedUrlRestarts() {
        QTemporaryDir dir;
        DownloadManager mgr(EngineConfig{}, dir.path());
        DownloadTableModel model(&mgr);
        MainWindow w(&mgr, &model, nullptr);
        const QUrl urlA("https://h/a.bin");
        const QUrl urlB("https://h/b.bin");
        const QUuid id = w.beginBackgroundLinkForTest(urlA, {});
        const QString destB = QDir(dir.path()).filePath("b.bin");
        w.reconcileReceivedLinkForTest(id, urlA, {}, /*accepted=*/true, urlB, destB);
        QVERIFY(mgr.taskById(id) == nullptr);                  // original discarded
        const auto tasks = mgr.tasks();
        QVERIFY(!tasks.isEmpty());
        QCOMPARE(tasks.last()->record().url, urlB);            // restarted for the new URL
    }
    void secondLinkWhileDialogOpenEnqueuesDirectly() {
        QTemporaryDir dir;
        DownloadManager mgr(EngineConfig{}, dir.path());
        DownloadTableModel model(&mgr);
        MainWindow w(&mgr, &model, nullptr);
        w.setDialogOpenForTest(true);                          // simulate a New dialog already up
        const int before = model.rowCount();
        w.emitBrowserDownloadForTest(QUrl("https://h/big.iso"), {}, "big.iso");
        QCOMPARE(model.rowCount(), before + 1);               // background-enqueued, no modal
    }
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build build --target tst_gui && ctest --test-dir build -R tst_gui -V`
Expected: FAIL to compile (`beginBackgroundLinkForTest` / `reconcileReceivedLinkForTest` / `setDialogOpenForTest` not declared).

- [ ] **Step 3: Declare the new methods, member, and hooks in `MainWindow.h`**

In the private section (after `void enqueue(...)`, line 79):

```cpp
    QUuid beginBackgroundLink(const QUrl& url, const HeaderList& headers);
    void  receiveLink(const QUrl& url, const HeaderList& headers);
    void  reconcileReceivedLink(const QUuid& id, const QUrl& origUrl, const HeaderList& headers,
                                bool accepted, const QUrl& chosenUrl, const QString& chosenDest);
```

Add the member near `m_lastDir` (line 95):

```cpp
    bool                 m_dialogOpen = false;   // guard: don't stack New dialogs
```

Add the test hooks in the public test-hooks block (near `emitBrowserDownloadForTest`, line 43):

```cpp
    QUuid beginBackgroundLinkForTest(const QUrl& url, const HeaderList& h) {
        return beginBackgroundLink(url, h);
    }
    void  reconcileReceivedLinkForTest(const QUuid& id, const QUrl& origUrl, const HeaderList& h,
                                       bool accepted, const QUrl& chosenUrl, const QString& chosenDest) {
        reconcileReceivedLink(id, origUrl, h, accepted, chosenUrl, chosenDest);
    }
    void  setDialogOpenForTest(bool v) { m_dialogOpen = v; }
```

Ensure `QUuid` and `HeaderList` are visible in the header (they already are — used by `onBrowserDownload`'s declaration).

- [ ] **Step 4: Implement the flow in `MainWindow.cpp`**

Add these functions (place near `onBrowserDownload`). `deriveFileName` (UrlName.h) and `NewDownloadDialog` are already included in this file.

```cpp
// Create the background download for a received link and show it in the list,
// so it starts fetching immediately while the user configures the New dialog.
QUuid MainWindow::beginBackgroundLink(const QUrl& url, const HeaderList& headers) {
    const QString provDest = QDir(defaultDir()).filePath(deriveFileName(url));
    const QUuid id = m_mgr->addDownload(url, provDest, headers, /*provisionalName=*/true);
    if (id.isNull()) return {};                          // unsupported scheme
    m_model->appendTask(m_mgr->taskById(id));
    return id;
}

// A link arrived with a URL (extension / clipboard-Ask / single drag): start it
// in the background and bring up the New dialog to confirm name/folder. On OK the
// running download is retargeted live (bytes preserved); on Cancel it's discarded.
void MainWindow::receiveLink(const QUrl& url, const HeaderList& headers) {
    const QUuid id = beginBackgroundLink(url, headers);
    if (id.isNull()) return;
    if (m_dialogOpen) {                                  // don't stack dialogs
        if (m_tray)
            m_tray->showMessage(tr("Orbit"),
                tr("New download from link: %1").arg(deriveFileName(url)),
                QSystemTrayIcon::Information, 5000);
        return;
    }
    raise();
    activateWindow();
    NewDownloadDialog d(this, url);
    m_dialogOpen = true;
    const bool accepted = (d.exec() == QDialog::Accepted);
    m_dialogOpen = false;
    reconcileReceivedLink(id, url, headers, accepted, d.url(), d.destPath());
}

// Apply the user's decision to the already-running background download.
void MainWindow::reconcileReceivedLink(const QUuid& id, const QUrl& origUrl,
                                       const HeaderList& headers, bool accepted,
                                       const QUrl& chosenUrl, const QString& chosenDest) {
    if (!accepted) {                                     // Cancel: discard fully + delete partial
        m_mgr->remove(id, /*deleteFiles=*/true);
        m_model->removeTaskById(id);
        return;
    }
    if (chosenUrl != origUrl) {                          // URL changed: can't reuse bytes -> restart
        m_mgr->remove(id, /*deleteFiles=*/true);
        m_model->removeTaskById(id);
        const QUuid nid = m_mgr->addDownload(chosenUrl, chosenDest, headers, /*provisionalName=*/false);
        if (!nid.isNull()) m_model->appendTask(m_mgr->taskById(nid));
        return;
    }
    DownloadTask* t = m_mgr->taskById(id);
    if (!t) return;
    m_lastDir = QFileInfo(chosenDest).absolutePath();    // remember chosen folder this session
    if (chosenDest != t->record().destPath)
        m_mgr->retarget(id, chosenDest);
    t->clearProvisionalName();                           // user confirmed the name
}
```

- [ ] **Step 5: Rewire the entry points**

Replace `onBrowserDownload`'s body (lines 561-577) with a delegation (keep the doc comment above it):

```cpp
void MainWindow::onBrowserDownload(const QUrl& url, const HeaderList& headers,
                                   const QString& /*filename*/) {
    // The extension-supplied filename is now derived inside beginBackgroundLink
    // (via deriveFileName) and can be overridden by the New dialog; the browser
    // filename is no longer trusted as the destination basename.
    receiveLink(url, headers);
}
```

In `addUrlViaDialog` — leave it as-is for manual `onNew()` (empty prefill, dialog-first). It is still used by `onNew()`. Do NOT route `onNew` through `receiveLink` (no URL to background-download).

Rewire the received-link entry points:
- `onClipboardUrl` (line 481-483): `case ClipboardMode::Ask:` → `receiveLink(url, {}); return;`
- `showLinkNotification`'s `linkActivated` lambda (line 514): `addUrlViaDialog(url);` → `receiveLink(url, {});`
- `dropEvent` single URL (line 294): `if (urls.size() == 1) { receiveLink(urls.first(), {}); return; }`

Leave unchanged: `onClipboardUrl` Auto (`enqueue`), drop of multiple URLs (`enqueue` loop), `onNew`.

Update `emitBrowserDownloadForTest` so it stays headless-safe (its current body calls `onBrowserDownload`, which now opens a modal dialog). In `MainWindow.h`, change it to force the no-dialog branch:

```cpp
    void emitBrowserDownloadForTest(const QUrl& url, const HeaderList& headers,
                                    const QString& filename) {
        const bool prev = m_dialogOpen;
        m_dialogOpen = true;                 // headless: take the background+tray branch (no exec)
        onBrowserDownload(url, headers, filename);
        m_dialogOpen = prev;
    }
```

(If `emitBrowserDownloadForTest` is currently defined inline across `MainWindow.h:43-46`, replace that definition; keep the same signature so existing callers compile.)

- [ ] **Step 6: Run the tests to verify they pass**

Run: `cmake --build build --target tst_gui && ctest --test-dir build -R tst_gui -V`
Expected: PASS, including the pre-existing `browserDownloadEnqueuesWithHeaders` (still sees a task created carrying the headers, now via the background+tray branch).

- [ ] **Step 7: Build the app and full suite**

Run: `cmake --build build --target orbit-gui && ctest --test-dir build --output-on-failure`
Expected: orbit-gui links; full suite green (timing-flaky tests may skip via `ORBIT_SKIP_TIMING_TESTS`).

- [ ] **Step 8: Commit** (ask for authorization first)

```bash
git add src/gui/MainWindow.h src/gui/MainWindow.cpp tests/tst_gui.cpp
git commit -m "feat(gui): open New dialog for received links with background download and live retarget"
```

---

## Self-Review Notes

- **Spec coverage:** Part A retry-backoff (Task 2) ✓; Part B responsive layout (Task 3) ✓; Part C.1 `retarget` (Task 1) ✓; Part C.2 `receiveLink`/`reconcileReceivedLink`/`clearProvisionalName` + wiring for extension/clipboard-Ask/Notify/single-drag (Task 4) ✓; `onNew` unchanged ✓; multi-link guard ✓.
- **Refinement over spec:** Cancel / changed-URL uses `remove(id, true)` + `removeTaskById` (fully drops the background task + deletes partial) instead of `cancel(id)` (which would leave a `Cancelled` row) — cleaner UX, noted in Global Constraints.
- **Type consistency:** `retarget(QUuid, QString)→bool`, `beginBackgroundLink(QUrl, HeaderList)→QUuid`, `reconcileReceivedLink(QUuid, QUrl, HeaderList, bool, QUrl, QString)`, `provisionalName()→bool`/`clearProvisionalName()` used identically across Tasks 1 and 4.
- **Headless testing:** GUI tests avoid `exec()` by driving `beginBackgroundLink`/`reconcileReceivedLink` via hooks and by forcing `m_dialogOpen` for the browser-download hook; the modal path itself is manual-E2E only.
- **Known minor:** `NewDownloadDialog` still defaults its folder field to Downloads (not `defaultDir()`); if they differ, `reconcileReceivedLink` retargets to the dialog's choice — correct result, possibly one extra move. Passing `defaultDir()` into the dialog is a deferred polish, out of scope here.
