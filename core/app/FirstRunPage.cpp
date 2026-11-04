#include "core/app/FirstRunPage.h"
#include "core/app/AppPlugin.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

namespace core {

FirstRunPage::FirstRunPage(AppPlugin& plugin, QWidget* parent) : QWidget(parent), m_plugin(plugin)
{
    auto* outer = new QVBoxLayout(this);
    outer->addStretch(1);
    auto* row = new QHBoxLayout();
    row->addStretch(1);
    auto* card = new QWidget(this);
    card->setMaximumWidth(720);
    auto* v = new QVBoxLayout(card);
    v->setSpacing(10);

    auto* title = new QLabel(QStringLiteral("Point %1 at your game").arg(m_plugin.productName()), card);
    QFont tf = title->font();
    tf.setPointSizeF(tf.pointSizeF() * 1.6);
    tf.setBold(true);
    title->setFont(tf);
    v->addWidget(title);

    auto* body = new QLabel(card);
    body->setWordWrap(true);
    body->setTextFormat(Qt::RichText);
    body->setText(m_plugin.firstRunText().isEmpty()
        ? QStringLiteral("Choose the game's install folder. Nothing in it is ever written to: everything this tool keeps "
                         "lives in <code>data\\</code> beside the executable.")
        : m_plugin.firstRunText());
    v->addWidget(body);

    auto* btns = new QHBoxLayout();
    auto* set = new QPushButton(QStringLiteral("Set game folder…"), card);
    m_start = new QPushButton(QStringLiteral("Start"), card);
    m_start->setDefault(true);
    btns->addWidget(set);
    btns->addStretch(1);
    btns->addWidget(m_start);
    v->addLayout(btns);

    m_hint = new QLabel(card);
    m_hint->setWordWrap(true);
    v->addWidget(m_hint);

    row->addWidget(card, 0);
    row->addStretch(1);
    outer->addLayout(row);
    outer->addStretch(2);

    connect(set, &QPushButton::clicked, this, [this] { if (m_plugin.configureFolder(this)) refresh(); });
    connect(m_start, &QPushButton::clicked, this, [this] { if (m_plugin.isConfigured()) emit startRequested(); });
    refresh();
}

void FirstRunPage::refresh()
{
    const bool ok = m_plugin.isConfigured();
    m_start->setEnabled(ok);
    m_hint->setText(ok ? QStringLiteral("Folder set. Start reads the game data — a while the first time on a full install, "
                                        "seconds after that from the cache.")
                       : QStringLiteral("No folder yet. Set one to start."));
}

}  // namespace core
