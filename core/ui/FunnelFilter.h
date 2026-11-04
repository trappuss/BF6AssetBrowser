#pragma once
// The funnel filter control (template §9): a funnel button whose popup STAYS OPEN while you tick
// grouped facet checkboxes, a Match-any (OR) toggle, live "(n)" counts beside each facet, and the
// active facets as removable chips in a bar the host places in its header. POE2's FunnelFilter
// (the sticky QWidgetAction popup, the counts, the tint) over a VOCABULARY the plugin supplies —
// core::TagGroup from IGameStore::tagGroups(kind) — with FOX's wrapping ChipFlowLayout for the chips
// (a single row elides, and §15 says no elided labels anywhere).
//
// State here is the set of ACTIVE FACET IDS (POE2's model; the host persists them under its own
// key). A game that prefers FOX's "ticks rewrite the query as #tag" connects changed() to a
// SearchQuery::withTag rewrite instead of persisting ids — both are one connect away.
#include "core/types/Asset.h"

#include <QHash>
#include <QStringList>
#include <QToolButton>
#include <QVector>
#include <functional>

class QCheckBox;
class QWidget;
class QVBoxLayout;

namespace core {

class ChipFlowLayout;

class FunnelFilter : public QToolButton {
    Q_OBJECT
public:
    explicit FunnelFilter(QWidget* parent = nullptr);

    // Replace the vocabulary. Ticks whose id survives are kept; the rest are dropped and, if any
    // were dropped, changed() fires so the host re-filters.
    void setGroups(const QVector<TagGroup>& groups);
    const QVector<TagGroup>& groups() const { return m_groups; }

    QStringList activeIds() const;
    bool matchAny() const;
    void setActive(const QStringList& ids, bool matchAny);   // restore persisted state (no signal)
    void setCounts(const QHash<QString, int>& counts);       // "(n)" per facet id + chip counts
    QWidget* chipBar() const { return m_chipBar; }

    // The evaluator the list model and bulk share: grouped AND-of-ORs by default, OR of the whole
    // ticked set with matchAny. `hit(facetId)` is the row's own answer (IGameStore::hasTag).
    static bool passes(const QVector<TagGroup>& groups, const QStringList& active, bool matchAny,
                       const std::function<bool(const QString& id)>& hit);
    static QString selfTest();

Q_SIGNALS:
    void changed();

private:
    void rebuildBoxes();
    void rebuildChips();
    void refreshTint();
    QString labelFor(const QString& id, QString* group = nullptr) const;

    QVector<TagGroup> m_groups;
    QWidget* m_panel = nullptr;
    QVBoxLayout* m_panelLayout = nullptr;
    QWidget* m_chipBar = nullptr;
    ChipFlowLayout* m_chipFlow = nullptr;
    QCheckBox* m_matchAny = nullptr;
    QHash<QString, QCheckBox*> m_boxes;   // facet id → its checkbox
    QHash<QString, int> m_counts;
};

}  // namespace core
