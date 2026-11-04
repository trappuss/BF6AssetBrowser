#include "core/app/Hotkeys.h"

namespace core::hotkeys {

namespace {
QVector<Def>& table() { static QVector<Def> t = coreDefs(); return t; }
QVector<SheetRow>& mouse()
{
    static QVector<SheetRow> m = {
        {QStringLiteral("Orbit"), QStringLiteral("Left drag"), QString()},
        {QStringLiteral("Pan"), QStringLiteral("Middle drag / Alt+Right drag"), QString()},
        {QStringLiteral("Zoom"), QStringLiteral("Wheel"), QString()},
        {QStringLiteral("Select part"), QStringLiteral("Click"),
         QStringLiteral("Shift or Ctrl to add to the selection")},
        {QStringLiteral("Frame part"), QStringLiteral("Double-click"), QString()},
        {QStringLiteral("Part menu"), QStringLiteral("Right-click"), QString()},
    };
    return m;
}
}  // namespace

QVector<Def> coreDefs()
{
    return {
        {kExportSelection(), QStringLiteral("Export selection…"), QStringLiteral("Ctrl+E"),
         QStringLiteral("Export whatever the current tab has selected, asking where to put it.")},
        {kExportToLast(), QStringLiteral("Export selection to last folder"), QStringLiteral("Ctrl+Shift+E"),
         QStringLiteral("The same export, straight into the folder the last export went to, no dialog. "
                        "Nothing happens until one export has been made the long way.")},
        {kExportAnimations(), QStringLiteral("Export animations only…"), QStringLiteral("Ctrl+Shift+A"),
         QStringLiteral("The skeleton and the clips, with no mesh — a retargeting library.")},
        {kSaveImage(), QStringLiteral("Save preview image…"), QStringLiteral("Ctrl+Shift+I"),
         QStringLiteral("The viewport as a PNG. No toolbar, no panels.")},
        {kTurntable(), QStringLiteral("Turntable GIF…"), QString(),
         QStringLiteral("One full revolution as an animated GIF. Unbound by default: it takes a while "
                        "and it is not something to start by accident.")},
        {kAnimLoop(), QStringLiteral("Animation-loop GIF…"), QString(),
         QStringLiteral("The current clip as a looping GIF. Unbound by default, for the same reason.")},
        {kFocusSearch(), QStringLiteral("Focus the search box"), QStringLiteral("Ctrl+F"),
         QStringLiteral("Jump to the current tab's search box. Esc clears it and gives the focus back.")},
        {kSettings(), QStringLiteral("Settings…"), QStringLiteral("Ctrl+,"),
         QStringLiteral("The settings dialog.")},
        {kReload(), QStringLiteral("Reload the game data"), QStringLiteral("F5"),
         QStringLiteral("Re-read the game folder. Use after a patch or after changing the folder.")},
        {kToggleConsole(), QStringLiteral("Log console"), QStringLiteral("Ctrl+`"),
         QStringLiteral("The session log, live, in a window beside the tab you are on.")},
    };
}

void setDefs(const QVector<Def>& t) { table() = t; }
const QVector<Def>& defs() { return table(); }

const Def* find(const QString& key)
{
    for (const Def& d : table())
        if (d.key == key) return &d;
    return nullptr;
}

QKeySequence seq(const QString& key, const QString& def)
{
    const QString s = QSettings().value(key, def).toString();
    return s.isEmpty() ? QKeySequence() : QKeySequence(s);
}

QKeySequence seq(const QString& key)
{
    const Def* d = find(key);
    return d ? seq(d->key, d->def) : QKeySequence();
}

void setMouseRows(const QVector<SheetRow>& rows) { mouse() = rows; }

QVector<SheetRow> sheetRows()
{
    QVector<SheetRow> rows;
    for (const Def& d : table()) {
        const QKeySequence k = seq(d.key, d.def);
        rows.append({d.label, k.isEmpty() ? QStringLiteral("(unbound)") : k.toString(QKeySequence::NativeText), d.hint});
    }
    rows += mouse();
    return rows;
}

QString cheatSheetText(const QString& productName)
{
    QString out = productName + QStringLiteral(" — keyboard and mouse\n\n");
    int w = 0;
    const QVector<SheetRow> rows = sheetRows();
    for (const SheetRow& r : rows) w = qMax(w, int(r.what.size()));
    for (const SheetRow& r : rows) {
        out += r.what.leftJustified(w + 2) + r.keys;
        if (!r.note.isEmpty()) out += QStringLiteral("   — ") + r.note;
        out += QLatin1Char('\n');
    }
    return out;
}

QString cheatSheetHtml(const QString& productName)
{
    QString out = QStringLiteral(
        "<html><head><meta charset=\"utf-8\"><title>%1 — keys</title>"
        "<style>body{font-family:sans-serif;margin:24px}table{border-collapse:collapse}"
        "td,th{padding:4px 12px;border-bottom:1px solid #ddd;text-align:left}"
        "kbd{font-family:monospace;background:#eee;padding:1px 5px;border-radius:3px}</style></head>"
        "<body><h2>%1 — keyboard and mouse</h2><table><tr><th>Action</th><th>Keys</th><th></th></tr>")
        .arg(productName.toHtmlEscaped());
    for (const SheetRow& r : sheetRows())
        out += QStringLiteral("<tr><td>%1</td><td><kbd>%2</kbd></td><td>%3</td></tr>")
                   .arg(r.what.toHtmlEscaped(), r.keys.toHtmlEscaped(), r.note.toHtmlEscaped());
    out += QStringLiteral("</table></body></html>\n");
    return out;
}

namespace Role {
QAction* find(const QList<QAction*>& acts, const QString& role)
{
    for (QAction* a : acts) {
        if (!a) continue;
        if (a->menu()) {
            if (QAction* hit = find(a->menu()->actions(), role)) return hit;
            continue;
        }
        if (a->property(kProperty).toString() == role) return a;
    }
    return nullptr;
}
}  // namespace Role

}  // namespace core::hotkeys
