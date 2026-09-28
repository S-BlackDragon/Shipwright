// ZMP: several Player actors in one lockstep simulation. See ZmpPlayers.h for the model.

#include "ZmpPlayers.h"
#include "Session.h"

#include <cstring>
#include <string>
#include <vector>
#include <cstdio>

#include "soh/Zmp/ZmpLog.h"

extern "C" {
#include "variables.h"
#include "functions.h"
#include "macros.h"
extern PlayState* gPlayState;
void Attention_Init(TargetContext* targetCtx, Actor* actor, PlayState* play);
void Interface_UpdateMagicBar(PlayState* play);
void Player_SetEquipmentData(PlayState* play, Player* player);
void Player_SetBootData(PlayState* play, Player* player);
void Player_ZmpRevive(PlayState* play, Player* player);
void Attention_Update(TargetContext* targetCtx, Player* player, Actor* actorArg, PlayState* play);
void Player_ReleaseLockOn(Player* player);
}

extern "C" {
s32 gZmpRandTrace = 0;
ZmpSimState gZmpSim;
void* gZmpCtxPlayer = nullptr;
s32 gZmpCameraInterfaceMuted = 0;
}

namespace {

constexpr u32 kSimMagic = 0x33534D5A; // "ZMS3" (phase 3 layout)
int sLocalSlot = -1;

// Debug RNG trace (desync hunting): who drew random numbers this tick, in order (run-length compressed).
struct RandTraceEntry {
    char phase;
    s16 actorId;
    s16 params;
    uintptr_t caller;
    int count;
};
std::vector<RandTraceEntry> sRandTrace;
const Actor* sTraceActor = nullptr;
char sTracePhase = '-';

// Render helpers (not simulation state).
bool sDrawBegun = false;
View sSimView;
MtxF sSimVP;
MtxF sSimBillboard;

ZmpPlayerSlot& Slot(int k) {
    return gZmpSim.slots[k];
}

bool Valid(int k) {
    return k >= 0 && k < ZMP_MAX_PLAYERS;
}

bool Present(int k) {
    return Valid(k) && Slot(k).present && Slot(k).player != nullptr;
}

// gSaveContext -> block (the per-player fields of PLAN.md 2.6).
void SaveToBlock(ZmpPlayerBlock& b) {
    b.health = gSaveContext.health;
    b.healthAccumulator = gSaveContext.healthAccumulator;
    b.magic = gSaveContext.magic;
    b.magicState = gSaveContext.magicState;
    b.prevMagicState = gSaveContext.prevMagicState;
    b.magicFillTarget = gSaveContext.magicFillTarget;
    b.magicTarget = gSaveContext.magicTarget;
    b.nayrusLoveTimer = gSaveContext.nayrusLoveTimer;
    b.equips = gSaveContext.equips;
    memcpy(b.ammo, gSaveContext.inventory.ammo, sizeof(b.ammo));
    for (int i = 0; i < 4; i++) {
        b.bottles[i] = gSaveContext.inventory.items[SLOT_BOTTLE_1 + i];
    }
    memcpy(b.buttonStatus, gSaveContext.buttonStatus, sizeof(b.buttonStatus));
}

void LoadFromBlock(const ZmpPlayerBlock& b) {
    gSaveContext.health = b.health;
    gSaveContext.healthAccumulator = b.healthAccumulator;
    gSaveContext.magic = b.magic;
    gSaveContext.magicState = b.magicState;
    gSaveContext.prevMagicState = b.prevMagicState;
    gSaveContext.magicFillTarget = b.magicFillTarget;
    gSaveContext.magicTarget = b.magicTarget;
    gSaveContext.nayrusLoveTimer = b.nayrusLoveTimer;
    gSaveContext.equips = b.equips;
    memcpy(gSaveContext.inventory.ammo, b.ammo, sizeof(b.ammo));
    for (int i = 0; i < 4; i++) {
        gSaveContext.inventory.items[SLOT_BOTTLE_1 + i] = b.bottles[i];
    }
    memcpy(gSaveContext.buttonStatus, b.buttonStatus, sizeof(b.buttonStatus));
}

// A C / D-pad button that points at a bottle slot shows what that bottle holds.
void FixBottleButtons(ZmpPlayerBlock& b) {
    for (int i = 1; i < 8; i++) {
        u8 slot = b.equips.cButtonSlots[i - 1];
        if (slot >= SLOT_BOTTLE_1 && slot <= SLOT_BOTTLE_4) {
            b.equips.buttonItems[i] = b.bottles[slot - SLOT_BOTTLE_1];
        }
    }
}

void Park(PlayState* play, int k) {
    ZmpPlayerSlot& s = Slot(k);
    memcpy(&s.camera, &play->mainCamera, sizeof(Camera));
    memcpy(&s.target, &play->actorCtx.targetCtx, sizeof(TargetContext));
    u8 before[4];
    memcpy(before, s.block.bottles, sizeof(before));
    SaveToBlock(s.block);
    // A newly found bottle is progression: every other player gets it too (empty).
    for (int i = 0; i < 4; i++) {
        if (before[i] == ITEM_NONE && s.block.bottles[i] != ITEM_NONE) {
            for (int j = 0; j < ZMP_MAX_PLAYERS; j++) {
                if (j != k && Slot(j).active && Slot(j).block.bottles[i] == ITEM_NONE) {
                    Slot(j).block.bottles[i] = ITEM_BOTTLE;
                }
            }
        }
    }
    s.input = play->state.input[0];
}

void Load(PlayState* play, int k) {
    ZmpPlayerSlot& s = Slot(k);
    memcpy(&play->mainCamera, &s.camera, sizeof(Camera));
    memcpy(&play->actorCtx.targetCtx, &s.target, sizeof(TargetContext));
    LoadFromBlock(s.block);
    play->state.input[0] = s.input;
    gZmpCtxPlayer = s.player;
}

bool Downed(int k) {
    return Valid(k) && Slot(k).downed;
}

int LowestPresent() {
    for (int k = 0; k < ZMP_MAX_PLAYERS; k++) {
        if (Present(k)) {
            return k;
        }
    }
    return -1;
}

int LowestActive() {
    for (int k = 0; k < ZMP_MAX_PLAYERS; k++) {
        if (Slot(k).active) {
            return k;
        }
    }
    return -1;
}

void SwitchTo(PlayState* play, int k) {
    if (!Valid(k) || k == gZmpSim.ctx) {
        return;
    }
    if (Valid(gZmpSim.ctx)) {
        Park(play, gZmpSim.ctx);
    }
    Load(play, k);
    gZmpSim.ctx = (s8)k;
}

// Nearest present player, ties to the lowest slot. Pure function of simulation state.
int NearestSlot(const Vec3f& pos) {
    int best = -1;
    f32 bestD = 0.0f;
    for (int k = 0; k < ZMP_MAX_PLAYERS; k++) {
        if (!Present(k) || Slot(k).downed) { // downed players are not "the player" of any actor
            continue;
        }
        const Vec3f& p = Slot(k).player->actor.world.pos;
        f32 dx = pos.x - p.x;
        f32 dy = pos.y - p.y;
        f32 dz = pos.z - p.z;
        f32 d = dx * dx + dy * dy + dz * dz;
        if (best < 0 || d < bestD) {
            best = k;
            bestD = d;
        }
    }
    return best;
}

int ContextSlot(const Actor* actor) {
    int k = Zmp::Players::SlotOf(actor);
    if (k >= 0) {
        return k;
    }
    if (actor->parent != nullptr && actor->parent->id == ACTOR_PLAYER) {
        k = Zmp::Players::SlotOf(actor->parent);
        if (k >= 0) {
            return k;
        }
    }
    k = NearestSlot(actor->world.pos);
    return k >= 0 ? k : gZmpSim.anchor;
}

void StepPad(Input* in, const OSContPad& pad) {
    // Same arithmetic as PadMgr_ProcessInputs + PadMgr_RequestPadData (mode 1): press/rel of this tick.
    in->prev = in->cur;
    in->cur = pad;
    in->cur.err_no = 0;
    in->cur.gyro_x = 0.0f;
    in->cur.gyro_y = 0.0f;
    u32 diff = in->prev.button ^ in->cur.button;
    in->press = OSContPad{};
    in->rel = OSContPad{};
    in->press.button = (u16)(diff & in->cur.button);
    in->rel.button = (u16)(diff & in->prev.button);
    PadUtils_UpdateRelXY(in);
    in->press.stick_x = (s8)(in->cur.stick_x - in->prev.stick_x);
    in->press.stick_y = (s8)(in->cur.stick_y - in->prev.stick_y);
    PadUtils_UpdateRelRXY(in);
    in->press.right_stick_x = (s8)(in->cur.right_stick_x - in->prev.right_stick_x);
    in->press.right_stick_y = (s8)(in->cur.right_stick_y - in->prev.right_stick_y);
}

// Spawns the Player actor of slot k with its own camera and Z-target context, seeded from the anchor's.
Player* SpawnPlayerActor(PlayState* play, int k, Vec3f pos, s16 yaw, s16 params, s32 bgCamIndex) {
    int anchor = gZmpSim.anchor;
    ZmpPlayerSlot& s = Slot(k);
    memcpy(&s.camera, &play->mainCamera, sizeof(Camera));
    memcpy(&s.target, &play->actorCtx.targetCtx, sizeof(TargetContext));
    s.player = nullptr;
    s.present = 1;
    s.hasView = 0;
    SwitchTo(play, k);
    gZmpCtxPlayer = nullptr;
    gZmpSim.spawningSlot = (s8)k;
    Actor* a = Actor_Spawn(&play->actorCtx, play, ACTOR_PLAYER, pos.x, pos.y, pos.z, 0, yaw, 0, params);
    gZmpSim.spawningSlot = -1;
    Player* player = (Player*)a;
    if (player == nullptr || s.player != player) {
        Zmp::Log("zmp: slot " + std::to_string(k) + ": Player spawn failed");
        s.present = 0;
        s.player = nullptr;
        if (Present(anchor)) {
            SwitchTo(play, anchor);
        }
        return nullptr;
    }
    Camera_InitPlayerSettings(&play->mainCamera, player);
    Camera_RequestMode(&play->mainCamera, CAM_MODE_NORMAL);
    if (bgCamIndex != 0xFF && bgCamIndex >= 0) {
        Camera_ChangeDataIdx(&play->mainCamera, bgCamIndex);
    }
    Attention_Init(&play->actorCtx.targetCtx, &player->actor, play);
    if (Present(anchor)) {
        SwitchTo(play, anchor);
    }
    return player;
}

Vec3f SideOffset(const Vec3f& base, s16 yaw, int n) {
    // n-th extra player: alternately right and left of the reference, 45 units apart.
    f32 side = (n % 2 == 1) ? 1.0f : -1.0f;
    f32 dist = 45.0f * (f32)((n + 1) / 2);
    Vec3f out = base;
    out.x += Math_CosS(yaw) * side * dist;
    out.z -= Math_SinS(yaw) * side * dist;
    return out;
}

// Same arithmetic as View_SetPerspective + View_ApplyPerspective (z_view.c) + Play_Draw, without emitting
// display list commands and without touching the frame interpolation camera heuristics (the picture is
// drawn with the local view).
void ComputeViewMatrices(View* view, f32 far, MtxF* vp, MtxF* billboard) {
    view->zFar = far;
    s32 width = view->viewport.rightX - view->viewport.leftX;
    s32 height = view->viewport.bottomY - view->viewport.topY;
    f32 aspect = (height != 0) ? (f32)width / (f32)height : (4.0f / 3.0f);
    if (view->eye.x == view->lookAt.x && view->eye.y == view->lookAt.y && view->eye.z == view->lookAt.z) {
        view->eye.x += 1.0f;
        view->eye.y += 1.0f;
        view->eye.z += 1.0f;
    }
    MtxF viewingF;
    guLookAtF(viewingF.mf, view->eye.x, view->eye.y, view->eye.z, view->lookAt.x, view->lookAt.y, view->lookAt.z,
              view->up.x, view->up.y, view->up.z);
    guPerspective(&view->projection, &view->normal, view->fovy, aspect, view->zNear, view->zFar, view->scale);
    Matrix_MtxFToMtx(&viewingF, &view->viewing);
    MtxF viewing;
    MtxF projection;
    Matrix_MtxToMtxF(&view->viewing, &viewing);
    Matrix_MtxToMtxF(&view->projection, &projection);
    SkinMatrix_MtxFMtxFMult(&projection, &viewing, vp);
    *billboard = viewing;
    billboard->mf[0][3] = billboard->mf[1][3] = billboard->mf[2][3] = billboard->mf[3][0] = billboard->mf[3][1] =
        billboard->mf[3][2] = 0.0f;
    Matrix_Transpose(billboard);
}

} // namespace

// ---------------------------------------------------------------------------------------------------
// C API

extern "C" s32 Zmp_MultiActive(void) {
    return gZmpSim.enabled && gZmpSim.inPlay && Valid(gZmpSim.anchor);
}

extern "C" void Zmp_PlayInitBegin(PlayState* play) {
    Zmp::Pause::Reset();
    if (!gZmpSim.enabled) {
        gZmpSim.inPlay = 0;
        return;
    }
    // The save context holds the block of the slot that was the context when the previous Play ended (parked at
    // Play_Destroy; anything that changed it since, like the respawn after a game over, belongs to that slot).
    if (Valid(gZmpSim.ctx) && Slot(gZmpSim.ctx).active) {
        SaveToBlock(Slot(gZmpSim.ctx).block);
    }
    for (int k = 0; k < ZMP_MAX_PLAYERS; k++) {
        Slot(k).player = nullptr;
        Slot(k).present = 0;
        Slot(k).hasView = 0;
    }
    int anchor = LowestActive();
    gZmpSim.anchor = (s8)anchor;
    gZmpSim.ctx = (s8)anchor;
    gZmpSim.spawningSlot = (s8)anchor;
    gZmpSim.msgOwner = -1;
    gZmpSim.pauseOwner = -1;
    gZmpSim.transitionArmed = 0;
    gZmpSim.transitionCountdown = 0;
    gZmpCtxPlayer = nullptr;
    gZmpSim.inPlay = 1;
    gZmpSim.groupDefeat = 0;
    // A new scene (group scene change, respawn after the group game over): downed players stand up again with
    // three hearts.
    for (int k = 0; k < ZMP_MAX_PLAYERS; k++) {
        ZmpPlayerSlot& s = Slot(k);
        if (!s.active) {
            continue;
        }
        if (s.downed || s.block.health <= 0) {
            s.block.health = MAX(s.block.health, MIN(0x30, gSaveContext.healthCapacity));
            s.block.healthAccumulator = 0;
        }
        s.downed = 0;
        s.spectate = -1;
        s.reviveProgress = 0;
        s.reviver = -1;
    }
    if (Valid(anchor)) {
        // The anchor's block is live in the save context while it is the context.
        LoadFromBlock(Slot(anchor).block);
        Slot(anchor).present = 1;
    }
}

extern "C" void Zmp_PlayInitPlayers(PlayState* play, s32 startBgCamIndex) {
    gZmpSim.spawningSlot = -1;
    if (!gZmpSim.enabled || !Valid(gZmpSim.anchor)) {
        return;
    }
    int anchor = gZmpSim.anchor;
    Player* lead = Slot(anchor).player;
    if (lead == nullptr) {
        // The scene's Player did not go through Player_Init yet (should not happen): take the list head.
        lead = (Player*)play->actorCtx.actorLists[ACTORCAT_PLAYER].head;
        Slot(anchor).player = lead;
        gZmpCtxPlayer = lead;
    }
    int n = 0;
    for (int k = 0; k < ZMP_MAX_PLAYERS; k++) {
        if (k == anchor || !Slot(k).active) {
            continue;
        }
        n++;
        Vec3f pos = SideOffset(lead->actor.world.pos, lead->actor.shape.rot.y, n);
        SpawnPlayerActor(play, k, pos, lead->actor.shape.rot.y, lead->actor.params, startBgCamIndex);
    }
    Zmp::Log("zmp: scene " + std::to_string(play->sceneNum) + " with " + std::to_string(Zmp::Players::PresentCount()) +
             " players (anchor slot " + std::to_string(anchor) + ")");
}

extern "C" void Zmp_PlayDestroy(PlayState* play) {
    Zmp::Pause::Reset();
    if (gZmpSim.enabled && Valid(gZmpSim.anchor) && gZmpSim.ctx != gZmpSim.anchor) {
        SwitchTo(play, gZmpSim.anchor);
    }
    if (gZmpSim.enabled && Valid(gZmpSim.anchor)) {
        Park(play, gZmpSim.anchor);
    }
    gZmpSim.inPlay = 0;
    gZmpCtxPlayer = nullptr;
}

extern "C" void Zmp_OnPlayerInit(Player* player, PlayState* play) {
    if (!gZmpSim.enabled || !Valid(gZmpSim.spawningSlot)) {
        return;
    }
    int k = gZmpSim.spawningSlot;
    Slot(k).player = player;
    Slot(k).present = 1;
    gZmpCtxPlayer = player;
    gZmpSim.spawningSlot = -1;
}

extern "C" void Zmp_OnPlayerDestroy(Player* player) {
    for (int k = 0; k < ZMP_MAX_PLAYERS; k++) {
        if (Slot(k).player == player) {
            Slot(k).player = nullptr;
            Slot(k).present = 0;
        }
    }
    if (gZmpCtxPlayer == player) {
        gZmpCtxPlayer = nullptr;
    }
}

extern "C" void Zmp_SetContext(PlayState* play, s32 slot) {
    if (!Zmp_MultiActive() || !Present(slot)) {
        return;
    }
    SwitchTo(play, slot);
}

extern "C" void Zmp_RestoreAnchor(PlayState* play) {
    sTraceActor = nullptr;
    sTracePhase = '-';
    if (!Zmp_MultiActive()) {
        return;
    }
    SwitchTo(play, gZmpSim.anchor);
}

extern "C" void Zmp_RandTraceHit(void* caller) {
    RandTraceEntry e{ sTracePhase, sTraceActor ? sTraceActor->id : (s16)-1, sTraceActor ? sTraceActor->params : (s16)0,
                      (uintptr_t)caller - (uintptr_t)0x140000000, 1 };
    if (!sRandTrace.empty()) {
        RandTraceEntry& b = sRandTrace.back();
        if (b.phase == e.phase && b.actorId == e.actorId && b.params == e.params && b.caller == e.caller) {
            b.count++;
            return;
        }
    }
    sRandTrace.push_back(e);
}

extern "C" Player* Zmp_ContextForActor(PlayState* play, Actor* actor) {
    sTraceActor = actor;
    sTracePhase = 'U';
    int k = ContextSlot(actor);
    if (Present(k)) {
        SwitchTo(play, k);
        if ((Actor*)Slot(k).player == actor) {
            // The boot data (run speed, turning, gravity...) lives in global game registers that the original only
            // recomputes when the boots, the water or the room change: with several players they must be this
            // player's before its update. Player_SetBootData is a pure function of its own state.
            Player_SetBootData(play, Slot(k).player);
        }
    }
    return (Player*)gZmpCtxPlayer;
}

extern "C" void Zmp_UpdateAttentionAll(PlayState* play) {
    sTraceActor = nullptr;
    sTracePhase = 'A';
    ActorContext* actorCtx = &play->actorCtx;
    int anchor = gZmpSim.anchor;
    for (int pass = 0; pass < 2; pass++) {
        for (int k = 0; k < ZMP_MAX_PLAYERS; k++) {
            // Other players first, the anchor last (its context stays live afterwards).
            if (!Present(k) || (pass == 0) == (k == anchor)) {
                continue;
            }
            SwitchTo(play, k);
            Player* player = Slot(k).player;
            Actor* actor = player->focusActor;
            if ((actor != NULL) && (actor->update == NULL)) {
                actor = NULL;
                Player_ReleaseLockOn(player);
            }
            if ((actor == NULL) || (player->zTargetActiveTimer < 5)) {
                actor = NULL;
                if (actorCtx->targetCtx.unk_4B != 0) {
                    actorCtx->targetCtx.unk_4B = 0;
                    if (k == sLocalSlot) {
                        Sfx_PlaySfxCentered(NA_SE_SY_LOCK_OFF);
                    }
                }
            }
            Attention_Update(&actorCtx->targetCtx, player, actor, play);
        }
    }
}

extern "C" void Zmp_UpdateMainCameras(PlayState* play) {
    int anchor = gZmpSim.anchor;
    for (int pass = 0; pass < 2; pass++) {
        for (int k = 0; k < ZMP_MAX_PLAYERS; k++) {
            if (!Present(k) || (pass == 0) == (k == anchor)) {
                continue;
            }
            SwitchTo(play, k);
            gZmpCameraInterfaceMuted = (k != anchor);
            // Spectator: the camera of a downed player follows the partner it watches.
            int t = Slot(k).downed ? Slot(k).spectate : -1;
            if (Present(t)) {
                play->mainCamera.player = Slot(t).player;
                gZmpCtxPlayer = Slot(t).player;
            }
            Camera_Update(&play->mainCamera);
            if (Present(t)) {
                play->mainCamera.player = Slot(k).player;
                gZmpCtxPlayer = Slot(k).player;
            }
            gZmpCameraInterfaceMuted = 0;
            ZmpPlayerSlot& s = Slot(k);
            s.hasView = play->mainCamera.status == CAM_STAT_ACTIVE;
            if (s.hasView) {
                s.view = play->view;
            }
        }
    }
}

extern "C" void Zmp_KaleidoSetupAll(PlayState* play) {
    int anchor = gZmpSim.anchor;
    for (int k = 0; k < ZMP_MAX_PLAYERS; k++) {
        if (!Present(k)) {
            continue;
        }
        SwitchTo(play, k);
        KaleidoSetup_Update(play);
        if (play->pauseCtx.state != 0 || play->pauseCtx.debugState != 0) {
            gZmpSim.pauseOwner = (s8)k;
            break;
        }
    }
    SwitchTo(play, anchor);
}

extern "C" void Zmp_EnterOwner(PlayState* play, s32 which) {
    if (!Zmp_MultiActive()) {
        return;
    }
    int k = which == 0 ? gZmpSim.msgOwner : gZmpSim.pauseOwner;
    SwitchTo(play, Present(k) ? k : gZmpSim.anchor);
}

extern "C" void Zmp_OnMessageStart(void) {
    if (gZmpSim.enabled && gZmpSim.inPlay) {
        gZmpSim.msgOwner = gZmpSim.ctx;
    }
}

namespace {

const Input& SlotInput(PlayState* play, int k) {
    return k == gZmpSim.ctx ? play->state.input[0] : Slot(k).input;
}

f32 DistSq(const Player* a, const Player* b) {
    f32 dx = a->actor.world.pos.x - b->actor.world.pos.x;
    f32 dy = a->actor.world.pos.y - b->actor.world.pos.y;
    f32 dz = a->actor.world.pos.z - b->actor.world.pos.z;
    return dx * dx + dy * dy + dz * dz;
}

// Nearest standing player to slot k (spectator target). Ties to the lowest slot.
int NearestStanding(int k) {
    int best = -1;
    f32 bestD = 0.0f;
    for (int j = 0; j < ZMP_MAX_PLAYERS; j++) {
        if (j == k || !Present(j) || Slot(j).downed) {
            continue;
        }
        f32 d = DistSq(Slot(k).player, Slot(j).player);
        if (best < 0 || d < bestD) {
            best = j;
            bestD = d;
        }
    }
    return best;
}

// Next standing player after `from` in slot order (dir = +1 / -1), skipping k.
int CycleStanding(int k, int from, int dir) {
    for (int n = 1; n <= ZMP_MAX_PLAYERS; n++) {
        int j = ((from + dir * n) % ZMP_MAX_PLAYERS + ZMP_MAX_PLAYERS) % ZMP_MAX_PLAYERS;
        if (j != k && Present(j) && !Slot(j).downed) {
            return j;
        }
    }
    return -1;
}

void ReviveInContext(void* p) {
    int k = *(int*)p;
    ZmpPlayerSlot& s = Slot(k);
    // Three hearts or a quarter of the capacity, whichever is more (whole hearts).
    s16 target = MAX(0x30, ((gSaveContext.healthCapacity / 4) + 0xF) & ~0xF);
    target = MIN(target, gSaveContext.healthCapacity);
    gSaveContext.health = 0;
    gSaveContext.healthAccumulator = target;
    Player_ZmpRevive(gPlayState, s.player);
    s.downed = 0;
    s.spectate = -1;
    s.reviveProgress = 0;
    s.reviver = -1;
}

void MagicInContext(void* p) {
    PlayState* play = (PlayState*)p;
    // Same conditions as Interface_Update for the anchor.
    if ((play->msgCtx.msgMode == MSGMODE_NONE) && (play->transitionTrigger == TRANS_TRIGGER_OFF) &&
        (play->gameOverCtx.state == GAMEOVER_INACTIVE) && (play->transitionMode == TRANS_MODE_OFF) &&
        ((play->csCtx.state == CS_STATE_IDLE) || !Player_InCsMode(play))) {
        Interface_UpdateMagicBar(play);
    }
}

void UpdateDowned(PlayState* play) {
    int standing = 0;
    int present = 0;
    for (int k = 0; k < ZMP_MAX_PLAYERS; k++) {
        if (!Present(k)) {
            continue;
        }
        present++;
        standing += Slot(k).downed ? 0 : 1;
    }
    for (int k = 0; k < ZMP_MAX_PLAYERS; k++) {
        ZmpPlayerSlot& s = Slot(k);
        if (!Present(k) || !s.downed) {
            continue;
        }
        // Spectator: follows a standing partner; C-left / C-right switch between them.
        const Input& in = SlotInput(play, k);
        if (!Present(s.spectate) || Slot(s.spectate).downed) {
            s.spectate = (s8)NearestStanding(k);
        } else if (CHECK_BTN_ALL(in.press.button, BTN_CLEFT)) {
            int n = CycleStanding(k, s.spectate, -1);
            s.spectate = (s8)(n >= 0 ? n : s.spectate);
        } else if (CHECK_BTN_ALL(in.press.button, BTN_CRIGHT)) {
            int n = CycleStanding(k, s.spectate, +1);
            s.spectate = (s8)(n >= 0 ? n : s.spectate);
        }
        // Revive: a standing partner within range holds A.
        int reviver = -1;
        for (int j = 0; j < ZMP_MAX_PLAYERS; j++) {
            if (j == k || !Present(j) || Slot(j).downed) {
                continue;
            }
            if (!CHECK_BTN_ALL(SlotInput(play, j).cur.button, BTN_A)) {
                continue;
            }
            if (DistSq(s.player, Slot(j).player) <= ZMP_REVIVE_RANGE * ZMP_REVIVE_RANGE) {
                reviver = j;
                break;
            }
        }
        if (reviver >= 0 && (reviver == s.reviver || s.reviver < 0)) {
            s.reviver = (s8)reviver;
            s.reviveProgress++;
        } else {
            s.reviver = (s8)reviver;
            s.reviveProgress = reviver >= 0 ? 1 : 0;
        }
        if (s.reviveProgress >= ZMP_REVIVE_TICKS) {
            Zmp::Log("zmp: slot " + std::to_string(k) + " revived by slot " + std::to_string(reviver));
            int arg = k;
            Zmp::Players::RunInContext(k, ReviveInContext, &arg);
        }
    }
    // Group defeat: nobody left standing. The original game over runs (the death action of the downed players
    // moves it on once it waits for the ground).
    if (present > 0 && standing == 0 && !gZmpSim.groupDefeat && play->gameOverCtx.state == GAMEOVER_INACTIVE) {
        gZmpSim.groupDefeat = 1;
        play->gameOverCtx.state = GAMEOVER_DEATH_START;
        Audio_StopBgmAndFanfare(0);
        Audio_PlayFanfare(NA_BGM_GAME_OVER);
        gSaveContext.seqId = (u8)NA_BGM_DISABLED;
        gSaveContext.natureAmbienceId = NATURE_ID_DISABLED;
        Zmp::Log("zmp: every player is down: group game over");
    }
}

} // namespace

extern "C" void Zmp_UpdateHealthAccumulators(PlayState* play) {
    if (!Zmp_MultiActive()) {
        return;
    }
    for (int k = 0; k < ZMP_MAX_PLAYERS; k++) {
        if (!Present(k) || k == gZmpSim.ctx) {
            continue;
        }
        ZmpPlayerBlock& s = Slot(k).block;
        if (s.healthAccumulator != 0) {
            s.healthAccumulator -= 4;
            s.health += 4;
            if ((s.health & 0xF) < 4 && k == sLocalSlot) {
                Audio_PlaySfxGeneral(NA_SE_SY_HP_RECOVER, &gSfxDefaultPos, 4, &gSfxDefaultFreqAndVolScale,
                                     &gSfxDefaultFreqAndVolScale, &gSfxDefaultReverb);
            }
            if (s.health >= gSaveContext.healthCapacity) {
                s.health = gSaveContext.healthCapacity;
                s.healthAccumulator = 0;
            }
        }
    }
    // The shared magic meter grew (first meter, double magic): every other player's meter fills up too, as the
    // anchor's did.
    if (gSaveContext.magicCapacity > gZmpSim.lastMagicCapacity) {
        for (int k = 0; k < ZMP_MAX_PLAYERS; k++) {
            ZmpPlayerBlock& b = Slot(k).block;
            s16 full = MAX(gSaveContext.magicCapacity, gSaveContext.magicLevel * MAGIC_NORMAL_METER);
            if (!Slot(k).active || k == gZmpSim.ctx) {
                continue;
            }
            if (b.magicState == MAGIC_STATE_IDLE) {
                b.prevMagicState = MAGIC_STATE_IDLE;
                b.magicState = MAGIC_STATE_FILL;
                b.magicFillTarget = full;
            } else if (b.magicState == MAGIC_STATE_FILL) {
                b.magicFillTarget = MAX(b.magicFillTarget, full);
            }
        }
    }
    gZmpSim.lastMagicCapacity = gSaveContext.magicCapacity;
    // Magic meter (consumption, refill) of the players out of context: Interface_Update only ran it for the
    // anchor.
    for (int k = 0; k < ZMP_MAX_PLAYERS; k++) {
        if (Present(k) && k != gZmpSim.anchor && Slot(k).block.magicState != MAGIC_STATE_IDLE) {
            Zmp::Players::RunInContext(k, MagicInContext, play);
        }
    }
    UpdateDowned(play);
    if (gZmpSim.ctx != gZmpSim.anchor) {
        SwitchTo(play, gZmpSim.anchor);
    }
}

extern "C" s32 Zmp_OnPlayerDeath(PlayState* play, Player* player) {
    if (!Zmp_MultiActive()) {
        return 0;
    }
    int k = Zmp::Players::SlotOf(&player->actor);
    if (!Valid(k)) {
        return 0;
    }
    // A fairy in a bottle keeps its original behaviour (instant revive).
    for (int i = 0; i < 4; i++) {
        if (gSaveContext.inventory.items[SLOT_BOTTLE_1 + i] == ITEM_FAIRY) {
            return 0;
        }
    }
    int standing = 0;
    for (int j = 0; j < ZMP_MAX_PLAYERS; j++) {
        if (j != k && Present(j) && !Slot(j).downed) {
            standing++;
        }
    }
    ZmpPlayerSlot& s = Slot(k);
    s.downed = 1;
    s.spectate = (s8)NearestStanding(k);
    s.reviveProgress = 0;
    s.reviver = -1;
    if (standing == 0) {
        gZmpSim.groupDefeat = 1; // the last one: original game over for the group
        Zmp::Log("zmp: slot " + std::to_string(k) + " down, nobody left standing: group game over");
        return 0;
    }
    Zmp::Log("zmp: slot " + std::to_string(k) + " is down (" + std::to_string(standing) + " standing)");
    return 1;
}

extern "C" s32 Zmp_IsDowned(Player* player) {
    if (!Zmp_MultiActive()) {
        return 0;
    }
    int k = Zmp::Players::SlotOf(&player->actor);
    return Downed(k) ? 1 : 0;
}

extern "C" s32 Zmp_AllowDeathCamera(void) {
    if (!Zmp_MultiActive()) {
        return 1;
    }
    // The last player down (group game over) keeps the original death camera: everybody watches it.
    return gZmpSim.groupDefeat ? 1 : 0;
}

extern "C" s32 Zmp_TunicColor(s32 tunic, const Color_RGB8* base, Color_RGB8* out) {
    // Tones for players 2..6 (player 1 keeps the original colours). Kokiri tunic: the tone itself; Goron and Zora
    // tunics keep their colour, tinted a third of the way to it.
    static const Color_RGB8 kTones[ZMP_MAX_PLAYERS] = {
        { 30, 105, 27 }, { 110, 40, 150 }, { 190, 120, 20 }, { 20, 120, 130 }, { 170, 50, 100 }, { 140, 140, 140 },
    };
    if (!Zmp_MultiActive() || gZmpCtxPlayer == nullptr) {
        return 0;
    }
    int k = Zmp::Players::SlotOf((const Actor*)gZmpCtxPlayer);
    if (k <= 0) {
        return 0;
    }
    const Color_RGB8& t = kTones[k];
    if (tunic == PLAYER_TUNIC_KOKIRI) {
        *out = t;
    } else {
        out->r = (u8)((base->r * 2 + t.r) / 3);
        out->g = (u8)((base->g * 2 + t.g) / 3);
        out->b = (u8)((base->b * 2 + t.b) / 3);
    }
    return 1;
}

extern "C" s32 Zmp_LocalHealthCritical(void) {
    int k = sLocalSlot;
    if (!Present(k) || Slot(k).downed) {
        return 0;
    }
    s16 keep = gSaveContext.health;
    gSaveContext.health = (s16)Zmp::Players::SlotHealth(k);
    s32 critical = HealthMeter_IsCritical() ? 1 : 0;
    gSaveContext.health = keep;
    return critical;
}

extern "C" s32 Zmp_PauseIsLocal(void) {
    return Zmp_MultiActive();
}

extern "C" s32 Zmp_TransitionGate(PlayState* play) {
    if (!Zmp_MultiActive() || Zmp::Players::PresentCount() < 2 || play->transitionTrigger != TRANS_TRIGGER_START ||
        play->transitionMode != TRANS_MODE_OFF || gSaveContext.respawnFlag != 0 ||
        play->gameOverCtx.state != GAMEOVER_INACTIVE || play->csCtx.state != CS_STATE_IDLE ||
        gSaveContext.nextCutsceneIndex >= 0xFFF0) {
        gZmpSim.transitionArmed = 0;
        gZmpSim.transitionCountdown = 0;
        return 0;
    }
    if (!gZmpSim.transitionArmed) {
        gZmpSim.transitionArmed = 1;
        gZmpSim.transitionCountdown = 100; // 5 s
        gZmpSim.transitionEntrance = play->nextEntranceIndex;
        Zmp::Log("zmp: group scene change to entrance " + std::to_string(play->nextEntranceIndex) + " in 5 s");
    }
    if (gZmpSim.transitionCountdown > 0) {
        gZmpSim.transitionCountdown--;
    }
    if (gZmpSim.transitionCountdown > 0) {
        return 1;
    }
    gZmpSim.transitionArmed = 0;
    return 0;
}

extern "C" void Zmp_DrawBeginView(PlayState* play) {
    sDrawBegun = false;
    if (!Zmp_MultiActive()) {
        return;
    }
    // Canonical view: the camera state the simulation left in play->view (anchor or active sub camera).
    View sim = play->view;
    ComputeViewMatrices(&sim, play->lightCtx.fogFar, &sSimVP, &sSimBillboard);
    sSimView = sim;
    sDrawBegun = true;
    int L = sLocalSlot;
    // Rooms with a pre-rendered background (houses, shops) only look right from their one fixed camera:
    // everybody draws with the canonical view there.
    bool prerendered = play->roomCtx.curRoom.meshHeader != nullptr && play->roomCtx.curRoom.meshHeader->base.type == 1;
    // A cutscene is seen by everybody through the same camera (PLAN.md 2.9: scripted cutscenes are GLOBAL).
    bool cutscene = play->csCtx.state != CS_STATE_IDLE || gSaveContext.cutsceneIndex >= 0xFFF0;
    if (!prerendered && !cutscene && play->activeCamera == CAM_ID_MAIN && L != gZmpSim.anchor && Present(L) &&
        Slot(L).hasView) {
        play->view = Slot(L).view;
    }
}

extern "C" MtxF* Zmp_SimViewProjection(PlayState* play) {
    return sDrawBegun ? &sSimVP : &play->viewProjectionMtxF;
}

extern "C" s32 Zmp_DrawAllActors(void) {
    return sDrawBegun;
}

extern "C" void Zmp_DrawActorContext(PlayState* play, Actor* actor) {
    sTraceActor = actor;
    sTracePhase = 'D';
    if (!sDrawBegun) {
        return;
    }
    int k = ContextSlot(actor);
    if (Present(k)) {
        SwitchTo(play, k);
    }
}

extern "C" void Zmp_DrawEndView(PlayState* play) {
    if (!sDrawBegun) {
        return;
    }
    SwitchTo(play, gZmpSim.anchor);
    // The matrix pointers of a View point into a frame's graphics pool. The copy taken at the start of the
    // draw holds the previous frame's (stale), and func_800AB944 writes through them when a camera asks for
    // a view recomputation: give the canonical view fresh matrices of this frame that no display list uses.
    play->view = sSimView;
    play->view.projectionPtr = (Mtx*)Graph_Alloc(play->state.gfxCtx, sizeof(Mtx));
    play->view.projectionFlippedPtr = (Mtx*)Graph_Alloc(play->state.gfxCtx, sizeof(Mtx));
    play->view.viewingPtr = (Mtx*)Graph_Alloc(play->state.gfxCtx, sizeof(Mtx));
    play->viewProjectionMtxF = sSimVP;
    play->billboardMtxF = sSimBillboard;
    // The local picture left view dependent matrices in the game's matrix stack. The next tick starts
    // from the same stack on every machine: identity everywhere, current = base.
    MtxF* stack = nullptr;
    MtxF* current = nullptr;
    Matrix_ZmpGetPointers(&stack, &current);
    if (stack != nullptr) {
        for (int i = 0; i < 20; i++) {
            SkinMatrix_Clear(&stack[i]);
        }
        Matrix_ZmpSetPointers(stack, stack);
    }
    sDrawBegun = false;
}

extern "C" void Zmp_OverlayBegin(PlayState* play) {
    if (Zmp_MultiActive() && Present(sLocalSlot)) {
        SwitchTo(play, sLocalSlot);
    }
}

extern "C" void Zmp_OverlayEnd(PlayState* play) {
    Zmp_RestoreAnchor(play);
}

extern "C" void Zmp_OnInputCopied(GameState* gameState) {
    if (!Zmp_MultiActive() || gPlayState == nullptr || gameState != &gPlayState->state) {
        return;
    }
    if (gZmpSim.ctx != gZmpSim.anchor) {
        SwitchTo(gPlayState, gZmpSim.anchor);
    }
    gameState->input[0] = Slot(gZmpSim.anchor).input;
    for (int i = 1; i < 4; i++) {
        memset(&gameState->input[i], 0, sizeof(Input));
    }
}

extern "C" s32 Zmp_OcarinaInput(Input* out) {
    if (!Zmp_MultiActive()) {
        return 0;
    }
    int k = gZmpSim.msgOwner;
    if (!Present(k)) {
        k = gZmpSim.anchor;
    }
    if (k == gZmpSim.ctx && gPlayState != nullptr) {
        *out = gPlayState->state.input[0];
    } else {
        *out = Slot(k).input;
    }
    return 1;
}

extern "C" s32 Zmp_AudioSession(void) {
    // Lockstep only: phase 1 recordings keep the behaviour they were recorded with.
    return gZmpSim.enabled ? 1 : 0;
}

extern "C" void Zmp_AudioTick(void) {
    if (Zmp_AudioSession()) {
        // Three audio tasks per 20 Hz game tick (60 Hz audio frames), as on the original hardware.
        gZmpSim.audioTaskCount += 3;
    }
}

extern "C" u32 Zmp_AudioTaskCount(u32 real) {
    return Zmp_AudioSession() ? gZmpSim.audioTaskCount : real;
}

extern "C" u32 Zmp_AudioRandom(void) {
    gZmpSim.audioRandom = gZmpSim.audioRandom * 1664525u + 1013904223u;
    return gZmpSim.audioRandom >> 8;
}

extern "C" void Zmp_AudioSfxPlayed(u16 sfxId) {
    if (gZmpSim.enabled && sfxId == NA_SE_SY_METRONOME) {
        gZmpSim.metronomeAt = gZmpSim.audioTaskCount;
    }
}

extern "C" s32 Zmp_AudioSfxPlaying(u32 sfxId, s32* playing) {
    if (!gZmpSim.enabled || sfxId != NA_SE_SY_METRONOME) {
        return 0;
    }
    // The metronome click lasts about 0.4 s: 8 game ticks (24 audio tasks).
    *playing = (gZmpSim.audioTaskCount - gZmpSim.metronomeAt) < 24 ? 1 : 0;
    return 1;
}

extern "C" s32 Zmp_LanguageLocked(void) {
    return gZmpSim.enabled ? 1 : 0;
}

// ---------------------------------------------------------------------------------------------------
// C++ API

namespace Zmp::Players {

void RandTraceBegin(bool enabled) {
    gZmpRandTrace = enabled ? 1 : 0;
    sRandTrace.clear();
    sTraceActor = nullptr;
    sTracePhase = 'P'; // pad / events before the update
}

std::string RandTraceText() {
    std::string out = "\n[rand_trace] (phase actor params caller_rva x count)\n";
    char line[96];
    for (auto& e : sRandTrace) {
        snprintf(line, sizeof(line), "%c %04X %04X +0x%llX x%d\n", e.phase, (unsigned)(u16)e.actorId,
                 (unsigned)(u16)e.params, (unsigned long long)e.caller, e.count);
        out += line;
    }
    return out;
}

void SetLocalSlot(int slot) {
    sLocalSlot = slot;
}

int LocalSlot() {
    return sLocalSlot;
}

bool Found(int slot) {
    PlayState* play = gPlayState;
    if (play == nullptr || !Valid(slot)) {
        return false;
    }
    Player* player = (Player*)play->actorCtx.actorLists[ACTORCAT_PLAYER].head;
    if (player == nullptr) {
        return false;
    }
    u32 keepTask = gZmpSim.audioTaskCount;
    memset(&gZmpSim, 0, sizeof(gZmpSim));
    gZmpSim.magic = kSimMagic;
    gZmpSim.audioTaskCount = keepTask;
    gZmpSim.audioRandom = 0x5A4D5032;
    gZmpSim.enabled = 1;
    gZmpSim.inPlay = 1;
    gZmpSim.anchor = (s8)slot;
    gZmpSim.ctx = (s8)slot;
    gZmpSim.msgOwner = -1;
    gZmpSim.pauseOwner = -1;
    gZmpSim.spawningSlot = -1;
    ZmpPlayerSlot& s = Slot(slot);
    s.active = 1;
    s.present = 1;
    s.player = player;
    s.spectate = -1;
    s.reviver = -1;
    SaveToBlock(s.block);
    s.input = play->state.input[0];
    for (int k = 0; k < ZMP_MAX_PLAYERS; k++) {
        Slot(k).spectate = -1;
        Slot(k).reviver = -1;
    }
    gZmpCtxPlayer = player;
    Log("zmp: multiplayer simulation founded, slot " + std::to_string(slot));
    return true;
}

void Reset() {
    if (gZmpSim.enabled && gPlayState != nullptr && gZmpSim.inPlay && Valid(gZmpSim.anchor)) {
        SwitchTo(gPlayState, gZmpSim.anchor);
    }
    gZmpSim.enabled = 0;
    gZmpCtxPlayer = nullptr;
    sDrawBegun = false;
    Zmp::Pause::Reset();
}

void Spawn(int slot) {
    PlayState* play = gPlayState;
    if (!Valid(slot) || play == nullptr || !Zmp_MultiActive()) {
        return;
    }
    ZmpPlayerSlot& s = Slot(slot);
    if (Present(slot)) {
        return;
    }
    if (gZmpSim.ctx != gZmpSim.anchor) {
        SwitchTo(play, gZmpSim.anchor);
    }
    s.active = 1;
    s.downed = 0;
    s.spectate = -1;
    s.reviveProgress = 0;
    s.reviver = -1;
    // A new player starts with the anchor's equipment, buttons and ammo, empty bottles, full health and magic.
    SaveToBlock(s.block);
    s.block.health = gSaveContext.healthCapacity;
    s.block.healthAccumulator = 0;
    // (the capacity may still be growing on the anchor right after a game starts: use the level)
    s16 magicFull = MAX(gSaveContext.magicCapacity, gSaveContext.magicLevel * MAGIC_NORMAL_METER);
    s.block.magic = (s8)magicFull;
    s.block.magicState = MAGIC_STATE_IDLE;
    s.block.prevMagicState = MAGIC_STATE_IDLE;
    s.block.magicFillTarget = magicFull;
    s.block.magicTarget = 0;
    s.block.nayrusLoveTimer = 0;
    for (int i = 0; i < 4; i++) {
        if (s.block.bottles[i] != ITEM_NONE) {
            s.block.bottles[i] = ITEM_BOTTLE;
        }
    }
    FixBottleButtons(s.block);
    ActorEntry* entry = play->linkActorEntry;
    Vec3f base = { (f32)entry->pos.x, (f32)entry->pos.y, (f32)entry->pos.z };
    s16 yaw = entry->rot.y;
    // Next to the entrance of the scene, standing (start mode "idle"), no start camera.
    Vec3f pos = SideOffset(base, yaw, slot + 1);
    s16 params = (s16)((PLAYER_START_MODE_IDLE << 8) | 0xFF);
    // Same background camera data as the anchor (fixed cameras of rooms and houses).
    Player* p = SpawnPlayerActor(play, slot, pos, yaw, params, play->mainCamera.camDataIdx);
    Log("zmp: SPAWN slot " + std::to_string(slot) + (p != nullptr ? " ok" : " FAILED"));
}

void Despawn(int slot) {
    PlayState* play = gPlayState;
    if (!Valid(slot)) {
        return;
    }
    ZmpPlayerSlot& s = Slot(slot);
    s.active = 0;
    if (play == nullptr || !Zmp_MultiActive() || !Present(slot)) {
        s.present = 0;
        s.player = nullptr;
        return;
    }
    if (slot == gZmpSim.anchor) {
        int next = -1;
        for (int k = 0; k < ZMP_MAX_PLAYERS; k++) {
            if (k != slot && Present(k)) {
                next = k;
                break;
            }
        }
        if (next < 0) {
            Log("zmp: DESPAWN of the last player ignored");
            s.active = 1;
            return;
        }
        SwitchTo(play, next);
        gZmpSim.anchor = (s8)next;
    } else if (gZmpSim.ctx != gZmpSim.anchor) {
        SwitchTo(play, gZmpSim.anchor);
    }
    Player* p = s.player;
    if (p->naviActor != nullptr) {
        Actor_Kill(p->naviActor);
    }
    Actor_Kill(&p->actor);
    s.present = 0;
    s.player = nullptr;
    if (gZmpSim.msgOwner == slot) {
        gZmpSim.msgOwner = -1;
    }
    if (gZmpSim.pauseOwner == slot) {
        gZmpSim.pauseOwner = -1;
    }
    Log("zmp: DESPAWN slot " + std::to_string(slot) + ", anchor " + std::to_string(gZmpSim.anchor));
}

void StepInput(int slot, const OSContPad& pad) {
    if (!Valid(slot)) {
        return;
    }
    StepPad(&Slot(slot).input, pad);
}

Player* SlotPlayer(int slot) {
    return Present(slot) ? Slot(slot).player : nullptr;
}

int SlotOf(const Actor* actor) {
    if (actor == nullptr || actor->id != ACTOR_PLAYER) {
        return -1;
    }
    for (int k = 0; k < ZMP_MAX_PLAYERS; k++) {
        if (Slot(k).present && (const Actor*)Slot(k).player == actor) {
            return k;
        }
    }
    return -1;
}

int SlotHealth(int slot) {
    if (!Valid(slot)) {
        return 0;
    }
    return slot == gZmpSim.ctx ? gSaveContext.health : Slot(slot).block.health;
}

ZmpPlayerBlock SlotBlock(int slot) {
    ZmpPlayerBlock b{};
    if (!Valid(slot)) {
        return b;
    }
    if (slot == gZmpSim.ctx && gZmpSim.inPlay) {
        SaveToBlock(b);
        return b;
    }
    return Slot(slot).block;
}

bool SlotDowned(int slot) {
    return Downed(slot);
}

bool GroupDefeat() {
    return gZmpSim.groupDefeat != 0;
}

int SlotSpectate(int slot) {
    return Valid(slot) ? Slot(slot).spectate : -1;
}

int SlotReviveProgress(int slot) {
    return Valid(slot) ? Slot(slot).reviveProgress : 0;
}

int SlotReviver(int slot) {
    return Valid(slot) ? Slot(slot).reviver : -1;
}

void RunInContext(int slot, void (*fn)(void*), void* arg) {
    PlayState* play = gPlayState;
    if (!Zmp_MultiActive() || play == nullptr || !Present(slot)) {
        fn(arg);
        return;
    }
    int back = gZmpSim.ctx;
    SwitchTo(play, slot);
    fn(arg);
    SwitchTo(play, Valid(back) && Present(back) ? back : gZmpSim.anchor);
}

namespace {
struct EquipArg {
    int slot;
    ItemEquips equips;
};
void ApplyEquipInContext(void* p) {
    EquipArg* a = (EquipArg*)p;
    gSaveContext.equips = a->equips;
    Player* player = Zmp::Players::SlotPlayer(a->slot);
    if (player != nullptr && gPlayState != nullptr) {
        Player_SetEquipmentData(gPlayState, player);
    }
}
} // namespace

namespace {
struct BlockArg {
    int slot;
    ZmpPlayerBlock block;
};
void ApplyBlockInContext(void* p) {
    BlockArg* a = (BlockArg*)p;
    ZmpPlayerBlock b;
    SaveToBlock(b);
    b.magic = (s8)MIN((s16)a->block.magic, gSaveContext.magicCapacity);
    b.equips = a->block.equips;
    memcpy(b.ammo, a->block.ammo, sizeof(b.ammo));
    for (int i = 0; i < 4; i++) {
        // having a bottle is shared: only its contents come from the save
        if (b.bottles[i] != ITEM_NONE && a->block.bottles[i] != ITEM_NONE) {
            b.bottles[i] = a->block.bottles[i];
        }
    }
    FixBottleButtons(b);
    b.health = gSaveContext.healthCapacity;
    b.healthAccumulator = 0;
    LoadFromBlock(b);
    Player* player = Zmp::Players::SlotPlayer(a->slot);
    if (player != nullptr && gPlayState != nullptr) {
        Player_SetEquipmentData(gPlayState, player);
    }
}
} // namespace

void ApplySavedBlock(int slot, const ZmpPlayerBlock& block) {
    if (!Valid(slot) || !Present(slot)) {
        return;
    }
    BlockArg a{ slot, block };
    RunInContext(slot, ApplyBlockInContext, &a);
    Zmp::Log("zmp: saved block applied to slot " + std::to_string(slot));
}

void ApplyEquip(int slot, const ItemEquips& equips) {
    if (!Valid(slot) || !Slot(slot).active) {
        return;
    }
    if (!Present(slot)) {
        Slot(slot).block.equips = equips; // applied when its Player appears
        return;
    }
    EquipArg a{ slot, equips };
    RunInContext(slot, ApplyEquipInContext, &a);
}

int PresentCount() {
    int n = 0;
    for (int k = 0; k < ZMP_MAX_PLAYERS; k++) {
        n += Present(k) ? 1 : 0;
    }
    return n;
}

void AfterStateLoad() {
    sDrawBegun = false;
    if (gZmpSim.magic != kSimMagic) {
        memset(&gZmpSim, 0, sizeof(gZmpSim));
    }
    if (gZmpSim.enabled && gZmpSim.inPlay && Valid(gZmpSim.ctx)) {
        gZmpCtxPlayer = Slot(gZmpSim.ctx).player;
    } else {
        gZmpCtxPlayer = nullptr;
    }
}

} // namespace Zmp::Players
