#include "games/bf6/Bf6Names.h"
#include "core/app/AppPaths.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace bf6game {

void Bf6Names::load()
{
    m_codes.clear();
    // 1) the shipped seed (canonical, version-controlled).
    { QFile f(QStringLiteral(":/bf6/bf6_names.json"));
      if (f.open(QIODevice::ReadOnly)) mergeJson(f.readAll()); }
    // 2) the optional user override beside the exe, overlaid on top (add IDs without a rebuild).
    { QFile f(core::paths::file(QStringLiteral("bf6_names.json")));
      if (f.exists() && f.open(QIODevice::ReadOnly)) mergeJson(f.readAll()); }
    m_loaded = true;
}

void Bf6Names::mergeJson(const QByteArray& bytes)
{
    QJsonParseError pe{};
    const QJsonDocument doc = QJsonDocument::fromJson(bytes, &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject()) return;
    const QJsonObject root = doc.object();
    const QJsonObject codes = root.value(QStringLiteral("codenames")).toObject();
    for (auto it = codes.constBegin(); it != codes.constEnd(); ++it) {
        const QJsonObject c = it.value().toObject();
        Code& code = m_codes[it.key()];   // merge into any existing (override wins field-by-field)
        if (c.contains(QStringLiteral("operator"))) code.op    = c.value(QStringLiteral("operator")).toString();
        if (c.contains(QStringLiteral("squad")))    code.squad = c.value(QStringLiteral("squad")).toString();
        if (c.contains(QStringLiteral("skin")))     code.skin  = c.value(QStringLiteral("skin")).toString();
        const QJsonObject sets = c.value(QStringLiteral("sets")).toObject();
        for (auto sit = sets.constBegin(); sit != sets.constEnd(); ++sit) {
            const QJsonObject s = sit.value().toObject();
            Entry& e = code.sets[sit.key()];
            if (s.contains(QStringLiteral("operator"))) e.op    = s.value(QStringLiteral("operator")).toString();
            if (s.contains(QStringLiteral("squad")))    e.squad = s.value(QStringLiteral("squad")).toString();
            if (s.contains(QStringLiteral("skin")))     e.skin  = s.value(QStringLiteral("skin")).toString();
            if (s.contains(QStringLiteral("desc")))     e.desc  = s.value(QStringLiteral("desc")).toString();
            e.named = true;
        }
    }
}

int Bf6Names::skinCount() const
{
    int n = 0;
    for (const Code& c : m_codes) n += c.sets.size();
    return n;
}

Bf6Names::Entry Bf6Names::resolve(const QString& charId, const QString& set) const
{
    Entry e;
    const auto it = m_codes.constFind(charId);
    if (it == m_codes.constEnd()) return e;      // unknown codename → fail closed
    const Code& c = *it;
    e.op = c.op; e.squad = c.squad; e.skin = c.skin;   // codename-level defaults
    const auto sit = c.sets.constFind(set);
    if (sit != c.sets.constEnd()) {
        const Entry& s = *sit;
        if (!s.op.isEmpty())    e.op    = s.op;     // a set can host a different operator
        if (!s.squad.isEmpty()) e.squad = s.squad;
        if (!s.skin.isEmpty())  e.skin  = s.skin;   // per-set skin overrides the codename default
        e.desc = s.desc;
    }
    e.named = !e.op.isEmpty();
    return e;
}

QString Bf6Names::displayFor(const QString& charId, const QString& set, const QString& part) const
{
    const Entry e = resolve(charId, set);
    if (!e.named) return {};
    QString s = e.op;
    if (!e.skin.isEmpty()) s += QStringLiteral(" — ") + e.skin;
    if (!part.isEmpty())   s += QStringLiteral(" · ") + part;
    return s;
}

}  // namespace bf6game
