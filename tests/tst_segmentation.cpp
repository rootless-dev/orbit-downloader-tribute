#include <QtTest>
#include "Segmentation.h"

class TstSegmentation : public QObject {
    Q_OBJECT
private slots:
    void splitsEvenly() {
        auto s = computeSegments(1000, true, 4, 1);   // minSeg=1 so no clamp
        QCOMPARE(s.size(), 4);
        QCOMPARE(s[0].start, 0LL);   QCOMPARE(s[0].end, 249LL);
        QCOMPARE(s[1].start, 250LL); QCOMPARE(s[1].end, 499LL);
        QCOMPARE(s[3].end, 999LL);                    // last covers remainder
        // contiguous, no gaps/overlap
        for (int i = 1; i < s.size(); ++i)
            QCOMPARE(s[i].start, s[i-1].end + 1);
    }
    void lastAbsorbsRemainder() {
        auto s = computeSegments(1003, true, 4, 1);
        QCOMPARE(s.last().end, 1002LL);
        QCOMPARE(s[0].end - s[0].start + 1, 250LL);   // 1003/4 = 250
        QCOMPARE(s.last().end - s.last().start + 1, 253LL);
    }
    void clampsToMinSegSize() {
        auto s = computeSegments(1000, true, 8, 400); // ceil(1000/400)=3 max segs
        QCOMPARE(s.size(), 3);
    }
    void fallbackWhenNoRange() {
        auto s = computeSegments(1000, false, 4, 1);
        QCOMPARE(s.size(), 1);
        QCOMPARE(s[0].start, 0LL);
        QCOMPARE(s[0].end, -1LL);
    }
    void fallbackWhenUnknownSize() {
        auto s = computeSegments(-1, true, 4, 1);
        QCOMPARE(s.size(), 1);
        QCOMPARE(s[0].end, -1LL);
    }
    void currentStartsAtStart() {
        auto s = computeSegments(1000, true, 4, 1);
        for (const auto& seg : s) QCOMPARE(seg.current, seg.start);
    }

    // --- planSplit (divisão dinâmica) ---------------------------------

    void splitPicksTheLongestRemaining() {
        auto s = computeSegments(1000, true, 4, 1);   // 4 x 250
        s[0].current = s[0].end + 1;                  // completo
        s[1].current = 300;                           // resta 200
        s[2].current = 520;                           // resta 230  <- maior
        s[3].current = 800;                           // resta 200
        const auto p = planSplit(s, 1);
        QVERIFY(p.ok);
        QCOMPARE(p.index, 2);
        QCOMPARE(p.splitAt, 520 + 230 / 2);
    }

    void splitLeavesBothHalvesNonEmpty() {
        auto s = computeSegments(1000, true, 4, 1);
        const auto p = planSplit(s, 1);
        QVERIFY(p.ok);
        QVERIFY(p.splitAt > s[p.index].current);      // sobra trabalho p/ quem encolhe
        QVERIFY(p.splitAt <= s[p.index].end);         // e p/ quem recebe
    }

    void splitSkipsCompleteSegments() {
        auto s = computeSegments(1000, true, 2, 1);
        s[0].current = s[0].end + 1;                  // completo, resto "grande" se contado errado
        s[1].current = s[1].end - 9;                  // restam 10
        const auto p = planSplit(s, 1);
        QVERIFY(p.ok);
        QCOMPARE(p.index, 1);
    }

    void splitSkipsFallbackSegment() {
        QVector<Segment> s{Segment{0, 0, 0, -1}};     // sem Range: não dá p/ dividir
        QVERIFY(!planSplit(s, 1).ok);
    }

    void splitRefusedWhenRemainderTooSmall() {
        auto s = computeSegments(1000, true, 1, 1);   // 1 segmento de 1000
        s[0].current = 300;                           // restam 700
        QVERIFY(planSplit(s, 350).ok);                // cabe 2 x 350
        QVERIFY(!planSplit(s, 351).ok);               // não cabe -> deixa terminar
    }

    void splitOfAllCompleteIsRefused() {
        auto s = computeSegments(1000, true, 4, 1);
        for (auto& seg : s) seg.current = seg.end + 1;
        QVERIFY(!planSplit(s, 1).ok);
    }
};

QTEST_MAIN(TstSegmentation)
#include "tst_segmentation.moc"
