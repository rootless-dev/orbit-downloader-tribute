// Orbit Downloader Tribute — background service worker (MV3).
//
// Interception strategy (mirrors fast download managers like NDM):
//
//   1. chrome.webRequest.onHeadersReceived observes EVERY response. Its most
//      important job is to WAKE the service worker EARLY — during the HTTP
//      response, before the browser creates the download. That is what closes
//      the MV3 cold-start race: an idle worker is revived here, so its async
//      config load has time to finish before downloads.onCreated fires, and our
//      synchronous cancel there always lands in time — the browser never renders
//      its "Save As" dialog / shelf entry. When a response also LOOKS like a file
//      download (Content-Disposition: attachment, or a download-y Content-Type),
//      we record the URL + the server-suggested filename for onCreated to use.
//
//   2. chrome.downloads.onCreated cancels the browser download SYNCHRONOUSLY
//      (before any await) for interceptable URLs, then hands the URL off to the
//      Orbit app over HTTP.
//
// Fail-safe: because we cancel first, if Orbit can't take the download we
// re-issue it ourselves via chrome.downloads.download({ saveAs: false }). The
// explicit saveAs:false guarantees no dialog even when the browser's "Ask where
// to save each file before downloading" setting is on.

const DEFAULTS = { enabled: false, port: 8697, token: "" };

// Retry the handoff a few times before falling back, so a download doesn't
// escape just because Orbit was briefly unreachable. 4 attempts with the three
// backoff delays below spread the retries over ~6.5 s total. Bounded on purpose:
// an MV3 worker can be suspended during a long await, so this is best-effort.
const HANDOFF_MAX_ATTEMPTS = 4;
const HANDOFF_BACKOFF_MS = [500, 1500, 4500];   // delay BEFORE attempt i (i>0)
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// In-memory config so onCreated can decide + cancel synchronously (no await).
// Kept in sync with chrome.storage.local. cfgReady resolves once the initial
// load completes, so async paths (handoff) can await the real port/token.
let cfg = { ...DEFAULTS };
const cfgReady = chrome.storage.local.get(DEFAULTS).then((c) => { cfg = { ...cfg, ...c }; });
chrome.storage.onChanged.addListener((changes, area) => {
  if (area !== "local") return;
  for (const k in changes) cfg[k] = changes[k].newValue;
});

// URLs we re-issued ourselves (fail-safe fallback) — skip re-intercepting them
// so the re-download doesn't loop. Cleared shortly after.
const passthrough = new Set();

// URLs the network layer flagged as downloads, with the server-suggested
// filename. onCreated consumes and clears these; entries also self-expire.
const pending = new Map();            // url -> { filename }
const PENDING_TTL_MS = 30000;

function pickUrl(item) { return item.finalUrl || item.url; }
function enabled() { return !!cfg.enabled && !!cfg.token; }
function isHttp(url) { return /^https?:\/\//i.test(url); }   // blob/data/filesystem skipped
function isPassthrough(item) {
  return passthrough.has(item.url) || passthrough.has(item.finalUrl);
}

// ---- Network-layer detection (early wake + filename capture) ---------------

// Content-Types that mean "download this", not "render this". Kept conservative
// so we don't flag normal page navigations (html/css/js/images render inline).
const DOWNLOAD_CT = new RegExp(
  "^(?:application/(?:octet-stream|x-msdownload|x-msdos-program|force-download|" +
  "download|zip|x-zip-compressed|x-rar-compressed|x-rar|x-7z-compressed|x-tar|" +
  "gzip|x-gzip|x-apple-diskimage)|binary/octet-stream)", "i");

function headerValue(headers, name) {
  if (!headers) return "";
  const n = name.toLowerCase();
  for (const h of headers) if (h.name.toLowerCase() === n) return h.value || "";
  return "";
}

// Parse a filename out of a Content-Disposition header. RFC 5987 filename*
// (with its charset''value encoding) wins over a plain filename= when present.
function filenameFromDisposition(cd) {
  if (!cd) return "";
  let m = /filename\*\s*=\s*[^']*''([^;]+)/i.exec(cd);
  if (m) { try { return decodeURIComponent(m[1].trim()); } catch (e) { return m[1].trim(); } }
  m = /filename\s*=\s*"([^"]*)"/i.exec(cd) || /filename\s*=\s*([^;]+)/i.exec(cd);
  return m ? m[1].trim() : "";
}

function looksLikeDownload(headers) {
  const cd = headerValue(headers, "content-disposition");
  if (/(?:^|[;\s])attachment/i.test(cd)) return { yes: true, filename: filenameFromDisposition(cd) };
  if (DOWNLOAD_CT.test(headerValue(headers, "content-type"))) return { yes: true, filename: "" };
  return { yes: false, filename: "" };
}

chrome.webRequest.onHeadersReceived.addListener(
  (details) => {
    // Merely running here wakes an idle worker during the response — the key to
    // beating the cold-start race. We only MARK the URL (never cancel; that
    // happens in onCreated). Marking is gated on `enabled()`; when the worker was
    // asleep, cfg may not be loaded for this very first event, but the wake it
    // triggers lets cfg finish loading before onCreated fires.
    if (!enabled() || !isHttp(details.url)) return;
    const d = looksLikeDownload(details.responseHeaders);
    if (!d.yes) return;
    pending.set(details.url, { filename: d.filename });
    setTimeout(() => pending.delete(details.url), PENDING_TTL_MS);
  },
  { urls: ["<all_urls>"],
    types: ["main_frame", "sub_frame", "xmlhttprequest", "media", "object", "other", "image"] },
  ["responseHeaders"]
);

// ---- Download-layer interception (synchronous cancel + handoff) ------------

chrome.downloads.onCreated.addListener((item) => {
  if (isPassthrough(item)) return;
  const url = pickUrl(item);
  if (!isHttp(url) || !enabled()) return;

  // Capture everything SYNCHRONOUSLY, then cancel+erase immediately — before any
  // await — so Chrome never renders its Save As box / shelf entry. Prefer the
  // server-suggested filename the network layer captured over the browser's.
  const marked = pending.get(item.url) || pending.get(item.finalUrl);
  const referrer = item.referrer || "";
  const filename = (marked && marked.filename) || item.filename || "";
  pending.delete(item.url);
  pending.delete(item.finalUrl);
  chrome.downloads.cancel(item.id).catch(() => {});
  chrome.downloads.erase({ id: item.id }).catch(() => {});

  handoff(url, referrer, filename);
});

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
    return cookies.map((c) => `${c.name}=${c.value}`).join("; ");
  } catch (e) { return ""; }
}

async function handoff(url, referrer, filename) {
  await cfgReady;                                  // ensure port/token are loaded
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

  // Fail-safe: we already cancelled, so re-issue the download ourselves. The
  // explicit saveAs:false guarantees no dialog even when the browser's "ask where
  // to save each file" setting is on. Mark passthrough so onCreated skips it.
  passthrough.add(url);
  chrome.downloads.download({ url, saveAs: false }).catch(() => {});
  setTimeout(() => passthrough.delete(url), 10000);
}
