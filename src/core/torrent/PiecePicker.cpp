#include "torrent/PiecePicker.h"

#include <algorithm>

#include "torrent/Bitfield.h"

PiecePicker::PiecePicker(int pieceCount, qint64 pieceLength, qint64 totalLength,
                          const QVector<int>& wantedPieces, PieceStrategy s, quint32 rngSeed)
    : m_pieceCount(pieceCount), m_pieceLength(pieceLength), m_totalLength(totalLength),
      m_strategy(s), m_rngSeed(rngSeed), m_availability(pieceCount, 0) {
    // Bounds-check here (mirroring peerHas) so a misbehaving caller can't
    // later cause an out-of-bounds m_availability[...] read in the
    // rarest-first comparator.
    for (int p : wantedPieces) {
        if (p >= 0 && p < m_pieceCount) m_wantedPieces.push_back(p);
    }
}

void PiecePicker::setStrategy(PieceStrategy s) { m_strategy = s; }

void PiecePicker::setHave(const Bitfield& ours) {
    // Merge (union), not replace: consistent with the class's other additive
    // mutators, so a later resync can only add pieces, never forget ones
    // already marked complete via markPieceComplete.
    const int n = std::min(ours.size(), m_pieceCount);
    for (int i = 0; i < n; ++i) {
        if (ours.has(i)) m_have.insert(i);
    }
}

void PiecePicker::resetHave(const Bitfield& ours) {
    // Replace, not merge: the re-check pass is authoritative about what is
    // actually present on disk, so a previously-held piece that no longer
    // verifies must be dropped from the have-set and become pickable again.
    m_have.clear();
    const int n = std::min(ours.size(), m_pieceCount);
    for (int i = 0; i < n; ++i) {
        if (ours.has(i)) m_have.insert(i);
    }
}

void PiecePicker::addPeerBitfield(const Bitfield& peer) {
    const int n = std::min(peer.size(), m_pieceCount);
    for (int i = 0; i < n; ++i) {
        if (peer.has(i)) ++m_availability[i];
    }
}

void PiecePicker::peerHas(int piece) {
    if (piece < 0 || piece >= m_pieceCount) return;
    ++m_availability[piece];
}

qint64 PiecePicker::pieceSizeAt(int piece) const {
    if (m_pieceCount > 0 && piece == m_pieceCount - 1) {
        return m_totalLength - qint64(piece) * m_pieceLength;
    }
    return m_pieceLength;
}

QVector<qint64> PiecePicker::blockBegins(int piece) const {
    QVector<qint64> begins;
    const qint64 size = pieceSizeAt(piece);
    for (qint64 off = 0; off < size; off += kBlockSize) begins.push_back(off);
    return begins;
}

bool PiecePicker::isFullyCovered(int piece) const {
    const auto inFlightIt = m_inFlight.find(piece);
    const auto recvIt = m_received.find(piece);
    // Fast reject: if neither set has an entry there is nothing covering it.
    if (inFlightIt == m_inFlight.end() && recvIt == m_received.end()) return false;
    for (qint64 b : blockBegins(piece)) {
        const bool inflight = inFlightIt != m_inFlight.end() && inFlightIt.value().contains(b);
        const bool received = recvIt != m_received.end() && recvIt.value().contains(b);
        if (!inflight && !received) return false;
    }
    return true;
}

QVector<int> PiecePicker::orderedCandidates(const Bitfield& peerBits) const {
    QVector<int> candidates;
    for (int p : m_wantedPieces) {
        if (m_have.contains(p)) continue;
        if (p < 0 || p >= peerBits.size() || !peerBits.has(p)) continue;
        if (isFullyCovered(p)) continue; // every block already in-flight or received
        candidates.push_back(p);
    }

    if (m_strategy == PieceStrategy::Sequential) {
        std::sort(candidates.begin(), candidates.end());
        return candidates;
    }

    // RarestFirst: order by ascending availability. Ties are broken by a
    // deterministic shuffle seeded from m_rngSeed rather than input order,
    // so a fixed seed always yields the same tie-break for the same
    // candidate set, without depending on any global/wall-clock RNG.
    // Re-seeded fresh on every call (deliberate, not an oversight): the same
    // tied candidate set always shuffles to the same order, keeping pick()
    // reproducible call-to-call for a given rngSeed rather than depending on
    // hidden generator state that would drift between calls.
    std::mt19937 rng(m_rngSeed);
    std::shuffle(candidates.begin(), candidates.end(), rng);
    std::stable_sort(candidates.begin(), candidates.end(), [this](int a, int b) {
        return m_availability[a] < m_availability[b];
    });
    return candidates;
}

QVector<BlockRequest> PiecePicker::pick(const Bitfield& peerBits, int count) {
    QVector<BlockRequest> out;
    if (count <= 0) return out;

    const QVector<int> candidates = orderedCandidates(peerBits);
    for (int piece : candidates) {
        const qint64 size = pieceSizeAt(piece);
        const auto inFlightIt = m_inFlight.find(piece);
        const bool hasInFlight = inFlightIt != m_inFlight.end();
        const auto recvIt = m_received.find(piece);
        const bool hasReceived = recvIt != m_received.end();
        for (qint64 begin : blockBegins(piece)) {
            // Skip blocks already in-flight OR already received: re-offering a
            // received block is the multi-block-piece hang this fixes. The loop
            // continues to the next un-covered block (and next piece), so pick()
            // always makes forward progress across pieces.
            if (hasInFlight && inFlightIt.value().contains(begin)) continue;
            if (hasReceived && recvIt.value().contains(begin)) continue;
            const qint64 length = std::min(kBlockSize, size - begin);
            out.push_back(BlockRequest{piece, begin, length});
            if (out.size() >= count) return out;
        }
    }
    return out;
}

void PiecePicker::markInFlight(const BlockRequest& b) {
    m_inFlight[b.piece].insert(b.begin);
}

void PiecePicker::clearInFlight(const BlockRequest& b) {
    const auto it = m_inFlight.find(b.piece);
    if (it == m_inFlight.end()) return;
    it.value().remove(b.begin);
    if (it.value().isEmpty()) m_inFlight.erase(it);
}

void PiecePicker::markBlockReceived(int piece, qint64 begin) {
    m_received[piece].insert(begin);
}

void PiecePicker::resetPieceBlocks(int piece) {
    m_received.remove(piece);
}

void PiecePicker::markPieceComplete(int piece) {
    m_have.insert(piece);
    m_inFlight.remove(piece);
    m_received.remove(piece); // piece is now "have" and skipped; free the record
}
