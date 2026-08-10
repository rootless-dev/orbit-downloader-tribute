# BitTorrent MVP (leech-only) — Design

Date: 2026-07-23
Status: Implemented (pending human E2E)

Phase 6, Sub-phase A of the P2P/BitTorrent work (see `ROADMAP.md` §"Fase 6"). This is the first
of five sub-phases; magnet/DHT (C), UDP trackers (B), seeding (D) and encryption/uTP (E) are
explicitly out of this spec and get their own spec → plan → implementation later.

## Summary

Add a from-scratch **BitTorrent client** to `orbitcore` — no external library (libtorrent was
considered and rejected to stay faithful to the from-scratch tribute spirit, matching how FTP was
built over `QTcpSocket`). This sub-phase downloads a `.torrent` **from start to finish** through a
real swarm, with SHA-1 integrity verification and crash-safe resume, presented in the existing GUI
(table row, progress grid, Log/Properties tabs, context menu) as just another download.

Scope decisions locked during brainstorming (2026-07-23):

1. **BitTorrent, not P2SP.** The proprietary Orbit P2SP network is permanently out (dead
   servers/closed protocol). "P2P" here means open BitTorrent.
2. **From scratch, not libtorrent.**
3. **Parallel subsystem, not `Transport`.** BitTorrent does not fit the `Transport` abstraction
   (probe → byte-ranges from a single origin). It is a separate task kind that emits the **same
   outward signals** the GUI already consumes.
4. **Download-only (choke all).** Outbound connections only; we never send `unchoke`/`piece` and
   do not accept incoming connections. Seeds and peers' optimistic-unchoke still serve us. Real
   seeding + choke/unchoke is Sub-phase D.
5. **Both piece-selection strategies**, rarest-first default, switchable per-download via the
   context menu.
6. **Both resume-verification modes** (trust bitfield / re-check on open), chosen in Preferences,
   plus an on-demand **Force re-check** in the context menu.
7. **Selective file download in the MVP** — a file-tree dialog on open.
8. **Global bandwidth cap applies** — the existing `RateLimiter` throttles peer downloads.
9. **Entry via `File > Open Torrent…` + drag&drop of `.torrent`.**

## Background / Current State

- **Engine core** (`src/core/`, library `orbitcore`, Core+Network only, **no QtWidgets** → tested
  headless): `DownloadManager` owns `QVector<DownloadTask*> m_tasks`, a `scheme → Transport*`
  registry, a global `RateLimiter m_limiter`, session persistence, and a `pump()` that promotes
  `Queued → Downloading` up to `maxConcurrent`. It exposes per-id control (`pause/resume/cancel/
  setPriority/moveFiles/remove`) and emits `taskProgress(id, received, total)` /
  `taskStateChanged(id, state)`.
- **`DownloadTask`** (`src/core/DownloadTask.h`) drives one HTTP/FTP download over a `Transport`,
  emitting `progress(received, total)`, `stateChanged(DownloadState)`, `segmentProgress(index,
  offset)`; exposes `id()`, `state()`, `record()`, `segments()`, `priority()`. `DownloadState`
  (`DownloadTypes.h`) is the shared enum (`Queued`, `Connecting`, `Downloading`, `Paused`,
  `Completed`, `Cancelled`, `Error`).
- **`RateLimiter`** (`src/core/RateLimiter.h`): token-bucket global download cap, consulted by the
  HTTP/FTP segment workers with `setReadBufferSize` backpressure. Thread-aware.
- **`Persistence`** (`src/core/Persistence.h`): `read/writeJsonObject` helpers; the download
  session and per-download `.meta` files are written through it.
- **`Logger`** (`src/core/Logger.h`): app log + per-download logs with rotation; `DownloadTask`
  takes a `Logger*` and logs lifecycle lines.
- **GUI** (`src/gui/`): `DownloadTableModel` (a `QAbstractTableModel` reading the task list by id
  and the manager's signals), `ProgressGridWidget` + `GridGeometry`/`computeCells` (renders the
  block grid from byte-segment state with a write-head cell), `CategoryTree`/`CategoryFilterProxy`
  (state/type filters), `MainWindow` (toolbar, tabs Log/Progress/Properties, context menu built
  from `ContextMenuRules.h`), `NewDownloadDialog`, `DropTargets` (drag&drop of `uri-list`/
  `text/plain`), `Settings`/`SettingsIo` (`AppSettings { EngineConfig; UiPrefs; SchedulerConfig;
  BrowserPrefs; }` → `settings.json`), `PreferencesDialog` (recently redesigned as a **sidebar of
  categories** — commit `0406c8d`).
- **Testing** (`tests/`): QtTest, offline. Network protocols are exercised against in-process test
  doubles — `QHttpServer` for HTTP and a hand-written `TestFtpServer` over `QTcpSocket` for FTP.
  This spec follows the same doctrine with a `TestSeeder`.

## Goals

- Open a `.torrent` (menu or drag&drop), pick which files to download, and fetch them to completion
  through a real HTTP-tracker swarm, verifying every piece against its SHA-1.
- Show the torrent as a normal row: name, size, state, speed/ETA, priority; render pieces in the
  progress grid; log lifecycle; show tracker/piece info in Properties.
- Pause/resume/cancel with the same semantics as downloads; resume survives process restart.
- Let the user switch piece strategy per-download and force a re-check from the context menu.
- Respect the global bandwidth cap.

## Non-Goals (YAGNI — belong to later sub-phases)

- Magnet links, DHT, extension protocol / metadata exchange (Sub-phase C).
- UDP trackers, `announce-list` multi-tracker rotation, tracker scrape (Sub-phase B) — the MVP
  uses **only the primary `announce` HTTP(S) URL**; if it is UDP or fails, the torrent errors with
  a clear message.
- Seeding / uploading, accepting incoming connections, choke/unchoke (tit-for-tat), PEX
  (Sub-phase D).
- Encryption (MSE/PE), uTP (Sub-phase E).
- Endgame mode (final blocks may drag) — accepted MVP limitation.
- Super-seeding, sequential-availability streaming heuristics, per-file priority beyond
  selected/unselected, changing file selection after the torrent has started (selection is fixed at
  add time; changing it later is a follow-up).

## Design

New code lives under `src/core/torrent/` inside `orbitcore`. Pure units (no network) carry the bulk
of the test coverage; Qt/event-loop units are thin and driven by the pure ones.

### 1. `AbstractTask` — the integration seam

The manager, model, grid and context menu all key off `DownloadTask*`. To let a torrent be "just
another row" without duplicating that machinery, extract an abstract base:

```cpp
// src/core/AbstractTask.h
class AbstractTask : public QObject {
    Q_OBJECT
public:
    enum class Kind { Http, Ftp, Torrent };
    virtual Kind    kind() const = 0;
    virtual QUuid   id() const = 0;
    virtual DownloadState state() const = 0;
    virtual QString displayName() const = 0;   // file/torrent name shown in the table
    virtual qint64  totalBytes() const = 0;     // -1 until known
    virtual qint64  receivedBytes() const = 0;
    virtual Priority priority() const = 0;
    virtual void    setPriority(Priority p) = 0;
    virtual void    start() = 0;
    virtual void    pause() = 0;
    virtual void    requeue() = 0;
    virtual void    cancel() = 0;
signals:
    void progress(qint64 received, qint64 total);
    void stateChanged(DownloadState state);
};
```

- `DownloadTask` is refactored to inherit `AbstractTask` — a **mechanical, behavior-preserving**
  change: it already has every method/signal above (add `kind() { return Http/Ftp; }`,
  `displayName()`, `totalBytes()`, `receivedBytes()` thin accessors). Its extra
  `segmentProgress`/`record`/`segments`/credentials surface stays on `DownloadTask` and is used via
  `qobject_cast<DownloadTask*>` where segment-specific behavior is needed.
- `DownloadManager::m_tasks` becomes `QVector<AbstractTask*>`; `taskById`, the signal wiring
  (`wire()`), `pump()`, `pause/resume/cancel/setPriority/remove` operate on the interface.
  `pump()`'s concurrency cap counts a torrent as **one** slot (like a download) even though it
  internally opens many peer sockets.
- The **alternative** (a separate torrent list unioned in the model) was rejected: it would
  duplicate the model/grid/menu/filter logic and re-introduce the row-staleness bug class that cost
  real time in Phases 2–3.

A new `DownloadState::Checking` is added for the hash-verification phase (initial verify of
on-disk data and Force re-check). Model text/colors and `CategoryFilterProxy` treat `Checking` like
an active, non-completed state.

### 2. `Bencode` — codec (pure)

`src/core/torrent/Bencode.{h,cpp}`. Decode `QByteArray → BencodeValue` (a variant over
`qint64` / `QByteArray` / `QList<BencodeValue>` / ordered `QMap<QByteArray, BencodeValue>`), and
encode back. **Critical:** the decoder records the **raw byte span** `[start, end)` of every dict
value it parses, so the caller can take the exact bytes of the `info` dictionary for the info-hash
**without canonical re-encoding** (re-encoding is a classic source of hash mismatches). Strict
parsing: reject leading zeros, negative-zero, unsorted/duplicate keys leniently-but-safely, and
truncated input. Pure, exhaustively unit-tested.

### 3. `TorrentMetainfo` — `.torrent` parsing (pure)

`src/core/torrent/TorrentMetainfo.{h,cpp}`. From a decoded metainfo:

```cpp
struct FileEntry { QString path;  qint64 length;  qint64 offset; }; // offset into the logical stream
struct TorrentMetainfo {
    QByteArray  infoHash;         // 20-byte SHA-1 of the raw info dict
    QString     name;             // suggested dir (multi-file) or file name (single-file)
    qint64      pieceLength;
    QVector<QByteArray> pieceHashes;   // 20 bytes each
    qint64      totalLength;
    QVector<FileEntry>  files;    // 1 entry for single-file torrents
    QUrl        announce;         // primary tracker (announce-list ignored in the MVP)
    bool        isMultiFile;
};
```

Single-file (`length`) and multi-file (`files` list of `{length, path[]}`) layouts both map to one
logical byte stream (files laid end-to-end). `parse()` returns an error string on malformed input
(missing keys, hash-length not a multiple of 20, byte totals inconsistent). Pure.

### 4. `PieceStore` — mapping, I/O, verification, file selection (pure logic + `QFile`)

`src/core/torrent/PieceStore.{h,cpp}`. Owns the on-disk file(s) and translates between the piece
space and the file space.

- **Mapping:** `blocksToRegions(pieceIndex, begin, length) → [{FileEntry, fileOffset, length}]`,
  splitting a write that spans multiple files. Pure and unit-tested independently of `QFile`.
- **Layout:** single-file → `<destDir>/<name>`; multi-file → `<destDir>/<name>/<file path…>`.
  Files are created/opened lazily and written at the correct offset (sparse allowed; no
  preallocation in the MVP).
- **Selection:** given a set of selected file indices, a piece is **wanted** iff it overlaps any
  selected file's byte range. A boundary piece overlapping selected + unselected files is downloaded
  whole and its bytes are written into whichever files it spans (so shared boundary bytes land
  correctly); files touched by no wanted piece are never created.
- **Verification:** `verify(pieceIndex, buffer) → bool` compares `SHA-1(buffer)` to
  `pieceHashes[pieceIndex]`. `verifyOnDisk(pieceIndex) → bool` re-reads the piece from disk and
  hashes it (used by resume re-check).

### 5. `Bitfield` (pure)

`src/core/torrent/Bitfield.{h,cpp}`. Fixed-size bitset over pieces: `has(i)`, `set(i)`, `count()`,
`isComplete()`, `toBytes()`/`fromBytes()` in the wire big-endian-bit order (bit 0 = MSB of byte 0).
Backs resume, the "wanted & missing" set, and what the grid displays.

### 6. `PiecePicker` — both strategies (pure)

`src/core/torrent/PiecePicker.{h,cpp}`. Given: our `Bitfield`, the set of **wanted** pieces
(from selection), per-peer availability (peer bitfields folded into a rarity count), and the
in-flight block set, choose the next block(s) to request. Two modes behind one interface:

```cpp
enum class PieceStrategy { RarestFirst, Sequential };
```

- **RarestFirst** (default): among wanted-and-missing pieces the peer has, prefer the lowest
  availability count; break ties randomly (random seeded from a caller-injected value so tests are
  deterministic — no `Math.random`/`Date::now` in headless logic).
- **Sequential:** lowest wanted-and-missing piece index the peer has.
- Blocks are 16 KiB (`1 << 14`); the picker hands out up to `pipelineDepth` (default 8) outstanding
  requests per peer and tracks in-flight `(piece, begin)` to avoid duplicate requests (no endgame
  duplication in the MVP). Strategy is switchable at runtime via `setStrategy`.

### 7. `HttpTrackerClient` (Qt/network)

`src/core/torrent/HttpTrackerClient.{h,cpp}`. Announces to the primary HTTP(S) `announce` URL via
`QNetworkAccessManager`:

- **Request:** GET with `info_hash` (raw 20 bytes, URL-encoded), `peer_id` (see §9), `port`
  (nominal, from Preferences/default — we do not actually listen), `uploaded=0`, `downloaded`,
  `left`, `compact=1`, and `event` (`started` on first announce, `stopped` on pause/remove,
  `completed` once at 100%; empty on periodic re-announce).
- **Response:** bencoded dict → `interval`, `peers` (compact 6-byte `IPv4:port` **and** dict-list
  forms), `failure reason` / `warning message`. Emits `peersReceived(QList<PeerAddress>)` and
  schedules the next announce with a `QTimer` at `interval` (floored to a sane minimum). If the
  `announce` scheme is `udp` or the request fails, emits `announceFailed(reason)`.

### 8. `PeerConnection` — peer wire protocol, download-only (Qt/network)

`src/core/torrent/PeerConnection.{h,cpp}`. One per peer over `QTcpSocket`.

- **Handshake:** `<19>"BitTorrent protocol"<8 reserved=0><20 info_hash><20 peer_id>`; validate the
  echoed info-hash, drop on mismatch.
- **Framing:** length-prefixed messages; `keep-alive` (len 0); ids `choke(0)`, `unchoke(1)`,
  `interested(2)`, `not interested(3)`, `have(4)`, `bitfield(5)`, `request(6)`, `piece(7)`,
  `cancel(8)`. Parser tolerates partial reads (accumulate until a full frame is available) and
  guards against absurd lengths.
- **State:** tracks `peer_choking`, `peer_interested`, and the peer's `Bitfield`. We are
  **always `am_choking = true`, `am_interested`** set when the peer has a wanted piece. We send
  `interested`, then `request` blocks handed to us by the owner (`TorrentTask`) when the peer sends
  `unchoke`; we ignore/deny incoming `request` (download-only). Emits `blockReceived(index, begin,
  data)`, `haveReceived`, `bitfieldReceived`, `unchoked`/`choked`, `disconnected`.
- **Throttling:** peer socket reads honor the global `RateLimiter` via the same
  `setReadBufferSize` backpressure pattern used by the HTTP/FTP workers, so torrent download obeys
  the global cap.

### 9. `TorrentTask` — orchestration + resume (Qt, `: AbstractTask`)

`src/core/torrent/TorrentTask.{h,cpp}`. The engine of a single torrent.

- **Construction:** from a `TorrentMetainfo`, a `destDir`, a selection set, a `PieceStrategy`, the
  `RateLimiter*`, and a `Logger*`. Generates a 20-byte `peer_id`: prefix `-OB0001-` (Orbit,
  version 0.0.1, Azureus style) + 12 bytes from an injected RNG seed.
- **Lifecycle / state machine** (drives `DownloadState`): `Queued` → (on `start`) `Checking` if
  on-disk data must be verified, then `Connecting` (first announce + handshakes) →
  `Downloading` → `Completed`. `pause()` → announce `stopped`, tear down peers, keep the bitfield,
  state `Paused`. `cancel()` → `Cancelled` (+ optional file delete via the manager's `remove`).
  Errors (announce failed / metainfo invalid) → `Error` with a message.
- **Loop:** announce → connect up to `maxPeersPerTorrent` (default 50) peers → send `interested`
  → on `unchoke`, ask the `PiecePicker` for blocks and pipeline `request`s → assemble a full piece
  → `PieceStore::verify` → on pass, write + `Bitfield::set` + advance progress; on fail, discard
  the piece and re-request (drop the offending peer after repeated failures). Re-announce on the
  tracker interval; `completed` event once `isComplete()`.
- **Progress signals:** emits `progress(received, total)` at ≤1 Hz (received = verified bytes) and
  `stateChanged`; emits **`pieceStateChanged(int index, PieceState)`** (`Missing`/`InFlight`/
  `Have`) for the grid. Speed/ETA reuse the GUI `SpeedSampler` off `progress`, exactly as
  downloads do.
- **Resume:** persists a bitfield file and its record (see §12). On `start` after restore, the
  Preferences resume mode decides: **TrustBitfield** → adopt the saved bitfield as-is;
  **RecheckOnOpen** → `Checking` phase re-hashes each piece via `PieceStore::verifyOnDisk` and
  rebuilds the bitfield. **Force re-check** (context menu) runs the same `Checking` pass on demand.

### 10. `DownloadManager` integration

- New `QUuid addTorrent(const QString& torrentPath, const QString& destDir, const QSet<int>&
  selectedFiles, PieceStrategy strategy)`: parses the metainfo, **copies the `.torrent` into
  `<dataDir>/torrents/<infoHash>.torrent`** (so resume never depends on the user's original file),
  constructs a `TorrentTask`, `wire()`s it, appends to `m_tasks`, and pumps. Rejects a duplicate
  info-hash (returns the existing id).
- `wire()` connects the `AbstractTask` `progress`/`stateChanged` to the existing
  `taskProgress`/`taskStateChanged` re-emitters (unchanged shape). Torrent-specific signals
  (`pieceStateChanged`) are exposed for the grid via `qobject_cast<TorrentTask*>`.
- `remove/pause/resume/setPriority` already operate by id on `AbstractTask` after §1.
- **Session:** `saveSession`/`loadSession` gain a torrent section recording, per torrent,
  `{infoHash, destDir, selectedFiles, strategy, state}`; the metainfo is re-read from the stored
  copy and the bitfield from its resume file on load.

### 11. GUI

- **Open Torrent:**
  - `File > Open Torrent…` → `QFileDialog` (`*.torrent`) → parse → **selection dialog**.
  - `DropTargets` extended: a dropped `*.torrent` (by extension / bencode sniff) routes to the same
    open flow instead of the URL path.
  - **`TorrentOpenDialog`** (new, in `orbitgui`): shows the torrent name, total size, a **file tree
    with checkboxes** (default all selected), a destination-folder picker (default download dir),
    and the initial piece strategy (default rarest-first). On accept → `addTorrent(...)`. Pure
    selection/summary logic factored into `orbitgui_logic` for testing.
- **Progress grid:** `ProgressGridWidget` learns a second source. When the selected row is a
  torrent, it renders **piece → cell** from `TorrentTask`'s `PieceState`s (aggregating N pieces per
  cell when pieces exceed the tile budget, mirroring how the byte grid aggregates slices), reusing
  the Orbit color scheme (missing / in-flight / have). Byte-segment downloads render as today.
- **Context menu:** for a torrent row, add (via `ContextMenuRules`): a checkable **Piece strategy →
  Rarest-first / Sequential** submenu (calls `TorrentTask::setStrategy`, persisted) and **Force
  re-check** (triggers the `Checking` pass). Non-torrent rows are unchanged.
- **Preferences → BitTorrent** (new sidebar category): **Max peers per torrent** (default 50),
  **Resume verification** (Trust bitfield / Re-check on open), **Default piece strategy**
  (Rarest-first / Sequential), **Listen port** (nominal, announced to trackers; default e.g.
  6881). Applied to new/loaded torrents via `TorrentTask` setters where live-applicable.
- **Properties/Log:** the per-download `Logger` logs torrent lifecycle (announce, peer counts,
  piece verify pass/fail); Properties shows info-hash, piece length/count, tracker, peer count,
  selected files.

### 12. Persistence / resume format

- **`.torrent` copy:** `<dataDir>/torrents/<infoHash>.torrent` (hex info-hash).
- **Bitfield resume:** `<dataDir>/torrents/<infoHash>.bitfield` — a small binary file: a header
  (`piece count`, `piece length`, `total length`) + the raw bitfield bytes. Written on piece
  completion (debounced, like `.meta`) and on pause/quit.
- **Session record** (in the existing session JSON via `Persistence`): `{infoHash, destDir,
  selectedFiles: [int], strategy: "rarest"|"sequential", state}` per torrent. On load, metainfo
  comes from the stored `.torrent`, progress from the `.bitfield` (or a re-check if Preferences say
  so).

## Data / Settings changes

- **`DownloadState`** gains `Checking`.
- **`AppSettings`** gains `BitTorrentPrefs { int maxPeersPerTorrent = 50; ResumeVerifyMode
  verify = TrustBitfield; PieceStrategy defaultStrategy = RarestFirst; quint16 listenPort = 6881; }`,
  round-tripped in `SettingsIo::toJson`/`fromJson`, defaulting when absent (backward compatible).
- **Session JSON** gains a `torrents` array (see §12); older sessions with no such key load with
  zero torrents.

## Error handling

- **Malformed `.torrent` / info dict** → `addTorrent` returns a null id and surfaces the parser
  error in the open dialog; nothing is added.
- **UDP-only or failing announce** → task goes `Error` with a clear message ("tracker uses UDP —
  supported in a later version" / the tracker's `failure reason`). No peers, no silent hang.
- **Piece verification failure** → discard the assembled piece, re-request its blocks; a peer that
  supplies repeated bad pieces is dropped. Never write unverified data.
- **Peer socket errors / handshake mismatch** → drop that peer; the task keeps running on the rest
  and refills from the peer list / next announce.
- **No peers at all after announce** → stay in `Connecting`, re-announce on interval; the user can
  pause/cancel.
- **Bitfield/`.torrent` copy missing on load** → the torrent is dropped from the session with a
  logged warning (cannot resume without metainfo); a truncated/corrupt bitfield falls back to a
  full re-check.
- **RateLimiter** integration reuses the proven HTTP/FTP backpressure path; residual buffered bytes
  are drained on stop.

## Testing

Offline, QtTest, mirroring the `QHttpServer`/`TestFtpServer` doctrine.

- **Pure units (bulk of coverage):**
  - `tst_bencode` — round-trip encode/decode, strictness (leading zeros, truncation), and **exact
    `info` byte-span** extraction feeding a known info-hash.
  - `tst_metainfo` — single-file and multi-file parsing, byte totals, error cases; known-vector
    info-hash.
  - `tst_piecepicker` — rarest-first ordering under crafted availability, sequential ordering,
    pipeline depth, in-flight dedup, deterministic tie-break via injected seed.
  - `tst_piecestore` — multi-file region splitting, boundary pieces, selection wanted-set,
    `verify`/`verifyOnDisk`.
  - `tst_bitfield` — bit order, `toBytes`/`fromBytes`, completeness.
- **`HttpTrackerClient`** — against a local `QHttpServer` returning a canned bencoded announce
  (compact and dict peer forms, `failure reason`); assert parsed peers/interval and `event`
  sequencing.
- **`TestSeeder`** (deliverable, `tests/`): a minimal in-process BitTorrent **seeder** over
  `QTcpSocket` for a small known torrent — accepts a handshake, sends `bitfield` (all pieces),
  `unchoke`, and answers `request` with `piece`. Enables the **end-to-end offline test**
  (`tst_torrent`): `TorrentTask` downloads a multi-piece file (single- and multi-file torrents,
  with a partial selection) from the local seeder and the output is **byte-identical** to the
  source; then a **resume** test (kill mid-download, reload, finish) and a **Force re-check** test.
- **Integration/GUI** (`tst_gui`, offscreen): `addTorrent` produces a row with the right
  name/size/state; `TorrentOpenDialog`/selection logic (via `orbitgui_logic`) computes the wanted
  set; the grid maps pieces to cells; context-menu strategy switch and Force re-check reach the
  task; Preferences BitTorrent round-trips through `settings.json`. `DownloadState::Checking`
  renders and filters correctly.
- **Gate:** the existing `tst_download` expectations must remain unchanged (the `AbstractTask`
  refactor is behavior-preserving for HTTP/FTP).

## Rollout / files touched

New (`src/core/`):
- `AbstractTask.h` — the interface (§1).
- `torrent/Bencode.{h,cpp}`, `torrent/TorrentMetainfo.{h,cpp}`, `torrent/PieceStore.{h,cpp}`,
  `torrent/Bitfield.{h,cpp}`, `torrent/PiecePicker.{h,cpp}`, `torrent/HttpTrackerClient.{h,cpp}`,
  `torrent/PeerConnection.{h,cpp}`, `torrent/TorrentTask.{h,cpp}`.

Changed (`src/core/`):
- `DownloadTask.{h,cpp}` — inherit `AbstractTask`, add thin accessors (behavior-preserving).
- `DownloadTypes.h` — `DownloadState::Checking`, `PieceStrategy`, `ResumeVerifyMode`.
- `DownloadManager.{h,cpp}` — `m_tasks` as `AbstractTask*`, `addTorrent`, session torrents,
  wiring.
- `CMakeLists.txt` — add the torrent sources.

Changed (`src/gui/`):
- `Settings.h` + `SettingsIo` — `BitTorrentPrefs`.
- `MainWindow.{h,cpp}` — `File > Open Torrent…`, drag&drop routing, context-menu items, grid
  source switch, Properties fields.
- `DropTargets.{cpp}` — `.torrent` detection.
- `ProgressGridWidget.{h,cpp}` — piece-state source.
- `ContextMenuRules.h` — torrent items.
- `PreferencesDialog.{cpp}` — BitTorrent category.
- New `TorrentOpenDialog.{h,cpp}` (+ pure selection logic in `orbitgui_logic`).
- `DownloadTableModel.{cpp}` — `Checking` text/colors, `AbstractTask` reads.

Tests (`tests/`):
- `tst_bencode`, `tst_metainfo`, `tst_piecepicker`, `tst_piecestore`, `tst_bitfield`,
  `tst_torrent` (+ `TestSeeder`), tracker cases; `tst_gui` additions; `tests/CMakeLists.txt`.
