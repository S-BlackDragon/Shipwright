// ZMP: several Player actors in one lockstep simulation. See ZmpPlayers.h for the model.

#include "ZmpPlayers.h"
#include "Session.h"

#include <cmath>
#include <cstring>
#include <string>
#include <vector>
#include <cstdio>

#include "soh/Zmp/ZmpLog.h"
#include "soh/Zmp/Net/Lockstep.h"

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

constexpr u32 kSimMagic = 0x62354D5A; // "ZM5b" (phase 5b layout)
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
// 3D sound from the local camera (phase 5b): clip space of the canonical camera -> clip space of the local one.
// Both are affine in the world position, so the map between them is affine too (no perspective divide involved).
bool sSfxRemapValid = false;
float sSfxRemap[3][4];
void ComputeSfxRemap(const MtxF& sim, const MtxF& loc);

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

// Phase 5b: the block of a slot goes from the current age (gSaveContext.linkAge) to the other one with the game's
// own swap, using what this player wore at the other age (or the game's defaults).
void SwapSlotAge(ZmpPlayerSlot& s) {
    bool toAdult = gSaveContext.linkAge == LINK_AGE_CHILD;
    ZmpPlayerBlock live;
    SaveToBlock(live);
    LoadFromBlock(s.block);
    ItemEquips& in = toAdult ? gSaveContext.adultEquips : gSaveContext.childEquips;
    ItemEquips& saved = toAdult ? gSaveContext.childEquips : gSaveContext.adultEquips;
    if (s.otherValid) {
        in = s.otherEquips;
    } else {
        in.buttonItems[0] = ITEM_NONE;
    }
    Inventory_SwapAgeEquipment();
    if (!toAdult && !s.otherValid) {
        // (the original has no "first time as a child": a child's sword and shield, the rest as it was)
        bool sword = CHECK_OWNED_EQUIP(EQUIP_TYPE_SWORD, EQUIP_INV_SWORD_KOKIRI);
        bool shield = CHECK_OWNED_EQUIP(EQUIP_TYPE_SHIELD, EQUIP_INV_SHIELD_DEKU);
        gSaveContext.equips.buttonItems[0] = sword ? ITEM_SWORD_KOKIRI : ITEM_NONE;
        gSaveContext.equips.equipment = (u16)(((sword ? EQUIP_VALUE_SWORD_KOKIRI : 0) << (EQUIP_TYPE_SWORD * 4)) |
                                              ((shield ? EQUIP_VALUE_SHIELD_DEKU : 0) << (EQUIP_TYPE_SHIELD * 4)) |
                                              (EQUIP_VALUE_TUNIC_KOKIRI << (EQUIP_TYPE_TUNIC * 4)) |
                                              (EQUIP_VALUE_BOOTS_KOKIRI << (EQUIP_TYPE_BOOTS * 4)));
    }
    s.otherEquips = saved;
    s.otherValid = 1;
    SaveToBlock(s.block);
    LoadFromBlock(live);
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
    s.activeCam = play->activeCamera;
}

// --- one text box per player (phase 5b) ---
constexpr size_t kMsgFontOff = offsetof(MessageContext, font);
constexpr size_t kMsgTailOff = kMsgFontOff + sizeof(Font);

void MsgCopySmall(MessageContext* dst, const MessageContext* src) {
    memcpy(dst, src, kMsgFontOff);
    memcpy((u8*)dst + kMsgTailOff, (const u8*)src + kMsgTailOff, sizeof(MessageContext) - kMsgTailOff);
}

void MsgSwitch(PlayState* play, int k) {
    if (!gZmpSim.msgReady || !Valid(k) || k == gZmpSim.msgLoaded) {
        return;
    }
    int cur = gZmpSim.msgLoaded;
    if (Valid(cur)) {
        MsgCopySmall(&gZmpSim.msgStore[cur], &play->msgCtx);
        if (play->msgCtx.msgMode != MSGMODE_NONE) {
            memcpy(&gZmpSim.msgStore[cur].font, &play->msgCtx.font, sizeof(Font));
        }
        Message_ZmpStatics(gZmpSim.msgStatics[cur], 0);
    }
    MsgCopySmall(&play->msgCtx, &gZmpSim.msgStore[k]);
    if (gZmpSim.msgStore[k].msgMode != MSGMODE_NONE) {
        memcpy(&play->msgCtx.font, &gZmpSim.msgStore[k].font, sizeof(Font));
    }
    Message_ZmpStatics(gZmpSim.msgStatics[k], 1);
    gZmpSim.msgLoaded = (s8)k;
}

// Every slot starts from the text box state of this moment, closed (the slot in context keeps its own).
void MsgInitSlots(PlayState* play) {
    int c = gZmpSim.ctx;
    for (int k = 0; k <= ZMP_MAX_PLAYERS; k++) {
        MessageContext& m = gZmpSim.msgStore[k];
        memcpy(&m, &play->msgCtx, sizeof(MessageContext));
        Message_ZmpStatics(gZmpSim.msgStatics[k], 0);
        if (k < ZMP_MAX_PLAYERS) {
            m.textboxSegment = gZmpSim.msgSegment[k];
        }
        if (k != c) {
            m.msgMode = MSGMODE_NONE;
            m.msgLength = 0;
        }
    }
    if (Valid(c)) {
        if (play->msgCtx.textboxSegment != nullptr && play->msgCtx.textboxSegment != (void*)gZmpSim.msgSegment[c]) {
            memmove(gZmpSim.msgSegment[c], play->msgCtx.textboxSegment, sizeof(gZmpSim.msgSegment[c]));
        }
        play->msgCtx.textboxSegment = gZmpSim.msgSegment[c];
    }
    gZmpSim.msgLoaded = (s8)c;
    gZmpSim.msgReady = 1;
}

// A player who appears (or left) has no text box open.
void MsgResetSlot(PlayState* play, int k) {
    if (!gZmpSim.msgReady || !Valid(k)) {
        return;
    }
    const MessageContext& idle = gZmpSim.msgStore[ZMP_MAX_PLAYERS];
    if (k == gZmpSim.msgLoaded) {
        void* seg = play->msgCtx.textboxSegment;
        MsgCopySmall(&play->msgCtx, &idle);
        play->msgCtx.textboxSegment = seg;
        Message_ZmpStatics(gZmpSim.msgStatics[ZMP_MAX_PLAYERS], 1);
        return;
    }
    MsgCopySmall(&gZmpSim.msgStore[k], &idle);
    gZmpSim.msgStore[k].textboxSegment = gZmpSim.msgSegment[k];
    memcpy(gZmpSim.msgStatics[k], gZmpSim.msgStatics[ZMP_MAX_PLAYERS], ZMP_MSG_STATICS_SIZE);
}

const MessageContext* SlotMsg(int k) {
    if (gPlayState == nullptr) {
        return nullptr;
    }
    if (!gZmpSim.msgReady || !Valid(k) || k == gZmpSim.msgLoaded) {
        return &gPlayState->msgCtx;
    }
    return &gZmpSim.msgStore[k];
}

void Load(PlayState* play, int k) {
    ZmpPlayerSlot& s = Slot(k);
    MsgSwitch(play, k);
    memcpy(&play->mainCamera, &s.camera, sizeof(Camera));
    memcpy(&play->actorCtx.targetCtx, &s.target, sizeof(TargetContext));
    LoadFromBlock(s.block);
    play->state.input[0] = s.input;
    play->activeCamera = s.activeCam;
    gZmpCtxPlayer = s.player;
    // curRoom is the room of the player in context (Rooms.cpp)
    Zmp::Players::ArrangeRooms(play, k);
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
    if (actor->id == ACTOR_DOOR_WARP1 && Present(gZmpSim.warpOwner)) {
        return gZmpSim.warpOwner; // the blue warp acts on the player floating in it (phase 5b)
    }
    if (actor->parent != nullptr && actor->parent->id == ACTOR_PLAYER) {
        k = Zmp::Players::SlotOf(actor->parent);
        if (k >= 0) {
            return k;
        }
    }
    // Phase 5b, one text box per player: whoever a player is talking with runs with that player's text box (and
    // attends nobody else meanwhile), whoever is nearer.
    for (int j = 0; j < ZMP_MAX_PLAYERS; j++) {
        if (Present(j) && Slot(j).player->talkActor == actor && (Slot(j).player->stateFlags1 & PLAYER_STATE1_TALKING)) {
            return j;
        }
    }
    // What listens to the ocarina hears the player who plays it (its song lives in that player's text box state).
    int o = gZmpSim.msgOwner;
    if (Present(o)) {
        const MessageContext* m = SlotMsg(o);
        if (m != nullptr && m->ocarinaMode != OCARINA_MODE_00 && m->ocarinaMode != OCARINA_MODE_04) {
            const Vec3f& a = actor->world.pos;
            const Vec3f& b = Slot(o).player->actor.world.pos;
            f32 dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
            if (dx * dx + dy * dy + dz * dz < 600.0f * 600.0f) {
                return o;
            }
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
    // Its own main camera, not queued behind the anchor's cutscenes (Cameras.cpp keeps the global ones).
    s.camera.status = CAM_STAT_ACTIVE;
    s.camera.childCamIdx = SUBCAM_FREE;
    s.camera.parentCamIdx = SUBCAM_FREE;
    s.activeCam = CAM_ID_MAIN;
    s.room = play->roomCtx.curRoom.num;
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

// A spawn spot with floor near the reference's height, not a scene exit, a void or lava (phase 4: the side offset of
// the third player could be over the pit of Gohma's lair). Tries the side offsets n, n+2, n+4..., then behind the
// reference, then the reference itself.
bool GoodFloor(PlayState* play, Vec3f& pos, f32 refY) {
    Vec3f probe = pos;
    probe.y = refY + 50.0f;
    CollisionPoly* poly = nullptr;
    s32 bgId = 0;
    f32 y = BgCheck_EntityRaycastFloor3(&play->colCtx, &poly, &bgId, &probe);
    if (y == BGCHECK_Y_MIN || poly == nullptr || fabsf(y - refY) > 40.0f) {
        return false;
    }
    if (SurfaceType_GetSceneExitIndex(&play->colCtx, poly, bgId) != 0) {
        return false;
    }
    u32 floorType = SurfaceType_GetFloorType(&play->colCtx, poly, bgId);
    if (floorType == 5 || floorType == 9 || floorType == 12) {
        return false;
    }
    pos.y = y;
    return true;
}

Vec3f SafeSpawnPos(PlayState* play, const Vec3f& base, s16 yaw, int n) {
    for (int m = n; m < n + 10; m += 2) {
        Vec3f pos = SideOffset(base, yaw, m);
        if (GoodFloor(play, pos, base.y)) {
            return pos;
        }
    }
    for (int m = n + 1; m < n + 10; m += 2) {
        Vec3f pos = SideOffset(base, yaw, m);
        if (GoodFloor(play, pos, base.y)) {
            return pos;
        }
    }
    for (int d = 1; d <= 3; d++) {
        Vec3f pos = base;
        pos.x -= Math_SinS(yaw) * 45.0f * d;
        pos.z -= Math_CosS(yaw) * 45.0f * d;
        if (GoodFloor(play, pos, base.y)) {
            return pos;
        }
    }
    return base;
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

namespace {
uint32_t sPlayInits = 0;
}

uint32_t Zmp::Players::PlayInitCount() {
    return sPlayInits;
}

extern "C" void Zmp_PlayInitBegin(PlayState* play) {
    sPlayInits++;
    Zmp::Pause::Reset();
    Zmp::Players::PresentReset();
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
        Slot(k).activeCam = CAM_ID_MAIN;
        Slot(k).room = -1;
        Slot(k).heatState = 0;
    }
    for (int i = 0; i < NUM_CAMS; i++) {
        gZmpSim.camScope[i] = ZMP_CAM_GLOBAL;
        gZmpSim.camCreator[i] = -1;
    }
    // The original heat / deep water timer is not used in multiplayer (each player has its own).
    if (gSaveContext.timerState >= TIMER_STATE_ENV_HAZARD_INIT &&
        gSaveContext.timerState <= TIMER_STATE_ENV_HAZARD_TICK) {
        gSaveContext.timerState = TIMER_STATE_OFF;
    }
    gZmpSim.globalCs = 0;
    gZmpSim.csTrigger = -1;
    gZmpSim.csStarter = -1;
    gZmpSim.extraRoomCount = 0;
    gZmpSim.loadingRoom = -1;
    gZmpSim.setupRoom = -1;
    gZmpSim.queuedRoomCount = 0;
    int anchor = LowestActive();
    gZmpSim.anchor = (s8)anchor;
    gZmpSim.ctx = (s8)anchor;
    gZmpSim.spawningSlot = (s8)anchor;
    gZmpSim.msgOwner = -1;
    gZmpSim.pauseOwner = -1;
    gZmpSim.transitionBy = -1;
    gZmpSim.undoValid = 0;
    gZmpSim.warpOwner = -1;
    gZmpSim.transitionSolo = 0;
    gZmpSim.inviteBy = -1;
    gZmpSim.inviteTicks = 0;
    for (int k = 0; k < ZMP_MAX_PLAYERS; k++) {
        Slot(k).warpPending = 0;
        Slot(k).inviteHold = 0;
    }
    gZmpCtxPlayer = nullptr;
    gZmpSim.inPlay = 1;
    gZmpSim.groupDefeat = 0;
    MsgInitSlots(play); // (Message_Init already ran for this Play)
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
    // Everybody starts in the room of the entrance.
    for (int k = 0; k < ZMP_MAX_PLAYERS; k++) {
        Slot(k).room = play->roomCtx.curRoom.num;
    }
    int n = 0;
    for (int k = 0; k < ZMP_MAX_PLAYERS; k++) {
        if (k == anchor || !Slot(k).active) {
            continue;
        }
        n++;
        Vec3f pos = SafeSpawnPos(play, lead->actor.world.pos, lead->actor.shape.rot.y, n);
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
            // A player's main camera drives only that player's letterbox and HUD (Present.cpp, phase 4).
            gZmpCameraInterfaceMuted = k + 1;
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

extern "C" s32 Zmp_AgeSwapAll(PlayState* play) {
    if (!gZmpSim.enabled || !Valid(gZmpSim.ctx)) {
        return 0;
    }
    // (the save context holds the block of the slot in context)
    SaveToBlock(Slot(gZmpSim.ctx).block);
    for (int k = 0; k < ZMP_MAX_PLAYERS; k++) {
        if (Slot(k).active) {
            SwapSlotAge(Slot(k));
        }
    }
    LoadFromBlock(Slot(gZmpSim.ctx).block);
    Zmp::Log("zmp: age change, every player's equipment swapped");
    return 1;
}

extern "C" s32 Zmp_OcarinaBusy(void) {
    if (!Zmp_MultiActive()) {
        return 0;
    }
    int o = gZmpSim.msgOwner;
    if (!Present(o) || o == gZmpSim.ctx) {
        return 0;
    }
    const MessageContext* m = SlotMsg(o);
    return m != nullptr && m->msgMode != MSGMODE_NONE;
}

extern "C" s32 Zmp_MessageUpdateAll(PlayState* play) {
    if (!Zmp_MultiActive()) {
        return 0;
    }
    for (int k = 0; k < ZMP_MAX_PLAYERS; k++) {
        if (Present(k)) {
            SwitchTo(play, k);
            Message_Update(play);
        }
    }
    SwitchTo(play, gZmpSim.anchor);
    return 1;
}

extern "C" s32 Zmp_MessageDrawAll(PlayState* play) {
    if (!Zmp_MultiActive()) {
        return 0;
    }
    // The text box / ocarina runs part of its logic while drawing (it spawns the song effects): every player's is
    // "drawn" on every machine, and what is not for this screen is thrown away (Zmp_MessageDrawBegin).
    for (int k = 0; k < ZMP_MAX_PLAYERS; k++) {
        if (Present(k)) {
            SwitchTo(play, k);
            Zmp_MessageDrawBegin(play);
            Message_Draw(play);
            Zmp_MessageDrawEnd(play);
        }
    }
    return 1;
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

// Heat / deep water timer of the player in context (phase 4). Same rules and timing as the original's (Interface_Update
// starts and stops it, Interface_Draw counted it down), but with the player's own room, tunic, health and timer.
void HazardInContext(void* p) {
    PlayState* play = (PlayState*)p;
    int k = gZmpSim.ctx;
    ZmpPlayerSlot& s = Slot(k);
    if (s.downed || gZmpSim.groupDefeat) {
        s.heatState = 0;
        return;
    }
    s32 hz = Player_GetEnvironmentalHazard(play);
    if (hz == PLAYER_ENV_HAZARD_HOTROOM) {
        if (CUR_EQUIP_VALUE(EQUIP_TYPE_TUNIC) == EQUIP_VALUE_TUNIC_GORON) {
            hz = PLAYER_ENV_HAZARD_NONE;
        }
    } else if (hz >= 2 && hz < 5) {
        if (CUR_EQUIP_VALUE(EQUIP_TYPE_TUNIC) == EQUIP_VALUE_TUNIC_ZORA) {
            hz = PLAYER_ENV_HAZARD_NONE;
        }
    }
    bool hazard = hz == PLAYER_ENV_HAZARD_HOTROOM || hz == PLAYER_ENV_HAZARD_UNDERWATER_FLOOR ||
                  hz == PLAYER_ENV_HAZARD_UNDERWATER_FREE;
    if (s.heatState == 0) {
        if (hazard && (gSaveContext.health >> 1) != 0) {
            s.heatState = 1;
            s.heatSeconds = gSaveContext.health >> 1;
            s.heatTicks = 20;
            s.heatPreview = 41; // init, 20 ticks of preview, 20 of moving to its place
        }
        return;
    }
    if (!hazard) {
        s.heatState = 0;
        return;
    }
    if (s.heatState == 3) {
        return; // ran out: stays stopped until the hazard ends
    }
    Player* player = s.player;
    if (!((play->gameOverCtx.state == GAMEOVER_INACTIVE) && (play->msgCtx.msgMode == MSGMODE_NONE) &&
          !(player->stateFlags2 & PLAYER_STATE2_ATTEMPT_PLAY_FOR_ACTOR) &&
          (play->transitionTrigger == TRANS_TRIGGER_OFF) && (play->transitionMode == TRANS_MODE_OFF) &&
          !Play_InCsMode(play))) {
        return;
    }
    if (s.heatPreview > 0) {
        if (--s.heatPreview == 0) {
            s.heatState = 2;
        }
        return;
    }
    if (play->msgCtx.msgLength != 0 || --s.heatTicks != 0) {
        return;
    }
    s.heatTicks = 20;
    if (s.heatSeconds != 0) {
        s.heatSeconds--;
    }
    if (s.heatSeconds == 0) {
        s.heatState = 3;
        Zmp::Log("zmp: slot " + std::to_string(k) + ": heat / water timer ran out");
        gSaveContext.health = 0;
        play->damagePlayer(play, -(gSaveContext.health + 2));
    } else if (k == sLocalSlot) {
        Audio_PlaySfxGeneral(s.heatSeconds >= 11 ? NA_SE_SY_WARNING_COUNT_N : NA_SE_SY_WARNING_COUNT_E, &gSfxDefaultPos,
                             4, &gSfxDefaultFreqAndVolScale, &gSfxDefaultFreqAndVolScale, &gSfxDefaultReverb);
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
    // Heat / deep water timers, each player with its own room, tunic and health.
    for (int k = 0; k < ZMP_MAX_PLAYERS; k++) {
        if (Present(k)) {
            Zmp::Players::RunInContext(k, HazardInContext, play);
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

extern "C" s32 Zmp_OwnHazardTimers(void) {
    return Zmp_MultiActive();
}

extern "C" s32 Zmp_PauseIsLocal(void) {
    return Zmp_MultiActive();
}

extern "C" {
void DoorWarp1_ZmpRelease(Actor* thisx, PlayState* play);
}

extern "C" void Zmp_WarpBegin(Player* player) {
    if (Zmp_MultiActive()) {
        gZmpSim.warpOwner = (s8)Zmp::Players::SlotOf(&player->actor);
    }
}

extern "C" void Zmp_WarpTransition(Player* player) {
    if (!Zmp_MultiActive()) {
        return;
    }
    int k = Present(gZmpSim.warpOwner) ? gZmpSim.warpOwner : Zmp::Players::SlotOf(&player->actor);
    if (k >= 0) {
        gZmpSim.transitionBy = (s8)k;
        gZmpSim.transitionSolo = 1;
    }
}

extern "C" s32 Zmp_WarpIsShared(void) {
    return Zmp_MultiActive() && Zmp::Players::PresentCount() >= 2;
}

extern "C" s32 Zmp_WarpSongStart(Player* player, PlayState* play) {
    if (!Zmp_MultiActive() || Zmp::Players::PresentCount() < 2 || !Zmp::Lockstep::SceneGroups()) {
        return 0;
    }
    int k = Zmp::Players::SlotOf(&player->actor);
    if (k < 0) {
        return 0;
    }
    gZmpSim.inviteBy = (s8)k;
    gZmpSim.inviteEntrance = gSaveContext.respawn[RESPAWN_MODE_RETURN].entranceIndex;
    gZmpSim.inviteSong = gSaveContext.respawn[RESPAWN_MODE_RETURN].data;
    gZmpSim.inviteTicks = ZMP_INVITE_TICKS;
    gZmpSim.inviteLeaveIn = ZMP_INVITE_LEAVE_TICKS;
    // (the song is over for the ones who stay: the next ocarina of anybody must not find "warp chosen")
    play->msgCtx.ocarinaMode = OCARINA_MODE_04;
    for (int j = 0; j < ZMP_MAX_PLAYERS; j++) {
        Slot(j).inviteHold = 0;
        Slot(j).warpPending = 0;
    }
    Zmp::Log("zmp: slot " + std::to_string(k) + " warps with a song to entrance " +
             std::to_string(gZmpSim.inviteEntrance) + "; the others are invited");
    return 1;
}

namespace {
// Every tick without a pending scene change: the invitation runs (who holds L joins), and one player per tick starts
// its own warp (the same scene change the song makes: respawn data of the warp pad, white fade).
void SongWarpTick(PlayState* play) {
    if (gZmpSim.inviteTicks > 0 && Valid(gZmpSim.inviteBy)) {
        gZmpSim.inviteTicks--;
        if (gZmpSim.inviteLeaveIn > 0 && --gZmpSim.inviteLeaveIn == 0 && Present(gZmpSim.inviteBy)) {
            Slot(gZmpSim.inviteBy).warpPending = 1;
        }
        for (int j = 0; j < ZMP_MAX_PLAYERS; j++) {
            if (!Present(j) || j == gZmpSim.inviteBy || Slot(j).downed || Slot(j).warpPending) {
                continue;
            }
            Player* p = Slot(j).player;
            bool busy =
                (p->stateFlags1 & (PLAYER_STATE1_DEAD | PLAYER_STATE1_IN_CUTSCENE | PLAYER_STATE1_TALKING |
                                   PLAYER_STATE1_LOADING | PLAYER_STATE1_GETTING_ITEM | PLAYER_STATE1_IN_ITEM_CS)) != 0;
            bool held = (SlotInput(play, j).cur.button & BTN_L) != 0 && !busy;
            Slot(j).inviteHold = held ? (s16)(Slot(j).inviteHold + 1) : (s16)0;
            if (Slot(j).inviteHold >= ZMP_INVITE_HOLD_TICKS) {
                Slot(j).inviteHold = 0;
                Slot(j).warpPending = 1;
                Zmp::Log("zmp: slot " + std::to_string(j) + " accepts the warp invitation");
            }
        }
        if (gZmpSim.inviteTicks == 0) {
            gZmpSim.inviteBy = -1;
        }
    }
    for (int k = 0; k < ZMP_MAX_PLAYERS; k++) {
        if (!Slot(k).warpPending) {
            continue;
        }
        Slot(k).warpPending = 0;
        if (!Present(k)) {
            continue;
        }
        gSaveContext.respawn[RESPAWN_MODE_RETURN].entranceIndex = gZmpSim.inviteEntrance;
        gSaveContext.respawn[RESPAWN_MODE_RETURN].playerParams = 0x5FF;
        gSaveContext.respawn[RESPAWN_MODE_RETURN].data = gZmpSim.inviteSong;
        gSaveContext.respawnFlag = -3;
        play->nextEntranceIndex = gZmpSim.inviteEntrance;
        play->transitionTrigger = TRANS_TRIGGER_START;
        play->transitionType = TRANS_TYPE_FADE_WHITE_FAST;
        gSaveContext.nextTransitionType = TRANS_TYPE_FADE_WHITE;
        gZmpSim.transitionBy = (s8)k;
        return;
    }
}
} // namespace

Zmp::Players::WarpInvite Zmp::Players::Invite() {
    WarpInvite w;
    if (Zmp_MultiActive() && gZmpSim.inviteTicks > 0 && Valid(gZmpSim.inviteBy)) {
        w.by = gZmpSim.inviteBy;
        int e = gZmpSim.inviteEntrance;
        w.scene = e >= 0 && e < ENTR_MAX ? gEntranceTable[e].scene : -1;
        w.ticks = gZmpSim.inviteTicks;
        w.hold = Valid(sLocalSlot) ? Slot(sLocalSlot).inviteHold : 0;
    }
    return w;
}

extern "C" s32 Zmp_PlayerOutsideRoom(Player* player, s32 room) {
    if (!Zmp_MultiActive() || player == nullptr) {
        return 0;
    }
    int k = Zmp::Players::SlotOf(&player->actor);
    return Present(k) && Slot(k).room >= 0 && Slot(k).room != room ? 1 : 0;
}

extern "C" s32 Zmp_ExitsLocked(void) {
    return Zmp_MultiActive() && gZmpSim.exitsLocked;
}

extern "C" void Zmp_NoteTransitionBy(Player* player) {
    if (!Zmp_MultiActive() || player == nullptr) {
        return;
    }
    int k = Zmp::Players::SlotOf(&player->actor);
    if (k >= 0 && gZmpSim.transitionBy < 0) {
        gZmpSim.transitionBy = (s8)k;
    }
}

// Phase 5 (PLAN.md 2.11, docs/DECISIONES.md D-062): no countdown. A scene change started by one player's own action
// (walking into an exit, a void, Farore's Wind) takes only that player: on its machine the transition goes on and the
// client leaves the group; on the others its Link disappears and the scene stays. Scene changes of the world (a
// scripted cutscene, a warp song, the group game over, the blue warp) still take the whole group, which then belongs to
// the new scene (the client reports it).
extern "C" s32 Zmp_TransitionGate(PlayState* play) {
    if (!Zmp_MultiActive() || play->transitionMode != TRANS_MODE_OFF) {
        return 1;
    }
    if (play->transitionTrigger == TRANS_TRIGGER_OFF) {
        // What an exit may change in the save context, as it was before any exit (restored if the group stays).
        gZmpSim.undoRespawnFlag = gSaveContext.respawnFlag;
        memcpy(gZmpSim.undoRespawn, gSaveContext.respawn, sizeof(gZmpSim.undoRespawn));
        gZmpSim.undoEntranceSpeed = gSaveContext.entranceSpeed;
        gZmpSim.undoNextTransitionType = gSaveContext.nextTransitionType;
        gZmpSim.undoRetainWeather = gSaveContext.retainWeatherMode;
        gZmpSim.undoSeqId = gSaveContext.seqId;
        gZmpSim.undoNatureId = gSaveContext.natureAmbienceId;
        gZmpSim.undoHaltAll = play->haltAllActors;
        gZmpSim.undoNextCutsceneIndex = gSaveContext.nextCutsceneIndex;
        gZmpSim.undoValid = 1;
        gZmpSim.transitionBy = -1;
        gZmpSim.transitionSolo = 0;
        if (gZmpSim.agePending != 0) {
            // Phase 5b: the room changed age in another group. This scene reloads with the new age (a scene change
            // of the whole group to the entrance it came in by), once no cutscene or menu is in the way.
            int age = gZmpSim.agePending - 1;
            if (age == gSaveContext.linkAge) {
                gZmpSim.agePending = 0;
            } else if (play->csCtx.state == CS_STATE_IDLE && !gZmpSim.globalCs && gSaveContext.cutsceneIndex < 0xFFF0 &&
                       play->pauseCtx.state == 0 && play->gameOverCtx.state == GAMEOVER_INACTIVE) {
                play->linkAgeOnLoad = age;
                play->nextEntranceIndex = gSaveContext.entranceIndex;
                play->transitionTrigger = TRANS_TRIGGER_START;
                play->transitionType = TRANS_TYPE_FADE_WHITE;
                gSaveContext.nextTransitionType = TRANS_TYPE_FADE_WHITE;
                Zmp::Log("zmp: reloading the scene with the room's new age");
            }
        }
        if (play->transitionTrigger == TRANS_TRIGGER_OFF) {
            SongWarpTick(play); // (may start the warp of one player, handled right below)
        }
        if (play->transitionTrigger == TRANS_TRIGGER_OFF) {
            return 1;
        }
    }
    int k = gZmpSim.transitionBy;
    bool solo = gZmpSim.transitionSolo != 0;
    gZmpSim.transitionBy = -1;
    gZmpSim.transitionSolo = 0;
    gZmpSim.transitionEntrance = play->nextEntranceIndex;
    // (a blue warp takes its player alone even with the cutscene of the destination, phase 5b)
    bool own = Present(k) && play->gameOverCtx.state == GAMEOVER_INACTIVE && !gZmpSim.groupDefeat &&
               (solo ||
                (play->csCtx.state == CS_STATE_IDLE && !gZmpSim.globalCs && gSaveContext.nextCutsceneIndex < 0xFFF0)) &&
               Zmp::Lockstep::SceneGroups();
    if (!own) {
        Zmp::Log("zmp: scene change of the whole group to entrance " + std::to_string(play->nextEntranceIndex));
        return 1;
    }
    if (k == sLocalSlot) {
        // This machine's player leaves: the transition happens here, the others stay where they are.
        Zmp::Log("zmp: this player leaves the group through entrance " + std::to_string(play->nextEntranceIndex));
        Zmp::Players::KeepOnlyLocal();
        Zmp::Lockstep::DetachForTransition(play->nextEntranceIndex, gSaveContext.nextCutsceneIndex >= 0xFFF0);
        return 1;
    }
    // Another player leaves: its Link goes away, the scene and everything the exit touched stay as they were.
    play->transitionTrigger = TRANS_TRIGGER_OFF;
    if (gZmpSim.undoValid) {
        gSaveContext.respawnFlag = gZmpSim.undoRespawnFlag;
        memcpy(gSaveContext.respawn, gZmpSim.undoRespawn, sizeof(gZmpSim.undoRespawn));
        gSaveContext.entranceSpeed = gZmpSim.undoEntranceSpeed;
        gSaveContext.nextTransitionType = gZmpSim.undoNextTransitionType;
        gSaveContext.retainWeatherMode = gZmpSim.undoRetainWeather;
        gSaveContext.seqId = gZmpSim.undoSeqId;
        gSaveContext.natureAmbienceId = gZmpSim.undoNatureId;
        play->haltAllActors = gZmpSim.undoHaltAll;
        gSaveContext.nextCutsceneIndex = gZmpSim.undoNextCutsceneIndex;
    }
    if (gZmpSim.warpOwner == k) {
        // The blue warp is free again for the next player.
        gZmpSim.warpOwner = -1;
        for (Actor* a = play->actorCtx.actorLists[ACTORCAT_ITEMACTION].head; a != nullptr; a = a->next) {
            if (a->id == ACTOR_DOOR_WARP1) {
                DoorWarp1_ZmpRelease(a, play);
            }
        }
    }
    Zmp::Log("zmp: slot " + std::to_string(k) + " leaves the scene through entrance " +
             std::to_string(play->nextEntranceIndex) + " (its Link goes away here)");
    Zmp::Players::Despawn(k);
    return 0;
}

extern "C" void Zmp_DrawBeginView(PlayState* play) {
    sDrawBegun = false;
    sSfxRemapValid = false;
    if (!Zmp_MultiActive()) {
        return;
    }
    // Canonical view: the camera state the simulation left in play->view (anchor or active sub camera).
    View sim = play->view;
    ComputeViewMatrices(&sim, Zmp_SimFogFar(play), &sSimVP, &sSimBillboard);
    sSimView = sim;
    sDrawBegun = true;
    int L = sLocalSlot;
    // Rooms with a pre-rendered background (houses, shops) only look right from their one fixed camera:
    // everybody draws with the canonical view there.
    bool prerendered = play->roomCtx.curRoom.meshHeader != nullptr && play->roomCtx.curRoom.meshHeader->base.type == 1;
    // A cutscene is seen by everybody through the same camera (PLAN.md 2.9: scripted cutscenes are GLOBAL).
    bool cutscene = play->csCtx.state != CS_STATE_IDLE || gSaveContext.cutsceneIndex >= 0xFFF0;
    // Otherwise each player sees its own active camera (phase 4): its main camera, or a sub camera of its own
    // one-point cutscene (chest, item, crawlspace...), or the global cutscene camera everybody shares.
    if (!prerendered && !cutscene && L != gZmpSim.anchor && Present(L)) {
        const View* local = Zmp::Players::LocalPicture(play, L);
        if (local != nullptr) {
            play->view = *local;
            // (sound follows the picture: heard from the camera this machine draws with)
            View heard = *local;
            MtxF localVP;
            MtxF unused;
            ComputeViewMatrices(&heard, Zmp_SimFogFar(play), &localVP, &unused);
            ComputeSfxRemap(sSimVP, localVP);
        }
    }
}

namespace {
void ComputeSfxRemap(const MtxF& sim, const MtxF& loc) {
    sSfxRemapValid = false;
    double a[3][3] = { { sim.xx, sim.xy, sim.xz }, { sim.yx, sim.yy, sim.yz }, { sim.zx, sim.zy, sim.zz } };
    double t[3] = { sim.xw, sim.yw, sim.zw };
    double b[3][3] = { { loc.xx, loc.xy, loc.xz }, { loc.yx, loc.yy, loc.yz }, { loc.zx, loc.zy, loc.zz } };
    double u[3] = { loc.xw, loc.yw, loc.zw };
    double det = a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
                 a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
    if (!(det > 1e-12 || det < -1e-12)) {
        return;
    }
    double inv[3][3];
    inv[0][0] = (a[1][1] * a[2][2] - a[1][2] * a[2][1]) / det;
    inv[0][1] = (a[0][2] * a[2][1] - a[0][1] * a[2][2]) / det;
    inv[0][2] = (a[0][1] * a[1][2] - a[0][2] * a[1][1]) / det;
    inv[1][0] = (a[1][2] * a[2][0] - a[1][0] * a[2][2]) / det;
    inv[1][1] = (a[0][0] * a[2][2] - a[0][2] * a[2][0]) / det;
    inv[1][2] = (a[0][2] * a[1][0] - a[0][0] * a[1][2]) / det;
    inv[2][0] = (a[1][0] * a[2][1] - a[1][1] * a[2][0]) / det;
    inv[2][1] = (a[0][1] * a[2][0] - a[0][0] * a[2][1]) / det;
    inv[2][2] = (a[0][0] * a[1][1] - a[0][1] * a[1][0]) / det;
    for (int i = 0; i < 3; i++) {
        double m[3];
        for (int j = 0; j < 3; j++) {
            m[j] = b[i][0] * inv[0][j] + b[i][1] * inv[1][j] + b[i][2] * inv[2][j];
            sSfxRemap[i][j] = (float)m[j];
        }
        sSfxRemap[i][3] = (float)(u[i] - (m[0] * t[0] + m[1] * t[1] + m[2] * t[2]));
    }
    sSfxRemapValid = true;
}
} // namespace

extern "C" f32 Zmp_SfxCoord(const f32* posX, s32 axis) {
    // Only positions inside the game heap are projected actor positions; fixed positions of the code (the "no
    // position" default, interface sounds) are not in the camera's space and stay as they are.
    uintptr_t p = (uintptr_t)posX;
    uintptr_t h = (uintptr_t)gSystemHeap;
    if (!sSfxRemapValid || p < h || p >= h + SYSTEM_HEAP_SIZE) {
        return posX[axis];
    }
    const float* m = sSfxRemap[axis];
    return m[0] * posX[0] + m[1] * posX[1] + m[2] * posX[2] + m[3];
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

void SwitchContext(PlayState* play, int slot) {
    if (Present(slot)) {
        SwitchTo(play, slot);
    }
}

bool IsPresent(int slot) {
    return Present(slot);
}

int ActorTargetSlot(const Actor* actor) {
    return ContextSlot(actor);
}

const Actor* CurrentActor() {
    return sTracePhase == 'U' ? sTraceActor : nullptr;
}

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
    if (gZmpSim.enabled && gZmpSim.inPlay && Present(sLocalSlot)) {
        // A client that was already in a multiplayer game (server restart, regroup): its own Link founds the new
        // simulation with its own block; the other Links it still had are gone.
        int L = sLocalSlot;
        SwitchTo(play, L);
        player = Slot(L).player;
        for (int k = 0; k < ZMP_MAX_PLAYERS; k++) {
            if (k != L && Present(k)) {
                Player* p = Slot(k).player;
                if (p->naviActor != nullptr) {
                    Actor_Kill(p->naviActor);
                }
                Actor_Kill(&p->actor);
            }
        }
    }
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
        Slot(k).activeCam = CAM_ID_MAIN;
        Slot(k).room = -1;
    }
    s.activeCam = play->activeCamera;
    s.room = play->roomCtx.curRoom.num;
    {
        // (what the founder's own save keeps for its other age)
        const ItemEquips& o =
            gSaveContext.linkAge == LINK_AGE_CHILD ? gSaveContext.adultEquips : gSaveContext.childEquips;
        s.otherEquips = o;
        s.otherValid = o.buttonItems[0] != ITEM_NONE;
    }
    MsgInitSlots(play);
    if (gSaveContext.timerState >= TIMER_STATE_ENV_HAZARD_INIT &&
        gSaveContext.timerState <= TIMER_STATE_ENV_HAZARD_TICK) {
        gSaveContext.timerState = TIMER_STATE_OFF;
    }
    for (int i = 0; i < NUM_CAMS; i++) {
        gZmpSim.camScope[i] = ZMP_CAM_GLOBAL;
        gZmpSim.camCreator[i] = (s8)slot;
    }
    gZmpSim.csTrigger = -1;
    gZmpSim.csStarter = -1;
    gZmpSim.extraRoomCount = 0;
    gZmpSim.loadingRoom = -1;
    gZmpSim.setupRoom = -1;
    gZmpSim.queuedRoomCount = 0;
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

namespace {
// "zmp_spawn entrance place x y z yaw room health block..." (LocalSpawnInfo); block = magic b0..b7 c0..c6 equipment
// ammo0..15 bottle0..3.
struct SpawnInfo {
    bool valid = false;
    int entrance = -1;
    bool place = false;
    Vec3f pos = {};
    s16 yaw = 0;
    int room = -1;
    int health = 0;
    int age = -1; // linkAge the block is of (-1: not told, taken as the group's)
    ZmpPlayerBlock block = {};
};

SpawnInfo ParseSpawnInfo(const std::string& info) {
    SpawnInfo si;
    if (info.rfind("zmp_spawn ", 0) != 0) {
        return si;
    }
    std::vector<int> v;
    size_t pos = 10;
    while (pos < info.size()) {
        size_t next = info.find(' ', pos);
        std::string tok = info.substr(pos, next == std::string::npos ? std::string::npos : next - pos);
        if (!tok.empty()) {
            v.push_back(atoi(tok.c_str()));
        }
        if (next == std::string::npos) {
            break;
        }
        pos = next + 1;
    }
    if (v.size() != 45 && v.size() != 46) {
        return si;
    }
    si.age = v.size() > 45 ? v[45] : -1;
    si.valid = true;
    si.entrance = v[0];
    si.place = v[1] != 0;
    si.pos = { (f32)v[2], (f32)v[3], (f32)v[4] };
    si.yaw = (s16)v[5];
    si.room = v[6];
    si.health = v[7];
    ZmpPlayerBlock& b = si.block;
    b.magic = (s8)v[8];
    for (int i = 0; i < 8; i++) {
        b.equips.buttonItems[i] = (u8)v[9 + i];
    }
    for (int i = 0; i < 7; i++) {
        b.equips.cButtonSlots[i] = (u8)v[17 + i];
    }
    b.equips.equipment = (u16)v[24];
    for (int i = 0; i < 16; i++) {
        b.ammo[i] = (s8)v[25 + i];
    }
    for (int i = 0; i < 4; i++) {
        b.bottles[i] = (u8)v[41 + i];
    }
    return si;
}

bool RoomLoaded(PlayState* play, int room) {
    for (int r : Zmp::Players::LoadedRooms(play)) {
        if (r == room) {
            return true;
        }
    }
    return false;
}
} // namespace

std::string LocalSpawnInfo(int entrance, bool withPlace) {
    ZmpPlayerBlock b{};
    Vec3f pos = {};
    s16 yaw = 0;
    int room = -1;
    int health = 0;
    int L = sLocalSlot;
    if (Zmp_MultiActive() && Present(L)) {
        b = SlotBlock(L);
        health = SlotHealth(L);
        Player* p = Slot(L).player;
        pos = p->actor.world.pos;
        yaw = p->actor.shape.rot.y;
        room = Slot(L).room;
    } else {
        SaveToBlock(b);
        health = gSaveContext.health;
        Player* p = gPlayState != nullptr ? (Player*)gPlayState->actorCtx.actorLists[ACTORCAT_PLAYER].head : nullptr;
        if (p != nullptr) {
            pos = p->actor.world.pos;
            yaw = p->actor.shape.rot.y;
            room = gPlayState->roomCtx.curRoom.num;
        } else {
            withPlace = false;
        }
    }
    char head[128];
    snprintf(head, sizeof(head), "zmp_spawn %d %d %d %d %d %d %d %d", entrance, withPlace ? 1 : 0, (int)pos.x,
             (int)pos.y, (int)pos.z, (int)yaw, room, health);
    std::string s = head;
    s += " " + std::to_string(b.magic);
    for (int i = 0; i < 8; i++) {
        s += " " + std::to_string(b.equips.buttonItems[i]);
    }
    for (int i = 0; i < 7; i++) {
        s += " " + std::to_string(b.equips.cButtonSlots[i]);
    }
    s += " " + std::to_string(b.equips.equipment);
    for (int i = 0; i < 16; i++) {
        s += " " + std::to_string(b.ammo[i]);
    }
    for (int i = 0; i < 4; i++) {
        s += " " + std::to_string(b.bottles[i]);
    }
    s += " " + std::to_string((int)gSaveContext.linkAge); // (the age those buttons and equipment are of, phase 5b)
    return s;
}

void KeepOnlyLocal() {
    // The other Links stay in the scene being left (standing still until it unloads) and are not spawned in the next
    // one: only this machine's player is active.
    for (int k = 0; k < ZMP_MAX_PLAYERS; k++) {
        if (k != sLocalSlot) {
            Slot(k).active = 0;
        }
    }
    if (gZmpSim.msgOwner != sLocalSlot) {
        gZmpSim.msgOwner = -1;
    }
}

void Spawn(int slot, const std::string& infoText) {
    PlayState* play = gPlayState;
    if (!Valid(slot) || play == nullptr || !Zmp_MultiActive()) {
        return;
    }
    SpawnInfo info = ParseSpawnInfo(infoText);
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
    s.otherValid = 0;
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
            // having a bottle is shared (an empty one); a player who comes from another group keeps its contents
            u8 own = info.valid ? info.block.bottles[i] : (u8)ITEM_NONE;
            s.block.bottles[i] = own != ITEM_NONE ? own : (u8)ITEM_BOTTLE;
        }
    }
    if (info.valid) {
        // Phase 5: the player keeps its own health, magic, equipment, buttons and ammo from the group it left.
        s.block.magic = (s8)MIN((s16)info.block.magic, magicFull);
        s.block.magicFillTarget = s.block.magic;
        s.block.equips = info.block.equips;
        memcpy(s.block.ammo, info.block.ammo, sizeof(s.block.ammo));
        if (info.health > 0) {
            s.block.health = (s16)MIN(info.health, (int)gSaveContext.healthCapacity);
        } else {
            s.block.health = (s16)MIN(0x30, (int)gSaveContext.healthCapacity);
        }
    }
    if (info.valid && info.age >= 0 && info.age != gSaveContext.linkAge) {
        // Phase 5b: it comes from a group that had not changed age yet (it was on its way when the room did):
        // its buttons and equipment are of the other age.
        s32 now = gSaveContext.linkAge;
        gSaveContext.linkAge = info.age;
        SwapSlotAge(s);
        gSaveContext.linkAge = now;
        s.otherValid = 0;
    }
    FixBottleButtons(s.block);
    ActorEntry* entry = play->linkActorEntry;
    Vec3f base = { (f32)entry->pos.x, (f32)entry->pos.y, (f32)entry->pos.z };
    s16 yaw = entry->rot.y;
    int entranceRoom = play->setupEntranceList[play->curSpawn].room;
    int n = slot + 1;
    const char* where = "the group's entrance";
    if (info.valid && info.place && RoomLoaded(play, info.room)) {
        // Where it was (a reconnection, a group that moved into this scene).
        base = info.pos;
        yaw = info.yaw;
        entranceRoom = info.room;
        n = 0;
        where = "its own place";
    } else if (info.valid && info.entrance >= 0) {
        // The door it came through (phase 5): that entrance's spawn point in this scene.
        int e = info.entrance + gSaveContext.sceneLayer;
        if (e >= 0 && e < ENTR_MAX && gEntranceTable[e].scene == play->sceneNum && play->linkActorEntry != nullptr) {
            int spawnIdx = gEntranceTable[e].spawn;
            ActorEntry* list = play->linkActorEntry - play->setupEntranceList[play->curSpawn].spawn;
            ActorEntry* sp = &list[play->setupEntranceList[spawnIdx].spawn];
            base = { (f32)sp->pos.x, (f32)sp->pos.y, (f32)sp->pos.z };
            yaw = sp->rot.y;
            entranceRoom = play->setupEntranceList[spawnIdx].room;
            n = 0;
            where = "its entrance";
        }
    }
    // Standing (start mode "idle"), no start camera. When that room is no longer loaded (the group moved on), next to
    // the anchor instead.
    bool entranceLoaded = RoomLoaded(play, entranceRoom);
    int spawnRoom = entranceRoom;
    if (!entranceLoaded && Present(gZmpSim.anchor)) {
        Player* a = Slot(gZmpSim.anchor).player;
        base = a->actor.world.pos;
        yaw = a->actor.shape.rot.y;
        spawnRoom = Slot(gZmpSim.anchor).room;
        n = slot + 1;
        where = "next to the anchor";
    }
    if (infoText.rfind("zmp_spawn_near ", 0) == 0) {
        // Phase 5: a player without a game of its own who asked to join this one appears next to it.
        int k = atoi(infoText.c_str() + 15);
        if (Present(k)) {
            Player* f = Slot(k).player;
            base = f->actor.world.pos;
            yaw = f->actor.shape.rot.y;
            spawnRoom = Slot(k).room;
            n = 1;
            where = "next to the player it follows";
        }
    }
    Vec3f pos = SafeSpawnPos(play, base, yaw, n);
    s16 params = (s16)((PLAYER_START_MODE_IDLE << 8) | 0xFF);
    // Same background camera data as the anchor (fixed cameras of rooms and houses).
    MsgResetSlot(play, slot);
    Player* p = SpawnPlayerActor(play, slot, pos, yaw, params, play->mainCamera.camDataIdx);
    s.room = (s16)spawnRoom;
    Log("zmp: SPAWN slot " + std::to_string(slot) + (p != nullptr ? " ok" : " FAILED") + " at " + where +
        (info.valid ? " (block from its previous group)" : ""));
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
    MsgResetSlot(play, slot);
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

int SlotRoom(int slot) {
    return Valid(slot) ? Slot(slot).room : -1;
}

void SetSlotRoom(int slot, int room) {
    if (Valid(slot)) {
        Slot(slot).room = (s16)room;
    }
}

int SlotHeatSeconds(int slot) {
    if (!Present(slot) || Slot(slot).heatState == 0) {
        return -1;
    }
    return Slot(slot).heatSeconds;
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

const MessageContext* SlotMessage(int slot) {
    return Valid(slot) ? SlotMsg(slot) : nullptr;
}

int Anchor() {
    return gZmpSim.anchor;
}

int SlotMsgMode(int slot) {
    const MessageContext* m = SlotMsg(slot);
    return m != nullptr ? m->msgMode : MSGMODE_NONE;
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
