#include "core/app/ExportNotifier.h"

#include <QDesktopServices>
#include <QDir>
#include <QProcess>
#include <QSettings>
#include <QUrl>

namespace core {
namespace {
bool g_sessionAfter = false;
QString g_sessionMode, g_sessionCommand;
const char* const kModeKey = "export/after";
const char* const kCmdKey = "export/afterCommand";

QString normalizeMode(const QString& m)
{
    return m == QLatin1String("reveal") || m == QLatin1String("command") ? m : QStringLiteral("none");
}
}  // namespace

QString ExportNotifier::afterMode()
{
    if (g_sessionAfter) return g_sessionMode;
    return normalizeMode(QSettings().value(QLatin1String(kModeKey), QStringLiteral("none")).toString());
}

QString ExportNotifier::afterCommand()
{
    if (g_sessionAfter) return g_sessionCommand;
    return QSettings().value(QLatin1String(kCmdKey)).toString();
}

void ExportNotifier::setAfter(const QString& mode, const QString& command)
{
    QSettings s;
    s.setValue(QLatin1String(kModeKey), normalizeMode(mode));
    s.setValue(QLatin1String(kCmdKey), command);
}

void ExportNotifier::setSessionAfter(const QString& mode, const QString& command)
{
    g_sessionAfter = true;
    g_sessionMode = normalizeMode(mode);
    g_sessionCommand = command;
}

QString ExportNotifier::runAfter(const QString& folder, const QString& file)
{
    const QString mode = afterMode();
    if (mode == QLatin1String("reveal")) {
        if (folder.isEmpty()) return QStringLiteral("after: nothing to reveal");
        QDesktopServices::openUrl(QUrl::fromLocalFile(folder));
        return QStringLiteral("after: revealed %1").arg(QDir::toNativeSeparators(folder));
    }
    if (mode == QLatin1String("command")) {
        QString cmd = afterCommand().trimmed();
        if (cmd.isEmpty()) return QStringLiteral("after: no command configured");
        // Quoted: a path with a space is the ordinary case on Windows.
        cmd.replace(QLatin1String("{{File}}"), QLatin1Char('"') + QDir::toNativeSeparators(file) + QLatin1Char('"'));
        cmd.replace(QLatin1String("{{Folder}}"), QLatin1Char('"') + QDir::toNativeSeparators(folder) + QLatin1Char('"'));
        QStringList args = QProcess::splitCommand(cmd);
        if (args.isEmpty()) return QStringLiteral("after: empty command");
        const QString program = args.takeFirst();
        const bool ok = QProcess::startDetached(program, args, folder.isEmpty() ? QDir::currentPath() : folder);
        return QStringLiteral("after: %1 %2").arg(ok ? QStringLiteral("ran") : QStringLiteral("COULD NOT START"), cmd);
    }
    return QString();
}

}  // namespace core
