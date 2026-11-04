#pragma once
// What a game answers so the core Customize tab can dress its characters
// (docs/customize/CUSTOMIZE_CORE_DESIGN.md §5). Five required questions; everything else defaults
// to "not in this game". Meshes, rigs, clips and materials still come from IGameStore — this only
// says WHICH assets make up a character and HOW each attaches.
//
// Threading: every call is made on the GUI thread. The heavy part — loading meshes, materials and
// rigs, and merging them — runs on the tab's assembly worker through IGameStore's thread-safe set.
#include "core/customize/Build.h"
#include "core/types/Asset.h"
#include "core/math/RigMath.h"
#include "core/types/Geometry.h"
#include "core/types/Material.h"

#include <QColor>
#include <QMap>
#include <QSet>
#include <QString>
#include <QVector>

namespace core::customize {

struct Category {                 // what KIND of thing is being built (FOX: weapon, character, vehicle)
    QString id;                   // stable (written into builds as category=)
    QString label;
    QString subjectNoun;          // the subject row's label on this page ("" = the source's subjectNoun)
    QString variantNoun;          // the variant row's label on this page ("" = the source's variantNoun)
};

struct Subject {                  // who is being dressed: a class, a character, a body
    QString id;                   // stable (it is written into builds)
    QString label;
    QString group;                // caption the subject list groups under ("" = none)
    QString note;                 // e.g. "unreleased"
    QString category;             // the category it is listed under ("" = the game has one)
};

enum class SlotKind {
    Part,                         // a mesh (or none): exported, searched, has an export button
    Look,                         // an appearance choice (skin tone, eye colour, a variation): not a
                                  // mesh — no export button, not counted by Find (FOX's look rows)
};

struct SlotDef {
    QString id;                   // stable
    QString label;
    bool randomize = true;        // "Random" fills it (false: a shapeshift form, which replaces the body)
    bool inSets = true;           // a set (outfit/theme) may fill it
    SlotKind kind = SlotKind::Part;
    QString noneLabel;            // row 0's text ("" = "— none —"; FOX's look rows: "— from face preset —")
};

struct Option {                   // one choice for a slot
    QString id;                   // stable — usually the asset path
    QString label;                // what the list shows
    QString detail;               // tooltip (e.g. the full path)
    QString set;                  // the set/outfit it belongs to ("" = none)
    QString section;              // caption of the block it is listed under ("" = none); consecutive
                                  // options with the same section share one unselectable header row
    QColor  swatch;               // a colour chip beside the label (invalid = none): dyes, skin tones
};

// Whether a slot's list is narrowed to what fits the rest of the build (FOX: parts that fit this
// receiver). The tab shows the state honestly: rule off, rule on with data, or no rule for this.
struct Compat {
    enum State { NoRule, Rule } state = NoRule;
    QSet<QString> allowed;        // State::Rule: the option ids that fit
    bool unusable = false;        // nothing ever goes in this slot on this subject/variant (struck through)
    QString why;                  // said in the slot's tooltip
};

struct Toggle {                   // a show/filter switch that is part of the build (hair, FX, lock…)
    QString id, label, tooltip;
    bool defaultOn = true;
};

struct SetDef { QString id, label; };   // a named group of options that fills several slots at once

enum class Placement {
    InPlace,      // skinned to the body's rig: joints bound BY BONE NAME, and bones the body lacks
                  // are APPENDED under their own parents (D4's merge by bone; FOX's partial rigs)
    Socket,       // rigid on a named bone at its bind pose × socketOffset (weapons) — the piece's
                  // own rig, if any, is dropped
    SocketRig,    // on a socket like Socket, but KEEPS its own rig: its bones are appended under the
                  // socket bone, so a clip that drives them (or a later piece seated on them) works
                  // (D4 attachSubRigAt: flails, animated trophies; FOX parts that carry sub-rigs)
    ReplaceBody,  // this mesh IS the figure, with its own rig and clips (a shapeshift form)
};

// A look on ONE mesh (stage 2c): what a variation / dye / swatch / skin tone does to it. The source
// works it out in resolve() from the build's look rows, per piece (FOX resolves a variation name
// against each part's own stem), and the tab only applies it. Group names match a part's name or
// the name of any group above it, case-insensitively. Per part, FOX's rule (CUSTOMIZE_FOX.md §4):
//     hidden = ((shipped hidden ∪ hide) − show) ∪ forceHide
// so `show` turns on an alternative the game ships switched off, and `forceHide` wins over both.
struct Look {
    MaterialLook material;        // texture swaps + the game's own inputs, through the store
    QSet<QString> hide;           // groups the look switches off (FOX variation-hidden)
    QSet<QString> show;           // groups it switches on (FOX variation- / look- / hair-shown)
    QSet<QString> forceHide;      // groups off whatever else says (FOX look- / sleeve- / hair-hidden)
    // Non-empty: keep only the triangles skinned into this bone's subtree (by the mesh's OWN rig) —
    // FOX's head-only base (modelload::keepBoneSubtree): a triangle stays when ANY corner's heaviest
    // bone is in the subtree (the neck seam belongs to both), a part with no skinning goes entirely,
    // and a rig without the bone leaves the mesh whole (said).
    QString keepSubtree;
    bool isEmpty() const { return material.isEmpty() && hide.isEmpty() && show.isEmpty() && forceHide.isEmpty() && keepSubtree.isEmpty(); }
};

// A named hardpoint on a named host (FOX: a connect point on a part; D4: a hardpoint on an item).
struct SocketOption {
    QString host;                 // the label of a piece listed EARLIER ("" = the figure)
    QString hardpoint;            // its name as IGameStore::hardpoints lists it (exact match, as FOX)
};

struct Piece {
    AssetId mesh;
    QString label;                // shown in the ATTACHMENTS panel and in export names
    QString slot;                 // the slot it came from ("" = the body's own default piece)
    Placement placement = Placement::InPlace;
    QString socket;               // Socket / SocketRig: the bone name — in the body's rig, or among
                                  // the bones an earlier piece appended (a chain: receiver → barrel → muzzle)
    // Seating on a HOST'S hardpoint (FOX's attach plan, CUSTOMIZE_FOX.md §4). When non-empty it
    // replaces `socket`: the options are tried in order and the first whose host is already fitted,
    // carries that hardpoint, and has not given it to another piece wins. Pieces with options are
    // seated in LIST order, once — a host listed later is "not fitted" (FOX's rows; a weapon source
    // lists every host before what it carries). Placement Socket stays rigid; anything else keeps
    // its own rig. Nothing seats it → it stays where it was authored, counted SEATED when it has no
    // hardpoints of its own (authored in the figure's space — FOX's door armour) and unseated when
    // it has some (FOX: "offers no connect point for it").
    QVector<SocketOption> seatOn;
    rm::Mat4 socketOffset = rm::identity();   // the socket's own transform in its bone's space
                                  // (POE2 .ao socket translation, FOX connect point, D4 hardpoint)
    // FOX's rest alignment (CustomizeTab::restAlignmentFor): a part that ships only the bones it
    // needs declares its topmost one a ROOT AT THE ORIGIN, so it renders offset by where that bone
    // really is (hair a head's length low). With alignRoot the piece is moved so its root lands on
    // the figure's bone of the same name (FOX exit 'b') — unless that bone is itself a root of the
    // figure, which knows no more than the piece does (FOX's hat31 lesson). When the figure has no
    // such bone and `socket` is set, the piece's ROOT (not its origin) is seated there and it keeps
    // its rig (FOX exits 'c' connect point / 'a' anchor bone). Measured on FOX data; the other games
    // author parts in the figure's own frame and leave this off.
    bool alignRoot = false;
    QString tag;                  // free text the game uses to group pieces ("default", "fx", "hair")
    bool held = false;            // a held item (weapon, shield) — left out of "without held items"
    Look look;                    // this mesh's look (2c); empty = as shipped
    // Non-empty: a model a LOOK brings along (FOX .fv2 attachments), named by the label of the
    // piece whose look it is. It lives only while the source lists it, and it is skipped — and said
    // — when the same mesh is already fitted as the body or an earlier piece (FOX matches by path).
    QString broughtBy;
};

struct Resolved {                 // what a build is made of
    AssetId body;                 // the base figure (its rig and clips drive everything InPlace)
    QString bodyLabel;
    Look bodyLook;                // the body's own look (2c)
    QVector<Piece> pieces;
    QString error;                // non-empty = nothing could be resolved (said on the status line)
    QStringList notes;            // said with the assembly's own notes (FOX: a tier's parts this install lacks)
};

class ICustomizeSource {
public:
    virtual ~ICustomizeSource() = default;

    // ── required ────────────────────────────────────────────────────────────────────────────
    virtual QVector<Subject> subjects() = 0;
    virtual QVector<SlotDef> slotDefs(const QString& subject) = 0;
    // The choices for a slot given the rest of the build (a body type, a lock toggle…). Not
    // including "none" — the tab adds that row.
    virtual QVector<Option>  options(const QString& subject, const QString& slot, const Build& build) = 0;
    virtual Resolved         resolve(const Build& build) = 0;

    // ── optional ────────────────────────────────────────────────────────────────────────────
    virtual QString subjectNoun() const { return QStringLiteral("Character"); }
    virtual QString settingsKey() const { return QStringLiteral("customize"); }   // presets live under it
    virtual void    prepare() {}                  // the index is ready: build catalogues
    virtual void    reset() {}                    // the index is going away
    virtual QString subjectInfo(const QString& subject) { Q_UNUSED(subject); return {}; }
    virtual QVector<Toggle> toggles(const QString& subject) { Q_UNUSED(subject); return {}; }
    virtual QVector<SetDef> sets(const QString& subject) { Q_UNUSED(subject); return {}; }
    // The slot → option picks that set `set` makes for this build (slots it leaves out are cleared
    // only if the slot def says inSets).
    virtual QMap<QString, QString> setPicks(const QString& set, const Build& build) { Q_UNUSED(set); Q_UNUSED(build); return {}; }
    // Body part indices (into the body mesh's own parts) to hide for this build — e.g. the head,
    // to preview a full helmet. Applied after assembly by the one visibility rule.
    virtual QSet<int> hiddenBodyParts(const Build& build, const Geometry& body) { Q_UNUSED(build); Q_UNUSED(body); return {}; }
    // Radians added to the camera's orbit on the first assembly of a subject (a character authored
    // facing away from the default camera).
    virtual float firstViewYaw() const { return 0.0f; }
    // True when a slot's options depend on the OTHER slots (FOX: parts that fit this receiver), so
    // every slot change re-asks options(). False (POE2): only the subject and toggles matter.
    virtual bool optionsDependOnSlots() const { return false; }
    // Presets written by an older version of the game's tab, converted to builds (name → build).
    virtual QMap<QString, Build> legacyPresets() { return {}; }

    // ── categories and variants (FOX's category → subject → variant) ─────────────────────────
    // More than one category shows a Category row; each subject lists under its `category`.
    virtual QVector<Category> categories() { return {}; }
    // The subject's bases (FOX: receiver / body / star tier). Non-empty shows a variant row; the
    // pick is Build::variant (row 0 when the build names none). options() and resolve() read it.
    virtual QVector<Option> variants(const QString& subject, const Build& build) { Q_UNUSED(subject); Q_UNUSED(build); return {}; }
    virtual QString variantNoun() const { return QStringLiteral("Variant"); }

    // ── honest list states ────────────────────────────────────────────────────────────────────
    // What the game dresses the subject in when the slot is left empty. Non-empty makes row 0
    // "— default — <label>" rather than "— none —" (FOX: "what the game starts this character in").
    virtual QString defaultLabel(const QString& subject, const QString& slot, const Build& build)
    { Q_UNUSED(subject); Q_UNUSED(slot); Q_UNUSED(build); return {}; }

    // ── game rules (FOX/MGO: Exclude / Must) ───────────────────────────────────────────────────
    // True shows "Unlocked — keep every slot as I set it". applyRules is called after the user
    // changes `changedSlot`: with enforce, the game edits `build` (clears what excludes, fits what
    // is required) and returns what it did; without, it only describes what it WOULD do.
    virtual bool hasRules(const QString& subject) { Q_UNUSED(subject); return false; }
    virtual QStringList applyRules(Build& build, const QString& changedSlot, bool enforce)
    { Q_UNUSED(build); Q_UNUSED(changedSlot); Q_UNUSED(enforce); return {}; }
    // What a pick changes elsewhere in the game's own editor: not a rule, so "Unlocked" does not
    // switch it off (FOX: a new face is a new character, so the hairstyle goes back to that face
    // preset's, CTW.cpp:2754). Called on a USER pick only, before applyRules; the source edits
    // `build` and returns what it did.
    virtual QStringList followPick(Build& build, const QString& changedSlot)
    { Q_UNUSED(build); Q_UNUSED(changedSlot); return {}; }
    // The same for a new SUBJECT (subjectChanged) or a new VARIANT picked by the user: what the game's
    // editor does to the slots (FOX: a new character or weapon opens on its own dress, a star tier
    // is a whole build, CTW:4436 / CTW:4617). Default: the picks stay (POE2 keeps them across classes).
    virtual QStringList followBase(Build& build, bool subjectChanged)
    { Q_UNUSED(build); Q_UNUSED(subjectChanged); return {}; }

    // ── compatibility narrowing (FOX: "Only parts that fit this weapon") ───────────────────────
    virtual bool hasCompat(const QString& subject) { Q_UNUSED(subject); return false; }
    virtual Compat compat(const QString& subject, const QString& slot, const Build& build)
    { Q_UNUSED(subject); Q_UNUSED(slot); Q_UNUSED(build); return {}; }
};

}  // namespace core::customize
