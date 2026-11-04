#pragma once
// The generic main() (app-shell.md §5): SEH translator, GL default format, QApplication with
// org/app/version, the INI in data/, log + crash handler, key migrations, cache prune, self-tests,
// icon, style, the window, exec. Every game's main.cpp was these same lines in a slightly different
// order with slightly different omissions (POE2 installed no message handler at all; D4 hand-copied
// fourteen cache version numbers); this is the one copy, and a game supplies only its own tables.
#include "core/app/AppPlugin.h"

#include <QPair>
#include <QString>
#include <QStringList>
#include <QVector>
#include <functional>

namespace core {

struct RunOptions {
    // The default GL surface: 4.5 core (the core viewport is QOpenGLFunctions_4_5_Core), 4x MSAA,
    // depth 24, stencil 8 (the selection outline), alpha 8 (transparent capture).
    int glMajor = 4, glMinor = 5, samples = 4;
    QString organization;                 // defaults to productName
    QString iconResource;                 // ":/app_256.png" — warned about if missing
    QVector<QPair<QString, QString>> renamedKeys;   // old → new QSettings key migrations, applied once
    struct Prune { QString dir, stem; int currentVersion; QString ext; };   // dir "" = data/cache/
    QVector<Prune> prunes;                // versioned caches to prune at startup
    QStringList migrateCacheGlobs;        // caches an older build left in data/ → data/cache/
    bool logFileDefault = true;           // D4: default ON
    // Runs after QApplication, settings, log and cache pruning exist and before the self-tests
    // and the window — a game's own startup step (FOX prunes its own versioned caches).
    std::function<void()> beforeShell;
    // Runs after the event loop ends and BEFORE QApplication and the plugin's statics are torn down
    // — where a game stops the background sweeps it started (FOX's TextureUsers/RefIndex: a detached
    // sweep that outlives its singletons locks a destroyed mutex, and a harness run quits mid-sweep
    // every time — the 0xc0000005 on exit the 2026-09-22 character shot left in a crash note).
    std::function<void()> afterExec;
};

// Runs the whole application. Understands these flags and strips them before Qt sees the rest:
//   --check              run the self-tests and exit with the failure count (no window)
//   --quit-when-ready    build the window, load the game data, print "shell: ready" and quit
//   --harness            suppress every layout/session write
//   --show-tab <id>      select this tab once the window is up (--tab <id-or-title> is an alias)
//   --shot <png>         once ready (or at once when unconfigured): show the tab, let the plugin act
//                        on its own flags, wait --settle <ms> (default 1500), grab the window to
//                        <png>, log "shot: <png> (WxH)", quit unless --stay
//   --wait-secondary     the harness acts once the SECONDARY indexes are in, not at "index ready"
//                        (a game whose clips come from one — D4's animation index — needs it for
//                        --clip, or a shot of a model that should list its clips)
//   --density <key>      session-only row density / text size (never written to the INI)
//   --fontscale <pct>
// Flags a plugin recognises are handed to AppPlugin::parseArgs and removed before Qt sees them;
// when it took any, AppPlugin::applyArgs(shell) runs once the index is ready (before a --shot
// settles, before --quit-when-ready quits), so a flag that exports or dumps needs no --shot.
int runApp(int argc, char** argv, AppPlugin& plugin, const RunOptions& opt = RunOptions());

}  // namespace core
