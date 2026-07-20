#include <QtTest>
#include "DownloadTypes.h"

class TstSmoke : public QObject {
    Q_OBJECT
private slots:
    void enumExists() {
        QCOMPARE(static_cast<int>(DownloadState::Queued), 0);
    }
};

QTEST_MAIN(TstSmoke)
#include "tst_smoke.moc"
