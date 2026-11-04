#pragma once
#include <QImage>
#include <QMap>
#include <QString>

// ── The engine-neutral material ─────────────────────────────────────────────────────────────────
// The glTF-facing subset all three agree on, plus POE2's extensions (geometry-export.md §4).
// Everything viewport-only — D4's dye/fur/FX/detail/hair/eye, FOX's SRM/TRM/layer/glass/MTM,
// POE2's subsurface/fur — stays in the plugin behind `engine`, because putting forty
// engine-specific fields in core is how core stops being core.
namespace core {

struct TextureImage {
    QImage  image;              // decoded RGBA
    QString sourcePath;         // [FOX *_Source] [POE2 .mat role path] [D4 texName/SNO]
    bool    srgb = true;
    bool    lossyOk = false;    // [FOX] JPEG permitted (opaque base maps only)

    bool isNull() const { return image.isNull(); }
};

struct Material {
    QString name;
    bool    doubleSided = false;

    enum AlphaMode { Opaque, Mask, Blend, Additive };   // glTF has no Additive → exporters write BLEND
    AlphaMode alphaMode = Opaque;
    float     alphaCutoff = 0.5f;                       // D4 0.35, FOX 0.35, POE2 0.5

    float metalFactor = 0.0f;
    float roughFactor = 1.0f;

    TextureImage baseColor;
    TextureImage normal;
    TextureImage orm;           // R = occlusion, G = roughness, B = metalness
    TextureImage emissive;
    bool  ormHasOcclusion = false;   // FOX's SRM has no AO channel — writing one would invent data
    float occlusionStrength = 1.0f;

    float emissiveFactor[3] = {1,1,1};
    float emissiveStrength = 1.0f;

    // KHR extensions POE2 authors today; an exporter emits each only when set.
    TextureImage specularColor;
    bool  dielectricSpec = false;    // KHR_materials_specular
    float transmission = 0.0f;       // KHR_materials_transmission
    float ior = 0.0f;                // KHR_materials_ior; 0 = unset (glTF default 1.5). BF6 authors 1.0.

    // The SOURCE's normal-map convention, so the conversion happens ONCE in core instead of under
    // three different names (D4 flipNormalGreen, FOX normalsGreenDown, POE2 reconstructNormalZ).
    bool normalGreenDown = false;
    bool normalNeedsZ    = false;

    // Opaque handle to the plugin's own material record, for viewport extensions. Core never
    // dereferences it; a plugin static_casts it back.
    const void* engine = nullptr;
};

// ── A look on one mesh's materials (Customize stage 2c) ─────────────────────────────────────────
// What a variation, dye, swatch or skin tone does to a mesh's materials, as the game states it.
// Core only carries it to the store (IGameStore::resolveLookedMaterials) and uses key() to cache
// the result; what the names mean is the game's. FOX: a .fv2's texture table, a layer swatch.
struct MaterialLook {
    // A source texture path, as the store names it (TextureImage::sourcePath) → its replacement.
    QMap<QString, QString> textures;
    // Inputs only the game's own material code and renderer read (FOX "layercolor" → a swatch
    // path, "skin" → a tone). Text, so a look is comparable and cacheable without core knowing it.
    QMap<QString, QString> engine;

    bool isEmpty() const { return textures.isEmpty() && engine.isEmpty(); }
    bool operator==(const MaterialLook& o) const { return textures == o.textures && engine == o.engine; }
    // Length-prefixed, so no key or value text can make two different looks read the same (the
    // first cut joined with '>' / '=' / newline: {"a=b":"c"} and {"a":"b=c"} collided — audit
    // 2026-09-25 — and the cache would serve one look's materials for the other).
    QString key() const
    {
        QString k;
        auto add = [&](QChar tag, const QString& a, const QString& b) {
            k += tag; k += QString::number(a.size()); k += QLatin1Char(':'); k += a;
            k += QString::number(b.size()); k += QLatin1Char(':'); k += b;
        };
        for (auto it = textures.constBegin(); it != textures.constEnd(); ++it) add(QLatin1Char('t'), it.key(), it.value());
        for (auto it = engine.constBegin(); it != engine.constEnd(); ++it) add(QLatin1Char('e'), it.key(), it.value());
        return k;
    }
};

}  // namespace core
