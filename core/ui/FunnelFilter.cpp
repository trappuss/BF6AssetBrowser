#include "core/ui/FunnelFilter.h"
#include "core/ui/FilterChips.h"
#include "core/ui/Glyphs.h"

#include <QCheckBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <QWidgetAction>

namespace core {

FunnelFilter::FunnelFilter(QWidget* parent) : QToolButton(parent)
{
    setText(QStringLiteral("Filters"));
    setIcon(glyph::toolIcon(18));   // the funnel
    setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    setToolTip(QStringLiteral("Filter by category and type — the popup stays open while you tick."));
    setPopupMode(QToolButton::InstantPopup);

    // The popup: one QWidgetAction holding a panel of grouped checkboxes. A QWidgetAction's widget
    // takes clicks WITHOUT closing the menu — the "menu that stays open" trick (POE2, template §9).
    auto* menu = new QMenu(this);
    m_panel = new QWidget(menu);
    m_panelLayout = new QVBoxLayout(m_panel);
    m_panelLayout->setContentsMargins(10, 8, 10, 8);
    m_matchAny = new QCheckBox(QStringLiteral("Match any (OR) instead of all"), m_panel);
    m_panelLayout->addWidget(m_matchAny);
    connect(m_matchAny, &QCheckBox::toggled, this, [this] { emit changed(); });
    m_panelLayout->addStretch(1);

    auto* scroll = new QScrollArea(menu);
    scroll->setWidget(m_panel); scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setMinimumWidth(260); scroll->setMinimumHeight(360);
    auto* wa = new QWidgetAction(menu); wa->setDefaultWidget(scroll);
    menu->addAction(wa);
    setMenu(menu);

    m_chipBar = new QWidget(parent);
    m_chipFlow = new ChipFlowLayout(m_chipBar);
    m_chipBar->setVisible(false);
    refreshTint();
}

void FunnelFilter::setGroups(const QVector<TagGroup>& groups)
{
    const QStringList before = activeIds();
    const bool any = m_matchAny->isChecked();
    m_groups = groups;
    rebuildBoxes();
    QStringList kept;
    for (const QString& id : before) if (m_boxes.contains(id)) kept << id;
    setActive(kept, any);
    if (kept.size() != before.size()) emit changed();
}

void FunnelFilter::rebuildBoxes()
{
    // Everything but the Match-any box and the trailing stretch goes.
    for (QCheckBox* cb : m_boxes) cb->deleteLater();
    m_boxes.clear();
    while (m_panelLayout->count() > 1) {
        QLayoutItem* it = m_panelLayout->takeAt(1);
        if (it->widget() && it->widget() != m_matchAny) it->widget()->deleteLater();
        delete it;
    }
    for (const TagGroup& g : m_groups) {
        auto* line = new QFrame(m_panel); line->setFrameShape(QFrame::HLine); line->setFrameShadow(QFrame::Sunken);
        m_panelLayout->addWidget(line);
        m_panelLayout->addWidget(new QLabel(QStringLiteral("<b>%1</b>").arg(g.label.toHtmlEscaped()), m_panel));
        for (const TagGroup::Entry& e : g.entries) {
            auto* cb = new QCheckBox(e.label, m_panel);
            cb->setProperty("facetId", e.id);
            m_panelLayout->addWidget(cb);
            m_boxes.insert(e.id, cb);
            connect(cb, &QCheckBox::toggled, this, [this] { rebuildChips(); refreshTint(); emit changed(); });
        }
    }
    m_panelLayout->addStretch(1);
}

QStringList FunnelFilter::activeIds() const
{
    QStringList ids;
    // Registration order, not QHash order, so the persisted list and the chips are stable.
    for (const TagGroup& g : m_groups)
        for (const TagGroup::Entry& e : g.entries)
            if (QCheckBox* cb = m_boxes.value(e.id)) if (cb->isChecked()) ids << e.id;
    return ids;
}

bool FunnelFilter::matchAny() const { return m_matchAny->isChecked(); }

void FunnelFilter::setActive(const QStringList& ids, bool matchAny)
{
    for (auto it = m_boxes.constBegin(); it != m_boxes.constEnd(); ++it) {
        QSignalBlocker b(it.value());
        it.value()->setChecked(ids.contains(it.key()));
    }
    { QSignalBlocker b(m_matchAny); m_matchAny->setChecked(matchAny); }
    rebuildChips();
    refreshTint();
}

QString FunnelFilter::labelFor(const QString& id, QString* group) const
{
    for (const TagGroup& g : m_groups)
        for (const TagGroup::Entry& e : g.entries)
            if (e.id == id) { if (group) *group = g.label; return e.label; }
    return id;
}

void FunnelFilter::setCounts(const QHash<QString, int>& counts)
{
    m_counts = counts;
    for (const TagGroup& g : m_groups)
        for (const TagGroup::Entry& e : g.entries)
            if (QCheckBox* cb = m_boxes.value(e.id))
                cb->setText(QStringLiteral("%1 (%2)").arg(e.label).arg(counts.value(e.id, 0)));
    rebuildChips();
}

void FunnelFilter::rebuildChips()
{
    while (QLayoutItem* it = m_chipFlow->takeAt(0)) { if (it->widget()) it->widget()->deleteLater(); delete it; }
    const QStringList ids = activeIds();
    if (ids.isEmpty()) { m_chipBar->setVisible(false); return; }
    m_chipBar->setVisible(true);
    for (const QString& id : ids) {
        QString group;
        const QString label = labelFor(id, &group);
        const int n = m_counts.value(id, -1);
        auto* chip = new QToolButton(m_chipBar);
        chip->setText(n >= 0 ? QStringLiteral("%1: %2 (%3)  ✕").arg(group, label).arg(n)
                             : QStringLiteral("%1: %2  ✕").arg(group, label));
        chip->setToolTip(QStringLiteral("Remove this filter"));
        chip->setAutoRaise(true);
        // 3px, not a pill: every other control in the window is a square-cornered Qt widget.
        chip->setStyleSheet(QStringLiteral(
            "QToolButton{border:1px solid palette(mid);border-radius:3px;padding:2px 8px;background:palette(button);}"
            "QToolButton:hover{border-color:palette(highlight);}"));
        connect(chip, &QToolButton::clicked, this, [this, id] {
            if (auto* cb = m_boxes.value(id)) cb->setChecked(false);   // fires changed() + rebuild
        });
        m_chipFlow->addWidget(chip);
    }
    m_chipBar->updateGeometry();
}

void FunnelFilter::refreshTint()
{
    const bool active = !activeIds().isEmpty();
    setText(active ? QStringLiteral("Filters •") : QStringLiteral("Filters"));
    setStyleSheet(QStringLiteral(
        "QToolButton{border:1px solid palette(mid);border-radius:3px;padding:4px 10px;%1}"
        "QToolButton:hover{border-color:palette(highlight);}"
        "QToolButton::menu-indicator{width:0px;}")
        .arg(active ? QStringLiteral("font-weight:bold;color:palette(highlight);") : QString()));
}

bool FunnelFilter::passes(const QVector<TagGroup>& groups, const QStringList& active, bool matchAny,
                          const std::function<bool(const QString&)>& hit)
{
    if (active.isEmpty()) return true;
    if (matchAny) {
        for (const QString& id : active) if (hit(id)) return true;
        return false;
    }
    // AND across groups, OR within a group. An id no group knows constrains nothing.
    for (const TagGroup& g : groups) {
        bool has = false, ok = false;
        for (const TagGroup::Entry& e : g.entries) {
            if (!active.contains(e.id)) continue;
            has = true;
            if (hit(e.id)) { ok = true; break; }
        }
        if (has && !ok) return false;
    }
    return true;
}

QString FunnelFilter::selfTest()
{
    QVector<TagGroup> g(2);
    g[0].id = QStringLiteral("cat"); g[0].entries = {{QStringLiteral("cat.a"), QStringLiteral("A")}, {QStringLiteral("cat.b"), QStringLiteral("B")}};
    g[1].id = QStringLiteral("typ"); g[1].entries = {{QStringLiteral("typ.x"), QStringLiteral("X")}, {QStringLiteral("typ.y"), QStringLiteral("Y")}};
    const QStringList tags = {QStringLiteral("cat.a"), QStringLiteral("typ.y")};   // the row's own tags
    auto hit = [&](const QString& id) { return tags.contains(id); };
    if (!passes(g, {}, false, hit)) return QStringLiteral("Funnel: empty selection must pass");
    if (!passes(g, {QStringLiteral("cat.a")}, false, hit)) return QStringLiteral("Funnel: one matching tick");
    if (passes(g, {QStringLiteral("cat.b")}, false, hit)) return QStringLiteral("Funnel: one non-matching tick");
    if (!passes(g, {QStringLiteral("cat.a"), QStringLiteral("cat.b")}, false, hit)) return QStringLiteral("Funnel: OR within a group");
    if (passes(g, {QStringLiteral("cat.a"), QStringLiteral("typ.x")}, false, hit)) return QStringLiteral("Funnel: AND across groups");
    if (!passes(g, {QStringLiteral("cat.a"), QStringLiteral("typ.x")}, true, hit)) return QStringLiteral("Funnel: matchAny ORs the whole set");
    if (passes(g, {QStringLiteral("cat.b"), QStringLiteral("typ.x")}, true, hit)) return QStringLiteral("Funnel: matchAny with no hit");
    if (!passes(g, {QStringLiteral("nope")}, false, hit)) return QStringLiteral("Funnel: an unknown id constrains nothing");
    return QString();
}

}  // namespace core
