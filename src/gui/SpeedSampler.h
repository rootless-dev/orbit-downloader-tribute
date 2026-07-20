#pragma once
#include <QVector>
#include <QtGlobal>

class SpeedSampler {
public:
    void   addSample(qint64 bytes, qint64 tMs);
    double bytesPerSec() const;
    qint64 etaSeconds(qint64 totalBytes) const;
    void   reset();
private:
    struct Sample { qint64 bytes; qint64 tMs; };
    QVector<Sample> m_samples;
    static constexpr qint64 kWindowMs = 5000;
};
