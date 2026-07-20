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
