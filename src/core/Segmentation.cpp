#include "Segmentation.h"
#include <algorithm>

QVector<Segment> computeSegments(qint64 totalBytes, bool supportsRange,
                                 int segmentCount, qint64 minSegSize) {
    QVector<Segment> segs;
    if (!supportsRange || totalBytes <= 0) {
        segs.append(Segment{0, 0, 0, -1});
        return segs;
    }
    const qint64 minSeg = std::max<qint64>(1, minSegSize);
    const int maxByMin  = static_cast<int>((totalBytes + minSeg - 1) / minSeg);
    const int n         = std::max(1, std::min(segmentCount, maxByMin));
    const qint64 base   = totalBytes / n;
    qint64 offset = 0;
    for (int i = 0; i < n; ++i) {
        const qint64 len = (i == n - 1) ? (totalBytes - offset) : base;
        Segment s;
        s.index   = i;
        s.start   = offset;
        s.current = offset;
        s.end     = offset + len - 1;
        segs.append(s);
        offset += len;
    }
    return segs;
}

SplitPlan planSplit(const QVector<Segment>& segments, qint64 minSplitBytes) {
    const qint64 minHalf = std::max<qint64>(1, minSplitBytes);
    SplitPlan best;
    qint64 bestRemaining = 0;
    for (const Segment& s : segments) {
        if (s.end < 0 || s.isComplete()) continue;
        const qint64 remaining = s.end - s.current + 1;
        if (remaining < 2 * minHalf) continue;
        if (remaining <= bestRemaining) continue;      // empate -> menor índice (o já visto)
        bestRemaining = remaining;
        best.ok      = true;
        best.index   = s.index;
        best.splitAt = s.current + remaining / 2;
    }
    return best;
}
