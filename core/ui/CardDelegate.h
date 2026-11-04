#pragma once
// A card renderer for the asset grid (QListView in IconMode). Replaces QListView's default
// icon+wrapped-label item — which reads as "a mess, hard to select, not cards" — with a rounded
// tile: the thumbnail framed on top, a name caption below, an obvious hover and selection state, and
// a quiet placeholder while the GL thumbnail is still rendering on its thread.
//
// It reads the AssetListModel roles only, so any tab that lists an AssetListModel in a grid can set
// it: DecorationRole (the thumbnail QIcon, null while rendering), DisplayRole (the short name, already
// carrying the ⚠/🔒 prefixes), the ColInGameName sibling (the real Operator — Skin name when known),
// and ForegroundRole (dim grey for unrenderable/encrypted rows).
#include <QStyledItemDelegate>

namespace core {

class CardDelegate : public QStyledItemDelegate {
    Q_OBJECT
public:
    explicit CardDelegate(QObject* parent = nullptr) : QStyledItemDelegate(parent) {}

    // The cell for an icon of side `px` (= the view's iconSize): the image square + a two-line caption.
    // Kept as one formula so the delegate and the view's gridSize cannot disagree.
    static QSize cellFor(int px) { return QSize(px + 20, px + 64); }

    QSize sizeHint(const QStyleOptionViewItem& opt, const QModelIndex& idx) const override;
    void  paint(QPainter* p, const QStyleOptionViewItem& opt, const QModelIndex& idx) const override;
};

}  // namespace core
