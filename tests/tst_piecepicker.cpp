#include <QtTest>
#include "torrent/PiecePicker.h"
#include "torrent/Bitfield.h"

class TstPiecePicker : public QObject { Q_OBJECT
private slots:
    void sequentialPicksLowestIndex() {
        PiecePicker p(4, 16384, 4*16384, {0,1,2,3}, PieceStrategy::Sequential, 1);
        Bitfield peer(4); peer.set(0); peer.set(2);
        auto b = p.pick(peer, 1); QCOMPARE(b.size(),1); QCOMPARE(b[0].piece, 0);
    }
    void rarestFirstPrefersLowAvailability() {
        PiecePicker p(4, 16384, 4*16384, {0,1,2,3}, PieceStrategy::RarestFirst, 1);
        Bitfield a(4); a.set(0); a.set(1); a.set(2); p.addPeerBitfield(a); // pieces 0,1,2 common
        Bitfield b(4); b.set(0); b.set(1);            p.addPeerBitfield(b);
        Bitfield c(4); c.set(3);                       p.addPeerBitfield(c); // piece 3 rare
        Bitfield peer(4); peer.set(1); peer.set(3);
        auto req = p.pick(peer, 1); QCOMPARE(req[0].piece, 3);  // rarest among what peer has
    }
    void doesNotPickInFlightOrHave() {
        PiecePicker p(2, 16384, 2*16384, {0,1}, PieceStrategy::Sequential, 1);
        Bitfield ours(2); ours.set(0); p.setHave(ours);
        Bitfield peer(2); peer.set(0); peer.set(1);
        auto b = p.pick(peer, 5);
        for (auto& r : b) QVERIFY(r.piece != 0);        // already have piece 0
    }
    void splitsPieceIntoBlocks() {
        PiecePicker p(1, 40000, 40000, {0}, PieceStrategy::Sequential, 1); // 40000 -> 16384+16384+7232
        Bitfield peer(1); peer.set(0);
        auto b = p.pick(peer, 10); QCOMPARE(b.size(), 3);
        QCOMPARE(b[2].length, qint64(40000-2*16384));
    }
    void setHaveMergesWithCompletedPieces() {
        PiecePicker p(3, 16384, 3*16384, {0,1,2}, PieceStrategy::Sequential, 1);
        p.markPieceComplete(0);                 // piece 0 done via the engine
        Bitfield resume(3); resume.set(1);      // resume-state bitfield only covers piece 1
        p.setHave(resume);                      // must NOT forget piece 0
        Bitfield peer(3); peer.set(0); peer.set(1); peer.set(2);
        auto b = p.pick(peer, 5);
        for (auto& r : b) QVERIFY(r.piece != 0);   // still considered "have"
        for (auto& r : b) QVERIFY(r.piece != 1);   // also have, from setHave
    }
    void markInFlightPreventsRepickAndClearReallows() {
        PiecePicker p(1, 16384, 2*16384, {0}, PieceStrategy::Sequential, 1);
        Bitfield peer(1); peer.set(0);
        auto first = p.pick(peer, 1);
        QCOMPARE(first.size(), 1);
        p.markInFlight(first[0]);
        auto second = p.pick(peer, 1);
        QCOMPARE(second.size(), 1);
        QVERIFY(second[0].begin != first[0].begin);   // first block skipped while in-flight
        p.clearInFlight(first[0]);
        auto third = p.pick(peer, 1);
        QCOMPARE(third.size(), 1);
        QCOMPARE(third[0].begin, first[0].begin);      // re-allowed after clearInFlight
    }
    void pickSkipsReceivedBlocksAndAdvances() {
        // A single wanted piece with 4 blocks (pieceLength = 4*16384). This is
        // the path that the old picker got stuck on: it re-offered already
        // RECEIVED low blocks forever instead of advancing to the un-received
        // ones. markBlockReceived teaches it to skip them.
        PiecePicker p(1, 4 * 16384, 4 * 16384, {0}, PieceStrategy::Sequential, 1);
        Bitfield peer(1); peer.set(0);

        auto first = p.pick(peer, 4);
        QCOMPARE(first.size(), 4);
        QCOMPARE(first[0].begin, qint64(0));
        QCOMPARE(first[1].begin, qint64(16384));
        QCOMPARE(first[2].begin, qint64(32768));
        QCOMPARE(first[3].begin, qint64(49152));

        // Blocks 0 and 16384 delivered: free their pipeline slot AND mark them
        // received (the two-step the engine does in onBlock()).
        p.markInFlight(first[0]); p.markInFlight(first[1]);
        p.clearInFlight(first[0]); p.markBlockReceived(0, 0);
        p.clearInFlight(first[1]); p.markBlockReceived(0, 16384);

        // The next pick MUST advance to the un-received blocks, never re-offer
        // the received begins 0 / 16384.
        auto next = p.pick(peer, 4);
        for (auto& r : next) {
            QVERIFY(r.begin != 0);
            QVERIFY(r.begin != 16384);
        }
        QCOMPARE(next.size(), 2);
        QCOMPARE(next[0].begin, qint64(32768));
        QCOMPARE(next[1].begin, qint64(49152));

        // resetPieceBlocks re-opens the whole piece (verify-fail path).
        p.resetPieceBlocks(0);
        auto reset = p.pick(peer, 4);
        QCOMPARE(reset.size(), 4);
        QCOMPARE(reset[0].begin, qint64(0));
        QCOMPARE(reset[1].begin, qint64(16384));
        QCOMPARE(reset[2].begin, qint64(32768));
        QCOMPARE(reset[3].begin, qint64(49152));
    }

    void pickAdvancesAcrossPiecesSkippingReceived() {
        // Two wanted pieces, each with 2 blocks. Proves pick() spans multiple
        // pieces and never re-offers a received block of either.
        PiecePicker p(2, 2 * 16384, 4 * 16384, {0, 1}, PieceStrategy::Sequential, 1);
        Bitfield peer(2); peer.set(0); peer.set(1);

        auto all = p.pick(peer, 4);
        QCOMPARE(all.size(), 4); // p0:0, p0:16384, p1:0, p1:16384

        // Receive the first block of each piece.
        p.markBlockReceived(0, 0);
        p.markBlockReceived(1, 0);

        auto next = p.pick(peer, 4);
        QCOMPARE(next.size(), 2);              // only the two un-received begins
        for (auto& r : next) {
            QCOMPARE(r.begin, qint64(16384));  // never begin 0 (received)
        }
        QSet<int> pieces;
        for (auto& r : next) pieces.insert(r.piece);
        QCOMPARE(pieces.size(), 2);            // spans BOTH pieces
    }

    void shortLastPieceOfMultiPieceTorrent() {
        // 3 pieces of 16384, last piece short: total = 2*16384 + 5000.
        PiecePicker p(3, 16384, 2*16384 + 5000, {0,1,2}, PieceStrategy::Sequential, 1);
        Bitfield peer(3); peer.set(0); peer.set(1); peer.set(2);
        auto b = p.pick(peer, 100);
        QVERIFY(!b.isEmpty());
        auto& last = b.last();
        QCOMPARE(last.piece, 2);
        QCOMPARE(last.length, qint64(5000));
    }
};

QTEST_APPLESS_MAIN(TstPiecePicker)
#include "tst_piecepicker.moc"
