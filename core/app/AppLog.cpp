#include "core/app/AppLog.h"
#include "core/app/AppPaths.h"
#include "core/app/CrashHandler.h"

#include <QDateTime>
#include <QFile>
#include <QMutex>
#include <QMutexLocker>
#include <QSettings>
#include <QTextStream>
#include <cstdio>

namespace {

QFile g_logFile;
QMutex g_logMutex;
QStringList g_tail;
QString g_product = QStringLiteral("AssetBrowser");
bool g_echo = true;

void handler(QtMsgType type, const QMessageLogContext&, const QString& msg)
{
    QString line;
    {
        QMutexLocker lock(&g_logMutex);
        const char* lvl = "INFO";
        switch (type) {
        case QtDebugMsg:    lvl = "DBG "; break;
        case QtWarningMsg:  lvl = "WARN"; break;
        case QtCriticalMsg: lvl = "CRIT"; break;
        case QtFatalMsg:    lvl = "FATAL"; break;
        default: break;
        }
        line = QStringLiteral("%1 %2  %3")
                   .arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz")),
                        QLatin1String(lvl), msg);
        if (g_logFile.isOpen()) {
            QTextStream(&g_logFile) << line << '\n';
            g_logFile.flush();   // flush-per-line: a crash must not eat the tail
        }
        if (g_echo) std::fprintf(stderr, "%s\n", line.toUtf8().constData());
        g_tail.append(line);
        if (g_tail.size() > core::log::kTailMax) g_tail.remove(0, g_tail.size() - core::log::kTailMax);
    }
    // The crash note's own fixed-buffer copy of the tail — fed here so the note and the log can
    // never disagree about what the tool last said.
    core::crash::noteLogLine(line);
    // OUTSIDE the lock (FOX): a directly-connected receiver that logs would otherwise deadlock on
    // the non-recursive mutex. The FILE stays ordered; a console may see two racing lines swapped.
    emit core::log::Notifier::instance().line(int(type), line);
}

}  // namespace

namespace core::log {

Notifier& Notifier::instance()
{
    // DELIBERATELY LEAKED (FOX): a function-local static is destroyed at exit BEFORE the
    // namespace-scope file and tail, and the handler is never uninstalled — a qWarning from a
    // later static destructor would emit on a dead object.
    static Notifier* n = new Notifier;
    return *n;
}

QString filePath() { return paths::file(g_product + QStringLiteral(".log")); }

void install(const QString& productName, bool fileDefault)
{
    g_product = productName;
    qInstallMessageHandler(handler);
    setFileLogging(QSettings().value(QStringLiteral("log/autoFile"), fileDefault).toBool());
}

bool fileLogging()
{
    QMutexLocker lock(&g_logMutex);
    return g_logFile.isOpen();
}

void setFileLogging(bool on)
{
    QSettings().setValue(QStringLiteral("log/autoFile"), on);
    QMutexLocker lock(&g_logMutex);
    if (on) {
        if (g_logFile.isOpen()) return;
        g_logFile.setFileName(filePath());
        // Truncate: ONE file that is always the current session, never a growing history.
        g_logFile.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text);
    } else if (g_logFile.isOpen()) {
        g_logFile.close();
    }
}

QStringList tail()
{
    QMutexLocker lock(&g_logMutex);
    return g_tail;
}

void setEchoStderr(bool on) { g_echo = on; }

}  // namespace core::log
