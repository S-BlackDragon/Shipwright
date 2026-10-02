// State hash (PLAN.md 8.2) and readable state dump. Both walk the same fields through one
// visitor so the dump always explains the hash: if two hashes differ, the dumps of the same tick
// differ in at least one line.

#include "ZmpSim.h"
#include "Session.h"

#include <cinttypes>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include "soh/ActorDB.h"
#include "soh/Zmp/State/FixedHeap.h"
#include "ZmpPlayers.h"

extern "C" {
#include <z64.h>
#include "variables.h"
#include "functions.h"
#include "macros.h"
extern PlayState* gPlayState;
void Play_Main(GameState* thisx);
void FileChoose_Main(GameState* thisx);
}

#ifdef _WIN32
extern "C" IMAGE_DOS_HEADER __ImageBase;
#endif

namespace Zmp::Sim {

namespace {

// Never a pointer value. An address inside the resource region becomes its offset from the region
// base (deterministic, see ResourceSlots.h). An address inside the executable is build dependent,
// so it is reduced to its meaning when that is stable: an asset name ("__OTR__..." string, how SoH
// refers to animations and display lists) is hashed by content; anything else (a function) only
// counts as "inside the executable". Recordings made with one build then replay in the next build
// as long as the game logic did not change. Anything else is "null" or "other".
uint64_t PointerToken(const void* p) {
    if (p == nullptr) {
        return 0;
    }
    uintptr_t a = (uintptr_t)p;
#ifdef _WIN32
    static uintptr_t sBase = 0;
    static uintptr_t sSize = 0;
    if (sBase == 0) {
        sBase = (uintptr_t)&__ImageBase;
        auto nt = (const IMAGE_NT_HEADERS*)(sBase + __ImageBase.e_lfanew);
        sSize = nt->OptionalHeader.SizeOfImage;
    }
    if (a >= sBase && a < sBase + sSize) {
        const char* str = (const char*)p;
        if (a + 8 < sBase + sSize && memcmp(str, "__OTR__", 7) == 0) {
            uint64_t h = 0xCBF29CE484222325ULL;
            for (size_t i = 0; i < 256 && a + i < sBase + sSize && str[i] != 0; i++) {
                h = (h ^ (uint8_t)str[i]) * 0x100000001B3ULL;
            }
            return 0x3000000000000000ULL | (h & 0x0FFFFFFFFFFFFFFFULL);
        }
        return 0x1000000000000000ULL;
    }
#endif
    if (a >= ZMP_RESOURCE_REGION_ADDR && a < ZMP_RESOURCE_REGION_ADDR + (1ULL << 42)) {
        return 0x2000000000000000ULL | (uint64_t)(a - ZMP_RESOURCE_REGION_ADDR);
    }
    return 0xF000000000000000ULL;
}

class Visitor {
  public:
    virtual ~Visitor() = default;
    virtual void U(const char* key, uint64_t v) = 0;
    virtual void S(const char* key, int64_t v) = 0;
    virtual void F(const char* key, float v) = 0;
    virtual void Bytes(const char* key, const void* p, size_t n) = 0;
    virtual void Section(const std::string& name) {
    }
    virtual void ActorHeader(const std::string& prefix, const Actor* a) {
    }
    // Dump only (never hashed): what explains a value that is in the hash.
    virtual void Info(const char* key, float v) {
    }
    std::string prefix;
};

std::string K(const Visitor& v, const char* field) {
    return v.prefix + field;
}

void Vec(Visitor& v, const char* name, const Vec3f& x) {
    std::string n = name;
    v.F((n + ".x").c_str(), x.x);
    v.F((n + ".y").c_str(), x.y);
    v.F((n + ".z").c_str(), x.z);
}

void Rot(Visitor& v, const char* name, const Vec3s& r) {
    std::string n = name;
    v.S((n + ".x").c_str(), r.x);
    v.S((n + ".y").c_str(), r.y);
    v.S((n + ".z").c_str(), r.z);
}

bool InPlay() {
    return gGameState != nullptr && gGameState->main == Play_Main && gPlayState != nullptr;
}

void VisitActor(Visitor& v, const Actor* a) {
    v.S("id", a->id);
    v.U("category", a->category);
    v.S("params", a->params);
    Vec(v, "pos", a->world.pos);
    Rot(v, "rot", a->world.rot);
    Rot(v, "shape_rot", a->shape.rot);
    v.S("health", a->colChkInfo.health);
    v.U("freeze_timer", a->freezeTimer);
    v.U("flags", a->flags);
    // (the culling flag comes from this projection, made while drawing with a view of the simulation)
    v.Info("~proj.x", a->projectedPos.x);
    v.Info("~proj.y", a->projectedPos.y);
    v.Info("~proj.z", a->projectedPos.z);
    v.Info("~proj.w", a->projectedW);
    v.S("room", a->room);
    Vec(v, "velocity", a->velocity);
    v.F("speed_xz", a->speedXZ);
    v.F("gravity", a->gravity);
    if (a->id == ACTOR_PLAYER) {
        const Player* p = (const Player*)a;
        v.U("state_flags1", p->stateFlags1);
        v.U("state_flags2", p->stateFlags2);
        v.U("state_flags3", p->stateFlags3);
        v.U("action_func", PointerToken((const void*)p->actionFunc));
        v.F("linear_velocity", p->linearVelocity);
        v.S("yaw", p->yaw);
        v.F("anim_cur_frame", p->skelAnime.curFrame);
        v.U("anim", PointerToken(p->skelAnime.animation));
        // Per-player setting that changes how input is interpreted (hold / switch targeting). It
        // lives in the save context but is excluded there (PLAN.md 2.4 point 8).
        v.U("z_target_setting", gSaveContext.zTargetSetting);
    }
}

void VisitPlay(Visitor& v) {
    PlayState* play = gPlayState;
    v.Section("play");
    v.prefix = "play.";
    v.S("scene", play->sceneNum);
    v.S("room_cur", play->roomCtx.curRoom.num);
    v.S("room_prev", play->roomCtx.prevRoom.num);
    v.U("room_status", play->roomCtx.status);
    v.U("gameplay_frames", play->gameplayFrames);
    v.S("transition_trigger", play->transitionTrigger);
    v.U("transition_mode", play->transitionMode);
    v.S("next_entrance", play->nextEntranceIndex);
    v.S("active_camera", play->activeCamera);
    v.U("game_over_state", play->gameOverCtx.state);
    v.U("pause_state", play->pauseCtx.state);
    v.U("msg_mode", play->msgCtx.msgMode);
    v.U("cs_state", play->csCtx.state);
    v.U("cs_frames", play->csCtx.frames);

    v.Section("camera");
    v.prefix = "camera.main.";
    const Camera* cam = &play->mainCamera;
    Vec(v, "at", cam->at);
    Vec(v, "eye", cam->eye);
    Vec(v, "up", cam->up);
    v.F("fov", cam->fov);
    v.S("setting", cam->setting);
    v.S("mode", cam->mode);

    v.Section("env");
    v.prefix = "env.";
    const EnvironmentContext* env = &play->envCtx;
    v.U("skybox_blend", env->skyboxBlend);
    v.U("skybox_disabled", env->skyboxDisabled);
    v.U("weather_cur", env->unk_17);
    v.U("weather_next", env->unk_18);
    v.U("indoors", env->indoors);
    v.U("outdoor_light", env->unk_1F);
    v.U("outdoor_light_prev", env->unk_20);
    Rot(v, "wind_dir", env->windDirection);
    v.F("wind_speed", env->windSpeed);
    v.U("indoor_light", env->unk_BD);
    v.U("gloomy_sky_mode", env->gloomySkyMode);
    v.U("lightning_mode", env->lightningMode);
    v.U("sandstorm_state", env->sandstormState);
    v.U("fill_screen", env->fillScreen);

    v.Section("actors");
    for (int cat = 0; cat < ACTORCAT_MAX; cat++) {
        v.prefix = "actors." + std::to_string(cat) + ".";
        v.U("count", play->actorCtx.actorLists[cat].length);
        int idx = 0;
        for (const Actor* a = play->actorCtx.actorLists[cat].head; a != nullptr; a = a->next, idx++) {
            v.prefix = "actor." + std::to_string(cat) + "." + std::to_string(idx) + ".";
            v.ActorHeader(v.prefix, a);
            VisitActor(v, a);
        }
    }
}

// Shared part of the save context. Per-client settings are skipped (PLAN.md 2.4 point 8 and
// docs/DETERMINISMO_AUDITORIA.md): language, audioSetting, zTargetSetting, the SoH gameplay stats
// (wall clock timestamps, timers kept per client) and filenameLanguage.
void VisitSaveContext(Visitor& v) {
    v.Section("save");
    v.prefix = "save.";
    const uint8_t* base = (const uint8_t*)&gSaveContext;
    const size_t lang = offsetof(SaveContext, language);
    const size_t audio = offsetof(SaveContext, audioSetting);
    const size_t ztarget = offsetof(SaveContext, zTargetSetting);
    const size_t shipOff = offsetof(SaveContext, ship);
    v.Bytes("main_0", base, lang);
    v.Bytes("main_1", base + audio + 1, ztarget - (audio + 1));
    v.Bytes("main_2", base + ztarget + 1, shipOff - (ztarget + 1));
    const ShipSaveContextData& ship = gSaveContext.ship;
    v.U("ship.pending_sale", ship.pendingSale);
    v.U("ship.pending_sale_mod", ship.pendingSaleMod);
    v.U("ship.pending_ice_traps", ship.pendingIceTrapCount);
    v.Bytes("ship.backup_fw", &ship.backupFW, sizeof(ship.backupFW));
    v.Bytes("ship.quest", &ship.quest, sizeof(ship.quest));
    v.U("ship.mask_memory", ship.maskMemory);
    v.Bytes("ship.randomizer_inf", ship.randomizerInf, sizeof(ship.randomizerInf));
    v.U("ship.reset_to_spawn", ship.resetToSpawn);
}

// Multiplayer simulation (ZmpPlayers.h): per-player blocks, owners, deterministic audio counters.
void VisitZmp(Visitor& v) {
    // Only in lockstep: recordings of phase 1 keep their hashes.
    if (!gZmpSim.enabled) {
        return;
    }
    v.Section("zmp");
    v.prefix = "zmp.";
    v.U("audio_tasks", gZmpSim.audioTaskCount);
    v.U("audio_random", gZmpSim.audioRandom);
    v.U("metronome_at", gZmpSim.metronomeAt);
    v.S("anchor", gZmpSim.anchor);
    v.S("ctx", gZmpSim.ctx);
    v.S("msg_owner", gZmpSim.msgOwner);
    v.S("msg_loaded", gZmpSim.msgLoaded);
    v.U("age_pending", gZmpSim.agePending);
    v.S("title_for", gZmpSim.titleFor);
    v.S("adj_owner", gZmpSim.adjOwner);
    v.U("refresh_pending", gZmpSim.refreshPending);
    v.S("rupee_debt", gZmpSim.rupeeDebt);
    v.S("pause_owner", gZmpSim.pauseOwner);
    v.S("transition_by", gZmpSim.transitionBy);
    v.U("clock_hold", gZmpSim.clockHold);
    v.U("exits_locked", gZmpSim.exitsLocked);
    v.S("warp_owner", gZmpSim.warpOwner);
    v.S("invite_by", gZmpSim.inviteBy);
    v.S("invite_entrance", gZmpSim.inviteEntrance);
    v.S("invite_ticks", gZmpSim.inviteTicks);
    v.S("invite_leave_in", gZmpSim.inviteLeaveIn);
    v.U("clock_hold_value", gZmpSim.clockHoldValue);
    v.U("shared_valid", gZmpSim.sharedValid);
    v.Bytes("shared_base", gZmpSim.sharedBase, gZmpSim.sharedValid ? gZmpSim.sharedSize : 0);
    v.U("group_defeat", gZmpSim.groupDefeat);
    v.S("effect_ss_search", EffectSs_ZmpGetSearchIndex()); // D-044
    v.S("last_magic_capacity", gZmpSim.lastMagicCapacity);
    v.Bytes("cam_scope", gZmpSim.camScope, sizeof(gZmpSim.camScope));
    v.Bytes("cam_creator", gZmpSim.camCreator, sizeof(gZmpSim.camCreator));
    v.U("global_cs", gZmpSim.globalCs);
    v.S("cs_trigger", gZmpSim.csTrigger);
    v.S("cs_starter", gZmpSim.csStarter);
    v.U("skip_requested", gZmpSim.skipRequested);
    v.U("extra_rooms", gZmpSim.extraRoomCount);
    for (int i = 0; i < gZmpSim.extraRoomCount; i++) {
        v.S("extra_room", gZmpSim.extraRooms[i].num);
    }
    v.S("loading_room", gZmpSim.loadingRoom);
    v.S("setup_room", gZmpSim.setupRoom);
    v.Bytes("queued_rooms", gZmpSim.queuedRooms, gZmpSim.queuedRoomCount);
    for (int k = 0; k < ZMP_MAX_PLAYERS; k++) {
        const ZmpPlayerSlot& s = gZmpSim.slots[k];
        v.prefix = "zmp.slot" + std::to_string(k) + ".";
        v.U("active", s.active);
        v.U("present", s.present);
        if (!s.present) {
            continue;
        }
        v.U("msg_mode", (u32)Zmp::Players::SlotMsgMode(k));
        bool live = k == gZmpSim.ctx && InPlay();
        ZmpPlayerBlock b = Zmp::Players::SlotBlock(k);
        v.S("health", b.health);
        v.S("health_acc", b.healthAccumulator);
        v.S("magic", b.magic);
        v.S("magic_state", b.magicState);
        v.S("magic_target", b.magicTarget);
        v.S("nayru_timer", b.nayrusLoveTimer);
        v.Bytes("buttons", b.equips.buttonItems, sizeof(b.equips.buttonItems));
        v.Bytes("c_slots", b.equips.cButtonSlots, sizeof(b.equips.cButtonSlots));
        v.U("equipment", b.equips.equipment);
        v.Bytes("ammo", b.ammo, sizeof(b.ammo));
        v.Bytes("bottles", b.bottles, sizeof(b.bottles));
        v.Bytes("button_status", b.buttonStatus, sizeof(b.buttonStatus));
        v.U("downed", s.downed);
        v.S("spectate", s.spectate);
        v.S("revive_progress", s.reviveProgress);
        v.S("reviver", s.reviver);
        const Camera* cam = live ? &gPlayState->mainCamera : &s.camera;
        Vec(v, "cam.at", cam->at);
        Vec(v, "cam.eye", cam->eye);
        v.S("cam.setting", cam->setting);
        v.S("cam.mode", cam->mode);
        const Input* in = live ? &gPlayState->state.input[0] : &s.input;
        v.U("input.buttons", in->cur.button);
        v.S("input.stick_x", in->cur.stick_x);
        v.S("input.stick_y", in->cur.stick_y);
        v.U("player_actor", s.player != nullptr ? (uint64_t)s.player->actor.id : 0xFFFF);
        v.S("active_cam", live ? gPlayState->activeCamera : s.activeCam);
        v.S("cam.status", cam->status);
        v.S("room", s.room);
        v.S("heat_state", s.heatState);
        v.S("heat_seconds", s.heatSeconds);
        v.S("heat_ticks", s.heatTicks);
        v.S("heat_preview", s.heatPreview);
        v.S("invite_hold", s.inviteHold);
        v.S("room2", s.room2);
        v.U("warp_pending", s.warpPending);
        // (dump only: the view the last draw used for this player's screen, which decides the actors' culling flag)
        if (const float* vp = Zmp::Players::SlotViewProjectionForDump(k)) {
            for (int i = 0; i < 16; i++) {
                v.Info(("~vp" + std::to_string(i)).c_str(), vp[i]);
            }
        }
        // (and what that matrix is made from: the slot's own view when it has one, else the canonical one)
        const View* pv = InPlay() ? Zmp::Players::LocalPicture(gPlayState, k) : nullptr;
        if (pv == nullptr && InPlay()) {
            pv = &gPlayState->view;
        }
        if (pv != nullptr) {
            v.Info("~view.fovy", pv->fovy);
            v.Info("~view.scale", pv->scale);
            v.Info("~view.near", pv->zNear);
            v.Info("~view.vp_top", (float)pv->viewport.topY);
            v.Info("~view.vp_bottom", (float)pv->viewport.bottomY);
            v.Info("~view.vp_left", (float)pv->viewport.leftX);
            v.Info("~view.vp_right", (float)pv->viewport.rightX);
        }
    }
}

void VisitAll(Visitor& v, uint32_t tick) {
    v.Section("tick");
    v.prefix = "";
    v.U("tick", tick);
    u32 randFloat = 0;
    u32 randInt = Rand_ZmpGetState(&randFloat);
    v.U("rng.int", randInt);
    v.U("rng.float", randFloat);
    // Which game state runs (as a stable number, not the address of its main function).
    uint64_t state = 0;
    if (gGameState != nullptr) {
        state = gGameState->main == Play_Main ? 1 : (gGameState->main == FileChoose_Main ? 2 : 3);
    }
    v.U("game_state", state);
    if (InPlay()) {
        VisitPlay(v);
    }
    VisitSaveContext(v);
    VisitZmp(v);
}

class HashVisitor : public Visitor {
  public:
    uint64_t h = 0xCBF29CE484222325ULL;

    void Mix(const void* p, size_t n) {
        const uint8_t* b = (const uint8_t*)p;
        for (size_t i = 0; i < n; i++) {
            h ^= b[i];
            h *= 0x100000001B3ULL;
        }
    }
    void U(const char*, uint64_t x) override {
        Mix(&x, sizeof(x));
    }
    void S(const char*, int64_t x) override {
        Mix(&x, sizeof(x));
    }
    void F(const char*, float x) override {
        uint32_t bits;
        memcpy(&bits, &x, sizeof(bits));
        Mix(&bits, sizeof(bits));
    }
    void Bytes(const char*, const void* p, size_t n) override {
        Mix(p, n);
    }
};

class DumpVisitor : public Visitor {
  public:
    FILE* f;
    explicit DumpVisitor(FILE* file) : f(file) {
    }
    void Line(const char* key, const char* fmt, ...) {
        fprintf(f, "%s%s = ", prefix.c_str(), key);
        va_list ap;
        va_start(ap, fmt);
        vfprintf(f, fmt, ap);
        va_end(ap);
        fputc('\n', f);
    }
    void U(const char* key, uint64_t x) override {
        Line(key, "%" PRIu64 " (0x%" PRIX64 ")", x, x);
    }
    void S(const char* key, int64_t x) override {
        Line(key, "%" PRId64, x);
    }
    void Info(const char* key, float x) override {
        F(key, x);
    }
    void F(const char* key, float x) override {
        uint32_t bits;
        memcpy(&bits, &x, sizeof(bits));
        Line(key, "%.9g (0x%08X)", x, bits);
    }
    void Bytes(const char* key, const void* p, size_t n) override {
        // One line per 32 bytes so the diff points at a small range; key carries the offset.
        const uint8_t* b = (const uint8_t*)p;
        size_t structOff = (const uint8_t*)p >= (const uint8_t*)&gSaveContext &&
                                   (const uint8_t*)p < (const uint8_t*)&gSaveContext + sizeof(gSaveContext)
                               ? (size_t)((const uint8_t*)p - (const uint8_t*)&gSaveContext)
                               : 0;
        for (size_t i = 0; i < n; i += 32) {
            fprintf(f, "%s%s@0x%04zX = ", prefix.c_str(), key, structOff + i);
            for (size_t j = i; j < n && j < i + 32; j++) {
                fprintf(f, "%02X", b[j]);
            }
            fputc('\n', f);
        }
    }
    void Section(const std::string& name) override {
        fprintf(f, "# %s\n", name.c_str());
    }
    void ActorHeader(const std::string& pfx, const Actor* a) override {
        const char* actorName = "?";
        if (a->id >= 0 && a->id < ActorDB::Instance->GetEntryCount()) {
            actorName = ActorDB::Instance->RetrieveEntry(a->id).name.c_str();
        }
        fprintf(f, "%sname = %s\n", pfx.c_str(), actorName);
    }
};

} // namespace

uint64_t HashState(uint32_t tick) {
    HashVisitor v;
    VisitAll(v, tick);
    return v.h;
}

bool DumpState(const std::string& path, uint32_t tick, std::string* err) {
    FILE* f = fopen(path.c_str(), "w");
    if (f == nullptr) {
        if (err) {
            *err = "cannot open " + path;
        }
        return false;
    }
    fprintf(f, "# ZMP state dump v1\n");
    fprintf(f, "hash = %016" PRIX64 "\n", HashState(tick));
    DumpVisitor v(f);
    VisitAll(v, tick);
    fclose(f);
    return true;
}

} // namespace Zmp::Sim

extern "C" uint64_t Zmp_StateHash(void) {
    return Zmp::Sim::HashState(Zmp::Sim::CurrentTick());
}

extern "C" int32_t Zmp_DumpState(const char* path) {
    std::string err;
    return Zmp::Sim::DumpState(path, Zmp::Sim::CurrentTick(), &err) ? 0 : -1;
}
