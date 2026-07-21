#include <QtTest>
#include <QLocalSocket>
#include <QSignalSpy>
#include "SingleInstanceGuard.h"

class TstSingleInstance : public QObject {
    Q_OBJECT
    QString name() const { return "orbit-test-" + QString::number(quintptr(this)); }
private slots:
    void firstBecomesPrimary() {
        SingleInstanceGuard g(name());
        QVERIFY(g.tryBecomePrimary(kShowMessage));   // nobody else -> primary
    }
    void secondIsSecondaryAndSignalsShow() {
        const QString n = name();
        SingleInstanceGuard primary(n);
        QVERIFY(primary.tryBecomePrimary(kPingMessage));
        QSignalSpy spy(&primary, &SingleInstanceGuard::showRequested);
        SingleInstanceGuard second(n);
        QVERIFY(!second.tryBecomePrimary(kShowMessage));  // primary exists
        QVERIFY(spy.wait(1000));
        QCOMPARE(spy.count(), 1);
    }
    void pingDoesNotSignalShow() {
        const QString n = name();
        SingleInstanceGuard primary(n);
        QVERIFY(primary.tryBecomePrimary(kPingMessage));
        QSignalSpy spy(&primary, &SingleInstanceGuard::showRequested);
        SingleInstanceGuard second(n);
        QVERIFY(!second.tryBecomePrimary(kPingMessage));  // background secondary
        QTest::qWait(300);
        QCOMPARE(spy.count(), 0);
    }
};
QTEST_MAIN(TstSingleInstance)
#include "tst_singleinstance.moc"
