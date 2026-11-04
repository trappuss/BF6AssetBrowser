// MenuText.h — ONE vocabulary for every menu label in this application (template §12). FOX's
// util/MenuText.h, lifted; the engine-worded entries (kCopyHash, .gani…) are replaced by NOUNS the
// plugin supplies (AppPlugin::Nouns), so core can say "Copy SNO" in D4 and "Copy hash" in FOX
// without either word living here — and "Copy collection name" is OMITTED where the engine has no
// collections, never a placeholder.
//
// Rules, so additions stay consistent (FOX's):
//   * Sentence case. "Export part", not "Export Part".
//   * A trailing "…" means AND ONLY MEANS "this opens a dialog". prompts() is the only thing that adds one.
//   * An action that writes somewhere remembered says where: "Export to last folder (…/exports)".
//   * Copy actions carry their value in parentheses.
//   * The export label names ONE thing or counts several — "Export bdf6_main0_def…" / "Export 12
//     models…", never "Export selection…". exportLabel() is that rule.
#pragma once
#include "core/app/AppPlugin.h"

#include <QDir>
#include <QLocale>
#include <QString>
#include <QStringList>

namespace core::MenuText {

inline const QString kCopyPath     = QStringLiteral("Copy path");
inline const QString kCopyFileName = QStringLiteral("Copy file name");
inline const QString kCopyName     = QStringLiteral("Copy name");
inline const QString kCopyMaterial = QStringLiteral("Copy material name");

// ── The engine nouns, set once by the shell from AppPlugin::nouns() ────────────────────────
inline AppPlugin::Nouns& nouns() { static AppPlugin::Nouns n; return n; }
inline void setNouns(const AppPlugin::Nouns& n) { nouns() = n; }
// "Copy SNO" / "Copy hash" / "Copy id" — the identity noun capitalised as the game spells it.
inline QString kCopyId() { return QStringLiteral("Copy %1").arg(nouns().id); }
// Empty when the engine has no collections: the caller adds the action only when this is non-empty.
inline QString kCopyCollection()
{
    return nouns().collection.isEmpty() ? QString() : QStringLiteral("Copy %1 name").arg(nouns().collection);
}
inline const QString kCopyValue     = QStringLiteral("Copy value");
inline const QString kCopyText      = QStringLiteral("Copy text");
inline const QString kCopyKey       = QStringLiteral("Copy key");
inline const QString kCopyRow       = QStringLiteral("Copy row");
// The export pair, base labels only: NO ellipsis, because suffixes get
// appended ("(12,345 tris)") and prompts() adds the ellipsis last.
inline const QString kExportModel     = QStringLiteral("Export model");
inline const QString kExportModelLast = QStringLiteral("Export model to last folder");
inline const QString kExportPart      = QStringLiteral("Export part");
inline const QString kExportPartLast  = QStringLiteral("Export part to last folder");
inline const QString kCopyImage     = QStringLiteral("Copy image");
// Base labels, NO ellipsis: prompts() adds it at the call site, which is the
// rule for every entry here. (kSaveImage / kSaveImageLast carried a baked
// ellipsis and no callers — removed; the viewport's captures are below.)
inline const QString kShowDeps        = QStringLiteral("Show dependencies");          // PROMPTS
inline const QString kExtractDeps     = QStringLiteral("Extract with dependencies");  // PROMPTS
inline const QString kExportDds       = QStringLiteral("Export DDS");                 // PROMPTS
inline const QString kExportPng       = QStringLiteral("Export PNG");                 // PROMPTS
inline const QString kRerenderThumb   = QStringLiteral("Re-render thumbnail");
inline const QString kRerenderThumbs  = QStringLiteral("Re-render %1 thumbnails");    // arg: count
inline const QString kExportPerVariation = QStringLiteral("Export one file per variation"); // PROMPTS
inline const QString kComposeChannels = QStringLiteral("Compose channels from %1 texture(s)"); // PROMPTS
inline const QString kCompareTwo      = QStringLiteral("Compare these two");          // PROMPTS
// The viewport's capture block (export/ViewCapture.cpp), one spelling each.
inline const QString kSaveViewportImage = QStringLiteral("Save viewport image");     // PROMPTS
inline const QString kTurntableGif      = QStringLiteral("Turntable GIF");           // PROMPTS
inline const QString kAnimationGif      = QStringLiteral("Save animation GIF");      // PROMPTS
inline const QString kExportSettings    = QStringLiteral("Export settings");         // PROMPTS
inline const QString kShowAll       = QStringLiteral("Show all");
inline const QString kHideAll       = QStringLiteral("Hide all");
inline const QString kInvert        = QStringLiteral("Invert");
inline const QString kFramePart     = QStringLiteral("Frame part");
inline const QString kSelectPart    = QStringLiteral("Select part");
inline const QString kHidePart      = QStringLiteral("Hide part");
inline const QString kShowPart      = QStringLiteral("Show part");
inline const QString kIsolatePart   = QStringLiteral("Isolate part");

// "C:/Users/me/Documents/FOX/exports" → "…/FOX/exports". A full path makes a
// menu unreadable; the last two components are what actually distinguishes one
// export folder from another.
inline QString condensePath(const QString& path)
{
    if (path.isEmpty()) return {};
    const QString clean = QDir::fromNativeSeparators(QDir::cleanPath(path));
    const QStringList parts = clean.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (parts.size() <= 2) return QDir::toNativeSeparators(clean);
    return QStringLiteral("…/%1/%2").arg(parts[parts.size() - 2], parts.last());
}

inline QString withValue(const QString& label, const QString& value)
{
    return value.isEmpty() ? label : QStringLiteral("%1 (%2)").arg(label, value);
}

// A value preview, cut at 30 characters with a real ellipsis character. An
// asset path is longer than a menu should be, and a menu as wide as the screen
// is the §5 off-screen-panel bug wearing different clothes.
inline QString preview(const QString& s)
{
    return s.size() > 30 ? s.left(29) + QStringLiteral("…") : s;
}

// The COPY form of withValue: the value slot is always drawn, and an empty one
// renders as "—" with the caller disabling the action. §6 — disable, don't
// hide, when the action applies to this kind of object but has no value right
// now. withValue() above drops the slot entirely, which is right for a folder
// name and wrong for a clipboard value: "Copy name" with nothing after it reads
// as an action that works.
inline QString withCopyValue(const QString& label, const QString& value)
{
    return QStringLiteral("%1  (%2)")
        .arg(label, value.isEmpty() ? QStringLiteral("—") : preview(value));
}

// The multi-selection form: "Copy hash  —  12 rows". The clipboard gets one
// value per line, so the label says how many lines are coming.
inline QString withRows(const QString& label, int rows)
{
    return QStringLiteral("%1  —  %2 rows").arg(label).arg(rows);
}

inline QString withCount(const QString& label, int tris)
{
    return tris > 0 ? QStringLiteral("%1 (%2 tris)")
                          .arg(label, QLocale().toString(tris))
                    : label;
}

// The ONLY way an ellipsis gets onto a label. Applied last, to the finished
// string, so a suffix can never strand one mid-label.
inline QString prompts(const QString& label) { return label + QStringLiteral("…"); }

// The plural of a noun this application actually uses. Not a general
// pluraliser — a general one would be wrong for exactly the words that matter.
inline QString plural(const QString& noun, int n)
{
    if (n == 1) return noun;
    if (noun.endsWith(QLatin1Char('h')) || noun.endsWith(QLatin1Char('s')))
        return noun + QStringLiteral("es");
    return noun + QStringLiteral("s");
}

// THE export label. `verb` is "Export" or "Extract"; `noun` is what the things
// are ("model", "texture", "file", "submesh", "material"); `oneName` is the
// name to use when there is exactly one. No ellipsis — wrap in prompts() when
// the action opens a dialog, which is what the caller knows and this does not.
inline QString exportLabel(const QString& verb, int count,
                           const QString& oneName, const QString& noun)
{
    if (count <= 0) return verb;
    if (count == 1 && !oneName.isEmpty())
        return QStringLiteral("%1 %2").arg(verb, oneName);
    return QStringLiteral("%1 %2 %3")
        .arg(verb, QLocale().toString(count), plural(noun, count));
}

// The same, for the silent path that writes to the remembered folder. Says
// where, because an action that writes without asking has to.
inline QString exportLastLabel(const QString& verb, int count,
                               const QString& oneName, const QString& noun,
                               const QString& dir)
{
    return withValue(
        QStringLiteral("%1 to last folder")
            .arg(exportLabel(verb, count, oneName, noun)),
        condensePath(dir));
}

// ── THE SUBJECT OF AN EXPORT ─────────────────────────────────────────────
// "Export .glb (rigged, bind pose)…" tells you the FILE FORMAT and the pose
// and never says what is being exported. The user's complaint was exactly
// that: the Export menu should say "Export character", "Export weapon",
// "Export model", "Export part", "Export scene" — the noun for the thing in
// front of them, which is the only part of that label they did not already
// know.
//
// So the tab supplies the noun (see BrowserTab::exportSubjectNoun) and this
// builds the label. The format stays, in parentheses, because .glb-vs-.fbx is
// still worth knowing — it is just not the headline.
inline QString exportSubject(const QString& subject, const QString& detail)
{
    return detail.isEmpty()
               ? QStringLiteral("Export %1").arg(subject)
               : QStringLiteral("Export %1 (%2)").arg(subject, detail);
}

}  // namespace core::MenuText
