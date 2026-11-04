#pragma once
#include <QMetaType>
#include <QString>
#include <QStringList>
#include <QVector>
#include <cstdint>

// ── Asset identity and the list row ─────────────────────────────────────────────────────────────
// Three id schemes (plugin-build.md §4): D4 a 32-bit SNO scoped by a group id, FOX a 64-bit
// PathFileNameCode (51-bit CityHash + 13-bit extension code) with a SECOND legacy scheme for
// Ground Zeroes archives, POE2 a MurmurHash64A of the lowercased path.
//
// `scheme` is what keeps them apart: the same 64-bit number means different things in two games,
// and inside FOX it means different things in two archive kinds. A bare quint64 would have forced
// D4 to give up group scoping (settled: keep the struct — plan §8.5).
namespace core {

struct AssetId {
    uint16_t scheme = 0;        // plugin-defined; 0 = invalid/none
    uint64_t value  = 0;

    bool isValid() const { return scheme != 0; }
    bool operator==(const AssetId& o) const { return scheme == o.scheme && value == o.value; }
    bool operator!=(const AssetId& o) const { return !(*this == o); }
};
inline uint qHash(const AssetId& a, uint seed = 0)
{
    return ::qHash(a.value, seed) ^ (uint(a.scheme) * 2654435761u);
}

// What a list row shows without touching the store again. Anything more expensive than this is
// answered lazily through IGameStore, which is already how D4's std::function hooks and POE2's
// icon provider work (tabs-store.md §1.4).
struct AssetRow {
    AssetId  id;
    QString  name;              // file name or path — what the user recognises
    QString  inGameName;        // [D4 AppearanceMeta title] [POE2 NameIndex] [FOX NameCatalog]
    QString  collection;        // [D4] named set; empty elsewhere
    QString  kind;              // SNO group name / file extension
    uint64_t size = 0;

    QStringList tags;

    enum Flag : uint32_t {
        None         = 0,
        Encrypted    = 1u << 0,   // [D4] present but unreadable — a distinct answer from absent
        Unnamed      = 1u << 1,   // [D4 ~unnamed_] [FOX unresolved hash] [POE2 unnamedFiles]
        Unrenderable = 1u << 2,   // [D4] blocklisted or no geometry
        Animated     = 1u << 3,
        Rigged       = 1u << 4,
        Orphaned     = 1u << 5,
        New          = 1u << 6,   // [D4] arrived in the current game build
        Shadowed     = 1u << 7,   // [FOX] a mod copy overriding a shipped file
    };
    uint32_t flags = None;

    bool has(Flag f) const { return (flags & uint32_t(f)) != 0; }
};

// A grouped facet vocabulary the plugin supplies and the core funnel renders.
struct TagGroup {
    QString id;
    QString label;
    struct Entry { QString id; QString label; int count = 0; };
    QVector<Entry> entries;
    bool shaderLike = false;    // [POE2] matched against an index, never against the path
};

}  // namespace core

Q_DECLARE_METATYPE(core::AssetId)
