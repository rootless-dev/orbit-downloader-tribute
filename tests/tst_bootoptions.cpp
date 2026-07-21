#include <QtTest>
#include "BootOptions.h"

class TstBootOptions : public QObject {
    Q_OBJECT
private slots:
    void hiddenWhenFlagPresent() {
        QVERIFY(shouldStartHidden({"orbit", "--background"}));
    }
    void visibleWhenFlagAbsent() {
        QVERIFY(!shouldStartHidden({"orbit"}));
        QVERIFY(!shouldStartHidden({"orbit", "--other"}));
    }
};
QTEST_APPLESS_MAIN(TstBootOptions)
#include "tst_bootoptions.moc"
