// ZMP: several Player actors in one lockstep simulation. See ZmpPlayers.h for the model.

#include "ZmpPlayers.h"
#include "Session.h"

#include <cstring>
#include <string>

#include "soh/Zmp/ZmpLog.h"

extern "C" {
#include "variables.h"
#include "functions.h"
#include "macros.h"
extern PlayState* gPlayState;
void Attention_Init(TargetContext* targetCtx, Actor* actor, PlayState* play);
void Attention_Update(TargetContext* targetCtx, Player* player, Actor* actorArg, PlayState* play);
void Player_ReleaseLockOn(Player* player);
}

extern "C" {
ZmpSimState gZmpSim;
void* gZmpCtxPlayer = nullptr;
s32 gZmpCameraInterfaceMuted = 0;
}

namespace {

constexpr u32 kSimMagic = 0x534D505A; // "ZPMS"
int sLocalSlot = -1;

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

void Park(PlayState* play, int k) {
    ZmpPlayerSlot& s = Slot(k);
    memcpy(&s.camera, &play->mainCamera, sizeof(Camera));
    memcpy(&s.target, &play->actorCtx.targetCtx, sizeof(TargetContext));
    s.health = gSaveContext.health;
    s.healthAccumulator = gSaveContext.healthAccumulator;
    s.input = play->state.input[0];
}

void Load(PlayState* play, int k) {
    ZmpPlayerSlot& s = Slot(k);
    memcpy(&play->mainCamera, &s.camera, sizeof(Camera));
    memcpy(&play->actorCtx.targetCtx, &s.target, sizeof(TargetContext));
    gSaveContext.health = s.health;
    gSaveContext.healthAccumulator = s.healthAccumulator;
    play->state.input[0] = s.input;
    gZmpCtxPlayer = s.player;
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
        if (!Present(k)) {
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
    if (bgCamIndex != 0xFF) {
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
    if (!gZmpSim.enabled) {
        gZmpSim.inPlay = 0;
        return;
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
    if (Valid(anchor)) {
        // The anchor's health is live in the save context while it is the context.
        gSaveContext.health = Slot(anchor).health;
        gSaveContext.healthAccumulator = Slot(anchor).healthAccumulator;
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
    if (!Zmp_MultiActive()) {
        return;
    }
    SwitchTo(play, gZmpSim.anchor);
}

extern "C" Player* Zmp_ContextForActor(PlayState* play, Actor* actor) {
    int k = ContextSlot(actor);
    if (Present(k)) {
        SwitchTo(play, k);
    }
    return (Player*)gZmpCtxPlayer;
}

extern "C" void Zmp_UpdateAttentionAll(PlayState* play) {
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
            Camera_Update(&play->mainCamera);
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

extern "C" void Zmp_UpdateHealthAccumulators(PlayState* play) {
    if (!Zmp_MultiActive()) {
        return;
    }
    for (int k = 0; k < ZMP_MAX_PLAYERS; k++) {
        if (!Present(k) || k == gZmpSim.ctx) {
            continue;
        }
        ZmpPlayerSlot& s = Slot(k);
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
}

extern "C" s32 Zmp_TransitionGate(PlayState* play) {
    if (!Zmp_MultiActive() || Zmp::Players::PresentCount() < 2 || play->transitionTrigger != TRANS_TRIGGER_START ||
        play->transitionMode != TRANS_MODE_OFF || gSaveContext.respawnFlag != 0 ||
        play->gameOverCtx.state != GAMEOVER_INACTIVE) {
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
    if (play->activeCamera == CAM_ID_MAIN && L != gZmpSim.anchor && Present(L) && Slot(L).hasView) {
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
    play->view = sSimView;
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

extern "C" s32 Zmp_LanguageLocked(void) {
    return gZmpSim.enabled ? 1 : 0;
}

// ---------------------------------------------------------------------------------------------------
// C++ API

namespace Zmp::Players {

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
    s.health = gSaveContext.health;
    s.healthAccumulator = gSaveContext.healthAccumulator;
    s.input = play->state.input[0];
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
    s.active = 1;
    s.health = gSaveContext.healthCapacity;
    s.healthAccumulator = 0;
    if (gZmpSim.ctx != gZmpSim.anchor) {
        SwitchTo(play, gZmpSim.anchor);
    }
    ActorEntry* entry = play->linkActorEntry;
    Vec3f base = { (f32)entry->pos.x, (f32)entry->pos.y, (f32)entry->pos.z };
    s16 yaw = entry->rot.y;
    // Next to the entrance of the scene, standing (start mode "idle"), no start camera.
    Vec3f pos = SideOffset(base, yaw, slot + 1);
    s16 params = (s16)((PLAYER_START_MODE_IDLE << 8) | 0xFF);
    Player* p = SpawnPlayerActor(play, slot, pos, yaw, params, 0xFF);
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
    return slot == gZmpSim.ctx ? gSaveContext.health : Slot(slot).health;
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
