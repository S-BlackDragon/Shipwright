// ZMP: several rooms loaded at once (PLAN.md 2.10, phase 4). See ZmpPlayers.h.
//
// The engine keeps at most two rooms: curRoom and prevRoom (the one being left through a door). In the multiplayer
// simulation every player has its room (gZmpSim.slots[k].room) and a room stays loaded while somebody stands in it:
//   - The rooms the engine does not hold are kept in gZmpSim.extraRooms: drawn, their actors alive, their doors
//     (transition actors) spawned.
//   - While code runs for player k, curRoom is k's room (the context switch swaps it in): the room of what a player's
//     actors spawn, the room clear flags, the room type (heat, water, idle animations) and the respawn point are that
//     player's, as in the original with one player.
//   - Opening a door into a loaded room only moves the player; into a new room loads it as the original does (the
//     previous room stays for whoever is in it). Finishing a room change unloads only the rooms nobody stands in.
//   - The objects of a new room are added to the loaded ones instead of replacing them (the rooms still loaded need
//     theirs). The object bank has 128 slots in SoH and objects take no arena memory.
// Everything here is simulation state (gZmpSim travels in the portable state and is hashed).

#include "ZmpPlayers.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "soh/Zmp/ZmpLog.h"

extern "C" {
#include "variables.h"
#include "functions.h"
#include "macros.h"
extern PlayState* gPlayState;
s32 OTRRoom_RequestNewRoom(PlayState* play, RoomContext* roomCtx, s32 roomNum);
void Actor_Destroy(Actor* actor, PlayState* play);
}

namespace {

bool sSpawningSetup = false;
bool sInternalRequest = false;

int Ctx() {
    return (gZmpSim.ctx >= 0 && gZmpSim.ctx < ZMP_MAX_PLAYERS) ? gZmpSim.ctx : gZmpSim.anchor;
}

bool IsLoadedStruct(const Room& r) {
    return r.num >= 0 && (r.segment != nullptr || (gZmpSim.loadingRoom == r.num));
}

Room* FindLoaded(RoomContext* rc, int num) {
    if (num < 0) {
        return nullptr;
    }
    if (rc->curRoom.num == num && IsLoadedStruct(rc->curRoom)) {
        return &rc->curRoom;
    }
    if (rc->prevRoom.num == num && IsLoadedStruct(rc->prevRoom)) {
        return &rc->prevRoom;
    }
    for (int i = 0; i < gZmpSim.extraRoomCount; i++) {
        if (gZmpSim.extraRooms[i].num == num) {
            return &gZmpSim.extraRooms[i];
        }
    }
    return nullptr;
}

bool Occupied(int room) {
    if (room < 0) {
        return false;
    }
    for (int k = 0; k < ZMP_MAX_PLAYERS; k++) {
        if (Zmp::Players::IsPresent(k) && (gZmpSim.slots[k].room == room || gZmpSim.slots[k].room2 == room)) {
            return true;
        }
    }
    return false;
}

bool sPreload = false; // the room being requested is the far side of a passage: the player stays in its own

void SwapRooms(Room* a, Room* b) {
    if (a == b) {
        return;
    }
    Room t = *a;
    *a = *b;
    *b = t;
}

void PushExtra(const Room& r) {
    if (gZmpSim.extraRoomCount >= ZMP_MAX_EXTRA_ROOMS) {
        Zmp::Log("zmp: rooms: no place to keep room " + std::to_string(r.num));
        return;
    }
    gZmpSim.extraRooms[gZmpSim.extraRoomCount++] = r;
}

bool Loaded(PlayState* play, int room) {
    return FindLoaded(&play->roomCtx, room) != nullptr;
}

std::string RoomList(PlayState* play) {
    std::string s = std::to_string(play->roomCtx.curRoom.num) + "," + std::to_string(play->roomCtx.prevRoom.num);
    for (int i = 0; i < gZmpSim.extraRoomCount; i++) {
        s += "," + std::to_string(gZmpSim.extraRooms[i].num);
    }
    return s;
}

// Kills the actors of the rooms that are no longer loaded (func_80031B14 with the kept rooms).
void KillUnloadedActors(PlayState* play) {
    ActorContext* actorCtx = &play->actorCtx;
    for (int i = 0; i < ACTORCAT_MAX; i++) {
        Actor* actor = actorCtx->actorLists[i].head;
        while (actor != nullptr) {
            if (actor->room >= 0 && !Loaded(play, actor->room)) {
                if (!actor->isDrawn) {
                    actor = Actor_Delete(actorCtx, actor, play);
                } else {
                    Actor_Kill(actor);
                    Actor_Destroy(actor, play);
                    actor = actor->next;
                }
            } else {
                actor = actor->next;
            }
        }
    }
}

} // namespace

extern "C" s32 Zmp_RoomRequest(PlayState* play, RoomContext* rc, s32 roomNum) {
    if (!Zmp_MultiActive() || roomNum < 0 || roomNum >= play->numRooms) {
        return -1;
    }
    if (sInternalRequest) {
        // A queued request: the engine moves curRoom to prevRoom; keep the old prevRoom if somebody is there.
        if (rc->prevRoom.num >= 0 && rc->prevRoom.num != rc->curRoom.num && Occupied(rc->prevRoom.num)) {
            PushExtra(rc->prevRoom);
        }
        gZmpSim.loadingRoom = (s8)roomNum;
        return -1;
    }
    int k = Ctx();
    if (sPreload) {
        if (Loaded(play, roomNum)) {
            return 1;
        }
        if (rc->status != 0) {
            bool queued = false;
            for (int i = 0; i < gZmpSim.queuedRoomCount; i++) {
                queued = queued || gZmpSim.queuedRooms[i] == roomNum;
            }
            if (!queued && gZmpSim.queuedRoomCount < ZMP_MAX_QUEUED_ROOMS) {
                gZmpSim.queuedRooms[gZmpSim.queuedRoomCount++] = (s8)roomNum;
            }
            return 1;
        }
        if (rc->prevRoom.num >= 0 && rc->prevRoom.num != rc->curRoom.num && Occupied(rc->prevRoom.num)) {
            PushExtra(rc->prevRoom);
        }
        gZmpSim.loadingRoom = (s8)roomNum;
        Zmp::Log("zmp: rooms: room " + std::to_string(roomNum) + " loads for slot " + std::to_string(k) +
                 ", who stands at the passage to it (loaded " + RoomList(play) + ")");
        return -1;
    }
    if (Loaded(play, roomNum)) {
        // Already loaded (another player is there, or it is loading): this player is in it now.
        Zmp::Players::SetSlotRoom(k, roomNum);
        Zmp::Players::ArrangeRooms(play, k);
        Zmp::Log("zmp: rooms: slot " + std::to_string(k) + " -> room " + std::to_string(roomNum) + " (already loaded)");
        return 1;
    }
    if (rc->status != 0) {
        // Another room is loading this tick: this one loads right after (Zmp_SetupSpawnEnd).
        bool queued = false;
        for (int i = 0; i < gZmpSim.queuedRoomCount; i++) {
            queued = queued || gZmpSim.queuedRooms[i] == roomNum;
        }
        if (!queued && gZmpSim.queuedRoomCount < ZMP_MAX_QUEUED_ROOMS) {
            gZmpSim.queuedRooms[gZmpSim.queuedRoomCount++] = (s8)roomNum;
        }
        Zmp::Players::SetSlotRoom(k, roomNum);
        Zmp::Log("zmp: rooms: slot " + std::to_string(k) + " -> room " + std::to_string(roomNum) + " (queued)");
        return 1;
    }
    // The engine moves curRoom (this player's room) to prevRoom; the old prevRoom stays loaded if somebody is there.
    if (rc->prevRoom.num >= 0 && rc->prevRoom.num != rc->curRoom.num && Occupied(rc->prevRoom.num)) {
        PushExtra(rc->prevRoom);
    }
    Zmp::Players::SetSlotRoom(k, roomNum);
    gZmpSim.loadingRoom = (s8)roomNum;
    Zmp::Log("zmp: rooms: slot " + std::to_string(k) + " -> room " + std::to_string(roomNum) + " (load; loaded " +
             RoomList(play) + ")");
    return -1;
}

extern "C" void Zmp_HollNear(PlayState* play, s32 room, s32 otherRoom) {
    int k = Ctx();
    if (k < 0 || k >= ZMP_MAX_PLAYERS) {
        return;
    }
    ZmpPlayerSlot& s = gZmpSim.slots[k];
    bool other = s.room2 >= 0 && s.room2 != otherRoom; // (it was at another passage a moment ago)
    s.room2 = (s16)otherRoom;
    s.room2Fresh = 3;
    if (!Loaded(play, otherRoom)) {
        sPreload = true;
        Room_RequestNewRoom(play, &play->roomCtx, otherRoom);
        sPreload = false;
    }
    if (s.room != room && Loaded(play, room) && play->roomCtx.status == 0) {
        Zmp::Players::SetSlotRoom(k, room);
        Zmp::Players::ArrangeRooms(play, k);
        other = true;
    }
    if (other && play->roomCtx.status == 0) {
        Room_FinishRoomChange(play, &play->roomCtx);
    }
}

// Every tick: a player who is no longer at a passage (it walked off sideways, was moved, fell) stops keeping the room
// on its other side loaded.
extern "C" void Zmp_HollTick(PlayState* play) {
    bool dropped = false;
    for (int k = 0; k < ZMP_MAX_PLAYERS; k++) {
        ZmpPlayerSlot& s = gZmpSim.slots[k];
        if (s.room2 >= 0 && (!Zmp::Players::IsPresent(k) || --s.room2Fresh <= 0)) {
            s.room2 = -1;
            dropped = true;
        }
    }
    if (dropped && play->roomCtx.status == 0) {
        Room_FinishRoomChange(play, &play->roomCtx);
    }
}

extern "C" void Zmp_HollSettle(PlayState* play, s32 room) {
    int k = Ctx();
    if (k < 0 || k >= ZMP_MAX_PLAYERS) {
        return;
    }
    ZmpPlayerSlot& s = gZmpSim.slots[k];
    bool changed = s.room2 >= 0 || s.room != room;
    s.room2 = -1;
    if (s.room != room) {
        if (Loaded(play, room)) {
            Zmp::Players::SetSlotRoom(k, room);
            Zmp::Players::ArrangeRooms(play, k);
        } else {
            Room_RequestNewRoom(play, &play->roomCtx, room);
            return;
        }
    }
    if (changed && play->roomCtx.status == 0) {
        Room_FinishRoomChange(play, &play->roomCtx); // (rooms nobody is in or looking into are unloaded)
    }
}

extern "C" s32 Zmp_NextPresentSlot(s32 after) {
    for (int k = after + 1; k < ZMP_MAX_PLAYERS; k++) {
        if (Zmp::Players::IsPresent(k)) {
            return k;
        }
    }
    return -1;
}

extern "C" void Zmp_RoomLoadBegin(PlayState* play, RoomContext* rc) {
    if (!Zmp_MultiActive() || rc->status != 1 || gZmpSim.loadingRoom < 0) {
        return;
    }
    // The room being loaded completes in curRoom (the context switches may have moved it).
    Room* r = FindLoaded(rc, gZmpSim.loadingRoom);
    if (r != nullptr && r != &rc->curRoom) {
        SwapRooms(r, &rc->curRoom);
    }
}

extern "C" void Zmp_RoomLoadEnd(PlayState* play, RoomContext* rc) {
    if (!Zmp_MultiActive()) {
        return;
    }
    gZmpSim.setupRoom = (s8)rc->curRoom.num;
    gZmpSim.loadingRoom = -1;
    Zmp::Players::ArrangeRooms(play, Ctx());
}

extern "C" s32 Zmp_RoomFinish(PlayState* play, RoomContext* rc) {
    if (!Zmp_MultiActive()) {
        return 0;
    }
    int k = Ctx();
    // Loaded rooms, the ones to keep first.
    std::vector<Room> keep;
    std::vector<int> dropped;
    auto consider = [&](const Room& r) {
        if (r.num < 0) {
            return;
        }
        for (auto& x : keep) {
            if (x.num == r.num) {
                return;
            }
        }
        if (Occupied(r.num) || (rc->status == 1 && r.num == gZmpSim.loadingRoom)) {
            keep.push_back(r);
        } else {
            dropped.push_back(r.num);
        }
    };
    consider(rc->curRoom);
    consider(rc->prevRoom);
    for (int i = 0; i < gZmpSim.extraRoomCount; i++) {
        consider(gZmpSim.extraRooms[i]);
    }
    if (dropped.empty()) {
        return 1; // somebody is in every loaded room: nothing to unload
    }
    if (keep.empty()) {
        keep.push_back(rc->curRoom); // nobody tracked (should not happen): keep the engine's current room
    }
    // curRoom: this player's room when kept.
    int first = 0;
    for (int i = 0; i < (int)keep.size(); i++) {
        if (keep[i].num == gZmpSim.slots[k].room) {
            first = i;
        }
    }
    std::swap(keep[0], keep[first]);
    rc->curRoom = keep[0];
    if (keep.size() > 1) {
        rc->prevRoom = keep[1];
    } else {
        rc->prevRoom.num = -1;
        rc->prevRoom.segment = nullptr;
    }
    gZmpSim.extraRoomCount = 0;
    for (size_t i = 2; i < keep.size(); i++) {
        PushExtra(keep[i]);
    }
    std::string list;
    for (int d : dropped) {
        list += (list.empty() ? "" : ",") + std::to_string(d);
    }
    Zmp::Log("zmp: rooms: unload " + list + " (loaded " + RoomList(play) + ")");

    // What the original's room change finish does, for the rooms that went away.
    KillUnloadedActors(play);
    CollisionCheck_ClearContext(play, &play->colChkCtx);
    u32 keepMask = 0;
    for (auto& r : keep) {
        if (r.num >= 0 && r.num < 32) {
            keepMask |= 1u << r.num;
        }
    }
    play->actorCtx.flags.tempClear &= keepMask;
    if (keep.size() == 1) {
        play->actorCtx.flags.tempSwch &= 0xFFFFFF;
        play->msgCtx.unk_E3F4 = 0;
    }
    Actor_SpawnTransitionActors(play, &play->actorCtx);
    Map_InitRoomData(play, rc->curRoom.num);
    if (!((play->sceneNum >= SCENE_HYRULE_FIELD) && (play->sceneNum <= SCENE_LON_LON_RANCH))) {
        Map_SavePlayerInitialInfo(play);
    }
    if (k == Zmp::Players::LocalSlot()) {
        Audio_SetEnvReverb(rc->curRoom.echo);
    }
    return 1;
}

extern "C" s32 Zmp_RoomKept(s32 room) {
    if (!Zmp_MultiActive() || room < 0) {
        return 0;
    }
    for (int i = 0; i < gZmpSim.extraRoomCount; i++) {
        if (gZmpSim.extraRooms[i].num == room) {
            return 1;
        }
    }
    return 0;
}

extern "C" s8 Zmp_SpawnRoom(PlayState* play) {
    if (!Zmp_MultiActive()) {
        return play->roomCtx.curRoom.num;
    }
    if (sSpawningSetup && gZmpSim.setupRoom >= 0) {
        return gZmpSim.setupRoom;
    }
    // What an actor spawns belongs to its room (with one room loaded that is always curRoom).
    const Actor* a = Zmp::Players::CurrentActor();
    if (a != nullptr && a->room >= 0 && Loaded(play, a->room)) {
        return a->room;
    }
    return play->roomCtx.curRoom.num;
}

extern "C" void Zmp_SetupSpawnBegin(PlayState* play) {
    sSpawningSetup = Zmp_MultiActive() != 0;
}

extern "C" void Zmp_SetupSpawnEnd(PlayState* play) {
    sSpawningSetup = false;
    if (!Zmp_MultiActive()) {
        return;
    }
    gZmpSim.setupRoom = -1;
    // A room requested while another one was loading loads now (its actors spawn next tick).
    RoomContext* rc = &play->roomCtx;
    while (gZmpSim.queuedRoomCount > 0 && rc->status == 0) {
        int q = gZmpSim.queuedRooms[0];
        for (int i = 1; i < gZmpSim.queuedRoomCount; i++) {
            gZmpSim.queuedRooms[i - 1] = gZmpSim.queuedRooms[i];
        }
        gZmpSim.queuedRoomCount--;
        if (Loaded(play, q) || !Occupied(q)) {
            continue;
        }
        sInternalRequest = true;
        OTRRoom_RequestNewRoom(play, rc, q);
        sInternalRequest = false;
        Zmp::Log("zmp: rooms: queued room " + std::to_string(q) + " loads (loaded " + RoomList(play) + ")");
        break;
    }
}

extern "C" void Zmp_DrawExtraRooms(PlayState* play, u32 flags) {
    if (!Zmp_MultiActive()) {
        return;
    }
    for (int i = 0; i < gZmpSim.extraRoomCount; i++) {
        Room_Draw(play, &gZmpSim.extraRooms[i], flags);
    }
}

extern "C" s32 Zmp_EnemyRemoved(PlayState* play, Actor* actor) {
    if (!Zmp_MultiActive()) {
        return 0;
    }
    // The last enemy of a loaded room clears that room (the original: the last enemy of the scene, in curRoom).
    if (actor->category != ACTORCAT_ENEMY || actor->room < 0 || !Loaded(play, actor->room)) {
        return 1;
    }
    for (Actor* a = play->actorCtx.actorLists[ACTORCAT_ENEMY].head; a != nullptr; a = a->next) {
        if (a->room == actor->room || a->room < 0) {
            return 1;
        }
    }
    Flags_SetTempClear(play, actor->room);
    return 1;
}

extern "C" s32 Zmp_KeepObjects(void) {
    return (Zmp_MultiActive() && Zmp::Players::PresentCount() >= 2) ? 1 : 0;
}

// ---------------------------------------------------------------------------------------------------
// Room ambience (presentation)

namespace {

struct LocalLight {
    bool valid = false;
    int index = -1;
    int prev = -1;
    float blend = 1.0f;
    u32 frame = 0xFFFFFFFF; // gameplayFrames of the last update
};
LocalLight sLocal;

struct SavedLight {
    bool active = false;
    u8 ambient[3];
    u8 fog[3];
    s16 fogNear;
    s16 fogFar;
    LightInfo dir1;
    LightInfo dir2;
};
SavedLight sSaved;
bool sHideAdj = false; // this frame's picture ignores another player's own light effect (tests read it)

u8 Clamp8(s16 v) {
    return (u8)(v > 255 ? 255 : (v < 0 ? 0 : v));
}

// Same rules as func_80074CE8 and the indoor blend of Environment_Update, for the local player's floor.
void StepLocalLight(PlayState* play) {
    int L = Zmp::Players::LocalSlot();
    Player* p = Zmp::Players::SlotPlayer(L);
    EnvironmentContext* env = &play->envCtx;
    // (SoH does not fill envCtx.numLightSettings: the light setting index is used as the floor gives it.)
    if (p == nullptr || L == gZmpSim.anchor || env->lightSettingsList == nullptr) {
        sLocal.valid = false;
        return;
    }
    if (sLocal.frame == play->gameplayFrames) {
        return; // once per tick
    }
    sLocal.frame = play->gameplayFrames;
    if (!sLocal.valid) {
        sLocal.valid = true;
        sLocal.index = sLocal.prev = env->unk_BD;
        sLocal.blend = 1.0f;
    }
    if (p->actor.floorBgId == BGCHECK_SCENE && p->actor.floorPoly != nullptr) {
        int idx = (int)SurfaceType_GetLightSettingIndex(&play->colCtx, p->actor.floorPoly, BGCHECK_SCENE);
        if (idx > 30) {
            idx = 0;
        }
        if (idx != sLocal.index && sLocal.blend >= 1.0f) {
            sLocal.prev = sLocal.index;
            sLocal.index = idx;
            sLocal.blend = 0.0f;
        }
    }
    u8 rate = (env->lightSettingsList[sLocal.index].fogNear >> 0xA) * 4;
    if (rate == 0) {
        rate = 1;
    }
    sLocal.blend = std::min(1.0f, sLocal.blend + rate / 255.0f);
}

} // namespace

extern "C" s32 Zmp_DrivesLighting(Player* player) {
    if (!Zmp_MultiActive()) {
        return 1;
    }
    return Zmp::Players::SlotOf(&player->actor) == gZmpSim.anchor ? 1 : 0;
}

extern "C" s32 Zmp_IsLocalAudioPlayer(Player* player) {
    if (!Zmp_MultiActive()) {
        return 1;
    }
    return Zmp::Players::SlotOf(&player->actor) == Zmp::Players::LocalSlot() ? 1 : 0;
}

extern "C" void Zmp_DrawLightBegin(PlayState* play) {
    sSaved.active = false;
    if (!Zmp_MultiActive()) {
        sLocal.valid = false;
        return;
    }
    StepLocalLight(play);
    EnvironmentContext* env = &play->envCtx;
    LightContext* lc = &play->lightCtx;
    // Phase 5b: the darkening of another player's own effect (the fairy that revives it, its spin attack) is not
    // drawn here.
    int owner = gZmpSim.adjOwner;
    bool adjusted = env->adjFogNear != 0 || env->adjFogFar != 0;
    for (int i = 0; i < 3; i++) {
        adjusted = adjusted || env->adjAmbientColor[i] != 0 || env->adjLight1Color[i] != 0 || env->adjFogColor[i] != 0;
    }
    bool hideAdj = adjusted && owner >= 0 && owner < ZMP_MAX_PLAYERS && owner != Zmp::Players::LocalSlot() &&
                   Zmp::Players::LocalSlot() >= 0;
    sHideAdj = hideAdj;
    // Only the indoor light settings (picked by the floor) change per room; outdoor lighting follows the time of day.
    bool ownRoom = sLocal.valid && env->indoors && env->unk_BF == 0xFF &&
                   !(sLocal.index == env->unk_BD && sLocal.prev == env->unk_BE && sLocal.blend == env->unk_D8);
    if (!ownRoom && !hideAdj) {
        return;
    }
    sSaved.active = true;
    if (!ownRoom) {
        // the scene's lights as the simulation has them, before the adjustment (z_kankyo.c, Environment_Update)
        memcpy(sSaved.ambient, lc->ambientColor, 3);
        memcpy(sSaved.fog, lc->fogColor, 3);
        sSaved.fogNear = lc->fogNear;
        sSaved.fogFar = lc->fogFar;
        sSaved.dir1 = env->dirLight1;
        sSaved.dir2 = env->dirLight2;
        for (int i = 0; i < 3; i++) {
            lc->ambientColor[i] = env->lightSettings.ambientColor[i];
            env->dirLight1.params.dir.color[i] = env->lightSettings.light1Color[i];
            env->dirLight2.params.dir.color[i] = env->lightSettings.light2Color[i];
            lc->fogColor[i] = env->lightSettings.fogColor[i];
        }
        lc->fogNear = env->lightSettings.fogNear <= 996 ? env->lightSettings.fogNear : 996;
        lc->fogFar = env->lightSettings.fogFar <= 12800 ? env->lightSettings.fogFar : 12800;
        return;
    }
    s16 adjOn = hideAdj ? 0 : 1;
    memcpy(sSaved.ambient, lc->ambientColor, 3);
    memcpy(sSaved.fog, lc->fogColor, 3);
    sSaved.fogNear = lc->fogNear;
    sSaved.fogFar = lc->fogFar;
    sSaved.dir1 = env->dirLight1;
    sSaved.dir2 = env->dirLight2;
    const EnvLightSettings& a = env->lightSettingsList[sLocal.prev];
    const EnvLightSettings& b = env->lightSettingsList[sLocal.index];
    f32 t = sLocal.blend;
    for (int i = 0; i < 3; i++) {
        lc->ambientColor[i] =
            Clamp8((s16)(LERP(a.ambientColor[i], b.ambientColor[i], t) + adjOn * env->adjAmbientColor[i]));
        env->dirLight1.params.dir.color[i] =
            Clamp8((s16)(LERP(a.light1Color[i], b.light1Color[i], t) + adjOn * env->adjLight1Color[i]));
        env->dirLight2.params.dir.color[i] =
            Clamp8((s16)(LERP(a.light2Color[i], b.light2Color[i], t) + adjOn * env->adjLight1Color[i]));
        lc->fogColor[i] = Clamp8((s16)(LERP(a.fogColor[i], b.fogColor[i], t) + adjOn * env->adjFogColor[i]));
    }
    env->dirLight1.params.dir.x = (s8)LERP16(a.light1Dir[0], b.light1Dir[0], t);
    env->dirLight1.params.dir.y = (s8)LERP16(a.light1Dir[1], b.light1Dir[1], t);
    env->dirLight1.params.dir.z = (s8)LERP16(a.light1Dir[2], b.light1Dir[2], t);
    env->dirLight2.params.dir.x = (s8)LERP16(a.light2Dir[0], b.light2Dir[0], t);
    env->dirLight2.params.dir.y = (s8)LERP16(a.light2Dir[1], b.light2Dir[1], t);
    env->dirLight2.params.dir.z = (s8)LERP16(a.light2Dir[2], b.light2Dir[2], t);
    s16 fogNear = (s16)(LERP16(a.fogNear & 0x3FF, b.fogNear & 0x3FF, t) + adjOn * env->adjFogNear);
    s16 fogFar = (s16)(LERP16(a.fogFar, b.fogFar, t) + adjOn * env->adjFogFar);
    lc->fogNear = fogNear <= 996 ? fogNear : 996;
    lc->fogFar = fogFar <= 12800 ? fogFar : 12800;
}

extern "C" void Zmp_DrawLightEnd(PlayState* play) {
    if (!sSaved.active) {
        return;
    }
    LightContext* lc = &play->lightCtx;
    memcpy(lc->ambientColor, sSaved.ambient, 3);
    memcpy(lc->fogColor, sSaved.fog, 3);
    lc->fogNear = sSaved.fogNear;
    lc->fogFar = sSaved.fogFar;
    play->envCtx.dirLight1 = sSaved.dir1;
    play->envCtx.dirLight2 = sSaved.dir2;
    sSaved.active = false;
}

extern "C" f32 Zmp_SimFogFar(PlayState* play) {
    return sSaved.active ? (f32)sSaved.fogFar : (f32)play->lightCtx.fogFar;
}

namespace Zmp::Players {

bool LightEffectHidden() {
    return sHideAdj;
}

int LocalLightSetting() {
    return sLocal.valid ? sLocal.index : -1;
}

void ArrangeRooms(PlayState* play, int slot) {
    if (play == nullptr || slot < 0 || slot >= ZMP_MAX_PLAYERS) {
        return;
    }
    int r = gZmpSim.slots[slot].room;
    RoomContext* rc = &play->roomCtx;
    if (r >= 0 && rc->curRoom.num != r) {
        Room* p = FindLoaded(rc, r);
        if (p != nullptr) {
            SwapRooms(p, &rc->curRoom);
        }
    }
    // Phase 5b: the other loaded rooms in a fixed order (by number), whatever order the contexts were visited in:
    // each machine also arranges the rooms for its own player while it draws, and with three or more rooms the
    // previous room and the kept ones ended up exchanged between machines (same rooms, different hash).
    if (gZmpSim.extraRoomCount > 0) {
        std::vector<Room> others;
        bool prev = rc->prevRoom.num >= 0;
        if (prev) {
            others.push_back(rc->prevRoom);
        }
        for (int i = 0; i < gZmpSim.extraRoomCount; i++) {
            others.push_back(gZmpSim.extraRooms[i]);
        }
        std::stable_sort(others.begin(), others.end(), [](const Room& x, const Room& y) { return x.num < y.num; });
        size_t n = 0;
        if (prev) {
            rc->prevRoom = others[n++];
        }
        for (int i = 0; i < gZmpSim.extraRoomCount; i++) {
            gZmpSim.extraRooms[i] = others[n++];
        }
    }
}

std::vector<int> LoadedRooms(PlayState* play) {
    std::vector<int> out;
    if (play == nullptr) {
        return out;
    }
    if (play->roomCtx.curRoom.num >= 0) {
        out.push_back(play->roomCtx.curRoom.num);
    }
    if (play->roomCtx.prevRoom.num >= 0) {
        out.push_back(play->roomCtx.prevRoom.num);
    }
    for (int i = 0; i < gZmpSim.extraRoomCount; i++) {
        out.push_back(gZmpSim.extraRooms[i].num);
    }
    return out;
}

} // namespace Zmp::Players
