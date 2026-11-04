#pragma once
#include "core/math/RigMath.h"
#include <QString>
#include <QStringList>
#include <QVector>
#include <cstdint>

// ── The engine-neutral mesh ─────────────────────────────────────────────────────────────────────
//
// Derived field-by-field from the three real contracts (geometry-export.md §1):
//   POE2 ModelGeometry  — shared vertex/index buffer, parts as index ranges, tangent, bbox  (the base)
//   D4   ModelGeometry  — per-primitive buffers, COLOR_0/1, UV1, per-part doubleSided, slotHash
//   FOX  fox::FmdlFile  — SoA per mesh, palette-relative u8 joints, a mesh-group TREE
//
// Structural shape is POE2's: D4's per-primitive arrays are a degenerate case (concatenate and
// record ranges) and FOX's SoA flattens to it — its own viewport already does exactly that.
//
// FRAME: canonical Y-up, right-handed, ALWAYS. An adapter applies its one core::rm::k*ToYUp
// rotation at ingest and nothing downstream rotates again. D4's contract is mixed-space today
// (Y-up vertices, Z-up rest TRS and hardpoints) and that is the bug this rule exists to prevent.
//
// UNITS: whatever the engine authored. The scale to metres is an EXPORT option (D4 1.0, FOX 1.0,
// POE2 0.005), not a property of the geometry — baking it here would make the viewport's grid and
// the exporter's unit scale two spellings of one number, which is how they drift.
namespace core {

struct Vertex {
    float px = 0, py = 0, pz = 0;              // POSITION                      [D4][FOX][POE2]
    float nx = 0, ny = 0, nz = 1;              // NORMAL (unit)                 [D4][FOX][POE2]
    float tx = 1, ty = 0, tz = 0, tw = 1;      // TANGENT xyz + handedness      [FOX][POE2]
    float u  = 0, v  = 0;                      // TEXCOORD_0                    [D4][FOX][POE2]
    float u2 = 0, v2 = 0;                      // TEXCOORD_1                    [D4][POE2]
    uint8_t color[4]  = {255,255,255,255};     // COLOR_0                       [D4 f32→u8][POE2]
    uint8_t color1[4] = {255,255,255,255};     // COLOR_1 (D4 diagnostic)       [D4]
    // u16, not u8: POE2 stores u8 and so cannot represent a rig past 256 bones before its own
    // exporter widens it (geometry-export.md appendix). D4 is already u16; FOX's palette indices
    // are resolved to global skeleton indices by its adapter.
    uint16_t joints[4]  = {0,0,0,0};           // JOINTS_0
    float    weights[4] = {0,0,0,0};           // WEIGHTS_0 — all zero ⇒ static
};

// One drawable submesh: a contiguous run of the shared index buffer.
struct Part {
    QString  name;                      // [FOX] group name  [POE2] Maya DAG leaf  [D4] from subObjectHash
    uint32_t indexStart = 0;
    uint32_t indexCount = 0;
    int      materialIndex = -1;        // into Geometry::materialNames (-1 = none)
    bool     visible = true;            // [POE2 MeshPart::visible] [FOX shippedHiddenGroups]
    bool     doubleSided = false;       // [D4 per primitive]; FOX is always true, POE2 per material
    int      group = -1;                // into Geometry::groups; -1 = ungrouped
    uint32_t nameHash = 0;              // [D4 subObjectHash] [FOX nameHash32] — survives renaming
    uint32_t slotHash = 0;              // [D4] equipment slot (bdy/trs/leg/hlm/glv/bts); 0 elsewhere

    uint32_t triangleCount() const { return indexCount / 3; }
};

// A node in the part TREE. Only Fox Engine authors one (its FMDL mesh groups, which is what its
// scene outliner draws and what a .fv2 variation names when it hides a pouch); D4 and PoE2 leave
// `groups` empty and their parts sit at the root. One int on Part cannot carry a tree, and
// flattening the tree at ingest would lose exactly the structure the Outliner (template §21)
// exists to show — so it is four fields here rather than a lossy encoding.
struct PartGroup {
    QString  name;
    int      parent = -1;               // into Geometry::groups; -1 = root. Parents precede children.
    bool     visible = true;            // [FOX shippedHiddenGroups] — authored-hidden alternatives
    uint32_t nameHash = 0;
};

struct Geometry {
    QVector<Vertex>   vertices;
    QVector<uint32_t> indices;          // triangle list; u32 everywhere (FOX widens its u16 free)
    QVector<Part>     parts;
    QVector<PartGroup> groups;          // [FOX] the mesh-group tree; empty elsewhere
    QStringList       materialNames;    // roster: [D4 palette names] [POE2 .mat paths] [FOX instance names]

    float bboxMin[3] = {0,0,0};
    float bboxMax[3] = {0,0,0};

    // Attribute presence — an exporter must not emit TANGENT for a mesh that never had one.
    bool hasTangent = false;
    bool hasUv1     = false;
    bool hasColor   = false;
    bool hasColor1  = false;
    bool skinned    = false;

    // Provenance, for the Explain report and for honest error messages.
    QString sourcePath;
    int     formatVersion = 0;
    QString formatNote;                 // e.g. "smd v2", "fmdl v2.04", "app LOD0"
    // Parts the parser could not load — non-zero means the mesh RENDERS BUT IS INCOMPLETE, which
    // passes every "did it load?" check while visibly missing pieces. [D4 droppedSubObjects]
    int      droppedParts = 0;
    uint32_t droppedVerts = 0;

    bool isEmpty() const { return vertices.isEmpty() || indices.isEmpty(); }
    int  triangleCount() const { return indices.size() / 3; }

    // Highest joint index a WEIGHTED vertex references, +1 — the palette a rig must cover.
    int jointPaletteSize() const
    {
        int m = -1;
        for (const Vertex& v : vertices)
            for (int k = 0; k < 4; ++k)
                if (v.weights[k] > 0.0f && int(v.joints[k]) > m) m = int(v.joints[k]);
        return m + 1;
    }

    void computeBounds()
    {
        if (vertices.isEmpty()) { for (int i=0;i<3;++i) bboxMin[i] = bboxMax[i] = 0; return; }
        bboxMin[0]=bboxMax[0]=vertices[0].px; bboxMin[1]=bboxMax[1]=vertices[0].py; bboxMin[2]=bboxMax[2]=vertices[0].pz;
        for (const Vertex& v : vertices) {
            const float p[3] = {v.px, v.py, v.pz};
            for (int i = 0; i < 3; ++i) { if (p[i] < bboxMin[i]) bboxMin[i] = p[i]; if (p[i] > bboxMax[i]) bboxMax[i] = p[i]; }
        }
    }

    // Apply a frame conversion to every position, normal and tangent, once, at ingest.
    void applyFrame(const rm::Mat4& m)
    {
        if (m == rm::kIdentityFrame) { computeBounds(); return; }   // still the bounds pass
        for (Vertex& v : vertices) {
            float o[3];
            rm::transformPoint(m, v.px, v.py, v.pz, o); v.px=o[0]; v.py=o[1]; v.pz=o[2];
            rm::transformDir  (m, v.nx, v.ny, v.nz, o); v.nx=o[0]; v.ny=o[1]; v.nz=o[2];
            rm::transformDir  (m, v.tx, v.ty, v.tz, o); v.tx=o[0]; v.ty=o[1]; v.tz=o[2];
        }
        computeBounds();
    }
};

}  // namespace core
