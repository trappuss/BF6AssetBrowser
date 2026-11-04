#include "core/app/Application.h"
#include "core/app/AppLog.h"
#include "core/app/AppPlugin.h"
#include "core/app/AppPaths.h"
#include "core/app/AppShell.h"
#include "core/app/CrashHandler.h"
#include "core/app/Density.h"
#include "core/app/FontScale.h"
#include "core/app/SehGuard.h"
#include "core/app/SelfTests.h"
#include "core/app/StartupProfile.h"
#include "core/store/IGameStore.h"
#include "core/tabs/ModelLoad.h"
#include "core/ui/QueryTerm.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIcon>
#include <QSettings>
#include <QSize>
#include <QSurfaceFormat>
#include <QPixmap>
#include <QTimer>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

namespace core {

namespace {

// --dump-paths: each game path read through the store, written under `dir` mirroring the path.
void dumpGamePaths(AppPlugin& plugin, const QString& list, const QString& dirArg)
{
    IGameStore* store = plugin.store();
    const QString dir = dirArg.isEmpty() ? QDir::current().filePath(QStringLiteral("dump")) : dirArg;
    for (const QString& raw : list.split(QLatin1Char(';'), Qt::SkipEmptyParts)) {
        const QString gp = raw.trimmed();
        if (gp.isEmpty()) continue;
        QString err;
        const QByteArray bytes = store ? store->readPath(gp, &err) : QByteArray();
        if (bytes.isEmpty()) {
            qWarning("dump: FAILED %s — %s", qUtf8Printable(gp), qUtf8Printable(!store ? QStringLiteral("no store") : err.isEmpty() ? QStringLiteral("no bytes (not in the index, or the store has no readPath)") : err));
            continue;
        }
        const QString out = model::mirrorPath(dir, QString(gp).replace(QLatin1Char(':'), QLatin1Char('_')));
        QDir().mkpath(QFileInfo(out).absolutePath());
        QFile f(out);
        if (!f.open(QIODevice::WriteOnly) || f.write(bytes) != bytes.size()) { qWarning("dump: FAILED %s — could not write %s", qUtf8Printable(gp), qUtf8Printable(out)); continue; }
        f.close();
        // UTF-16LE text (a BOM, or zero high bytes in the first characters) gets a UTF-8 copy.
        const bool bom = bytes.size() >= 2 && uchar(bytes[0]) == 0xFF && uchar(bytes[1]) == 0xFE;
        const bool zeros = bytes.size() >= 8 && bytes[1] == 0 && bytes[3] == 0 && bytes[5] == 0 && bytes[7] == 0;
        QString note;
        if (bom || zeros) {
            const QByteArray body = bom ? bytes.mid(2) : bytes;
            const QString text = QString::fromUtf16(reinterpret_cast<const char16_t*>(body.constData()), body.size() / 2);
            QFile t(out + QStringLiteral(".utf8.txt"));
            if (t.open(QIODevice::WriteOnly)) { t.write(text.toUtf8()); note = QStringLiteral(" (+ .utf8.txt)"); }
        }
        // "wrote" is the verb a check's needle names: the failure lines above say FAILED, and the
        // first cut's needle ("dump: <path>") matched a failure line too (audit 2026-09-25).
        qInfo("dump: wrote %s — %lld bytes → %s%s", qUtf8Printable(gp), qint64(bytes.size()), qUtf8Printable(out), qUtf8Printable(note));
    }
}

}  // namespace

int runApp(int argc, char** argv, AppPlugin& plugin, const RunOptions& opt)
{
    StartupProfile::instance().begin();
    // Structured-exception translator on the GUI thread (template §2). Background threads install
    // it themselves through seh::runGuarded.
    seh::installSehTranslator();

    QSurfaceFormat fmt;
    fmt.setVersion(opt.glMajor, opt.glMinor);
    fmt.setProfile(QSurfaceFormat::CoreProfile);
    fmt.setDepthBufferSize(24);
    fmt.setStencilBufferSize(8);
    fmt.setAlphaBufferSize(8);
    fmt.setSamples(opt.samples);
    QSurfaceFormat::setDefaultFormat(fmt);
    QApplication::setAttribute(Qt::AA_ShareOpenGLContexts);

    // Our flags come out before Qt parses the rest.
    bool check = false, quitWhenReady = false, harness = false, stay = false, waitSecondary = false, exportMenu = false;
    QString showTab, shotPng, dumpPaths, dumpTo, crashTest, crashNote;
    int settleMs = 1500;
    int harnessTimeoutS = 1800;   // --harness-timeout: a harness run that never finishes must not hang its caller
    QSize harnessWindow(1920, 1080);   // --window WxH: the harness's fixed window (C13)
    QVector<char*> args;
    QStringList pluginArgs;   // what the plugin claims (AppPlugin::parseArgs), after our own flags
    for (int i = 0; i < argc; ++i) {
        const QString a = QString::fromLocal8Bit(argv[i]);
        if (a == QLatin1String("--check")) { check = true; continue; }
        if (a == QLatin1String("--quit-when-ready")) { quitWhenReady = true; harness = true; continue; }
        if (a == QLatin1String("--harness")) { harness = true; continue; }
        if ((a == QLatin1String("--show-tab") || a == QLatin1String("--tab")) && i + 1 < argc) { showTab = QString::fromLocal8Bit(argv[++i]); continue; }
        // The screenshot harness (FOX's DevShot mechanism: schedule → ready → settle → grab → quit).
        if (a == QLatin1String("--shot") && i + 1 < argc) { shotPng = QString::fromLocal8Bit(argv[++i]); harness = true; continue; }
        if (a == QLatin1String("--settle") && i + 1 < argc) { settleMs = QString::fromLocal8Bit(argv[++i]).toInt(); continue; }
        if (a == QLatin1String("--stay")) { stay = true; continue; }
        if (a == QLatin1String("--wait-secondary")) { waitSecondary = true; continue; }
        if (a == QLatin1String("--harness-timeout") && i + 1 < argc) { harnessTimeoutS = QString::fromLocal8Bit(argv[++i]).toInt(); continue; }
        if (a == QLatin1String("--window") && i + 1 < argc) {
            const QStringList wh = QString::fromLocal8Bit(argv[++i]).split(QLatin1Char('x'));
            if (wh.size() == 2) harnessWindow = QSize(wh[0].toInt(), wh[1].toInt());
            continue;
        }
        // Crash on purpose once the window is up (segv | abort | throw), writing the crash note to
        // --crash-note instead of the user's: the bat checks each handler leaves a note.
        if (a == QLatin1String("--crash-test") && i + 1 < argc) { crashTest = QString::fromLocal8Bit(argv[++i]); continue; }
        if (a == QLatin1String("--crash-note") && i + 1 < argc) { crashNote = QString::fromLocal8Bit(argv[++i]); continue; }
        if (a == QLatin1String("--export-menu")) { exportMenu = true; harness = true; continue; }
        // Raw game files out through the store (readPath), for format research: the bytes as the
        // game holds them, plus a UTF-8 .txt beside any that read as UTF-16 text. ';'-separated.
        if (a == QLatin1String("--dump-paths") && i + 1 < argc) { dumpPaths = QString::fromLocal8Bit(argv[++i]); harness = true; continue; }
        if (a == QLatin1String("--dump-to") && i + 1 < argc) { dumpTo = QString::fromLocal8Bit(argv[++i]); continue; }
        // Session overrides, never written to the INI (FOX's rule): a harness run measures the
        // density and text size it asked for, not what the player left set. apply() runs below.
        if (a == QLatin1String("--density") && i + 1 < argc) { density::setSession(QString::fromLocal8Bit(argv[++i])); continue; }
        if (a == QLatin1String("--fontscale") && i + 1 < argc) { fontscale::setSession(QString::fromLocal8Bit(argv[++i]).toInt()); continue; }
        if (i > 0) pluginArgs << a;
        args.append(argv[i]);
    }
    // A plugin takes its own flags out of what Qt would otherwise see (FOX's --games, --character…).
    const QStringList consumed = plugin.parseArgs(pluginArgs);
    if (!consumed.isEmpty()) {
        QVector<char*> kept{args[0]};
        for (int i = 1; i < args.size(); ++i) if (!consumed.contains(QString::fromLocal8Bit(args[i]))) kept.append(args[i]);
        args = kept;
    }
    int qargc = args.size();
    QApplication app(qargc, args.data());
    QApplication::setOrganizationName(opt.organization.isEmpty() ? plugin.productName() : opt.organization);
    QApplication::setApplicationName(plugin.productName());
    QApplication::setApplicationVersion(plugin.version());

    // Portable settings: data/<product>.ini beside the exe, never the registry.
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, paths::dataDir());

    log::install(plugin.productName(), opt.logFileDefault);
    crash::install(plugin.productName());
    qInfo("%s %s starting", qUtf8Printable(plugin.productName()), qUtf8Printable(plugin.version()));
    // What no flag took — not ours, not the plugin's, and not Qt's (QApplication removed its own from
    // the array above). Said, not dropped: run 15 (2026-09-25) passed "--show-tab a,b,c" unquoted
    // through cmd's call, which splits on commas, so only "a" was shown and b, c vanished silently.
    {
        QStringList stray;
        for (int i = 1; i < qargc; ++i) stray << QString::fromLocal8Bit(args[i]);
        if (!stray.isEmpty())
            qWarning("args: %d argument(s) no flag took, ignored: %s (a comma or ';' list must be ONE quoted argument)",
                     int(stray.size()), qUtf8Printable(stray.join(QStringLiteral(" | "))));
    }

    // Renamed keys: copy old → new once, then drop the old (D4's table mechanism).
    if (!opt.renamedKeys.isEmpty()) {
        QSettings s;
        int moved = 0;
        for (const auto& kv : opt.renamedKeys) {
            if (!s.contains(kv.first)) continue;
            if (!s.contains(kv.second)) s.setValue(kv.second, s.value(kv.first));
            s.remove(kv.first);
            ++moved;
        }
        if (moved) qInfo("settings: migrated %d renamed key(s)", moved);
    }
    // Caches: move strays into data/cache, then prune superseded versions — AFTER the log is up so
    // the line reaches the log (FOX main.cpp:111-115).
    if (!opt.migrateCacheGlobs.isEmpty()) {
        const int n = paths::migrateCaches(opt.migrateCacheGlobs);
        if (n) qInfo("caches: moved %d file(s) into data/cache", n);
    }
    for (const RunOptions::Prune& p : opt.prunes) {
        // An empty dir means data/cache/ — resolvable only now that QApplication exists.
        const int n = paths::pruneOldCaches(p.dir.isEmpty() ? paths::cacheDir() : p.dir, p.stem, p.currentVersion, p.ext);
        if (n) qInfo("caches: pruned %d superseded %s*%s", n, qUtf8Printable(p.stem), qUtf8Printable(p.ext));
    }
    StartupProfile::instance().mark(QStringLiteral("settings, log, caches"));

    if (opt.beforeShell) opt.beforeShell();

    // The plugin's id policy, before any list exists.
    search::setIdPolicy([&plugin](const QString& t) { return plugin.isIdTerm(t); },
                        [&plugin](const QString& t, const AssetId& id) { return plugin.idTermMatches(t, id); });

    // Self-tests: a failure is loud but does not stop the app.
    const int failed = selftest::run(selftest::coreTests() + plugin.selfTests());
    StartupProfile::instance().mark(QStringLiteral("self-tests"));
    if (check) {
        // The conformance runner's needle: one line, one number.
        qInfo("self-test summary: %d failure(s)", failed);
        return failed;
    }

    if (!opt.iconResource.isEmpty()) {
        const QIcon appIcon(opt.iconResource);
        if (!appIcon.isNull()) app.setWindowIcon(appIcon);
        else qWarning("app icon: %s missing from the Qt resource — using the default", qUtf8Printable(opt.iconResource));
    }
    fontscale::apply();
    density::apply();

    AppShell shell(plugin);
    shell.setHarness(harness);
    if (harness) shell.setHarnessWindow(harnessWindow.width(), harnessWindow.height());
    // The harness watchdog (audit 2026-09-25): a run that never reaches its end — an index that
    // never reports, a hung load — used to hang the verification bat with it, silently. A thread,
    // not a timer, so a stuck GUI thread cannot stop it; stderr directly, not the log (whose lock
    // that stuck thread may hold). Exit code 4 = timed out.
    static std::atomic<bool> harnessDone{false};
    if (harness && harnessTimeoutS > 0) {
        std::thread([harnessTimeoutS] {
            for (int s = 0; s < harnessTimeoutS && !harnessDone.load(); ++s) std::this_thread::sleep_for(std::chrono::seconds(1));
            if (harnessDone.load()) return;
            std::fprintf(stderr, "harness: TIMEOUT after %d s — the run never finished; exiting with code 4\n", harnessTimeoutS);
            std::fflush(stderr);
            std::quick_exit(4);
        }).detach();
    }
    // A failed read of the game data ends a harness run with a failure line and code 2 — it used to
    // wait forever for a "ready" that could not come.
    if (quitWhenReady || !shotPng.isEmpty())
        QObject::connect(&shell, &AppShell::indexFailed, &app, [](const QString& status) {
            qWarning("shell: index FAILED — %s", qUtf8Printable(status));
            QCoreApplication::exit(2);
        });
    // --show-tab a,b,c brings each up in turn (one launch covers several tabs' checks); the last
    // stays on screen for a --shot.
    const QStringList showTabs = showTab.split(QLatin1Char(','), Qt::SkipEmptyParts);
    // The harness sequence, FOX's DevShot mechanism made shell: once the index is ready (or at
    // once when there is no game folder and it never will be) — show the tab, let the plugin act
    // on the flags it kept, then either settle → grab → write the PNG → quit (--shot, unless
    // --stay) or log the ready line and quit (--quit-when-ready).
    const bool pluginFlags = !consumed.isEmpty();
    if (quitWhenReady || !shotPng.isEmpty() || pluginFlags || !dumpPaths.isEmpty() || exportMenu) {
        auto onReady = [&shell, &plugin, showTabs, shotPng, settleMs, stay, quitWhenReady, pluginFlags, dumpPaths, dumpTo, exportMenu] {
            if (!dumpPaths.isEmpty()) dumpGamePaths(plugin, dumpPaths, dumpTo);
            for (const QString& t : showTabs) { shell.showTab(t.trimmed()); QApplication::processEvents(); }
            if (pluginFlags) plugin.applyArgs(shell);
            // --export-menu: after the plugin's flags, so the menu is the one a built scene offers.
            if (exportMenu) for (const QString& line : shell.exportMenuReport()) qInfo("exportmenu: %s", qUtf8Printable(line));
            // One event-loop turn so the current tab's refresh() has run and logged first.
            const bool configured = plugin.isConfigured();
            QTimer::singleShot(0, &shell, [&shell, shotPng, settleMs, stay, quitWhenReady, configured] {
                if (quitWhenReady)
                    qInfo("shell: ready — %s%s", qUtf8Printable(shell.tabIds().join(QStringLiteral(", "))),
                          configured ? "" : " (unconfigured: first-run page shown)");
                if (shotPng.isEmpty()) { if (quitWhenReady) qApp->quit(); return; }
                QTimer::singleShot(qMax(0, settleMs), &shell, [&shell, shotPng, stay] {
                    QApplication::processEvents();
                    const QPixmap pm = shell.grab();
                    const bool ok = !pm.isNull() && pm.save(shotPng, "PNG");
                    if (ok) qInfo("shot: %s (%dx%d)", qUtf8Printable(shotPng), pm.width(), pm.height());
                    else    qWarning("shot: could not write %s", qUtf8Printable(shotPng));
                    if (!stay) qApp->quit();
                });
            });
        };
        // --wait-secondary: act once the secondary indexes are in too (D4's clips come from one).
        if (plugin.isConfigured())
            QObject::connect(&shell, waitSecondary ? &AppShell::secondaryBecameReady : &AppShell::indexBecameReady, &app, onReady);
        else QTimer::singleShot(0, &shell, onReady);
    }
    shell.show();
    if (!showTabs.isEmpty()) shell.showTab(showTabs.last().trimmed());
    if (!crashTest.isEmpty()) {
        crash::setNotePath(crashNote.isEmpty() ? paths::file(plugin.productName() + QStringLiteral("-crash-test.txt")) : crashNote);
        // On the GUI thread, from the event loop — where a real fault happens.
        QTimer::singleShot(0, &shell, [crashTest] { crash::faultForShot(crashTest); });
    }
    StartupProfile::instance().mark(QStringLiteral("window shown"));
    shell.start();
    const int rc = app.exec();
    harnessDone = true;   // what follows (the plugin's own bounded shutdown) is not the harness's to time
    if (opt.afterExec) opt.afterExec();
    return rc;
}

}  // namespace core
