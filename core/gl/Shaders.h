#pragma once
#include "core/gl/ViewportExtension.h"
#include <QByteArray>

// The core GLSL, assembled with an extension's blocks spliced in. Kept apart from the widget so the
// conformance suite can compile every variant (empty blocks, D4's blocks) under Mesa without a
// window, and so the text is reviewable as text.
namespace core::gl {

struct ShaderSources {
    QByteArray vert, frag;          // the main PBR program
    QByteArray shadowVert, shadowFrag;
    QByteArray posVert, posFrag;    // SSAO world-position prepass
    QByteArray bgVert, bgFrag;      // studio-gradient backdrop
};

ShaderSources assembleShaders(const ShaderBlocks& blocks);
// The same with a game's whole shader in place of the core's main program (ViewportExtension::
// fragmentShader / vertexShader; an empty one keeps the core's stage). The shadow/SSAO/backdrop
// programs are always the core's.
ShaderSources assembleShaders(const ShaderBlocks& blocks, const QByteArray& vertexOverride, const QByteArray& fragmentOverride);

// Texture units the core owns. An extension uses 6-13 and 17-31.
enum TextureUnit : int {
    kUnitBase = 0, kUnitNormal = 1, kUnitOrm = 2, kUnitEmissive = 3, kUnitSpecColor = 4,
    kUnitLut = 5, kUnitReflCube = 14, kUnitShadow = 15, kUnitSsaoPos = 16,
};

}  // namespace core::gl
