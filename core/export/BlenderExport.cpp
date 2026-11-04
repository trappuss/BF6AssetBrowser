#include "core/export/BlenderExport.h"

#include <QDir>
#include <QEventLoop>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>

namespace core::blender {
namespace {

bool runnable(const QString& exe) { return !exe.isEmpty() && QFileInfo(exe).isExecutable(); }

// The import/pack/save script, run as `blender -b --factory-startup --python <this> -- <glb> <blend>`.
// Native glTF import does the material wiring; we only make the .blend self-contained and tidy.
const char* kScript = R"PY(
import bpy, sys, os
argv = sys.argv[sys.argv.index("--") + 1:]
glb, blend = argv[0], argv[1]
bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.gltf(filepath=glb)
# The glTF importer builds a Principled BSDF per material from the PBR data (base colour, metallic +
# roughness from the ORM map via Separate Color, a Normal Map node, IOR from KHR_materials_ior).
# Pack the images so the .blend opens anywhere without the loose files.
try:
    bpy.ops.file.pack_all()
except Exception as e:
    print("pack_all:", e)
if bpy.context.scene.world is None:
    bpy.context.scene.world = bpy.data.worlds.new("World")
# Material-preview shading so the textures show the moment it opens.
for area in (bpy.context.screen.areas if bpy.context.screen else []):
    if area.type == 'VIEW_3D':
        for sp in area.spaces:
            if sp.type == 'VIEW_3D':
                sp.shading.type = 'MATERIAL'
d = os.path.dirname(blend)
if d:
    os.makedirs(d, exist_ok=True)
bpy.ops.wm.save_as_mainfile(filepath=blend)
print("BLEND_OK", blend)
)PY";

}  // namespace

QString findBlender(const QString& savedPath)
{
    if (runnable(savedPath)) return savedPath;

    const QString env = QProcessEnvironment::systemEnvironment().value(QStringLiteral("BLENDER"));
    if (runnable(env)) return env;

#ifdef Q_OS_WIN
    const QString exeName = QStringLiteral("blender.exe");
#else
    const QString exeName = QStringLiteral("blender");
#endif

    // Blender Launcher: %LOCALAPPDATA%\Blender Launcher\Blender Launcher.ini → library_folder + a
    // per-channel favorite build folder holding blender.exe.
    const QString la = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    for (const QString& base : { la }) {
        const QString ini = base + QStringLiteral("/Blender Launcher/Blender Launcher.ini");
        if (!QFileInfo::exists(ini)) continue;
        QSettings s(ini, QSettings::IniFormat);
        const QString lib = s.value(QStringLiteral("library_folder")).toString();
        const QString fav = QFileInfo(s.value(QStringLiteral("favorite_path")).toString()).fileName();
        if (lib.isEmpty()) continue;
        for (const QString& ch : { QStringLiteral("stable"), QStringLiteral("daily"), QStringLiteral("experimental") }) {
            if (!fav.isEmpty()) {
                const QString cand = QDir(lib).filePath(ch + QLatin1Char('/') + fav + QLatin1Char('/') + exeName);
                if (runnable(cand)) return cand;
            }
            // else fall through to scanning the channel dir below
            QDir chan(QDir(lib).filePath(ch));
            const QFileInfoList builds = chan.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name | QDir::Reversed);
            for (const QFileInfo& b : builds) {
                const QString cand = b.filePath() + QLatin1Char('/') + exeName;
                if (runnable(cand)) return cand;
            }
        }
    }

#ifdef Q_OS_WIN
    // Standard Blender Foundation install, highest version.
    for (const QString& pf : { QProcessEnvironment::systemEnvironment().value(QStringLiteral("ProgramFiles")),
                               QProcessEnvironment::systemEnvironment().value(QStringLiteral("ProgramW6432")) }) {
        if (pf.isEmpty()) continue;
        QDir bf(pf + QStringLiteral("/Blender Foundation"));
        const QFileInfoList vers = bf.entryInfoList(QStringList{ QStringLiteral("Blender*") },
                                                    QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name | QDir::Reversed);
        for (const QFileInfo& v : vers) {
            const QString cand = v.filePath() + QStringLiteral("/blender.exe");
            if (runnable(cand)) return cand;
        }
    }
#endif
    // PATH.
    const QString onPath = QStandardPaths::findExecutable(QStringLiteral("blender"));
    if (runnable(onPath)) return onPath;
    return {};
}

bool toBlend(const QString& blenderExe, const QString& glbPath, const QString& blendPath,
             QString* log, QString* err, int timeoutMs)
{
    const auto fail = [&](const QString& m) { if (err) *err = m; return false; };
    if (!runnable(blenderExe)) return fail(QStringLiteral("Blender executable not found or not runnable: %1").arg(blenderExe));
    if (!QFileInfo::exists(glbPath)) return fail(QStringLiteral("glb not found: %1").arg(glbPath));

    QTemporaryDir tmp;
    if (!tmp.isValid()) return fail(QStringLiteral("could not create a temp dir for the Blender script"));
    const QString scriptPath = tmp.filePath(QStringLiteral("ab_import.py"));
    { QFile f(scriptPath);
      if (!f.open(QIODevice::WriteOnly) || f.write(kScript) < 0) return fail(QStringLiteral("could not write the Blender script")); }

    QProcess proc;
    proc.setProcessChannelMode(QProcess::MergedChannels);
    proc.start(blenderExe, { QStringLiteral("-b"), QStringLiteral("--factory-startup"),
                             QStringLiteral("--python"), scriptPath,
                             QStringLiteral("--"), glbPath, blendPath });
    if (!proc.waitForStarted(15000)) return fail(QStringLiteral("could not start Blender: %1").arg(proc.errorString()));
    // Wait on a local event loop rather than blocking: a GUI caller that spins this inside a busy dialog
    // stays responsive while Blender runs (tens of seconds), and a headless caller just runs a nested
    // loop until the process exits. A timer caps the wait.
    bool finished = false;
    QEventLoop loop;
    QObject::connect(&proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                     &loop, [&](int, QProcess::ExitStatus) { finished = true; loop.quit(); });
    QTimer timer; timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, [&] { loop.quit(); });
    timer.start(timeoutMs);
    if (proc.state() != QProcess::NotRunning) loop.exec();
    if (!finished && proc.state() != QProcess::NotRunning) {
        proc.kill(); proc.waitForFinished(3000);
        return fail(QStringLiteral("Blender timed out"));
    }
    const QString out = QString::fromUtf8(proc.readAll());
    if (log) *log = out;

    if (!(out.contains(QStringLiteral("BLEND_OK")) && QFileInfo::exists(blendPath)))
        return fail(QStringLiteral("Blender did not produce the .blend (exit %1). Tail:\n%2")
                        .arg(proc.exitCode()).arg(out.right(600)));
    return true;
}

}  // namespace core::blender
