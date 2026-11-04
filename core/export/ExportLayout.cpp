#include "core/export/ExportLayout.h"
#include "core/export/NameTemplate.h"

#include <QDir>
#include <QFileInfo>
#include <QMap>
#include <QStringList>

namespace core::layout {
namespace {

const QString kMisc = QStringLiteral("_misc");

// Every segment of a generated folder path goes through the file-name rules — the reserved device
// names apply to directories exactly as they do to files, and POE2's mirror mode builds folders out
// of game path segments it has never validated.
QString segment(const QString& s) { return core::name::sanitizeStem(s); }

QString mirrorPath(const Item& it)
{
    if (it.sourcePath.isEmpty()) return QString();
    const QString dir = QFileInfo(it.sourcePath).path();
    if (dir.isEmpty() || dir == QStringLiteral(".")) return QString();
    QStringList out;
    for (const QString& seg : dir.split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
        const QString s = segment(seg);
        if (!s.isEmpty()) out << s;
    }
    return out.join(QLatin1Char('/'));
}

QString perItemFolder(const Spec& spec, const Item& it)
{
    core::name::Fields f;
    f.fileName = it.sourcePath.isEmpty() ? it.name : QFileInfo(it.sourcePath).completeBaseName();
    if (f.fileName.isEmpty()) f.fileName = it.name;
    f.id = it.id.value;
    return core::name::apply(spec.itemNameTemplate, f);
}

}  // namespace

const Mode* Spec::find(const QString& id) const
{
    for (const Mode& m : modes) if (m.id == id) return &m;
    return nullptr;
}

bool Spec::isKnown(const QString& id) const
{
    return id.isEmpty() || find(id) != nullptr;
}

QVector<Mode> builtinModes()
{
    return {
        {kFlat(), QStringLiteral("Flat — everything at the root"),
         QStringLiteral("The output folder holds the exported files and nothing else."), false},
        {kMirrorPath(), QStringLiteral("Mirror the game's folders"),
         QStringLiteral("Each file lands under the same folders it has inside the game."), false},
        {kPerItem(), QStringLiteral("One folder per asset"),
         QStringLiteral("Each asset gets a folder named after it — useful when every export "
                        "carries its own textures."), false},
    };
}

QString resolveMode(const Spec& spec, const QString& stored)
{
    return spec.isKnown(stored) ? stored : kFlat();
}

QString subfolderFor(const Spec& spec, const QString& mode, const Item& item)
{
    if (mode.isEmpty() || !spec.isKnown(mode)) return QString();
    if (mode == kMirrorPath()) {
        const QString p = mirrorPath(item);
        return p.isEmpty() ? kMisc : p;
    }
    if (mode == kPerItem()) {
        const QString f = perItemFolder(spec, item);
        return f.isEmpty() ? kMisc : f;
    }
    if (!spec.tagFor) return kMisc;
    const QString tag = segment(spec.tagFor(mode, item));
    return tag.isEmpty() ? kMisc : tag;
}

QVector<Group> group(const Spec& spec, const QString& mode, const QVector<Item>& items,
                     bool* fellBackToFlat)
{
    if (fellBackToFlat) *fellBackToFlat = false;
    const QString eff = resolveMode(spec, mode);
    if (eff.isEmpty() || items.isEmpty()) return {Group{QString(), items}};

    const Mode* m = spec.find(eff);
    if (m && m->needsTags && spec.tagsReady && !spec.tagsReady()) {
        // Tags are not built yet. One "_misc" folder holding the entire run is worse than no
        // grouping at all, so fall back — and tell the caller, because silently ignoring the
        // setting is exactly as confusing as silently misusing it.
        if (fellBackToFlat) *fellBackToFlat = true;
        return {Group{QString(), items}};
    }

    // QMap, not QHash: folders come out in a stable order, so two runs over one selection log the
    // same sequence.
    QMap<QString, QVector<Item>> by;
    for (const Item& it : items) by[subfolderFor(spec, eff, it)].append(it);

    QVector<Group> out;
    out.reserve(by.size());
    for (auto g = by.constBegin(); g != by.constEnd(); ++g) out.append(Group{g.key(), g.value()});
    return out;
}

QString folderFor(const QString& root, const Group& g)
{
    return g.folder.isEmpty() ? root : QDir(root).filePath(g.folder);
}

QString selfTest()
{
    const auto bad = [](const QString& m) { return QStringLiteral("ExportLayout self-test: ") + m; };

    Spec spec;
    spec.modes = builtinModes();
    spec.modes.append({QStringLiteral("family"), QStringLiteral("By family"), QString(), true});
    bool tagsUp = true;
    spec.tagsReady = [&tagsUp] { return tagsUp; };
    spec.tagFor = [](const QString& mode, const Item& it) -> QString {
        if (mode != QStringLiteral("family")) return QString();
        return it.name.startsWith(QStringLiteral("bar")) ? QStringLiteral("Barbarian") : QString();
    };

    Item a; a.id = {1, 10}; a.name = QStringLiteral("barM_sets53_hlm");
    a.sourcePath = QStringLiteral("art/models/items/armours/foo/barM_sets53_hlm.smd");
    Item b; b.id = {1, 11}; b.name = QStringLiteral("sorF_base00");
    b.sourcePath = QStringLiteral("art/models/items/armours/foo/sorF_base00.smd");
    Item bare; bare.id = {1, 12}; bare.name = QStringLiteral("loose");
    bare.sourcePath = QStringLiteral("loose.dds");

    if (!subfolderFor(spec, kFlat(), a).isEmpty()) return bad(QStringLiteral("Flat must be the root"));
    if (!subfolderFor(spec, QStringLiteral("Nonsense"), a).isEmpty())
        return bad(QStringLiteral("an unknown mode must read as Flat, not as a tag name"));
    if (subfolderFor(spec, kMirrorPath(), a) != QStringLiteral("art/models/items/armours/foo"))
        return bad(QStringLiteral("mirror mode should reproduce the game's directory"));
    if (subfolderFor(spec, kMirrorPath(), bare) != QStringLiteral("_misc"))
        return bad(QStringLiteral("an asset with no directory must not land loose in the parent"));
    if (subfolderFor(spec, kPerItem(), a) != QStringLiteral("barM_sets53_hlm"))
        return bad(QStringLiteral("a per-asset folder should be named from the asset's stem"));
    if (subfolderFor(spec, QStringLiteral("family"), a) != QStringLiteral("Barbarian"))
        return bad(QStringLiteral("a tag mode should use the plugin's answer"));
    if (subfolderFor(spec, QStringLiteral("family"), b) != QStringLiteral("_misc"))
        return bad(QStringLiteral("an unplaceable item must go to _misc, never to the root"));

    // Grouping: stable order, one group per folder, every item kept exactly once.
    {
        const QVector<Item> items{a, b, bare};
        const QVector<Group> gs = group(spec, kMirrorPath(), items);
        if (gs.size() != 2) return bad(QStringLiteral("expected 2 mirror groups, got %1").arg(gs.size()));
        if (gs[0].folder != QStringLiteral("_misc") || gs[1].folder != QStringLiteral("art/models/items/armours/foo"))
            return bad(QStringLiteral("groups are not in a stable sorted order"));
        int total = 0;
        for (const Group& g : gs) total += g.items.size();
        if (total != items.size()) return bad(QStringLiteral("grouping lost or duplicated an item"));

        const QVector<Group> flat = group(spec, kFlat(), items);
        if (flat.size() != 1 || !flat[0].folder.isEmpty() || flat[0].items.size() != 3)
            return bad(QStringLiteral("Flat must be one group at the root, not a special case"));
    }

    // Tags not ready: fall back to Flat AND say so.
    {
        tagsUp = false;
        bool fell = false;
        const QVector<Group> gs = group(spec, QStringLiteral("family"), {a, b}, &fell);
        if (!fell) return bad(QStringLiteral("a fallback to Flat must be reported, not silent"));
        if (gs.size() != 1 || !gs[0].folder.isEmpty())
            return bad(QStringLiteral("unready tags should group everything at the root, not under _misc"));
        tagsUp = true;
    }

    // A tag name from the plugin is still untrusted text: it becomes a folder.
    {
        Spec s2 = spec;
        s2.tagFor = [](const QString&, const Item&) { return QStringLiteral("a/b"); };
        if (subfolderFor(s2, QStringLiteral("family"), a).contains(QLatin1Char('/')))
            return bad(QStringLiteral("a tag carrying a path separator must not escape the destination"));
    }

    if (folderFor(QStringLiteral("/out"), Group{QString(), {}}) != QStringLiteral("/out"))
        return bad(QStringLiteral("a Flat group resolves to the destination itself"));
    if (folderFor(QStringLiteral("/out"), Group{QStringLiteral("Barbarian"), {}}) != QStringLiteral("/out/Barbarian"))
        return bad(QStringLiteral("folderFor should join the group under the root"));
    return QString();
}

}  // namespace core::layout
