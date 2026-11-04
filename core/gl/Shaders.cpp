#include "core/gl/Shaders.h"

namespace core::gl {
namespace {

// ── Vertex ──────────────────────────────────────────────────────────────────────────────────────
// Ported from D4 unchanged in what it computes: the fur and FX displacement moved into the
// @@VERT_DISPLACE@@ block (D4's extension supplies them). The silhouette jitter stays: the stencil
// outline is core.
const char* kVert = R"(#version 450 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;
layout(location = 3) in vec3 aTangent;
uniform mat4 uMVP;
uniform mat4 uModel;
// Selection silhouette: shifts the projected vertex by a whole number of screen pixels so the
// outline has CONSTANT width. Multiplying by w cancels the perspective divide, so the offset is
// in NDC/screen space, not model space — unlike a normal-push hull it cannot tear on the split
// normals game meshes carry at UV seams.
uniform vec2  uNdcOffset;
@@VERT_UNIFORMS@@
out vec3 vNormal;
out vec2 vUV;
out vec3 vTangent;
out vec3 vWorldPos;
void main() {
    vNormal = mat3(uModel) * aNormal;
    vTangent = mat3(uModel) * aTangent;
    vUV = aUV;
    vec3 pos = aPos;
@@VERT_DISPLACE@@
    vWorldPos = vec3(uModel * vec4(pos, 1.0));
    gl_Position = uMVP * vec4(pos, 1.0);
    gl_Position.xy += uNdcOffset * gl_Position.w;   // silhouette jitter (zero for every other pass)
}
)";

// ── Fragment ────────────────────────────────────────────────────────────────────────────────────
// D4's PBR path with the Diablo-specific material code lifted out to the blocks. What stays is
// engine-neutral: Cook-Torrance with multiscatter compensation, the three-light camera rig, key
// shadow (PCF), SSAO, analytic hemisphere IBL with an optional prefiltered cubemap, spec AA, ACES
// + the optional grade (stylised or a 256x16 LUT), the alpha cutout as coverage, and the debug
// channels 1-6. Two things are new against D4 and both are the POE2 graft: uAlphaMode (0 opaque,
// 1 mask, 2 blend, 3 additive — D4 cut EVERY textured part at 0.35) and uAlphaCutoff (D4's 0.35,
// with hair's 0.16 now the extension's business via @@FRAG_CUTOUT@@).
const char* kFragA = R"(#version 450 core
in vec3 vNormal;
in vec2 vUV;
in vec3 vTangent;
in vec3 vWorldPos;
out vec4 FragColor;
uniform int  uSolid;        // 1 = emit uBase flat and skip all lighting (selection silhouette)
uniform vec3 uLightDir;     // key light direction (world), surface -> light
uniform vec3 uViewPos;      // camera world position
uniform vec3 uBase;
uniform sampler2D uTex;        uniform int uHasTex;
uniform sampler2D uNormalTex;  uniform int uHasNormal;
uniform int   uPbr;
uniform float uMetal;
uniform float uRough;
uniform float uIor;         // KHR_materials_ior -> dielectric F0. <=1 (or unset=0) means "use the 1.5 default".
uniform sampler2D uOrmTex;     uniform int uHasOrm;       // AO(R)/rough(G)/metal(B)
uniform int   uOrmHasAo;                                  // 0 = the R channel is not occlusion
uniform sampler2D uEmissiveTex;uniform int uHasEmissive;
uniform float uEmisMul;   // material emissive strength (linear gain)
uniform float uEmisScale; // global emissive-intensity slider (tames tonemap whiteout)
uniform vec3  uEmisColor; // material emissive factor (the emissive texture is often a mask)
uniform sampler2D uSpecColorTex; uniform int uHasSpecColor;   // KHR_materials_specular F0 colour
uniform int   uAlphaMode;    // 0 opaque, 1 mask (cutout), 2 blend, 3 additive
uniform float uAlphaCutoff;  // mask threshold
// 1 = the draw target is multisampled, so alpha-to-coverage is doing the cutout anti-aliasing and
// fractional alpha is the coverage signal. 0 = single-sampled target (a supersampled capture FBO):
// coverage would collapse to a 1-bit, driver-dithered test, so the cutout becomes a hard discard
// and alpha stays 1.0, letting the downscale do the anti-aliasing.
uniform int   uA2C;
uniform int   uFSubsurf, uFIbl, uFTonemap;
uniform int   uSpecAA;        // geometric specular anti-aliasing toggle
uniform int   uViewChannel;   // 0 shaded 1 base 2 normal 3 rough 4 metal 5 AO 6 emissive
uniform float uExposure;
uniform int   uColorGrade;    // 1 = apply the post-tonemap colour grade (off by default)
uniform float uCgContrast;    // S-curve contrast around 0.5 pivot (~1.05)
uniform float uCgSat;         // saturation (~1.10)
uniform float uCgWarmth;      // split-tone: warm shadows / cool highlights (~0.03)
uniform sampler2D uLut;       // colour-grade LUT (16^3 unwrapped to 256x16), when loaded
uniform int   uHasLut;        // 1 = a valid LUT is bound -> use it instead of the stylised grade
uniform vec3  uEnvSky, uEnvHor, uEnvGnd;   // environment gradient
uniform vec3  uLightCol;                   // KEY light colour x intensity
uniform vec3  uRimDir,  uRimCol;           // cool back rim  (surface->light dir, colour x intensity)
uniform vec3  uFillDir, uFillCol;          // cool front fill (surface->light dir, colour x intensity)
uniform float uAmbScale;                   // hemisphere-ambient (IBL) multiplier
uniform samplerCube uReflCube;             // reflection probe (prefiltered HDR cubemap)
uniform int   uHasReflCube;                // 1 = sample uReflCube for ambient specular
uniform float uReflMaxMip;                 // top mip index of uReflCube (roughness -> mip)
uniform float uReflStrength;               // ambient-specular (reflection) intensity multiplier
uniform sampler2D uShadowMap;              // depth from the key light's POV (self-shadowing)
uniform mat4  uLightMVP;
uniform int   uShadowOn;
uniform float uShadowStr;                  // shadow darkness
uniform float uShadowSoft;                 // PCF radius in texels (softness)
uniform float uShadowBias;
uniform float uShadowNBias;                // world-space normal offset (slope-aware acne fix)
uniform float uShadowSize;                 // shadow-map resolution
uniform mat4  uMVP;                        // view-projection (model = identity) — projects SSAO samples
uniform sampler2D uPosTex;                 // SSAO G-buffer: world-space position (xyz), a=1 geometry
uniform int   uSsaoOn;
uniform float uSsaoStr;                    // SSAO darkness multiplier
uniform float uSsaoRad;                    // SSAO sample radius (world units)
uniform float uTime;
@@FRAG_UNIFORMS@@
const float PI = 3.14159265359;

float distGGX(float NoH, float a) { float a2=a*a; float d=NoH*NoH*(a2-1.0)+1.0; return a2/max(PI*d*d,1e-7); }
float gSchlick(float x, float k) { return x/(x*(1.0-k)+k); }
float gSmith(float NoV, float NoL, float r) { float k=(r+1.0); k=k*k/8.0; return gSchlick(NoV,k)*gSchlick(NoL,k); }
vec3 fres(float c, vec3 F0) { return F0 + (1.0-F0)*pow(clamp(1.0-c,0.0,1.0),5.0); }
// Roughness-aware Fresnel for ambient reflection (so rough metals don't over-reflect at grazing).
vec3 fresRough(float c, vec3 F0, float r) {
    return F0 + (max(vec3(1.0-r), F0) - F0) * pow(clamp(1.0-c,0.0,1.0),5.0);
}
// Karis split-sum environment BRDF (the analytic "DFG" LUT) -> scale/bias for ambient specular.
vec2 envDFG(float NoV, float r) {
    const vec4 c0 = vec4(-1.0, -0.0275, -0.572, 0.022);
    const vec4 c1 = vec4( 1.0,  0.0425,  1.040, -0.04);
    vec4 rr = r * c0 + c1;
    float a004 = min(rr.x*rr.x, exp2(-9.28*NoV))*rr.x + rr.y;
    return vec2(-1.04, 1.04) * a004 + rr.zw;
}
// Lagarde specular occlusion: AO shouldn't fully kill grazing reflections.
float specOcclusion(float NoV, float ao, float r) {
    return clamp(pow(NoV + ao, exp2(-16.0*r - 1.0)) - 1.0 + ao, 0.0, 1.0);
}
// One directional rig light (key/rim/fill): energy-conserving Cook-Torrance with the same
// multiscatter compensation as the key. specMul lets an extension damp the hard highlight.
vec3 shadeLight(vec3 N, vec3 V, vec3 Ldir, vec3 lcol, vec3 albedo, vec3 F0,
                float rough, float metal, float specMul) {
    if (dot(lcol, lcol) < 1e-8) return vec3(0.0);
    vec3 L = normalize(Ldir);
    float NoL = max(dot(N, L), 0.0);
    if (NoL <= 0.0) return vec3(0.0);
    vec3 H = normalize(V + L);
    float NoV = max(dot(N, V), 1e-4), NoH = max(dot(N, H), 0.0), VoH = max(dot(V, H), 0.0);
    float D = distGGX(NoH, rough*rough);
    float G = gSmith(NoV, NoL, rough);
    vec3  F = fres(VoH, F0);
    vec3 spec = (D*G*F)/max(4.0*NoV*NoL, 1e-4);
    vec2 dfg = envDFG(NoV, rough);
    spec *= 1.0 + F0 * (1.0/max(dfg.x + dfg.y, 1e-3) - 1.0);
    spec *= specMul;
    vec3 kd = (vec3(1.0) - F)*(1.0 - clamp(metal, 0.0, 1.0));
    return (kd*albedo/PI + spec) * lcol * NoL;
}
// Key-light shadow: project the world position into the light's depth map and PCF-compare.
// Returns a lit factor 1..(1-strength). 1 = fully lit; lower = in shadow.
float keyShadow(vec3 worldPos, vec3 nrm) {
    if (uShadowOn == 0) return 1.0;
    vec4 lp = uLightMVP * vec4(worldPos + nrm * uShadowNBias, 1.0);
    vec3 p = lp.xyz / lp.w * 0.5 + 0.5;
    if (p.z > 1.0 || p.x < 0.0 || p.x > 1.0 || p.y < 0.0 || p.y > 1.0) return 1.0;   // outside the map
    float texel = 1.0 / max(uShadowSize, 1.0);
    float lit = 0.0;
    for (int y = -1; y <= 1; ++y)
    for (int x = -1; x <= 1; ++x) {
        float d = texture(uShadowMap, p.xy + vec2(x, y) * texel * uShadowSoft).r;
        lit += (p.z - uShadowBias <= d) ? 1.0 : 0.0;
    }
    lit /= 9.0;
    return 1.0 - (1.0 - lit) * clamp(uShadowStr, 0.0, 1.0);
}
// Screen-space ambient occlusion: sample a hemisphere of world-space offsets around the
// fragment, project each into the world-position G-buffer, and count how many land behind a
// nearer surface. Returns an ambient multiplier in 1..(1-strength).
float ssaoFactor(vec3 P, vec3 Nw) {
    if (uSsaoOn == 0) return 1.0;
    const vec3 kern[16] = vec3[16](
        vec3( 0.05,  0.02, 0.06), vec3(-0.11,  0.07, 0.10), vec3( 0.14, -0.09, 0.13),
        vec3(-0.07, -0.15, 0.17), vec3( 0.20,  0.13, 0.21), vec3(-0.22,  0.05, 0.25),
        vec3( 0.09, -0.24, 0.29), vec3(-0.28, -0.14, 0.33), vec3( 0.31,  0.22, 0.38),
        vec3(-0.12,  0.36, 0.42), vec3( 0.38, -0.20, 0.47), vec3(-0.40, -0.30, 0.52),
        vec3( 0.18,  0.48, 0.57), vec3(-0.52,  0.16, 0.62), vec3( 0.44, -0.44, 0.68),
        vec3(-0.30,  0.55, 0.75));
    float rnd = fract(sin(dot(gl_FragCoord.xy, vec2(12.9898, 78.233))) * 43758.5453);
    float ca = cos(rnd * 6.2831853), sa = sin(rnd * 6.2831853);
    vec3 up = abs(Nw.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    vec3 T = normalize(cross(up, Nw));
    vec3 B = cross(Nw, T);
    float occ = 0.0;
    for (int i = 0; i < 16; ++i) {
        vec3 k = kern[i];
        vec3 kr = vec3(k.x*ca - k.y*sa, k.x*sa + k.y*ca, k.z);   // rotate about the normal
        vec3 sPos = P + (T*kr.x + B*kr.y + Nw*kr.z) * uSsaoRad;
        vec4 clip = uMVP * vec4(sPos, 1.0);
        if (clip.w <= 0.0) continue;
        vec2 suv = clip.xy / clip.w * 0.5 + 0.5;
        if (any(lessThan(suv, vec2(0.0))) || any(greaterThan(suv, vec2(1.0)))) continue;
        vec4 sc = texture(uPosTex, suv);
        if (sc.a < 0.5) continue;                               // sample hit the background
        float sampleDist = distance(uViewPos, sPos);
        float sceneDist  = distance(uViewPos, sc.xyz);
        float rangeCheck = smoothstep(0.0, 1.0, uSsaoRad / max(abs(sampleDist - sceneDist), 1e-4));
        occ += (sceneDist < sampleDist - 0.02 * uSsaoRad) ? rangeCheck : 0.0;
    }
    return clamp(1.0 - (occ / 16.0) * uSsaoStr, 0.0, 1.0);
}
// Hemisphere environment: sky tint up, ground tint down, by world-up component.
vec3 envColor(vec3 dir) {
    float t = clamp(dir.y*0.5+0.5, 0.0, 1.0);
    return (t>0.5) ? mix(uEnvHor,uEnvSky,(t-0.5)*2.0) : mix(uEnvGnd,uEnvHor,t*2.0);
}
)";

const char* kFragB = R"(
void main() {
    // Flat unlit fill for the selection silhouette — the outline must be one solid colour, not a
    // shaded surface, or the ring reads as another lit copy of the mesh. The cutout is honoured
    // (hard in/out) so the outline follows the visible strands, not the card quad.
    if (uSolid==1) {
        if (uHasTex==1 && uAlphaMode==1 && texture(uTex, vUV).a < uAlphaCutoff) discard;
        FragColor = vec4(uBase, 1.0);
        return;
    }
    vec4 base = (uHasTex==1) ? texture(uTex, vUV) : vec4(uBase, 1.0);
@@FRAG_EARLY@@
    float cutCov = 1.0;                          // alpha-cutout coverage (1 = solid)
    if (uHasTex==1 && uAlphaMode==1) {
        // Smooth alpha test: turn the hard cutout into screen-space coverage so alpha-to-coverage
        // (MSAA) anti-aliases the edge instead of a jagged fringe. An extension may widen the band
        // (aaFloor) or lower the single-sample cut (hardCutBias) for wispy materials.
        float thr = uAlphaCutoff;
        float aaFloor = 0.0;
        float hardCutBias = 0.0;
@@FRAG_CUTOUT@@
        if (uA2C==0) {
            // Single-sampled target: a plain binary test. Fragments that survive are fully opaque,
            // so a saved PNG gets solid cutouts instead of a half-transparent ghost of them.
            if (base.a < thr - hardCutBias) discard;
        } else {
            float aa  = max(fwidth(base.a), 1e-4);
            aa = max(aa, aaFloor);
            cutCov = clamp((base.a - thr) / aa + 0.5, 0.0, 1.0);
            if (cutCov <= 0.0) discard;
        }
    }
    vec3 N = normalize(vNormal);
    // Smooth vertex-tangent basis — no per-pixel derivative artifacts (a full cotangent frame
    // speckled fur and drew black outlines along alpha / thin two-sided card edges). Handedness
    // is corrected for MIRRORED UV islands via the SIGN of the UV Jacobian determinant, which
    // still removes the dark normal-map seam along symmetry lines while keeping thin/alpha
    // geometry clean (only a sign is taken from the derivatives, never the frame direction).
    vec3 Tv = vTangent - N*dot(N, vTangent);
    if (dot(Tv, Tv) < 1e-8) Tv = cross(N, vec3(0.0, 1.0, 0.0));
    Tv = normalize(Tv);
    vec3 Bv = cross(N, Tv);
    bool flipBitangent = true;
@@FRAG_TBN@@
    if (flipBitangent) {
        vec2 du1 = dFdx(vUV), du2 = dFdy(vUV);
        if (du1.x*du2.y - du1.y*du2.x < 0.0) Bv = -Bv;   // mirrored UV island -> flip bitangent
    }
    mat3 tbn = mat3(Tv, Bv, N);
    vec3 T = Tv;
    vec3 B = Bv;
    if (uHasNormal==1) {
        vec2 nxy = texture(uNormalTex, vUV).xy*2.0-1.0;
        float nz = sqrt(max(0.0, 1.0-dot(nxy,nxy)));
        N = normalize(tbn * vec3(nxy, nz));
    }
    float metal=uMetal, rough=uRough, ao=1.0;
    if (uHasOrm==1) { vec3 o=texture(uOrmTex,vUV).rgb; if (uOrmHasAo==1) ao=o.r; rough=o.g*uRough; metal=o.b*uMetal; }
@@FRAG_SURFACE@@
    rough=clamp(rough,0.04,1.0);
    vec3 V = normalize(uViewPos - vWorldPos);
    // Two-sided: flip only genuine BACK-faces (winding), not front-faces whose detail-perturbed
    // normal happens to graze past the viewer — a dot(N,V)<0 test tips fine detail into dark
    // bands at glancing angles. gl_FrontFacing is view- and detail-independent.
    if (!gl_FrontFacing) N = -N;
    // Geometric specular anti-aliasing (Kaplanyan): where the shading normal changes fast across
    // a pixel (fine normal-map detail, curved grazing edges), widen roughness so the highlight
    // stops sparkling/shimmering when the model is zoomed or spinning.
    if (uSpecAA==1) {
        vec3 dNx = dFdx(N), dNy = dFdy(N);
        float normalVar = 0.5 * (dot(dNx, dNx) + dot(dNy, dNy));
        rough = clamp(sqrt(rough*rough + min(normalVar, 0.18)), 0.04, 1.0);
    }
    // Emissive: sampled and scaled in ONE place. The texture and the factor are sRGB-ENCODED;
    // uEmisMul (the material's strength) and uEmisScale (the slider) are LINEAR INTENSITIES.
    // Folding them together before the pow() raised the intensity to the 2.2 along with the
    // colour (an authored 10 became 158x) and every glowing gem tonemapped to paper.
    vec3 emis = vec3(0.0);
    if (uHasEmissive==1) {
        vec3 e = texture(uEmissiveTex,vUV).rgb * uEmisColor;   // the sRGB-encoded part
        if (uFTonemap!=0) e = pow(e, vec3(2.2));               // decode it, and only it
        emis = e * uEmisMul * uEmisScale;                      // linear gain, applied after
    }
    // Material-channel viewer (the "Color" dropdown): output one raw channel, unlit.
    if (uViewChannel > 0) {
        vec3 dbg;
        if      (uViewChannel == 1) dbg = base.rgb;        // Base Colour
        else if (uViewChannel == 2) dbg = N*0.5 + 0.5;     // Normal (final shading normal)
        else if (uViewChannel == 3) dbg = vec3(rough);     // Roughness
        else if (uViewChannel == 4) dbg = vec3(metal);     // Metallic
        else if (uViewChannel == 5) dbg = vec3(ao);        // Ambient occlusion
        else if (uViewChannel == 6) dbg = emis;            // Emissive
@@FRAG_CHANNELS@@
        else                        dbg = emis;
        FragColor = vec4(dbg, cutCov);
        return;
    }

    if (uPbr==0) {   // PBR off -> flat two-sided Lambert
        float d=max(dot(N, normalize(uLightDir)),0.0);
        float aOut = (uAlphaMode==2 || uAlphaMode==3) ? base.a : 1.0;
        FragColor=vec4(base.rgb*(0.2+0.8*d)+emis, aOut); return;
    }

    vec3 albedo=base.rgb;
    // sRGB-correct: when tonemapping, light in linear space (albedo/emissive are
    // sRGB-encoded textures) and re-encode at the end.
    if (uFTonemap!=0) albedo = pow(albedo, vec3(2.2));   // emis was decoded at its sample, above
@@FRAG_ALBEDO@@
    // Dielectric F0 from the material IOR (glTF KHR_materials_ior). Unset/<=1 keeps the 1.5 default
    // (F0 0.04) so every render path that doesn't bind uIor is unchanged; a real IOR shifts the base
    // reflectance (BF6 authors 1.45 -> ~0.033). Metals replace F0 with the albedo as usual.
    float f0d = (uIor > 1.0001) ? pow((uIor-1.0)/(uIor+1.0), 2.0) : 0.04;
    vec3 F0=mix(vec3(f0d), albedo, clamp(metal,0.0,1.0));
    // KHR_materials_specular: an authored F0 colour (spec-gloss families) replaces the dielectric
    // 0.04 for the non-metal part of the mix.
    if (uHasSpecColor==1) {
        vec3 sc = texture(uSpecColorTex, vUV).rgb;
        if (uFTonemap!=0) sc = pow(sc, vec3(2.2));
        F0 = mix(sc, albedo, clamp(metal,0.0,1.0));
    }
    vec3 L=normalize(uLightDir), H=normalize(V+L);
    float NoL=max(dot(N,L),0.0), NoV=max(dot(N,V),1e-4);
    float NoH=max(dot(N,H),0.0), VoH=max(dot(V,H),0.0);

    vec3 spec;
    {                                        // Cook-Torrance microfacet
        float D=distGGX(NoH, rough*rough);
        float G=gSmith(NoV,NoL,rough);
        vec3  F=fres(VoH,F0);
        spec=(D*G*F)/max(4.0*NoV*NoL,1e-4);
        // Multiscatter energy compensation: single-scatter GGX loses energy on rough/metal
        // surfaces (they look too dark); add back the missing inter-reflections (Fdez-Aguera).
        vec2 dfg=envDFG(NoV, rough);
        spec *= 1.0 + F0 * (1.0/max(dfg.x + dfg.y, 1e-3) - 1.0);
    }
    float specMul = 1.0;       // an extension may damp the rig lights' highlight (fur)
    vec3 diffNoL = vec3(NoL);  // an extension may wrap this (pre-integrated skin)
@@FRAG_SPEC@@
    vec3 kd=(vec3(1.0)-fres(VoH,F0))*(1.0-clamp(metal,0.0,1.0));
    vec3 lightColor=uLightCol;
    vec3 Lo=(kd*albedo/PI)*lightColor*diffNoL + spec*lightColor*NoL;
@@FRAG_LIGHTING@@
    // Self-shadow the whole key-light contribution; the rim/fill are separate lights and stay
    // unshadowed.
    Lo *= keyShadow(vWorldPos, N);

    // Secondary rig lights — cool back rim + cool front fill.
    Lo += shadeLight(N, V, uRimDir,  uRimCol,  albedo, F0, rough, metal, specMul);
    Lo += shadeLight(N, V, uFillDir, uFillCol, albedo, F0, rough, metal, specMul);

    // Ambient: analytic hemisphere IBL — split-sum specular (env-BRDF DFG) + diffuse irradiance,
    // with energy-correct Fresnel and specular occlusion. Diffuse is AO-darkened; specular uses
    // its own grazing-aware occlusion so reflections aren't flattened by cavity AO.
    vec3 ambDiff, ambSpec;
    if (uFIbl==1) {
        vec3 irr=envColor(N);
        vec3 R = reflect(-V, N);
        vec3 pref = (uHasReflCube==1) ? textureLod(uReflCube, R, rough*uReflMaxMip).rgb
                                      : mix(envColor(R), irr, rough);
        vec2 dfg=envDFG(NoV, rough);
        vec3 kdA=(vec3(1.0)-fresRough(NoV,F0,rough))*(1.0-clamp(metal,0.0,1.0));
        ambDiff = irr*albedo*kdA;
        ambSpec = pref*(F0*dfg.x + dfg.y) * specOcclusion(NoV, ao, rough);
    } else {
        ambDiff = albedo*(1.0-clamp(metal,0.0,1.0))*0.22;
        ambSpec = F0*0.18;
    }
@@FRAG_AMBIENT@@
    ambSpec *= uReflStrength;   // reflection-intensity slider
    float aoSS = ssaoFactor(vWorldPos, normalize(N));   // screen-space ambient occlusion
    // Emissive: a subtle slow pulse so glowing runes/gems read alive (preview only — export writes
    // the steady authored value). Only pulses where there's actual glow.
    vec3 emisP = emis * (0.90 + 0.10 * sin(uTime * 2.2));
    vec3 col = (ambDiff*ao*aoSS + ambSpec*aoSS)*uAmbScale + Lo*mix(1.0,ao,0.35) + emisP;

    col *= uExposure;                        // exposure always applies (key control of the rig)
    // Tonemap curve: 1 = ACES filmic (default). 2 = neutral (engine-style). RE of the Messiah post
    // chain (Screen/MainPS SPIR-V, tools/drshader.py) found NO analytic ACES/Hable tonemap constants
    // and NO in-shader sRGB encode anywhere — the engine writes to a hardware sRGB target. So mode 2
    // drops the filmic curve and just clamps before the sRGB encode, approximating the engine's
    // lighter look. Default stays ACES; opt in with AB_TONEMAP=2 for calibration against the game.
    if (uFTonemap!=0) {                      // sRGB-correct output path (ACES or neutral)
        if (uFTonemap==1)                    // ACES filmic
            col = clamp((col*(2.51*col+0.03))/(col*(2.43*col+0.59)+0.14), 0.0, 1.0);
        else                                 // neutral: clamp to the HW sRGB target's [0,1]
            col = clamp(col, 0.0, 1.0);
        // Optional colour grade (post-tonemap, pre-gamma): a real LUT when loaded, else a
        // stylised contrast S-curve, saturation, and a subtle split-tone. Off unless enabled.
        if (uColorGrade==1) {
            if (uHasLut==1) {
                // 16x16x16 unwrapped to a 256x16 strip (16 blue-tiles across). Bilinear in R/G
                // within a tile, linear across the two bracketing blue tiles.
                vec3 c = clamp(col, 0.0, 1.0);
                float b = c.b * 15.0; float b0 = floor(b); float b1 = min(b0 + 1.0, 15.0); float f = b - b0;
                vec2 uvA = vec2((b0*16.0 + c.r*15.0 + 0.5)/256.0, (c.g*15.0 + 0.5)/16.0);
                vec2 uvB = vec2((b1*16.0 + c.r*15.0 + 0.5)/256.0, (c.g*15.0 + 0.5)/16.0);
                col = mix(texture(uLut, uvA).rgb, texture(uLut, uvB).rgb, f);
            } else {
                col = clamp(mix(vec3(0.5), col, uCgContrast), 0.0, 1.0);     // contrast about mid-grey
                float l = dot(col, vec3(0.2126, 0.7152, 0.0722));           // luma
                col = clamp(mix(vec3(l), col, uCgSat), 0.0, 1.0);          // saturation
                col += vec3( 0.9, 0.35, -0.6) * uCgWarmth * (1.0 - l);      // warm the shadows
                col += vec3(-0.3, 0.0,   0.9) * uCgWarmth * l;              // cool the highlights
                col = clamp(col, 0.0, 1.0);
            }
        }
        col = pow(col, vec3(1.0/2.2));
    }
    // Output alpha: coverage for the cutout pass; the texture's alpha for the blended passes.
    float outAlpha = (uAlphaMode==2 || uAlphaMode==3) ? base.a : cutCov;
@@FRAG_FINAL@@
    FragColor=vec4(col, outAlpha);
}
)";

// Shadow depth pass: render the model from the key light's POV, writing depth only.
const char* kShadowVert = R"(#version 450 core
layout(location=0) in vec3 aPos;
uniform mat4 uLightMVP;
void main() { gl_Position = uLightMVP * vec4(aPos, 1.0); }
)";
const char* kShadowFrag = R"(#version 450 core
void main() {}
)";

// SSAO position prepass: render the model's world-space position into an RGBA16F G-buffer
// (xyz = world pos, a = 1 marks geometry vs cleared background) for screen-space AO.
const char* kPosVert = R"(#version 450 core
layout(location=0) in vec3 aPos;
uniform mat4 uMVP;
uniform mat4 uModel;
out vec3 vWorld;
void main() {
    vWorld = vec3(uModel * vec4(aPos, 1.0));
    gl_Position = uMVP * vec4(aPos, 1.0);
}
)";
const char* kPosFrag = R"(#version 450 core
in vec3 vWorld;
out vec4 oPos;
void main() { oPos = vec4(vWorld, 1.0); }
)";

// Studio-gradient backdrop: a vertical wash drawn as a single vertex-ID fullscreen triangle.
const char* kBgVert = R"(#version 450 core
out float vT;
void main() {
    vec2 p = vec2(gl_VertexID == 2 ? 3.0 : -1.0, gl_VertexID == 1 ? 3.0 : -1.0);
    vT = p.y * 0.5 + 0.5;
    gl_Position = vec4(p, 0.0, 1.0);
})";
const char* kBgFrag = R"(#version 450 core
in float vT; out vec4 o;
uniform vec3 uTop; uniform vec3 uBot; uniform float uA;
void main() { o = vec4(mix(uBot, uTop, clamp(vT, 0.0, 1.0)), uA); })";

QByteArray splice(QByteArray text, const char* marker, const QByteArray& block)
{
    // The marker line is replaced whole, so an empty block leaves no stray blank line and a
    // non-empty one lands at the marker's indentation.
    const int at = text.indexOf(marker);
    if (at < 0) return text;
    int lineStart = text.lastIndexOf('\n', at) + 1;
    int lineEnd = text.indexOf('\n', at);
    if (lineEnd < 0) lineEnd = text.size();
    return text.left(lineStart) + (block.isEmpty() ? QByteArray() : block + '\n') + text.mid(lineEnd + 1);
}

}  // namespace

ShaderSources assembleShaders(const ShaderBlocks& b)
{
    ShaderSources s;
    QByteArray v = kVert;
    v = splice(v, "@@VERT_UNIFORMS@@", b.vertUniforms);
    v = splice(v, "@@VERT_DISPLACE@@", b.vertDisplace);
    s.vert = v;

    QByteArray f = QByteArray(kFragA) + QByteArray(kFragB);
    f = splice(f, "@@FRAG_UNIFORMS@@", b.fragUniforms);
    f = splice(f, "@@FRAG_EARLY@@", b.fragEarly);
    f = splice(f, "@@FRAG_CUTOUT@@", b.fragCutout);
    f = splice(f, "@@FRAG_TBN@@", QByteArray());   // reserved: an extension that owns its TBN sign
    f = splice(f, "@@FRAG_SURFACE@@", b.fragSurface);
    f = splice(f, "@@FRAG_CHANNELS@@", b.fragChannels);
    f = splice(f, "@@FRAG_ALBEDO@@", b.fragAlbedo);
    f = splice(f, "@@FRAG_SPEC@@", b.fragSpec);
    f = splice(f, "@@FRAG_LIGHTING@@", b.fragLighting);
    f = splice(f, "@@FRAG_AMBIENT@@", b.fragAmbient);
    f = splice(f, "@@FRAG_FINAL@@", b.fragFinal);
    s.frag = f;

    s.shadowVert = kShadowVert; s.shadowFrag = kShadowFrag;
    s.posVert = kPosVert;       s.posFrag = kPosFrag;
    s.bgVert = kBgVert;         s.bgFrag = kBgFrag;
    return s;
}

ShaderSources assembleShaders(const ShaderBlocks& b, const QByteArray& vertexOverride, const QByteArray& fragmentOverride)
{
    ShaderSources s = assembleShaders(b);
    if (!vertexOverride.isEmpty())   s.vert = vertexOverride;
    if (!fragmentOverride.isEmpty()) s.frag = fragmentOverride;
    return s;
}

}  // namespace core::gl
