# BitTorrent Sub-phase B — UDP trackers + multi-tracker + compact peers (design)

- **Status:** Implemented (8 TDD tasks, subagent-driven + final whole-branch review). Suite 28/28; offline E2E downloads byte-identically through the full UDP-tracker → AnnounceController → UdpTrackerClient path (20/20 anti-flake). Committed to `develop`.
- **Date:** 2026-07-24
- **Phase:** 6 (P2P / BitTorrent), Sub-phase B.
- **Depends on:** Sub-phase A (MVP leech-only), committed in `develop` (`184ca2e`).
- **Predecessor spec:** `2026-07-23-bittorrent-mvp-leech-design.md`.

## 1. Goal

Widen peer discovery and tracker robustness so the client works with the
torrents people actually use today:

- **UDP trackers (BEP 15)** — most public torrents announce over `udp://`
  today. Sub-phase A rejected `udp://` synchronously; this sub-phase implements it.
- **Multi-tracker via `announce-list` (BEP 12)** — honor announce tiers with
  in-tier ordering, promotion of the responsive tracker, and fallback.
- **Compact peers, IPv4 *and* IPv6 (BEP 23 / BEP 7)** — robust parsing of both
  the 6-byte (IPv4) and 18-byte (IPv6) compact forms, plus the dictionary form,
  with filtering of always-invalid addresses.

This sub-phase remains **leech-only**. Nothing in the download/peer engine
changes: the same `progress` / `stateChanged` / per-piece signals drive the GUI,
and `PeerConnection` is untouched. The work is confined to the tracker layer and
`TorrentMetainfo`.

Out of scope (unchanged from the Phase 6 decomposition): magnet/DHT/ut_metadata
(Sub-phase C), seeding/upload and the global `RateLimiter` for torrent traffic
(Sub-phase D), MSE/uTP (Sub-phase E).

## 2. Current state (what Sub-phase A left)

- `TorrentMetainfo` (`src/core/torrent/TorrentMetainfo.h`) exposes a single
  `QUrl announce`. It does **not** parse `announce-list`.
- `HttpTrackerClient` (`src/core/torrent/HttpTrackerClient.{h,cpp}`) wraps a
  `QNetworkAccessManager`, exposes `announce(...)` and emits
  `peersReceived(peers, intervalSecs, minIntervalSecs)` / `announceFailed(reason)`.
  It rejects a `udp://` tracker **synchronously** (before any request) via
  `announceFailed`.
- The pure helpers live in namespace `TrackerProto`:
  `buildAnnounceUrl(...)` and `parseResponse(body, peers*, interval*, minInterval*, failure*)`.
  `parseResponse` already handles compact **6-byte IPv4** and the **dictionary**
  peer form; it has no IPv6 (`peers6`) support and no bogon filtering.
- `TorrentTask` (`src/core/torrent/TorrentTask.{h,cpp}`) wires exactly one
  `HttpTrackerClient* m_tracker` against `m_meta.announce`. It owns the announce
  timer `m_announceTimer` and the pure adaptive-cadence function
  `nextAnnounceDelaySecsForTest(connectedPeers, hasWantedProgress, intervalSecs, minIntervalSecs)`.
  It dedupes discovered peers by `host:port` (`m_knownPeers`) into `m_pendingPeers`.
- `PeerConnection(const PeerAddress& addr, ...)` connects over `QTcpSocket`,
  which accepts an IPv6 host string as-is.

## 3. Architecture

Introduce a tracker **interface** and an **orchestrator**, so `TorrentTask`
keeps talking to a single object exposing the same two signals it already
consumes.

```
TorrentTask
  └ AnnounceController        owns tiers; BEP 12 order/promotion; dedup union; hungry fallback
       ├ ITrackerClient       announce(...) + signals peersReceived / announceFailed
       │    ├ HttpTrackerClient   (existing, refactored onto the interface)
       │    └ UdpTrackerClient    (new, BEP 15, over QUdpSocket)
       └ TrackerPeers         pure peer parsing: compact IPv4/IPv6 + dict + bogon filter
```

### 3.1 `ITrackerClient` (interface)

Minimal QObject contract, matching what `HttpTrackerClient` already exposes:

```cpp
class ITrackerClient : public QObject {
    Q_OBJECT
public:
    virtual void announce(const QByteArray& infoHash, const QByteArray& peerId,
                          quint16 port, qint64 downloaded, qint64 left,
                          TrackerEvent ev) = 0;
signals:
    void peersReceived(QVector<PeerAddress> peers, int intervalSecs, int minIntervalSecs);
    void announceFailed(QString reason);
};
```

- `HttpTrackerClient` is refactored to implement it. Its tracker URL moves from a
  parameter of `announce(...)` to a construction-time value (each client is bound
  to one tracker URL — the controller owns the list). The existing `tst_tracker`
  suite is kept green (this is a refactor with coverage, not a rewrite).
- The synchronous `udp://` rejection is **removed** from `HttpTrackerClient`;
  scheme dispatch now lives in the controller's client factory (§3.4).

### 3.2 `TrackerPeers` (pure peer parsing)

Extract the peer-decoding logic out of `TrackerProto::parseResponse` into a
reusable pure unit used by both HTTP and UDP paths:

- **Compact IPv4** — 6 bytes each (4-byte BE IPv4 + 2-byte BE port). (Existing.)
- **Compact IPv6 (BEP 7)** — 18 bytes each (16-byte IPv6 + 2-byte BE port), from
  the `peers6` key. IPv6 rendered as a bracketless string that `QTcpSocket`
  accepts. (New.)
- **Dictionary form** — list of `{ip, port}` dicts, `ip` may be an IPv4 or IPv6
  string. (Existing; IPv6 string acknowledged.)
- **Bogon filter (revised from brainstorming).** Filter only **always-invalid**
  addresses; do **not** try to distinguish "public vs private" trackers (that
  determination is unreliable and would wrongly drop legitimate LAN peers). Drop:
  `0.0.0.0` / `0.0.0.0/8`, loopback `127.0.0.0/8` and `::1`, multicast
  (`224.0.0.0/4`, `ff00::/8`), broadcast `255.255.255.255`, the unspecified IPv6
  `::`, and any record with **port 0**. RFC1918 / link-local addresses are
  **kept** — a peer that turns out unreachable simply fails one TCP connect,
  which is harmless.

`TrackerProto::parseResponse` (HTTP) is reworked to delegate its peer extraction
to `TrackerPeers`, gaining IPv6 and the bogon filter for free.

### 3.3 `TorrentMetainfo` — `announce-list` (BEP 12)

Add:

```cpp
QVector<QVector<QUrl>> announceList; // tiers; each tier is an ordered list of tracker URLs
```

- Parsed from the top-level `announce-list` key (a bencoded list of lists of
  URL byte-strings). When absent, synthesize a single tier holding `announce`.
- When both are present, BEP 12 says `announce-list` takes precedence; `announce`
  remains populated (back-compat / single-tracker callers).
- Invalid/empty entries are skipped; a torrent with neither `announce` nor a
  usable `announce-list` yields an empty tier set (the task still runs — see §6).

### 3.4 `UdpTrackerClient` (BEP 15)

A new `ITrackerClient` over `QUdpSocket`, one instance per `udp://` tracker URL.

**Protocol (BEP 15):**

1. **Connect** — send a connect request (magic `0x41727101980`, action `0`,
   `transaction_id`); receive a 64-bit `connection_id`. The `connection_id` is
   cached and valid for **60 seconds**; on expiry (or an announce that comes back
   with a "connection id mismatch" error action) the client transparently
   re-connects before announcing.
2. **Announce** — send an announce request (`connection_id`, action `1`,
   `transaction_id`, `info_hash`, `peer_id`, `downloaded`, `left`, `uploaded=0`,
   `event`, `key`, `num_want=-1`, `port`); receive `{action, transaction_id,
   interval, leechers, seeders, peers[6-byte compact]}`. Peers go through
   `TrackerPeers`. On an error action (`3`), emit `announceFailed` with the
   tracker's message.

**Determinism (matches the subsystem's no-RNG/no-clock rule):**

- `transaction_id` is a **monotonic counter seeded deterministically from the
  existing `rngSeed`** (same rule that already derives `peer_id`). It only needs
  to match request↔response; it is not used for security. *(Conscious tradeoff:
  BEP 15 suggests randomizing `transaction_id` to resist blind response
  injection; for a leech client the risk is negligible and determinism keeps the
  UDP path unit-testable. Recorded, not fixed.)*
- `key` is likewise derived from `rngSeed` (stable per task).

**Retries / timeouts:**

- BEP 15 prescribes a timeout of `15·2ⁿ` seconds for `n = 0..8`. We **cap `n`
  low** (timeouts `15s → 30s → 60s`, i.e. `n = 0..2`) rather than waiting up to
  3840 s on a dead tracker; when the cap is exhausted the client emits
  `announceFailed` and the `AnnounceController` falls back to other trackers.
- The timeout schedule (`nthTimeoutSecs(n)`) and the "should retry vs give up"
  decision are a **pure, clock-free function** so they are unit-testable in
  isolation, mirroring `nextAnnounceDelaySecsForTest`.
- Request↔response correlation is by `transaction_id`; late/duplicate/mismatched
  datagrams are dropped.

### 3.5 `AnnounceController` (BEP 12 + hungry fallback)

Owns the tier list and every `ITrackerClient`, and presents `TorrentTask` the
**same two signals** (`peersReceived` / `announceFailed`). It is a pure router of
*which* trackers to contact — it holds **no timer**; `TorrentTask` decides *when*
(see §4).

**Client factory.** For each tracker URL: `http`/`https` → `HttpTrackerClient`,
`udp` → `UdpTrackerClient`. Unknown schemes are dropped with a logged note.

**Normal regime (BEP 12).** On each announce round, iterate tiers **in order**;
within each tier, contact trackers **in order** until one responds; the
responsive tracker is **moved to the front of its tier** (promotion persists for
subsequent rounds). The client thus announces to **one responsive tracker per
tier, across all tiers**, and emits the **deduped union** (`host:port`) of the
peers those trackers returned.

**Hungry fallback (the chosen strategy).** Because the normal regime already
covers one-per-tier across all tiers, "fallback" means: when `TorrentTask`
signals starvation (few/no peers — the same condition its adaptive cadence
already detects), the controller **additionally probes the backup (non-front)
trackers within each tier** on that round, widening the union. It is not about
discovering new tiers (all tiers are already in play).

**Events.** `started` / `completed` / `stopped` are forwarded to the currently
active (front) tracker of each tier. `stopped` best-effort fires to all
trackers that had a live session.

**Interval aggregation.** The controller reports back to `TorrentTask` a single
`intervalSecs` (the **minimum** positive interval seen, so a fast tier isn't
starved) and a single `minIntervalSecs` (the **maximum** `min interval` seen, a
conservative floor so no strict tracker can ban a well-behaved client). This
matches the existing single-timer model in `TorrentTask`.

## 4. Ownership of announce timing

**`TorrentTask` keeps the announce timer and the adaptive cadence.** Rationale:
the cadence depends on torrent state (`connectedPeers`, `hasWantedProgress`) that
lives in `TorrentTask`; the controller has no view of it. Concretely:

- `TorrentTask` keeps `m_announceTimer`, `nextAnnounceDelaySecs(...)`,
  `m_announceIntervalSecs`, `m_minAnnounceIntervalSecs`.
- On each tick (and on `Started`), `TorrentTask` calls
  `m_announce->announce(infoHash, peerId, port, downloaded, left, event)` on the
  **controller** instead of a single `HttpTrackerClient`.
- The controller's aggregated `peersReceived(peers, interval, minInterval)`
  updates the same `m_announceIntervalSecs` / `m_minAnnounceIntervalSecs` fields
  and reschedules the timer exactly as today.
- Starvation → the existing cadence already shortens the delay; `TorrentTask`
  additionally passes a "hungry" flag into the controller's `announce(...)` (or a
  dedicated method) so the controller widens to backup trackers that round.

So `m_tracker` (single `HttpTrackerClient*`) is replaced by
`m_announce` (`AnnounceController*`); the rest of `TorrentTask`'s announce plumbing
stays structurally the same.

## 5. Data flow

Unchanged from `TorrentTask`'s perspective. It connects to
`AnnounceController::peersReceived` / `announceFailed` exactly as it did to
`HttpTrackerClient`. `m_knownPeers` (host:port dedup) and `m_pendingPeers` are
untouched. `PeerConnection` connects to IPv6 peers via `QTcpSocket` with no code
change. The GUI, `DownloadManager`, and resume (`.bitfield`) are unaffected.

## 6. Error handling & edge cases

- **UDP tracker silent** → capped backoff (§3.4) → `announceFailed` → controller
  falls back to other trackers; the torrent does not die (the adaptive cadence
  keeps retrying, as today).
- **`connection_id` expired mid-use** → transparent re-connect before the next
  announce; a "mismatch" error action also triggers re-connect.
- **Truncated / garbage datagram** → dropped (unmatched `transaction_id`);
  partial compact records are ignored (as HTTP already does), then bogon-filtered.
- **All trackers failing** → aggregated `announceFailed`; torrent stays alive and
  keeps retrying on cadence (no regression from Sub-phase A).
- **No usable trackers in metainfo** (no `announce`, empty `announce-list`) →
  controller has no clients; `announce(...)` is a no-op that reports "no
  trackers". The torrent still runs on injected/known peers (test seam,
  `addPeerForTest`) and — in a later sub-phase — DHT/PEX. No crash.
- **Mixed HTTP+UDP in one tier** → handled uniformly; promotion works across
  schemes (the front of a tier may be UDP or HTTP).

## 7. Testing (mirrors Sub-phase A's pattern)

**Pure units (no network):**

- `TrackerPeers` — compact IPv4 (6B), compact IPv6 (18B), dict form (IPv4+IPv6
  strings), bogon filter (drops `0.0.0.0`/loopback/multicast/port-0; **keeps**
  RFC1918), trailing-partial-record tolerance.
- UDP packet codecs — build/parse connect and announce requests/responses,
  `transaction_id` correlation, error-action parsing.
- `nthTimeoutSecs(n)` / retry-cap decision — clock-free.
- `TorrentMetainfo` `announce-list` parse — single/multi tier, precedence over
  `announce`, absent-list fallback, malformed-entry skipping.
- `AnnounceController` policy — tier order, promotion of responsive tracker,
  deduped union, hungry-fallback widening, interval/min-interval aggregation
  (driven by injecting client results, no real sockets).

**In-process UDP tracker server (new test seam).** A minimal `QUdpSocket`
server answering connect/announce with a configured peer set — mirrors
`TestFtpServer` and the in-process BitTorrent seeder from Sub-phase A. Enables an
**offline E2E**: a torrent pointing at a local `udp://` tracker → peers from the
local seeder → byte-identical download.

**Gate.** Existing suites stay intact: `tst_download`, `tst_ftp`,
`tst_torrent`, and the refactored `tst_tracker` (HTTP) all green.

## 8. Out of scope (this sub-phase)

- **Tracker scrape (BEP 48 HTTP / BEP 15 UDP)** — deferred. Diagnostics-only
  (seeders/leechers counts in Properties); does not help downloading, and adds
  scrape-URL derivation + a second UDP action + GUI wiring. **Registered as a
  planned future increment** (bring it in when the Properties panel warrants it),
  per the brainstorming decision.
- Magnet / DHT / `ut_metadata` — Sub-phase C.
- Seeding / upload / choke-unchoke / PEX, and routing torrent traffic through the
  global `RateLimiter` — Sub-phase D.
- MSE/PE encryption and uTP — Sub-phase E.
- WebSocket / WebTorrent trackers — not planned.

## 9. Risks / debts to track

- **`HttpTrackerClient` refactor** touches tested code; the URL-at-construction
  change ripples into `tst_tracker` and `TorrentTask`'s wiring. Covered by the
  gate, but it is the highest-churn part.
- **Interval aggregation is a simplification** — one global cadence for all
  tiers (min interval / max min-interval). Safe (never too fast) but a very fast
  tier may be announced to less often than it allows. Acceptable for a leech MVP.
- **`transaction_id`/`key` are deterministic, not random** — conscious tradeoff
  for testability (§3.4); negligible risk for a download-only client.
- **IPv6 reachability is environment-dependent** — if there is no IPv6 route, v6
  peers simply fail to connect (harmless); not a correctness concern.
- **Bogon filter is deliberately minimal** — keeps private ranges to avoid
  dropping LAN peers; the cost is at most a failed TCP connect per stale record.

## 10. Field finding (out of scope for this sub-phase — tracked, deferred)

During real-world testing (2026-07-27, Ubuntu 24.04 ISO), a **separate, pre-existing
leech-engine bug** surfaced that is unrelated to this sub-phase's tracker work:
a download can stall at 1 piece against a connected, unchoked remote seed because
the seed's `bitfield` message is never processed (`bitfieldReceived` never fires →
`PiecePicker` starves). It reproduces only against a **large / remote** bitfield
(Ubuntu: 1689-byte bitfield over the real network); a local seeder with a 4-byte
bitfield (`orbit-bt-test`, 32 pieces) downloads to completion normally, so it was
masked by the small-fixture E2Es. Suspected causes (not yet root-caused): (A) the
remote seed sends BEP 6 `have_all` (id 14), which the download-only client ignores;
or (B) a framing bug specific to a large bitfield message fragmented across TCP
reads. **Next step:** instrument `PeerConnection` inbound message ids + peer reserved
bytes and reproduce to decide A vs B before fixing (no fix without confirmed root
cause). This is distinct from the known DHT peer-discovery limitation (Ubuntu-style
swarms need Sub-phase C for peer *count*); this bug concerns *using* a peer we
already have.
