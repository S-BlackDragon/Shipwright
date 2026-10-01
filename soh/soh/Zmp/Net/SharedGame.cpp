#include "SharedGame.h"

#include <cstddef>
#include <cstring>
#include <map>
#include <mutex>

#include "soh/Zmp/ZmpLog.h"
#include "soh/Zmp/Sim/ZmpPlayers.h"

extern "C" {
#include <z64.h>
#include "variables.h"
#include "macros.h"
extern PlayState* gPlayState;
void Play_Main(GameState* thisx);
}

namespace Zmp::SharedGame {

namespace {

enum Op : uint8_t { kBits = 0, kDelta = 1 };
enum Live : int8_t { kNone = -1, kChest = 0, kSwch = 1, kClear = 2, kCollect = 3 };

struct Field {
    uint32_t saveOff; // offset in SaveContext
    uint16_t off;     // offset in the compact buffer
    uint8_t size;
    uint8_t op;
    int8_t live;   // scene flag also live in play->actorCtx.flags while that scene is loaded
    uint8_t scene; // scene of a scene flag
    bool rupees;
    bool age = false; // Link's age (phase 5b): never written into a running scene, it asks for a reload instead
};

std::vector<Field> sFields;
std::map<uint16_t, size_t> sByOff;
uint32_t sLayoutHash = 0;
size_t sSize = 0;
std::once_flag sOnce;

void Add(size_t saveOff, uint8_t size, uint8_t op, int8_t live = kNone, uint8_t scene = 0, bool rupees = false) {
    Field f{ (uint32_t)saveOff, (uint16_t)sSize, size, op, live, scene, rupees };
    sByOff[f.off] = sFields.size();
    sFields.push_back(f);
    sSize += size;
}

#define SAVE_OFF(member) offsetof(SaveContext, member)

void Build() {
    // Owned items (bottle slots excluded: their contents belong to each player, D-035).
    for (int i = 0; i < 24; i++) {
        if (i >= SLOT_BOTTLE_1 && i <= SLOT_BOTTLE_4) {
            continue;
        }
        Add(SAVE_OFF(inventory.items) + i, 1, kBits);
    }
    Add(SAVE_OFF(inventory.equipment), 2, kBits);
    Add(SAVE_OFF(inventory.upgrades), 4, kBits);
    Add(SAVE_OFF(inventory.questItems), 4, kBits);
    for (int i = 0; i < 20; i++) {
        Add(SAVE_OFF(inventory.dungeonItems) + i, 1, kBits);
    }
    for (int i = 0; i < 19; i++) {
        Add(SAVE_OFF(inventory.dungeonKeys) + i, 1, kDelta);
    }
    Add(SAVE_OFF(inventory.defenseHearts), 1, kDelta);
    Add(SAVE_OFF(inventory.gsTokens), 2, kDelta);
    Add(SAVE_OFF(healthCapacity), 2, kDelta);
    Add(SAVE_OFF(magicLevel), 1, kBits);
    Add(SAVE_OFF(isMagicAcquired), 1, kBits);
    Add(SAVE_OFF(isDoubleMagicAcquired), 1, kBits);
    Add(SAVE_OFF(isDoubleDefenseAcquired), 1, kBits);
    Add(SAVE_OFF(bgsFlag), 1, kBits);
    Add(SAVE_OFF(rupees), 2, kDelta, kNone, 0, true);
    const int scenes = (int)ARRAY_COUNT(gSaveContext.sceneFlags);
    for (int s = 0; s < scenes; s++) {
        size_t base = SAVE_OFF(sceneFlags) + (size_t)s * sizeof(SavedSceneFlags);
        Add(base + offsetof(SavedSceneFlags, chest), 4, kBits, kChest, (uint8_t)s);
        Add(base + offsetof(SavedSceneFlags, swch), 4, kBits, kSwch, (uint8_t)s);
        Add(base + offsetof(SavedSceneFlags, clear), 4, kBits, kClear, (uint8_t)s);
        Add(base + offsetof(SavedSceneFlags, collect), 4, kBits, kCollect, (uint8_t)s);
        Add(base + offsetof(SavedSceneFlags, unk), 4, kBits, kNone, (uint8_t)s);
        Add(base + offsetof(SavedSceneFlags, rooms), 4, kBits, kNone, (uint8_t)s);
        Add(base + offsetof(SavedSceneFlags, floors), 4, kBits, kNone, (uint8_t)s);
    }
    for (int i = 0; i < 6; i++) {
        Add(SAVE_OFF(gsFlags) + 4 * i, 4, kBits);
    }
    for (int i = 0; i < 14; i++) {
        Add(SAVE_OFF(eventChkInf) + 2 * i, 2, kBits);
    }
    for (int i = 0; i < 4; i++) {
        Add(SAVE_OFF(itemGetInf) + 2 * i, 2, kBits);
    }
    for (int i = 0; i < 30; i++) {
        Add(SAVE_OFF(infTable) + 2 * i, 2, kBits);
    }
    Add(SAVE_OFF(worldMapAreaData), 4, kBits);
    // Phase 5b: everybody changes age together, in whatever group (low byte of linkAge: 0 adult, 1 child).
    Add(SAVE_OFF(linkAge), 1, kBits);
    sFields.back().age = true;
    // FNV-1a of the table and of the save context size.
    uint32_t h = 2166136261u;
    auto mix = [&](uint32_t v) {
        for (int i = 0; i < 4; i++) {
            h ^= (v >> (8 * i)) & 0xFF;
            h *= 16777619u;
        }
    };
    mix((uint32_t)sizeof(SaveContext));
    for (auto& f : sFields) {
        mix(f.saveOff);
        mix(f.size | (f.op << 8) | ((uint8_t)f.live << 16));
    }
    sLayoutHash = h;
}

void Init() {
    std::call_once(sOnce, Build);
}

bool InPlayScene(int* scene) {
    if (gGameState == nullptr || gGameState->main != Play_Main || gPlayState == nullptr) {
        return false;
    }
    int s = gPlayState->sceneNum;
    if (s < 0 || s >= (int)ARRAY_COUNT(gSaveContext.sceneFlags)) {
        return false;
    }
    *scene = s;
    return true;
}

u32* LiveFlag(int live) {
    switch (live) {
        case kChest:
            return &gPlayState->actorCtx.flags.chest;
        case kSwch:
            return &gPlayState->actorCtx.flags.swch;
        case kClear:
            return &gPlayState->actorCtx.flags.clear;
        case kCollect:
            return &gPlayState->actorCtx.flags.collect;
    }
    return nullptr;
}

uint32_t Get(const uint8_t* p, int size) {
    uint32_t v = 0;
    memcpy(&v, p, size); // little endian
    return v;
}

void Put(uint8_t* p, int size, uint32_t v) {
    memcpy(p, &v, size);
}

int32_t SignExtend(uint32_t v, int size) {
    if (size == 1) {
        return (int8_t)v;
    }
    if (size == 2) {
        return (int16_t)v;
    }
    return (int32_t)v;
}

uint32_t Mask(int size) {
    return size == 4 ? 0xFFFFFFFFu : ((1u << (8 * size)) - 1);
}

// One patch entry applied to a value.
uint32_t ApplyEntry(uint32_t v, const Field& f, uint32_t a, uint32_t b) {
    if (f.op == kBits) {
        return (v | a) & ~b;
    }
    return (v + a) & Mask(f.size);
}

struct Entry {
    const Field* f;
    uint32_t a; // set mask / delta
    uint32_t b; // clear mask
};

bool Parse(const std::vector<uint8_t>& patch, std::vector<Entry>& out) {
    Init();
    size_t i = 0;
    while (i < patch.size()) {
        if (i + 4 > patch.size()) {
            return false;
        }
        uint16_t off = (uint16_t)(patch[i] | (patch[i + 1] << 8));
        uint8_t size = patch[i + 2];
        uint8_t op = patch[i + 3];
        i += 4;
        auto it = sByOff.find(off);
        if (it == sByOff.end()) {
            return false;
        }
        const Field& f = sFields[it->second];
        if (f.size != size || f.op != op) {
            return false;
        }
        size_t need = op == kBits ? 2 * size : size;
        if (i + need > patch.size()) {
            return false;
        }
        Entry e{ &f, Get(&patch[i], size), op == kBits ? Get(&patch[i + size], size) : 0 };
        i += need;
        out.push_back(e);
    }
    return true;
}

} // namespace

uint32_t LayoutHash() {
    Init();
    return sLayoutHash;
}

size_t Size() {
    Init();
    return sSize;
}

std::vector<uint8_t> Snapshot() {
    Init();
    std::vector<uint8_t> buf(sSize);
    const uint8_t* save = (const uint8_t*)&gSaveContext;
    for (auto& f : sFields) {
        memcpy(&buf[f.off], save + f.saveOff, f.size);
        if (f.age && gZmpSim.agePending != 0) {
            buf[f.off] = (uint8_t)(gZmpSim.agePending - 1); // the age this group is about to reload with
        }
    }
    int scene;
    if (InPlayScene(&scene)) {
        for (auto& f : sFields) {
            if (f.live != kNone && f.scene == scene) {
                Put(&buf[f.off], 4, *LiveFlag(f.live));
            }
        }
    }
    return buf;
}

std::vector<uint8_t> Diff(const std::vector<uint8_t>& base, const std::vector<uint8_t>& cur) {
    Init();
    std::vector<uint8_t> out;
    if (base.size() != sSize || cur.size() != sSize) {
        return out;
    }
    for (auto& f : sFields) {
        if (memcmp(&base[f.off], &cur[f.off], f.size) == 0) {
            continue;
        }
        uint32_t b = Get(&base[f.off], f.size);
        uint32_t c = Get(&cur[f.off], f.size);
        out.push_back((uint8_t)(f.off & 0xFF));
        out.push_back((uint8_t)(f.off >> 8));
        out.push_back(f.size);
        out.push_back(f.op);
        uint8_t tmp[8];
        if (f.op == kBits) {
            Put(tmp, f.size, c & ~b);
            Put(tmp + f.size, f.size, b & ~c);
            out.insert(out.end(), tmp, tmp + 2 * f.size);
        } else {
            Put(tmp, f.size, (c - b) & Mask(f.size));
            out.insert(out.end(), tmp, tmp + f.size);
        }
    }
    return out;
}

bool ApplyToBuffer(std::vector<uint8_t>& buf, const std::vector<uint8_t>& patch) {
    std::vector<Entry> entries;
    if (buf.size() != Size() || !Parse(patch, entries)) {
        return false;
    }
    for (auto& e : entries) {
        uint32_t v = Get(&buf[e.f->off], e.f->size);
        Put(&buf[e.f->off], e.f->size, ApplyEntry(v, *e.f, e.a, e.b));
    }
    return true;
}

bool ApplyToGame(const std::vector<uint8_t>& patch, std::string* summary) {
    std::vector<Entry> entries;
    if (!Parse(patch, entries)) {
        return false;
    }
    uint8_t* save = (uint8_t*)&gSaveContext;
    int scene = -1;
    bool inScene = InPlayScene(&scene);
    for (auto& e : entries) {
        const Field& f = *e.f;
        uint32_t v = Get(save + f.saveOff, f.size);
        uint32_t nv = ApplyEntry(v, f, e.a, e.b);
        if (f.age) {
            // The Links of the running scene were built for the current age: the scene reloads with the new one
            // (Zmp_TransitionGate) instead of changing the value under them.
            uint32_t cur = gZmpSim.agePending != 0 ? (uint32_t)(gZmpSim.agePending - 1) : v;
            if ((nv & 0xFF) != (cur & 0xFF)) {
                gZmpSim.agePending = (u8)(1 + (nv & 1));
                Log("shared: the room changed age (" + std::string((nv & 1) ? "child" : "adult") +
                    "): this scene reloads");
            }
            continue;
        }
        if (f.rupees && SignExtend(nv, f.size) < 0) {
            nv = 0; // spent in two places at once: nobody goes below zero (the difference is sent back as a change)
        }
        Put(save + f.saveOff, f.size, nv);
        if (inScene && f.live != kNone && f.scene == scene) {
            u32* live = LiveFlag(f.live);
            *live = ApplyEntry(*live, f, e.a, e.b);
        }
    }
    if (summary != nullptr) {
        *summary = Describe(patch);
    }
    return true;
}

bool HasBaseline() {
    return gZmpSim.sharedValid != 0 && gZmpSim.sharedSize == Size();
}

std::vector<uint8_t> Baseline() {
    if (!HasBaseline()) {
        return {};
    }
    return std::vector<uint8_t>(gZmpSim.sharedBase, gZmpSim.sharedBase + gZmpSim.sharedSize);
}

void SetBaseline(const std::vector<uint8_t>& buf) {
    if (buf.size() != Size() || buf.size() > sizeof(gZmpSim.sharedBase)) {
        Log("shared: baseline of " + std::to_string(buf.size()) + " bytes ignored (layout " + std::to_string(Size()) +
            ")");
        gZmpSim.sharedValid = 0;
        return;
    }
    memcpy(gZmpSim.sharedBase, buf.data(), buf.size());
    gZmpSim.sharedSize = (u16)buf.size();
    gZmpSim.sharedValid = 1;
}

void ClearBaseline() {
    gZmpSim.sharedValid = 0;
}

std::vector<uint8_t> TakeTickPatch() {
    if (!HasBaseline()) {
        return {};
    }
    std::vector<uint8_t> cur = Snapshot();
    std::vector<uint8_t> base = Baseline();
    std::vector<uint8_t> patch = Diff(base, cur);
    if (!patch.empty()) {
        SetBaseline(cur);
    }
    return patch;
}

void MergeOnFound(const std::vector<uint8_t>& canonical, const std::vector<uint8_t>& sent) {
    std::vector<uint8_t> base = HasBaseline() ? Baseline() : std::vector<uint8_t>();
    if (canonical.size() == Size()) {
        std::vector<uint8_t> cur = Snapshot();
        if (base.empty()) {
            base = cur; // a fresh game: the canonical one replaces its shared part
        }
        std::vector<uint8_t> patch = Diff(base, canonical);
        std::string sum;
        ApplyToGame(patch, &sum);
        SetBaseline(canonical);
        Log("shared: founded with the room's game (" + std::to_string(patch.size()) + " bytes of changes: " + sum +
            ")");
    } else {
        // The server took this client's shared game: that is the baseline (changes since then are sent next tick).
        SetBaseline(sent.size() == Size() ? sent : Snapshot());
        Log("shared: this game became the room's canonical game");
    }
}

std::string Describe(const std::vector<uint8_t>& patch) {
    std::vector<Entry> entries;
    if (!Parse(patch, entries)) {
        return "malformed";
    }
    std::string out;
    int flags = 0;
    for (auto& e : entries) {
        const Field& f = *e.f;
        if (f.rupees) {
            out += "rupees " + std::to_string(SignExtend(e.a, f.size)) + " ";
        } else if (f.op == kDelta) {
            out += "counter@" + std::to_string(f.saveOff) + " " + std::to_string(SignExtend(e.a, f.size)) + " ";
        } else if (f.live != kNone || f.saveOff >= SAVE_OFF(gsFlags)) {
            flags++;
        } else {
            out += "field@" + std::to_string(f.saveOff) + " ";
        }
    }
    if (flags > 0) {
        out += std::to_string(flags) + " flag words";
    }
    return out.empty() ? "nothing" : out;
}

} // namespace Zmp::SharedGame
