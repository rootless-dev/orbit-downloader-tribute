// Orbit Downloader Tribute — background service worker (MV3).
//
// True interception: we cancel the browser's download in the FIRST synchronous
// statement of chrome.downloads.onCreated, BEFORE any `await`. Every `await`
// yields control back to Chrome, which then gets a chance to show its
// "Save As" dialog / download shelf entry — so awaiting config, cookies, or the
// fetch before cancelling is exactly what lets the box appear. Fast download
// managers avoid the box by cancelling immediately; we do the same.
//
// Fail-safe: because we cancel first, if Orbit can't take the download (app
// down / refused), we re-issue it ourselves via chrome.downloads.download()
// WITHOUT `saveAs` (which never prompts), so the user never loses a download.

const DEFAULTS = { enabled: false, port: 8697, token: "" };

// In-memory config so onCreated can decide + cancel synchronously (no await).
// Kept in sync with chrome.storage.local. NOTE (MV3 caveat): the service worker
// is non-persistent; right after a cold start `cfg` holds DEFAULTS until the
// async load below completes, so the very first download after the worker has
// been idle may slip through to the browser normally. Subsequent ones intercept.
let cfg = { ...DEFAULTS };
chrome.storage.local.get(DEFAULTS).then(c => { cfg = { ...cfg, ...c }; });
chrome.storage.onChanged.addListener((changes, area) => {
  if (area !== "local") return;
  for (const k in changes) cfg[k] = changes[k].newValue;
});

// URLs we re-issued ourselves (fail-safe fallback) — skip re-intercepting them
// so the re-download doesn't loop. Cleared shortly after.
const passthrough = new Set();

function pickUrl(item) { return item.finalUrl || item.url; }

function shouldIntercept(item) {
  if (!cfg.enabled || !cfg.token) return false;
  if (passthrough.has(item.url) || passthrough.has(item.finalUrl)) return false;
  return /^https?:\/\//i.test(pickUrl(item));   // http/https only; blob/data/filesystem skipped
}

function notify(msg) {
  chrome.notifications.create({
    type: "basic",
    iconUrl: chrome.runtime.getURL("icon128.png"),
    title: "Orbit",
    message: msg,
  });
}

async function cookieHeader(url) {
  try {
    const cookies = await chrome.cookies.getAll({ url });
    return cookies.map(c => `${c.name}=${c.value}`).join("; ");
  } catch (e) { return ""; }
}

chrome.downloads.onCreated.addListener((item) => {
  if (!shouldIntercept(item)) return;

  // Capture everything we need SYNCHRONOUSLY, then cancel+erase immediately —
  // before any await — so Chrome never renders its Save As box / shelf entry.
  const url = pickUrl(item);
  const referrer = item.referrer || "";
  const filename = item.filename || "";
  chrome.downloads.cancel(item.id).catch(() => {});
  chrome.downloads.erase({ id: item.id }).catch(() => {});

  // Hand off to Orbit asynchronously (cookies + POST).
  handoff(url, referrer, filename);
});

async function handoff(url, referrer, filename) {
  const payload = {
    url,
    filename,
    referrer,
    userAgent: navigator.userAgent,
    cookie: await cookieHeader(url),
  };
  try {
    const resp = await fetch(`http://127.0.0.1:${cfg.port}/add`, {
      method: "POST",
      headers: { "Content-Type": "application/json", "X-Orbit-Token": cfg.token },
      body: JSON.stringify(payload),
    });
    if (resp.ok) { notify("Sent to Orbit"); return; }
    notify("Orbit refused the download — falling back to the browser");
  } catch (e) {
    notify("Orbit not reachable — falling back to the browser");
  }

  // Fail-safe: we already cancelled, so re-issue the download ourselves. No
  // `saveAs`, so no dialog. Mark it passthrough so onCreated doesn't re-grab it.
  passthrough.add(url);
  chrome.downloads.download({ url }).catch(() => {});
  setTimeout(() => passthrough.delete(url), 10000);
}
