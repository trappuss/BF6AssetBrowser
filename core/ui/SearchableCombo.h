#pragma once
// A QComboBox that keeps the ordinary click-to-open dropdown AND lets you type to filter a long list.
//
// Why this exists: a plain editable QComboBox (what you need for type-to-search) only opens its popup
// from the little arrow — a click on the text field just drops a cursor. On a slot list of hundreds of
// items that reads as "the dropdown doesn't drop down; I have to scroll-wheel through it." So here a
// click ANYWHERE on the widget opens the full list, and typing filters it through a contains-completer.
//
// No Q_OBJECT: this only overrides virtuals and installs an event filter, so it needs no moc (keeping
// it a drop-in header, like util/ExportLayout.h). Read the current selection the usual way, via
// currentData()/currentIndex(); the completer never inserts new items (NoInsert), so a typed string
// that matches nothing leaves the selection untouched.
#include <QAbstractItemView>
#include <QComboBox>
#include <QCompleter>
#include <QEvent>
#include <QLineEdit>
#include <QMouseEvent>

namespace core {

class SearchableCombo : public QComboBox {
public:
    explicit SearchableCombo(QWidget* parent = nullptr) : QComboBox(parent)
    {
        setEditable(true);
        setInsertPolicy(QComboBox::NoInsert);          // a picker, never a text-entry field
        setMaxVisibleItems(24);
        if (QLineEdit* le = lineEdit()) {
            le->setPlaceholderText(QStringLiteral("click, or type to search…"));
            le->installEventFilter(this);              // so a click on the field opens the popup
        }
        if (QCompleter* c = completer()) {
            c->setCompletionMode(QCompleter::PopupCompletion);
            c->setFilterMode(Qt::MatchContains);       // match anywhere in the name, not just the start
            c->setCaseSensitivity(Qt::CaseInsensitive);
        }
    }

protected:
    // A left-click on the line-edit opens the full dropdown instead of only placing a text cursor —
    // this is the whole point (restores "click to drop down" on an editable combo).
    bool eventFilter(QObject* obj, QEvent* ev) override
    {
        if (lineEdit() && obj == lineEdit() && ev->type() == QEvent::MouseButtonPress) {
            auto* me = static_cast<QMouseEvent*>(ev);
            if (me->button() == Qt::LeftButton) {
                if (!view()->isVisible()) showPopup();   // toggle-open, don't fight an already-open list
                return true;                             // consume: no cursor-drop on the field
            }
        }
        return QComboBox::eventFilter(obj, ev);
    }

    // Clicking the arrow (or anywhere, via the filter above) shows every item — clear any leftover
    // filter text first so the list is never a stale, narrowed view of itself.
    void showPopup() override
    {
        if (lineEdit() && !lineEdit()->text().isEmpty() && currentIndex() >= 0)
            lineEdit()->setText(currentText());   // restore the real selection's text before showing all
        QComboBox::showPopup();
    }
};

}  // namespace core
