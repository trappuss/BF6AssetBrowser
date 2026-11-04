// MenuDump.cpp — see the header.
#include "core/ui/MenuDump.h"

#include <QAction>
#include <QMenu>

namespace core::menudump {

namespace {
bool g_active = false;
QStringList g_rows;
}  // namespace

bool active() { return g_active; }

void setActive(bool on)
{
    g_active = on;
    if (!on) g_rows.clear();
}

bool capture(QMenu* menu, const QString& surface)
{
    if (!g_active) return false;
    QStringList labels;
    if (menu)
        for (QAction* a : menu->actions()) {
            if (a->isSeparator()) {
                labels << QStringLiteral("---");
                continue;
            }
            QString l = a->text();
            l.remove(QLatin1Char('&'));
            // A SHORTCUT HINT is a tab and a key ("Hide part\tH"), and this
            // record is written as a TSV — so a raw label would end the field
            // mid-menu and every entry after it would be lost. It cost an hour
            // once: the Customize PARTS panel looked as though it stopped at
            // "Hide 3 parts" when it in fact had six more rows.
            const int tab = l.indexOf(QLatin1Char('\t'));
            if (tab >= 0)
                l = l.left(tab) + QStringLiteral(" [")
                    + l.mid(tab + 1).trimmed() + QLatin1Char(']');
            if (a->menu())
                l += QStringLiteral(" [%1 item(s)]")
                         .arg(a->menu()->actions().size());
            if (!a->isEnabled()) l += QStringLiteral(" (disabled)");
            labels << l;
        }
    g_rows << surface + QLatin1Char('\t') + labels.join(QStringLiteral(" | "));
    return true;
}

void noteEmpty(const QString& surface)
{
    if (!g_active) return;
    g_rows << surface + QStringLiteral("\t(no menu)");
}

QStringList taken()
{
    const QStringList out = g_rows;
    g_rows.clear();
    return out;
}

}  // namespace core::menudump
