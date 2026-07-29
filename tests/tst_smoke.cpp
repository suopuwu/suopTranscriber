#include <QtTest>

// Placeholder smoke test — keeps the test harness itself buildable.
class Tstsmoke : public QObject
{
    Q_OBJECT
private slots:
    void placeholder() { QVERIFY(true); }
};

QTEST_GUILESS_MAIN(Tstsmoke)
#include "tst_smoke.moc"
