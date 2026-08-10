# BitTorrent Sub-phase C — DHT + magnet links + metadata exchange (design)

- **Status:** Draft (brainstormed 2026-07-27/28; awaiting user review before planning).
- **Date:** 2026-07-28
- **Phase:** 6 (P2P / BitTorrent), Sub-phase C.
- **Depends on:** Sub-phase A (MVP leech-only) and Sub-phase B (UDP + multi-tracker),
  both committed in `develop` (`184ca2e`, `8ac9add`).
- **Predecessor spec:** `2026-07-24-bittorrent-udp-multitracker-design.md`.

## 1. Goal

Let the client download torrents whose peers live on the **DHT**, and torrents you
only have as a **magnet link** (no `.torrent`). Concretely:

- **DHT peer discovery (BEP 5)** so a `.torrent` we already hold (e.g. the Ubuntu
  ISO) finds peers even when the tracker is stingy. This is the immediate, measured
  need — see §1.1.
- **Magnet links** (`magnet:?xt=urn:btih:…`): paste a magnet and download, with no
  `.torrent` file — fetching the metadata (info dict) from peers via the **extension
  protocol (BEP 10)** + **`ut_metadata` (BEP 9)**.

Practical success criterion: **the Ubuntu 24.04 ISO downloads to completion** in the
app (which it cannot today — see §1.1), and a **magnet link resolves and downloads**
offline in the test suite.

### 1.1 Why (the measured motivation)

Field investigation on 2026-07-27 (recorded in the Sub-phase B spec §10.1) established,
by direct measurement, that the "Ubuntu stalls at 1 piece" problem is **not** a
peer-wire bug:

- The Ubuntu tracker (`torrent.ubuntu.com`), even with `numwant=200`, returns
  **exactly one peer** — `complete:133` seeds exist, but it hands out one.
- That one peer (`185.125.190.59`) is a **super-seed**: `reserved=0`, no `bitfield`,
  reveals one piece at a time via `have`, and will not reveal more to a download-only
  client. An independent reference client sees identical bytes, confirming our wire
  code is correct.
- **qBittorrent downloads it via DHT** (a libtorrent frontend, DHT on by default:
  status bar "DHT: 94 nodes" → 30 seeds). We have no DHT.

So the missing capability is **DHT peer discovery**. A separate real torrent downloads
at 62 MiB/s in the app, proving the download engine is sound on normal swarms.

## 2. Scope decisions (closed in brainstorming — do not re-decide without reason)

| Decision | Choice |
|---|---|
| **Sub-phase C shape** | **Whole C in one spec/plan**: DHT + magnet + extension protocol + `ut_metadata`. Internally decomposed into small testable units; one spec → plan → implementation cycle. |
| **DHT fidelity** | **Full "good-citizen" DHT (BEP 5)**: responds to incoming `ping`/`find_node`/`get_peers`, stores third-party `announce_peer`, maintains a persistent routing table (`dht.dat`) + persistent node id, refreshes buckets. |
| **`announce_peer` of our own torrents** | **Deferred to Sub-phase D.** We do not accept incoming BT connections yet, so advertising ourselves as a servable peer is misleading. |
| **Serving `ut_metadata` to others** | **Deferred to Sub-phase D.** We advertise `ut_metadata` support and only *request*; we do not answer others' metadata requests. |
| **PEX / `ut_pex` (BEP 11)** | **Deferred to Sub-phase D** (already roadmapped there). |
| **BEP 42 (secure node id from IP)** | **Out.** Random persisted node id; anti-spoof hardening is marginal for the tribute. |
| **DHT over IPv6 (BEP 32)** | **Out** for now (IPv4 DHT resolves the motivating case); tracked as a follow-up. |
| **RateLimiter over DHT/torrent traffic** | **Still out** (pre-existing debt since Sub-phase A). |
| **Transaction ids / RNG** | Deterministic from `rngSeed` (as `UdpTrackerClient`), so offline tests reproduce without wall clock / `Math.random`. |
| **Token** | Opaque, derived from requester IP + a rotating secret; validated on `announce_peer`. |

## 3. Architecture (overall shape)

Four new pieces; three of them pure and unit-testable in isolation.

1. **`MagnetUri` (pure)** — parses `magnet:?xt=urn:btih:<hash>&dn=<name>&tr=<tr>` →
   `{ infoHash, displayName, trackers[] }`. Accepts info_hash as **hex (40 chars)**
   and **base32 (32 chars)**; multiple `tr` become an `announceList`. No I/O.

2. **`DhtNode` (networked, app-wide singleton)** — **one** instance owned by
   `DownloadManager` (like the shared torrent `QNetworkAccessManager`), **not**
   per-torrent: one `QUdpSocket`, one Kademlia routing table, one persistent node id.
   Serves every torrent's `get_peers`. Both client and good-citizen server (see §4).
   Exposes a per-info_hash lookup that emits peers in the **same shape** the
   `AnnounceController` already delivers them.

3. **Extension protocol (BEP 10) in `PeerConnection`** — today download-only and
   ignores id 20. It gains: advertising the extension + DHT reserved bits, the
   extended handshake, and **`ut_metadata` (BEP 9)** support. Prerequisite for magnet.

4. **`MetadataFetch` (new, orchestrates the pre-download phase)** — given only an
   `infoHash` + peer sources (DHT + magnet trackers), connects with the extension
   protocol, downloads the info dict in 16 KiB pieces via `ut_metadata`, **verifies
   SHA-1 == infoHash**, and produces a `TorrentMetainfo`. Then `DownloadManager`
   builds the normal `TorrentTask` and the download proceeds **identically** to the
   `.torrent` flow.

**Magnet flow (end to end):**

```
addMagnet(uri) → MagnetUri → { infoHash, dn, trackers }
   → MetadataFetch:  DhtNode.lookup(infoHash) + trackers → peers
        → PeerConnection (extended) → ut_metadata → info dict → verify == infoHash
   → TorrentMetainfo → makeTorrentTask(...) → normal download (Sub-phases A/B)
```

**Existing `.torrent` flow (e.g. Ubuntu):** gains DHT as a **parallel peer source**
alongside `AnnounceController` — `TorrentTask` subscribes to `DhtNode.lookup(infoHash)`
and pushes peers into the same `m_pendingPeers`. This is what unblocks Ubuntu. No
change to the download engine.

## 4. DHT internals (`DhtNode`)

Decomposed into small, testable units.

### 4.1 `NodeId` + XOR distance (pure)
160-bit id; distance = XOR; comparators for "closest to a target". Basis of routing
and lookups. Trivially testable.

### 4.2 `RoutingTable` (pure)
Kademlia k-buckets (**k = 8**) covering the 160-bit space; **split** the bucket
containing our own id when it fills; node states **good / questionable / bad** from
last-seen + failed-query counts. Pure (no I/O) → tests cover split, "n closest to a
target", promotion/eviction. Nodes seen from responses (and valid incoming queries)
are inserted.

### 4.3 `KrpcCodec` (pure)
KRPC messages are bencode dicts over UDP; **reuses the existing `Bencode` codec**.
Encodes/decodes `query` (`y=q`), `response` (`y=r`), `error` (`y=e`) with `t`
(transaction id), `q`, `a`/`r`. Pure/testable.

### 4.4 `DhtNode` (networked, `QUdpSocket`)
Two sides:

- **Client (outgoing):** `ping`, `find_node(target)`, `get_peers(infoHash)`. The
  **iterative `get_peers` lookup** is the core: keep a shortlist of the nodes closest
  to the info_hash, query **α = 3** in parallel, converge, collect `values` (peers) →
  emit `peersFound(infoHash, peers)`. **Re-lookup periodically** while a torrent is
  downloading (unblocks starving swarms).
- **Server (incoming, "good citizen"):** answers `ping` / `find_node` / `get_peers`
  (returns closest nodes / stored peers + a **token**), and **accepts third-party
  `announce_peer`** into a `infoHash → peers` store (with expiry + token validation)
  so it can answer others' `get_peers`. (Announcing *ourselves* is deferred to D.)

### 4.5 Bootstrap + maintenance
DNS-resolve the well-known routers (`router.bittorrent.com:6881`,
`router.utorrent.com:6881`, `dht.transmissionbt.com:6881`, `dht.libtorrent.org:25401`)
and `find_node(our own id)` to populate the table. `QTimer`-driven maintenance:
refresh stale buckets, ping questionable nodes. **Requires `QHostInfo::lookupHost`** —
Sub-phase B left DNS as a follow-up (udp:// trackers required a numeric host); the DHT
forces implementing it now (and it closes that follow-up too).

### 4.6 Persistence (`dht.dat`)
Node id + a set of good nodes, in the app data dir. Loaded at boot for fast bootstrap,
saved periodically and on shutdown.

### 4.7 Security (new attack surface)
Answering KRPC makes us reachable over UDP. `DhtNode` must be **robust to malformed
datagrams**, **validate the token** on `announce_peer`, and **not be an
amplification/reflection vector** (respond only to valid queries, with bounded
response sizes). Explicit requirements in the plan; a light fuzz test on the codec.

## 5. Extension protocol (BEP 10) + `ut_metadata` (BEP 9) + magnet

### 5.1 Extension protocol in `PeerConnection`
- Advertises the **extension bit** (reserved byte 5, `0x10`) and **DHT bit** (byte 7,
  `0x01`) in the handshake.
- Exchanges the **extended handshake** (id 20, ext msg 0): sends an `m` dict declaring
  `ut_metadata`; reads the peer's to learn the peer's `ut_metadata` id and
  `metadata_size`.
- Supports a **metadata mode** (constructed **without a known `pieceCount`**, which a
  magnet does not have yet): in that mode it only does the handshake + extended
  handshake + `ut_metadata`, with no bitfield/piece logic.

### 5.2 `ut_metadata` (BEP 9)
The bencoded info dict, split into **16 KiB** pieces. Messages: `request{msg_type:0,
piece:N}`, `data{msg_type:1, piece:N, total_size:S}` + raw bytes, `reject{msg_type:2}`.
We request pieces `0..ceil(size/16KiB)`, assemble, **SHA-1 the whole == infoHash**
(rejects a lying peer), and parse the result as the info dict → `TorrentMetainfo`.
We advertise `ut_metadata` but answer incoming requests with `reject`/nothing
(serving deferred to D).

### 5.3 `MetadataFetch` (orchestrator)
Given `infoHash` + peer sources (DHT + magnet trackers): opens metadata-mode
`PeerConnection`s, drives `ut_metadata`, and on the first **verified** metadata emits
`metainfoReady(TorrentMetainfo)`. `DownloadManager` then calls the normal
`makeTorrentTask` and **discards the metadata connections** (the `TorrentTask`
reconnects with `pieceCount` known — handing off a live connection is complex;
reconnecting is simple and cheap).

## 6. Integration (Core + state + persistence + GUI + settings)

### 6.1 Core
`DownloadManager` owns the single `DhtNode` (created when DHT is enabled in settings)
and exposes it. New `addMagnet(uri, destDir, …)` → `MagnetUri` → `MetadataFetch` → on
`metainfoReady`, normal `makeTorrentTask`. `TorrentTask` subscribes to
`DhtNode.lookup(infoHash)` as a peer source parallel to `AnnounceController` (same
`m_pendingPeers`), with **re-lookup when starving** (mirrors the adaptive reannounce).
Unsubscribes on stop/complete.

### 6.2 State
A magnet gains a new phase before `Downloading`: **`DownloadState::FetchingMetadata`**
("Resolving magnet…"). Size is **unknown** (shown as "—") until metadata arrives, then
it transitions to the normal `Connecting`/`Downloading`.

### 6.3 Persistence / resume
- An unresolved magnet entry is persisted in `torrents.json` (infoHash + trackers +
  displayName + destDir) → a restart **restarts the `MetadataFetch`**.
- On resolution, the reconstructed `.torrent` is **cached to
  `torrents/<infohash>.torrent`** (that directory already exists) and the entry
  becomes a normal torrent.
- `dht.dat` in the app data dir; loaded at boot, saved periodically + on shutdown.

### 6.4 Magnet file selection
Files are unknown until metadata arrives. Flow: **resolve metadata first → then** open
the existing `TorrentOpenDialog` (selection tree) with the now-known files. The
`.torrent` flow is unchanged.

### 6.5 GUI — magnet entry
`File > Open Magnet…`; the **New dialog accepts `magnet:`**; **clipboard detection of
`magnet:`** (the `ClipboardWatcher` already dispatches by scheme); **drag & drop** of
magnet text.

### 6.6 Edge cases
- **Magnet with DHT disabled and no `tr` trackers** cannot find peers → it stays in
  `FetchingMetadata` with no progress; surface this clearly (a log line + a Properties
  hint that DHT is off), rather than silently hanging. A magnet *with* trackers still
  resolves via them even with DHT off.
- **Session-restored magnet** (unresolved on quit) resolving after a restart has no
  interactive context to pop a selection dialog → it **defaults to all files**. The
  selection tree (§6.4) is only shown for an interactive add.
- **Deterministic tests:** the node id, like the transaction ids, is derived from an
  injected seed in tests; in production it is random and persisted (`dht.dat`).

### 6.7 Preferences / settings
BitTorrent category gains a **DHT on/off** toggle, a **DHT UDP port** (default 6881),
and shows the **DHT node count** (diagnostic). `settings.json` gains a `dht` block
(`enabled`, `port`). DHT diagnostics (node count, lookup results) appear in the
per-torrent **log + Properties**, as with trackers in Sub-phases A/B.

## 7. Testing strategy

**Pure units (no network):** `NodeId`/XOR distance, `RoutingTable` (bucket split,
closest-n, node states), `KrpcCodec` (encode/decode query/response/error), `MagnetUri`
(hex **and** base32, multiple `tr`), `ut_metadata` assembly + SHA-1 verification, token
generation/validation.

**In-process DHT harness (the heavy lift):** stand up **N `DhtNode`** instances on
loopback UDP — one acts as router/bootstrap, others join, one stores an info_hash, and
a `get_peers` lookup from another node **finds** that peer. Mirrors
`TestSeeder`/`TestUdpTracker`. Deterministic via **seeded transaction ids + injected
time**; timeout seam (default `ORBIT_UDP_FAST_TIMEOUT`) and loopback seam (default
`ORBIT_ALLOW_LOOPBACK_PEERS`).

**Test metadata peer:** a peer that advertises `ut_metadata` and serves the info dict →
`MetadataFetch` fetches, **verifies == infoHash**, and produces the correct
`TorrentMetainfo`.

**Full magnet E2E, offline (the crown jewel):** `magnet → MetadataFetch (peers via
in-process DHT + metadata peer) → TorrentMetainfo → TorrentTask downloads from
TestSeeder → byte-identical`. All offline, anti-flake in a loop (the 20/20 pattern from
Sub-phase B). Existing gates (`tst_download`/`tst_ftp`/`tst_torrent`) stay intact.

## 8. Out of scope (deferred / YAGNI)

- **Sub-phase D:** `announce_peer` of our own torrents, serving `ut_metadata`/pieces to
  others, PEX/`ut_pex`, accepting incoming connections.
- **Later/never:** DHT over IPv6 (BEP 32), BEP 42 secure node id, uTP/encryption
  (Sub-phase E). Torrent/DHT traffic still bypasses the global `RateLimiter`
  (pre-existing debt).

## 9. Risks

1. Real-internet lookups are slow/nondeterministic (bootstrap latency) — mitigated by
   requiring determinism **only offline**.
2. New **DNS dependency** (`QHostInfo`) for the routers.
3. UDP blocked on some networks — the DHT simply finds no peers (degrades, does not
   break).
4. The **in-process DHT test harness is the largest single effort** of the sub-phase.
5. The magnet resume state machine + the `FetchingMetadata` state touch the GUI
   model/table.
6. New UDP attack surface (§4.7) — must be hardened against malformed input and
   reflection.
