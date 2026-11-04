#pragma once
#include "core/types/Asset.h"
#include <QString>
#include <QVector>
#include <functional>

// ── Where a multi-file export puts its files ────────────────────────────────────────────────────
//
// ONE implementation, shared by every path that writes more than one file. All three tools already
// learned that the hard way; what none of them could share was the VOCABULARY:
//
//   D4    Flat · Class · Type · by model      tags from AppearanceMeta, keyed by appearance SNO
//   FOX   Flat · game · category · family · by file   tags from index/ModelTags
//   POE2  Flat · Type · Folder · by model     type from the extension, folder from the game path
//
// So the mechanism is here and the vocabulary is a `Spec` the plugin fills. Three of the modes
// need no vocabulary at all and are built in: Flat, MirrorPath (the asset's own directory, POE2's
// "Folder") and PerItem (a folder per asset, named by the export name template).
//
// TWO RULES, both from real bugs, both kept:
//
//  * An UNKNOWN mode id reads as Flat. A newer build's mode or a hand-edited INI would otherwise be
//    taken for a tag name, match nothing, and file the whole run under "_misc" while the combo
//    (findData → -1) displayed "Flat". Fail to where the user pointed, never somewhere else.
//
//  * An item a mode cannot place goes to "_misc", never to the empty string. An empty folder name
//    scatters those files into the parent, mixed in with the grouped ones.
//
// THE MODE IS CHOSEN BY THE ENTRY POINT, NOT THE ITEM COUNT. The single-file paths (Ctrl+E, the
// preview pane, drag-out) pass kFlat() regardless of the saved setting: a global "by Class"
// silently turning Ctrl+E into barbarian\foo.glb is a surprise the caller cannot undo, and drag-out
// rebuilds its own paths so a folder would make the drag carry nothing. The batch paths pass the
// saved mode even for a one-row selection, because that IS the right answer — one more barbarian
// belongs in Barbarian\ with the rest.
//
// Core does not read the setting. The app reads its own key and passes the string through
// resolveMode(); core owning a key name is how two tools end up disagreeing about where it lives.
namespace core::layout {

inline QString kFlat()       { return QString(); }                    // everything at the root
inline QString kMirrorPath() { return QStringLiteral("_path"); }      // the asset's own directory
inline QString kPerItem()    { return QStringLiteral("_item"); }      // one folder per asset

// The ids are stable STRINGS, never a combo index. An index is the identity this family warns
// against persisting: inserting a mode reorders every saved value, and each clamp guarding it has
// to be found and corrected by hand every time the list grows. The sentinels start with '_' so
// they can never collide with a plugin's tag names, which are plain words.
struct Mode {
    QString id;
    QString label;          // what a combo shows
    QString hint;           // the one line under it
    bool    needsTags = false;   // resolves through Spec::tagFor, so it is useless where tags are absent
};

struct Item {
    AssetId id;
    QString name;           // the display name — the stem a PerItem folder is named from
    QString sourcePath;     // the asset's own path inside the game, '/'-separated; may be empty
};

struct Group {
    QString folder;         // empty = the destination itself (Flat)
    QVector<Item> items;
};

// The plugin's half of the layout.
struct Spec {
    // The roster a combo lists, in order. Start from builtinModes() and append the game's own.
    QVector<Mode> modes;

    // Folder name for one item under a tag mode. Return an empty string for "cannot place this"
    // — the caller turns that into "_misc" so it is never the plugin's job to remember.
    std::function<QString(const QString& mode, const Item&)> tagFor;

    // False while the tag source is still building. A run started then would file EVERYTHING under
    // "_misc", which helps nobody, so the layout falls back to Flat and says so through wasReady.
    std::function<bool()> tagsReady;

    // Used by kPerItem(); see core::name::apply.
    QString itemNameTemplate = QStringLiteral("{{FileName}}");

    const Mode* find(const QString& id) const;
    bool isKnown(const QString& id) const;
};

// Flat, MirrorPath and PerItem, with labels. Every game offers these three; a plugin appends its
// own tag modes and never has to spell these out.
QVector<Mode> builtinModes();

// The mode actually in force, given whatever was stored. Unknown → kFlat().
QString resolveMode(const Spec& spec, const QString& stored);

// The relative folder for one item, "" for the destination itself. Never leading or trailing '/'.
QString subfolderFor(const Spec& spec, const QString& mode, const Item& item);

// Split `items` into destination groups in a stable order, so the log and the progress bar read
// the same way on every run over the same selection. Flat returns exactly one group with an empty
// folder, so a caller can treat the result uniformly instead of special-casing it.
//
// `fellBackToFlat`, when given, is set true if a tag mode was asked for while its tags were not
// ready — the caller should say so rather than let the user wonder why the folders are missing.
QVector<Group> group(const Spec& spec, const QString& mode, const QVector<Item>& items,
                     bool* fellBackToFlat = nullptr);

// The destination for one group, resolved against the run's root.
QString folderFor(const QString& root, const Group& g);

QString selfTest();   // "" on success

}  // namespace core::layout
