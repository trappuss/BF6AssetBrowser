#include "core/types/ExportOptions.h"
#include <QStringList>

namespace core {

QString ExportOptions::describe() const
{
    QStringList p;
    p << QStringLiteral("scale=%1%2").arg(double(unitScale))
             .arg(scaleMode == WrapperNode ? QStringLiteral("(node)") : QString());
    if (upAxis == ZUp)          p << QStringLiteral("z-up");
    if (yawDegrees != 0.0f)     p << QStringLiteral("yaw=%1").arg(double(yawDegrees));
    p << (includeSkeleton ? QStringLiteral("skeleton") : QStringLiteral("no-skeleton"));
    if (!includeAnimations)     p << QStringLiteral("no-anim");
    else if (onlyClip >= 0)     p << QStringLiteral("clip=%1").arg(onlyClip);
    if (animationLibrary)       p << QStringLiteral("anim-library");
    if (includeAttachments)     p << QStringLiteral("attachments");
    p << (embedTextures ? QStringLiteral("embedded") : looseTextures ? QStringLiteral("loose") : QStringLiteral("no-textures"));
    if (jpegQuality > 0)        p << QStringLiteral("jpeg=%1").arg(jpegQuality);
    if (maxTextureSize > 0)     p << QStringLiteral("texcap=%1").arg(maxTextureSize);
    if (!pruneUnusedMaterials)  p << QStringLiteral("keep-unused-materials");
    if (hardpointEmpties)       p << QStringLiteral("hardpoints");
    if (reduceRig)              p << QStringLiteral("reduce-rig");
    if (xMirrorSymmetrize)      p << QStringLiteral("x-mirror");
    if (writeVertexColor)       p << QStringLiteral("vertex-colour");
    if (gltfSeparate)           p << QStringLiteral("gltf+bin");
    return p.join(QStringLiteral(" · "));
}

}  // namespace core
