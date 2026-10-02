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
// behind and beside it, facing where it faces. A spot without floor near the same height, or behind a wall, is
// skipped; with no free spot the player stays where it is.
void PlaceInArc(PlayState* play, int trigger) {
    Player* t = Zmp::Players::SlotPlayer(trigger);
    if (t == nullptr) {
        return;
    }
    // Beside it first, then behind-beside, in front-beside and behind (behind is often the door it came in by).
    static const s16 kAngles[] = { (s16)0x4000, (s16)0xC000, (s16)0x6000, (s16)0xA000, (s16)0x2000,
                                   (s16)0xE000, (s16)0x5000, (s16)0xB000, (s16)0x8000 };
    const f32 kRadius = 120.0f;
    s16 yaw = t->actor.shape.rot.y;
    int next = 0;
    for (int k = 0; k < ZMP_MAX_PLAYERS; k++) {
        Player* p = Zmp::Players::SlotPlayer(k);
        if (k == trigger || p == nullptr || Zmp::Players::SlotDowned(k)) {
            continue;
        }
        bool placed = false;
        while (next < (int)(sizeof(kAngles) / sizeof(kAngles[0])) && !placed) {
            s16 ang = (s16)(yaw + kAngles[next++]);
            Vec3f pos = t->actor.world.pos;
            pos.x += Math_SinS(ang) * kRadius;
            pos.z += Math_CosS(ang) * kRadius;
            Vec3f probe = pos;
            probe.y += 50.0f;
            CollisionPoly* poly = nullptr;
            s32 bgId = 0;
            f32 floorY = BgCheck_EntityRaycastFloor3(&play->colCtx, &poly, &bgId, &probe);
            if (floorY == BGCHECK_Y_MIN || fabsf(floorY - t->actor.world.pos.y) > 40.0f) {
                continue;
            }
            // Never on a scene exit, a void or lava.
            if (SurfaceType_GetSceneExitIndex(&play->colCtx, poly, bgId) != 0) {
                continue;
            }
            u32 floorType = SurfaceType_GetFloorType(&play->colCtx, poly, bgId);
            if (floorType == 5 || floorType == 9 || floorType == 12) {
                continue;
            }
            pos.y = floorY;
            Vec3f from = t->actor.world.pos;
            from.y += 30.0f;
            Vec3f to = pos;
            to.y += 30.0f;
            Vec3f hit;
            CollisionPoly* wall = nullptr;
            if (BgCheck_EntityLineTest1(&play->colCtx, &from, &to, &hit, &wall, true, false, false, true, &bgId)) {
                continue; // a wall between them
            }
            p->actor.world.pos = pos;
            p->actor.prevPos = pos;
            p->actor.home.pos = pos;
            p->actor.world.rot.y = p->actor.shape.rot.y = yaw;
            p->yaw = yaw;
            p->linearVelocity = 0.0f;
            p->actor.speedXZ = 0.0f;
            placed = true;
            Zmp::Log("zmp: cutscene: slot " + std::to_string(k) + " placed next to slot " + std::to_string(trigger));
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
        PlaceInArc(play, trigger);
    } else if (!active && gZmpSim.globalCs) {
        gZmpSim.globalCs = 0;
        gZmpSim.csTrigger = -1;
        gZmpSim.csStarter = -1;
        Camera_ZmpResetInterface(1);
        Zmp::Log("zmp: global cutscene ends");
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
    bool shared = gZmpSim.globalCs || play->csCtx.state != CS_STATE_IDLE;
    if (owner == local || local < 0 || (shared && owner == gZmpSim.anchor)) {
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
