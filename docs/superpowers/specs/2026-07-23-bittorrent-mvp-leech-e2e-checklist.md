# BitTorrent MVP (leech-only) — Human E2E Checklist

Related spec: `2026-07-23-bittorrent-mvp-leech-design.md` (Status: Implemented, pending this
checklist). Everything below runs against **real** `.torrent` files on the public internet with a
live **HTTP** tracker (no UDP-only trackers — out of MVP scope) and needs a human, a display, and
network access — none of it runs in CI.

Pick a small, legally-distributable torrent with an HTTP tracker and healthy seeds for step 1–5
(e.g. a Linux distro's official torrent, or any `.torrent` you control with a tracker + seed you
run yourself). Keep the file small enough to complete in a few minutes.

## Setup

- [ ] Build a fresh `orbit-gui` (Release or Debug, either is fine) and launch it normally (not
      headless/offscreen).
- [ ] Have the `.torrent` file ready on disk, plus its magnet-free download URL if you need to
      re-fetch it.

## Checklist

1. **Open via menu** — *(human-only: requires clicking through a real file dialog)*
   - [ ] File → Open Torrent… (or equivalent menu entry), pick the `.torrent` file.
   - [ ] The file-selection dialog appears listing the torrent's files with sizes.

2. **Open via drag & drop** — *(human-only: OS-level drag gesture)*
   - [ ] Drag the same (or a second) `.torrent` file from Finder onto the main window.
   - [ ] Same file-selection dialog appears; behaves the same as the menu path.

3. **File selection** — *(human-only, but the underlying selection logic has unit coverage —
   `tst_gui`/`TorrentSelection` — this step is only verifying the dialog wiring)*
   - [ ] For a multi-file torrent, uncheck at least one file before confirming.
   - [ ] After the torrent completes (step 4), confirm the unchecked file was **not** downloaded
         (absent, or zero-length placeholder per current behavior) and the checked ones were.

4. **Download to completion with byte-verification** — *(human-only: needs real peers/tracker
   over real time; SHA-1-per-piece verification itself is automated in `tst_piecestore`/
   `tst_torrent`)*
   - [ ] Confirm the torrent, watch it progress through Connecting → Downloading → Completed in
         the grid.
   - [ ] Once Completed, verify at least one downloaded file's checksum (`shasum -a 1` or compare
         size) against a known-good copy or the torrent's declared piece hashes.

5. **Pause / resume across an app restart** — *(human-only: needs a real restart of the process)*
   - [ ] While downloading, pause the torrent from the grid/context menu.
   - [ ] Quit the app fully (not just close-to-tray) and relaunch it.
   - [ ] Confirm the torrent reappears **Paused** with prior progress intact (not restarted from
         zero).
   - [ ] Resume it and confirm it continues (doesn't re-download already-verified pieces) through
         to Completed.

6. **Strategy switch via context menu** — *(human-only: needs a live, still-downloading torrent
   to observe the effect; the picker's per-strategy piece order is unit-tested in
   `tst_piecepicker`)*
   - [ ] Add a fresh copy of the torrent (or restart an incomplete one), right-click it in the
         grid, switch strategy (Rarest-first ↔ Sequential).
   - [ ] Confirm no crash/stall and the download continues (exact piece order isn't asserted here,
         just that the switch is safe live).

7. **Force re-check** — *(human-only: needs a completed/partial torrent and wall-clock time to
   observe the Checking pass; the verify pass itself is unit-tested in `tst_torrent`)*
   - [ ] On a completed (or partially-downloaded) torrent, right-click → Force re-check.
   - [ ] Confirm the task transitions through a **Checking** state and settles back to the correct
         state (Completed if all pieces verify, or the correct partial otherwise) without data
         loss.

8. **Bandwidth cap applies to torrents** — *(human-only: needs a live transfer to observe a rate;
   `RateLimiter`'s accounting itself is unit-tested in `tst_ratelimiter`)*
   - [ ] Set a low global bandwidth cap in Preferences (e.g. 200 KB/s) while a torrent is actively
         downloading.
   - [ ] Confirm the torrent's observed throughput respects the cap (shared with any concurrent
         HTTP/FTP downloads, same global limiter).

9. **Preferences BitTorrent values honored by a newly-added torrent** — *(human-only end-to-end
   observation; the plumbing itself — `DownloadManager::setTorrentDefaults` reaching
   `TorrentTask` — is unit-tested in `tst_torrent::setTorrentDefaultsReachesNewTorrentTask`, added
   in Task 15)*
   - [ ] In Preferences → BitTorrent, change **Max peers per torrent**, **Listen port**, and
         **Verify mode** (Trust bitfield / Recheck on open) to non-default values, and confirm the
         **Default strategy** to a specific value; click OK/Apply.
   - [ ] Add a **new** torrent afterward and confirm it behaves consistently with the new prefs —
         e.g. Recheck-on-open causes a Checking pass on next launch/force-recheck, the default
         strategy shown in its context menu matches what was set, and it doesn't fail to bind if
         the configured listen port is free.
   - [ ] (Not expected to affect torrents already added before the Preferences change — the
         contract, like `setConfig`, is "applies to what's added/resumed after this point.")

## Automatable vs. human-only summary

| Area | Automated coverage | Needs a human for |
|---|---|---|
| Bencode/metainfo parsing | `tst_bencode`, `tst_metainfo` | — |
| Bitfield/piece store/verify | `tst_bitfield`, `tst_piecestore` | Real-file byte-verification (step 4) |
| Piece picker strategies | `tst_piecepicker` | Observing a live strategy switch (step 6) |
| Peer wire / PeerConnection | `tst_peerwire`, `tst_seeder` | — |
| HTTP tracker client | `tst_tracker` | Live tracker over the real internet |
| TorrentTask engine/resume/rate limiting | `tst_torrent`, `tst_ratelimiter` | Real restart (step 5), real cap observation (step 8) |
| DownloadManager add/session/settings plumbing | `tst_torrent`, `tst_settings` | — |
| Open dialog / file selection / drag & drop | `tst_gui` (selection logic) | Actual file dialog / OS drag gesture (steps 1–3) |
| MainWindow context menu / Preferences wiring | `tst_gui` | Full end-to-end UI flow (steps 6, 7, 9) |

None of steps 1–9 above run in CI; they are the maintainer's manual gate before merge, per the
spec's Task 15 hand-off.
