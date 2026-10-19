#include "z_bg_mori_elevator.h"
#include "objects/object_mori_objects/object_mori_objects.h"
#include "soh/Enhancements/savestate_serialize.h" // ZMP (D-198)
#include "soh/Zmp/Sim/ZmpPlayers.h"               // ZMP
#include "soh/Zmp/Test/Mutants.h"                 // ZMP

#define FLAGS ACTOR_FLAG_UPDATE_CULLING_DISABLED

void BgMoriElevator_Init(Actor* thisx, PlayState* play);
void BgMoriElevator_Destroy(Actor* thisx, PlayState* play);
void BgMoriElevator_Update(Actor* thisx, PlayState* play);

void BgMoriElevator_SetupWaitAfterInit(BgMoriElevator* this);
void BgMoriElevator_WaitAfterInit(BgMoriElevator* this, PlayState* play);
void BgMoriElevator_SetupSetPosition(BgMoriElevator* this);
void BgMoriElevator_SetPosition(BgMoriElevator* this, PlayState* play);
void BgMoriElevator_Draw(Actor* thisx, PlayState* play);
void BgMoriElevator_StopMovement(BgMoriElevator* this);
void func_808A2008(BgMoriElevator* this, PlayState* play);
void BgMoriElevator_MoveIntoGround(BgMoriElevator* this, PlayState* play);
void BgMoriElevator_MoveAboveGround(BgMoriElevator* this, PlayState* play);

static s16 sKankyoIsSpawned = false;
// ZMP (F10, D-198): whether the elevator exists (it is of no room and stays): who entered the group had its own, and
// would spawn a second elevator when room 2 or 17 loads again.
#define BG_MORI_ELEVATOR_ZMP_NEW_FIELDS(F) F(sKankyoIsSpawned)
ZMP_SAVESTATE_DEFINE(BgMoriElevator, ZMP_SAVESTATE_NONE, BG_MORI_ELEVATOR_ZMP_NEW_FIELDS, ZMP_SAVESTATE_NONE)

// ZMP (F10, D-198): the elevator decided with curRoom, the room of the player of its context (the nearest Link). Its
// drop into room 2's floor when the switch is set (the four Poes) fired only in the tick the switch changed and only if
// that Link stood in room 2: with the nearest Link in another room, or the elevator moving, the drop was lost for good
// (the elevator is of no room and never loads again) and the way to the basement stayed closed. Now: the drop waits
// until somebody stands in room 2 and the elevator is still, and a ride goes where the Link on it is (its room). The
// calls (come up, go down) stay with the nearest Link. (Mutant "sala_del_contexto": as before.)
static s32 BgMoriElevator_ZmpOff(void) {
    return !Zmp_MultiActive() || Zmp_TestMutant("sala_del_contexto");
}

// The room of the Link that stands on the elevator (the first in slot order), -1 if nobody does.
static s32 BgMoriElevator_ZmpRiderRoom(BgMoriElevator* this) {
    s32 k = -1;

    while ((k = Zmp_NextPresentSlot(k)) >= 0) {
        Player* link = Zmp_SlotPlayer(k);

        if ((link != NULL) && (link->actor.floorBgId == this->dyna.bgId) &&
            ((link->actor.world.pos.y - this->dyna.actor.world.pos.y) < 80.0f)) {
            return Zmp_SlotRoomOf(k);
        }
    }
    return -1;
}

const ActorInit Bg_Mori_Elevator_InitVars = {
    ACTOR_BG_MORI_ELEVATOR,
    ACTORCAT_BG,
    FLAGS,
    OBJECT_MORI_OBJECTS,
    sizeof(BgMoriElevator),
    (ActorFunc)BgMoriElevator_Init,
    (ActorFunc)BgMoriElevator_Destroy,
    (ActorFunc)BgMoriElevator_Update,
    NULL,
    NULL,
};

static InitChainEntry sInitChain[] = {
    ICHAIN_F32(uncullZoneForward, 2000, ICHAIN_CONTINUE),
    ICHAIN_F32(uncullZoneScale, 500, ICHAIN_CONTINUE),
    ICHAIN_F32(uncullZoneDownward, 3000, ICHAIN_CONTINUE),
    ICHAIN_VEC3F_DIV1000(scale, 1000, ICHAIN_STOP),
};

f32 func_808A1800(f32* pValue, f32 target, f32 scale, f32 maxStep, f32 minStep) {
    f32 var = (target - *pValue) * scale;

    if (*pValue < target) {
        if (maxStep < var) {
            var = maxStep;
        } else {
            if (var < minStep) {
                var = minStep;
            }
        }
        *pValue = (*pValue + var);

        if (target < *pValue) {
            *pValue = target;
        }
    } else {
        if (target < *pValue) {
            if (var < (-maxStep)) {
                var = (-maxStep);
            } else {
                if ((-minStep) < var) {
                    var = (-minStep);
                }
            }
            *pValue = (*pValue + var);
            if (*pValue < target) {
                *pValue = target;
            }
        } else {
            var = 0.0f;
        }
    }
    return var;
}

void func_808A18FC(BgMoriElevator* this, f32 distTo) {
    f32 temp;

    temp = fabsf(distTo) * 0.09f;
    func_800F436C(&this->dyna.actor.projectedPos, NA_SE_EV_ELEVATOR_MOVE2 - SFX_FLAG, CLAMP(temp, 0.0f, 1.0f));
}

void BgMoriElevator_Init(Actor* thisx, PlayState* play) {
    BgMoriElevator* this = (BgMoriElevator*)thisx;
    s32 pad;
    CollisionHeader* colHeader = NULL;

    this->unk_172 = sKankyoIsSpawned;
    this->moriTexObjIndex = Object_GetIndex(&play->objectCtx, OBJECT_MORI_TEX);
    if (this->moriTexObjIndex < 0) {
        Actor_Kill(thisx);
        // "Forest Temple obj elevator Bank Danger!"
        osSyncPrintf("Error : 森の神殿 obj elevator バンク危険！(%s %d)\n", __FILE__, __LINE__);
    } else {
        switch (sKankyoIsSpawned) {
            case false:
                // "Forest Temple elevator CT"
                osSyncPrintf("森の神殿 elevator CT\n");
                sKankyoIsSpawned = true;
                this->dyna.actor.room = -1;
                Actor_ProcessInitChain(&this->dyna.actor, sInitChain);
                DynaPolyActor_Init(&this->dyna, DPM_PLAYER);
                CollisionHeader_GetVirtual(&gMoriElevatorCol, &colHeader);
                this->dyna.bgId = DynaPoly_SetBgActor(play, &play->colCtx.dyna, thisx, colHeader);
                BgMoriElevator_SetupWaitAfterInit(this);
                break;
            case true:
                Actor_Kill(thisx);
                break;
        }
    }
}

void BgMoriElevator_Destroy(Actor* thisx, PlayState* play) {
    BgMoriElevator* this = (BgMoriElevator*)thisx;

    if (this->unk_172 == 0) {
        // "Forest Temple elevator DT"
        osSyncPrintf("森の神殿 elevator DT\n");
        DynaPoly_DeleteBgActor(play, &play->colCtx.dyna, this->dyna.bgId);
        sKankyoIsSpawned = false;
    }
}

s32 BgMoriElevator_IsPlayerRiding(BgMoriElevator* this, PlayState* play) {
    if (!BgMoriElevator_ZmpOff()) { // ZMP (D-198): a Link on it, whoever is the nearest
        return (this->dyna.interactFlags & DYNA_INTERACT_PLAYER_ON_TOP) &&
               !(this->unk_170 & DYNA_INTERACT_PLAYER_ON_TOP) && (BgMoriElevator_ZmpRiderRoom(this) >= 0);
    }
    return ((this->dyna.interactFlags & DYNA_INTERACT_PLAYER_ON_TOP) &&
            !(this->unk_170 & DYNA_INTERACT_PLAYER_ON_TOP) &&
            ((GET_PLAYER(play)->actor.world.pos.y - this->dyna.actor.world.pos.y) < 80.0f));
}

void BgMoriElevator_SetupWaitAfterInit(BgMoriElevator* this) {
    this->actionFunc = BgMoriElevator_WaitAfterInit;
}

void BgMoriElevator_WaitAfterInit(BgMoriElevator* this, PlayState* play) {
    if (Object_IsLoaded(&play->objectCtx, this->moriTexObjIndex)) {
        if (Flags_GetSwitch(play, this->dyna.actor.params & 0x3F)) {
            if (Zmp_AnyPlayerInRoom(play, 2)) { // ZMP (D-198): somebody in room 2, not the nearest
                this->dyna.actor.world.pos.y = 73.0f;
                BgMoriElevator_SetupSetPosition(this);
            } else {
                // "Error: Forest Temple obj elevator Room setting is dangerous"
                osSyncPrintf("Error : 森の神殿 obj elevator 部屋設定が危険(%s %d)\n", __FILE__, __LINE__);
            }
        } else {
            BgMoriElevator_SetupSetPosition(this);
        }
        this->dyna.actor.draw = BgMoriElevator_Draw;
    }
}

void func_808A1C30(BgMoriElevator* this) {
    this->actionFunc = BgMoriElevator_MoveIntoGround;
}

void BgMoriElevator_MoveIntoGround(BgMoriElevator* this, PlayState* play) {
    f32 distToTarget;

    func_808A1800(&this->dyna.actor.velocity.y, 2.0f, 0.05f, 1.0f, 0.0f);
    distToTarget = func_808A1800(&this->dyna.actor.world.pos.y, 73.0f, 0.08f, this->dyna.actor.velocity.y, 1.5f);
    if (fabsf(distToTarget) < 0.001f) {
        BgMoriElevator_SetupSetPosition(this);
        Audio_PlayActorSound2(&this->dyna.actor, NA_SE_EV_ELEVATOR_STOP);
    } else {
        func_808A18FC(this, distToTarget);
    }
}

void func_808A1CF4(BgMoriElevator* this, PlayState* play) {
    this->actionFunc = BgMoriElevator_MoveAboveGround;
    OnePointCutscene_Init(play, 3230, 70, &this->dyna.actor, CAM_ID_MAIN);
    OnePointCutscene_Init(play, 1020, 15, &this->dyna.actor, CAM_ID_MAIN);
}

void BgMoriElevator_MoveAboveGround(BgMoriElevator* this, PlayState* play) {
    f32 distToTarget;

    func_808A1800(&this->dyna.actor.velocity.y, 2.0f, 0.05f, 1.0f, 0.0f);
    distToTarget = func_808A1800(&this->dyna.actor.world.pos.y, 233.0f, 0.08f, this->dyna.actor.velocity.y, 1.5f);
    if (fabsf(distToTarget) < 0.001f) {
        BgMoriElevator_SetupSetPosition(this);
        Audio_PlayActorSound2(&this->dyna.actor, NA_SE_EV_ELEVATOR_STOP);
    } else {
        func_808A18FC(this, distToTarget);
    }
}

void BgMoriElevator_SetupSetPosition(BgMoriElevator* this) {
    this->actionFunc = BgMoriElevator_SetPosition;
}

void BgMoriElevator_SetPosition(BgMoriElevator* this, PlayState* play) {
    s32 pad;

    s32 riderRoom = BgMoriElevator_ZmpOff() ? play->roomCtx.curRoom.num // ZMP (D-198): the room of the Link on it
                                            : BgMoriElevator_ZmpRiderRoom(this);
    s32 inRoom2 = Zmp_AnyPlayerInRoom(play, 2); // ZMP (D-198)

    if (BgMoriElevator_IsPlayerRiding(this, play)) {
        if (riderRoom == 2) {
            this->targetY = -779.0f;
            BgMoriElevator_StopMovement(this);
        } else if (riderRoom == 17) {
            this->targetY = 233.0f;
            BgMoriElevator_StopMovement(this);
        } else {
            // "Error:Forest Temple obj elevator Room setting is dangerous(%s %d)"
            osSyncPrintf("Error : 森の神殿 obj elevator 部屋設定が危険(%s %d)\n", __FILE__, __LINE__);
        }
    } else if ((play->roomCtx.curRoom.num == 2) && (this->dyna.actor.world.pos.y < -275.0f)) {
        this->targetY = 233.0f;
        BgMoriElevator_StopMovement(this);
    } else if ((play->roomCtx.curRoom.num == 17) && (-275.0f < this->dyna.actor.world.pos.y)) {
        this->targetY = -779.0f;
        BgMoriElevator_StopMovement(this);
    } else if (inRoom2 && Flags_GetSwitch(play, this->dyna.actor.params & 0x3F) && (this->unk_16C == 0)) {
        this->targetY = 73.0f;
        func_808A1C30(this);
    } else if (inRoom2 && !Flags_GetSwitch(play, this->dyna.actor.params & 0x3F) && (this->unk_16C != 0)) {
        this->targetY = 233.0f;
        func_808A1CF4(this, play);
    }
}

void BgMoriElevator_StopMovement(BgMoriElevator* this) {
    this->actionFunc = func_808A2008;
    this->dyna.actor.velocity.y = 0.0f;
}

void func_808A2008(BgMoriElevator* this, PlayState* play) {
    f32 distTo;

    func_808A1800(&this->dyna.actor.velocity.y, 12.0f, 0.1f, 1.0f, 0.0f);
    distTo = func_808A1800(&this->dyna.actor.world.pos.y, this->targetY, 0.1f, this->dyna.actor.velocity.y, 0.3f);
    if (fabsf(distTo) < 0.001f) {
        BgMoriElevator_SetupSetPosition(this);
        Audio_PlayActorSound2(&this->dyna.actor, NA_SE_EV_ELEVATOR_STOP);

    } else {
        func_808A18FC(this, distTo);
    }
}

void BgMoriElevator_Update(Actor* thisx, PlayState* play) {
    BgMoriElevator* this = (BgMoriElevator*)thisx;

    this->actionFunc(this, play);
    this->unk_170 = this->dyna.interactFlags;
    // ZMP (D-198): the switch's change is taken as seen only when the elevator acted on it (sank into room 2's floor or
    // came back up); while nobody is in room 2, or a ride or a call moves it, the change waits.
    if (BgMoriElevator_ZmpOff() || (this->actionFunc == BgMoriElevator_MoveIntoGround) ||
        (this->actionFunc == BgMoriElevator_MoveAboveGround)) {
        this->unk_16C = Flags_GetSwitch(play, (thisx->params & 0x3F));
    }
}

void BgMoriElevator_Draw(Actor* thisx, PlayState* play) {
    s32 pad;
    BgMoriElevator* this = (BgMoriElevator*)thisx;

    OPEN_DISPS(play->state.gfxCtx);

    Gfx_SetupDL_25Opa(play->state.gfxCtx);
    gSPSegment(POLY_OPA_DISP++, 0x08, play->objectCtx.status[this->moriTexObjIndex].segment);
    gSPMatrix(POLY_OPA_DISP++, MATRIX_NEWMTX(play->state.gfxCtx), G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
    gSPDisplayList(POLY_OPA_DISP++, gMoriElevatorDL);

    CLOSE_DISPS(play->state.gfxCtx);
}
