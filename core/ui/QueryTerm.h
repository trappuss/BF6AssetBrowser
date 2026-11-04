#pragma once
// ── The ONE definition of what a single search term matches (template §4) ────────────────────
// The per-TERM matcher all three projects carried verbatim from D4, with the id-term rule made a
// POLICY the plugin sets: what counts as an id, and how an id term matches a row, is engine-specific
// (D4 a run of SNO digits; FOX a 64-bit hash, exact; POE2 decimal or 0x-hex, matched as a suffix of
// the 16-digit hex form). The matcher itself never varies, which is what lets the Models list, the
// Textures list and Bulk agree on what a query means (app-shell.md §1.1).
//
//   foo         substring, case-insensitive
//   a|b         OR within one term; -a|b excludes when ANY alternative matches
//   <id>        per the policy; an id term matches the row's id and NOTHING else
#include "core/types/Asset.h"

#include <QLatin1Char>
#include <QString>
#include <QStringList>
#include <algorithm>
#include <functional>

namespace core::search {

inline bool matches(const QString& hay, const QString& term)
{
    if (!term.contains(QLatin1Char('|')))
        return hay.contains(term, Qt::CaseInsensitive);
    const QStringList alts = term.split(QLatin1Char('|'), Qt::SkipEmptyParts);
    if (alts.isEmpty()) return true;   // a term that is ONLY pipes constrains nothing
    for (const QString& a : alts)
        if (hay.contains(a, Qt::CaseInsensitive)) return true;
    return false;
}

// ── The id policy ─────────────────────────────────────────────────────────────────────────────
using IsIdFn      = std::function<bool(const QString& term)>;
using IdMatchesFn = std::function<bool(const QString& term, const AssetId& id)>;

struct IdPolicy { IsIdFn isId; IdMatchesFn matches; };
inline IdPolicy& policy() { static IdPolicy p; return p; }
inline void setIdPolicy(IsIdFn isId, IdMatchesFn matches) { policy() = {std::move(isId), std::move(matches)}; }

inline bool isIdTerm(const QString& term) { return policy().isId && policy().isId(term); }
inline bool idMatches(const QString& term, const AssetId& id) { return policy().matches && policy().matches(term, id); }

// The same test, with an id in hand: an id term matches the entry it names and nothing else — not
// the text of the path, which would make a hash search return every file containing those digits.
inline bool matchesWithId(const QString& hay, const AssetId& id, const QString& term)
{
    if (isIdTerm(term)) return idMatches(term, id);
    return matches(hay, term);
}

// ── Self-test — "" on success. The cases are the union of the three projects' tables. ─────────
inline QString selfTest()
{
    struct Case { const char* hay; const char* term; bool want; };
    static const Case kCases[] = {
        {"helmet_str03",     "helmet",                  true },
        {"helmet_str03",     "gloves",                  false},
        {"helmet_str03",     "HELMET",                  true },   // case-insensitive
        {"helmet_str03",     "gloves|helmet",           true },
        {"helmet_str03",     "gloves|boots",            false},
        {"tatteredrobe_mc",  "robe|test999",            true },
        {"body_base00",      "body_base00|body_base01", true },
        {"body_base01_trs",  "body_base00|arm_base00",  false},
        {"avm0_type0_def",   "avf0|avm0",               true },
        {"tcl1_main0_def_f", "tcl0_main0_def|rcl0_main0_def", false},
        {"anything",         "|",                       true },
        {"anything",         "zzz|",                    false},
    };
    for (const auto& c : kCases) {
        const QString hay = QString::fromLatin1(c.hay), term = QString::fromLatin1(c.term);
        if (matches(hay, term) != c.want)
            return QStringLiteral("QueryTerm: \"%1\" vs \"%2\" expected %3")
                .arg(hay, term, c.want ? QStringLiteral("match") : QStringLiteral("no match"));
    }
    // With NO policy installed nothing is an id, so a digit run is a substring (D4's Textures tab
    // regression: "a number in a texture name must still be searchable as text").
    const IdPolicy saved = policy();
    policy() = IdPolicy{};
    if (!matchesWithId(QStringLiteral("tex_1234567890"), AssetId{1, 42}, QStringLiteral("1234567890")))
        return QStringLiteral("QueryTerm: without a policy a digit run must be text");
    // With a policy: an id term matches its own id only.
    setIdPolicy([](const QString& t) { return t.size() >= 6 && std::all_of(t.begin(), t.end(), [](QChar c) { return c.isDigit(); }); },
                [](const QString& t, const AssetId& id) { return t.toULongLong() == id.value; });
    if (matchesWithId(QStringLiteral("/a/1234567890_thing"), AssetId{1, 42}, QStringLiteral("1234567890")))
        { policy() = saved; return QStringLiteral("QueryTerm: an id term matched a path containing those digits"); }
    if (!matchesWithId(QStringLiteral("/a/anything"), AssetId{1, 1234567890ULL}, QStringLiteral("1234567890")))
        { policy() = saved; return QStringLiteral("QueryTerm: an id term did not match its own id"); }
    if (!matchesWithId(QStringLiteral("hat21"), AssetId{1, 7}, QStringLiteral("hat21")))
        { policy() = saved; return QStringLiteral("QueryTerm: a short word must stay a substring"); }
    policy() = saved;
    return QString();
}

}  // namespace core::search
