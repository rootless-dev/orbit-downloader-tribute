#pragma once
#include "DownloadTypes.h"

QVector<Segment> computeSegments(qint64 totalBytes, bool supportsRange,
                                 int segmentCount, qint64 minSegSize);
