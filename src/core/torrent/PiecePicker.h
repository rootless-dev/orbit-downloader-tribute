#pragma once

#include <QHash>
#include <QSet>
#include <QVector>
#include <random>

#include "DownloadTypes.h"

class Bitfield;

// A block request within a piece: [begin, begin+length) bytes of `piece`.
struct BlockRequest {
    int piece = 0;
    qint64 begin = 0;
    qint64 length = 0;
};

// Chooses which (piece, block) requests to send to a peer, implementing
// either sequential or rarest-first piece selection (BEP 3 §"Algorithms").
// Pure logic: no I/O, no network, no wall-clock or global RNG.
//
// Availability is a running per-piece count of how many peer bitfields/haves
// have reported owning that piece; RarestFirst uses it to prefer pieces held
// by the fewest peers, with ties broken by a deterministic shuffle seeded
// from the constructor's `rngSeed` (so runs are reproducible for a given
// seed). Candidates for a given pick() call are pieces that are: wanted,
// not already held (setHave/markPieceComplete), not fully in-flight, and
// present in the peer bitfield passed to pick().
class PiecePicker {
public:
    static constexpr qint64 kBlockSize = 1 << 14; // 16 KiB, per BEP 3

    PiecePicker(int pieceCount, qint64 pieceLength, qint64 totalLength,
                const QVector<int>& wantedPieces, PieceStrategy s, quint32 rngSeed);

    void setStrategy(PieceStrategy s);

    // Pieces we already have. Merges (unions) `ours` into the existing
    // "have" set — consistent with the class's other additive mutators
    // (markPieceComplete, peerHas, addPeerBitfield) — so calling this again
    // later (e.g. a periodic resume-state sync in the engine) can only add
    // pieces, never forget ones already marked complete via
    // markPieceComplete.
    void setHave(const Bitfield& ours);

    // Non-additive counterpart to setHave: REPLACES the have-set with exactly
    // `ours` (clears first, then sets the bits in `ours`). Used by the
    // authoritative re-check pass, which must be able to FORGET a piece that
    // was previously marked complete but no longer verifies on disk — the one
    // thing setHave's union semantics deliberately cannot do.
    void resetHave(const Bitfield& ours);

    // Folds a peer's bitfield into the running availability counts.
    void addPeerBitfield(const Bitfield& peer);

    // Folds a single peer 'have' message into the running availability counts.
    void peerHas(int piece);

    // Up to `count` blocks that a peer with `peerBits` can serve, ordered by
    // the active strategy, skipping pieces we have/don't want and blocks
    // already marked in-flight.
    QVector<BlockRequest> pick(const Bitfield& peerBits, int count);

    void markInFlight(const BlockRequest& b);
    void clearInFlight(const BlockRequest& b);

    // Records that (piece, begin) has been RECEIVED and verified into the
    // piece buffer, so pick() must never offer it again until the piece is
    // reset (resetPieceBlocks) or completed (markPieceComplete). This is the
    // fix for multi-block pieces: clearInFlight only frees the pipeline slot,
    // which on its own would let pick() re-offer an already-received low block
    // forever and never advance to the un-received tail of a piece.
    void markBlockReceived(int piece, qint64 begin);

    // Clears the received-block record for `piece` so ALL its blocks become
    // re-requestable again. Used on the verify-failed path, where the whole
    // piece must be re-downloaded from scratch.
    void resetPieceBlocks(int piece);

    // Marks `piece` as held (like setHave for a single piece) and clears any
    // in-flight AND received-block bookkeeping for it (the piece is now "have"
    // and skipped anyway; dropping the record keeps memory bounded).
    void markPieceComplete(int piece);

private:
    qint64 pieceSizeAt(int piece) const;
    QVector<qint64> blockBegins(int piece) const;
    // True iff every block of `piece` is already accounted for by in-flight
    // ∪ received — such a piece offers no pickable work and must not be treated
    // as a candidate that stalls pick().
    bool isFullyCovered(int piece) const;
    QVector<int> orderedCandidates(const Bitfield& peerBits) const;

    int m_pieceCount = 0;
    qint64 m_pieceLength = 0;
    qint64 m_totalLength = 0;
    QVector<int> m_wantedPieces; // insertion order, as given to the constructor (bounds-checked)
    PieceStrategy m_strategy;
    quint32 m_rngSeed = 0;

    QSet<int> m_have;
    QVector<int> m_availability; // per piece, size == m_pieceCount

    QHash<int, QSet<qint64>> m_inFlight; // piece -> begins currently in-flight
    QHash<int, QSet<qint64>> m_received; // piece -> begins already received (done)
};
