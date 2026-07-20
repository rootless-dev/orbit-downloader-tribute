#include <QtTest>
#include "RateLimiter.h"

class TstRateLimiter : public QObject {
    Q_OBJECT
private slots:
    void unlimitedGrantsEverything() {
        RateLimiter r;                 // taxa default 0
        QCOMPARE(r.take(1'000'000, 0), qint64(1'000'000));
        QCOMPARE(r.take(1'000'000, 0), qint64(1'000'000));
    }
    void cappedByBurstThenRefills() {
        RateLimiter r; r.setRate(1000); // 1000 B/s, burst = 1000
        QCOMPARE(r.take(5000, 0), qint64(1000));  // prime cheio -> concede 1000
        QCOMPARE(r.take(5000, 0), qint64(0));     // sem tokens no mesmo instante
        QCOMPARE(r.take(5000, 1000), qint64(1000));// +1s -> +1000 (cap no burst)
    }
    void partialRefillProportionalToElapsed() {
        RateLimiter r; r.setRate(1000);
        QCOMPARE(r.take(5000, 0), qint64(1000));   // esvazia
        QCOMPARE(r.take(5000, 500), qint64(500));  // 0,5s -> 500 tokens
    }
    void grantsNoMoreThanRequested() {
        RateLimiter r; r.setRate(1000);
        QCOMPARE(r.take(300, 0), qint64(300));     // pediu menos que o bucket
    }
    void settingRateZeroAfterCapReturnsUnlimited() {
        RateLimiter r; r.setRate(1000); r.take(1000, 0);
        r.setRate(0);
        QCOMPARE(r.take(9999, 0), qint64(9999));
    }
    void unlimitedNegativeWantGrantsZero() {
        RateLimiter r;
        QCOMPARE(r.take(-5, 0), qint64(0));
    }
    void cappedNegativeWantGrantsZero() {
        RateLimiter r; r.setRate(1000);
        QCOMPARE(r.take(-5, 0), qint64(0));
    }
};

QTEST_MAIN(TstRateLimiter)
#include "tst_ratelimiter.moc"
