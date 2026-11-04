#pragma once
#include "core/math/RigMath.h"
#include <QByteArray>
#include <QOpenGLFunctions_4_5_Core>
#include <QVector>
#include <functional>

// ── The viewport's engine seam ──────────────────────────────────────────────────────────────────
//
// D4's GLModelWidget is the rendering base (plan §8.2), and it carries ~1,900 lines of cloth solver
// and ~60 material setters for dye, fur, mesh-FX, detail maps, hair, eyes, skin, wetness and snow —
// all of it Diablo IV. The plan's phase-3 surgery moves every one of those behind THIS interface,
// so that `core::gl::GLModelWidget` is the rendering pipeline alone (shadow map, SSAO, IBL, ACES,
// alpha-to-coverage, stencil outline, CPU skinning, camera, picking) and a game that needs more
// attaches an extension.
//
// The shader is the hard part: D4's fragment shader is one 775-line program with 120 uniforms, and
// the D4-only code is interleaved with the PBR core at eleven points. Rather than a second copy of
// the shader per game, the core shader has SPLICE POINTS and an extension supplies GLSL text for
// each. With every block empty the core shader compiles and renders the plain PBR path; with D4's
// blocks in, the assembled program is D4's. `core::gl::assembleShaders()` does the splice and the
// conformance suite compiles the result under Mesa so a bad block fails there, not on the user's
// GPU.
//
// Everything here is called with the widget's GL context current and its main program bound.
namespace core::gl {

// GLSL fragments spliced into the core shaders at fixed points. Every block defaults to empty, and
// the core shader is required to compile and render correctly with all of them empty — that is the
// "no extension" viewport. Variables named in the comments are in scope at that point.
struct ShaderBlocks {
    // Vertex stage.
    QByteArray vertUniforms;    // extra `uniform`/`out` declarations
    QByteArray vertDisplace;    // `vec3 pos = aPos;` is in scope and may be modified (fur, FX wobble)

    // Fragment stage.
    QByteArray fragUniforms;    // extra `uniform`/`in` declarations, helper functions
    QByteArray fragEarly;       // `vec4 base` sampled; may `return` (an unlit FX path) or edit `base`
    QByteArray fragCutout;      // `float thr`, `float aaFloor`, `float hardCutBias` may be adjusted
                                //   before the alpha-cutout test (D4 hair widens the band)
    QByteArray fragSurface;     // N, T, B, tbn, base, metal, rough, ao in scope; may modify any
    QByteArray fragChannels;    // `else if (uViewChannel == N) dbg = …;` extra debug channels
    QByteArray fragAlbedo;      // `vec3 albedo` (linear when tonemapping), rough, metal; may modify
    QByteArray fragSpec;        // `vec3 spec` from Cook-Torrance is in scope; may replace it (hair)
    QByteArray fragLighting;    // `vec3 Lo` = key-light diffuse+spec; add terms BEFORE the shadow
    QByteArray fragAmbient;     // `vec3 ambDiff, ambSpec` in scope; may modify (hair kills reflections)
    QByteArray fragFinal;       // `vec3 col`, `float outAlpha` before write; may modify (fur AO)
};

// What the widget knows about the frame when it asks an extension for uniforms.
struct FrameContext {
    GLuint prog = 0;            // the main program, already bound
    float  radius = 1.0f;       // model bounding radius (D4 scales fur/skin curvature by it)
    float  timeSeconds = 0.0f;  // wall-clock or deterministic capture time
    bool   pbr = true;          // ShadingMode::Rendered
    bool   texturesOn = true;
    bool   wireframe = false;
    bool   msaa = true;         // the bound target is multisampled (alpha-to-coverage available)
    int    partCount = 0;
};

// Per-part draw callback handed to the extension so it can redraw a part (fur shells) or draw it
// in its own pass (mesh FX) without knowing the VAO layout.
using DrawPartFn = std::function<void(int part)>;

// Line-overlay callback: `xyz` is a flat list of line-segment endpoints (6 floats per segment) in
// model space, drawn with depth test off in the given colour. For extension overlays (D4's physics
// bones and collision capsules).
using DrawLinesFn = std::function<void(const QVector<float>& xyz, float r, float g, float b)>;

// ── Which look a viewport has ─────────────────────────────────────────────────────────────────
//
// The core viewport is the PIPELINE, not a look (plan decision, 2026-09-23): the goal of every
// (Game|Engine)AssetBrowser viewport is to come as close to the GAME's own rendering as the data
// allows, and nothing a game does not do should be on by default. So the lit features the core
// carries — shadow map, SSAO, IBL, ACES tonemap, specular AA, subsurface wrap, reflection cubemap,
// colour grade — all default OFF; the extension-less look is the plain read of the store's
// materials (Cook-Torrance, one key light, gamma). A game's plugin asks for what its renderer has
// (AppPlugin::viewFeatures; D4's rig turns its own on) and its extension supplies the rest.
struct ViewFeatures {
    bool shadow = false, ssao = false, ibl = false, tonemap = false, specAA = false, subsurface = false, reflection = false;
};

class ViewportExtension {
public:
    virtual ~ViewportExtension() = default;

    // ── The shading, two ways ──────────────────────────────────────────────────────────────────
    // REPLACE: the game's whole fragment shader (and vertex shader if it needs one). Empty = use
    // the core's with the blocks below spliced in. A replacement is compiled as-is, so it declares
    // its own uniforms and samplers; the core guarantees these inputs and sets them by name (a
    // name the shader does not declare is simply not set):
    //   vertex   : layout 0 aPos, 1 aNormal, 2 aUV, 3 aTangent (11-float interleaved, CPU-skinned)
    //              uniform mat4 uMVP, uModel; vec2 uNdcOffset (add uNdcOffset*w to gl_Position.xy —
    //              the selection silhouette pass draws the same VAO through the same program)
    //   fragment : the core vertex stage's outs — vNormal, vUV, vTangent, vWorldPos (world space);
    //              per part: sampler2D uTex/uNormalTex/uOrmTex/uEmissiveTex/uSpecColorTex on units
    //              0-4 with int uHasTex/uHasNormal/uHasOrm/uHasEmissive/uHasSpecColor, float uMetal,
    //              uRough, uAlphaCutoff, int uAlphaMode (0 opaque 1 mask 2 blend 3 add), uOrmHasAo;
    //              per frame: vec3 uViewPos, uLightDir, uLightCol, float uExposure, uTime, int uPbr
    //              (0 = Flat/Wireframe shading: draw unlit), uViewChannel (0 = shaded; 1.. the core
    //              debug channels the tab offers); vec3 uBase is the material's flat colour when
    //              uHasTex == 0 AND the silhouette colour when int uSolid == 1 (the selection
    //              outline pass draws the same parts through THIS program: when uSolid == 1 write
    //              vec4(uBase, 1) and return, honouring the alpha cutout).
    //              Anything else (uShadowMap…, uPosTex, uReflCube, uLut) is the core look's and a
    //              replacement ignores it.
    //   output   : layout(location = 0) out vec4 FragColor, DISPLAY-referred (gamma applied).
    // The extension's own uniforms/textures are set in frameUniforms / partUniforms as usual.
    virtual QByteArray fragmentShader() const { return {}; }
    virtual QByteArray vertexShader() const { return {}; }

    // SPLICE: the GLSL blocks spliced into the core shader. Called when the program is (re)built,
    // i.e. in initializeGL. Ignored when fragmentShader() is non-empty.
    virtual ShaderBlocks shaderBlocks() const { return {}; }

    // GL lifetime. `gl` is the widget's function table; destroyGL is also called before a context
    // is lost, after which initializeGL runs again on the new one.
    virtual void initializeGL(QOpenGLFunctions_4_5_Core& gl) { Q_UNUSED(gl); }
    virtual void destroyGL(QOpenGLFunctions_4_5_Core& gl) { Q_UNUSED(gl); }
    // The extension's own textures are uploaded here, once per frame when it has reported them
    // pending (see texturesPending). `partCount` is the current part roster size.
    virtual bool texturesPending() const { return false; }
    virtual void uploadTextures(QOpenGLFunctions_4_5_Core& gl, int partCount) { Q_UNUSED(gl); Q_UNUSED(partCount); }

    // Uniforms that hold for the whole frame (global toggles, rig-derived values). Texture units
    // 0-5 and 14-16 are the core's (base, normal, orm, emissive, spec-colour, LUT, cubemap, shadow,
    // SSAO); an extension may use 6-13 and 17-31.
    virtual void frameUniforms(QOpenGLFunctions_4_5_Core& gl, const FrameContext& fc) { Q_UNUSED(gl); Q_UNUSED(fc); }
    // Per-part uniforms and texture binds, just before the part is drawn in the main pass.
    virtual void partUniforms(QOpenGLFunctions_4_5_Core& gl, const FrameContext& fc, int part) { Q_UNUSED(gl); Q_UNUSED(fc); Q_UNUSED(part); }
    // False keeps a part out of the main, shadow and SSAO passes (D4's transparent mesh-FX parts,
    // which the extension draws itself in extraPass).
    virtual bool partInMainPass(int part) const { Q_UNUSED(part); return true; }
    // After a part's main draw, with its textures still bound (D4 redraws fur parts as shells).
    virtual void afterPart(QOpenGLFunctions_4_5_Core& gl, const FrameContext& fc, int part, const DrawPartFn& draw)
    { Q_UNUSED(gl); Q_UNUSED(fc); Q_UNUSED(part); Q_UNUSED(draw); }
    // After the opaque/mask pass and the selection outline, before the core's blended pass.
    virtual void extraPass(QOpenGLFunctions_4_5_Core& gl, const FrameContext& fc, const DrawPartFn& draw)
    { Q_UNUSED(gl); Q_UNUSED(fc); Q_UNUSED(draw); }
    // True asks the widget to keep repainting (animated FX).
    virtual bool wantsContinuousRepaint() const { return false; }

    // ── Pose ───────────────────────────────────────────────────────────────────────────────────
    // The world matrices for the current frame, AFTER the clip was evaluated and BEFORE the skin
    // palette is built. D4's cloth solver rewrites the cloth bones here. `baseBones` is
    // Skeleton::nBaseBones. Return true if anything changed.
    virtual bool modifyPose(QVector<rm::Mat4>& global, int baseBones, float dtSeconds)
    { Q_UNUSED(global); Q_UNUSED(baseBones); Q_UNUSED(dtSeconds); return false; }
    // Where the skeleton overlay should draw joint `j`'s head, if not at the posed matrix (D4 draws
    // simulated cloth bones at their live particle position). Return false to use the pose.
    virtual bool jointHeadOverride(int j, float out[3]) const { Q_UNUSED(j); Q_UNUSED(out); return false; }
    // A new model or clip was installed: reset any per-model solver state.
    virtual void sceneChanged() {}
    virtual void clipChanged() {}
    // Export capture: deterministic stepping on, wall-clock idle stepping off (see D4 setCaptureMode).
    virtual void setCaptureMode(bool on) { Q_UNUSED(on); }

    // ── Overlays ───────────────────────────────────────────────────────────────────────────────
    // Drawn after the core overlays, depth test off, through the line helper.
    virtual void drawOverlays(const DrawLinesFn& lines) { Q_UNUSED(lines); }
};

}  // namespace core::gl
