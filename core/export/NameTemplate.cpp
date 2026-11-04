#include "core/export/NameTemplate.h"

#include <QRegularExpression>
#include <QStringList>

namespace core::name {
namespace {

// The reserved DOS device names. A file or folder with one of these stems cannot be created on
// Windows whatever its extension, and the failure surfaces as a bare "cannot write" with no clue.
const QStringList& reservedNames()
{
    static const QStringList kReserved = {
        QStringLiteral("CON"),  QStringLiteral("PRN"),  QStringLiteral("AUX"),  QStringLiteral("NUL"),
        QStringLiteral("COM1"), QStringLiteral("COM2"), QStringLiteral("COM3"), QStringLiteral("COM4"),
        QStringLiteral("COM5"), QStringLiteral("COM6"), QStringLiteral("COM7"), QStringLiteral("COM8"),
        QStringLiteral("COM9"), QStringLiteral("LPT1"), QStringLiteral("LPT2"), QStringLiteral("LPT3"),
        QStringLiteral("LPT4"), QStringLiteral("LPT5"), QStringLiteral("LPT6"), QStringLiteral("LPT7"),
        QStringLiteral("LPT8"), QStringLiteral("LPT9")};
    return kReserved;
}

}  // namespace

QString sanitizeStem(QString stem)
{
    static const QRegularExpression bad(QStringLiteral("[\\\\/:*?\"<>|]"));
    stem.replace(bad, QStringLiteral("_"));
    // Control characters survive a path-separator filter and make a path unusable just as surely.
    stem.remove(QRegularExpression(QStringLiteral("[\\x00-\\x1f]")));
    stem = stem.simplified();                                    // collapse doubles from empty fields
    while (stem.endsWith(QLatin1Char('.')) || stem.endsWith(QLatin1Char(' '))) stem.chop(1);
    // The device rule applies to the stem before the extension, so compare against the leading
    // segment: "CON" and "CON.glb" are both refused by Windows.
    const QString head = stem.section(QLatin1Char('.'), 0, 0);
    if (reservedNames().contains(head, Qt::CaseInsensitive)) stem.prepend(QLatin1Char('_'));
    return stem;
}

QString apply(const QString& tpl, const Fields& f)
{
    QString out = tpl;
    out.replace(QLatin1String("{{FileName}}"), f.fileName);
    out.replace(QLatin1String("{{Id}}"), QString::number(f.id));
    out.replace(QLatin1String("{{SNO}}"), QString::number(f.id));   // D4/POE2 templates already saved
    out.replace(QLatin1String("{{Collection}}"), f.collection);
    out.replace(QLatin1String("{{FrameIdx}}"), f.frameIndex >= 0 ? QString::number(f.frameIndex) : QString());
    out.replace(QLatin1String("{{FrameName}}"), f.frameName);
    out = sanitizeStem(out);
    if (!out.isEmpty()) return out;
    const QString fallback = sanitizeStem(f.fileName);
    return fallback.isEmpty() ? QStringLiteral("asset_%1").arg(f.id) : fallback;
}

QString selfTest()
{
    const auto bad = [](const QString& m) { return QStringLiteral("NameTemplate self-test: ") + m; };
    Fields f;
    f.fileName = QStringLiteral("barM_sets53_hlm");
    f.id = 1234567;

    if (apply(QStringLiteral("{{FileName}}"), f) != f.fileName)
        return bad(QStringLiteral("the plain default must pass the name through"));
    if (apply(QStringLiteral("{{FileName}} [{{SNO}}]"), f) != QStringLiteral("barM_sets53_hlm [1234567]"))
        return bad(QStringLiteral("{{SNO}} must keep working — D4 and POE2 users have it saved"));
    if (apply(QStringLiteral("{{Id}}"), f) != QStringLiteral("1234567"))
        return bad(QStringLiteral("{{Id}} is the portable spelling of the same field"));

    // An unfilled placeholder must not leave a double space behind.
    if (apply(QStringLiteral("{{FileName}} {{FrameName}} end"), f) != QStringLiteral("barM_sets53_hlm end"))
        return bad(QStringLiteral("empty fields should collapse, not leave gaps"));

    // Never empty, whatever the template says.
    if (apply(QStringLiteral("{{FrameName}}"), f) != f.fileName)
        return bad(QStringLiteral("a template that resolves to nothing must fall back to the name"));
    Fields nameless; nameless.id = 42;
    if (apply(QStringLiteral(""), nameless) != QStringLiteral("asset_42"))
        return bad(QStringLiteral("a nameless asset must still get a file name"));

    // Path separators can never survive: a name carrying one silently writes outside the folder
    // the user chose.
    if (sanitizeStem(QStringLiteral("a/b\\c:d*e?f\"g<h>i|j")).contains(QLatin1Char('/'))
        || sanitizeStem(QStringLiteral("a/b")).contains(QLatin1Char('\\')))
        return bad(QStringLiteral("a path separator survived sanitising"));

    // Windows: trailing dots and the device names.
    if (sanitizeStem(QStringLiteral("model.")) != QStringLiteral("model"))
        return bad(QStringLiteral("a trailing dot makes mkpath fail"));
    if (sanitizeStem(QStringLiteral("CON")) != QStringLiteral("_CON"))
        return bad(QStringLiteral("reserved device name not escaped"));
    if (sanitizeStem(QStringLiteral("con.glb")) != QStringLiteral("_con.glb"))
        return bad(QStringLiteral("a reserved name with an extension is still reserved"));
    if (sanitizeStem(QStringLiteral("console")) != QStringLiteral("console"))
        return bad(QStringLiteral("only the exact device names are reserved, not names starting with one"));
    return QString();
}

}  // namespace core::name
