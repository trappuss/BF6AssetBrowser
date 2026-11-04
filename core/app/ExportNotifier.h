#pragma once
// One place every export path reports itself (template §15). POE2's core-sized class (no
// game-typed glbOptionsLine — a tab that knows what it wrote appends its own line) plus FOX's two
// additions: the `file` argument and the after-export action (none / reveal / run a command with
// {{File}} and {{Folder}} filled in), with a session override for the harness.
#include <QObject>
#include <QString>

namespace core {

class ExportNotifier : public QObject {
    Q_OBJECT
public:
    static ExportNotifier& instance() { static ExportNotifier n; return n; }

    // `folder` is the directory to reveal (empty = none); `file` the one file written, when there is one.
    void notify(const QString& text, const QString& folder = QString(), const QString& file = QString())
    {
        emit exported(text, folder, file);
    }

    static QString afterMode();                   // "none" | "reveal" | "command"
    static QString afterCommand();
    static void setAfter(const QString& mode, const QString& command);
    static void setSessionAfter(const QString& mode, const QString& command);
    static QString runAfter(const QString& folder, const QString& file);   // what it did, for the log

Q_SIGNALS:
    void exported(const QString& text, const QString& folder, const QString& file);

private:
    ExportNotifier() = default;
};

}  // namespace core
