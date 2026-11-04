#pragma once
// The one line every tab reports itself on (FOX app/StatusLine.h). One singleton, tabs call it,
// the window is the only listener. `source` is the widget reporting: the window shows the message
// only while that widget is inside the CURRENT tab, so switching tabs cannot leave the Models
// tab's last load sitting under another tab. Null means "from nowhere in particular" and always
// shows. Replaces POE2's per-tab `status` signals connected one by one in MainWindow.
#include <QObject>
#include <QString>
#include <QWidget>

namespace core {

class StatusLine : public QObject {
    Q_OBJECT
public:
    static StatusLine& instance() { static StatusLine s; return s; }
    void report(QWidget* source, const QString& text) { emit reported(source, text); }
Q_SIGNALS:
    void reported(QWidget* source, const QString& text);
private:
    StatusLine() = default;
};

}  // namespace core
