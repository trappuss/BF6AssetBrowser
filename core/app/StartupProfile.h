// StartupProfile.h (FOX, lifted) — where the time between launching and a usable window
// went, kept so a person can SEE it (Help ▸ Startup profile) instead of
// diffing log timestamps (sweep item 48).
//
// The index already logs its phases ("dictionaries 532 ms, archive cache 0
// ms, discovery walk 13 ms, entry tables 0 ms, entries 1 ms"), the self-tests
// run at startup, and the catalogues build on first ready. None of that was
// anywhere a user could look, and "startup is slow" is not a report anyone
// can act on. This is the one place every startup cost is recorded, in the
// order it happened, each as a duration and a running total — the same
// numbers the log carries, gathered.
//
// ONE clock, from as early in main() as a timer can start. mark() records the
// gap since the previous mark; add() records a phase whose own duration is
// already known (the index hands over its sub-phases this way, so the profile
// and the log cannot disagree). Everything is process-wide and append-only;
// reading it is a table.
//
// A ROLLING HISTORY too (batch 11k): the last five launches are kept in
// data\cache\startup_history.tsv — one line each: when, the total, and every
// phase's ms — and the report prints them under the current one, so "is it
// slower than yesterday" is a glance rather than two saved files. Harness
// runs neither write it nor read it: a headless launch against a test tree
// is not one of the user's launches.
#pragma once
#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QString>
#include <QStringList>
#include <QTextStream>
#include <QVector>

namespace core {

class StartupProfile {
public:
    struct Phase {
        QString label;
        qint64 ms = 0;          // this phase's own duration
        qint64 sinceStart = 0;  // running total at the end of this phase
        int depth = 0;          // 1 = a sub-phase of the entry above it
        QString note;           // whatever the phase counted
    };

    static StartupProfile& instance()
    {
        static StartupProfile p;
        return p;
    }

    // Call once, as early in main() as possible. The wall-clock time is kept
    // for the report's header ("started 14:02:09").
    void begin()
    {
        m_clock.start();
        m_lastMs = 0;
        m_started = QDateTime::currentDateTime();
        m_phases.clear();
    }
    bool started() const { return m_started.isValid(); }
    QDateTime startedAt() const { return m_started; }
    qint64 elapsed() const { return m_clock.isValid() ? m_clock.elapsed() : 0; }

    // A milestone: the time since the previous mark (or begin()) is this
    // phase's duration.
    void mark(const QString& label, const QString& note = QString())
    {
        if (!m_clock.isValid()) return;
        const qint64 now = m_clock.elapsed();
        m_phases.push_back({label, now - m_lastMs, now, 0, note});
        m_lastMs = now;
    }

    // A phase whose duration is already known — the index's own sub-phases,
    // which it timed itself. `depth` 1 indents it under the mark above.
    // `sinceStart` is left as the clock reads now, so the running total column
    // stays monotone even for a borrowed duration.
    void add(const QString& label, qint64 ms, int depth = 0,
             const QString& note = QString())
    {
        const qint64 now = m_clock.isValid() ? m_clock.elapsed() : m_lastMs;
        m_phases.push_back({label, ms, now, depth, note});
        if (depth == 0) m_lastMs = now;
    }

    const QVector<Phase>& phases() const { return m_phases; }
    qint64 total() const { return m_phases.isEmpty() ? 0 : m_phases.back().sinceStart; }

    // How many launches the history keeps.
    static constexpr int kHistory = 5;
    // Where the history lives; empty (the default) means no history is read
    // or written — the harness leaves it empty.
    void setHistoryPath(const QString& path) { m_historyPath = path; }
    QString historyPath() const { return m_historyPath; }

    // One launch, one line: started<TAB>totalMs<TAB>label=ms;label=ms;…
    // (top-level phases only — the sub-phases are the index's own breakdown
    // and the index line carries their sum).
    QString historyLine() const
    {
        QStringList ph;
        for (const Phase& p : m_phases)
            if (p.depth == 0)
                ph << QStringLiteral("%1=%2").arg(p.label).arg(p.ms);
        return QStringLiteral("%1\t%2\t%3")
            .arg(m_started.toString(Qt::ISODate))
            .arg(total())
            .arg(ph.join(QLatin1Char(';')));
    }

    // Append this launch and trim the file to the last kHistory. Returns
    // false when no path is set or the file cannot be written.
    bool record()
    {
        if (m_historyPath.isEmpty() || m_phases.isEmpty()) return false;
        QStringList lines = history();
        lines.append(historyLine());
        while (lines.size() > kHistory) lines.removeFirst();
        QFile f(m_historyPath);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) return false;
        QTextStream ts(&f);
        for (const QString& l : lines) ts << l << '\n';
        return true;
    }

    // The launches on file, oldest first, as their raw lines.
    QStringList history() const
    {
        QStringList out;
        if (m_historyPath.isEmpty()) return out;
        QFile f(m_historyPath);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return out;
        QTextStream ts(&f);
        while (!ts.atEnd()) {
            const QString l = ts.readLine().trimmed();
            if (!l.isEmpty() && l.count(QLatin1Char('\t')) == 2) out << l;
        }
        return out;
    }

    // The report, as plain text — the dialog and the --startupprofile dump
    // both render this, so they say the same thing.
    QString text() const
    {
        QString out;
        out += QStringLiteral("Startup profile");
        if (m_started.isValid())
            out += QStringLiteral(" — started %1")
                       .arg(m_started.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")));
        out += QStringLiteral("\n\n%1  %2  %3\n")
                   .arg(QStringLiteral("phase"), -34)
                   .arg(QStringLiteral("ms"), 8)
                   .arg(QStringLiteral("total"));
        for (const Phase& p : m_phases) {
            const QString name = QString(p.depth * 2, QLatin1Char(' ')) + p.label;
            // The running total is meaningful only for top-level milestones;
            // a sub-phase's own ms is the number that matters, and its
            // "total" would just repeat its parent's, so it is left blank.
            const QString total = p.depth == 0
                ? QStringLiteral("%1").arg(p.sinceStart, 6)
                : QStringLiteral("%1").arg(QString(), 6);
            out += QStringLiteral("%1  %2  %3%4\n")
                       .arg(name, -34)
                       .arg(p.ms, 8)
                       .arg(total)
                       .arg(p.note.isEmpty() ? QString()
                                             : QStringLiteral("   %1").arg(p.note));
        }
        out += QStringLiteral("\n%1 ms to a usable window.\n").arg(total());

        // The previous launches, newest first — the ones on file other than
        // this one (record() runs before the report is shown, so the file's
        // last line IS this launch and is skipped by its timestamp).
        const QStringList past = history();
        QStringList rows;
        const QString me = m_started.toString(Qt::ISODate);
        for (int i = past.size() - 1; i >= 0; --i) {
            const QStringList f = past[i].split(QLatin1Char('\t'));
            if (f.size() != 3 || f[0] == me) continue;
            // The index phase is the one that moves between launches; show
            // it beside the total so the comparison is one line.
            QString index;
            for (const QString& kv : f[2].split(QLatin1Char(';')))
                if (kv.startsWith(QLatin1String("index built=")))
                    index = kv.section(QLatin1Char('='), 1);
            rows << QStringLiteral("%1  %2 ms total%3")
                        .arg(QDateTime::fromString(f[0], Qt::ISODate)
                                 .toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")))
                        .arg(f[1], 6)
                        .arg(index.isEmpty() ? QString()
                                             : QStringLiteral("  (index %1 ms)").arg(index));
        }
        if (!rows.isEmpty())
            out += QStringLiteral("\nPrevious launches (last %1 kept):\n  %2\n")
                       .arg(kHistory)
                       .arg(rows.join(QStringLiteral("\n  ")));
        return out;
    }

private:
    QElapsedTimer m_clock;
    qint64 m_lastMs = 0;
    QDateTime m_started;
    QVector<Phase> m_phases;
    QString m_historyPath;
};

}  // namespace core
