#include "core/ui/AssetListModel.h"
#include "core/ui/FunnelFilter.h"
#include "core/ui/SearchQuery.h"

#include <QColor>
#include <QLocale>
#include <algorithm>

namespace core {

AssetListModel::AssetListModel(QObject* parent) : QAbstractTableModel(parent) {}

void AssetListModel::setSource(IGameStore* store, const QStringList& kinds)
{
    beginResetModel();
    m_store = store;
    m_kinds = kinds;
    m_base.clear();
    m_baseKind.clear();
    m_rows.clear();
    m_queryRows.clear();
    m_showCollection = false;
    if (m_store && m_store->isOpen()) {
        const QVector<QString> have = m_store->kinds();
        for (const QString& kind : kinds) {
            if (!have.contains(kind)) continue;
            const int n = m_store->rowCount(kind);
            m_base.reserve(m_base.size() + n);
            for (int i = 0; i < n; ++i) {
                AssetRow r = m_store->row(kind, i);
                if (!r.collection.isEmpty()) m_showCollection = true;
                m_base.append(std::move(r));
                m_baseKind.append(kind);
            }
        }
    }
    m_iconCache.clear();
    endResetModel();
    rebuild();
}

void AssetListModel::refreshFromStore()
{
    setSource(m_store, m_kinds);
}

QStringList AssetListModel::rowKinds() const
{
    QStringList out;
    for (const AssetRow& r : m_base) if (!r.kind.isEmpty() && !out.contains(r.kind)) out << r.kind;
    out.sort(Qt::CaseInsensitive);
    return out;
}

void AssetListModel::applyFilters(const QString& query, const QString& kindFacet, const QStringList& facetIds, bool matchAny)
{
    m_query = query; m_kindFacet = kindFacet; m_facetIds = facetIds; m_matchAny = matchAny;
    rebuild();
}

QString AssetListModel::hayFor(const AssetRow& r) const
{
    // Name plus everything else a person searches by. D4 folds tags into a search blob, POE2 adds
    // shader meta and in-game names, FOX searches the path: the union is one haystack.
    QString h = r.name;
    if (!r.inGameName.isEmpty()) { h += QLatin1Char(' '); h += r.inGameName; }
    if (!r.collection.isEmpty()) { h += QLatin1Char(' '); h += r.collection; }
    if (m_store) {
        const QString blob = m_store->searchBlob(r.id);
        if (!blob.isEmpty()) { h += QLatin1Char(' '); h += blob; }
    }
    return h;
}

namespace {
struct FlagFacet { AssetRow::Flag flag; const char* id; const char* label; };
// The order the Special group lists them. D4's own Special section: Latest, Animated, Rigged,
// Orphaned; the rest are flags other games (or D4's other filters) set.
const FlagFacet kFlagFacets[] = {
    { AssetRow::New,       "flag:new",       "New this update" },
    { AssetRow::Animated,  "flag:animated",  "Animated (owns or inherits clips)" },
    { AssetRow::Rigged,    "flag:rigged",    "Rigged" },
    { AssetRow::Orphaned,  "flag:orphaned",  "Orphaned (nothing uses it)" },
    { AssetRow::Encrypted, "flag:encrypted", "Encrypted" },
    { AssetRow::Unnamed,   "flag:unnamed",   "Unnamed" },
    { AssetRow::Shadowed,  "flag:shadowed",  "Shadowed (mod copy)" },
};
}  // namespace

QString AssetListModel::flagFacetId(AssetRow::Flag f)
{
    for (const FlagFacet& ff : kFlagFacets) if (ff.flag == f) return QString::fromLatin1(ff.id);
    return {};
}

void AssetListModel::setTagGroups(const QVector<TagGroup>& groups, bool flagsInEveryKind)
{
    m_groups = groups;
    uint32_t seen = 0;
    if (!flagsInEveryKind) {
        for (const AssetRow& r : m_base) seen |= r.flags;
    } else {
        QHash<QString, uint32_t> byKind;
        for (int i = 0; i < m_base.size(); ++i) byKind[m_baseKind.value(i)] |= m_base[i].flags;
        seen = byKind.isEmpty() ? 0u : ~0u;
        for (uint32_t f : byKind) seen &= f;
    }
    TagGroup special;
    special.id = QStringLiteral("flags");
    special.label = QStringLiteral("Special");
    for (const FlagFacet& ff : kFlagFacets)
        if (seen & uint32_t(ff.flag)) special.entries.append({ QString::fromLatin1(ff.id), QString::fromLatin1(ff.label), 0 });
    if (!special.entries.isEmpty()) m_groups.append(special);
}

bool AssetListModel::facetHit(const AssetRow& r, const QString& id) const
{
    for (const FlagFacet& ff : kFlagFacets) if (id == QLatin1String(ff.id)) return r.has(ff.flag);   // exact ids
    return r.tags.contains(id) || (m_store && m_store->hasTag(r.id, id));
}

bool AssetListModel::tagHit(const AssetRow& r, const QString& tag) const
{
    if (r.tags.contains(tag, Qt::CaseInsensitive)) return true;
    if (m_store && m_store->hasTag(r.id, tag)) return true;
    // POE2's "#family:metalrough" / "#armour" — a text token against the metadata haystack.
    if (m_store) {
        const QString blob = m_store->searchBlob(r.id);
        if (!blob.isEmpty() && search::matches(blob, tag)) return true;
    }
    return search::matches(r.name, tag);   // POE2: '#' terms also match the path
}

void AssetListModel::rebuild()
{
    beginResetModel();
    m_queryRows.clear();
    m_rows.clear();
    const SearchQuery q(m_query);
    const bool needHay = q.hasText();
    m_queryRows.reserve(m_base.size());
    for (int i = 0; i < m_base.size(); ++i) {
        const AssetRow& r = m_base[i];
        if (!m_kindFacet.isEmpty() && r.kind.compare(m_kindFacet, Qt::CaseInsensitive) != 0) continue;
        if (!q.isEmpty()) {
            if (needHay && !q.matchesWithId(hayFor(r), r.id)) continue;
            bool tagsOk = true;
            for (const QString& t : q.mustTags())    if (!tagHit(r, t)) { tagsOk = false; break; }
            if (tagsOk) for (const QString& t : q.mustNotTags()) if (tagHit(r, t)) { tagsOk = false; break; }
            if (!tagsOk) continue;
        }
        m_queryRows.append(i);
    }
    if (m_facetIds.isEmpty()) {
        m_rows = m_queryRows;
    } else {
        m_rows.reserve(m_queryRows.size());
        for (int i : m_queryRows) {
            const AssetRow& r = m_base[i];
            auto hit = [&](const QString& id) { return facetHit(r, id); };
            if (FunnelFilter::passes(m_groups, m_facetIds, m_matchAny, hit)) m_rows.append(i);
        }
    }
    sortRows();
    endResetModel();
}

QHash<QString, int> AssetListModel::facetCounts() const
{
    QHash<QString, int> counts;
    for (int i : m_queryRows) {
        const AssetRow& r = m_base[i];
        for (const TagGroup& g : m_groups)
            for (const TagGroup::Entry& e : g.entries)
                if (facetHit(r, e.id)) ++counts[e.id];
    }
    return counts;
}

void AssetListModel::sort(int column, Qt::SortOrder order)
{
    if (column < 0 || column >= ColCount) return;
    m_sortColumn = column; m_sortOrder = order;
    beginResetModel();
    sortRows();
    endResetModel();
}

void AssetListModel::sortRows()
{
    if (m_sortColumn < 0 || m_rows.isEmpty()) return;
    const bool asc = (m_sortOrder == Qt::AscendingOrder);
    if (m_sortColumn == ColSize) {
        std::stable_sort(m_rows.begin(), m_rows.end(), [&](int a, int b) {
            return asc ? m_base[a].size < m_base[b].size : m_base[a].size > m_base[b].size; });
        return;
    }
    if (m_sortColumn == ColId) {
        std::stable_sort(m_rows.begin(), m_rows.end(), [&](int a, int b) {
            return asc ? m_base[a].id.value < m_base[b].id.value : m_base[a].id.value > m_base[b].id.value; });
        return;
    }
    auto key = [&](int i) -> const QString& {
        const AssetRow& r = m_base[i];
        switch (m_sortColumn) {
            case ColInGameName: return r.inGameName;
            case ColKind:       return r.kind;
            case ColCollection: return r.collection;
            default:            return r.name;
        }
    };
    std::stable_sort(m_rows.begin(), m_rows.end(), [&](int a, int b) {
        const int c = QString::compare(key(a), key(b), Qt::CaseInsensitive);
        return asc ? c < 0 : c > 0;
    });
}

int AssetListModel::bestRowFor(const QString& text) const
{
    const QString t = text.trimmed();
    for (int i = 0; i < m_rows.size(); ++i) {
        const QString& n = rowAt(i).name;
        if (n.compare(t, Qt::CaseInsensitive) == 0) return i;
        const int slash = int(qMax(n.lastIndexOf(QLatin1Char('/')), n.lastIndexOf(QLatin1Char('\\'))));
        const QString file = n.mid(slash + 1);
        const int dot = int(file.lastIndexOf(QLatin1Char('.')));
        if ((dot > 0 ? file.left(dot) : file).compare(t, Qt::CaseInsensitive) == 0) return i;
    }
    return 0;
}

const AssetRow& AssetListModel::rowAt(int row) const
{
    static const AssetRow kNone;
    if (row < 0 || row >= m_rows.size()) return kNone;
    return m_base[m_rows[row]];
}

QString AssetListModel::storeKindAt(int row) const
{
    if (row < 0 || row >= m_rows.size()) return QString();
    return m_baseKind.value(m_rows[row]);
}

int AssetListModel::rowOfId(const AssetId& id) const
{
    for (int r = 0; r < m_rows.size(); ++r) if (m_base[m_rows[r]].id == id) return r;
    return -1;
}

QVector<AssetId> AssetListModel::visibleIds() const
{
    QVector<AssetId> out; out.reserve(m_rows.size());
    for (int i : m_rows) out.append(m_base[i].id);
    return out;
}

QVariant AssetListModel::data(const QModelIndex& idx, int role) const
{
    if (!idx.isValid() || idx.row() >= m_rows.size()) return {};
    const AssetRow& r = m_base[m_rows[idx.row()]];
    switch (role) {
    case Qt::DisplayRole:
        switch (idx.column()) {
            case ColName: {
                // Grid mode: the tile CAPTION is the short name, not the path.
                QString n = m_gridMode ? r.name.section(QLatin1Char('/'), -1) : r.name;
                if (r.has(AssetRow::Unrenderable)) n.prepend(QStringLiteral("⚠ "));
                if (r.has(AssetRow::Encrypted))    n.prepend(QStringLiteral("🔒 "));
                return n;
            }
            case ColInGameName: return r.inGameName;
            case ColKind:       return r.kind;
            case ColSize:       return r.size ? QLocale().formattedDataSize(qint64(r.size)) : QString();
            case ColId:         return r.id.value ? QString::number(r.id.value) : QString();
            case ColCollection: return r.collection;
        }
        break;
    case Qt::ForegroundRole:
        if (r.has(AssetRow::Unrenderable) || r.has(AssetRow::Encrypted)) return QColor(0x8a, 0x8a, 0x8a);
        break;
    case Qt::DecorationRole:
        if (idx.column() == ColName && m_iconProvider) return iconData(r.id);
        break;
    case Qt::TextAlignmentRole:
        if (m_gridMode && idx.column() == ColName) return int(Qt::AlignHCenter | Qt::AlignTop);
        break;
    case Qt::ToolTipRole: {
        QString tip = r.name;
        if (!r.inGameName.isEmpty()) tip += QStringLiteral("\n") + r.inGameName;
        if (r.has(AssetRow::Unrenderable)) tip += QStringLiteral("\nNot renderable in this tool");
        if (r.has(AssetRow::Encrypted))    tip += QStringLiteral("\nEncrypted — present but unreadable");
        return tip;
    }
    case Qt::UserRole:
        return QVariant::fromValue(r.id);
    default: break;
    }
    return {};
}

QVariant AssetListModel::iconData(const AssetId& id) const
{
    if (const QIcon* hit = m_iconCache.object(id)) return hit->isNull() ? QVariant() : QVariant(*hit);
    QPixmap pm = m_iconProvider ? m_iconProvider(id) : QPixmap();
    if (pm.isNull()) { m_iconCache.insert(id, new QIcon(), 1); return QVariant(); }
    if (m_iconPx > 0 && (pm.width() != m_iconPx || pm.height() != m_iconPx))
        pm = pm.scaled(m_iconPx, m_iconPx, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    const QIcon ic(pm);
    const int costKB = qMax(1, int((qint64(pm.width()) * pm.height() * 4) / 1024));
    m_iconCache.insert(id, new QIcon(ic), costKB);
    return QVariant(ic);
}

void AssetListModel::setIconProvider(std::function<QPixmap(const AssetId&)> fn)
{
    m_iconProvider = std::move(fn);
    refreshAllIcons();
}

void AssetListModel::setIconPx(int px)
{
    if (px == m_iconPx) return;
    m_iconPx = px;
    refreshAllIcons();   // every cached icon is scaled to the OLD size
}

void AssetListModel::setGridMode(bool on)
{
    if (on == m_gridMode) return;
    m_gridMode = on;
    if (!m_rows.isEmpty())
        emit dataChanged(index(0, ColName), index(m_rows.size() - 1, ColName),
                         {Qt::DisplayRole, Qt::DecorationRole, Qt::TextAlignmentRole});
}

void AssetListModel::refreshIcon(const AssetId& id)
{
    m_iconCache.remove(id);
    const int r = rowOfId(id);
    if (r >= 0) emit dataChanged(index(r, ColName), index(r, ColName), {Qt::DecorationRole});
}

void AssetListModel::refreshAllIcons()
{
    m_iconCache.clear();
    if (!m_rows.isEmpty())
        emit dataChanged(index(0, ColName), index(m_rows.size() - 1, ColName), {Qt::DecorationRole});
}

QVariant AssetListModel::headerData(int section, Qt::Orientation o, int role) const
{
    if (role != Qt::DisplayRole || o != Qt::Horizontal) return {};
    switch (section) {
        case ColName:       return QStringLiteral("Name");
        case ColInGameName: return QStringLiteral("In-game name");
        case ColKind:       return QStringLiteral("Type");
        case ColSize:       return QStringLiteral("Size");
        case ColId:         return QStringLiteral("Id");
        case ColCollection: return QStringLiteral("Collection");
    }
    return {};
}

}  // namespace core
