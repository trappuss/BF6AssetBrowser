// BF6AssetBrowser — Battlefield 6 (Frostbite) on the AssetBrowser core.
//
// Everything generic — the GL format, QApplication identity, the portable INI, the log handler, the
// self-tests, the window, the harness flags — is core::runApp. BF6 supplies only its plugin: the
// Frostbite store (Bf6Toc + Bf6Store) and the id/noun/export policy. In-app 3D rendering and cooked
// mesh/texture export wait on BF6's object-format RE (phase 2); v1 lists, searches and raw-exports.
#include "core/app/Application.h"
#include "games/bf6/Bf6Plugin.h"

int main(int argc, char** argv)
{
    bf6game::Bf6Plugin plugin;
    core::RunOptions opt;
    opt.organization = QStringLiteral("BF6AssetBrowser");
    opt.logFileDefault = false;
    opt.afterExec = [&plugin] { plugin.shutdown(); };   // join the index worker while qApp still exists
    return core::runApp(argc, argv, plugin, opt);
}
