#pragma once
// ── BF6 Frostbite storage layer (Qt-free engine) ─────────────────────────────────────────────────
//
// Reads Battlefield 6's superbundle TOCs and content store (cas_NN.cas) IN PLACE and decompresses a
// resource's bytes on demand — no dump. The format was recovered by decompiling the reshax toolkit's
// toc_bf6.exe (.NET, "toctest") and verified against the retail install; the full spec, including why
// the `.toc` is read big-endian after a fixed 556-byte header and needs no key, is in
// docs/BF6_STORAGE_FORMAT.md.
//
// This library is Qt-free on purpose (mirrors games/dr's dr_engine): the GUI store (Bf6Store) wraps
// it, and a console conformance tool (conformance/tools/bf6_toc_check.cpp) exercises it without Qt.
#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>

namespace bf6 {

enum class ResKind : uint8_t { Ebx = 0, Res = 1, Chunk = 2 };

// One entry of a superbundle: an EBX asset definition, a cooked RES resource, or a raw chunk.
// Its location is fully resolved to an absolute (cas file, offset, size) at parse time.
struct Resource {
    ResKind     kind = ResKind::Ebx;
    std::string name;            // ebx/res authored name; chunk = 32-hex GUID
    uint64_t    nameHash = 0;    // ebx/res 64-bit name hash (0 for chunks)
    uint32_t    typeHash = 0;    // res type hash (djb2 over types.txt); 0 otherwise
    std::string typeName;        // resolved type ("MeshSet", "DxTexture", …) or 8-hex fallback; "" for ebx/chunk
    int         persistentIndex = -1;  // install-bundle index (→ m_bundles path)
    int         casNo = -1;      // cas_NN file number
    int64_t     offset = 0;      // byte offset into the cas file
    int64_t     size = 0;        // compressed byte length in the cas (block container)
    int         bundleIndex = -1;      // which bundle of the superbundle it came from (provenance)
    std::string superbundle;     // "Win32/characters" (provenance / grouping)
};

class Toc {
public:
    // Load Data/layout.toc from the game's Data dir → the persistentIndex → installBundle map that
    // locates cas files. Must be called before loadSuperbundle.
    bool loadLayout(const std::string& dataDir, std::string* err);
    // Load types.txt (one type name per line) → type-hash map, so RES rows carry real type names.
    void loadTypes(const std::string& typesTxtPath);
    // Same, from the file's contents already in memory (the GUI ships types.txt in a Qt resource).
    void loadTypesData(const std::string& content);
    // Parse one superbundle .toc (absolute path, e.g. Data/Win32/characters.toc), appending its
    // resources. May be called for several superbundles into one Toc.
    bool loadSuperbundle(const std::string& tocPath, std::string* err);

    const std::vector<Resource>& resources() const { return m_res; }
    const std::unordered_map<int, std::string>& bundles() const { return m_bundles; }
    const std::string& dataDir() const { return m_dataDir; }

    // Extra cas roots searched (after m_dataDir) when a resource's install-bundle path doesn't exist
    // under the base Data dir — BF6's HD texture pack overlays the same bundle path (e.g.
    // "Win32/installation/detail/initialexperienceinstallpackage") under Update/<pkg>/Data roots.
    void addCasRoot(const std::string& root) { m_casRoots.push_back(root); }

    // Absolute path of the cas file a resource lives in (first candidate root where the file exists).
    std::string casPath(const Resource& r) const;
    // Read a resource's full payload: seek its cas, read `size` bytes, decompress the Frostbite block
    // container (raw / zstd / Oodle). When several roots carry the same bundle path, picks the one whose
    // bytes at the offset are a valid block (so the HD overlay resolves correctly). Returns false + *err.
    bool readResource(const Resource& r, std::vector<uint8_t>& out, std::string* err) const;

    size_t countKind(ResKind k) const;

private:
    std::string m_dataDir;
    std::vector<std::string> m_casRoots;                  // extra roots for the HD/patch overlay (see addCasRoot)
    std::unordered_map<int, std::string>      m_bundles;  // persistentIndex -> installBundle ("Win32/installation/commonbase")
    std::unordered_map<uint32_t, std::string> m_types;    // type hash -> type name
    std::vector<Resource> m_res;
};

// Decompress a Frostbite block container: a sequence of
//   [u32 BE decompressedSize][u32 BE (compType<<24 | compSize&0xFFFFF)][compSize bytes]
// blocks, compType 0=raw, 0x0F=zstd, 0x11=Oodle. Concatenates block outputs into `out`.
bool decompressBlocks(const uint8_t* data, size_t len, std::vector<uint8_t>& out, std::string* err);

// Load Oodle (oo2core_9_win64.dll) for 0x11 blocks. Pass the DLL's absolute path (it ships with the
// game and the toolkit). Safe to call once; a no-op if already loaded. Without it, Oodle blocks fail
// with a clear error while raw/zstd still work.
bool initOodle(const std::string& dllPath, std::string* err);
bool oodleReady();

// djb2 type hash used by the TOC (matches toctest: h=5381; h = h*33 ^ lowercase(c)).
uint32_t typeHash(const std::string& typeName);

}  // namespace bf6
