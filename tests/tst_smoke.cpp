#include <QtTest>

// Placeholder — populated by the Milestone 6 test-writing step.
class Tstsmoke : public QObject
{
    Q_OBJECT
private slots:
    void placeholder() { QVERIFY(true); }
};

QTEST_GUILESS_MAIN(Tstsmoke)
#include "tst_smoke.moc"
