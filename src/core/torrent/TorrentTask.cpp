#include "torrent/TorrentTask.h"

#include "Logger.h"
#include "torrent/AnnounceController.h"
#include "torrent/PeerConnection.h"
#include "torrent/PieceStore.h"

#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QTimer>

#include <algorithm>
#include <random>

namespace {

// Resume-file framing (spec §12): a small header validated against the
// metainfo on restore, then the raw bitfield bytes.
constexpr quint32 kResumeMagic = 0x4F424254; // "OBBT"
constexpr quint32 kResumeVersion = 1;

// Per-peer request pipeline depth (BEP 3 recommends keeping several requests
// outstanding to keep the pipe full).
constexpr int kPipelineDepth = 8;

// Drop a peer after this many pieces it contributed to failed verification.
constexpr int kMaxBadPieces = 5;

// Progress signal cap: at most one emit per second (≤1 Hz).
constexpr int kProgressThrottleMs = 1000;

// Debounce window before flushing the resume bitfield to disk.
constexpr int kResumeDebounceMs = 500;

// Heartbeat diagnostics cadence (part B): log a one-line peer/piece status
// snapshot, and take the opportunity to re-evaluate the announce cadence.
constexpr int kHeartbeatMs = 5000;

// Adaptive re-announce cadence (part C): a peer-starved or stalled torrent
// (e.g. a tracker returning only 1 peer, no DHT) must not sit idle for the
// full 1800s tracker interval — it re-announces every 30-90s instead, until
// it has enough peers AND is actually making progress, at which point it
// backs off to the tracker-provided interval.
constexpr int kHealthyPeers = 4;
constexpr int kMinAnnounceSecs = 30;
constexpr int kStarvedCeilSecs = 90;

QString peerKey(const PeerAddress& a) {
    return a.host + QLatin1Char(':') + QString::number(a.port);
}

// Seconds until the next tracker announce. PURE and clock/RNG-free: given the
// same inputs it always returns the same delay, so it is directly unit
// testable (see TorrentTask::nextAnnounceDelaySecsForTest).
//
// minIntervalSecs is the tracker's BEP 3 "min interval" (0 if it never sent
// one): a floor on re-announce frequency that must be honored even while
// starved, or a strict tracker can rate-limit/ban an otherwise well-behaved
// client. When starved: never faster than max(kMinAnnounceSecs, minInterval),
// and if the tracker demands a min interval bigger than our usual starved
// ceiling, that larger floor wins (qBound clamps its own bound arguments, so
// passing a hi below lo would be a bug — qMax(90, minIntervalSecs) keeps hi
// always >= lo here).
int nextAnnounceDelaySecsImpl(int connectedPeers, bool hasWantedProgress, int trackerIntervalSecs,
                              int minIntervalSecs) {
    const int fallback = trackerIntervalSecs > 0 ? trackerIntervalSecs : 1800;
    if (connectedPeers < kHealthyPeers || !hasWantedProgress) {
        const int lo = qMax(kMinAnnounceSecs, minIntervalSecs);
        const int hi = qMax(kStarvedCeilSecs, minIntervalSecs);
        return qBound(lo, fallback, hi);
    }
    return fallback;
}

} // namespace

TorrentTask::TorrentTask(const TorrentMetainfo& m, const QString& destDir,
                         const QSet<int>& selectedFiles, PieceStrategy strategy, quint16 listenPort,
                         int maxPeers, ResumeVerifyMode verify, quint32 rngSeed,
                         QNetworkAccessManager* nam, RateLimiter* limiter, Logger* logger,
                         const QString& resumeDir, QObject* parent)
    : AbstractTask(parent), m_meta(m), m_destDir(destDir), m_selectedFiles(selectedFiles),
      m_strategy(strategy), m_listenPort(listenPort), m_maxPeers(maxPeers), m_verifyMode(verify),
      m_rngSeed(rngSeed), m_nam(nam), m_limiter(limiter), m_logger(logger), m_resumeDir(resumeDir),
      m_pieceCount(m.pieceHashes.size()), m_have(m.pieceHashes.size()) {
    qRegisterMetaType<PieceState>("PieceState");

    // Deterministic task id + peer_id from the metainfo/seed: no global RNG,
    // no wall clock, so a given torrent+seed always reproduces (spec §9).
    m_id = QUuid::fromRfc4122(m_meta.infoHash.left(16));

    std::mt19937 rng(m_rngSeed);
    m_peerId = QByteArray("-OB0001-");
    for (int i = 0; i < 12; ++i) m_peerId.append(char(rng() & 0xFF));

    m_store = std::make_unique<PieceStore>(m_meta, m_destDir, m_selectedFiles);
    const QVector<int> wanted = m_store->wantedPieces();
    m_wanted = QSet<int>(wanted.begin(), wanted.end());
    m_picker = std::make_unique<PiecePicker>(m_pieceCount, m_meta.pieceLength, m_meta.totalLength,
                                             wanted, m_strategy, m_rngSeed);

    m_pieceStates = QVector<PieceState>(m_pieceCount, PieceState::Missing);

    // total = wanted bytes (selected files' portions of the wanted pieces).
    for (int p : wanted) m_totalWantedBytes += pieceWantedBytes(p);
}

TorrentTask::~TorrentTask() {
    // Flush any debounced resume state that hasn't hit disk yet, so a task
    // destroyed mid-flight (the "restart" resume path) never loses progress.
    if (m_resumePending) saveResumeNow();
    // Peers are QObject children; Qt tears them down. unique_ptr members
    // (PieceStore/PiecePicker) need the complete type here — provided by the
    // includes above.
}

// ---------------------------------------------------------------------------
// State machine
// ---------------------------------------------------------------------------

void TorrentTask::setState(DownloadState s) {
    if (m_state == s) return;
    m_state = s;
    emit stateChanged(s);
}

void TorrentTask::start() {
    if (m_state == DownloadState::Completed) return;

    if (m_forceRecheck || m_verifyMode == ResumeVerifyMode::RecheckOnOpen) {
        setState(DownloadState::Checking);
        runChecking();
        m_forceRecheck = false;
    } else if (!m_bitfieldRestored) {
        restoreBitfield();
    }

    if (haveAllWanted()) {
        maybeFinish();
        return;
    }
    beginLeeching();
}

void TorrentTask::runChecking() {
    // AUTHORITATIVE re-check: the have-set becomes EXACTLY what verifies on
    // disk right now. Unlike the additive resume path (adoptHaveBitfield),
    // this can DOWNGRADE a piece that was trusted-present (e.g. from a restored
    // bitfield) but is now corrupt or missing on disk, so RecheckOnOpen / Force
    // re-check genuinely detect on-disk corruption instead of silently
    // degrading to TrustBitfield. Only wanted pieces are checked (unselected
    // files are never fetched/written).
    QVector<int> wanted(m_wanted.begin(), m_wanted.end());
    std::sort(wanted.begin(), wanted.end());

    Bitfield verified(m_pieceCount);
    for (int p : wanted) {
        QString err;
        if (m_store->verifyOnDisk(p, &err)) verified.set(p);
    }

    // Discard any in-progress assembly state: the check is the new ground
    // truth, so partial buffers from a prior run must not survive it. The
    // picker's received-block records must be dropped in lockstep, or a
    // mid-download recheck would leave the picker thinking blocks are done
    // while their buffers are gone — stalling the re-download.
    m_pieceBuf.clear();
    m_pieceBlocks.clear();
    for (int p : m_wanted) m_picker->resetPieceBlocks(p);

    // Reset the have-state, then set exactly the verified pieces, recomputing
    // counts from scratch. Emit pieceStateChanged for any piece whose state
    // actually changes (including a Have -> Missing downgrade, which the
    // setPieceState() "Have is terminal" guard would otherwise refuse).
    m_have = Bitfield(m_pieceCount);
    m_wantedHaveCount = 0;
    m_verifiedBytes = 0;
    for (int p : wanted) {
        const bool nowHave = verified.has(p);
        const PieceState newSt = nowHave ? PieceState::Have : PieceState::Missing;
        if (nowHave) {
            m_have.set(p);
            ++m_wantedHaveCount;
            m_verifiedBytes += pieceWantedBytes(p);
        }
        if (m_pieceStates[p] != newSt) {
            m_pieceStates[p] = newSt;
            emit pieceStateChanged(p, newSt);
        }
    }
    m_picker->resetHave(m_have); // non-additive: REPLACE the picker's have-set

    logLine(LogLevel::Info, QStringLiteral("recheck: %1/%2 wanted pieces present")
                                .arg(m_wantedHaveCount).arg(m_wanted.size()));
}

void TorrentTask::adoptHaveBitfield(const Bitfield& bf, bool emitStates) {
    const int n = std::min(bf.size(), m_pieceCount);
    for (int p = 0; p < n; ++p) {
        if (!bf.has(p) || m_have.has(p)) continue;
        m_have.set(p);
        if (m_wanted.contains(p)) {
            ++m_wantedHaveCount;
            m_verifiedBytes += pieceWantedBytes(p);
        }
        m_pieceStates[p] = PieceState::Have;
        if (emitStates) emit pieceStateChanged(p, PieceState::Have);
    }
    m_picker->setHave(m_have); // additive merge (never forgets)
}

bool TorrentTask::haveAllWanted() const { return m_wantedHaveCount >= m_wanted.size(); }

void TorrentTask::beginLeeching() {
    setState(DownloadState::Connecting);

    if (!m_meta.announceList.isEmpty()) {
        if (!m_announce) {
            m_announce = new AnnounceController(m_meta.announceList, m_nam, m_rngSeed, this);
            connect(m_announce, &AnnounceController::peersReceived, this,
                    [this](QVector<PeerAddress> peers, int interval, int minInterval) {
                        int newCount = 0;
                        for (const auto& p : peers) {
                            const QString k = peerKey(p);
                            if (m_knownPeers.contains(k)) continue;
                            m_knownPeers.insert(k);
                            m_pendingPeers.append(p);
                            ++newCount;
                        }
                        if (interval > 0) m_announceIntervalSecs = interval;
                        if (minInterval > 0) m_minAnnounceIntervalSecs = minInterval;
                        m_trackerStatus = QStringLiteral("OK: %1 peers (interval %2s)")
                                              .arg(peers.size())
                                              .arg(m_announceIntervalSecs > 0 ? m_announceIntervalSecs : 1800);
                        logLine(LogLevel::Info,
                                QStringLiteral("tracker: %1 peers received (%2 new), interval %3s")
                                    .arg(peers.size()).arg(newCount).arg(interval));
                        rescheduleAnnounceTimer();
                        openPeers();
                    });
            connect(m_announce, &AnnounceController::announceFailed, this,
                    [this](const QString& why) {
                        m_trackerStatus = QStringLiteral("failed: %1").arg(why);
                        logLine(LogLevel::Warn, QStringLiteral("announce failed: %1").arg(why));
                        rescheduleAnnounceTimer();
                    });
        }
        const qint64 left = m_totalWantedBytes - m_verifiedBytes;
        m_trackerStatus = QStringLiteral("announcing…");
        m_announce->announce(m_meta.infoHash, m_peerId, m_listenPort, m_verifiedBytes, left,
                             TrackerEvent::Started, /*hungry*/false);

        if (!m_announceTimer) {
            m_announceTimer = new QTimer(this);
            connect(m_announceTimer, &QTimer::timeout, this, [this] {
                if (!m_announce) return;
                const qint64 left = m_totalWantedBytes - m_verifiedBytes;
                // Hungry when peer-starved: widen to every tracker to refill fast.
                const bool hungry = (connectedPeerCount() == 0);
                m_announce->announce(m_meta.infoHash, m_peerId, m_listenPort, m_verifiedBytes, left,
                                     TrackerEvent::None, hungry);
            });
        }
        rescheduleAnnounceTimer(); // starved-by-default cadence until the first response lands
    }

    if (!m_heartbeatTimer) {
        m_heartbeatTimer = new QTimer(this);
        connect(m_heartbeatTimer, &QTimer::timeout, this, [this] {
            logLine(LogLevel::Info,
                    QStringLiteral("status: %1 peers (%2 unchoked), %3/%4 pieces")
                        .arg(connectedPeerCount()).arg(unchokedPeerCount())
                        .arg(m_wantedHaveCount).arg(m_wanted.size()));
            rescheduleAnnounceTimer();
        });
    }
    m_heartbeatTimer->start(kHeartbeatMs);

    openPeers(); // includes any test-injected peers
}

void TorrentTask::openPeers() {
    if (m_state != DownloadState::Connecting && m_state != DownloadState::Downloading) return;
    while (m_peers.size() < m_maxPeers && !m_pendingPeers.isEmpty()) {
        const PeerAddress addr = m_pendingPeers.takeFirst();
        auto* pc = new PeerConnection(addr, m_meta.infoHash, m_peerId, m_pieceCount, m_limiter, this);
        m_peers.append(pc);
        wirePeer(pc);
        pc->connectToPeer();
    }
}

void TorrentTask::wirePeer(PeerConnection* pc) {
    connect(pc, &PeerConnection::handshakeOk, this, [this, pc] { onHandshake(pc); });
    connect(pc, &PeerConnection::bitfieldReceived, this, [this, pc] {
        m_picker->addPeerBitfield(pc->peerBitfield());
        logLine(LogLevel::Info, QStringLiteral("peer %1 has %2/%3 pieces")
                                    .arg(pc->label())
                                    .arg(pc->peerBitfield().count())
                                    .arg(pc->peerBitfield().size()));
        requestMore(pc);
    });
    connect(pc, &PeerConnection::haveReceived, this, [this, pc](int piece) {
        m_picker->peerHas(piece);
        requestMore(pc);
    });
    connect(pc, &PeerConnection::unchoked, this, [this, pc] { onUnchoked(pc); });
    connect(pc, &PeerConnection::choked, this, [this, pc] {
        logLine(LogLevel::Info, QStringLiteral("peer %1 choked us").arg(pc->label()));
    });
    connect(pc, &PeerConnection::blockReceived, this,
            [this, pc](int piece, qint64 begin, QByteArray data) { onBlock(pc, piece, begin, data); });
    connect(pc, &PeerConnection::disconnected, this,
            [this, pc](const QString& why) {
                logLine(LogLevel::Debug, QStringLiteral("peer disconnected: %1").arg(why));
                onPeerDisconnected(pc);
            });
}

void TorrentTask::onHandshake(PeerConnection* pc) {
    logLine(LogLevel::Info, QStringLiteral("peer %1 connected (handshake ok)").arg(pc->label()));
    pc->sendInterested();
}

void TorrentTask::onUnchoked(PeerConnection* pc) {
    logLine(LogLevel::Info, QStringLiteral("peer %1 unchoked us").arg(pc->label()));
    if (m_state == DownloadState::Connecting) setState(DownloadState::Downloading);
    requestMore(pc);
}

void TorrentTask::requestMore(PeerConnection* pc) {
    if (m_state != DownloadState::Connecting && m_state != DownloadState::Downloading) return;
    if (!pc->amUnchoked()) return;
    int freeSlots = kPipelineDepth - m_inflightByPeer[pc].size();
    if (freeSlots <= 0) return;
    const QVector<BlockRequest> blocks = m_picker->pick(pc->peerBitfield(), freeSlots);
    for (const auto& b : blocks) {
        m_picker->markInFlight(b);
        m_inflightByPeer[pc].append(b);
        setPieceState(b.piece, PieceState::InFlight);
        pc->sendRequest(b.piece, b.begin, b.length);
    }
}

void TorrentTask::pumpPeers() {
    // Top up every connected, unchoked peer's pipeline. Called after any event
    // that frees or creates pickable work (a dropped peer's blocks released, a
    // piece failed and re-opened, a peer newly unchoked / newly-available
    // pieces), so freed work always reaches an otherwise-idle peer instead of
    // relying solely on the reactive per-peer onBlock() path — which never
    // fires again for a peer that has already delivered all its assigned
    // blocks. requestMore() respects pipeline depth + in-flight dedup, so this
    // is idempotent. m_pumping guards against re-entrancy (requestMore ->
    // sendRequest is synchronous and cannot re-enter here, but a defensive
    // guard keeps a future change from looping).
    if (m_pumping) return;
    m_pumping = true;
    // Iterate a copy: requestMore() must not mutate m_peers, but a copy keeps
    // this safe even if a future edit makes it drop a peer mid-loop.
    const QVector<PeerConnection*> peers = m_peers;
    for (auto* pc : peers) requestMore(pc);
    m_pumping = false;
}

void TorrentTask::onBlock(PeerConnection* pc, int piece, qint64 begin, const QByteArray& data) {
    if (!m_firstBlockLogged.contains(pc)) {
        m_firstBlockLogged.insert(pc);
        logLine(LogLevel::Info, QStringLiteral("receiving data from peer %1").arg(pc->label()));
    }

    // Free the pipeline slot this block occupied (match by piece+begin).
    auto& list = m_inflightByPeer[pc];
    for (int i = 0; i < list.size(); ++i) {
        if (list[i].piece == piece && list[i].begin == begin) {
            m_picker->clearInFlight(list[i]);
            list.removeAt(i);
            break;
        }
    }

    if (m_have.has(piece) || !m_wanted.contains(piece)) {
        requestMore(pc);
        return;
    }

    const qint64 psize = m_store->pieceSize(piece);
    QByteArray& buf = m_pieceBuf[piece];
    if (buf.isEmpty()) buf = QByteArray(psize, '\0');
    // Only an in-bounds block counts toward completion: a rejected (malformed)
    // block must not fill a slot in m_pieceBlocks, or the set could reach
    // expectedBlockCount with a zero-filled gap and force a needless
    // verify-fail / re-download loop. The slot was already freed
    // (clearInFlight above), so the picker can re-request this block.
    if (begin >= 0 && begin + data.size() <= psize) {
        buf.replace(begin, int(data.size()), data);
        m_pieceBlocks[piece].insert(begin);
        // Tell the picker this block is DONE so pick() advances to the next
        // un-received block instead of re-offering this one forever (the
        // multi-block-piece hang). clearInFlight above only freed the slot.
        m_picker->markBlockReceived(piece, begin);
    }

    if (m_pieceBlocks[piece].size() >= expectedBlockCount(piece)) {
        const QByteArray whole = m_pieceBuf.value(piece);
        const bool ok = m_store->verify(piece, whole);
        m_pieceBuf.remove(piece);
        m_pieceBlocks.remove(piece);
        if (ok) {
            completePiece(piece, whole);
            if (m_state == DownloadState::Completed) return; // torn down; don't touch pc
        } else {
            if (failPiece(pc, piece)) return; // pc was dropped & deleteLater'd; don't touch it
        }
    }

    requestMore(pc);
}

void TorrentTask::completePiece(int piece, const QByteArray& whole) {
    QString err;
    if (!m_store->writePiece(piece, whole, &err)) {
        logLine(LogLevel::Error, QStringLiteral("writePiece(%1) failed: %2").arg(piece).arg(err));
        if (m_announceTimer) m_announceTimer->stop();
        if (m_heartbeatTimer) m_heartbeatTimer->stop();
        setState(DownloadState::Error);
        return;
    }
    m_have.set(piece);
    m_picker->markPieceComplete(piece);
    if (m_wanted.contains(piece)) {
        ++m_wantedHaveCount;
        m_verifiedBytes += pieceWantedBytes(piece);
    }
    setPieceState(piece, PieceState::Have);
    scheduleProgress();
    scheduleResumeSave();
    maybeFinish();
}

bool TorrentTask::failPiece(PeerConnection* pc, int piece) {
    logLine(LogLevel::Warn, QStringLiteral("piece %1 failed hash check; re-requesting").arg(piece));
    setPieceState(piece, PieceState::Missing); // picker will re-pick (not in have)
    // Re-open EVERY block of this piece for re-download: the whole piece must be
    // re-fetched from scratch. onBlock() already dropped this piece's
    // m_pieceBuf/m_pieceBlocks entries before calling us; clear the picker's
    // received-block record too so task and picker agree the piece restarts.
    m_pieceBuf.remove(piece);
    m_pieceBlocks.remove(piece);
    m_picker->resetPieceBlocks(piece);
    if (++m_badPieces[pc] >= kMaxBadPieces) {
        logLine(LogLevel::Warn, QStringLiteral("dropping peer after %1 bad pieces").arg(kMaxBadPieces));
        dropPeer(pc); // releases the peer's in-flight blocks back to the picker + pumpPeers()
        openPeers();
        return true;
    }
    // Piece re-opened (its blocks were already delivered, so it has no in-flight
    // bookkeeping left): make sure some peer picks it up even if the delivering
    // peer goes idle.
    pumpPeers();
    return false;
}

void TorrentTask::onPeerDisconnected(PeerConnection* pc) {
    // A peer dropping mid-download must release its in-flight blocks back to
    // the picker, or PiecePicker::isFullyCovered would permanently block
    // every other peer from re-picking those pieces (hang). dropPeer() does
    // exactly that.
    dropPeer(pc);
    openPeers(); // refill from any pending peers
}

void TorrentTask::dropPeer(PeerConnection* pc) {
    for (const auto& b : m_inflightByPeer.value(pc)) m_picker->clearInFlight(b);
    m_inflightByPeer.remove(pc);
    m_badPieces.remove(pc);
    m_firstBlockLogged.remove(pc);
    m_peers.removeAll(pc);
    pc->disconnect(this);
    pc->deleteLater();
    // The blocks just released are re-pickable now: hand them to any idle peer
    // (the surviving good peer may have no outstanding request, so its reactive
    // onBlock() path would never fire again — see pumpPeers()).
    pumpPeers();
}

void TorrentTask::setPieceState(int piece, PieceState st) {
    if (piece < 0 || piece >= m_pieceCount) return;
    if (m_pieceStates[piece] == st) return;
    if (m_pieceStates[piece] == PieceState::Have && st != PieceState::Have) return; // Have is terminal
    m_pieceStates[piece] = st;
    emit pieceStateChanged(piece, st);
}

void TorrentTask::maybeFinish() {
    if (!haveAllWanted()) return;

    if (m_announce) {
        m_announce->announce(m_meta.infoHash, m_peerId, m_listenPort, m_verifiedBytes, 0,
                             TrackerEvent::Completed, false);
    }
    if (m_announceTimer) m_announceTimer->stop();
    if (m_heartbeatTimer) m_heartbeatTimer->stop();
    teardownPeers();
    saveResumeNow();
    emitProgressNow();
    logLine(LogLevel::Info, QStringLiteral("download complete: %1").arg(m_meta.name));
    setState(DownloadState::Completed);
}

// ---------------------------------------------------------------------------
// pause / requeue / cancel
// ---------------------------------------------------------------------------

void TorrentTask::pause() {
    if (m_announce) {
        const qint64 left = m_totalWantedBytes - m_verifiedBytes;
        m_announce->announce(m_meta.infoHash, m_peerId, m_listenPort, m_verifiedBytes, left,
                             TrackerEvent::Stopped, false);
    }
    if (m_announceTimer) m_announceTimer->stop();
    if (m_heartbeatTimer) m_heartbeatTimer->stop();
    teardownPeers();
    if (m_progressPending) emitProgressNow();
    if (m_progressTimer) m_progressTimer->stop();
    saveResumeNow(); // keep the bitfield
    if (m_state != DownloadState::Completed) setState(DownloadState::Paused);
}

void TorrentTask::requeue() {
    if (m_state == DownloadState::Paused || m_state == DownloadState::Error ||
        m_state == DownloadState::Cancelled)
        setState(DownloadState::Queued);
}

void TorrentTask::cancel() {
    if (m_announce) {
        const qint64 left = m_totalWantedBytes - m_verifiedBytes;
        m_announce->announce(m_meta.infoHash, m_peerId, m_listenPort, m_verifiedBytes, left,
                             TrackerEvent::Stopped, false);
    }
    if (m_announceTimer) m_announceTimer->stop();
    if (m_heartbeatTimer) m_heartbeatTimer->stop();
    teardownPeers();
    if (m_progressTimer) m_progressTimer->stop();
    m_progressPending = false;
    setState(DownloadState::Cancelled);
}

void TorrentTask::teardownPeers() {
    for (auto* pc : m_peers) {
        for (const auto& b : m_inflightByPeer.value(pc)) m_picker->clearInFlight(b);
        pc->disconnect(this);
        pc->deleteLater();
    }
    m_peers.clear();
    m_inflightByPeer.clear();
    m_badPieces.clear();
    m_firstBlockLogged.clear();
}

// ---------------------------------------------------------------------------
// Public knobs / accessors
// ---------------------------------------------------------------------------

void TorrentTask::setStrategy(PieceStrategy s) {
    m_strategy = s;
    if (m_picker) m_picker->setStrategy(s);
}

void TorrentTask::forceRecheck() {
    // Run a check NOW, from ANY state (Completed, Downloading, Paused, ...) —
    // not a deferred flag that start() consumes (and that start() would ignore
    // for an already-Completed torrent). Tear down any active peers/announce
    // first so runChecking()/beginLeeching() start from a clean slate (no
    // double peer set, no dangling in-flight, no UAF), then drive the same
    // Checking entrypoint start() uses: re-verify on disk, and either settle
    // back to Completed if everything still verifies or resume leeching for any
    // piece that is now missing/corrupt.
    m_forceRecheck = false; // consumed here; start() must not re-trigger it
    if (m_announceTimer) m_announceTimer->stop();
    if (m_heartbeatTimer) m_heartbeatTimer->stop();
    teardownPeers();

    setState(DownloadState::Checking);
    runChecking();

    if (haveAllWanted()) {
        maybeFinish();
        return;
    }
    beginLeeching();
}

QString TorrentTask::payloadRootPath() const {
    return m_store ? m_store->destRootPath() : m_destDir;
}

PieceState TorrentTask::pieceState(int i) const {
    if (i < 0 || i >= m_pieceStates.size()) return PieceState::Missing;
    return m_pieceStates[i];
}

int TorrentTask::unchokedPeerCount() const {
    int n = 0;
    for (auto* pc : m_peers)
        if (pc->amUnchoked()) ++n;
    return n;
}

int TorrentTask::nextAnnounceDelaySecsForTest(int connectedPeers, bool hasWantedProgress,
                                              int trackerIntervalSecs, int minIntervalSecs) {
    return nextAnnounceDelaySecsImpl(connectedPeers, hasWantedProgress, trackerIntervalSecs, minIntervalSecs);
}

void TorrentTask::rescheduleAnnounceTimer() {
    if (!m_announceTimer) return;
    // "Progress" means verified bytes actually moving, not merely having
    // peers connected (spec: don't conflate the two).
    const bool hasWantedProgress = m_verifiedBytes > 0;
    const int delaySecs = nextAnnounceDelaySecsImpl(connectedPeerCount(), hasWantedProgress,
                                                    m_announceIntervalSecs, m_minAnnounceIntervalSecs);
    m_announceTimer->start(delaySecs * 1000);
}

void TorrentTask::addPeerForTest(const PeerAddress& p) {
    const QString k = peerKey(p);
    if (m_knownPeers.contains(k)) return;
    m_knownPeers.insert(k);
    m_pendingPeers.append(p);
    openPeers(); // no-op unless already leeching
}

// ---------------------------------------------------------------------------
// Progress throttle (≤1 Hz), like DownloadTask
// ---------------------------------------------------------------------------

void TorrentTask::scheduleProgress() {
    if (!m_progressTimer) {
        m_progressTimer = new QTimer(this);
        m_progressTimer->setSingleShot(true);
        connect(m_progressTimer, &QTimer::timeout, this, &TorrentTask::emitProgressNow);
    }
    if (!m_progressTimer->isActive() && !m_progressPending) {
        m_progressPending = true;
        m_progressTimer->start(kProgressThrottleMs);
    }
}

void TorrentTask::emitProgressNow() {
    m_progressPending = false;
    if (m_progressTimer) m_progressTimer->stop();
    emit progress(m_verifiedBytes, m_totalWantedBytes);
}

// ---------------------------------------------------------------------------
// Resume persistence (spec §12)
// ---------------------------------------------------------------------------

QString TorrentTask::resumeFilePath() const {
    return QDir(m_resumeDir).filePath(QString::fromLatin1(m_meta.infoHash.toHex()) +
                                      QStringLiteral(".bitfield"));
}

void TorrentTask::scheduleResumeSave() {
    if (!m_resumeTimer) {
        m_resumeTimer = new QTimer(this);
        m_resumeTimer->setSingleShot(true);
        connect(m_resumeTimer, &QTimer::timeout, this, &TorrentTask::saveResumeNow);
    }
    m_resumePending = true;
    m_resumeTimer->start(kResumeDebounceMs); // debounce: reset on each completion
}

void TorrentTask::saveResumeNow() {
    m_resumePending = false;
    if (m_resumeTimer) m_resumeTimer->stop();
    if (m_resumeDir.isEmpty()) return;
    QDir().mkpath(m_resumeDir);
    QFile f(resumeFilePath());
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        logLine(LogLevel::Warn, QStringLiteral("failed to write resume file %1").arg(resumeFilePath()));
        return;
    }
    QDataStream ds(&f);
    ds.setByteOrder(QDataStream::BigEndian);
    ds << kResumeMagic << kResumeVersion << qint32(m_pieceCount) << qint64(m_meta.pieceLength)
       << qint64(m_meta.totalLength) << m_have.toBytes();
}

void TorrentTask::restoreBitfield() {
    m_bitfieldRestored = true;
    QFile f(resumeFilePath());
    if (!f.open(QIODevice::ReadOnly)) return;

    QDataStream ds(&f);
    ds.setByteOrder(QDataStream::BigEndian);
    quint32 magic = 0, version = 0;
    qint32 pieceCount = 0;
    qint64 pieceLength = 0, totalLength = 0;
    QByteArray bits;
    ds >> magic >> version >> pieceCount >> pieceLength >> totalLength >> bits;
    if (ds.status() != QDataStream::Ok) return;

    // Header must match this metainfo exactly; otherwise treat as no progress.
    if (magic != kResumeMagic || version != kResumeVersion || pieceCount != m_pieceCount ||
        pieceLength != m_meta.pieceLength || totalLength != m_meta.totalLength)
        return;

    const Bitfield bf = Bitfield::fromBytes(bits, m_pieceCount);
    adoptHaveBitfield(bf, /*emitStates=*/true);
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

int TorrentTask::expectedBlockCount(int piece) const {
    const qint64 sz = m_store->pieceSize(piece);
    const qint64 bs = PiecePicker::kBlockSize;
    return int((sz + bs - 1) / bs);
}

qint64 TorrentTask::pieceWantedBytes(int piece) const {
    // Bytes of `piece` that fall inside a selected file. Summed over all wanted
    // pieces this equals the total selected-file size, so progress reaches 100%.
    const qint64 pieceStart = qint64(piece) * m_meta.pieceLength;
    const qint64 pieceEnd = pieceStart + m_store->pieceSize(piece);
    qint64 sum = 0;
    for (int fi : m_selectedFiles) {
        if (fi < 0 || fi >= m_meta.files.size()) continue;
        const FileEntry& file = m_meta.files[fi];
        const qint64 lo = std::max(pieceStart, file.offset);
        const qint64 hi = std::min(pieceEnd, file.offset + file.length);
        if (hi > lo) sum += hi - lo;
    }
    return sum;
}

void TorrentTask::logLine(LogLevel level, const QString& msg) {
    if (m_logger) m_logger->logTask(m_id, m_store ? m_store->destRootPath() : m_destDir, level, msg);
}
