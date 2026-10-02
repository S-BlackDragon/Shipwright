#include "global.h"
#include "soh/Zmp/Sim/ZmpPlayers.h"               // ZMP
#include "soh/Enhancements/savestate_serialize.h" // ZMP
#include "vt.h"

#include <string.h>

QuakeRequest sQuakeRequest[4];
s16 D_80126250 = 1;
s16 sQuakeRequestCount = 0;

s16 (*sQuakeCallbacks[])(QuakeRequest*, ShakeInfo*) = {
    NULL, Quake_Callback1, Quake_Callback2, Quake_Callback3, Quake_Callback4, Quake_Callback5, Quake_Callback6,
};

Vec3f* Quake_AddVec(Vec3f* dst, Vec3f* arg1, VecSph* arg2) {
    Vec3f vec1;
    Vec3f vec2;

    OLib_VecSphGeoToVec3f(&vec2, arg2);
    vec1.x = arg1->x + vec2.x;
    vec1.y = arg1->y + vec2.y;
    vec1.z = arg1->z + vec2.z;
    *dst = vec1;
    return dst;
}

void Quake_UpdateShakeInfo(QuakeRequest* req, ShakeInfo* shake, f32 y, f32 x) {
    Vec3f* unk50 = &req->cam->at;
    Vec3f* unk5C = &req->cam->eye;

    Vec3f vec;
    VecSph struc2;
    VecSph struc1;
    Vec3f vec2;

    if (req->unk_1C) {
        vec.x = 0;
        vec.y = 0;
        vec.z = 0;
        OLib_Vec3fDiffToVecSphGeo(&struc1, unk5C, unk50);
        struc2.r = req->y * y;
        struc2.pitch = struc1.pitch + req->unk_14.unk_00 + 0x4000;
        struc2.yaw = struc1.yaw + req->unk_14.unk_02;
        Quake_AddVec(&vec, &vec, &struc2);
        struc2.r = req->x * x;
        struc2.pitch = struc1.pitch + req->unk_14.unk_00;
        struc2.yaw = struc1.yaw + req->unk_14.unk_02 + 0x4000;
        Quake_AddVec(&vec, &vec, &struc2);
    } else {
        vec.x = 0;
        vec.y = req->y * y;
        vec.z = 0;
        struc2.r = req->x * x;
        struc2.pitch = req->unk_14.unk_00;
        struc2.yaw = req->unk_14.unk_02;
        Quake_AddVec(&vec, &vec, &struc2);
    }

    vec2 = vec;
    shake->vec2 = vec2;
    shake->vec1 = vec2;
    shake->unk_1A = (f32)0x8000 * y;
    shake->rotZ = req->rotZ * y;
    shake->zoom = req->zoom * y;
}

s16 Quake_Callback1(QuakeRequest* req, ShakeInfo* shake) {
    s32 pad;

    if (req->countdown > 0) {
        f32 a = Math_SinS(req->speed * req->countdown);

        Quake_UpdateShakeInfo(req, shake, a, Rand_ZeroOne() * a);
        req->countdown--;
    }
    return req->countdown;
}

s16 Quake_Callback5(QuakeRequest* req, ShakeInfo* shake) {
    if (req->countdown > 0) {
        f32 a = Math_SinS(req->speed * req->countdown);

        Quake_UpdateShakeInfo(req, shake, a, a);
        req->countdown--;
    }
    return req->countdown;
}

s16 Quake_Callback6(QuakeRequest* req, ShakeInfo* shake) {
    s32 pad;
    f32 a;

    req->countdown--;
    a = Math_SinS(req->speed * ((req->countdown & 0xF) + 500));
    Quake_UpdateShakeInfo(req, shake, a, Rand_ZeroOne() * a);
    return 1;
}

s16 Quake_Callback3(QuakeRequest* req, ShakeInfo* shake) {
    if (req->countdown > 0) {
        f32 a = Math_SinS(req->speed * req->countdown) * ((f32)req->countdown / (f32)req->countdownMax);

        Quake_UpdateShakeInfo(req, shake, a, a);
        req->countdown--;
    }
    return req->countdown;
}

s16 Quake_Callback2(QuakeRequest* req, ShakeInfo* shake) {
    if (req->countdown > 0) {
        f32 a = Rand_ZeroOne();

        Quake_UpdateShakeInfo(req, shake, a, Rand_ZeroOne() * a);
        req->countdown--;
    }
    return req->countdown;
}

s16 Quake_Callback4(QuakeRequest* req, ShakeInfo* shake) {
    if (req->countdown > 0) {
        f32 a = Rand_ZeroOne() * ((f32)req->countdown / (f32)req->countdownMax);

        Quake_UpdateShakeInfo(req, shake, a, Rand_ZeroOne() * a);
        req->countdown--;
    }
    return req->countdown;
}

s16 Quake_GetFreeIndex(void) {
    s32 i;
    s32 ret;
    s32 min = 0x10000;

    for (i = 0; i < ARRAY_COUNT(sQuakeRequest); i++) {
        if (sQuakeRequest[i].callbackIdx == 0) {
            ret = i;
            min = 0x20000;
            break;
        }

        if (sQuakeRequest[i].countdown < min) {
            min = sQuakeRequest[i].countdown;
            ret = i;
        }
    }

    if (min != 0x20000) {
        osSyncPrintf(VT_COL(YELLOW, BLACK) "quake: too many request %d is changed new one !!\n" VT_RST, ret);
    }

    return ret;
}

// ZMP: in multiplayer every player's main camera is camera 0: a quake a player caused (owner) shakes only that player's
// camera, a quake of the world (owner -1) every camera; and a quake advances once per tick whatever the number of
// cameras that apply it.
static s32 sZmpQuakeOwner[4] = { -1, -1, -1, -1 };
static u32 sZmpQuakeFrame[4];
static ShakeInfo sZmpQuakeShake[4];

// ZMP: the quakes in progress are simulation state kept outside the heap: they shake the picture of the players
// (eye, look-at, field of view), those pictures decide which actors are "on screen" (phase 5b), and a quake takes a
// number from the shared random generator when it starts. A player who enters a group, or reloads its state, must
// get the group's quakes with it (D-081: before, it kept its own, or none, and desynchronized).
// clang-format off (its layout of this list is different on every run)
#define QUAKE_SHIP_SAVESTATE_FIELDS(F) \
    F(sQuakeRequest)                   \
    F(D_80126250)                      \
    F(sQuakeRequestCount)              \
    F(sZmpQuakeOwner)                  \
    F(sZmpQuakeFrame)                  \
    F(sZmpQuakeShake)
// clang-format on
SHIP_SAVESTATE_DEFINE(Quake, QUAKE_SHIP_SAVESTATE_FIELDS)

// ZMP: the values of the quakes in progress for the state hash of a group (no pointers). Returns how many s16 it
// wrote (at most `max`).
s32 Zmp_QuakeHashData(s16* out, s32 max) {
    s32 n = 0;
    s32 i;

    if (max < 1) {
        return 0;
    }
    out[n++] = sQuakeRequestCount;
    for (i = 0; i < ARRAY_COUNT(sQuakeRequest); i++) {
        QuakeRequest* r = &sQuakeRequest[i];

        if (r->callbackIdx == 0 || n + 14 > max) {
            continue;
        }
        out[n++] = (s16)i;
        out[n++] = (s16)r->callbackIdx;
        out[n++] = r->randIdx;
        out[n++] = r->countdownMax;
        out[n++] = r->countdown;
        out[n++] = r->speed;
        out[n++] = r->y;
        out[n++] = r->x;
        out[n++] = r->zoom;
        out[n++] = r->rotZ;
        out[n++] = r->unk_1C;
        out[n++] = r->camPtrIdx;
        out[n++] = r->unk_14.unk_00;
        out[n++] = (s16)sZmpQuakeOwner[i];
    }
    return n;
}

// ZMP (diagnostics, state dumps): the quake in slot idx. Returns 0 if that slot is free.
s32 Zmp_QuakeInfo(s32 idx, s32* callback, s32* countdown, s32* zoom, s32* owner) {
    if (idx < 0 || idx >= ARRAY_COUNT(sQuakeRequest) || sQuakeRequest[idx].callbackIdx == 0) {
        return 0;
    }
    *callback = (s32)sQuakeRequest[idx].callbackIdx;
    *countdown = sQuakeRequest[idx].countdown;
    *zoom = sQuakeRequest[idx].zoom;
    *owner = sZmpQuakeOwner[idx];
    return 1;
}

s32 Zmp_QuakeCount(void) {
    return sQuakeRequestCount;
}

QuakeRequest* Quake_AddImpl(Camera* cam, u32 callbackIdx) {
    s16 idx = Quake_GetFreeIndex();
    QuakeRequest* req = &sQuakeRequest[idx];

    sZmpQuakeOwner[idx] = Zmp_QuakeOwner(); // ZMP
    sZmpQuakeFrame[idx] = 0xFFFFFFFF;       // ZMP

    memset(req, 0, sizeof(QuakeRequest));
    req->cam = cam;
    req->camPtrIdx = cam->thisIdx;
    req->callbackIdx = callbackIdx;
    req->unk_1C = 1;
    req->randIdx = ((s16)(Rand_ZeroOne() * (f32)0x10000) & ~3) + idx;
    sQuakeRequestCount++;

    return req;
}

void Quake_Remove(QuakeRequest* req) {
    req->callbackIdx = 0;
    req->countdown = -1;
    sQuakeRequestCount--;
}

QuakeRequest* Quake_GetRequest(s16 idx) {
    QuakeRequest* req = &sQuakeRequest[idx & 3];

    if (req->callbackIdx == 0) {
        return NULL;
    }

    if (idx != req->randIdx) {
        return NULL;
    }

    return req;
}

QuakeRequest* Quake_SetValue(s16 idx, s16 valueType, s16 value) {
    QuakeRequest* req = Quake_GetRequest(idx);

    if (req == NULL) {
        return NULL;
    } else {
        switch (valueType) {
            case 1:
                req->speed = value;
                break;
            case 2:
                req->y = value;
                break;
            case 4:
                req->x = value;
                break;
            case 8:
                req->zoom = value;
                break;
            case 0x10:
                req->rotZ = value;
                break;
            case 0x20:
                req->unk_14.unk_00 = value;
                break;
            case 0x40:
                req->unk_14.unk_02 = value;
                break;
            case 0x80:
                req->unk_14.unk_04 = value;
                break;
            case 0x100:
                req->countdown = value;
                req->countdownMax = req->countdown;
                break;
            case 0x200:
                req->unk_1C = value;
                break;
        }
    }
}

u32 Quake_SetSpeed(s16 idx, s16 value) {
    QuakeRequest* req = Quake_GetRequest(idx);

    if (req != NULL) {
        req->speed = value;
        return true;
    }
    return false;
}

u32 Quake_SetCountdown(s16 idx, s16 value) {
    QuakeRequest* req = Quake_GetRequest(idx);

    if (req != NULL) {
        req->countdown = value;
        req->countdownMax = req->countdown;
        return true;
    }
    return false;
}

s16 Quake_GetCountdown(s16 idx) {
    QuakeRequest* req = Quake_GetRequest(idx);

    if (req != NULL) {
        return req->countdown;
    }
    return 0;
}

u32 Quake_SetQuakeValues(s16 idx, s16 y, s16 x, s16 zoom, s16 rotZ) {
    QuakeRequest* req = Quake_GetRequest(idx);

    if (req != NULL) {
        req->y = y;
        req->x = x;
        req->zoom = zoom;
        req->rotZ = rotZ;
        return true;
    }
    return false;
}

u32 Quake_SetUnkValues(s16 idx, s16 arg1, SubQuakeRequest14 arg2) {
    QuakeRequest* req = Quake_GetRequest(idx);

    if (req != NULL) {
        req->unk_1C = arg1;

        req->unk_14 = arg2;
        return true;
    }
    return false;
}

void Quake_Init(void) {
    s16 i;

    for (i = 0; i < ARRAY_COUNT(sQuakeRequest); i++) {
        sQuakeRequest[i].callbackIdx = 0;
        sQuakeRequest[i].countdown = 0;
    }
    D_80126250 = 1;
    sQuakeRequestCount = 0;
}

s16 Quake_Add(Camera* cam, u32 callbackIdx) {
    return Quake_AddImpl(cam, callbackIdx)->randIdx;
}

u32 Quake_RemoveFromIdx(s16 idx) {
    QuakeRequest* req = Quake_GetRequest(idx);

    if (req != NULL) {
        Quake_Remove(req);
        return true;
    }
    return false;
}

s16 Quake_Calc(Camera* camera, QuakeCamCalc* camData) {
    f32 max;
    f32 max2;
    QuakeRequest* req;
    ShakeInfo shake;
    f32 absSpeedDiv;
    s16* temp;
    u32 pad2;
    s32 idx;
    s32 ret;
    u32 eq;
    Vec3f vec;
    PlayState* play;

    play = camera->play;
    vec.x = 0.0f;
    vec.y = 0.0f;
    vec.z = 0.0f;
    camData->rotZ = 0;
    camData->unk_1A = 0;
    camData->zoom = 0;
    camData->atOffset.x = 0.0f;
    camData->atOffset.y = 0.0f;
    camData->atOffset.z = 0.0f;
    camData->eyeOffset.x = 0.0f;
    camData->eyeOffset.y = 0.0f;
    camData->eyeOffset.z = 0.0f;
    camData->unk_20 = 0.0f;

    if (sQuakeRequestCount == 0) {
        return 0;
    }

    ret = 0;
    for (idx = 0; idx < ARRAY_COUNT(sQuakeRequest); idx++) {
        req = &sQuakeRequest[idx];
        if (req->callbackIdx != 0) {
            if (play->cameraPtrs[req->camPtrIdx] == NULL) {
                osSyncPrintf(VT_COL(YELLOW, BLACK) "quake: stopped! 'coz camera [%d] killed!!\n" VT_RST,
                             req->camPtrIdx);
                Quake_Remove(req);
            } else {
                s16 zmpAlive;
                temp = &camera->thisIdx;
                eq = req->cam->thisIdx != *temp;
                if (!Zmp_QuakeShakesCamera(sZmpQuakeOwner[idx], camera)) { // ZMP: another player's quake
                    continue;
                }
                absSpeedDiv = ABS(req->speed) / (f32)0x8000;
                if (Zmp_MultiActive() && sZmpQuakeFrame[idx] == play->gameplayFrames) { // ZMP: once per tick
                    shake = sZmpQuakeShake[idx];
                    zmpAlive = 1;
                } else {
                    zmpAlive = sQuakeCallbacks[req->callbackIdx](req, &shake);
                    sZmpQuakeFrame[idx] = play->gameplayFrames; // ZMP
                    sZmpQuakeShake[idx] = shake;                // ZMP
                }
                if (zmpAlive == 0) {
                    Quake_Remove(req);
                } else if (eq == 0) {
                    if (fabsf(camData->atOffset.x) < fabsf(shake.vec1.x)) {
                        camData->atOffset.x = shake.vec1.x;
                    }
                    if (fabsf(camData->atOffset.y) < fabsf(shake.vec1.y)) {
                        camData->atOffset.y = shake.vec1.y;
                    }
                    if (fabsf(camData->atOffset.z) < fabsf(shake.vec1.z)) {
                        camData->atOffset.z = shake.vec1.z;
                    }
                    if (fabsf(camData->eyeOffset.x) < fabsf(shake.vec2.x)) {
                        camData->eyeOffset.x = shake.vec2.x;
                    }
                    if (fabsf(camData->eyeOffset.y) < fabsf(shake.vec2.y)) {
                        camData->eyeOffset.y = shake.vec2.y;
                    }
                    if (fabsf(camData->eyeOffset.z) < fabsf(shake.vec2.z)) {
                        camData->eyeOffset.z = shake.vec2.z;
                    }
                    if (camData->rotZ < shake.rotZ) {
                        camData->rotZ = shake.rotZ;
                        camData->unk_1A = shake.unk_1A;
                    }
                    if (camData->zoom < shake.zoom) {
                        camData->zoom = shake.zoom;
                    }

                    max = OLib_Vec3fDist(&shake.vec1, &vec) * absSpeedDiv;
                    max2 = OLib_Vec3fDist(&shake.vec2, &vec) * absSpeedDiv;
                    if (max < max2) {
                        max = max2;
                    }
                    max2 = (camData->rotZ * 0.005f) * absSpeedDiv;
                    if (max < max2) {
                        max = max2;
                    }
                    max2 = (camData->zoom * 0.005f) * absSpeedDiv;
                    if (max < max2) {
                        max = max2;
                    }
                    if (camData->unk_20 < max) {
                        camData->unk_20 = max;
                    }

                    ret++;
                }
            }
        }
    }
    return ret;
}
