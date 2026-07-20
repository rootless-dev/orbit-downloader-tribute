#include <QtTest>
#include "Scheduler.h"

static QDateTime at(int y,int mo,int d,int h,int mi) {
    return QDateTime(QDate(y,mo,d), QTime(h,mi));
}

class TstScheduler : public QObject {
    Q_OBJECT
private slots:
    void disabledNeverFires() {
        Scheduler s; SchedulerConfig c; c.enabled = false; s.setConfig(c);
        QVERIFY(s.tick(at(2026,7,18,8,0)) == SchedAction::None);
    }
    void dailyFiresStartThenStop() {
        Scheduler s; SchedulerConfig c;
        c.enabled = true; c.start = QTime(8,0); c.stop = QTime(18,0);
        c.recurrence = Recurrence::Daily; s.setConfig(c);
        QVERIFY(s.tick(at(2026,7,18,7,59)) == SchedAction::None);
        QVERIFY(s.tick(at(2026,7,18,8,0))  == SchedAction::StartAll);
        QVERIFY(s.tick(at(2026,7,18,9,0))  == SchedAction::None);   // sem repique na janela
        QVERIFY(s.tick(at(2026,7,18,18,0)) == SchedAction::StopAll);
        QVERIFY(s.tick(at(2026,7,18,19,0)) == SchedAction::None);
        QVERIFY(s.tick(at(2026,7,19,8,0))  == SchedAction::StartAll); // rearma no dia seguinte
    }
    void launchMidWindowStartsImmediately() {
        Scheduler s; SchedulerConfig c;
        c.enabled = true; c.start = QTime(8,0); c.stop = QTime(18,0); s.setConfig(c);
        QVERIFY(s.tick(at(2026,7,18,10,0)) == SchedAction::StartAll);
    }
    void launchAfterWindowDoesNotFire() {
        Scheduler s; SchedulerConfig c;
        c.enabled = true; c.start = QTime(8,0); c.stop = QTime(18,0); s.setConfig(c);
        QVERIFY(s.tick(at(2026,7,18,20,0)) == SchedAction::None);   // nem Start nem Stop
    }
    void onceDisarmsAfterStop() {
        Scheduler s; SchedulerConfig c;
        c.enabled = true; c.start = QTime(8,0); c.stop = QTime(18,0);
        c.recurrence = Recurrence::Once; s.setConfig(c);
        QVERIFY(s.tick(at(2026,7,18,8,0))  == SchedAction::StartAll);
        QVERIFY(s.tick(at(2026,7,18,18,0)) == SchedAction::StopAll);
        QVERIFY(s.tick(at(2026,7,19,8,0))  == SchedAction::None);   // não rearma
    }
    void setConfigRearmsAfterFire() {
        Scheduler s; SchedulerConfig c;
        c.enabled = true; c.start = QTime(8,0); c.stop = QTime(18,0);
        c.recurrence = Recurrence::Daily; s.setConfig(c);
        QVERIFY(s.tick(at(2026,7,18,8,0)) == SchedAction::StartAll);
        QVERIFY(s.tick(at(2026,7,18,9,0)) == SchedAction::None);
        s.setConfig(c);                                   // reconfig no mesmo dia -> re-arma
        QVERIFY(s.tick(at(2026,7,18,9,1)) == SchedAction::StartAll);
    }
};

QTEST_MAIN(TstScheduler)
#include "tst_scheduler.moc"
