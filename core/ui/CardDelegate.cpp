#include "core/ui/CardDelegate.h"
#include "core/ui/AssetListModel.h"

#include <QApplication>
#include <QIcon>
#include <QPainter>
#include <QPainterPath>

namespace core {

QSize CardDelegate::sizeHint(const QStyleOptionViewItem& opt, const QModelIndex&) const
{
    const int px = opt.decorationSize.width() > 0 ? opt.decorationSize.width() : 160;
    return cellFor(px);
}

void CardDelegate::paint(QPainter* p, const QStyleOptionViewItem& opt, const QModelIndex& idx) const
{
    p->save();
    p->setRenderHint(QPainter::Antialiasing, true);
    p->setRenderHint(QPainter::SmoothPixmapTransform, true);

    const QPalette& pal = opt.palette;
    const bool sel   = opt.state & QStyle::State_Selected;
    const bool hover = opt.state & QStyle::State_MouseOver;

    // ── Card background + border ────────────────────────────────────────────────────────────────
    const QRectF card = QRectF(opt.rect).adjusted(2.5, 2.5, -2.5, -2.5);
    const qreal radius = 9.0;
    QColor fill = pal.color(QPalette::Base);
    // Keep the card a touch lighter than the view backdrop so tiles read as cards, not a flat sheet.
    fill = fill.lighter(hover || sel ? 128 : 112);
    QColor border = pal.color(QPalette::Mid);
    if (sel) {
        QColor hl = pal.color(QPalette::Highlight);
        QColor tint = hl; tint.setAlpha(60);
        fill = QColor(pal.color(QPalette::Base)); fill.setAlpha(255);
        QPainterPath fillPath; fillPath.addRoundedRect(card, radius, radius);
        p->fillPath(fillPath, pal.color(QPalette::Base).lighter(118));
        p->fillPath(fillPath, tint);
        border = hl;
    } else {
        QPainterPath fillPath; fillPath.addRoundedRect(card, radius, radius);
        p->fillPath(fillPath, fill);
        if (hover) border = pal.color(QPalette::Highlight);
    }
    QPainterPath borderPath; borderPath.addRoundedRect(card, radius, radius);
    p->setPen(QPen(border, sel ? 2.0 : (hover ? 1.5 : 1.0)));
    p->drawPath(borderPath);

    // ── Geometry: image square on top, two caption lines below ──────────────────────────────────
    const int pad = 8;
    const int imgSide = int(card.width()) - 2 * pad;
    const QRect imgRect(int(card.left()) + pad, int(card.top()) + pad, imgSide, imgSide);
    const QRect capRect(int(card.left()) + 6, imgRect.bottom() + 5,
                        int(card.width()) - 12, int(card.bottom()) - (imgRect.bottom() + 5) - 5);

    // ── Thumbnail (or a quiet placeholder while the GL render is in flight) ──────────────────────
    const QVariant dv = idx.data(Qt::DecorationRole);
    const QIcon ic = dv.canConvert<QIcon>() ? qvariant_cast<QIcon>(dv) : QIcon();
    QPainterPath imgClip; imgClip.addRoundedRect(QRectF(imgRect), 6, 6);
    if (!ic.isNull()) {
        const qreal dpr = opt.widget ? opt.widget->devicePixelRatioF() : 1.0;
        QPixmap pm = ic.pixmap(imgRect.size() * dpr);
        if (!pm.isNull()) {
            pm.setDevicePixelRatio(dpr);
            QSizeF ps = QSizeF(pm.size()) / dpr; ps.scale(imgRect.size(), Qt::KeepAspectRatio);
            QRectF pr(QPointF(0, 0), ps); pr.moveCenter(QRectF(imgRect).center());
            p->save(); p->setClipPath(imgClip); p->drawPixmap(pr.toRect(), pm); p->restore();
        }
    } else {
        p->fillPath(imgClip, pal.color(QPalette::AlternateBase));
        p->setPen(pal.color(QPalette::Mid));
        QFont f = opt.font; f.setPointSizeF(f.pointSizeF() * 1.6); p->setFont(f);
        p->drawText(imgRect, Qt::AlignCenter, QStringLiteral("…"));
        p->setFont(opt.font);
    }

    // ── Caption: the real name (Operator — Skin) when known, else the short/codename ─────────────
    const QString shortName = idx.data(Qt::DisplayRole).toString();
    const QString ingame = idx.siblingAtColumn(AssetListModel::ColInGameName).data(Qt::DisplayRole).toString();
    const QString primary = !ingame.isEmpty() ? ingame : shortName;
    const QString secondary = !ingame.isEmpty() ? shortName : QString();

    QColor fg = sel ? pal.color(QPalette::HighlightedText) : pal.color(QPalette::Text);
    if (!sel) { const QVariant fgv = idx.data(Qt::ForegroundRole); if (fgv.canConvert<QColor>()) fg = fgv.value<QColor>(); }

    const QFontMetrics fm(opt.font);
    QFont pf = opt.font; pf.setBold(true);
    const QFontMetrics pfm(pf);
    const int line1 = pfm.height();
    QRect r1(capRect.left(), capRect.top(), capRect.width(), line1);
    p->setFont(pf); p->setPen(fg);
    p->drawText(r1, Qt::AlignHCenter | Qt::AlignVCenter,
                pfm.elidedText(primary, Qt::ElideRight, capRect.width()));
    if (!secondary.isEmpty() && capRect.height() >= line1 + fm.height() - 2) {
        QRect r2(capRect.left(), r1.bottom() + 1, capRect.width(), fm.height());
        QColor dim = fg; dim.setAlpha(150);
        p->setFont(opt.font); p->setPen(dim);
        p->drawText(r2, Qt::AlignHCenter | Qt::AlignVCenter,
                    fm.elidedText(secondary, Qt::ElideMiddle, capRect.width()));
    }

    p->restore();
}

}  // namespace core
