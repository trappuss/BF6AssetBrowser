#pragma once
// A build: the ONE description of a customised character (FOX's grammar, docs/customize/
// CUSTOMIZE_CORE_DESIGN.md §2). Presets, undo, the clipboard, build files and the harness all read
// and write this text, so a field cannot be saved by one of them and lost by another.
//
// Keys are STABLE ids the game chooses (an asset path, a table row) — never a combo index or a
// display string, which a re-index or a patch reorders (D4 convention 3).
//
//   category=character          (games with several categories — FOX: weapon / character / vehicle)
//   subject=Warrior
//   variant=art/…/body.smd       (games whose subject has a base to choose — FOX: receiver / body / tier)
//   slot.body=art/models/items/armours/bodyarmours/…/x.smd
//   toggle.hair=1
//
// Lines are sorted, so equal builds have equal text and a diff of two texts is the change.
#include <QMap>
#include <QString>

namespace core::customize {

struct Build {
    QString category;                 // "" = the game has one category
    QString subject;
    QString variant;                  // "" = the subject's first (or the game has no variants)
    QMap<QString, QString> picks;     // slot id → option id; absent/empty = none (the game's default)
    QMap<QString, QString> toggles;   // toggle id → "1" / "0"

    QString slot(const QString& id) const { return picks.value(id); }
    bool toggle(const QString& id, bool fallback) const
    {
        const auto it = toggles.constFind(id);
        return it == toggles.constEnd() ? fallback : it.value() == QLatin1String("1");
    }

    // Sorted "key=value" lines. Empty slot values are left out (absent = none).
    QString toText() const;
    // Accepts newline- or ';'-separated lines (';' so a build fits on a command line); unknown keys
    // are ignored and reported through `unknown`. False when no line parsed at all.
    static bool fromText(const QString& text, Build* out, QStringList* unknown = nullptr);

    bool operator==(const Build& o) const
    {
        return category == o.category && subject == o.subject && variant == o.variant && picks == o.picks && toggles == o.toggles;
    }
    bool operator!=(const Build& o) const { return !(*this == o); }
};

// A self-test for the grammar (round trip, separators, empty values), run by the core's --check.
QString buildSelfTest();

}  // namespace core::customize
