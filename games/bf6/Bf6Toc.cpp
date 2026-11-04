#include "Bf6Toc.h"

#include <cstring>
#include <fstream>

#include <zstd.h>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#endif

namespace bf6 {

// ── little in-memory byte cursors ────────────────────────────────────────────────────────────────
namespace {

static bool readWholeFile(const std::string& path, std::vector<uint8_t>& out)
{
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return false;
    const std::streamoff n = f.tellg();
    if (n < 0) return false;
    out.resize(static_cast<size_t>(n));
    f.seekg(0);
    if (n > 0) f.read(reinterpret_cast<char*>(out.data()), n);
    return static_cast<bool>(f);
}

// Big-endian reader over an in-memory buffer (the .toc is big-endian after the 556-byte header).
struct BE {
    const uint8_t* p; size_t n; size_t at = 0;
    BE(const uint8_t* d, size_t len) : p(d), n(len) {}
    bool ok(size_t need) const { return at + need <= n; }
    uint16_t u16() { uint16_t v = (uint16_t(p[at]) << 8) | p[at + 1]; at += 2; return v; }
    int32_t  i32() { uint32_t v = (uint32_t(p[at]) << 24) | (uint32_t(p[at + 1]) << 16) | (uint32_t(p[at + 2]) << 8) | p[at + 3]; at += 4; return int32_t(v); }
    uint32_t u32() { return uint32_t(i32()); }
};

// Little-endian cursor over an in-memory buffer (the bundle meta blob is little-endian).
struct LE {
    const uint8_t* p; size_t n; size_t at = 0;
    LE(const uint8_t* d, size_t len) : p(d), n(len) {}
    bool ok(size_t need) const { return at + need <= n; }
    uint32_t u32() { uint32_t v; std::memcpy(&v, p + at, 4); at += 4; return v; }
    int32_t  i32() { return int32_t(u32()); }
    uint64_t u64() { uint64_t v; std::memcpy(&v, p + at, 8); at += 8; return v; }
    void     skip(size_t k) { at += k; }
};

// Frostbite varint (little, 7 bits per byte, up to 4 bytes) over a byte buffer at *pos.
static int readVarInt(const uint8_t* p, size_t n, size_t& pos)
{
    int val = 0; int b;
    b = (pos < n) ? p[pos++] : 0; val = b & 0x7F;
    if (b > 127) { b = (pos < n) ? p[pos++] : 0; val += (b & 0x7F) << 7;
    if (b > 127) { b = (pos < n) ? p[pos++] : 0; val += (b & 0x7F) << 14;
    if (b > 127) { b = (pos < n) ? p[pos++] : 0; val += (b & 0x7F) << 21; } } }
    return val;
}

static std::string readCString(const uint8_t* p, size_t n, size_t pos)
{
    std::string s;
    while (pos < n && p[pos] != 0) s.push_back(char(p[pos++]));
    return s;
}

static std::string readCStringAdvance(const uint8_t* p, size_t n, size_t& pos)
{
    std::string s;
    while (pos < n && p[pos] != 0) s.push_back(char(p[pos++]));
    if (pos < n) ++pos;  // skip null
    return s;
}

constexpr size_t kTocHeaderSkip = 556;        // 0x22C — the ignored signature header
constexpr uint32_t kBundleMetaMagic = 0x9D798ED6u;

static std::string relCas(const std::string& bundle, int casNo)
{
    char nn[8]; std::snprintf(nn, sizeof nn, "%02d", casNo);
    return "/" + bundle + "/cas_" + std::string(nn) + ".cas";
}
static bool fileExists(const std::string& p) { std::ifstream f(p, std::ios::binary); return bool(f); }

}  // namespace

// ── Oodle (dynamic, no import lib) ───────────────────────────────────────────────────────────────
namespace {
#ifdef _WIN32
using OodleLZ_Decompress_t = int64_t(*)(const void* src, int64_t srcLen, void* dst, int64_t dstLen,
                                        int32_t fuzzSafe, int32_t checkCRC, int32_t verbosity,
                                        void* decBufBase, int64_t decBufSize, void* fpCallback,
                                        void* callbackUserData, void* decoderMemory, int64_t decoderMemorySize,
                                        int32_t threadPhase);
static HMODULE          g_oodle = nullptr;
static OodleLZ_Decompress_t g_oodleDecompress = nullptr;
#endif
}  // namespace

bool initOodle(const std::string& dllPath, std::string* err)
{
#ifdef _WIN32
    if (g_oodleDecompress) return true;
    g_oodle = LoadLibraryA(dllPath.c_str());
    if (!g_oodle) { if (err) *err = "could not load Oodle: " + dllPath; return false; }
    g_oodleDecompress = reinterpret_cast<OodleLZ_Decompress_t>(GetProcAddress(g_oodle, "OodleLZ_Decompress"));
    if (!g_oodleDecompress) { if (err) *err = "OodleLZ_Decompress not found in " + dllPath; return false; }
    return true;
#else
    (void)dllPath; if (err) *err = "Oodle only supported on Windows"; return false;
#endif
}

bool oodleReady()
{
#ifdef _WIN32
    return g_oodleDecompress != nullptr;
#else
    return false;
#endif
}

uint32_t typeHash(const std::string& typeName)
{
    uint32_t h = 5381u;
    for (char c : typeName) {
        char lc = (c >= 'A' && c <= 'Z') ? char(c + 32) : c;
        h = (h * 33u) ^ uint32_t(uint8_t(lc));
    }
    return h;
}

bool decompressBlocks(const uint8_t* data, size_t len, std::vector<uint8_t>& out, std::string* err)
{
    out.clear();
    size_t pos = 0;
    while (pos + 8 <= len) {
        uint32_t decompSize = (uint32_t(data[pos]) << 24) | (uint32_t(data[pos + 1]) << 16) | (uint32_t(data[pos + 2]) << 8) | data[pos + 3];
        uint32_t packed     = (uint32_t(data[pos + 4]) << 24) | (uint32_t(data[pos + 5]) << 16) | (uint32_t(data[pos + 6]) << 8) | data[pos + 7];
        pos += 8;
        const uint32_t compType = packed >> 24;
        const uint32_t compSize = packed & 0xFFFFF;
        if (pos + compSize > len) { if (err) *err = "block overruns resource"; return false; }
        const uint8_t* src = data + pos;
        const size_t base = out.size();
        switch (compType) {
        case 0x00:  // stored / raw — output = compSize bytes verbatim
            out.insert(out.end(), src, src + compSize);
            break;
        case 0x0F: {  // zstd
            out.resize(base + decompSize);
            size_t got = ZSTD_decompress(out.data() + base, decompSize, src, compSize);
            if (ZSTD_isError(got)) { if (err) *err = std::string("zstd: ") + ZSTD_getErrorName(got); return false; }
            out.resize(base + got);
            break;
        }
        case 0x11: {  // Oodle (Kraken)
#ifdef _WIN32
            if (!g_oodleDecompress) { if (err) *err = "Oodle block but Oodle not loaded (call initOodle)"; return false; }
            out.resize(base + decompSize);
            int64_t got = g_oodleDecompress(src, int64_t(compSize), out.data() + base, int64_t(decompSize),
                                            0, 0, 0, nullptr, 0, nullptr, nullptr, nullptr, 0, 3);
            if (got != int64_t(decompSize)) { if (err) *err = "Oodle decompress size mismatch"; return false; }
            break;
#else
            if (err) *err = "Oodle block unsupported on this platform"; return false;
#endif
        }
        default:
            if (err) *err = "unsupported compression type " + std::to_string(compType);
            return false;
        }
        pos += compSize;
    }
    return true;
}

// ── layout.toc → persistentIndex → installBundle ─────────────────────────────────────────────────
bool Toc::loadLayout(const std::string& dataDir, std::string* err)
{
    m_dataDir = dataDir;
    std::vector<uint8_t> buf;
    if (!readWholeFile(dataDir + "/layout.toc", buf)) { if (err) *err = "cannot read layout.toc"; return false; }
    if (buf.size() <= kTocHeaderSkip) { if (err) *err = "layout.toc too small"; return false; }

    const uint8_t* p = buf.data();
    const size_t   n = buf.size();
    size_t pos = kTocHeaderSkip;
    std::string curInstallBundle;   // last-seen installBundle string (paired with persistentIndex)

    while (pos < n) {
        int c = p[pos++];
        int flag = c >> 7;
        int type = c & 0x7F;
        std::string name;
        if (flag == 0 && c != 0) name = readCStringAdvance(p, n, pos);
        switch (type) {
        case 0: break;                              // end-of-object marker (flat scan continues)
        case 1: readVarInt(p, n, pos); break;       // array (count)
        case 2: readVarInt(p, n, pos); break;       // object (byte size)
        case 6: pos += 1; break;                    // bool
        case 7: {                                   // string: varint len (incl null), then bytes
            int ln = readVarInt(p, n, pos);
            std::string s = (ln > 0) ? readCString(p, n, pos) : std::string();
            pos += (ln > 0) ? size_t(ln) : 0;
            if (name == "installBundle") curInstallBundle = s;
            break;
        }
        case 8: {                                   // int32
            if (pos + 4 > n) { pos = n; break; }
            int32_t v; std::memcpy(&v, p + pos, 4); pos += 4;
            if (name == "persistentIndex") m_bundles[v] = curInstallBundle;
            break;
        }
        case 9:  pos += 8; break;                   // int64
        case 10: readVarInt(p, n, pos); break;      // varint
        case 11: pos += 4; break;                   // float
        case 12: pos += 8; break;                   // double
        case 15: pos += 16; break;                  // GUID
        case 16: pos += 20; break;                  // SHA1
        case 19:
        case 20: { int ln = readVarInt(p, n, pos); pos += size_t(ln < 0 ? 0 : ln); break; }  // blob
        default:
            // Unknown control byte — the flat format has no recovery; stop rather than mis-read.
            pos = n;
            break;
        }
    }
    if (m_bundles.empty()) { if (err) *err = "layout.toc parsed but no install bundles found"; return false; }
    return true;
}

void Toc::loadTypes(const std::string& typesTxtPath)
{
    std::ifstream f(typesTxtPath);
    std::string line;
    while (std::getline(f, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ')) line.pop_back();
        if (line.empty()) continue;
        m_types[typeHash(line)] = line;
    }
}

void Toc::loadTypesData(const std::string& content)
{
    size_t i = 0;
    while (i < content.size()) {
        size_t e = content.find('\n', i);
        if (e == std::string::npos) e = content.size();
        std::string line = content.substr(i, e - i);
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ')) line.pop_back();
        if (!line.empty()) m_types[typeHash(line)] = line;
        i = e + 1;
    }
}

// ── superbundle .toc → resources ─────────────────────────────────────────────────────────────────
bool Toc::loadSuperbundle(const std::string& tocPath, std::string* err)
{
    std::vector<uint8_t> t;
    if (!readWholeFile(tocPath, t)) { if (err) *err = "cannot read " + tocPath; return false; }
    if (t.size() <= kTocHeaderSkip + 60) { if (err) *err = "superbundle toc too small"; return false; }

    // superbundle display name = the file stem ("Win32/characters" style provenance)
    std::string sbName = tocPath;
    { size_t s = sbName.find_last_of("/\\"); if (s != std::string::npos) sbName = sbName.substr(s + 1);
      size_t d = sbName.find_last_of('.'); if (d != std::string::npos) sbName = sbName.substr(0, d);
      sbName = "Win32/" + sbName; }

    const uint8_t* p = t.data();
    const size_t   n = t.size();

    // header (big-endian, from 556)
    BE h(p, n); h.at = kTocHeaderSkip;
    h.i32();                              // unknown
    int32_t bundleTableOff    = h.i32();
    int32_t bundleCount       = h.i32();
    h.i32();                              // unknown
    int32_t chunkGuidTableOff = h.i32();
    int32_t chunkCount        = h.i32();
    int32_t chunkCasTableOff  = h.i32();
    // (8 more int32 header fields unused for per-bundle resources)

    for (int j = 0; j < bundleCount; ++j) {
        const size_t e = kTocHeaderSkip + size_t(bundleTableOff) + size_t(j) * 16;
        if (e + 16 > n) break;
        BE be(p, n); be.at = e + 12;      // 4th int32 of the 16-byte entry = block offset
        int32_t blockOff = be.i32();
        const size_t b = kTocHeaderSkip + size_t(blockOff);
        if (b + 20 > n) continue;
        BE bb(p, n); bb.at = b;
        bb.i32(); bb.i32();               // expected 0, 0
        int32_t flagsOff   = bb.i32();
        int32_t flagsCount = bb.i32();
        int32_t metaRefOff = bb.i32();

        const size_t flagsPos = b + size_t(flagsOff);
        if (flagsPos + size_t(flagsCount) > n || flagsCount < 1) continue;
        const uint8_t* flags = p + flagsPos;

        // Cas-reference table cursor (big-endian), consumed in order: meta, ebx…, res…, chunk…
        size_t refPos = b + size_t(metaRefOff);
        int    curPI = -1, curCas = -1;
        int    flagIdx = 0;

        auto readRef = [&](int64_t& off, int64_t& size) -> bool {
            if (flagIdx >= flagsCount) return false;
            int flag = flags[flagIdx++];
            BE r(p, n); r.at = refPos;
            if (flag > 0) {
                if (!r.ok(12 + 8)) return false;
                r.u16();                       // unknown
                curPI  = r.i32();
                curCas = r.u16();
            } else {
                if (!r.ok(8)) return false;
            }
            off  = int64_t(r.u32());
            size = int64_t(r.u32());
            refPos = r.at;
            return true;
        };

        // ref[0] = the bundle meta blob (stored raw in a cas: [u32 BE size][size bytes])
        int64_t metaOff = 0, metaSize = 0;
        if (!readRef(metaOff, metaSize)) continue;
        std::string rerr;
        std::vector<uint8_t> metaRaw;
        {
            // read metaSize bytes from the meta's cas
            Resource tmp; tmp.persistentIndex = curPI; tmp.casNo = curCas; tmp.offset = metaOff; tmp.size = metaSize;
            const std::string cp = casPath(tmp);
            std::ifstream cf(cp, std::ios::binary);
            if (!cf) continue;
            cf.seekg(metaOff);
            metaRaw.resize(size_t(metaSize));
            cf.read(reinterpret_cast<char*>(metaRaw.data()), metaSize);
            if (!cf) continue;
        }
        if (metaRaw.size() < 8) continue;
        // [u32 BE declared size][meta…]
        uint32_t declared = (uint32_t(metaRaw[0]) << 24) | (uint32_t(metaRaw[1]) << 16) | (uint32_t(metaRaw[2]) << 8) | metaRaw[3];
        if (declared + 4u != metaRaw.size()) continue;   // header size wrong — skip this bundle
        const uint8_t* meta = metaRaw.data() + 4;
        const size_t   mn   = declared;

        LE le(meta, mn);
        if (!le.ok(32)) continue;
        uint32_t magic = le.u32();
        if (magic != kBundleMetaMagic) continue;
        int32_t total      = le.i32();
        int32_t ebxCount   = le.i32();
        int32_t resCount   = le.i32();
        int32_t chunkCount = le.i32();
        int32_t strBase    = le.i32();
        le.skip(8);
        if (ebxCount < 0 || resCount < 0 || chunkCount < 0 || total < 0) continue;

        std::vector<uint64_t> hashes(static_cast<size_t>(total));
        for (int i = 0; i < total; ++i) { if (!le.ok(20)) { le.at = mn; break; } hashes[i] = le.u64(); le.skip(12); }
        std::vector<int32_t> nameOff(size_t(ebxCount + resCount));
        for (int i = 0; i < ebxCount + resCount; ++i) { if (!le.ok(8)) { le.at = mn; break; } nameOff[i] = le.i32(); le.i32(); }
        std::vector<uint32_t> resType(static_cast<size_t>(resCount));
        for (int i = 0; i < resCount; ++i) { if (!le.ok(4)) break; resType[i] = le.u32(); }
        for (int i = 0; i < resCount; ++i) { if (!le.ok(16)) break; le.skip(16); }
        for (int i = 0; i < resCount; ++i) { if (!le.ok(8)) break; le.skip(8); }
        const size_t chunkCursor = le.at;

        // EBX
        for (int i = 0; i < ebxCount; ++i) {
            int64_t off = 0, size = 0; if (!readRef(off, size)) break;
            Resource r;
            r.kind = ResKind::Ebx;
            r.nameHash = (i < total) ? hashes[size_t(i)] : 0;
            size_t no = size_t(strBase) + size_t(nameOff[size_t(i)]);
            r.name = (no < mn) ? readCString(meta, mn, no) : std::string();
            r.persistentIndex = curPI; r.casNo = curCas; r.offset = off; r.size = size;
            r.bundleIndex = j; r.superbundle = sbName;
            m_res.push_back(std::move(r));
        }
        // RES
        for (int i = 0; i < resCount; ++i) {
            int64_t off = 0, size = 0; if (!readRef(off, size)) break;
            Resource r;
            r.kind = ResKind::Res;
            r.nameHash = (ebxCount + i < total) ? hashes[size_t(ebxCount + i)] : 0;
            r.typeHash = resType[size_t(i)];
            auto it = m_types.find(r.typeHash);
            if (it != m_types.end()) r.typeName = it->second;
            else { char hx[9]; std::snprintf(hx, sizeof hx, "%08X", r.typeHash); r.typeName = hx; }
            size_t no = size_t(strBase) + size_t(nameOff[size_t(ebxCount + i)]);
            r.name = (no < mn) ? readCString(meta, mn, no) : std::string();
            r.persistentIndex = curPI; r.casNo = curCas; r.offset = off; r.size = size;
            r.bundleIndex = j; r.superbundle = sbName;
            m_res.push_back(std::move(r));
        }
        // chunks (GUIDs live in the meta from chunkCursor; each still consumes a cas ref)
        LE lc(meta, mn); lc.at = chunkCursor;
        for (int i = 0; i < chunkCount; ++i) {
            int64_t off = 0, size = 0; if (!readRef(off, size)) break;
            std::string guid;
            if (lc.ok(16 + 8)) {
                char hx[3];
                for (int k = 0; k < 16; ++k) { std::snprintf(hx, sizeof hx, "%02X", meta[lc.at + size_t(k)]); guid += hx; }
                lc.skip(16); lc.i32(); lc.i32();
            }
            Resource r;
            r.kind = ResKind::Chunk;
            r.name = guid;
            r.persistentIndex = curPI; r.casNo = curCas; r.offset = off; r.size = size;
            r.bundleIndex = j; r.superbundle = sbName;
            m_res.push_back(std::move(r));
        }
    }

    // Superbundle-global chunk store: the vertex/index/pixel payloads that RES entries reference (the
    // per-bundle lists above don't contain them). Entry = 16-byte GUID + big-endian int32 index at
    // `chunkGuidTableOff`; the chunk's cas ref sits at `chunkCasTableOff + (index & 0xFFFFFF)*4`. The
    // GUID bytes are stored REVERSED here (toc_bf6 prepends), so read them high-to-low to get the same
    // canonical forward GUID that RES/MeshSet references and per-bundle chunks use.
    for (int j = 0; j < chunkCount; ++j) {
        const size_t ge = kTocHeaderSkip + size_t(chunkGuidTableOff) + size_t(j) * 20;
        if (ge + 20 > n) break;
        std::string guid;
        { char hx[3]; for (int k = 15; k >= 0; --k) { std::snprintf(hx, sizeof hx, "%02X", p[ge + size_t(k)]); guid += hx; } }
        BE gi(p, n); gi.at = ge + 16;
        const uint32_t index = gi.u32() & 0xFFFFFFu;
        const size_t refPos = kTocHeaderSkip + size_t(chunkCasTableOff) + size_t(index) * 4;
        if (refPos + 16 > n) continue;
        BE r(p, n); r.at = refPos;
        r.u16();
        const int pi  = r.i32();
        const int cas = r.u16();
        const int64_t off  = int64_t(r.u32());
        const int64_t size = int64_t(r.u32());
        Resource rr;
        rr.kind = ResKind::Chunk;
        rr.name = guid;
        rr.persistentIndex = pi; rr.casNo = cas; rr.offset = off; rr.size = size;
        rr.bundleIndex = -1; rr.superbundle = sbName;
        m_res.push_back(std::move(rr));
    }

    (void)err;
    return true;
}

std::string Toc::casPath(const Resource& r) const
{
    auto it = m_bundles.find(r.persistentIndex);
    const std::string bundle = (it != m_bundles.end()) ? it->second : std::string();
    const std::string rel = relCas(bundle, r.casNo);
    if (fileExists(m_dataDir + rel)) return m_dataDir + rel;
    for (const auto& root : m_casRoots) if (fileExists(root + rel)) return root + rel;
    return m_dataDir + rel;   // fallback path (for error messages when nothing exists)
}

bool Toc::readResource(const Resource& r, std::vector<uint8_t>& out, std::string* err) const
{
    auto it = m_bundles.find(r.persistentIndex);
    const std::string bundle = (it != m_bundles.end()) ? it->second : std::string();
    const std::string rel = relCas(bundle, r.casNo);

    std::vector<std::string> roots; roots.reserve(1 + m_casRoots.size());
    roots.push_back(m_dataDir);
    for (const auto& x : m_casRoots) roots.push_back(x);

    // The HD texture pack overlays the same install-bundle path under an Update root, so the same
    // (bundle, cas, offset) can name files in several roots. Validate the Frostbite block header at the
    // offset and use the first root whose bytes are real (raw/zstd/Oodle), falling back to the first
    // existing file if none validates (don't turn a readable base chunk into a false miss).
    std::string firstExisting;
    std::string lastErr = "no cas root holds " + rel;
    for (const std::string& root : roots) {
        const std::string cp = root + rel;
        std::ifstream cf(cp, std::ios::binary);
        if (!cf) continue;
        if (firstExisting.empty()) firstExisting = cp;
        uint8_t hdr[8];
        cf.seekg(r.offset);
        cf.read(reinterpret_cast<char*>(hdr), 8);
        if (!cf) { lastErr = "short read (header) from " + cp; continue; }
        const uint32_t compType = ((uint32_t(hdr[4]) << 24) | (uint32_t(hdr[5]) << 16) | (uint32_t(hdr[6]) << 8) | hdr[7]) >> 24;
        if (compType != 0x00 && compType != 0x0F && compType != 0x11) continue;   // wrong overlay root
        cf.seekg(r.offset);
        std::vector<uint8_t> raw(size_t(r.size));
        cf.read(reinterpret_cast<char*>(raw.data()), r.size);
        if (!cf) { lastErr = "short read from " + cp; continue; }
        return decompressBlocks(raw.data(), raw.size(), out, err);
    }
    if (!firstExisting.empty()) {
        std::ifstream cf(firstExisting, std::ios::binary);
        if (cf) {
            cf.seekg(r.offset);
            std::vector<uint8_t> raw(size_t(r.size));
            cf.read(reinterpret_cast<char*>(raw.data()), r.size);
            if (cf) return decompressBlocks(raw.data(), raw.size(), out, err);
        }
    }
    if (err) *err = lastErr;
    return false;
}

size_t Toc::countKind(ResKind k) const
{
    size_t c = 0;
    for (const auto& r : m_res) if (r.kind == k) ++c;
    return c;
}

}  // namespace bf6
