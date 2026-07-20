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
};

QTEST_MAIN(TstSegmentation)
#include "tst_segmentation.moc"
