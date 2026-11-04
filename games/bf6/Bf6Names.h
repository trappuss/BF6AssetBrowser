#pragma once
// ── BF6 name layer: codename+set → real in-game Squad / Operator / Skin ──────────────────────────
//
// BF6 operator and skin display names are NOT in the installed files — they are served from EA's
// backend (verified exhaustively: a content scan of all 188k EBX found zero name strings, and the
// EBX import graph links the named UI portraits only to single-player campaign widgets, never to a
// character mesh; see memory bf6-skin-names). So the only ground truth is the user's in-game visual
// identifications. This layer carries them as DATA, keyed on the AUTHORED path tokens — the character
// codename (`cha0045hawk`) and set (`set_002`) — never a name-substring guess (template §3).
//
// Two sources, merged: the shipped seed `:/bf6/bf6_names.json` (the canonical, version-controlled
// copy of the confirmed IDs) and an optional user-editable `data/bf6_names.json` beside the exe
// (overlaid on top, so new IDs are added without a rebuild — the portable data/ convention, §1).
// Fails closed: a codename with no entry keeps its raw titlecased stem, and the funnel distinguishes
// Identified vs Unidentified rather than pretending every codename is named.
#include <QHash>
#include <QString>

namespace bf6game {

class Bf6Names {
public:
    struct Entry {
        QString squad;      // e.g. "Dagger 1-3" (may be empty)
        QString op;         // operator, e.g. "Gecko"
        QString skin;       // skin / outfit, e.g. "Deepwatch Operative" (per-set; may be empty)
        QString desc;       // flavour text (optional)
        bool    named = false;   // true once an operator name resolved
    };

    // Load the resource seed, then overlay data/bf6_names.json if present. Safe to call once per open.
    void load();

    bool loaded() const { return m_loaded; }
    int  codenameCount() const { return m_codes.size(); }   // distinct identified codenames
    int  skinCount() const;                                 // distinct identified codename+set skins

    // Resolve one character mesh's name. `charId` e.g. "cha0045hawk"; `set` e.g. "set_002" (may be
    // empty). A set-level operator/squad overrides the codename-level default (one codename can host
    // different operators across sets). Returns {named=false} when the codename is unknown.
    Entry resolve(const QString& charId, const QString& set) const;

    // The Characters-list display string when identified: "<Operator> — <Skin> · <part>" (skin and
    // part omitted when empty). Returns "" when not identified, so the caller keeps its fallback.
    QString displayFor(const QString& charId, const QString& set, const QString& part) const;

private:
    struct Code { QString op, squad, skin; QHash<QString, Entry> sets; };   // codename default + per-set
    void mergeJson(const QByteArray& bytes);

    QHash<QString, Code> m_codes;   // charId -> codename-level default + its sets
    bool m_loaded = false;
};

}  // namespace bf6game
