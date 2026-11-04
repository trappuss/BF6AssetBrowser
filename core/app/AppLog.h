#pragma once
// The session log. FOX's structure (one message handler, a bounded in-memory tail, a Notifier the
// console listens to, the crash note fed from the same lines) plus D4's LIVE file toggle
// (Settings ▸ Interface ▸ Diagnostics writes log/autoFile and the file opens or closes at once —
// "the reason to want the file is that something is misbehaving RIGHT NOW"). POE2 had this header
// and no implementation at all (app-shell.md §1.2).
//
// The line format is a contract: "HH:mm:ss.zzz LVL   message". LogConsole reads the level at
// column 13, the crash note copies the line verbatim, Help ▸ Copy log pastes the tail.
#include <QObject>
#include <QString>
#include <QStringList>

namespace core::log {

class Notifier : public QObject {
    Q_OBJECT
public:
    static Notifier& instance();
Q_SIGNALS:
    void line(int level, const QString& text);
private:
    Notifier() = default;
};

// Install the message handler. `productName` is the log file's stem (data/<product>.log);
// `fileDefault` is the log/autoFile default (D4 ships true: "the file costs nothing until
// something goes wrong, and the one time it matters is the run nobody thought to enable it for").
void install(const QString& productName, bool fileDefault = true);

// The live toggle. Persists log/autoFile; opens (truncating) or closes the file at once.
void setFileLogging(bool on);
bool fileLogging();

QString filePath();          // absolute, whether or not the file is open
QStringList tail();          // the last kTailMax lines
constexpr int kTailMax = 4000;

// Harness: mirror to stderr (on by default; a GUI build on Windows discards it anyway).
void setEchoStderr(bool on);

}  // namespace core::log
