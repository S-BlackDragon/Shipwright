#include "StateBlob.h"
#include "FixedHeap.h"
#include "ResourceSlots.h"

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>
#endif

#include <zstd.h>

#include <ship/Context.h>
#include <ship/resource/ResourceManager.h>

#include "soh/ActorDB.h"
#include "soh/Enhancements/savestate_serialize.h"
#include "soh/Zmp/ZmpLog.h"
#include "soh/Zmp/Sim/ZmpSim.h"
#include "soh/Zmp/Sim/ZmpPlayers.h"

extern "C" {
#include <z64.h>
#include "variables.h"
#include "functions.h"
extern PlayState* gPlayState;
extern EffectContext sEffectContext;
extern Arena sZeldaArena;
void Play_Main(GameState* thisx);
}

#ifdef _WIN32
extern "C" IMAGE_DOS_HEADER __ImageBase;
#endif

// Static data of the game code, serialized by the functions SoH already maintains for its
// in-memory save states (soh/Enhancements/savestates.cpp keeps the same list).
#define ZMP_STATIC_SAVESTATES(X)                                                                                     \
    X(Matrix)                                                                                                        \
    X(Lights)                                                                                                        \
    X(DoorWarp1)                                                                                                     \
    X(MapMark)                                                                                                       \
    X(Camera)                                                                                                        \
    X(OnePointCutscene)                                                                                              \
    X(Environment)                                                                                                   \
    X(MapExp)                                                                                                        \
    X(AudioOcarina)                                                                                                  \
    X(MessagePAL) X(BgDdanKd) X(BgDodoago) X(BgHakaTrap) X(BgHidanRock) X(BgMenkuriEye) X(BgMoriHineri) X(BgPoEvent) \
        X(BgRelayObjects) X(BgSpot18Basket) X(BossGanon) X(BossGanon2) X(BossMo) X(BossSst) X(BossTw) X(BossVa)      \
            X(Demo6k) X(DemoDu) X(DemoKekkai) X(EnBw) X(EnClearTag) X(EnFr) X(EnGoma) X(EnInsect) X(EnIshi) X(EnNiw) \
                X(EnPoField) X(EnTakaraMan) X(EnXc) X(EnZf) X(EnZl3) X(ObjectKankyo) X(EnHeishi1) X(Player) X(Demo)  \
                    X(MessageZmp)

#define ZMP_DECLARE_SAVESTATE(Tag) extern "C" void Tag##_SaveState(SaveStateCtx* ctx);
ZMP_STATIC_SAVESTATES(ZMP_DECLARE_SAVESTATE)
#undef ZMP_DECLARE_SAVESTATE

namespace Zmp::State {

namespace {

// Lockstep: the text language and the Z-target setting come from the shared game (the group), not
// from this client (PLAN.md 2.4 point 8 applies per client only outside lockstep).
bool sKeepSharedSettings = false;

constexpr uint32_t kMagic = 0x53504D5A; // "ZMPS"
constexpr uint32_t kVersion = 1;

enum SectionId : uint32_t {
    SEC_SAVECTX = 1,
    SEC_GAMEINFO,
    SEC_EFFECTCTX,
    SEC_HEAP,
    SEC_STATICS,
    SEC_RNG,
    SEC_TRANSITION_IDS,
    SEC_ARENAS,
    SEC_GLOBALS,
    SEC_ACTORDB,
    SEC_RESOURCES,
    SEC_HEAP_POINTER_STATICS,
    SEC_PADMGR,
    SEC_ZMPSIM,      // multiplayer simulation state (slots, parked cameras, per-player inputs, audio counters)
    SEC_GAMESTATICS, // file statics of the game code that are simulation state (docs/DECISIONES.md D-044)
};

#pragma pack(push, 1)
struct Header {
    uint32_t magic;
    uint32_t version;
    uint32_t headerSize;
    uint32_t tick;
    int32_t scene;
    uint8_t playerCount;
    uint8_t reserved[3];
    uint64_t hash;
    uint64_t imageBase;
    uint64_t heapBase;
    uint64_t heapSize;
    uint64_t resourceListHash;
    uint64_t rawSize;
    uint64_t compressedSize;
    uint64_t buildId; // hash of the executable's code: states only load in the same build
};
#pragma pack(pop)

struct Writer {
    std::vector<uint8_t> buf;
    void Raw(const void* p, size_t n) {
        const uint8_t* b = (const uint8_t*)p;
        buf.insert(buf.end(), b, b + n);
    }
    template <typename T> void Pod(const T& v) {
        Raw(&v, sizeof(T));
    }
    void Section(uint32_t id, const void* p, size_t n) {
        Pod(id);
        Pod((uint64_t)n);
        Raw(p, n);
    }
    void Section(uint32_t id, const std::vector<uint8_t>& v) {
        Section(id, v.data(), v.size());
    }
};

struct Reader {
    const uint8_t* p;
    size_t n;
    size_t off = 0;
    bool Raw(void* dst, size_t len) {
        if (off + len > n) {
            return false;
        }
        memcpy(dst, p + off, len);
        off += len;
        return true;
    }
    template <typename T> bool Pod(T& v) {
        return Raw(&v, sizeof(T));
    }
};

uint64_t BuildId() {
    static uint64_t sId = 0;
#ifdef _WIN32
    if (sId == 0) {
        uintptr_t base = (uintptr_t)&__ImageBase;
        auto nt = (const IMAGE_NT_HEADERS*)(base + __ImageBase.e_lfanew);
        const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
        uint64_t h = 0xCBF29CE484222325ULL;
        // The absolute addresses the loader wrote into the code (base relocations) depend on where the image
        // was loaded: they are hashed as offsets from the image base, so the id is the same in every process.
        std::vector<uint32_t> sites; // rva of every 64-bit relocation
        {
            const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
            uint32_t off = 0;
            while (dir.VirtualAddress != 0 && off + sizeof(IMAGE_BASE_RELOCATION) <= dir.Size) {
                auto block = (const IMAGE_BASE_RELOCATION*)(base + dir.VirtualAddress + off);
                if (block->SizeOfBlock < sizeof(IMAGE_BASE_RELOCATION)) {
                    break;
                }
                size_t count = (block->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) / sizeof(uint16_t);
                const uint16_t* e = (const uint16_t*)(block + 1);
                for (size_t k = 0; k < count; k++) {
                    if ((e[k] >> 12) == IMAGE_REL_BASED_DIR64) {
                        sites.push_back(block->VirtualAddress + (e[k] & 0xFFF));
                    }
                }
                off += block->SizeOfBlock;
            }
        }
        for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++) {
            if (sec->Characteristics & IMAGE_SCN_CNT_CODE) {
                size_t n = sec->Misc.VirtualSize / 8;
                std::vector<uint64_t> code(n);
                memcpy(code.data(), (const void*)(base + sec->VirtualAddress), n * 8);
                for (uint32_t rva : sites) {
                    if (rva >= sec->VirtualAddress && (size_t)(rva - sec->VirtualAddress) + 8 <= n * 8) {
                        uint64_t v;
                        uint8_t* at = (uint8_t*)code.data() + (rva - sec->VirtualAddress);
                        memcpy(&v, at, 8);
                        v -= (uint64_t)base;
                        memcpy(at, &v, 8);
                    }
                }
                for (size_t k = 0; k < n; k++) {
                    h = (h ^ code[k]) * 0x100000001B3ULL;
                }
            }
        }
        sId = h;
    }
#endif
    return sId;
}

bool InPlay() {
    return gGameState != nullptr && gGameState->main == Play_Main && gPlayState != nullptr;
}

uint64_t ImageBase() {
#ifdef _WIN32
    return (uint64_t)(uintptr_t)&__ImageBase;
#else
    return 0;
#endif
}

uint64_t ImageSize() {
#ifdef _WIN32
    auto nt = (const IMAGE_NT_HEADERS*)((uintptr_t)&__ImageBase + __ImageBase.e_lfanew);
    return nt->OptionalHeader.SizeOfImage;
#else
    return 0;
#endif
}

// Phase 5b (D-073): the executable is no longer at the same address in every process. The memory a save state
// carries (the game's heap, the parked contexts, the statics of the actors) holds pointers to the executable's
// functions and constants; each one that pointed into the saving process's image is moved by the distance between
// that image and this one. Same build (checked before) means same layout, so the offset inside the image is the
// same. `step` 8 for memory copied as it was (pointers are aligned there), 1 for a packed stream.
struct Relocation {
    uint64_t from = 0; // image base of the process that saved
    uint64_t size = 0;
    uint64_t delta = 0;
    uint32_t moved = 0;
    uint32_t odd = 0; // values that look like image pointers at unaligned offsets (not moved; diagnostic)

    void Apply(const uint8_t* constData, size_t n, size_t step) {
        uint8_t* p = (uint8_t*)constData;
        for (size_t off = 0; off + 8 <= n; off += step) {
            uint64_t v;
            memcpy(&v, p + off, 8);
            if (v - from < size) {
                v += delta;
                memcpy(p + off, &v, 8);
                moved++;
                if (step == 1) {
                    off += 7;
                }
            }
        }
    }
    std::string oddList;
    void CountOdd(const char* what, const uint8_t* p, size_t n) {
        for (size_t off = 0; off + 8 <= n; off++) {
            if ((off & 7) != 0) {
                uint64_t v;
                memcpy(&v, p + off, 8);
                if (v - from < size) {
                    if (odd < 16) {
                        char t[96];
                        snprintf(t, sizeof(t), " %s+0x%zX=image+0x%llX", what, off, (unsigned long long)(v - from));
                        oddList += t;
                    }
                    odd++;
                }
            }
        }
    }
};

std::vector<uint8_t> SaveStatics() {
    SaveStateCtx ctx = {};
    ctx.mode = SHIP_SAVESTATE_MEASURE;
#define ZMP_RUN(Tag) Tag##_SaveState(&ctx);
    ZMP_STATIC_SAVESTATES(ZMP_RUN)
    std::vector<uint8_t> buf(ctx.offset);
    ctx = {};
    ctx.mode = SHIP_SAVESTATE_SAVE;
    ctx.buffer = buf.data();
    ZMP_STATIC_SAVESTATES(ZMP_RUN)
    return buf;
}

bool LoadStatics(const std::vector<uint8_t>& buf) {
    SaveStateCtx ctx = {};
    ctx.mode = SHIP_SAVESTATE_MEASURE;
    ZMP_STATIC_SAVESTATES(ZMP_RUN)
    if (ctx.offset != buf.size()) {
        return false;
    }
    ctx = {};
    ctx.mode = SHIP_SAVESTATE_LOAD;
    ctx.buffer = (unsigned char*)buf.data();
    ZMP_STATIC_SAVESTATES(ZMP_RUN)
#undef ZMP_RUN
    return true;
}

struct ArenaState {
    uint64_t head;
    uint64_t start;
    uint8_t flag;
    uint8_t isInit;
};

ArenaState GetArena(const Arena& a) {
    return { (uint64_t)(uintptr_t)a.head, (uint64_t)(uintptr_t)a.start, a.flag, a.isInit };
}

void SetArena(Arena& a, const ArenaState& s) {
    a.head = (ArenaNode*)(uintptr_t)s.head;
    a.start = (void*)(uintptr_t)s.start;
    a.flag = s.flag;
    a.isInit = s.isInit;
}

struct Globals {
    uint64_t gameState;
    uint64_t playState;
    uint64_t matrixStack;
    uint64_t matrixCurrent;
    uint64_t segments[NUM_SEGMENTS];
};

// Writable sections of the executable (.data/.bss), for the heap pointer statics check.
struct Range {
    uintptr_t start;
    uintptr_t end;
};

std::vector<Range> WritableImageRanges() {
    std::vector<Range> out;
#ifdef _WIN32
    uintptr_t base = (uintptr_t)&__ImageBase;
    auto nt = (const IMAGE_NT_HEADERS*)(base + __ImageBase.e_lfanew);
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++) {
        if (sec->Characteristics & IMAGE_SCN_MEM_WRITE) {
            out.push_back({ base + sec->VirtualAddress, base + sec->VirtualAddress + sec->Misc.VirtualSize });
        }
    }
#endif
    return out;
}

bool InHeap(uint64_t v) {
    uint64_t h = (uint64_t)(uintptr_t)gSystemHeap;
    return v >= h && v < h + SYSTEM_HEAP_SIZE;
}

// (rva, value) of every 8-byte aligned word of the executable's writable data that points into
// the system heap. Words inside the blocks the blob restores explicitly are skipped.
std::vector<std::pair<uint32_t, uint64_t>> HeapPointerStatics() {
    std::vector<std::pair<uint32_t, uint64_t>> out;
    uintptr_t base = (uintptr_t)ImageBase();
    // Display list pools are rebuilt every frame by the renderer: not simulation state.
    uintptr_t gfxStart = (uintptr_t)&gGfxPools[0];
    uintptr_t gfxEnd = gfxStart + sizeof(gGfxPools);
    for (const Range& r : WritableImageRanges()) {
        for (uintptr_t a = (r.start + 7) & ~(uintptr_t)7; a + 8 <= r.end; a += 8) {
            if (a >= gfxStart && a < gfxEnd) {
                continue;
            }
            uint64_t v = *(const uint64_t*)a;
            if (InHeap(v)) {
                out.push_back({ (uint32_t)(a - base), v });
            }
        }
    }
    return out;
}

std::string SymbolName(uint64_t address) {
#ifdef _WIN32
    static bool sInit = false;
    static bool sOk = false;
    if (!sInit) {
        sInit = true;
        char exe[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, exe, MAX_PATH);
        std::filesystem::path dir = std::filesystem::path(exe).parent_path();
        std::string search = dir.string() + ";" + (dir / "debug").string();
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
        sOk = SymInitialize(GetCurrentProcess(), search.c_str(), TRUE) != FALSE;
    }
    if (sOk) {
        alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 256] = {};
        SYMBOL_INFO* sym = (SYMBOL_INFO*)buf;
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 255;
        DWORD64 disp = 0;
        if (SymFromAddr(GetCurrentProcess(), address, &disp, sym)) {
            return std::string(sym->Name) + "+0x" + ([](DWORD64 d) {
                       char t[32];
                       snprintf(t, sizeof(t), "%llX", (unsigned long long)d);
                       return std::string(t);
                   })(disp);
        }
    }
#endif
    char t[32];
    snprintf(t, sizeof(t), "rva 0x%llX", (unsigned long long)(address - ImageBase()));
    return t;
}

void AppendNote(BlobInfo* info, const std::string& s) {
    if (info != nullptr) {
        if (!info->notes.empty()) {
            info->notes += "; ";
        }
        info->notes += s;
    }
}

} // namespace

bool Save(std::vector<uint8_t>& out, uint32_t tick, uint64_t hash, std::string* err, BlobInfo* info) {
    auto t0 = std::chrono::steady_clock::now();
    if (!InPlay()) {
        *err = "save states need a game in progress (Play)";
        return false;
    }
    Writer w;
    w.Section(SEC_SAVECTX, &gSaveContext, sizeof(gSaveContext));
    w.Section(SEC_GAMEINFO, gGameInfo, sizeof(*gGameInfo));
    w.Section(SEC_EFFECTCTX, &sEffectContext, sizeof(sEffectContext));
    w.Section(SEC_HEAP, gSystemHeap, SYSTEM_HEAP_SIZE);
    w.Section(SEC_STATICS, SaveStatics());
    {
        uint32_t rng[2];
        rng[0] = Rand_ZmpGetState(&rng[1]);
        w.Section(SEC_RNG, rng, sizeof(rng));
    }
    {
        // The transition actor list lives in the scene resource and the game marks entries in place.
        std::vector<int16_t> ids;
        for (u32 i = 0; i < gPlayState->transiActorCtx.numActors; i++) {
            ids.push_back(gPlayState->transiActorCtx.list[i].id);
        }
        w.Section(SEC_TRANSITION_IDS, ids.data(), ids.size() * sizeof(int16_t));
    }
    {
        ArenaState arenas[2] = { GetArena(gSystemArena), GetArena(sZeldaArena) };
        w.Section(SEC_ARENAS, arenas, sizeof(arenas));
    }
    {
        Globals g = {};
        g.gameState = (uint64_t)(uintptr_t)gGameState;
        g.playState = (uint64_t)(uintptr_t)gPlayState;
        MtxF* stack = nullptr;
        MtxF* current = nullptr;
        Matrix_ZmpGetPointers(&stack, &current);
        g.matrixStack = (uint64_t)(uintptr_t)stack;
        g.matrixCurrent = (uint64_t)(uintptr_t)current;
        for (int i = 0; i < NUM_SEGMENTS; i++) {
            g.segments[i] = gSegments[i];
        }
        w.Section(SEC_GLOBALS, &g, sizeof(g));
    }
    {
        std::vector<int32_t> loaded;
        int count = ActorDB::Instance->GetEntryCount();
        for (int i = 0; i < count; i++) {
            loaded.push_back(ActorDB::Instance->RetrieveEntry(i).entry.numLoaded);
        }
        w.Section(SEC_ACTORDB, loaded.data(), loaded.size() * sizeof(int32_t));
    }
    uint32_t resources = 0;
    {
        Writer r;
        auto slots = ResSlots::ListSlots();
        r.Pod((uint32_t)slots.size());
        for (auto& s : slots) {
            r.Pod(s.index);
            r.Pod(s.used);
            r.Pod(s.loads);
            r.Pod((uint16_t)s.path.size());
            r.Raw(s.path.data(), s.path.size());
        }
        resources = (uint32_t)slots.size();
        w.Section(SEC_RESOURCES, r.buf);
    }
    // Controller history: press/rel edges of the next tick are computed from it.
    w.Section(SEC_PADMGR, gPadMgr.inputs, sizeof(gPadMgr.inputs));
    {
        int32_t gs[1] = { EffectSs_ZmpGetSearchIndex() };
        w.Section(SEC_GAMESTATICS, gs, sizeof(gs));
    }
    w.Section(SEC_ZMPSIM, &gZmpSim, sizeof(gZmpSim));
    {
        auto ptrs = HeapPointerStatics();
        w.Section(SEC_HEAP_POINTER_STATICS, ptrs.data(), ptrs.size() * sizeof(ptrs[0]));
    }

    size_t bound = ZSTD_compressBound(w.buf.size());
    out.resize(sizeof(Header) + bound);
    size_t csize = ZSTD_compress(out.data() + sizeof(Header), bound, w.buf.data(), w.buf.size(), 3);
    if (ZSTD_isError(csize)) {
        *err = std::string("zstd: ") + ZSTD_getErrorName(csize);
        return false;
    }
    out.resize(sizeof(Header) + csize);
    Header h = {};
    h.magic = kMagic;
    h.version = kVersion;
    h.headerSize = sizeof(Header);
    h.tick = tick;
    h.scene = gPlayState->sceneNum;
    h.playerCount = 1;
    h.hash = hash;
    h.imageBase = ImageBase();
    h.heapBase = (uint64_t)(uintptr_t)gSystemHeap;
    h.heapSize = SYSTEM_HEAP_SIZE;
    h.resourceListHash = ResSlots::FileListHash();
    h.rawSize = w.buf.size();
    h.compressedSize = csize;
    h.buildId = BuildId();
    memcpy(out.data(), &h, sizeof(h));
    if (info != nullptr) {
        info->version = kVersion;
        info->tick = tick;
        info->scene = h.scene;
        info->hash = hash;
        info->rawSize = h.rawSize;
        info->compressedSize = out.size();
        info->resources = resources;
        info->ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    }
    return true;
}

bool Peek(const std::vector<uint8_t>& blob, BlobInfo* info, std::string* err) {
    if (blob.size() < sizeof(Header)) {
        *err = "file too small for a ZMP save state";
        return false;
    }
    Header h;
    memcpy(&h, blob.data(), sizeof(h));
    if (h.magic != kMagic) {
        *err = "not a ZMP save state (bad magic)";
        return false;
    }
    if (h.version != kVersion) {
        *err = "save state version " + std::to_string(h.version) + ", this build reads " + std::to_string(kVersion);
        return false;
    }
    if (info != nullptr) {
        info->version = h.version;
        info->tick = h.tick;
        info->scene = h.scene;
        info->hash = h.hash;
        info->rawSize = h.rawSize;
        info->compressedSize = blob.size();
    }
    return true;
}

bool Load(const std::vector<uint8_t>& blob, std::string* err, BlobInfo* info) {
    auto t0 = std::chrono::steady_clock::now();
    BlobInfo local;
    if (info == nullptr) {
        info = &local;
    }
    if (!Peek(blob, info, err)) {
        return false;
    }
    Header h;
    memcpy(&h, blob.data(), sizeof(h));
    if (h.buildId != BuildId()) {
        *err = "the save state was made by a different build of the game";
        return false;
    }
    if (h.heapBase != (uint64_t)(uintptr_t)gSystemHeap || h.heapSize != SYSTEM_HEAP_SIZE) {
        *err = "the system heap is at a different address in this process";
        return false;
    }
    if (h.resourceListHash != ResSlots::FileListHash()) {
        *err = "different asset archives (oot.o2r / soh.o2r / mods) than the process that saved the state";
        return false;
    }
    if (!InPlay()) {
        *err = "load a game first: save states can only be loaded during gameplay";
        return false;
    }
    std::vector<uint8_t> raw(h.rawSize);
    size_t got = ZSTD_decompress(raw.data(), raw.size(), blob.data() + sizeof(Header), blob.size() - sizeof(Header));
    if (ZSTD_isError(got) || got != raw.size()) {
        *err = "corrupt save state (decompression failed)";
        return false;
    }

    // Parse every section before touching memory.
    std::map<uint32_t, std::pair<const uint8_t*, size_t>> sections;
    Reader r{ raw.data(), raw.size() };
    while (r.off < r.n) {
        uint32_t id;
        uint64_t len;
        if (!r.Pod(id) || !r.Pod(len) || r.off + len > r.n) {
            *err = "corrupt save state (section table)";
            return false;
        }
        sections[id] = { r.p + r.off, (size_t)len };
        r.off += len;
    }
    auto need = [&](uint32_t id, size_t size) -> const uint8_t* {
        auto it = sections.find(id);
        if (it == sections.end() || (size != 0 && it->second.second != size)) {
            return nullptr;
        }
        return it->second.first;
    };
    const uint8_t* saveCtx = need(SEC_SAVECTX, sizeof(gSaveContext));
    const uint8_t* gameInfo = need(SEC_GAMEINFO, sizeof(*gGameInfo));
    const uint8_t* effectCtx = need(SEC_EFFECTCTX, sizeof(sEffectContext));
    const uint8_t* heap = need(SEC_HEAP, SYSTEM_HEAP_SIZE);
    const uint8_t* rng = need(SEC_RNG, 8);
    const uint8_t* arenas = need(SEC_ARENAS, sizeof(ArenaState) * 2);
    const uint8_t* globals = need(SEC_GLOBALS, sizeof(Globals));
    if (!saveCtx || !gameInfo || !effectCtx || !heap || !rng || !arenas || !globals || !sections.count(SEC_STATICS) ||
        !sections.count(SEC_RESOURCES) || !sections.count(SEC_ACTORDB) || !sections.count(SEC_TRANSITION_IDS)) {
        *err = "save state is missing sections or was made by a different build";
        return false;
    }
    std::vector<uint8_t> statics(sections[SEC_STATICS].first,
                                 sections[SEC_STATICS].first + sections[SEC_STATICS].second);
    if (h.imageBase != ImageBase()) {
        // The saving process had the executable at another address (D-073): its pointers to the executable are
        // moved to this process's image, in the decompressed copy, before anything is written to memory.
        Relocation rel;
        rel.from = h.imageBase;
        rel.size = ImageSize();
        rel.delta = ImageBase() - h.imageBase;
        rel.CountOdd("heap", heap, SYSTEM_HEAP_SIZE);
        rel.Apply(heap, SYSTEM_HEAP_SIZE, 8);
        rel.Apply(saveCtx, sizeof(gSaveContext), 8);
        rel.Apply(gameInfo, sizeof(*gGameInfo), 8);
        rel.Apply(effectCtx, sizeof(sEffectContext), 8);
        rel.Apply(globals, sizeof(Globals), 8);
        if (sections.count(SEC_ZMPSIM)) {
            // (the statics of each player's text box and ocarina are packed streams inside the structure)
            const uint8_t* sim = sections[SEC_ZMPSIM].first;
            size_t simSize = sections[SEC_ZMPSIM].second;
            size_t packedStart = offsetof(std::remove_reference<decltype(gZmpSim)>::type, msgStatics);
            size_t packedEnd =
                offsetof(std::remove_reference<decltype(gZmpSim)>::type, ocaStatics) + sizeof(gZmpSim.ocaStatics);
            size_t after = (packedEnd + 7) & ~(size_t)7;
            if (simSize == sizeof(gZmpSim) && packedStart < packedEnd && after <= simSize) {
                rel.CountOdd("sim", sim, packedStart);
                rel.Apply(sim, packedStart, 8);
                rel.Apply(sim + packedStart, packedEnd - packedStart, 1);
                rel.CountOdd("sim2", sim + after, simSize - after);
                rel.Apply(sim + after, simSize - after, 8);
            }
        }
        rel.Apply(statics.data(), statics.size(), 1);
        info->relocated = rel.moved;
        info->relocatedOdd = rel.odd;
        if (rel.odd > 0) {
            Log("zmp: state load: " + std::to_string(rel.odd) +
                " values at unaligned offsets look like pointers to the executable (left as they were):" + rel.oddList);
        }
    }

    // Resources: every resource the saving process had parsed must exist at the same address here.
    // Missing ones are loaded now; each lands in its slot, at the address it had over there.
    {
        Reader rr{ sections[SEC_RESOURCES].first, sections[SEC_RESOURCES].second };
        uint32_t count = 0;
        rr.Pod(count);
        std::map<uint32_t, ResSlots::SlotInfo> localSlots;
        for (auto& s : ResSlots::ListSlots()) {
            localSlots[s.index] = s;
        }
        auto rm = Ship::Context::GetRawInstance()->GetResourceManager();
        uint32_t loadedNow = 0;
        for (uint32_t i = 0; i < count; i++) {
            uint32_t index, loads;
            uint64_t used;
            uint16_t len;
            if (!rr.Pod(index) || !rr.Pod(used) || !rr.Pod(loads) || !rr.Pod(len)) {
                *err = "corrupt resource manifest";
                return false;
            }
            std::string path(len, '\0');
            rr.Raw(path.data(), len);
            auto it = localSlots.find(index);
            if (it == localSlots.end()) {
                rm->LoadResourceProcess(path);
                loadedNow++;
                for (auto& s : ResSlots::ListSlots()) {
                    if (s.index == index) {
                        localSlots[index] = s;
                    }
                }
                it = localSlots.find(index);
            }
            if (it == localSlots.end()) {
                *err = "resource " + path + " could not be loaded at its fixed address";
                return false;
            }
            if (it->second.used != used) {
                // Same path, different layout: parsed a different number of times or differently.
                if (loads == 1 && it->second.loads == 1) {
                    *err = "resource " + path + " has a different size here (" + std::to_string(it->second.used) +
                           " vs " + std::to_string(used) + " bytes)";
                    return false;
                }
                AppendNote(info, "resource " + path + " parsed " + std::to_string(loads) + "/" +
                                     std::to_string(it->second.loads) + " times (sizes differ)");
            }
        }
        info->resources = count;
        if (loadedNow > 0) {
            AppendNote(info, std::to_string(loadedNow) + " resources loaded for this state");
        }
    }

    // Keep the per-client fields of the save context (PLAN.md 8.3).
    u8 language = gSaveContext.language;
    u8 audioSetting = gSaveContext.audioSetting;
    u8 zTargetSetting = gSaveContext.zTargetSetting;
    u8 filenameLanguage = gSaveContext.ship.filenameLanguage;
    SohStats stats = gSaveContext.ship.stats;

    memcpy(gSystemHeap, heap, SYSTEM_HEAP_SIZE);
    memcpy(&gSaveContext, saveCtx, sizeof(gSaveContext));
    if (!sKeepSharedSettings) {
        gSaveContext.language = language;
        gSaveContext.zTargetSetting = zTargetSetting;
    }
    gSaveContext.audioSetting = audioSetting;
    gSaveContext.ship.filenameLanguage = filenameLanguage;
    gSaveContext.ship.stats = stats;
    memcpy(gGameInfo, gameInfo, sizeof(*gGameInfo));
    memcpy(&sEffectContext, effectCtx, sizeof(sEffectContext));
    {
        uint32_t v[2];
        memcpy(v, rng, sizeof(v));
        Rand_ZmpSetState(v[0], v[1]);
    }
    {
        ArenaState a[2];
        memcpy(a, arenas, sizeof(a));
        SetArena(gSystemArena, a[0]);
        SetArena(sZeldaArena, a[1]);
    }
    {
        Globals g;
        memcpy(&g, globals, sizeof(g));
        gGameState = (GameState*)(uintptr_t)g.gameState;
        gPlayState = (PlayState*)(uintptr_t)g.playState;
        Matrix_ZmpSetPointers((MtxF*)(uintptr_t)g.matrixStack, (MtxF*)(uintptr_t)g.matrixCurrent);
        for (int i = 0; i < NUM_SEGMENTS; i++) {
            gSegments[i] = (uintptr_t)g.segments[i];
        }
    }
    {
        // Actor overlays whose last instance is gone get their static data reset, as the game does
        // when it frees an overlay; then the static data SoH serializes is restored on top.
        const int32_t* loaded = (const int32_t*)sections[SEC_ACTORDB].first;
        size_t n = sections[SEC_ACTORDB].second / sizeof(int32_t);
        int count = ActorDB::Instance->GetEntryCount();
        for (int i = 0; i < count && (size_t)i < n; i++) {
            auto& e = ActorDB::Instance->RetrieveEntry(i).entry;
            if (loaded[i] == 0 && e.reset != nullptr) {
                e.reset();
            }
            e.numLoaded = loaded[i];
        }
        if ((size_t)count != n) {
            AppendNote(info, "actor table size differs");
        }
    }
    if (sections.count(SEC_GAMESTATICS) && sections[SEC_GAMESTATICS].second == sizeof(int32_t) * 1) {
        int32_t gs[1];
        memcpy(gs, sections[SEC_GAMESTATICS].first, sizeof(gs));
        EffectSs_ZmpSetSearchIndex(gs[0]);
    }
    if (sections.count(SEC_PADMGR) && sections[SEC_PADMGR].second == sizeof(gPadMgr.inputs)) {
        memcpy(gPadMgr.inputs, sections[SEC_PADMGR].first, sizeof(gPadMgr.inputs));
    }
    if (sections.count(SEC_ZMPSIM) && sections[SEC_ZMPSIM].second == sizeof(gZmpSim)) {
        memcpy(&gZmpSim, sections[SEC_ZMPSIM].first, sizeof(gZmpSim));
    } else {
        memset(&gZmpSim, 0, sizeof(gZmpSim));
    }
    Zmp::Players::AfterStateLoad();
    if (!LoadStatics(statics)) {
        *err = "static data layout differs (different build?) - state partially loaded";
        return false;
    }
    {
        const int16_t* ids = (const int16_t*)sections[SEC_TRANSITION_IDS].first;
        size_t n = sections[SEC_TRANSITION_IDS].second / sizeof(int16_t);
        size_t m = std::min<size_t>(n, gPlayState->transiActorCtx.numActors);
        for (size_t i = 0; i < m; i++) {
            gPlayState->transiActorCtx.list[i].id = ids[i];
        }
    }
    // Check: every static of the saving process that pointed into the heap must point to the same
    // place here. A mismatch is a global the blob does not carry yet (it would dangle).
    if (sections.count(SEC_HEAP_POINTER_STATICS)) {
        auto& sec = sections[SEC_HEAP_POINTER_STATICS];
        size_t n = sec.second / sizeof(std::pair<uint32_t, uint64_t>);
        const auto* ptrs = (const std::pair<uint32_t, uint64_t>*)sec.first;
        uintptr_t base = (uintptr_t)ImageBase();
        uint32_t mismatches = 0;
        std::string names;
        FILE* report = fopen("logs/state-load-statics.txt", "w");
        for (size_t i = 0; i < n; i++) {
            uint64_t now = *(const uint64_t*)(base + ptrs[i].first);
            if (now != ptrs[i].second) {
                mismatches++;
                std::string name = SymbolName(base + ptrs[i].first);
                if (report != nullptr) {
                    fprintf(report, "%s saved=0x%llX now=0x%llX\n", name.c_str(), (unsigned long long)ptrs[i].second,
                            (unsigned long long)now);
                }
                if (mismatches <= 12) {
                    names += (names.empty() ? "" : ", ") + name;
                }
            }
        }
        if (report != nullptr) {
            fclose(report);
        }
        if (mismatches > 0) {
            AppendNote(info, std::to_string(mismatches) + " heap pointer statics differ: " + names);
            Log("zmp: state load: " + std::to_string(mismatches) + " heap pointer statics differ: " + names);
        }
    }
    info->ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return true;
}

bool ReadFile(const std::string& path, std::vector<uint8_t>& out, std::string* err) {
    FILE* f = fopen(path.c_str(), "rb");
    if (f == nullptr) {
        *err = "cannot open " + path;
        return false;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    out.resize(size > 0 ? (size_t)size : 0);
    size_t got = out.empty() ? 0 : fread(out.data(), 1, out.size(), f);
    fclose(f);
    if (got != out.size()) {
        *err = "cannot read " + path;
        return false;
    }
    return true;
}

bool WriteFile(const std::string& path, const std::vector<uint8_t>& data, std::string* err) {
    std::error_code ec;
    auto parent = std::filesystem::path(path).parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
    }
    FILE* f = fopen(path.c_str(), "wb");
    if (f == nullptr) {
        *err = "cannot write " + path;
        return false;
    }
    size_t put = data.empty() ? 0 : fwrite(data.data(), 1, data.size(), f);
    fclose(f);
    if (put != data.size()) {
        *err = "short write " + path;
        return false;
    }
    return true;
}

bool Census(const std::string& path, std::string* summary) {
    FILE* f = fopen(path.c_str(), "w");
    if (f == nullptr) {
        *summary = "cannot write " + path;
        return false;
    }
    uint64_t heap = (uint64_t)(uintptr_t)gSystemHeap;
    uint64_t image = ImageBase();
    uint64_t imageSize = 0;
#ifdef _WIN32
    {
        auto nt = (const IMAGE_NT_HEADERS*)((uintptr_t)image + __ImageBase.e_lfanew);
        imageSize = nt->OptionalHeader.SizeOfImage;
    }
#endif
    std::map<uint64_t, uint32_t> byRegion;
    uint32_t total = 0, inHeap = 0, inImage = 0, inResources = 0;
    fprintf(f, "# heap offset, value, region base, region type\n");
    for (uint64_t off = 0; off + 8 <= SYSTEM_HEAP_SIZE; off += 8) {
        uint64_t v = *(const uint64_t*)(heap + off);
        if (v < 0x10000 || v > 0x00007FFFFFFFFFFFULL) {
            continue;
        }
        if (v >= heap && v < heap + SYSTEM_HEAP_SIZE) {
            inHeap++;
            continue;
        }
        if (v >= image && v < image + imageSize) {
            inImage++;
            continue;
        }
        if (ResSlots::Owns((const void*)(uintptr_t)v)) {
            inResources++;
            continue;
        }
#ifdef _WIN32
        MEMORY_BASIC_INFORMATION mbi = {};
        if (VirtualQuery((const void*)(uintptr_t)v, &mbi, sizeof(mbi)) == 0 || mbi.State != MEM_COMMIT) {
            continue; // not mapped: not a live pointer
        }
        total++;
        byRegion[(uint64_t)(uintptr_t)mbi.AllocationBase]++;
        fprintf(f, "0x%06" PRIX64 " 0x%016" PRIX64 " 0x%016" PRIX64 " %s\n", off, v,
                (uint64_t)(uintptr_t)mbi.AllocationBase,
                mbi.Type == MEM_IMAGE ? "image" : (mbi.Type == MEM_MAPPED ? "mapped" : "private"));
#endif
    }
    fclose(f);
    *summary = "heap->heap " + std::to_string(inHeap) + ", heap->exe " + std::to_string(inImage) +
               ", heap->resources " + std::to_string(inResources) + ", heap->other " + std::to_string(total) + " in " +
               std::to_string(byRegion.size()) + " regions; resource slots " +
               (ResSlots::IsActive() ? "active" : "inactive") + ", fallback allocations " +
               std::to_string(ResSlots::FallbackCount());
    return true;
}

} // namespace Zmp::State

namespace Zmp::State {
void SetKeepSharedSettings(bool keep) {
    sKeepSharedSettings = keep;
}
} // namespace Zmp::State
