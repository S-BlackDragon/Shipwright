/*
 * File: z_mir_ray.c
 * Overlay: ovl_Mir_Ray
 * Description: Reflectable Light Beam and reflections
 */

#include "z_mir_ray.h"
#include "objects/object_mir_ray/object_mir_ray.h"
#include "soh/frame_interpolation.h"
#include "soh/Enhancements/game-interactor/GameInteractor_Hooks.h"
#include "soh/Zmp/Sim/ZmpPlayers.h" // ZMP (R2, D-212)
#include "soh/Zmp/Test/Mutants.h" // ZMP (Zmp_TestMutant is inline outside the test build)

#define FLAGS (ACTOR_FLAG_UPDATE_CULLING_DISABLED | ACTOR_FLAG_DRAW_CULLING_DISABLED)

void MirRay_Init(Actor* thisx, PlayState* play);
void MirRay_Destroy(Actor* thisx, PlayState* play);
void MirRay_Update(Actor* thisx, PlayState* play);
void MirRay_Draw(Actor* thisx, PlayState* play);

s32 MirRay_CheckInFrustum(Vec3f* vecA, Vec3f* vecB, f32 pointx, f32 pointy, f32 pointz, s16 radiusA, s16 radiusB);
static void MirRay_ZmpReflect(MirRay* this, PlayState* play); // ZMP (R2, D-212)

// Locations of light beams in sMirRayData
typedef enum {
    /* 0 */ MIRRAY_SPIRIT_BOMBCHUIWAROOM_DOWNLIGHT,
    /* 1 */ MIRRAY_SPIRIT_SUNBLOCKROOM_DOWNLIGHT,
    /* 2 */ MIRRAY_SPIRIT_SINGLECOBRAROOM_DOWNLIGHT,
    /* 3 */ MIRRAY_SPIRIT_ARMOSROOM_DOWNLIGHT,
    /* 4 */ MIRRAY_SPIRIT_TOPROOM_DOWNLIGHT,
    /* 5 */ MIRRAY_SPIRIT_TOPROOM_CEILINGMIRROR,
    /* 6 */ MIRRAY_SPIRIT_SINGLECOBRAROOM_COBRA,
    /* 7 */ MIRRAY_SPIRIT_TOPROOM_COBRA1,
    /* 8 */ MIRRAY_SPIRIT_TOPROOM_COBRA2,
    /* 9 */ MIRRAY_GANONSCASTLE_SPIRITTRIAL_DOWNLIGHT
} MirRayBeamLocations;

const ActorInit Mir_Ray_InitVars = {
    ACTOR_MIR_RAY,
    ACTORCAT_ITEMACTION,
    FLAGS,
    OBJECT_MIR_RAY,
    sizeof(MirRay),
    (ActorFunc)MirRay_Init,
    (ActorFunc)MirRay_Destroy,
    (ActorFunc)MirRay_Update,
    (ActorFunc)MirRay_Draw,
    NULL,
};

static u8 D_80B8E670 = 0;

static ColliderQuadInit sQuadInit = {
    {
        COLTYPE_NONE,
        AT_ON | AT_TYPE_PLAYER,
        AC_NONE,
        OC1_NONE,
        OC2_NONE,
        COLSHAPE_QUAD,
    },
    {
        ELEMTYPE_UNK0,
        { 0x00200000, 0x00, 0x00 },
        { 0xFFCFFFFF, 0x00, 0x00 },
        TOUCH_ON | TOUCH_SFX_NORMAL,
        BUMP_NONE,
        OCELEM_NONE,
    },
    { { { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f } } },
};

static ColliderJntSphElementInit sJntSphElementsInit[1] = {
    {
        {
            ELEMTYPE_UNK0,
            { 0x00200000, 0x00, 0x00 },
            { 0x00000000, 0x00, 0x00 },
            TOUCH_ON | TOUCH_SFX_NORMAL,
            BUMP_NONE,
            OCELEM_NONE,
        },
        { 0, { { 0, 0, 0 }, 50 }, 100 },
    },
};

static ColliderJntSphInit sJntSphInit = {
    {
        COLTYPE_NONE,
        AT_ON | AT_TYPE_PLAYER,
        AC_NONE,
        OC1_NONE,
        OC2_NONE,
        COLSHAPE_JNTSPH,
    },
    1,
    sJntSphElementsInit,
};

static MirRayDataEntry sMirRayData[] = {
    { { -1160, 686, -880 }, { -920, 480, -889 }, 30, 50, 1.0f, 50, 150, 0.8f, 255, 255, 255, 0x02 },
    { { -1856, 1092, -190 }, { -1703, 841, -186 }, 30, 70, 0.88f, 54, 150, 0.8f, 255, 255, 255, 0x02 },
    { { 1367, 738, -860 }, { 1091, 476, -860 }, 30, 85, 0.0f, 0, 150, 0.8f, 255, 255, 255, 0x00 },
    { { 2200, 1103, -220 }, { 2040, 843, -220 }, 30, 60, 0.0f, 0, 150, 0.8f, 255, 255, 255, 0x01 },
    { { -560, 2169, -310 }, { -560, 1743, -310 }, 30, 70, 0.0f, 0, 150, 0.8f, 255, 255, 255, 0x00 },
    { { 60, 1802, -1090 }, { 60, 973, -1090 }, 30, 70, 0.0f, 0, 150, 0.9f, 255, 255, 255, 0x0D },
    { { 1140, 480, -860 }, { 1140, 480, -860 }, 30, 30, 1.0f, 10, 100, 0.9f, 255, 255, 255, 0x0E },
    { { -560, 1743, -310 }, { -560, 1743, -310 }, 30, 30, 0.0f, 0, 100, 0.94f, 255, 255, 255, 0x0C },
    { { 60, 1743, -310 }, { 60, 1743, -310 }, 30, 30, 0.0f, 0, 100, 0.94f, 255, 255, 255, 0x0C },
    { { -1174, 448, 1194 }, { -1174, 148, 1194 }, 50, 100, 1.0f, 50, 150, 0.8f, 255, 255, 255, 0x03 }
};

static InitChainEntry sInitChain[] = {
    ICHAIN_VEC3F_DIV1000(scale, 0, ICHAIN_CONTINUE),
    ICHAIN_F32(uncullZoneForward, 4000, ICHAIN_CONTINUE),
    ICHAIN_F32(uncullZoneScale, 1000, ICHAIN_CONTINUE),
    ICHAIN_F32(uncullZoneDownward, 1000, ICHAIN_STOP),
};

void MirRay_SetupCollider(MirRay* this) {
    Vec3f colliderOffset;
    MirRayDataEntry* dataEntry = &sMirRayData[this->actor.params];

    colliderOffset.x = (this->poolPt.x - this->sourcePt.x) * dataEntry->unk_10;
    colliderOffset.y = (this->poolPt.y - this->sourcePt.y) * dataEntry->unk_10;
    colliderOffset.z = (this->poolPt.z - this->sourcePt.z) * dataEntry->unk_10;
    this->colliderSph.elements[0].dim.worldSphere.center.x = colliderOffset.x + this->sourcePt.x;
    this->colliderSph.elements[0].dim.worldSphere.center.y = colliderOffset.y + this->sourcePt.y;
    this->colliderSph.elements[0].dim.worldSphere.center.z = colliderOffset.z + this->sourcePt.z;
    this->colliderSph.elements[0].dim.worldSphere.radius = dataEntry->unk_14 * this->colliderSph.elements->dim.scale;
}

// Set up a light point between source point and reflection point. Reflection point is the pool point (for windows) or
// at the player position (for mirrors)
void MirRay_MakeShieldLight(MirRay* this, PlayState* play) {
    Player* player = GET_PLAYER(play);
    MirRayDataEntry* dataEntry = &sMirRayData[this->actor.params];
    Vec3f reflectionPt;
    Vec3s lightPt;

    if (MirRay_CheckInFrustum(&this->sourcePt, &this->poolPt, player->actor.world.pos.x,
                              player->actor.world.pos.y + 30.0f, player->actor.world.pos.z, this->sourceEndRad,
                              this->poolEndRad)) {

        if (dataEntry->params & 8) { // Light beams from mirrors
            Math_Vec3f_Diff(&player->actor.world.pos, &this->sourcePt, &reflectionPt);
        } else { // Light beams from windows
            Math_Vec3f_Diff(&this->poolPt, &this->sourcePt, &reflectionPt);
        }

        lightPt.x = (dataEntry->unk_18 * reflectionPt.x) + this->sourcePt.x;
        lightPt.y = (dataEntry->unk_18 * reflectionPt.y) + this->sourcePt.y;
        lightPt.z = (dataEntry->unk_18 * reflectionPt.z) + this->sourcePt.z;

        // Fade up
        Math_StepToS(&this->lightPointRad, dataEntry->lgtPtMaxRad, 6);
        Lights_PointNoGlowSetInfo(&this->lightInfo, lightPt.x, lightPt.y, lightPt.z, dataEntry->color.r,
                                  dataEntry->color.g, dataEntry->color.b, this->lightPointRad);
    } else {
        // Fade down
        Math_StepToS(&this->lightPointRad, 0, 6);
        Lights_PointSetColorAndRadius(&this->lightInfo, dataEntry->color.r, dataEntry->color.g, dataEntry->color.b,
                                      this->lightPointRad);
    }
}

void MirRay_Init(Actor* thisx, PlayState* play) {
    s32 pad;
    MirRay* this = (MirRay*)thisx;
    MirRayDataEntry* dataEntry = &sMirRayData[this->actor.params];

    Actor_ProcessInitChain(&this->actor, sInitChain);
    ActorShape_Init(&this->actor.shape, 0.0f, NULL, 0.0f);
    // "Generation of reflectable light!"
    osSyncPrintf("反射用 光の発生!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n");
    LOG_NUM("this->actor.arg_data", this->actor.params);

    if (this->actor.params >= 0xA) {
        // "Reflected light generation failure"
        LOG_STRING("反射光 発生失敗");
        Actor_Kill(&this->actor);
    }

    this->sourcePt.x = dataEntry->sourcePoint.x;
    this->sourcePt.y = dataEntry->sourcePoint.y;
    this->sourcePt.z = dataEntry->sourcePoint.z;
    this->sourceEndRad = dataEntry->sourceEndRadius;

    this->poolPt.x = dataEntry->poolPoint.x;
    this->poolPt.y = dataEntry->poolPoint.y;
    this->poolPt.z = dataEntry->poolPoint.z;
    this->poolEndRad = dataEntry->poolEndRadius;

    Lights_PointNoGlowSetInfo(&this->lightInfo, this->sourcePt.x, this->sourcePt.y, this->sourcePt.z, 255, 255, 255,
                              100);
    this->lightNode = LightContext_InsertLight(play, &play->lightCtx, &this->lightInfo);

    this->shieldCorners[0].x = -536.0f;
    this->shieldCorners[0].y = -939.0f;

    this->shieldCorners[1].x = -1690.0f;
    this->shieldCorners[1].y = 0.0f;

    this->shieldCorners[2].x = -536.0f;
    this->shieldCorners[2].y = 938.0f;

    this->shieldCorners[3].x = 921.0f;
    this->shieldCorners[3].y = 0.0f;

    this->shieldCorners[4].x = 758.0f;
    this->shieldCorners[4].y = 800.0f;

    this->shieldCorners[5].x = 758.0f;
    this->shieldCorners[5].y = -800.0f;

    if (dataEntry->params & 2) {
        Collider_InitJntSph(play, &this->colliderSph);
        Collider_SetJntSph(play, &this->colliderSph, &this->actor, &sJntSphInit, &this->colliderSphItem);
        if (!(dataEntry->params & 4)) { // Beams not from mirrors
            MirRay_SetupCollider(this);
        }
    }

    Collider_InitQuad(play, &this->shieldRay);
    Collider_SetQuad(play, &this->shieldRay, &this->actor, &sQuadInit);
    for (s32 k = 0; k < ZMP_MAX_PLAYERS; k++) { // ZMP (R2, D-212): one reflected beam per player
        this->zmpIntensity[k] = 0.0f;
        this->zmpRange[k] = 1.0f;
        Collider_InitQuad(play, &this->zmpShieldRay[k]);
        Collider_SetQuad(play, &this->zmpShieldRay[k], &this->actor, &sQuadInit);
    }

    // Spirit Temple top room mirrors
    if ((this->actor.params == 5) || (this->actor.params == 7) || (this->actor.params == 8)) {
        this->actor.room = -1;
    }
}

void MirRay_Destroy(Actor* thisx, PlayState* play) {
    MirRay* this = (MirRay*)thisx;

    LightContext_RemoveLight(play, &play->lightCtx, this->lightNode);

    if (sMirRayData[this->actor.params].params & 2) {
        Collider_DestroyJntSph(play, &this->colliderSph);
    }

    Collider_DestroyQuad(play, &this->shieldRay);
    for (s32 k = 0; k < ZMP_MAX_PLAYERS; k++) { // ZMP (R2, D-212)
        Collider_DestroyQuad(play, &this->zmpShieldRay[k]);
    }
}

void MirRay_Update(Actor* thisx, PlayState* play) {
    s32 pad;
    MirRay* this = (MirRay*)thisx;
    Player* player = GET_PLAYER(play);

    D_80B8E670 = 0;

    if (!this->unLit) {
        if (sMirRayData[this->actor.params].params & 2) {
            if (sMirRayData[this->actor.params].params & 4) { // Beams from mirrors
                MirRay_SetupCollider(this);
            }
            CollisionCheck_SetAT(play, &play->colChkCtx, &this->colliderSph.base);
        }
        if (Zmp_MultiActive()) {
            MirRay_ZmpReflect(this, play); // ZMP (R2, D-212): every player's shield, in the tick
        } else if (this->reflectIntensity > 0.0f) {
            CollisionCheck_SetAT(play, &play->colChkCtx, &this->shieldRay.base);
        }
        MirRay_MakeShieldLight(this, play);

        if (!Zmp_MultiActive() && (this->reflectIntensity > 0.0f)) {
            Actor_PlaySfx_Flagged2(&player->actor, NA_SE_IT_SHIELD_BEAM - SFX_FLAG);
        }
    } else if (Zmp_MultiActive()) {
        for (s32 k = 0; k < ZMP_MAX_PLAYERS; k++) { // ZMP (R2, D-212): a beam that is off reflects nowhere
            this->zmpIntensity[k] = 0.0f;
        }
    }
}

// ZMP (R2, D-212): for the given player, into the given intensity and range (the original: GET_PLAYER, this's fields).
static void MirRay_IntensityFor(MirRay* this, Player* player, f32* reflectIntensity, f32* reflectRange) {
    f32 sp4C[3];
    f32 temp_f0;
    f32 temp_f0_2;
    f32 temp_f2_2;
    s32 pad;
    MtxF* shieldMtx = &player->shieldMf;

    *reflectIntensity = 0.0f;

    if (MirRay_CheckInFrustum(&this->sourcePt, &this->poolPt, shieldMtx->xw, shieldMtx->yw, shieldMtx->zw,
                              this->sourceEndRad, this->poolEndRad)) {

        temp_f0 = sqrtf(SQ(shieldMtx->zz) + (SQ(shieldMtx->xz) + SQ(shieldMtx->yz)));

        if (temp_f0 == 0.0f) {
            *reflectRange = 1.0f;
        } else {
            *reflectRange = 1.0f / temp_f0;
        }

        // If light beam is adirectional, always reflect, else only reflect if shield is pointing in correct direction
        if (sMirRayData[this->actor.params].params & 1) {
            *reflectIntensity = 1.0f;
        } else {
            sp4C[0] = this->poolPt.x - this->sourcePt.x;
            sp4C[1] = this->poolPt.y - this->sourcePt.y;
            sp4C[2] = this->poolPt.z - this->sourcePt.z;

            temp_f2_2 = -shieldMtx->xz * sp4C[0] - shieldMtx->yz * sp4C[1] - shieldMtx->zz * sp4C[2];

            if (temp_f2_2 < 0.0f) {
                temp_f0_2 = sqrtf(SQ(sp4C[0]) + SQ(sp4C[1]) + SQ(sp4C[2]));
                if ((temp_f0 != 0.0f) && (temp_f0_2 != 0.0f)) {
                    *reflectIntensity = -temp_f2_2 / (temp_f0 * temp_f0_2);
                }
            }
        }
    }
}

void MirRay_SetIntensity(MirRay* this, PlayState* play) {
    MirRay_IntensityFor(this, GET_PLAYER(play), &this->reflectIntensity, &this->reflectRange);
}

// Draws six images, one for each corner of the shield, by finding the intersection of a line segment from the corner
// perpendicular to the shield with the nearest collision (if any).
void MirRay_SetupReflectionPolys(MirRay* this, PlayState* play, MirRayShieldReflection* reflection, Player* player,
                                 f32 reflectIntensity, f32 reflectRange) { // ZMP (R2, D-212): whose shield, explicit
    MtxF* shieldMtx;
    s32 i;
    Vec3f posA;
    Vec3f posB;
    Vec3f posResult;
    CollisionPoly* outPoly;
    Vec3f sp60;

    shieldMtx = &player->shieldMf;

    sp60.x = -((*shieldMtx).xz * reflectRange) * reflectIntensity * 400.0f;
    sp60.y = -((*shieldMtx).yz * reflectRange) * reflectIntensity * 400.0f;
    sp60.z = -((*shieldMtx).zz * reflectRange) * reflectIntensity * 400.0f;

    sp60 = sp60; // Need something involving sp60 or the whole function changes

    for (i = 0; i < 6; i++) {
        posA.x = ((*shieldMtx).xw + (this->shieldCorners[i].x * (*shieldMtx).xx)) +
                 (this->shieldCorners[i].y * (*shieldMtx).xy);
        posA.y = ((*shieldMtx).yw + (this->shieldCorners[i].x * (*shieldMtx).yx)) +
                 (this->shieldCorners[i].y * (*shieldMtx).yy);
        posA.z = ((*shieldMtx).zw + (this->shieldCorners[i].x * (*shieldMtx).zx)) +
                 (this->shieldCorners[i].y * (*shieldMtx).zy);
        posB.x = sp60.x + posA.x;
        posB.y = sp60.y + posA.y;
        posB.z = sp60.z + posA.z;
        if (BgCheck_AnyLineTest1(&play->colCtx, &posA, &posB, &posResult, &outPoly, 1)) {
            reflection[i].reflectionPoly = outPoly;
        } else {
            reflection[i].reflectionPoly = NULL;
        }
    }
}

// Remove reflections that are in the same position and are sufficiently near to the same plane
void MirRay_RemoveSimilarReflections(MirRayShieldReflection* reflection) {
    s32 i;
    s32 j;

    for (i = 0; i < 6; i++) {
        for (j = i + 1; j < 6; j++) {
            if (reflection[i].reflectionPoly != NULL) {
                if ((reflection[j].reflectionPoly != NULL) &&
                    (ABS(reflection[i].reflectionPoly->normal.x - reflection[j].reflectionPoly->normal.x) < 100) &&
                    (ABS(reflection[i].reflectionPoly->normal.y - reflection[j].reflectionPoly->normal.y) < 100) &&
                    (ABS(reflection[i].reflectionPoly->normal.z - reflection[j].reflectionPoly->normal.z) < 100) &&
                    (reflection[i].reflectionPoly->dist == reflection[j].reflectionPoly->dist)) {
                    reflection[j].reflectionPoly = NULL;
                }
            }
        }
    }
}

// Creates the reflected beam's collider (to interact with objects) and places and orients the shield images
void MirRay_ReflectedBeam(MirRay* this, PlayState* play, MirRayShieldReflection* reflection, Player* player,
                          f32 reflectIntensity, f32 reflectRange, ColliderQuad* beam) { // ZMP (R2, D-212)
    s32 i;
    f32 temp_f0;
    Vec3f vecB;
    Vec3f vecD;
    Vec3f sp118;
    Vec3f sp10C;
    Vec3f sp100;
    Vec3f intersection;
    Vec3f spE8;
    Vec3f normalVec;
    MtxF* shieldMtx;
    Vec3f vecA;
    Vec3f vecC;
    MirRayShieldReflection* currentReflection;

    shieldMtx = &player->shieldMf;

    spE8.x = -(shieldMtx->xz * reflectRange) * reflectIntensity * 400.0f;
    spE8.y = -(shieldMtx->yz * reflectRange) * reflectIntensity * 400.0f;
    spE8.z = -(shieldMtx->zz * reflectRange) * reflectIntensity * 400.0f;

    vecB.x = shieldMtx->xw;
    vecB.y = shieldMtx->yw;
    vecB.z = shieldMtx->zw;

    vecD.x = spE8.x + vecB.x;
    vecD.y = spE8.y + vecB.y;
    vecD.z = spE8.z + vecB.z;

    vecA.x = vecB.x + (shieldMtx->xx * 300.0f);
    vecA.y = vecB.y + (shieldMtx->yx * 300.0f);
    vecA.z = vecB.z + (shieldMtx->zx * 300.0f);

    vecC.x = vecD.x + (shieldMtx->xx * 300.0f);
    vecC.y = vecD.y + (shieldMtx->yx * 300.0f);
    vecC.z = vecD.z + (shieldMtx->zx * 300.0f);

    if (beam != NULL) { // ZMP (R2, D-212): the collider is the tick's (NULL from the draw in a session)
        Collider_SetQuadVertices(beam, &vecA, &vecB, &vecC, &vecD);
    }
    if (reflection == NULL) {
        return;
    }

    for (i = 0; i < 6; i++) {
        currentReflection = &reflection[i];
        if (currentReflection->reflectionPoly != NULL) {
            normalVec.x = COLPOLY_GET_NORMAL(currentReflection->reflectionPoly->normal.x);
            normalVec.y = COLPOLY_GET_NORMAL(currentReflection->reflectionPoly->normal.y);
            normalVec.z = COLPOLY_GET_NORMAL(currentReflection->reflectionPoly->normal.z);

            if (Math3D_LineSegVsPlane(normalVec.x, normalVec.y, normalVec.z, currentReflection->reflectionPoly->dist,
                                      &vecB, &vecD, &sp118, 1)) {

                currentReflection->pos.x = sp118.x;
                currentReflection->pos.y = sp118.y;
                currentReflection->pos.z = sp118.z;

                temp_f0 = sqrtf(SQ(sp118.x - vecB.x) + SQ(sp118.y - vecB.y) + SQ(sp118.z - vecB.z));

                if (temp_f0 < (reflectIntensity * 600.0f)) {
                    currentReflection->opacity = 200;
                } else {
                    currentReflection->opacity = (s32)(800.0f - temp_f0);
                }

                spE8 = spE8; // Required to match

                sp10C.x = (shieldMtx->xx * 100.0f) + vecB.x;
                sp10C.y = (shieldMtx->yx * 100.0f) + vecB.y;
                sp10C.z = (shieldMtx->zx * 100.0f) + vecB.z;

                sp100.x = (spE8.x * 4.0f) + sp10C.x;
                sp100.y = (spE8.y * 4.0f) + sp10C.y;
                sp100.z = (spE8.z * 4.0f) + sp10C.z;

                normalVec = normalVec; // Required to match

                currentReflection->mtx.zw = 0.0f;

                currentReflection->mtx.xx = currentReflection->mtx.yy = currentReflection->mtx.zz =
                    currentReflection->mtx.ww = 1.0f;
                currentReflection->mtx.yx = currentReflection->mtx.zx = currentReflection->mtx.wx =
                    currentReflection->mtx.xy = currentReflection->mtx.zy = currentReflection->mtx.wy =
                        currentReflection->mtx.xz = currentReflection->mtx.yz = currentReflection->mtx.wz =
                            currentReflection->mtx.xw = currentReflection->mtx.yw = currentReflection->mtx.zw;

                if (Math3D_LineSegVsPlane(normalVec.x, normalVec.y, normalVec.z,
                                          currentReflection->reflectionPoly->dist, &sp10C, &sp100, &intersection, 1)) {
                    currentReflection->mtx.xx = intersection.x - sp118.x;
                    currentReflection->mtx.yx = intersection.y - sp118.y;
                    currentReflection->mtx.zx = intersection.z - sp118.z;
                }

                sp10C.x = (shieldMtx->xy * 100.0f) + vecB.x;
                sp10C.y = (shieldMtx->yy * 100.0f) + vecB.y;
                sp10C.z = (shieldMtx->zy * 100.0f) + vecB.z;

                sp100.x = (spE8.x * 4.0f) + sp10C.x;
                sp100.y = (spE8.y * 4.0f) + sp10C.y;
                sp100.z = (spE8.z * 4.0f) + sp10C.z;

                if (Math3D_LineSegVsPlane(normalVec.x, normalVec.y, normalVec.z,
                                          currentReflection->reflectionPoly->dist, &sp10C, &sp100, &intersection, 1)) {
                    currentReflection->mtx.xy = intersection.x - sp118.x;
                    currentReflection->mtx.yy = intersection.y - sp118.y;
                    currentReflection->mtx.zy = intersection.z - sp118.z;
                }
            } else {
                currentReflection->reflectionPoly = NULL;
            }
        }
    }
}

// ZMP (R2, D-212): a reflection drawn for one player's shield with that player's intensity and range. `beam` is the
// reflected beam's collider the original set while drawing; NULL in a session, where the tick sets it.
static void MirRay_DrawReflection(MirRay* this, PlayState* play, Player* player, f32 reflectIntensity, f32 reflectRange,
                                  ColliderQuad* beam, s32 id) {
    s32 i;
    MirRayShieldReflection reflection[6];
    s32 temp;

    Matrix_Mult(&player->shieldMf, MTXMODE_NEW);
    OPEN_DISPS(play->state.gfxCtx);

    Gfx_SetupDL_25Xlu(play->state.gfxCtx);
    Matrix_Scale(1.0f, 1.0f, reflectIntensity * 5.0f, MTXMODE_APPLY);
    gSPMatrix(POLY_XLU_DISP++, MATRIX_NEWMTX(play->state.gfxCtx), G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
    gDPSetPrimColor(POLY_XLU_DISP++, 0, 0, 255, 255, 150, (s16)(temp = reflectIntensity * 100.0f));
    gSPDisplayList(POLY_XLU_DISP++, gShieldBeamGlowDL);
    MirRay_SetupReflectionPolys(this, play, reflection, player, reflectIntensity, reflectRange);
    MirRay_RemoveSimilarReflections(reflection);
    MirRay_ReflectedBeam(this, play, reflection, player, reflectIntensity, reflectRange, beam);

    if (reflection[0].reflectionPoly == NULL) {
        reflection[0].opacity = 0;
    }
    for (i = 1; i < 6; i++) {
        if (reflection[i].reflectionPoly != NULL) {
            if (reflection[0].opacity < reflection[i].opacity) {
                reflection[0].opacity = reflection[i].opacity;
            }
        }
    }
    for (i = 0; i < 6; i++) {
        if (GameInteractor_Should(VB_MIRRAY_DRAW_REFLECTION, reflection[i].reflectionPoly != NULL, &reflection[i])) {
            FrameInterpolation_RecordOpenChild(&reflection[i], id * 6 + i);
            Matrix_Translate(reflection[i].pos.x, reflection[i].pos.y, reflection[i].pos.z, MTXMODE_NEW);
            Matrix_Scale(0.01f, 0.01f, 0.01f, MTXMODE_APPLY);
            Matrix_Mult(&reflection[i].mtx, MTXMODE_APPLY);
            gSPMatrix(POLY_XLU_DISP++, MATRIX_NEWMTX(play->state.gfxCtx), G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
            gDPSetRenderMode(POLY_XLU_DISP++, G_RM_FOG_SHADE_A, G_RM_AA_ZB_XLU_DECAL2);
            gDPSetPrimColor(POLY_XLU_DISP++, 0, 0, 255, 255, 150, reflection[0].opacity);
            gSPDisplayList(POLY_XLU_DISP++, gShieldBeamImageDL);
            FrameInterpolation_RecordCloseChild();
        }
    }

    CLOSE_DISPS(play->state.gfxCtx);
}

// ZMP (R2, D-212): whether an earlier Mir_Ray (in the update order) already reflects off this player's shield this
// tick. The original reflected one beam per frame (D_80B8E670, set by the first one drawn): the same, per player.
static s32 MirRay_ZmpTakenBefore(MirRay* this, PlayState* play, s32 k) {
    Actor* a;

    for (a = play->actorCtx.actorLists[this->actor.category].head; (a != NULL) && (a != &this->actor); a = a->next) {
        if ((a->id == ACTOR_MIR_RAY) && (a->update != NULL) && (((MirRay*)a)->zmpIntensity[k] > 0.0f)) {
            return true;
        }
    }
    return false;
}

// ZMP (R2, D-212): the reflection is simulation (its beam lights the suns and the statue): computed in the tick, for
// every Link holding up a Mirror Shield, from that Link's shield (`shieldMf`, which every machine computes alike when
// it draws the players: the tick reads last tick's, as the original's draw did). Before, it was computed while
// drawing, with the shield of the actor's context player (the Link nearest to the Mir_Ray actor): with a partner
// nearer to the actor, the shield in the beam reflected nothing. (mutant "reflejo_del_ancla": only the context
// player's shield, as before)
static void MirRay_ZmpReflect(MirRay* this, PlayState* play) {
    s32 k;
    Player* ctx = GET_PLAYER(play);

    for (k = 0; k < ZMP_MAX_PLAYERS; k++) {
        Player* p = Zmp_SlotPlayer(k);

        this->zmpIntensity[k] = 0.0f;
        if ((p == NULL) || Zmp_IsDowned(p) || (p->rightHandType != PLAYER_MODELTYPE_RH_SHIELD) ||
            (p->currentShield != PLAYER_SHIELD_MIRROR)) {
            continue;
        }
        if (Zmp_TestMutant("reflejo_del_ancla") && (p != ctx)) {
            continue;
        }
        if (MirRay_ZmpTakenBefore(this, play, k)) {
            continue;
        }
        MirRay_IntensityFor(this, p, &this->zmpIntensity[k], &this->zmpRange[k]);
        if (this->zmpIntensity[k] > 0.0f) {
            MirRay_ReflectedBeam(this, play, NULL, p, this->zmpIntensity[k], this->zmpRange[k], &this->zmpShieldRay[k]);
            CollisionCheck_SetAT(play, &play->colChkCtx, &this->zmpShieldRay[k].base);
            Actor_PlaySfx_Flagged2(&p->actor, NA_SE_IT_SHIELD_BEAM - SFX_FLAG);
        }
    }
}

void MirRay_Draw(Actor* thisx, PlayState* play) {
    MirRay* this = (MirRay*)thisx;
    Player* player = GET_PLAYER(play);

    if (Zmp_MultiActive()) {
        // ZMP (R2, D-212): the tick's reflections, one per player whose shield reflects this beam; the draw writes
        // nothing the tick reads (D-192).
        if (!this->unLit) {
            for (s32 k = 0; k < ZMP_MAX_PLAYERS; k++) {
                Player* p = Zmp_SlotPlayer(k);
                if ((p != NULL) && (this->zmpIntensity[k] > 0.0f)) {
                    MirRay_DrawReflection(this, play, p, this->zmpIntensity[k], this->zmpRange[k], NULL, k);
                }
            }
        }
        return;
    }
    this->reflectIntensity = 0.0f;
    if ((D_80B8E670 == 0) && !this->unLit && Player_HasMirrorShieldSetToDraw(play)) {
        Matrix_Mult(&player->shieldMf, MTXMODE_NEW);
        MirRay_SetIntensity(this, play);
        if (!(this->reflectIntensity <= 0.0f)) {
            MirRay_DrawReflection(this, play, player, this->reflectIntensity, this->reflectRange, &this->shieldRay, 0);
            D_80B8E670 = 1;
        }
    }
}

// Computes if the Point (pointx, pointy, pointz) lies within the right conical frustum with one end centred at vecA
// with radius radiusA, the other at vecB with radius radiusB
s32 MirRay_CheckInFrustum(Vec3f* vecA, Vec3f* vecB, f32 pointx, f32 pointy, f32 pointz, s16 radiusA, s16 radiusB) {
    f32 coneRadius;
    f32 closestPtx;
    f32 closestPty;
    f32 closestPtz;
    Vec3f vecdiff;
    f32 dist;
    Vec3f sp5C;
    Vec3f sp50;
    Vec3f sp44;

    vecdiff.x = vecB->x - vecA->x;
    vecdiff.y = vecB->y - vecA->y;
    vecdiff.z = vecB->z - vecA->z;
    dist = SQ(vecdiff.x) + SQ(vecdiff.y) + SQ(vecdiff.z);

    if (dist == 0.0f) {
        return 0;
    }

    dist =
        (((pointx - vecA->x) * vecdiff.x) + ((pointy - vecA->y) * vecdiff.y) + ((pointz - vecA->z) * vecdiff.z)) / dist;

    // Closest point on line A-B to Point
    closestPtx = (vecdiff.x * dist) + vecA->x;
    closestPty = (vecdiff.y * dist) + vecA->y;
    closestPtz = (vecdiff.z * dist) + vecA->z;

    // Diameter of the double cone on the perpendicular plane through the closest point
    coneRadius = ((radiusB - radiusA) * dist) + radiusA;

    // If the Point is within the bounding double cone, check if it is in the frustum by checking whether it is between
    // the bounding planes
    if ((SQ(closestPtx - pointx) + SQ(closestPty - pointy) + SQ(closestPtz - pointz)) <= SQ(coneRadius)) {

        // Stores the vector difference again
        Math_Vec3f_Diff(vecB, vecA, &sp5C);

        sp50.x = pointx - vecA->x;
        sp50.y = pointy - vecA->y;
        sp50.z = pointz - vecA->z;

        if (Math3D_Cos(&sp5C, &sp50) < 0.0f) {
            return 0;
        }

        sp44.x = pointx - vecB->x;
        sp44.y = pointy - vecB->y;
        sp44.z = pointz - vecB->z;

        if (Math3D_Cos(&sp5C, &sp44) > 0.0f) {
            return 0;
        }
        return 1;
    }
    return 0;
}
