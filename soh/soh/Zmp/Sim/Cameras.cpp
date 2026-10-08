// ZMP: per-player active camera and cutscene scope (PLAN.md 2.9, phase 4). See ZmpPlayers.h.
//
// The engine has one "active camera" (play->activeCamera) and up to three sub cameras shared by the scene. In the
// multiplayer simulation the active camera is part of the player context, like the main camera: while code runs for
// player k, play->activeCamera is k's. Every sub camera gets a scope when it is created:
//   - GLOBAL: boss cutscenes, scripted cutscenes (CutsceneContext), scene transitions (blue warp) and a short list of
//     one-point cutscenes that introduce a room or a boss. Making it active makes it everybody's active camera; the
//     players the cutscene is not about stand still, placed in an arc around the one it is about.
//   - PLAYER(k): one-point cutscenes (chest appearing, crawlspace, switch results...) and the cutscene cameras of
//     NPCs and objects, created in the context of player k. Only k sees it and only k's active camera changes: the
//     other players keep their own camera and their control.
// Each sub camera updates in the context of its owner (PLAYER) or creator (GLOBAL); the anchor's active camera is
// updated last, so play->view ends the tick as the canonical view of the simulation (D-025).
// Everything here except the render views is simulation state (gZmpSim, identical on every machine).

#include "ZmpPlayers.h"

#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <string>

#include "soh/Zmp/ZmpLog.h"
#include "soh/Zmp/Test/Mutants.h"

extern "C" {
#include "variables.h"
#include "functions.h"
#include "macros.h"
extern PlayState* gPlayState;
}

namespace {

// Render helpers: the view each sub camera computed in the last tick (not simulation state).
View sSubView[NUM_CAMS];
bool sSubViewValid[NUM_CAMS];

// OnePointCutscene_Init in progress.
bool sOnePoint = false;
s16 sOnePointId = 0;
Actor* sOnePointActor = nullptr;
bool sOnePointHidesHud = true;
// A private one-point cutscene of another player than the anchor is set up from its owner's view (the original
// reads play->view, which is the anchor's canonical view).
bool sViewSwapped = false;
View sKeepView;

bool ValidSlot(int k) {
    return k >= 0 && k < ZMP_MAX_PLAYERS;
}

bool IsSub(int camId) {
    return camId >= CAM_ID_SUB_FIRST && camId < NUM_CAMS;
}

bool Present(int k) {
    return Zmp::Players::IsPresent(k);
}

// One-point cutscenes everybody watches: introductions of rooms and bosses (completed empirically, PLAN.md 2.9.3).
bool GlobalOnePoint(s16 csId) {
    // None so far: the room and boss introductions of the Deku Tree are not one-point cutscenes (the boss's own
    // cameras are global). Add identifiers here as they are found.
    (void)csId;
    return false;
}

// Actors whose own sub cameras (not one-point) everybody watches besides the bosses: scene transitions.
bool GlobalActor(const Actor* actor) {
    switch (actor->id) {
        // (the blue warp is its player's own since phase 5b: each player who enters it leaves alone)
        case ACTOR_EN_CLEAR_TAG: // boss-like
        case ACTOR_EN_FHG:       // Phantom Ganon's horse (boss intro)
            return true;
        case ACTOR_BOSS_MO:
            // D-135 (row A2 of the Water Temple): the camera of Morpha's tentacle shaking a Link it grabbed is that
            // Link's own; the others go on fighting and can free it (the core and its cutscenes stay everybody's).
            // The tentacle runs in the grabbed Link's context (D-134), so its camera is created as that player's.
            if (actor->params == 100 && !Zmp_TestMutant("sacudida_global")) { // (BOSSMO_TENTACLE; mutant: as before)
                return false;
            }
            return true;
        default:
            return actor->category == ACTORCAT_BOSS;
    }
}

int Ctx() {
    return ValidSlot(gZmpSim.ctx) ? gZmpSim.ctx : gZmpSim.anchor;
}

int ActiveOf(PlayState* play, int k) {
    return k == gZmpSim.ctx ? play->activeCamera : gZmpSim.slots[k].activeCam;
}

void SetActiveOf(PlayState* play, int k, s16 camId) {
    if (k == gZmpSim.ctx) {
        play->activeCamera = camId;
    } else {
        gZmpSim.slots[k].activeCam = camId;
    }
}

Camera* MainOf(PlayState* play, int k) {
    return k == gZmpSim.ctx ? &play->mainCamera : &gZmpSim.slots[k].camera;
}

bool IsGlobal(int camId) {
    return IsSub(camId) && gZmpSim.camScope[camId] == ZMP_CAM_GLOBAL;
}

// Context a sub camera updates and finishes in.
int CamContext(int camId) {
    int s = gZmpSim.camScope[camId];
    if (s != ZMP_CAM_GLOBAL && Present(s)) {
        return s;
    }
    int c = gZmpSim.camCreator[camId];
    return Present(c) ? c : gZmpSim.anchor;
}

int ScopeFor(PlayState* play, bool onePoint, s16 csId, Actor* actor) {
    int ctx = Ctx();
    if (play->csCtx.state != CS_STATE_IDLE || gZmpSim.globalCs || gZmpSim.groupDefeat) {
        return ZMP_CAM_GLOBAL; // inside a cutscene everybody watches, or the group's game over
    }
    const Actor* cur = actor != nullptr ? actor : Zmp::Players::CurrentActor();
    if (cur != nullptr && cur->id != ACTOR_PLAYER && GlobalActor(cur)) {
        return ZMP_CAM_GLOBAL;
    }
    if (onePoint) {
        return GlobalOnePoint(csId) ? ZMP_CAM_GLOBAL : ctx;
    }
    // A sub camera created outside any actor update (scene code, scripted cutscene set-up) is the scene's.
    return cur == nullptr ? ZMP_CAM_GLOBAL : ctx;
}

// The camera state other slots may have kept from a camera that no longer exists or is not theirs.
void Sanitize(PlayState* play) {
    for (int k = 0; k < ZMP_MAX_PLAYERS; k++) {
        if (!Present(k)) {
            continue;
        }
        int a = ActiveOf(play, k);
        if (IsSub(a) && play->cameraPtrs[a] == nullptr) {
            SetActiveOf(play, k, CAM_ID_MAIN);
            a = CAM_ID_MAIN;
        }
        Camera* main = MainOf(play, k);
        int child = main->childCamIdx;
        if (IsSub(child) && (play->cameraPtrs[child] == nullptr ||
                             (gZmpSim.camScope[child] != ZMP_CAM_GLOBAL && gZmpSim.camScope[child] != k))) {
            main->childCamIdx = SUBCAM_FREE;
            child = SUBCAM_FREE;
        }
        // Back on its main camera with nothing queued: the main camera runs again (a global cutscene can end in the
        // context of another player than the one whose main camera it put on hold).
        if (a == CAM_ID_MAIN && !IsSub(child) && (main->status == CAM_STAT_WAIT || main->status == CAM_STAT_UNK3)) {
            main->status = CAM_STAT_ACTIVE;
        }
    }
}

// Arc of radius 120 around the player a global cutscene is about (PLAN.md 2.9 point 4): the others are placed
// behind and beside it, facing where it faces.
//
// D-111 (family 4 of reports/fase6/FAMILIAS.md): the arc was laid out once, on the tick the cutscene started, and
// measured against the cutscene player's own height. Three things left the others far from it: (1) a boss intro that
// starts when the first player of the group arrives (Twinrova, Ganondorf, Ganon): the others arrive during it and
// appear at the entrance, 200-450 away, and in Ganondorf's room they kept watching their own camera; (2) the script
// moves its player after the first tick (the position "the script assigns"): the arc stayed where it had been;
// (3) the cutscene player in the air (King Dodongo's intro: it falls into the room): no spot had floor near its
// height and nobody moved. Now the arc is kept while the cutscene runs (MaintainArc): its centre is the floor under
// the cutscene player; whoever arrives is placed in it and watches the cutscene's camera; when the cutscene player
// has moved more than 40 from the centre (and stands still, or is more than 120 away) the arc is laid out again.
// A spot passes the spawn spot checks (D-107) from the centre and is not where another Link stands; sixteen
// directions on rings of 120, then 90, 150 and 60.
Vec3f ArcCentre(PlayState* play, Player* t) {
    // (mutant "arco_solo_al_empezar": the arc of before, measured from the cutscene player's own height)
    if (Zmp_TestMutant("arco_solo_al_empezar")) {
        return t->actor.world.pos;
    }
    return Zmp::Players::GroundBelow(play, t->actor.world.pos);
}

// D-138 (finding BI): a blue warp may appear while a group cutscene still holds everybody (Morpha's, 60 ticks before
// its death cutscene ends); when its player is moved out of the warp's reach the arc follows it and was laid out again
// with a spot 35 from the warp, which takes whoever stands there without stepping in. No arc spot within 90 of a blue
// warp.
bool NearBlueWarp(PlayState* play, const Vec3f& pos) {
    if (Zmp_TestMutant("arco_en_el_portal")) { // (mutant: the arc of before)
        return false;
    }
    for (Actor* a = play->actorCtx.actorLists[ACTORCAT_ITEMACTION].head; a != nullptr; a = a->next) {
        if (a->id == ACTOR_DOOR_WARP1) {
            f32 dx = a->world.pos.x - pos.x;
            f32 dz = a->world.pos.z - pos.z;
            if (sqrtf(dx * dx + dz * dz) < 90.0f && fabsf(a->world.pos.y - pos.y) < 100.0f) {
                return true;
            }
        }
    }
    return false;
}

void PlaceInArc(PlayState* play, int trigger, u8 mask) {
    Player* t = Zmp::Players::SlotPlayer(trigger);
    if (t == nullptr) {
        return;
    }
    // Beside it first, then behind-beside, in front-beside and behind (behind is often the door it came in by); then
    // the directions in between and in front (sixteen in all, the arc of a narrow or broken floor needs them).
    static const s16 kAngles[] = { (s16)0x4000, (s16)0xC000, (s16)0x6000, (s16)0xA000, (s16)0x2000, (s16)0xE000,
                                   (s16)0x5000, (s16)0xB000, (s16)0x8000, (s16)0x3000, (s16)0xD000, (s16)0x7000,
                                   (s16)0x9000, (s16)0x1000, (s16)0xF000, (s16)0x0000 };
    static const f32 kRadii[] = { 120.0f, 90.0f, 150.0f, 60.0f };
    const int kAngleCount = (int)(sizeof(kAngles) / sizeof(kAngles[0]));
    const int kCount = kAngleCount * (int)(sizeof(kRadii) / sizeof(kRadii[0]));
    Vec3f centre = ArcCentre(play, t);
    s16 yaw = t->actor.shape.rot.y;
    bool used[sizeof(kAngles) / sizeof(kAngles[0]) * sizeof(kRadii) / sizeof(kRadii[0])] = {};
    // Where Links will stand: the cutscene player and whoever is not moved now.
    Vec3f taken[ZMP_MAX_PLAYERS + 1];
    int nTaken = 0;
    taken[nTaken++] = t->actor.world.pos;
    for (int k = 0; k < ZMP_MAX_PLAYERS; k++) {
        Player* p = Zmp::Players::SlotPlayer(k);
        if (p != nullptr && k != trigger && !(mask & (1 << k))) {
            taken[nTaken++] = p->actor.world.pos;
        }
    }
    for (int k = 0; k < ZMP_MAX_PLAYERS; k++) {
        Player* p = Zmp::Players::SlotPlayer(k);
        if (k == trigger || p == nullptr || Zmp::Players::SlotDowned(k) || !(mask & (1 << k))) {
            continue;
        }
        bool placed = false;
        for (int c = 0; c < kCount && !placed; c++) {
            if (used[c]) {
                continue;
            }
            s16 ang = (s16)(yaw + kAngles[c % kAngleCount]);
            f32 radius = kRadii[c / kAngleCount];
            Vec3f pos = centre;
            pos.x += Math_SinS(ang) * radius;
            pos.z += Math_CosS(ang) * radius;
            bool free = true;
            for (int n = 0; n < nTaken && free; n++) {
                f32 dx = taken[n].x - pos.x;
                f32 dz = taken[n].z - pos.z;
                free = sqrtf(dx * dx + dz * dz) >= 40.0f || fabsf(taken[n].y - centre.y) >= 60.0f;
            }
            if (!free || NearBlueWarp(play, pos) || !Zmp::Players::ArcSpotOk(play, centre, pos)) {
                continue;
            }
            used[c] = true;
            taken[nTaken++] = pos;
            p->actor.world.pos = pos;
            p->actor.prevPos = pos;
            p->actor.home.pos = pos;
            p->actor.world.rot.y = p->actor.shape.rot.y = yaw;
            p->yaw = yaw;
            p->linearVelocity = 0.0f;
            p->actor.speedXZ = 0.0f;
            p->actor.velocity.y = 0.0f;
            placed = true;
            Zmp::Log("zmp: cutscene: slot " + std::to_string(k) + " placed next to slot " + std::to_string(trigger) +
                     " (" + std::to_string((int)radius) + " away)");
        }
        if (!placed) {
            Zmp::Log("zmp: cutscene: no spot next to slot " + std::to_string(trigger) + " for slot " +
                     std::to_string(k) + ", it stays where it is");
        }
    }
}

// Every tick of a group cutscene (D-111): who arrived is placed and watches the cutscene's camera; a cutscene player
// that moved away takes the arc with it.
void MaintainArc(PlayState* play) {
    if (Zmp_TestMutant("arco_solo_al_empezar")) {
        return;
    }
    int trigger = gZmpSim.csTrigger;
    Player* t = Zmp::Players::SlotPlayer(trigger);
    if (t == nullptr) {
        return;
    }
    u8 held = 0;
    for (int k = 0; k < ZMP_MAX_PLAYERS; k++) {
        if (k != trigger && Zmp::Players::SlotPlayer(k) != nullptr && !Zmp::Players::SlotDowned(k)) {
            held |= (u8)(1 << k);
        }
    }
    // (a player who left, or lies downed, is placed again when it is back)
    gZmpSim.csArcPlaced &= held;
    Vec3f c = ArcCentre(play, t);
    f32 cx = c.x - gZmpSim.csArcCentre[0];
    f32 cz = c.z - gZmpSim.csArcCentre[2];
    f32 away = sqrtf(cx * cx + cz * cz);
    f32 sx = t->actor.world.pos.x - t->actor.prevPos.x;
    f32 sz = t->actor.world.pos.z - t->actor.prevPos.z;
    f32 step = sqrtf(sx * sx + sz * sz);
    bool moved = away > 40.0f || fabsf(c.y - gZmpSim.csArcCentre[1]) > 40.0f;
    if (moved && (step < 1.0f || away > 120.0f)) {
        gZmpSim.csArcPlaced = 0;
    }
    u8 todo = held & (u8)~gZmpSim.csArcPlaced;
    if (todo != 0) {
        if (gZmpSim.csArcPlaced == 0) {
            gZmpSim.csArcCentre[0] = (s16)c.x;
            gZmpSim.csArcCentre[1] = (s16)c.y;
            gZmpSim.csArcCentre[2] = (s16)c.z;
        }
        PlaceInArc(play, trigger, todo);
        gZmpSim.csArcPlaced |= todo;
    }
    // Everybody watches the cutscene's camera (PLAN.md 2.9 point 2): one who arrived during it starts on its own.
    int cam = -1;
    for (int k = 0; k < ZMP_MAX_PLAYERS && cam < 0; k++) {
        int a = Present(k) ? ActiveOf(play, k) : CAM_ID_MAIN;
        if (IsGlobal(a) && play->cameraPtrs[a] != nullptr) {
            cam = a;
        }
    }
    if (cam < 0) {
        return;
    }
    for (int k = 0; k < ZMP_MAX_PLAYERS; k++) {
        if (Present(k) && !IsGlobal(ActiveOf(play, k))) {
            SetActiveOf(play, k, (s16)cam);
            Zmp::Log("zmp: cutscene: slot " + std::to_string(k) + " watches the group cutscene's camera " +
                     std::to_string(cam));
        }
    }
}

bool AnyGlobalActive(PlayState* play, int* creator) {
    for (int k = 0; k < ZMP_MAX_PLAYERS; k++) {
        if (!Present(k)) {
            continue;
        }
        int a = ActiveOf(play, k);
        if (IsGlobal(a)) {
            if (creator != nullptr) {
                *creator = gZmpSim.camCreator[a];
            }
            return true;
        }
    }
    return false;
}

// D-120 (finding AQ): a Link that lies downed when a group cutscene ends is left down and out of it: an order of the
// cutscene it could not carry out lying down (the "wait" every player gets, or the scene's own if it was its player
// and fell during it) does not outlive the cutscene. Before, a Link downed during King Dodongo's death stayed "in a
// cutscene" until it left the room: revived, it went into that order instead of standing up free. It stays down
// (D-036, D-117: nobody is revived for free); a partner revives it as always.
void ReleaseDowned() {
    if (Zmp_TestMutant("caido_retenido")) { // (mutant: the downed Link keeps the order, as before D-120)
        return;
    }
    for (int k = 0; k < ZMP_MAX_PLAYERS; k++) {
        Player* p = Zmp::Players::SlotPlayer(k);
        if (p == nullptr || !Zmp::Players::SlotDowned(k)) {
            continue;
        }
        if (p->csAction != 0 || (p->stateFlags1 & PLAYER_STATE1_IN_CUTSCENE)) {
            Zmp::Log("zmp: cutscene: slot " + std::to_string(k) + " lies downed: its cutscene order " +
                     std::to_string(p->csAction) + " is dropped");
            p->csAction = 0;
            p->unk_6AD = 0;
            p->stateFlags1 &= ~PLAYER_STATE1_IN_CUTSCENE;
        }
    }
}

void UpdateGlobalCutscene(PlayState* play) {
    // Skip request of the host (the anchor): START during a scripted cutscene. It stays until the cutscene ends, so a
    // press while the cutscene's own commands are not running (a text box) is not lost.
    if (play->csCtx.state == CS_STATE_IDLE) {
        gZmpSim.skipRequested = 0;
    } else if (Present(gZmpSim.anchor)) {
        const Input& in = gZmpSim.ctx == gZmpSim.anchor ? play->state.input[0] : gZmpSim.slots[gZmpSim.anchor].input;
        if (CHECK_BTN_ALL(in.press.button, BTN_START)) {
            gZmpSim.skipRequested = 1;
        }
    }
    int creator = -1;
    bool scripted = play->csCtx.state != CS_STATE_IDLE;
    bool active = scripted || AnyGlobalActive(play, &creator);
    if (active && !gZmpSim.globalCs) {
        int trigger = scripted ? gZmpSim.csStarter : creator;
        if (!Present(trigger) || Zmp::Players::SlotDowned(trigger)) {
            trigger = gZmpSim.anchor;
        }
        gZmpSim.globalCs = 1;
        gZmpSim.csTrigger = (s8)trigger;
        Camera_ZmpResetInterface(0);
        Zmp::Log("zmp: global cutscene starts (" + std::string(scripted ? "scripted" : "camera") + "), about slot " +
                 std::to_string(trigger));
        gZmpSim.csArcPlaced = 0;
        if (Zmp_TestMutant("arco_solo_al_empezar")) {
            PlaceInArc(play, trigger, 0xFF);
        }
    } else if (!active && gZmpSim.globalCs) {
        gZmpSim.globalCs = 0;
        gZmpSim.csTrigger = -1;
        gZmpSim.csStarter = -1;
        gZmpSim.csArcPlaced = 0;
        Camera_ZmpResetInterface(1);
        Zmp::Log("zmp: global cutscene ends");
        ReleaseDowned();
    }
    if (active && gZmpSim.globalCs) {
        MaintainArc(play);
    }
    // Outside the events everybody watches the shared letterbox and HUD are the normal ones (the players' own cameras
    // drive only their own, Present.cpp): what a camera left before the group formed or a scene started goes back.
    if (!active && play->gameOverCtx.state == GAMEOVER_INACTIVE && play->transitionMode == TRANS_MODE_OFF &&
        play->msgCtx.msgMode == MSGMODE_NONE) {
        if (ShrinkWindow_GetVal() != 0) {
            Letterbox_SetSizeTarget(0);
        }
        if (gSaveContext.hudVisibilityMode >= 1 && gSaveContext.hudVisibilityMode <= 13) {
            Interface_ChangeHudVisibilityMode(HUD_VISIBILITY_ALL);
        }
    }
}

} // namespace

// ---------------------------------------------------------------------------------------------------
// C API

extern "C" void Zmp_OnePointBegin(PlayState* play, s16 csId, Actor* actor) {
    sOnePoint = true;
    sOnePointId = csId;
    sOnePointActor = actor;
    sOnePointHidesHud = true;
    if (Zmp_MultiActive()) {
        int scope = ScopeFor(play, true, csId, actor);
        // A private cutscene leaves the HUD alone (the HUD is shared by everybody).
        sOnePointHidesHud = scope == ZMP_CAM_GLOBAL;
        if (scope != ZMP_CAM_GLOBAL && scope != gZmpSim.anchor && Present(scope) && gZmpSim.slots[scope].hasView) {
            sKeepView = play->view;
            play->view = gZmpSim.slots[scope].view;
            sViewSwapped = true;
        }
    }
}

extern "C" void Zmp_OnePointInitDone(PlayState* play) {
    if (sViewSwapped) {
        play->view = sKeepView;
        sViewSwapped = false;
    }
}

extern "C" void Zmp_OnePointEnd(void) {
    sOnePoint = false;
    sOnePointActor = nullptr;
}

extern "C" s32 Zmp_OnePointHidesHud(void) {
    return sOnePointHidesHud ? 1 : 0;
}

namespace {
uint32_t sInvalidCameraModes = 0;
}

extern "C" void Zmp_OnInvalidCameraMode(s16 setting, s16 mode) {
    sInvalidCameraModes++;
    Zmp::Log("zmp: INVALID CAMERA MODE: setting " + std::to_string(setting) + " has no mode " + std::to_string(mode) +
             " (the game reads past that setting's table), " + std::to_string(sInvalidCameraModes) + " so far");
}

extern "C" u32 Zmp_InvalidCameraModeCount(void) {
    return sInvalidCameraModes;
}

extern "C" void Zmp_OnSubCameraCreated(PlayState* play, s16 camId) {
    if (!IsSub(camId)) {
        return;
    }
    sSubViewValid[camId] = false;
    if (!Zmp_MultiActive()) {
        gZmpSim.camScope[camId] = ZMP_CAM_GLOBAL;
        gZmpSim.camCreator[camId] = -1;
        return;
    }
    int scope = ScopeFor(play, sOnePoint, sOnePointId, sOnePoint ? sOnePointActor : nullptr);
    gZmpSim.camScope[camId] = (s8)scope;
    gZmpSim.camCreator[camId] = (s8)Ctx();
    Zmp::Log("zmp: sub camera " + std::to_string(camId) + " created by slot " + std::to_string(Ctx()) + ", scope " +
             (scope == ZMP_CAM_GLOBAL ? std::string("GLOBAL") : "PLAYER(" + std::to_string(scope) + ")") +
             (sOnePoint ? ", one-point " + std::to_string(sOnePointId) : std::string()));
}

extern "C" void Zmp_OnePointCameraStart(PlayState* play, Camera* subCam) {
    // A private camera starts from its owner's own view (play->view, which OnePointCutscene_Init copied, is the
    // anchor's).
    if (!Zmp_MultiActive() || !IsSub(subCam->thisIdx)) {
        return;
    }
    int scope = gZmpSim.camScope[subCam->thisIdx];
    if (scope != ZMP_CAM_GLOBAL && scope != gZmpSim.anchor && Present(scope) && gZmpSim.slots[scope].hasView) {
        const View& v = gZmpSim.slots[scope].view;
        subCam->at = v.lookAt;
        subCam->eye = v.eye;
        subCam->fov = v.fovy;
    }
}

extern "C" s16 Zmp_ChangeCameraStatus(PlayState* play, s16 camIdx, s16 status) {
    if (status == CAM_STAT_ACTIVE) {
        s16 prev = play->activeCamera;
        play->activeCamera = camIdx;
        int ctx = Ctx();
        if (IsGlobal(camIdx)) {
            // Everybody watches it.
            for (int k = 0; k < ZMP_MAX_PLAYERS; k++) {
                if (k != ctx && Present(k)) {
                    SetActiveOf(play, k, camIdx);
                }
            }
        } else if (IsGlobal(prev) && prev != camIdx) {
            // Leaving a global camera: everybody who watched it goes to the same place (back to its own main camera
            // when that is the destination).
            for (int k = 0; k < ZMP_MAX_PLAYERS; k++) {
                if (k != ctx && Present(k) && ActiveOf(play, k) == prev) {
                    SetActiveOf(play, k, camIdx);
                }
            }
        }
    }
    return Camera_ChangeStatus(play->cameraPtrs[camIdx], status);
}

extern "C" void Zmp_OnCameraCleared(PlayState* play, s16 camIdx) {
    if (!IsSub(camIdx)) {
        return;
    }
    sSubViewValid[camIdx] = false;
    if (!Zmp_MultiActive()) {
        return;
    }
    for (int k = 0; k < ZMP_MAX_PLAYERS; k++) {
        if (Present(k) && k != gZmpSim.ctx && gZmpSim.slots[k].activeCam == camIdx) {
            gZmpSim.slots[k].activeCam = CAM_ID_MAIN;
        }
    }
}

// B1: the private one-point cutscene `csId` of a player who goes away (Ruto's warp starts one of 999 frames for the
// player in it; the scene ended there in the original). Nobody else watches it; left running it would take a sub
// camera for another player's turn and end, much later, in the context of whoever is the anchor then.
extern "C" void Zmp_ClearOnePointOf(PlayState* play, s32 slot, s16 csId) {
    if (!Zmp_MultiActive() || !ValidSlot(slot)) {
        return;
    }
    for (int i = CAM_ID_SUB_FIRST; i < NUM_CAMS; i++) {
        Camera* c = play->cameraPtrs[i];
        if (c != nullptr && gZmpSim.camScope[i] == slot && c->csId == csId) {
            Zmp::Log("zmp: one-point camera " + std::to_string(i) + " (" + std::to_string(csId) + ") of slot " +
                     std::to_string(slot) + " cleared: its player went away");
            Play_ClearCamera(play, i);
        }
    }
    // Play_ClearCamera puts back on its main camera every other player who watched a cleared camera, not the one
    // whose context is live: when that is this player, play->activeCamera would still name the cleared camera (this
    // one, or the warp's own that the caller cleared just before), and letting this player go right after reads it
    // (z_player.c, func_8005B1A4: the game closed when the anchor left through Ruto's warp). Same for the child of
    // its main camera.
    Sanitize(play);
}

extern "C" void Zmp_OnAllSubCamerasCleared(PlayState* play) {
    for (int i = 0; i < NUM_CAMS; i++) {
        sSubViewValid[i] = false;
    }
    if (!Zmp_MultiActive()) {
        return;
    }
    for (int k = 0; k < ZMP_MAX_PLAYERS; k++) {
        if (Present(k)) {
            SetActiveOf(play, k, CAM_ID_MAIN);
        }
    }
}

extern "C" void Zmp_UpdateCameras(PlayState* play) {
    int anchor = gZmpSim.anchor;
    Sanitize(play);
    UpdateGlobalCutscene(play);
    int anchorActive = ActiveOf(play, anchor);
    for (int i = CAM_ID_SUB_FIRST; i < NUM_CAMS; i++) {
        if (i == anchorActive || play->cameraPtrs[i] == nullptr) {
            continue;
        }
        int ctx = CamContext(i);
        Zmp::Players::SwitchContext(play, ctx);
        // A private camera drives only its owner's letterbox and HUD.
        gZmpCameraInterfaceMuted = gZmpSim.camScope[i] != ZMP_CAM_GLOBAL ? gZmpSim.camScope[i] + 1 : 0;
        Camera_Update(play->cameraPtrs[i]);
        gZmpCameraInterfaceMuted = 0;
        if (play->cameraPtrs[i] != nullptr) {
            sSubView[i] = play->view;
            sSubViewValid[i] = true;
        }
    }
    // Main cameras of every player (the anchor's last).
    Zmp_UpdateMainCameras(play);
    if (IsSub(anchorActive) && play->cameraPtrs[anchorActive] != nullptr) {
        Zmp::Players::SwitchContext(play, CamContext(anchorActive));
        gZmpCameraInterfaceMuted =
            gZmpSim.camScope[anchorActive] != ZMP_CAM_GLOBAL ? gZmpSim.camScope[anchorActive] + 1 : 0;
        Camera_Update(play->cameraPtrs[anchorActive]);
        gZmpCameraInterfaceMuted = 0;
        if (play->cameraPtrs[anchorActive] != nullptr) {
            sSubView[anchorActive] = play->view;
            sSubViewValid[anchorActive] = true;
        }
    }
    Zmp::Players::SwitchContext(play, anchor);
}

extern "C" void Zmp_CameraInterfaceOwnerBegin(Camera* camera) {
    if (!Zmp_MultiActive() || camera == nullptr) {
        return;
    }
    int id = camera->thisIdx;
    if (id == CAM_ID_MAIN) {
        gZmpCameraInterfaceMuted = Ctx() + 1;
    } else if (IsSub(id)) {
        gZmpCameraInterfaceMuted = gZmpSim.camScope[id] != ZMP_CAM_GLOBAL ? gZmpSim.camScope[id] + 1 : 0;
    }
}

extern "C" void Zmp_CameraInterfaceOwnerEnd(void) {
    gZmpCameraInterfaceMuted = 0;
}

extern "C" void Zmp_FinishCameras(PlayState* play) {
    int anchor = gZmpSim.anchor;
    // Distinct active sub cameras, in camera order.
    for (int i = CAM_ID_SUB_FIRST; i < NUM_CAMS; i++) {
        bool someone = false;
        for (int k = 0; k < ZMP_MAX_PLAYERS; k++) {
            if (Present(k) && ActiveOf(play, k) == i) {
                someone = true;
                break;
            }
        }
        if (!someone || play->cameraPtrs[i] == nullptr) {
            continue;
        }
        Zmp::Players::SwitchContext(play, CamContext(i));
        Camera_Finish(play->cameraPtrs[i]);
    }
    Zmp::Players::SwitchContext(play, anchor);
}

extern "C" s32 Zmp_OnUnderwaterLights(PlayState* play, s32 waterLightsIndex) {
    (void)play;
    // (mutation test, "agua_compartida": every camera changes the scene's lights and the sound, as before finding Y)
    if (!Zmp_MultiActive() || Zmp_TestMutant("agua_compartida")) {
        return 0;
    }
    // (coming out records nothing: the camera's own "eye under water" flag says whether the setting is in use)
    if (waterLightsIndex >= 0) {
        // (0x1F: the water has no setting of its own; the original uses the first one)
        gZmpSim.slots[Ctx()].waterLight = (s16)(waterLightsIndex == 0x1F ? 0 : waterLightsIndex);
    }
    return 1;
}

extern "C" s32 Zmp_HearsCameraWater(Camera* camera) {
    (void)camera;
    if (!Zmp_MultiActive() || Zmp_TestMutant("agua_compartida")) {
        return 1;
    }
    // (a camera updates in the context of the player it belongs to)
    return Ctx() == Zmp::Players::LocalSlot() ? 1 : 0;
}

extern "C" void Zmp_OnCutsceneStart(void) {
    if (gZmpSim.enabled && gZmpSim.inPlay) {
        gZmpSim.csStarter = gZmpSim.ctx;
    }
}

extern "C" s32 Zmp_FollowsCutsceneScript(Player* player) {
    if (!Zmp_MultiActive()) {
        return 1;
    }
    int k = Zmp::Players::SlotOf(&player->actor);
    int t = gZmpSim.globalCs ? gZmpSim.csTrigger : gZmpSim.csStarter;
    if (!Present(t)) {
        t = gZmpSim.anchor;
    }
    return k == t ? 1 : 0;
}

extern "C" s32 Zmp_PlayerHeldByCutscene(Player* player) {
    if (!Zmp_MultiActive() || !gZmpSim.globalCs) {
        return 0;
    }
    int k = Zmp::Players::SlotOf(&player->actor);
    return (k >= 0 && k != gZmpSim.csTrigger) ? 1 : 0;
}

extern "C" s32 Zmp_HostSkipsCutscene(PlayState* play) {
    if (!Zmp_MultiActive()) {
        return 0;
    }
    return gZmpSim.skipRequested ? 1 : 0;
}

// Text box of another player: its display list is dropped (the simulation side effects already happened).
namespace {
bool sHideText = false;
Gfx* sSaveOpa = nullptr;
Gfx* sSaveXlu = nullptr;
Gfx* sSaveOvl = nullptr;

struct NoticeRec {
    int slot;
    int itemId;
    std::chrono::steady_clock::time_point at;
};
std::deque<NoticeRec> sNotices;
} // namespace

extern "C" void Zmp_MessageDrawBegin(PlayState* play) {
    sHideText = false;
    if (!Zmp_MultiActive()) {
        return;
    }
    // Phase 5b: the text box in context is its player's. This screen shows the local player's, and the anchor's
    // during a cutscene everybody watches (the text of the cutscene).
    int owner = gZmpSim.ctx;
    int local = Zmp::Players::LocalSlot();
    if (local < 0 || Zmp::Players::ScreenShowsText(play, owner, local)) {
        return;
    }
    GraphicsContext* g = play->state.gfxCtx;
    sSaveOpa = g->polyOpa.p;
    sSaveXlu = g->polyXlu.p;
    sSaveOvl = g->overlay.p;
    sHideText = true;
}

extern "C" void Zmp_MessageDrawEnd(PlayState* play) {
    if (!sHideText) {
        return;
    }
    GraphicsContext* g = play->state.gfxCtx;
    g->polyOpa.p = sSaveOpa;
    g->polyXlu.p = sSaveXlu;
    g->overlay.p = sSaveOvl;
    sHideText = false;
}

extern "C" void Zmp_OnItemGet(Player* player, s32 itemId) {
    if (!Zmp_MultiActive()) {
        return;
    }
    int k = Zmp::Players::SlotOf(&player->actor);
    if (k < 0) {
        return;
    }
    Zmp::Log("zmp: slot " + std::to_string(k) + " got item " + std::to_string(itemId));
    sNotices.push_back({ k, itemId, std::chrono::steady_clock::now() });
    while (sNotices.size() > 8) {
        sNotices.pop_front();
    }
}

// ---------------------------------------------------------------------------------------------------
// C++ API

namespace Zmp::Players {

int SlotActiveCam(int slot) {
    if (!ValidSlot(slot) || gPlayState == nullptr) {
        return CAM_ID_MAIN;
    }
    return ActiveOf(gPlayState, slot);
}

int CamScope(int camId) {
    return IsSub(camId) ? gZmpSim.camScope[camId] : ZMP_CAM_GLOBAL;
}

bool GlobalCutscene() {
    return gZmpSim.globalCs != 0;
}

int CutsceneTrigger() {
    return gZmpSim.csTrigger;
}

std::vector<Notice> RecentNotices(double maxAge) {
    std::vector<Notice> out;
    auto now = std::chrono::steady_clock::now();
    for (auto& n : sNotices) {
        double age = std::chrono::duration<double>(now - n.at).count();
        if (age <= maxAge) {
            out.push_back({ n.slot, n.itemId, age });
        }
    }
    return out;
}

// Whether the screen of `local` shows the text box of `owner`: its own; during a cutscene everybody watches, the
// anchor's (a scripted cutscene's text) and, D-126 (row F6 of the Forest Temple), the one of the player the group
// cutscene is about: a boss that speaks in its own cutscene speaks to "the player", which is that player (D-084), and
// its words were read on one screen only (Phantom Ganon's last words, 0x108E).
bool ScreenShowsText(PlayState* play, int owner, int local) {
    if (owner == local) {
        return true;
    }
    bool shared = gZmpSim.globalCs || play->csCtx.state != CS_STATE_IDLE;
    if (shared && owner == gZmpSim.anchor) {
        return true;
    }
    return gZmpSim.globalCs && owner == gZmpSim.csTrigger && !Zmp_TestMutant("texto_de_uno");
}

bool LocalTextHidden() {
    // Another player of the scene has a text box open that this screen does not show.
    if (!Zmp_MultiActive() || gPlayState == nullptr || gZmpSim.globalCs || gPlayState->csCtx.state != CS_STATE_IDLE) {
        return false;
    }
    int local = LocalSlot();
    for (int k = 0; k < ZMP_MAX_PLAYERS; k++) {
        if (k != local && Present(k) && SlotMsgMode(k) != MSGMODE_NONE) {
            return local >= 0;
        }
    }
    return false;
}

int PictureWaterLight(PlayState* play) {
    int local = LocalSlot();
    if (!Zmp_MultiActive() || play == nullptr || !ValidSlot(local) || !Present(local) ||
        Zmp_TestMutant("agua_compartida")) {
        return -1;
    }
    // The camera this PC's picture comes from: its player's main camera, or the sub camera it is watching (whose
    // water was recorded for the player in whose context that camera updates).
    int active = ActiveOf(play, local);
    int owner = local;
    const Camera* cam = nullptr;
    if (active == CAM_ID_MAIN) {
        cam = MainOf(play, local);
    } else if (IsSub(active) && play->cameraPtrs[active] != nullptr) {
        cam = play->cameraPtrs[active];
        owner = CamContext(active);
    }
    if (cam == nullptr || !(cam->unk_14C & 0x100)) { // (0x100: its eye is under water, Camera_UpdateWater)
        return -1;
    }
    return gZmpSim.slots[owner].waterLight;
}

const View* LocalPicture(PlayState* play, int slot) {
    int anchor = gZmpSim.anchor;
    int a = ActiveOf(play, slot);
    int anchorActive = ActiveOf(play, anchor);
    if (a == CAM_ID_MAIN) {
        const ZmpPlayerSlot& s = gZmpSim.slots[slot];
        return s.hasView ? &s.view : nullptr;
    }
    if (a == anchorActive || !IsSub(a) || !sSubViewValid[a]) {
        return nullptr; // the canonical view is this camera's
    }
    return &sSubView[a];
}

} // namespace Zmp::Players
