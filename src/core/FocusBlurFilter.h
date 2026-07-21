#pragma once
#include <QObject>

// App-wide "click outside a text field blurs it" — installed on qApp so it sees
// every press BEFORE any QML grab (a Flickable/ScrollView/MouseArea swallowing
// the press is why the QML root TapHandlers weren't universal). Blurs ANY
// focused TextField/TextArea, everywhere. Purely observes: never consumes.
class FocusBlurFilter : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
};
