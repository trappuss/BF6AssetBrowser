#pragma once
// A flat, dense list over one KIND of an IGameStore's rows — the centrepiece of every list-driven
// tab (template §4). POE2's AssetListModel (std::function icon provider, applyFilters once per
// keystroke, facet counts) generalised over core::AssetRow, with D4's SnoListModel presentation
// hooks expressed as row flags (Unrenderable dims the row and prefixes ⚠; Encrypted prefixes 🔒).
//
// Filtering goes through the ONE matcher: SearchQuery (space-AND, -not, quotes, #tag) over
// core::search::matches, with the plugin's id policy; #tag terms resolve through IGameStore::hasTag
// OR the row's searchBlob (so POE2's "#family:metalrough" text tokens and FOX's exact tags both
// work); funnel facets through FunnelFilter::passes. The Models list, the Textures list and Bulk
// all use this class, so they cannot disagree on what a query means — the drift QueryTerm.h exists
// to end (D4's Textures tab bypassed it; tabs-store.md §9.1).
//
// The model holds a filtered vector of indices into a copied base row set; QTableView virtualises
// the rows, so a million assets cost nothing to display. Filtering rebuilds the vector (debounced
// by the tab). GUI thread only (IGameStore's row/hasTag/searchBlob contract).
#include "core/store/IGameStore.h"
#include "core/types/Asset.h"

#include <QAbstractTableModel>
#include <QCache>
#include <QHash>
#include <QIcon>
#include <QPixmap>
#include <QStringList>
#include <QVector>
#include <functional>

namespace core {

class AssetListModel : public QAbstractTableModel {
    Q_OBJECT
public:
    enum Column { ColName = 0, ColInGameName, ColKind, ColSize, ColId, ColCollection, ColCount };

    explicit AssetListModel(QObject* parent = nullptr);

    // Point the model at a store and one or more of its kinds (IGameStore::kinds()) — the Bulk tab
    // lists models and textures together. Copies the rows: the store may be re-opened underneath,
    // and a snapshot is what a QTableView must have. Null store or unknown kinds empty the list.
    void setSource(IGameStore* store, const QStringList& kinds);
    IGameStore* store() const { return m_store; }
    QStringList kinds() const { return m_kinds; }
    QStringList rowKinds() const;        // the distinct AssetRow::kind values in the base set, sorted
    void refreshFromStore();             // rows changed in place (secondary indexes arrived)

    // The vocabulary FunnelFilter shows; needed here for grouped facet evaluation and counts.
    // The store's groups plus the core's own "Special" group: one entry per row flag (New, Animated,
    // Rigged, Orphaned, Encrypted, Unnamed, Shadowed) that at least one row in the base set carries —
    // a game sees exactly the flags it sets. Facet ids "flag:<name>"; tabs hand tagGroups() to the funnel.
    // `flagsInEveryKind`: offer only flags rows of EVERY listed store kind carry (Bulk: a model-only
    // flag like Animated would silently hide every texture — the rule Bulk applies to tag groups).
    void setTagGroups(const QVector<TagGroup>& groups, bool flagsInEveryKind = false);
    const QVector<TagGroup>& tagGroups() const { return m_groups; }
    static QString flagFacetId(AssetRow::Flag f);   // "flag:animated" …; empty for a flag with no facet

    // Set query + kind facet + funnel facets together and rebuild ONCE.
    void applyFilters(const QString& query, const QString& kindFacet, const QStringList& facetIds, bool matchAny);
    void setQuery(const QString& q) { applyFilters(q, m_kindFacet, m_facetIds, m_matchAny); }
    QString query() const { return m_query; }

    // Per-facet match counts over the current search (query + kind facet) BEFORE the funnel is
    // applied — "the count you see is the set you get" (template §9/§14).
    QHash<QString, int> facetCounts() const;

    int rowCount(const QModelIndex& = {}) const override { return m_rows.size(); }
    int columnCount(const QModelIndex& = {}) const override { return m_showCollection ? ColCount : ColCount - 1; }
    QVariant data(const QModelIndex&, int role) const override;
    QVariant headerData(int section, Qt::Orientation, int role) const override;
    void sort(int column, Qt::SortOrder order = Qt::AscendingOrder) override;

    const AssetRow& rowAt(int row) const;     // a static empty row when out of range
    QString storeKindAt(int row) const;       // the IGameStore kind this row came from
    AssetId idAt(int row) const { return rowAt(row).id; }
    // The row a harness/search selection should take for `text`: the first whose name — or the
    // name's file stem — equals it (case-insensitive), else row 0. A substring search puts
    // npc_barF_base00 ahead of barF_base00; asking for barF_base00 must load barF_base00.
    int bestRowFor(const QString& text) const;
    int rowOfId(const AssetId& id) const;
    int totalInBaseSet() const { return m_base.size(); }
    QVector<AssetId> visibleIds() const;

    // Whether the collection column exists (D4 has one; FOX and POE2 do not — an empty column
    // is clutter, so a store whose rows never carry one hides it).
    bool showsCollection() const { return m_showCollection; }

    // ── Thumbnail icons (grid view) — D4's SnoListModel discipline via POE2: a bounded QCache
    //    keyed by id so a repaint (a mouse-over is one) never re-runs the provider; a miss is
    //    cached as a null icon so un-rendered rows do not hammer the provider every frame.
    void setIconProvider(std::function<QPixmap(const AssetId&)> fn);
    void setIconPx(int px);
    int  iconPx() const { return m_iconPx; }
    void setGridMode(bool on);
    void refreshIcon(const AssetId& id);
    void refreshAllIcons();

private:
    void rebuild();
    void sortRows();
    QVariant iconData(const AssetId& id) const;
    QString hayFor(const AssetRow& r) const;
    bool facetHit(const AssetRow& r, const QString& id) const;   // a tag, or a "flag:" facet
    bool tagHit(const AssetRow& r, const QString& tag) const;

    IGameStore* m_store = nullptr;
    QStringList m_kinds;
    QVector<AssetRow> m_base;        // every row of the kinds (the tab's universe)
    QVector<QString>  m_baseKind;    // parallel: the store kind each base row came from
    QVector<int> m_queryRows;        // after query + kind facet, BEFORE the funnel (for counts)
    QVector<int> m_rows;             // visible
    QVector<TagGroup> m_groups;
    QString m_query, m_kindFacet;
    QStringList m_facetIds;
    bool m_matchAny = false;
    bool m_showCollection = false;
    int m_sortColumn = -1;
    Qt::SortOrder m_sortOrder = Qt::AscendingOrder;

    std::function<QPixmap(const AssetId&)> m_iconProvider;
    mutable QCache<AssetId, QIcon> m_iconCache{ 96 * 1024 };
    int  m_iconPx = 0;
    bool m_gridMode = false;
};

}  // namespace core
