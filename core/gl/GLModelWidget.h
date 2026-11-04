#pragma once
#include "core/gl/ViewportExtension.h"
#include "core/math/RigMath.h"
#include "core/types/Clip.h"
#include "core/types/Geometry.h"
#include "core/types/Material.h"
#include "core/types/Skeleton.h"

#include <QByteArray>
#include <QColor>
#include <QElapsedTimer>
#include <QHash>
#include <QImage>
#include <QMatrix4x4>
#include <QOpenGLFunctions_4_5_Core>
#include <QOpenGLWidget>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector3D>

#include <array>
#include <memory>

class QLabel;
class QTimer;
class QOpenGLFramebufferObject;

// ── The ONE model viewport ──────────────────────────────────────────────────────────────────────
//
// D4's GLModelWidget is the base (plan §8.2): its rendering pipeline — shadow map, SSAO, analytic
// hemisphere IBL with an optional prefiltered cubemap, alpha-to-coverage cutouts, ACES + grade,
// the stencil selection silhouette, click-on-release selection, supersampled capture, live-pose
// framing, camera glide, memoised uniforms, CPU skinning — is here, over the core types instead of
// D4's. What LEFT (viewport.md §3.1, plan §3.3): the cloth solver, dye/fur/FX/detail/hair/eye/skin/
// wetness/snow, the game-data reads in setGeometry, the detail_mask_probe write, the bone-hash
// dictionaries, and the QSettings reads — every one of them a `ViewportExtension` hook or a setter
// the app calls.
//
// What CAME IN from the other two: FOX's `ShadingMode` as the one state (D4 composed four booleans
// in each tab), the overlay MASTER GATE inside the widget (D4 kept three copies of it, one per
// tab), a selection SET the widget owns and mirrors out (D4's widget could not answer "what is
// selected"), `CameraPose` with presets and one setter, `renderAtSize`, context-loss recovery;
// POE2's `AlphaMode` two-pass draw (D4 had only alpha-to-coverage plus an FX pass),
// `renderToImage(scale, transparentBg, cropToModel)`, and the `skeletonMatchesMesh` fail-closed
// gate.
//
// Frame: everything the widget receives is canonical Y-up (core::Geometry's contract). Matrices are
// core::rm (row-major, v·M); the GL uniforms take QMatrix4x4 for the camera only.
namespace core::gl {

// FOX's four-way shading state. One enum, so the toolbar toggles, the popover and the shading
// balls all drive the same viewport and cannot disagree.
enum class ShadingMode : int { Wireframe = 0, Flat, Shaded, Rendered };
const char* shadingModeName(ShadingMode m);

class GLModelWidget : public QOpenGLWidget, protected QOpenGLFunctions_4_5_Core {
    Q_OBJECT
public:
    explicit GLModelWidget(QWidget* parent = nullptr);
    ~GLModelWidget() override;

    // The engine extension (may be null — the plain PBR viewport). Not owned. Must be set before
    // the first paint: the shader is assembled with its blocks in initializeGL.
    void setExtension(ViewportExtension* ext);
    ViewportExtension* extension() const { return m_ext; }

    // ── Scene ──────────────────────────────────────────────────────────────────────────────────
    // Geometry + rig. `keepView` preserves the orbit (re-assembling a model the user is inspecting).
    // The skeleton may be empty (static model). Materials are indexed by Part::materialIndex.
    void setScene(const Geometry& geo, const Skeleton& skeleton, bool keepView = false);
    void setMaterials(const QVector<Material>& materials);
    void setHardpoints(const QVector<Hardpoint>& hps);
    void clearScene();
    bool hasGeometry() const { return !m_indices.isEmpty(); }
    const Skeleton& skeleton() const { return m_skeleton; }

    // POE2's fail-closed rule: playback is offered ONLY when the rig actually covers the mesh's
    // joint palette. A body-armour piece skinned to a character rig it does not carry shows at
    // bind pose with a note, never animated against a mismatched skeleton.
    bool skeletonMatchesMesh() const { return m_skelMatchesMesh; }

    // ── Animation ──────────────────────────────────────────────────────────────────────────────
    // The clip is COPIED (a plugin may drop its own). Time is in seconds; setFrame is the
    // frame-rate-aligned convenience D4's tabs use. Playback lives in the owning tab's timer, which
    // the widget is handed rather than a flag it would have to keep in step (QPointer: a destroyed
    // tab reads as "not playing").
    void setClip(const Clip& clip);
    void clearClip();
    bool  hasClip() const { return m_hasClip; }
    void  setTime(float seconds);
    float time() const { return m_time; }
    float clipDuration() const { return m_hasClip ? m_clip.duration : 0.0f; }
    float clipFps() const { return m_hasClip ? m_clip.fps : 0.0f; }
    int   frameCount() const { return m_hasClip ? m_clip.frameCount() : 0; }
    void  setFrame(int f);
    int   frame() const;
    void  setPlaybackTimer(QTimer* t);
    QTimer* playbackTimer() const;              // the tab's timer, so a capture can pause and resume it
    bool  animPlaying() const;
    // Copy the CURRENT on-screen pose back into `geo` (positions + normals, vertex order preserved)
    // and zero the skinning so an exporter writes a static mesh frozen in this pose.
    bool snapshotPose(Geometry& geo) const;

    // ── Parts ──────────────────────────────────────────────────────────────────────────────────
    int  partCount() const { return m_parts.size(); }
    int  partTriangles(int i) const;
    void setPartVisible(int i, bool on);
    bool partVisible(int i) const;
    void setAllPartsVisible(bool on);
    void isolateParts(const QSet<int>& keep);        // hide everything else
    QSet<int> hiddenParts() const;

    // ── Selection (FOX: a SET the widget owns) ─────────────────────────────────────────────────
    // Drawn as a RED see-through outline; `active` is the last-added member (what "." frames and
    // what an info panel describes), always in the set or -1. Every mutation emits
    // selectionChanged so a parts panel can mirror it without polling.
    const QSet<int>& selectedParts() const { return m_selected; }
    int  activePart() const { return m_active; }
    void setSelectedParts(const QSet<int>& parts, int active = -1);
    void setSelectedPart(int part);                   // replaces the whole selection
    void addToSelection(int part);
    void removeFromSelection(int part);
    void toggleInSelection(int part);
    void clearSelection();
    // The set a context menu is ABOUT — BLUE outline, drawn last so it wins on overlap, cleared
    // when the menu closes. Right-clicking inside the selection makes the whole selection the
    // context, which is what "the menu acts on all of these" has to look like.
    void setContextParts(const QSet<int>& parts);
    const QSet<int>& contextParts() const { return m_context; }

    // ── Shading and overlays ───────────────────────────────────────────────────────────────────
    void setShadingMode(ShadingMode m);
    ShadingMode shadingMode() const { return m_shading; }
    // 0 shaded · 1 base · 2 normal · 3 rough · 4 metal · 5 AO · 6 emissive; an extension may add
    // more. Out-of-range renders as shaded.
    void setViewChannel(int c);
    int  viewChannel() const { return m_viewChannel; }
    // The MASTER GATE (template §3.3). Every setShow* below is remembered as the user set it and
    // drawn only while overlays are enabled; toggling the gate off and on restores exactly the set
    // that was on. In D4 this gate lived in each tab (three copies) and a settings replay that
    // bypassed it re-enabled overlays the user had turned off.
    void setOverlaysEnabled(bool on);
    bool overlaysEnabled() const { return m_overlaysOn; }
    void setShowGrid(bool on);          bool showGrid() const { return m_showGrid; }
    void setShowSkeleton(bool on);      bool showSkeleton() const { return m_showSkeleton; }
    void setShowHardpoints(bool on);    bool showHardpoints() const { return m_showHardpoints; }
    void setShowBoneNames(bool on);     bool showBoneNames() const { return m_showBoneNames; }
    void setShowSelection(bool on);     bool showSelection() const { return m_showSelection; }
    void setShowAxisGizmo(bool on);     bool showAxisGizmo() const { return m_showAxisGizmo; }
    void setGridAxisColors(bool on);
    void setBackfaceCull(bool on) { m_backfaceCull = on; update(); }
    void setOverlayText(const QString& text);   // centred hint / "Loading…" (empty hides)

    // ── Lighting and post ──────────────────────────────────────────────────────────────────────
    struct LightRig {
        int   preset       = 0;      // 0 warm key + cool rim/fill · 1 neutral · 2 cool studio
        float keyInt       = 1.0f;
        float rimInt       = 1.0f;
        float fillInt      = 1.0f;
        float ambInt       = 1.0f;
        float keyAzimuth   = 15.0f;  // deg, + = key swings to camera-right
        float keyElevation = 25.0f;  // deg above the camera horizon
    };
    void     setLightRig(const LightRig& r);
    LightRig lightRig() const { return m_rig; }
    // The whole look at once — what the game's plugin asked for (AppPlugin::viewFeatures).
    void setFeatures(const ViewFeatures& f);
    ViewFeatures features() const;
    void setLightLock(bool worldFixed);
    void setExposure(float v);
    void setFeatureIbl(bool on);
    void setFeatureTonemap(bool on);
    void setFeatureSpecAA(bool on);
    void setFeatureSubsurface(bool on);          // forwarded to the extension's shader via uFSubsurf
    void setColorGrade(bool on, float contrast, float sat, float warmth);
    void setColorGradeLut(const QImage& lut);    // 256x16 strip; empty/other size clears it
    void setEmissiveScale(float v);
    // Reflection probe: six faces in cube order (+X -X +Y -Y +Z -Z), any QImage format (HDR via
    // Format_RGBA16FPx4). The mip chain is built on the GPU. Empty clears → analytic fallback.
    void setReflectionCubemap(const QVector<QImage>& faces);
    void setReflectionEnabled(bool on);
    void setReflectionStrength(float v);
    void setShadowEnabled(bool on);
    void setShadowParams(float strength, float softnessTexels, float bias);
    void setShadowExtra(float rangeMul, float normalBiasFrac, int resolution);
    void setSsaoEnabled(bool on);
    void setSsaoParams(float strength, float radiusFrac);
    void setBackgroundColor(const QColor& c);
    QColor backgroundColor() const;
    void setBackgroundGradient(bool on) { m_bgGradient = on; update(); }
    // Textures-off draws every part in flat grey (a Shaded/Rendered look at the geometry alone).
    void setShowTextures(bool on);

    // ── Camera ─────────────────────────────────────────────────────────────────────────────────
    // FOX's one camera setter: a pose is yaw, pitch, distance, centre, projection and field, and
    // every preset and restore goes through it.
    struct CameraPose {
        float yaw = 0.6f, pitch = 0.25f, dist = 3.0f, fov = 45.0f;
        QVector3D center;
        bool ortho = false;
        bool valid = false;
        QString toString() const;
        static bool fromString(const QString& s, CameraPose* out);
    };
    CameraPose cameraPose() const;
    void setCameraPose(const CameraPose& pose);
    // front, back, left, right, top, bottom, iso, default — framed on the live bounds.
    bool applyCameraPreset(const QString& name, bool animate = true);
    static const QStringList& builtinCameraPresets();
    void setOrthographic(bool on) { m_ortho = on; update(); }
    bool orthographic() const { return m_ortho; }
    void setFov(float deg);
    float camYaw() const { return m_yaw; }
    float camPitch() const { return m_pitch; }
    void  setOrbitYaw(float y) { m_yaw = y; update(); }   // turntable capture drives this
    void  setOrbitAngles(float yawRad, float pitchRad);
    void  orbitToAxis(float yawRad, float pitchRad);
    void  resetView();
    void  frameAll(bool keepRotation, bool animate = true);
    void  frameRegion(const QVector3D& center, float radius, float yawRad, float pitchRad, bool animate);
    void  frameRegionKeepRotation(const QVector3D& center, float radius, bool animate);
    bool  partsBounds(const QVector<int>& parts, QVector3D& center, float& radius) const;   // LIVE pose
    void  sceneBounds(QVector3D& center, float& radius) const { liveBounds(center, radius); } // whole scene, LIVE pose
    void  followParts(const QVector<int>& parts);
    void  setAutoSpin(bool on);
    void  setSpinSpeed(float radPerTick) { m_spinSpeed = radPerTick; }
    // Ray-pick the front-most visible part under a widget-space point (-1 = miss), against the
    // LIVE skinned vertices.
    int   pickPart(const QPoint& posPx) const;

    // ── Capture ────────────────────────────────────────────────────────────────────────────────
    // The full pipeline at `factor`x the viewport's own resolution — real extra pixels, not an
    // upscale: every pass sizes itself from the framebuffer size. Null if the driver refuses.
    QImage grabSupersampled(int factor);
    // The scene rendered at an arbitrary size (FOX). Same mechanism, explicit dimensions.
    QImage renderAtSize(int w, int h);
    // POE2's export capture: scalePercent re-renders larger, transparentBg clears alpha to 0,
    // cropToModel trims transparent margins (meaningful with transparentBg). Guides suppressed.
    QImage renderToImage(int scalePercent, bool transparentBg, bool cropToModel);
    // Bind-pose grey thumbnail from a persistent FBO (recreating one per call is a known
    // driver-crash source).
    QImage grabThumbnail(int size = 64);
    void setTransparentClear(bool on) { m_transparentClear = on; }
    void setCoverageAlpha(bool on) { m_coverageAlpha = on; }
    // Export capture scope: deterministic time, no idle stepping (forwarded to the extension).
    void setCaptureMode(bool on);
    bool capturing() const { return m_capturing; }
    void setCaptureTime(float seconds) { m_captureTime = seconds; }
    struct CaptureScope {
        GLModelWidget* v;
        explicit CaptureScope(GLModelWidget* w) : v(w) { if (v) v->setCaptureMode(true); }
        ~CaptureScope() { if (v) v->setCaptureMode(false); }
        CaptureScope(const CaptureScope&) = delete;
        CaptureScope& operator=(const CaptureScope&) = delete;
    };

    static QString glInfo() { return s_glInfo; }
    QString lastError() const { return m_error; }

signals:
    void selectionChanged(const QSet<int>& parts, int active);
    void partClicked(int part, Qt::KeyboardModifiers mods);   // left click, no drag (-1 = miss)
    void partRightClicked(int part, const QPoint& globalPos);  // right click, no drag
    void partFocused(int part);                                // double-click
    void shadingModeChanged(ShadingMode mode);
    void cameraChanged();

protected:
    void initializeGL() override;
    void resizeGL(int w, int h) override;
    void paintGL() override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;

private:
    // GL objects
    GLuint compileProgram(const QByteArray& vs, const QByteArray& fs, const char* what);
    GLint  uni(GLuint prog, const char* name);
    GLuint targetFbo() const { return m_captureFbo ? m_captureFbo : GLuint(defaultFramebufferObject()); }
    void   releaseGL();          // every GL handle → 0 (context lost or destroyed)
    void   uploadPending();
    void   uploadTextures();
    void   destroyBuffers();
    void   destroyTextures();
    void   uploadReflectionCubemap();
    void   renderShadow(const QMatrix4x4& lightMvp);
    void   renderPos(const QMatrix4x4& mvp, const QMatrix4x4& model);
    void   drawPartIndices(int part);
    void   drawLines(const QVector<float>& xyz, float r, float g, float b);
    void   buildGrid();
    void   buildSkeleton();
    void   buildHardpoints();
    void   updateBoneLabels();
    void   updateHardpointLabels();
    void   uploadLineVao(GLuint& vao, GLuint& vbo, const QVector<float>& v11, bool dynamic);
    QImage renderOffscreen(int w, int h, bool keepAlphaAsCoverage);

    // Pose
    void applySkinning();
    QVector<rm::Mat4> currentWorldPose() const;   // for overlays: the pose the mesh is drawn in
    void liveBounds(QVector3D& center, float& radius) const;
    void emitSelection();
    void applyRig();
    void cameraBasis(QVector3D& eye, QMatrix4x4& proj, QMatrix4x4& view, float aspect) const;

    ViewportExtension* m_ext = nullptr;
    QMetaObject::Connection m_ctxLostConn;   // see ~GLModelWidget

    // CPU-side geometry (11 interleaved floats: pos, normal, uv, tangent — D4's layout, kept so
    // the D4 extension's fur/FX vertex code and the overlays' VAOs need no change).
    bool                m_hasPending = false;
    QVector<float>      m_verts;          // LIVE (skinned) pose
    QVector<float>      m_bindVerts;      // immutable bind pose
    QVector<quint32>    m_indices;
    struct Part { int offset = 0; int count = 0; bool visible = true; int material = -1; QString name; bool doubleSided = false; };
    QVector<Part>       m_parts;
    QVector<std::array<quint16, 4>> m_vJoints;
    QVector<std::array<float, 4>>   m_vWeights;
    Skeleton            m_skeleton;
    bool                m_skinned = false;
    bool                m_skelMatchesMesh = false;
    QVector<Hardpoint>  m_hardpoints;
    QVector<Material>   m_materials;      // CPU copies (re-upload after a context loss)
    bool                m_hasPendingTex = false;

    // Selection
    QSet<int> m_selected, m_context;
    int       m_active = -1;
    QVector<int> m_followParts;

    // Animation
    Clip  m_clip;
    bool  m_hasClip = false;
    float m_time = 0.0f;
    QPointer<QObject> m_playbackTimer;
    QElapsedTimer m_clock;          // wall clock for the FX time uniform and pose dt
    qint64 m_lastPoseMs = -1;

    // Shading state
    ShadingMode m_shading = ShadingMode::Rendered;
    bool  m_showTex = true;
    int   m_viewChannel = 0;
    bool  m_overlaysOn = true;
    bool  m_showGrid = false, m_showSkeleton = false, m_showHardpoints = false, m_showBoneNames = false;
    bool  m_showSelection = true, m_showAxisGizmo = true, m_gridAxisColors = true;
    bool  m_backfaceCull = true;
    // The look, all OFF by default (ViewportExtension.h "Which look a viewport has"); setFeatures.
    bool  m_fIbl = false, m_fTonemap = false, m_fSpecAA = false, m_fSubsurf = false;
    float m_exposure = 1.0f, m_emisScale = 0.5f;
    bool  m_colorGrade = false;
    float m_cgContrast = 1.05f, m_cgSat = 1.10f, m_cgWarmth = 0.03f;
    QImage m_pendingLut; bool m_lutDirty = false, m_hasLut = false;
    LightRig m_rig;
    float m_envSky[3] = {0.42f, 0.48f, 0.60f}, m_envHor[3] = {0.30f, 0.30f, 0.33f}, m_envGnd[3] = {0.12f, 0.11f, 0.10f};
    float m_lightCol[3] = {3.0f, 3.0f, 3.0f}, m_rimCol[3] = {0, 0, 0}, m_fillCol[3] = {0, 0, 0};
    float m_ambScale = 1.0f;
    bool  m_lightLock = false, m_lockValid = false;
    QVector3D m_lockKey, m_lockRim, m_lockFill;
    bool  m_reflEnabled = false; float m_reflStrength = 1.0f;
    QVector<QImage> m_pendingRefl; bool m_hasPendingRefl = false; int m_reflMaxMip = 0;
    bool  m_shadowOn = false; float m_shadowStr = 0.6f, m_shadowSoft = 1.5f, m_shadowBias = 0.0018f;
    float m_shadowRange = 1.3f, m_shadowNBias = 0.01f; int m_shadowSize = 2048; bool m_shadowResDirty = false;
    bool  m_ssaoOn = false; float m_ssaoStr = 1.0f, m_ssaoRad = 0.30f;
    float m_bg[3] = {0.10f, 0.10f, 0.11f};
    bool  m_bgGradient = false, m_transparentClear = false, m_coverageAlpha = false;
    bool  m_capturing = false; float m_captureTime = 0.0f;

    // GL handles
    GLuint m_prog = 0, m_shadowProg = 0, m_posProg = 0, m_bgProg = 0;
    GLuint m_vao = 0, m_vbo = 0, m_ibo = 0, m_bgVao = 0;
    GLuint m_shadowFbo = 0, m_shadowTex = 0, m_posFbo = 0, m_posTex = 0, m_posDepth = 0;
    int    m_posW = -1, m_posH = -1;
    GLuint m_reflCube = 0, m_lutTex = 0;
    GLuint m_gridVao = 0, m_gridVbo = 0, m_skelVao = 0, m_skelVbo = 0, m_hpVao = 0, m_hpVbo = 0;
    GLuint m_lineVao = 0, m_lineVbo = 0;
    int    m_gridVerts = 0, m_gridAxisFirst = 0, m_skelVerts = 0, m_hpAxisVerts = 0;
    int    m_indexCount = 0;
    int    m_fbW = 1, m_fbH = 1;
    GLuint m_captureFbo = 0;
    std::unique_ptr<QOpenGLFramebufferObject> m_thumbFbo; int m_thumbFboSize = 0;
    float  m_maxAniso = 1.0f;
    QHash<GLuint, QHash<QByteArray, GLint>> m_uniLoc;
    // Per-material GL textures (index = material).
    struct MatTex { GLuint base = 0, normal = 0, orm = 0, emissive = 0, specColor = 0; };
    QVector<MatTex> m_matTex;
    QMatrix4x4 m_lastViewProj;
    static QString s_glInfo;
    QString m_error;

    // Camera
    QVector3D m_center, m_homeCenter;
    float m_radius = 1.0f, m_yaw = 0.6f, m_pitch = 0.25f, m_dist = 3.0f, m_fov = 45.0f;
    bool  m_ortho = false;
    QTimer* m_camAnim = nullptr;
    QVector3D m_tgtCenter; float m_tgtYaw = 0, m_tgtPitch = 0, m_tgtDist = 0;
    QTimer* m_spinTimer = nullptr; float m_spinSpeed = 0.025f;
    QTimer* m_repaintTimer = nullptr;
    QPoint m_lastPos, m_rightPressPx, m_leftPressPx;
    bool   m_swallowLeftClick = false;

    // Overlay children
    QLabel*  m_overlayLabel = nullptr;
    QWidget* m_gizmo = nullptr;
    QWidget* m_boneLabels = nullptr;
    QWidget* m_hpLabels = nullptr;
};

}  // namespace core::gl
