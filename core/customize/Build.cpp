#include "core/customize/Build.h"

#include <QStringList>

namespace core::customize {

QString Build::toText() const
{
    QStringList lines;
    if (!category.isEmpty()) lines << QStringLiteral("category=") + category;
    if (!subject.isEmpty()) lines << QStringLiteral("subject=") + subject;
    if (!variant.isEmpty()) lines << QStringLiteral("variant=") + variant;
    for (auto it = picks.constBegin(); it != picks.constEnd(); ++it)
        if (!it.value().isEmpty()) lines << QStringLiteral("slot.") + it.key() + QLatin1Char('=') + it.value();
    for (auto it = toggles.constBegin(); it != toggles.constEnd(); ++it)
        lines << QStringLiteral("toggle.") + it.key() + QLatin1Char('=') + it.value();
    lines.sort();
    return lines.join(QLatin1Char('\n'));
}

bool Build::fromText(const QString& text, Build* out, QStringList* unknown)
{
    Build b;
    int parsed = 0;
    QString norm = text;
    norm.replace(QLatin1Char(';'), QLatin1Char('\n'));
    norm.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    for (const QString& raw : norm.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
        const QString line = raw.trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#'))) continue;
        const int eq = line.indexOf(QLatin1Char('='));
        if (eq <= 0) { if (unknown) *unknown << line; continue; }
        const QString key = line.left(eq).trimmed(), val = line.mid(eq + 1).trimmed();
        if (key == QLatin1String("subject")) { b.subject = val; ++parsed; }
        else if (key == QLatin1String("category")) { b.category = val; ++parsed; }
        else if (key == QLatin1String("variant")) { b.variant = val; ++parsed; }
        else if (key.startsWith(QLatin1String("slot.")) && key.size() > 5) { if (!val.isEmpty()) b.picks.insert(key.mid(5), val); ++parsed; }
        else if (key.startsWith(QLatin1String("toggle.")) && key.size() > 7) { b.toggles.insert(key.mid(7), val == QLatin1String("1") || val.compare(QLatin1String("true"), Qt::CaseInsensitive) == 0 ? QStringLiteral("1") : QStringLiteral("0")); ++parsed; }
        else if (unknown) *unknown << line;
    }
    if (out) *out = b;
    return parsed > 0;
}

QString buildSelfTest()
{
    Build a;
    a.subject = QStringLiteral("Warrior");
    a.picks.insert(QStringLiteral("body"), QStringLiteral("art/a b/x.smd"));
    a.picks.insert(QStringLiteral("helmet"), QString());            // empty = none: not written
    a.toggles.insert(QStringLiteral("hair"), QStringLiteral("0"));
    const QString t = a.toText();
    if (t.contains(QLatin1String("helmet"))) return QStringLiteral("Build: an empty slot was written");
    Build b;
    if (!Build::fromText(t, &b)) return QStringLiteral("Build: its own text did not parse");
    a.picks.remove(QStringLiteral("helmet"));
    if (a != b) return QStringLiteral("Build: text round trip changed the build");
    Build c;
    QStringList unk;
    if (!Build::fromText(QStringLiteral("subject=Warrior; slot.body=art/a b/x.smd ;toggle.hair=false;bogus"), &c, &unk) || c != b || unk.size() != 1)
        return QStringLiteral("Build: ';' separators, spacing, false-as-0 or unknown-key reporting failed");
    if (Build::fromText(QStringLiteral("nonsense"), &c)) return QStringLiteral("Build: text with no key parsed");
    Build d = a;
    d.category = QStringLiteral("character"); d.variant = QStringLiteral("art/v/base.fmdl");
    Build e;
    if (!Build::fromText(d.toText(), &e) || e != d) return QStringLiteral("Build: category / variant did not round-trip");
    if (e == a) return QStringLiteral("Build: category / variant ignored by ==");
    return {};
}

}  // namespace core::customize
