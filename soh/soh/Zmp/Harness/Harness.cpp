#ifdef ZMP_HARNESS

// Game-thread side of the harness (PLAN.md 5.2): executes commands between frames, answers
// waits, and overrides the port 0 pad right after the physical read.

#include "Harness.h"
#include "HarnessQueue.h"
#include "Screenshot.h"

#include <chrono>
#include <thread>
#include <deque>
#include <filesystem>
#include <string>

#include <nlohmann/json.hpp>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <imgui.h>
#include <ship/Context.h>
#include <ship/window/Window.h>
#include <libultraship/bridge/consolevariablebridge.h>
#include <libultraship/libultra/controller.h>

#include "soh/Zmp/Zmp.h"
#include "soh/Zmp/ZmpLog.h"
#include "soh/Zmp/Net/ZmpClient.h"
#include "soh/Zmp/Sim/CVarProfile.h"
#include "soh/Zmp/Sim/Session.h"
#include "soh/Zmp/Sim/ZmpPlayers.h"
#include "soh/Zmp/Net/Lockstep.h"
#include "soh/Zmp/Net/Autosave.h"
#include "soh/Zmp/Net/SharedGame.h"
#include "soh/Zmp/Ui/ZmpWindow.h"
#include "soh/Zmp/Ui/ChatBox.h"
#include "soh/Zmp/ZmpCVars.h"
#include "soh/Zmp/State/StateBlob.h"
#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/Enhancements/game-interactor/GameInteractor_Hooks.h"

extern "C" {
#include <z64.h>
#include "variables.h"
#include "functions.h"
#include "macros.h"
#include "overlays/actors/ovl_Boss_Goma/z_boss_goma.h"
#include "overlays/actors/ovl_Boss_Dodongo/z_boss_dodongo.h" // ZMP phase 6 scenario tests
#include "overlays/actors/ovl_Boss_Va/z_boss_va.h"           // ZMP phase 6 scenario tests
#include "overlays/actors/ovl_En_Ru1/z_en_ru1.h"             // ZMP phase 6 scenario tests
#include "overlays/actors/ovl_En_Ossan/z_en_ossan.h"
#include "overlays/actors/ovl_En_GirlA/z_en_girla.h"
extern u16 gTimeSpeed;
extern u8 sAudioExtraFilter;
extern PlayState* gPlayState;
void FileChoose_Main(GameState* thisx);
void Opening_Main(GameState* thisx);
void Select_Main(GameState* thisx);
void Title_Main(GameState* thisx);
void Play_Main(GameState* thisx);
void Play_Init(GameState* thisx);
}

using json = nlohmann::json;

#ifdef _WIN32
extern "C" const char __ImageBase; // start of the executable image (MSVC linker symbol)
#endif

namespace Zmp::Harness {

namespace {
float sFrameIntervalMs = 0.0f;
float sFrameIntervalMaxMs = 0.0f;
// Phase 5b ("nobody waits for anybody"): wall time between two consecutive logic ticks, since the last reset.
std::vector<float> sTickGaps;
uint32_t sGapLastTick = 0;
std::chrono::steady_clock::time_point sGapLastAt;
bool sGapValid = false;
int sGapSettled = 0;
float sGateWaitMaxMs = 0.0f; // longest time the tick gate stayed closed (waiting for the others), settled ticks only

struct PadStep {
    uint32_t buttons = 0;
    int8_t stickX = 0;
    int8_t stickY = 0;
    int8_t rStickX = 0;
    int8_t rStickY = 0;
    int32_t frames = 1; // <= 0 means hold until input.release
};

enum class WaitKind { Tick, Stable, Scene, GameState, SimTick, ReplayEnd };

struct Wait {
    WaitKind kind;
    RequestPtr req;
    uint32_t targetTick = 0;
    int scene = -1;
    std::string gameState;
    uint32_t satisfiedFrames = 0;
    std::chrono::steady_clock::time_point deadline;
};

uint32_t sFrame = 0;
std::deque<PadStep> sScript;
int32_t sStepLeft = 0;
bool sStepRead = false; // the local menu logic has read the pad with the current step (AdvanceScript)
int sWaitedForRead = 0; // inputs sent while the current step waited for that read
bool sScriptActive = false;
RequestPtr sScriptReq; // answered when the script ends (null for input.set)
uint32_t sInjectedFrames = 0;
std::vector<Wait> sWaits;
bool sQuitRequested = false;

// A capture at its own size (fast suite): the window was resized for it; it is taken a few frames later (the swap
// chain has the new size by then) and the window goes back to its tile.
struct PendingShot {
    RequestPtr req;
    std::string path;
    int frames = 0;
    uint32_t afterTick = 0; // not before this simulation tick has been drawn (0: as soon as the window is ready)
    CaptureRestore restore;
};
std::vector<PendingShot> sShots;

void TakeShot(const RequestPtr& req, const std::string& path) {
    int w = 0, h = 0;
    std::string info;
    if (CaptureGameWindow(path, &w, &h, &info)) {
        req->Reply({ { "ok", true }, { "path", path }, { "width", w }, { "height", h }, { "method", info } });
    } else {
        req->Reply({ { "ok", false }, { "error", info } });
    }
}

std::string GameStateName() {
    if (gGameState == nullptr) {
        return "none";
    }
    // gGameState->init holds the *next* state's init (SET_NEXT_GAMESTATE); each state's
    // init stores its own main function, so identify the state by `main`.
    auto main = gGameState->main;
    if (main == Play_Main) {
        return "play";
    }
    if (main == FileChoose_Main) {
        return "file_select";
    }
    if (main == Title_Main) {
        return "title";
    }
    if (main == Opening_Main) {
        return "opening";
    }
    if (main == Select_Main) {
        return "map_select";
    }
    return "other";
}

bool InPlay() {
    return gGameState != nullptr && gGameState->main == Play_Main && gPlayState != nullptr;
}

bool SceneReady(int scene) {
    if (!InPlay() || gPlayState->sceneNum != scene) {
        return false;
    }
    if (gPlayState->transitionTrigger != TRANS_TRIGGER_OFF || gPlayState->transitionMode != TRANS_MODE_OFF) {
        return false;
    }
    return GET_PLAYER(gPlayState) != nullptr;
}

json Vec3(const Vec3f& v) {
    return json::array({ v.x, v.y, v.z });
}

json Rot3(const Vec3s& r) {
    return json::array({ r.x, r.y, r.z });
}

PadStep ParsePad(const json& j) {
    PadStep p;
    p.buttons = j.value("buttons", 0u);
    p.stickX = (int8_t)j.value("stick_x", 0);
    p.stickY = (int8_t)j.value("stick_y", 0);
    p.rStickX = (int8_t)j.value("rstick_x", 0);
    p.rStickY = (int8_t)j.value("rstick_y", 0);
    p.frames = j.value("frames", 1);
    return p;
}

void EndScript(bool completed) {
    if (sScriptReq) {
        sScriptReq->Reply({ { "ok", completed },
                            { "tick", sFrame },
                            { "injected_frames", sInjectedFrames },
                            { "error", completed ? "" : "released" } });
        sScriptReq.reset();
    }
    sScript.clear();
    sStepLeft = 0;
    sScriptActive = false;
}

void StartScript(std::deque<PadStep> steps, RequestPtr req) {
    EndScript(false);
    sScript = std::move(steps);
    sScriptReq = std::move(req);
    sInjectedFrames = 0;
    sScriptActive = !sScript.empty();
    sStepLeft = sScriptActive ? sScript.front().frames : 0;
    sStepRead = false;
    sWaitedForRead = 0;
    if (!sScriptActive && sScriptReq) {
        EndScript(true);
    }
}

// (control switch while a failure of this mechanism is investigated: 0 = the script advances with the pad read, as
// before the fast suite)
bool ScriptBySend() {
    return CVarGetInteger(CVAR_ZMP("Test.ScriptBySend"), 1) != 0;
}

// In a group the script's pad is this player's input to the group, except while its own pause menu is open: the menu
// reads the pad itself, tick by tick, and a step used up by an input sent (several can go out between two ticks when
// the PC is busy) would never be seen by it (a button press of two steps was lost: the fast suite, D-083).
bool ScriptGoesToTheGroup() {
    auto st = Zmp::Lockstep::GetStatus();
    return st.phase == Zmp::Lockstep::Phase::Running && !st.localInputBlocked;
}

// A step of the script is not over until this player's own menu logic has read the pad with it. The pause menu opens
// and is driven by the local pad, read once per tick; the steps advance with the inputs sent, and when the PC is busy
// two inputs go out between two ticks: a press of two steps was over before any tick had seen it, and the menu did not
// open (the fast suite, D-083). A step waits for that read for at most a second of inputs (a game that is not playing
// ticks reads nothing).
void AdvanceScript() {
    const PadStep& step = sScript.front();
    sInjectedFrames++;
    if (step.frames <= 0) {
        return; // hold
    }
    if (sStepLeft <= 1 && !sStepRead && ScriptBySend() && ScriptGoesToTheGroup() && sWaitedForRead < 20) {
        sWaitedForRead++;
        return;
    }
    if (--sStepLeft <= 0) {
        sStepRead = false;
        sWaitedForRead = 0;
        sScript.pop_front();
        if (sScript.empty()) {
            EndScript(true);
        } else {
            sStepLeft = sScript.front().frames;
        }
    }
}

// Called once per frame with the physical pads already read.
void ApplyScriptInput(OSContPad* pads) {
    if (!sScriptActive) {
        return;
    }
    const PadStep& step = sScript.front();
    pads[0].button = (CONTROLLERBUTTONS_T)step.buttons;
    pads[0].stick_x = step.stickX;
    pads[0].stick_y = step.stickY;
    pads[0].right_stick_x = step.rStickX;
    pads[0].right_stick_y = step.rStickY;
    pads[0].gyro_x = 0;
    pads[0].gyro_y = 0;
    pads[0].err_no = 0;
    if (ScriptBySend() && ScriptGoesToTheGroup()) {
        return; // in a group the script advances with each input sent (ScriptPadForSend)
    }
    sInjectedFrames++;
    if (step.frames <= 0) {
        return; // hold
    }
    if (--sStepLeft <= 0) {
        sStepRead = false;
        sWaitedForRead = 0;
        sScript.pop_front();
        if (sScript.empty()) {
            // The pad of this frame is the last one; the game update that follows uses it.
            EndScript(true);
        } else {
            sStepLeft = sScript.front().frames;
        }
    }
}

json PlayerJson(Player* player, int slot = 0) {
    Actor* a = &player->actor;
    // Multiplayer simulation: the slot's own health and camera (live when it is the context player).
    bool multi = Zmp_MultiActive() && slot >= 0;
    bool live = !multi || slot == gZmpSim.ctx;
    const Camera* cam = live ? &gPlayState->mainCamera : &gZmpSim.slots[slot].camera;
    const TargetContext* target = live ? &gPlayState->actorCtx.targetCtx : &gZmpSim.slots[slot].target;
    int health = multi ? Zmp::Players::SlotHealth(slot) : gSaveContext.health;
    // Per-player block (phase 3): the slot's own values, live in the save context when it is the context.
    ZmpPlayerBlock blk{};
    if (multi) {
        blk = Zmp::Players::SlotBlock(slot);
    } else {
        blk.magic = gSaveContext.magic;
        blk.equips = gSaveContext.equips;
        memcpy(blk.ammo, gSaveContext.inventory.ammo, sizeof(blk.ammo));
        for (int i = 0; i < 4; i++) {
            blk.bottles[i] = gSaveContext.inventory.items[SLOT_BOTTLE_1 + i];
        }
    }
    json eq = json::object();
    eq["button_items"] = json::array();
    eq["c_slots"] = json::array();
    for (int i = 0; i < 8; i++) {
        eq["button_items"].push_back(blk.equips.buttonItems[i]);
    }
    for (int i = 0; i < 7; i++) {
        eq["c_slots"].push_back(blk.equips.cButtonSlots[i]);
    }
    eq["equipment"] = blk.equips.equipment;
    json ammo = json::array();
    for (int i = 0; i < 16; i++) {
        ammo.push_back(blk.ammo[i]);
    }
    json bottles = json::array();
    for (int i = 0; i < 4; i++) {
        bottles.push_back(blk.bottles[i]);
    }
    bool downed = multi && Zmp::Players::SlotDowned(slot);
    int spectate = multi ? Zmp::Players::SlotSpectate(slot) : -1;
    return {
        { "ok", true },
        { "index", slot },
        { "pos", Vec3(a->world.pos) },
        { "rot", Rot3(a->world.rot) },
        { "shape_rot", Rot3(a->shape.rot) },
        { "yaw", player->yaw },
        { "speed", player->linearVelocity },
        { "health", health },
        { "health_capacity", gSaveContext.healthCapacity },
        { "magic", blk.magic },
        { "magic_capacity", gSaveContext.magicCapacity },
        { "rupees", gSaveContext.rupees },
        { "equips", eq },
        { "ammo", ammo },
        { "bottles", bottles },
        { "items", json(std::vector<int>(gSaveContext.inventory.items, gSaveContext.inventory.items + 24)) },
        { "boots", player->currentBoots },
        { "tunic", player->currentTunic },
        { "revive_progress", multi ? Zmp::Players::SlotReviveProgress(slot) : 0 },
        { "reviver", multi ? Zmp::Players::SlotReviver(slot) : -1 },
        { "game_over_state", gPlayState->gameOverCtx.state },
        { "state_flags", json::array({ player->stateFlags1, player->stateFlags2, player->stateFlags3 }) },
        { "room", a->room },
        { "cur_room", gPlayState->roomCtx.curRoom.num },
        { "age", gSaveContext.linkAge == LINK_AGE_CHILD ? "child" : "adult" },
        { "downed", downed },
        { "camera",
          { { "eye", Vec3(cam->eye) }, { "at", Vec3(cam->at) }, { "setting", cam->setting }, { "mode", cam->mode } } },
        { "focus_actor", player->focusActor != nullptr ? json(player->focusActor->id) : json(nullptr) },
        // (what Navi points at for this player: a Z press locks on to it instead of starting the parallel camera)
        { "navi_pointed", target->arrowPointedActor != nullptr ? json(target->arrowPointedActor->id) : json(nullptr) },
        { "action_offset", (uint64_t)((uintptr_t)player->actionFunc - (uintptr_t)&__ImageBase) },
        { "bg_flags", a->bgCheckFlags },
        { "floor_y", a->floorHeight },
        { "spectating", spectate >= 0 ? json(spectate) : json(nullptr) },
        { "pause_local", Zmp::Pause::State() },
        { "pause_save_stage", Zmp::Pause::SaveStage() },
        { "active_cam", multi ? Zmp::Players::SlotActiveCam(slot) : gPlayState->activeCamera },
        // (diagnosis: the buttons the simulation applied to this slot in its last tick)
        { "sim_buttons", multi ? (uint32_t)gZmpSim.slots[slot].input.cur.button : 0u },
        { "sim_stick", json::array({ multi ? (int)gZmpSim.slots[slot].input.cur.stick_x : 0,
                                     multi ? (int)gZmpSim.slots[slot].input.cur.stick_y : 0 }) },
        { "cs_action", player->csAction },
        { "heat_seconds", multi ? Zmp::Players::SlotHeatSeconds(slot) : -1 },
        { "zmp_room", multi ? Zmp::Players::SlotRoom(slot) : gPlayState->roomCtx.curRoom.num },
        { "timer_state", gSaveContext.timerState },
        { "tick", sFrame },
    };
}

json ActorsJson(const json& cmd) {
    int onlyCat = cmd.value("category", -1);
    int onlyId = cmd.value("actor_id", -1); // "id" is the request id
    json list = json::array();
    for (int cat = 0; cat < ACTORCAT_MAX; cat++) {
        if (onlyCat >= 0 && cat != onlyCat) {
            continue;
        }
        for (Actor* a = gPlayState->actorCtx.actorLists[cat].head; a != nullptr; a = a->next) {
            if (onlyId >= 0 && a->id != onlyId) {
                continue;
            }
            list.push_back({
                { "id", a->id },
                { "category", a->category },
                { "params", a->params },
                { "pos", Vec3(a->world.pos) },
                { "rot", Rot3(a->world.rot) },
                { "shape_rot", Rot3(a->shape.rot) }, // (phase 6: a skull's jaw, a turning platform)
                { "health", a->colChkInfo.health },
                { "room", a->room },
                { "freeze_timer", a->freezeTimer },
                { "proj", Vec3(a->projectedPos) },
                // The player its logic acts on and the yaw to that player computed by the simulation this tick.
                { "target_slot", Zmp::Players::ActorTargetSlot(a) },
                { "yaw_to_player", a->yawTowardsPlayer },
                { "dist_to_player", a->xzDistToPlayer },
            });
            if (a->id == ACTOR_EN_OSSAN) {
                // Phase 5 tests (shop): the shopkeeper's state and the shelf slot under the cursor.
                const EnOssan* o = (const EnOssan*)a;
                list.back()["ossan_state"] = o->stateFlag;
                list.back()["ossan_cursor"] = o->cursorIndex;
                const EnGirlA* sel = o->cursorIndex < 8 ? o->shelfSlots[o->cursorIndex] : nullptr;
                list.back()["ossan_item_price"] = sel != nullptr ? sel->basePrice : -1;
                list.back()["ossan_item_params"] = sel != nullptr ? sel->actor.params : -1;
            }
            if (a->id == ACTOR_BOSS_GOMA) {
                // Tests of phase 4 (Gohma in co-op): its state machine and its children.
                const BossGoma* g = (const BossGoma*)a;
                list.back()["goma_action"] = (uint64_t)((uintptr_t)g->actionFunc - (uintptr_t)&__ImageBase);
                list.back()["goma_state"] = g->actionState;
                list.back()["goma_timer"] = g->timer;
                list.back()["goma_children"] = { g->childrenGohmaState[0], g->childrenGohmaState[1],
                                                 g->childrenGohmaState[2] };
            }
            if (a->id == ACTOR_BOSS_DODONGO) {
                // Phase 6 scenario tests (King Dodongo with five players): its own health counter, its cutscene state
                // and its action (an offset in the executable: the same on every machine of one build).
                const BossDodongo* d = (const BossDodongo*)a;
                list.back()["kd_health"] = d->health;
                list.back()["kd_cs_state"] = d->csState;
                list.back()["kd_action"] = (uint64_t)((uintptr_t)d->actionFunc - (uintptr_t)&__ImageBase);
                list.back()["kd_timer"] = d->unk_1DA;         // the death cutscene counts it 1000 down to 600
                list.back()["kd_scene"] = d->unk_1BC;         // 0 fighting (colliders on), 1 or 2 in a cutscene
                list.back()["kd_inhaling"] = d->unk_1E2;      // 1 while it breathes in (a bomb at its mouth is eaten)
                list.back()["kd_down"] = d->unk_1BE;          // 10 while it lies stunned and the sword hurts it
                list.back()["kd_camera"] = d->cutsceneCamera; // its own sub camera (0 none)
                list.back()["kd_mouth"] = Vec3(d->mouthPos);
            }
            if (a->id == ACTOR_EN_RU1) {
                // Phase 6 scenario tests (Ruto in Jabu-Jabu): her action and who carries her. The parent pointer is
                // compared with the players' pointers and never followed (a carrier that left the group would leave a
                // freed pointer here: finding J1 of reports/fase6/INVENTARIO.md).
                const EnRu1* r = (const EnRu1*)a;
                int carrier = -2; // -2 nobody, -1 something that is not a present player
                if (a->parent != nullptr) {
                    carrier = -1;
                    for (int k = 0; k < ZMP_MAX_PLAYERS; k++) {
                        if (Zmp::Players::IsPresent(k) && (const Actor*)Zmp::Players::SlotPlayer(k) == a->parent) {
                            carrier = k;
                        }
                    }
                }
                list.back()["ru1_action"] = r->action;
                list.back()["ru1_carrier"] = carrier;
                list.back()["ru1_room2"] = r->roomNum2;
            }
            if (a->id == ACTOR_BOSS_VA) {
                // Phase 6 scenario tests (Barinade with five players): every part is a Boss_Va (params: body -1,
                // supports 0-2, zappers 3-5, Bari 6-15, stumps 16-18, door 19).
                const BossVa* v = (const BossVa*)a;
                list.back()["va_dead"] = v->isDead;
                list.back()["va_on_ceiling"] = v->onCeiling;
                list.back()["va_timer"] = v->timer;
                list.back()["va_invincible"] = v->invincibilityTimer;
                list.back()["va_action"] = (uint64_t)((uintptr_t)v->actionFunc - (uintptr_t)&__ImageBase);
                if (a->params == BOSSVA_BODY) {
                    s32 st[6];
                    BossVa_ZmpDebug(st); // the fight's progress: statics of the overlay
                    list.back()["va_fight_phase"] = st[0];
                    list.back()["va_cs_state"] = st[1];
                    list.back()["va_phase4_hp"] = st[2];
                    list.back()["va_body_state"] = st[3];
                    list.back()["va_y_offset"] = v->actor.shape.yOffset;
                    list.back()["home"] = Vec3(v->actor.home.pos);
                }
            }
        }
    }
    return { { "ok", true }, { "tick", sFrame }, { "count", list.size() }, { "actors", list } };
}

bool IsPlayOnlyConsoleCommand(const std::string& cmdline) {
    std::string name = cmdline.substr(0, cmdline.find(' '));
    static const char* sAlwaysOk[] = { "file_select", "reset", "quit", "help", "set_slot", "clear" };
    for (const char* ok : sAlwaysOk) {
        if (name == ok) {
            return false;
        }
    }
    return true;
}

json LockstepJson() {
    auto ls = Zmp::Lockstep::GetStatus();
    json players = json::array();
    for (auto& p : ls.players) {
        players.push_back({ { "slot", p.slot },
                            { "name", p.name },
                            { "state", p.state },
                            { "rtt", p.rtt },
                            { "leader", p.leader },
                            { "scene", p.scene },
                            { "group", p.group } });
    }
    json notices = json::array();
    for (auto& n : ls.sceneNotices) {
        notices.push_back({ { "name", n.name }, { "scene", n.scene }, { "age", n.age } });
    }
    return { { "phase", Zmp::Lockstep::PhaseName(ls.phase) },
             { "slot", ls.slot },
             { "tick", ls.tick },
             { "delay", ls.delay },
             { "queued", ls.queued },
             { "waiting", ls.waiting },
             { "wait_ms", ls.waitMs },
             { "waiting_for", ls.waitingFor },
             { "stalls", ls.stalls },
             { "max_stall_ms", ls.maxStallMs },
             { "resyncs", ls.resyncs },
             { "resyncs_seen", ls.resyncsSeen },
             { "last_resync_tick", ls.lastResyncTick },
             { "leader", ls.leader },
             { "group_tick", ls.groupTick },
             { "countdown", ls.countdown },
             { "error", ls.lastError },
             { "present", Zmp::Players::PresentCount() },
             { "anchor", gZmpSim.anchor },
             { "last_spawn_ms", ls.lastSpawnMs },
             { "last_entry_lead", ls.lastEntryLead },
             { "relocated_loads", ls.relocatedLoads },
             { "last_relocated", ls.lastRelocated },
             { "last_relocated_odd", ls.lastRelocatedOdd },
             { "catching_up", ls.catchingUp },
             // (diagnosis of inputs that do not reach the simulation: what this machine last sent and for which tick)
             { "last_sent_buttons", ls.lastSentButtons },
             { "next_input_tick", ls.nextInputTick },
             { "next_server_tick", ls.nextServerTick },
             { "local_input_blocked", ls.localInputBlocked },
             { "inputs_held", ls.inputsHeld },
             { "tick_cost_us", ls.tickCostUs },
             { "ticks_played", ls.ticksPlayed },
             // One snapshot for the tests' "is this player really playing in that group?": its own Link is in the
             // group it runs in (an event sent for it now acts on it), and the scene the game is in.
             { "own_present", ls.phase == Zmp::Lockstep::Phase::Running && Zmp_MultiActive() && ls.slot >= 0 &&
                                  Zmp::Players::IsPresent(ls.slot) },
             { "game_scene", (gGameState != nullptr && gGameState->main == Play_Main && gPlayState != nullptr)
                                 ? (int)gPlayState->sceneNum
                                 : -1 },
             { "invite_by", Zmp::Players::Invite().by },
             { "invite_scene", Zmp::Players::Invite().scene },
             { "invite_hold", Zmp::Players::Invite().hold },
             { "age_pending", gZmpSim.agePending },
             { "multi", Zmp_MultiActive() != 0 },
             { "game_over_chooser", Zmp::Lockstep::GameOverChooser() },
             { "end_notice", ls.endNotice },
             { "save_skipped", ls.saveSkipped },
             // Phase 5: groups per scene, shared game, world clock.
             { "group", ls.groupId },
             { "group_scene", ls.groupScene },
             { "detach_scene", ls.detachScene },
             { "free_run", ls.freeRun },
             { "owns_save", ls.ownsSave },
             { "shared_sent", ls.sharedSent },
             { "shared_applied", ls.sharedApplied },
             { "last_shared", ls.lastShared },
             { "group_joins", ls.groupJoins },
             { "name_tags", ls.nameTags },
             { "last_join_ms", ls.lastJoinMs },
             { "clock_hold", gZmpSim.clockHold },
             { "scene_notices", notices },
             { "players", players } };
}

json NetStatusJson() {
    auto st = Zmp::Client::Get().GetStatus();
    json players = json::array();
    for (auto& p : st.players) {
        players.push_back({ { "id", p.id }, { "name", p.name }, { "slot", p.slot }, { "state", p.state } });
    }
    auto hs = Zmp::Client::Get().GetHandshake();
    return { { "ok", true },
             { "state", Zmp::Client::StateName(st.state) },
             { "host", st.host },
             { "port", st.port },
             { "room", st.room },
             { "name", st.name },
             { "player_id", st.playerId },
             { "players", players },
             { "rtt_ms", st.rttMs },
             { "reason", st.reason },
             { "detail", st.detail },
             { "build_hash", hs.buildHash },
             // (local diagnostic, never sent to the server: where this process has the executable, D-073)
             { "image_base", (uint64_t)(uintptr_t)&__ImageBase },
             { "oot_hash", hs.ootHash },
             { "rom_name", hs.romName },
             { "soh_hash", hs.sohHash },
             { "cvar_profile_hash", hs.cvarProfileHash },
             { "lockstep", LockstepJson() } };
}

std::string Hex(uint64_t v) {
    char b[24];
    snprintf(b, sizeof(b), "%016llX", (unsigned long long)v);
    return b;
}

json SessionJson() {
    auto st = Sim::GetStatus();
    json j = { { "mode", Sim::ModeName(st.mode) },
               { "armed", st.armed },
               { "paused", st.paused },
               { "tick", st.tick },
               { "length", st.replayLength },
               { "finished", st.replayFinished },
               { "checked", st.checked },
               { "mismatches", st.mismatches },
               { "first_mismatch", st.firstMismatch == UINT32_MAX ? json(nullptr) : json(st.firstMismatch) },
               { "speed", st.speed },
               { "file", st.file } };
    if (st.hasHash) {
        j["hash"] = Hex(st.lastHash);
    }
    return j;
}

std::chrono::steady_clock::time_point Deadline(const json& cmd) {
    return std::chrono::steady_clock::now() + std::chrono::milliseconds(cmd.value("timeout_ms", 60000));
}

} // namespace

void Dispatch(const RequestPtr& req) {
    const json& cmd = req->cmd;
    std::string name = cmd.value("cmd", std::string());
    if (name != "ping" && name.rfind("query.", 0) != 0) {
        Log("harness: " + cmd.dump());
    }

    if (name == "ping") {
        req->Reply({ { "ok", true },
                     { "tick", sFrame },
                     { "scene", InPlay() ? gPlayState->sceneNum : -1 },
                     { "room", InPlay() ? gPlayState->roomCtx.curRoom.num : -1 },
                     { "state", GameStateName() },
                     { "harness_port", GetPort() } });
    } else if (name == "input.set") {
        PadStep step = ParsePad(cmd);
        StartScript({ step }, nullptr);
        req->Reply({ { "ok", true }, { "tick", sFrame } });
    } else if (name == "input.script") {
        std::deque<PadStep> steps;
        for (auto& s : cmd.value("steps", json::array())) {
            json pad = s.contains("pad") ? s["pad"] : s;
            PadStep step = ParsePad(pad);
            step.frames = s.value("frames", step.frames);
            if (step.frames <= 0) {
                step.frames = 1;
            }
            steps.push_back(step);
        }
        if (cmd.value("async", false)) {
            StartScript(std::move(steps), nullptr);
            req->Reply({ { "ok", true }, { "tick", sFrame } });
        } else {
            StartScript(std::move(steps), req);
        }
    } else if (name == "input.release") {
        EndScript(false);
        req->Reply({ { "ok", true }, { "tick", sFrame } });
    } else if (name == "console") {
        std::string line = cmd.value("cmd_line", cmd.value("line", std::string()));
        if (line.empty()) {
            req->Reply({ { "ok", false }, { "error", "missing 'line'" } });
        } else if (!InPlay() && IsPlayOnlyConsoleCommand(line)) {
            req->Reply({ { "ok", false }, { "error", "not in play (state " + GameStateName() + ")" } });
        } else {
            std::string output;
            int32_t rc = Ship::Context::GetRawInstance()->GetConsole()->Run(line, &output);
            req->Reply({ { "ok", rc == 0 }, { "rc", rc }, { "output", output }, { "tick", sFrame } });
        }
    } else if (name == "query.player") {
        if (!InPlay() || GET_PLAYER(gPlayState) == nullptr) {
            req->Reply({ { "ok", false }, { "error", "not in play" }, { "state", GameStateName() } });
        } else if (Zmp_MultiActive()) {
            int slot = cmd.value("index", Zmp::Players::LocalSlot());
            Player* p = Zmp::Players::SlotPlayer(slot);
            if (p == nullptr) {
                req->Reply({ { "ok", false }, { "error", "no player in slot " + std::to_string(slot) } });
            } else {
                req->Reply(PlayerJson(p, slot));
            }
        } else if (cmd.value("index", 0) != 0) {
            req->Reply({ { "ok", false }, { "error", "only player 0 exists outside a multiplayer game" } });
        } else {
            req->Reply(PlayerJson(GET_PLAYER(gPlayState)));
        }
    } else if (name == "query.actors") {
        if (!InPlay()) {
            req->Reply({ { "ok", false }, { "error", "not in play" } });
        } else {
            req->Reply(ActorsJson(cmd));
        }
    } else if (name == "query.state") {
        auto st = Sim::GetStatus();
        json resp = { { "ok", true },
                      { "tick", sFrame },
                      { "sim_tick", st.tick },
                      { "state", GameStateName() },
                      { "scene", InPlay() ? gPlayState->sceneNum : -1 },
                      { "room", InPlay() ? gPlayState->roomCtx.curRoom.num : -1 },
                      { "day_time", gSaveContext.dayTime },
                      { "script_active", sScriptActive },
                      { "session", SessionJson() },
                      { "cvar_profile_hash", Hex(CVarProfile::Hash()) },
                      { "cvar_reverts", CVarProfile::RevertCount() },
                      { "audio_muted", Zmp_AudioMuted() },
                      { "audio_output_muted", Zmp_AudioOutputMuted() },
                      { "net", Zmp::Client::StateName(Zmp::Client::Get().GetStatus().state) },
                      { "lockstep", LockstepJson() },
                      { "fps", ImGui::GetCurrentContext() != nullptr ? ImGui::GetIO().Framerate : 0.0f },
                      { "frame_interval_ms", sFrameIntervalMs },
                      { "frame_interval_max_ms", sFrameIntervalMaxMs } };
        if (st.hasHash) {
            resp["hash"] = Hex(st.lastHash);
            resp["hash_tick"] = st.tick - 1;
        }
        if (InPlay()) {
            // Phase 4: cameras (per-player active camera, scope of the sub cameras), cutscene, rooms.
            json subs = json::array();
            for (int i = CAM_ID_SUB_FIRST; i < NUM_CAMS; i++) {
                const Camera* c = gPlayState->cameraPtrs[i];
                subs.push_back({ { "id", i },
                                 { "exists", c != nullptr },
                                 { "scope", Zmp::Players::CamScope(i) },
                                 { "status", c != nullptr ? c->status : 0 },
                                 { "setting", c != nullptr ? c->setting : 0 },
                                 { "cs_id", c != nullptr ? c->csId : 0 } });
            }
            json active = json::array();
            for (int k = 0; k < ZMP_MAX_PLAYERS; k++) {
                active.push_back(Zmp::Players::SlotActiveCam(k));
            }
            resp["cameras"] = { { "active_camera", gPlayState->activeCamera },
                                { "per_slot", active },
                                { "sub", subs },
                                { "global_cs", Zmp::Players::GlobalCutscene() },
                                { "cs_trigger", Zmp::Players::CutsceneTrigger() } };
            resp["cs_state"] = gPlayState->csCtx.state;
            resp["cs_frames"] = gPlayState->csCtx.frames;
            // Phase 5b: one text box per player. The text box of this instance's player (the anchor's during a
            // cutscene everybody watches, when its own is closed), and the mode of every slot's.
            MessageContext* msg = &gPlayState->msgCtx;
            if (Zmp_MultiActive()) {
                const MessageContext* own = Zmp::Players::SlotMessage(Zmp::Players::LocalSlot());
                const MessageContext* shared = Zmp::Players::SlotMessage(Zmp::Players::Anchor());
                bool cs = gPlayState->csCtx.state != CS_STATE_IDLE;
                msg = (MessageContext*)((own == nullptr || (own->msgMode == MSGMODE_NONE && cs && shared != nullptr))
                                            ? shared
                                            : own);
                if (msg == nullptr) {
                    msg = &gPlayState->msgCtx;
                }
                json oca = json::array(); // per slot: [ocarina mode, last song played]
                for (int k = 0; k < ZMP_MAX_PLAYERS; k++) {
                    const MessageContext* m = Zmp::Players::SlotMessage(k);
                    oca.push_back(
                        { m != nullptr ? (int)m->ocarinaMode : -1, m != nullptr ? (int)m->lastPlayedSong : -1 });
                }
                resp["ocarinas"] = oca;
                json modes = json::array();
                for (int k = 0; k < ZMP_MAX_PLAYERS; k++) {
                    modes.push_back(Zmp::Players::SlotMsgMode(k));
                }
                resp["msg_modes"] = modes;
            }
            resp["msg_mode"] = msg->msgMode;
            resp["msg_text_id"] = msg->textId;
            resp["talk_state"] = Message_GetState(msg);
            // The simulation's pause context: the group game over menu (D-058).
            resp["pause_state"] = gPlayState->pauseCtx.state;
            resp["prompt_choice"] = gPlayState->pauseCtx.promptChoice;
            resp["game_over"] = gPlayState->gameOverCtx.state;
            resp["prev_room"] = gPlayState->roomCtx.prevRoom.num;
            resp["rooms_loaded"] = Zmp::Players::LoadedRooms(gPlayState);
            resp["text_hidden"] = Zmp::Players::LocalTextHidden();
            // Phase 5b: the title card with the scene's name (running in the simulation) and whether this screen shows
            // it
            resp["play_inits"] = Zmp::Players::PlayInitCount();
            resp["invalid_camera_modes"] = Zmp_InvalidCameraModeCount();
            resp["rupee_debt"] = gZmpSim.rupeeDebt;
            resp["bg_image"] = Zmp::Players::LocalBgImage();
            resp["hud_health"] = Zmp::Players::HudHealth();
            resp["picture_eye"] = Vec3(Zmp::Players::PictureEye());
            resp["picture_at"] = Vec3(Zmp::Players::PictureAt());
            resp["do_action"] = gPlayState->interfaceCtx.unk_1F0;
            resp["light_effect_hidden"] = Zmp::Players::LightEffectHidden();
            // (of the same picture as the line above: the simulation's value below may be a tick newer)
            resp["light_adj_drawn"] = Zmp::Players::LightEffectDrawn();
            resp["light_adj"] = gPlayState->envCtx.adjAmbientColor[0];
            resp["light_ambient"] = gPlayState->lightCtx.ambientColor[0];
            resp["title_alpha"] = gPlayState->actorCtx.titleCtx.alpha;
            resp["title_hidden"] = Zmp_TitleCardHidden() != 0;
            resp["local_light"] = Zmp::Players::LocalLightSetting();
            resp["sim_light"] = gPlayState->envCtx.unk_BD;
            // Finding Y: the water's lights and muffled sound are each player's own picture and sound
            resp["water_light"] = Zmp::Players::LocalWaterLight();
            resp["sim_outdoor_light"] = gPlayState->envCtx.unk_1F;
            resp["sim_water_light"] = gPlayState->envCtx.unk_BF;
            resp["water_sound"] = sAudioExtraFilter;
            {
                const u8* drawn = Zmp::Players::LocalDrawnLight();
                resp["drawn_fog"] = { drawn[0], drawn[1], drawn[2] };
                resp["drawn_ambient"] = { drawn[3], drawn[4], drawn[5] };
            }
            resp["env_indoors"] = gPlayState->envCtx.indoors;
            resp["local_letterbox"] = Zmp::Players::LocalLetterbox();
            resp["autosave_count"] = Zmp::Autosave::SavedCount();
            resp["downed_overlay"] = Zmp::DownedOverlayText();
            resp["shared_letterbox"] = (int)ShrinkWindow_GetCurrentVal();
            resp["local_hud_mode"] = Zmp::Players::LocalHudMode();
            resp["shared_hud_mode"] = gSaveContext.hudVisibilityMode;
            resp["do_action"] = gPlayState->interfaceCtx.unk_1F0;
            resp["fill_screen"] = gPlayState->envCtx.fillScreen;
            resp["num_light_settings"] = gPlayState->envCtx.numLightSettings;
            resp["local_slot"] = Zmp::Players::LocalSlot();
            {
                Player* lp =
                    Zmp_MultiActive() ? Zmp::Players::SlotPlayer(Zmp::Players::LocalSlot()) : GET_PLAYER(gPlayState);
                int fl = -1;
                if (lp != nullptr && lp->actor.floorPoly != nullptr && lp->actor.floorBgId == BGCHECK_SCENE) {
                    fl = (int)SurfaceType_GetLightSettingIndex(&gPlayState->colCtx, lp->actor.floorPoly, BGCHECK_SCENE);
                }
                resp["floor_light"] = fl;
                resp["room_hot"] = gPlayState->roomCtx.curRoom.behaviorType2 == ROOM_BEHAVIOR_TYPE2_3;
            }
            json notices = json::array();
            for (auto& n : Zmp::Players::RecentNotices(30.0)) {
                notices.push_back({ { "slot", n.slot }, { "item", n.itemId }, { "age", n.age } });
            }
            resp["notices"] = notices;
            json slotRooms = json::array();
            for (int k = 0; k < ZMP_MAX_PLAYERS; k++) {
                slotRooms.push_back(Zmp::Players::SlotRoom(k));
            }
            resp["slot_rooms"] = slotRooms;
        }
        req->Reply(resp);
    } else if (name == "query.project") {
        // Phase 5b: where a point of the world is in the picture of a player (clip space, as the simulation has it).
        Vec3f world = { cmd.value("x", 0.0f), cmd.value("y", 0.0f), cmd.value("z", 0.0f) };
        Vec3f proj;
        f32 w = 0.0f;
        bool ok = Zmp::Players::ProjectForSlot(cmd.value("slot", -1), world, &proj, &w);
        req->Reply({ { "ok", ok }, { "tick", sFrame }, { "proj", { proj.x, proj.y, proj.z } }, { "w", w } });
    } else if (name == "query.chat") {
        // Phase 5b: the message box: what it shows now and everything posted so far ([category, text]).
        json r = { { "ok", true }, { "tick", sFrame }, { "visible", Zmp::Chat::VisibleLines() } };
        json hist = json::array();
        for (auto& [cat, text] : Zmp::Chat::History()) {
            hist.push_back({ cat, text });
        }
        r["history"] = hist;
        r["debug_overlay"] = CVarGetInteger(ZMP_CVAR_DEBUG_OVERLAY, 0) != 0;
        req->Reply(r);
    } else if (name == "query.gaps") {
        // Time between consecutive logic ticks since the last reset: worst, 99th percentile, mean.
        std::vector<float> g = sTickGaps;
        std::sort(g.begin(), g.end());
        double sum = 0;
        for (float v : g) {
            sum += v;
        }
        json r = { { "ok", true },
                   { "tick", sFrame },
                   { "count", g.size() },
                   { "max_ms", g.empty() ? 0.0f : g.back() },
                   { "p99_ms", g.empty() ? 0.0f : g[(size_t)((g.size() - 1) * 0.99)] },
                   { "mean_ms", g.empty() ? 0.0 : sum / g.size() },
                   { "over_100", (int)(g.end() - std::upper_bound(g.begin(), g.end(), 100.0f)) },
                   { "wait_max_ms", sGateWaitMaxMs },
                   { "frame_max_ms", sFrameIntervalMaxMs } };
        if (cmd.value("reset", false)) {
            sGateWaitMaxMs = 0.0f;
            sTickGaps.clear();
            sGapValid = false;
            sFrameIntervalMaxMs = 0.0f;
        }
        req->Reply(r);
    } else if (name == "query.sfx") {
        // Phase 5b: where this machine hears the sounds of a player (its own camera) next to the canonical position
        // of the simulation, and the sounds alive in the sound banks.
        if (!InPlay()) {
            req->Reply({ { "ok", false }, { "error", "not in play" } });
            return;
        }
        json r = { { "ok", true }, { "tick", sFrame } };
        json links = json::array();
        for (int k = 0; k < ZMP_MAX_PLAYERS; k++) {
            Player* p = Zmp::Players::SlotPlayer(k);
            if (p == nullptr) {
                continue;
            }
            const f32* pos = &p->actor.projectedPos.x;
            f32 x = Zmp_SfxCoord(pos, 0), y = Zmp_SfxCoord(pos, 1), z = Zmp_SfxCoord(pos, 2);
            links.push_back({ { "slot", k },
                              { "canonical", { pos[0], pos[1], pos[2] } },
                              { "heard", { x, y, z } },
                              { "dist", sqrtf(x * x + y * y + z * z) } });
        }
        r["players"] = links;
        json live = json::array();
        for (int bank = 0; bank < 7; bank++) {
            u8 i = gSoundBanks[bank][0].next;
            int guard = 0;
            while (i != 0xFF && guard++ < 64) {
                SoundBankEntry* e = &gSoundBanks[bank][i];
                live.push_back(
                    { { "bank", bank },
                      { "sfx", e->sfxId },
                      { "heard", { Zmp_SfxCoord(e->posX, 0), Zmp_SfxCoord(e->posX, 1), Zmp_SfxCoord(e->posX, 2) } },
                      { "canonical", { e->posX[0], e->posX[1], e->posX[2] } } });
                i = e->next;
            }
        }
        r["live"] = live;
        req->Reply(r);
    } else if (name == "query.shared") {
        // Phase 5: the shared game (what every group must agree on once the patches arrived) and the world clock.
        std::vector<uint8_t> snap = SharedGame::Snapshot();
        uint64_t h = 1469598103934665603ull;
        for (uint8_t b : snap) {
            h = (h ^ b) * 1099511628211ull;
        }
        int scene = cmd.value("scene", InPlay() ? (int)gPlayState->sceneNum : 0);
        json flags = json::object();
        if (scene >= 0 && scene < (int)ARRAY_COUNT(gSaveContext.sceneFlags)) {
            const SavedSceneFlags& f = gSaveContext.sceneFlags[scene];
            flags = { { "chest", f.chest }, { "swch", f.swch }, { "clear", f.clear }, { "collect", f.collect } };
            if (InPlay() && gPlayState->sceneNum == scene) {
                flags["live_chest"] = gPlayState->actorCtx.flags.chest;
                flags["live_swch"] = gPlayState->actorCtx.flags.swch;
                flags["live_clear"] = gPlayState->actorCtx.flags.clear;
                flags["live_collect"] = gPlayState->actorCtx.flags.collect;
            }
        }
        json ev = json::array();
        for (int i = 0; i < 14; i++) {
            ev.push_back(gSaveContext.eventChkInf[i]);
        }
        auto base = SharedGame::Baseline();
        uint64_t bh = 1469598103934665603ull;
        for (uint8_t b : base) {
            bh = (bh ^ b) * 1099511628211ull;
        }
        req->Reply(
            { { "ok", true },
              { "hash", Hex(h) },
              { "baseline_hash", base.empty() ? json(nullptr) : json(Hex(bh)) },
              { "size", snap.size() },
              { "layout", SharedGame::LayoutHash() },
              { "rupees", gSaveContext.rupees },
              { "health_capacity", gSaveContext.healthCapacity },
              { "gs_tokens", gSaveContext.inventory.gsTokens },
              { "items", json(std::vector<int>(gSaveContext.inventory.items, gSaveContext.inventory.items + 24)) },
              { "upgrades", gSaveContext.inventory.upgrades },
              { "quest_items", gSaveContext.inventory.questItems },
              { "event_chk_inf", ev },
              { "scene", scene },
              { "scene_flags", flags },
              { "day_time", gSaveContext.dayTime },
              { "night", gSaveContext.nightFlag },
              { "time_speed", (int)gTimeSpeed } });
    } else if (name == "query.transitions") {
        // Phase 4: the scene's transition actors (doors and loading planes between rooms).
        json list = json::array();
        if (InPlay()) {
            const TransitionActorContext& t = gPlayState->transiActorCtx;
            for (int i = 0; i < t.numActors; i++) {
                const TransitionActorEntry& e = t.list[i];
                list.push_back({ { "index", i },
                                 { "id", e.id < 0 ? -e.id : e.id },
                                 { "spawned", e.id < 0 },
                                 { "front_room", e.sides[0].room },
                                 { "back_room", e.sides[1].room },
                                 { "pos", json::array({ e.pos.x, e.pos.y, e.pos.z }) },
                                 { "rot_y", e.rotY },
                                 { "params", e.params } });
            }
        }
        req->Reply({ { "ok", true }, { "tick", sFrame }, { "transitions", list } });
    } else if (name == "query.hash") {
        auto st = Sim::GetStatus();
        uint32_t t = cmd.contains("tick") ? cmd["tick"].get<uint32_t>() : (st.tick > 0 ? st.tick - 1 : 0);
        uint64_t h = 0;
        if (!Sim::HashAt(t, &h)) {
            req->Reply({ { "ok", false },
                         { "error", "no hash recorded for tick " + std::to_string(t) },
                         { "sim_tick", st.tick } });
        } else {
            json resp = { { "ok", true }, { "tick", t }, { "hash", Hex(h) }, { "sim_tick", st.tick } };
            uint64_t ref = 0;
            if (Sim::ReferenceHashAt(t, &ref)) {
                resp["ref"] = Hex(ref);
                resp["match"] = ref == h;
            }
            req->Reply(resp);
        }
    } else if (name == "query.hashes") {
        // Recorded hashes of ticks [from, to), for bulk comparison between instances.
        uint32_t from = cmd.value("from", 0u);
        uint32_t to = cmd.value("to", Sim::CurrentTick());
        json list = json::array();
        uint32_t first = UINT32_MAX;
        for (uint32_t t = from; t < to; t++) {
            uint64_t h = 0;
            if (Sim::HashAt(t, &h)) {
                if (first == UINT32_MAX) {
                    first = t;
                }
                list.push_back(Hex(h));
            } else if (first != UINT32_MAX) {
                break;
            }
        }
        req->Reply(
            { { "ok", true }, { "from", first == UINT32_MAX ? json(nullptr) : json(first) }, { "hashes", list } });
    } else if (name == "record.start") {
        Sim::StartSpec spec;
        spec.entrance = cmd.value("entrance", spec.entrance);
        spec.adult = cmd.value("age", std::string("child")) == "adult";
        spec.dayTime = (uint16_t)cmd.value("day_time", (int)spec.dayTime);
        spec.seed = cmd.value("seed", spec.seed);
        std::string err;
        std::string path = std::filesystem::absolute(cmd.value("path", std::string("recording.zmpinput"))).string();
        if (Sim::StartRecording(path, spec, cmd.value("from_state", false), &err)) {
            req->Reply({ { "ok", true }, { "session", SessionJson() } });
        } else {
            req->Reply({ { "ok", false }, { "error", err } });
        }
    } else if (name == "record.stop") {
        std::string err;
        uint32_t ticks = 0;
        bool ok = Sim::StopRecording(&err, &ticks);
        req->Reply({ { "ok", ok }, { "ticks", ticks }, { "error", err } });
    } else if (name == "record.event") {
        std::string err;
        uint32_t t = 0;
        bool ok = Sim::RecordEvent(cmd.value("line", std::string()), &err, &t);
        req->Reply({ { "ok", ok }, { "sim_tick", t }, { "error", err } });
    } else if (name == "record.checkpoint" || name == "record.rewind") {
        std::string err;
        uint32_t t = 0;
        std::string cpName = cmd.value("name", std::string("default"));
        bool ok = name == "record.checkpoint" ? Sim::Checkpoint(cpName, &err, &t) : Sim::Rewind(cpName, &err, &t);
        req->Reply({ { "ok", ok }, { "sim_tick", t }, { "error", err } });
    } else if (name == "replay.start") {
        std::string err;
        if (cmd.contains("speed")) {
            Sim::SetSpeed(cmd["speed"].get<int>());
        }
        std::string path = std::filesystem::absolute(cmd.value("path", std::string())).string();
        std::string fromState = cmd.value("from_state", std::string());
        if (!fromState.empty()) {
            fromState = std::filesystem::absolute(fromState).string();
        }
        Sim::PauseOnActivate(cmd.value("pause", false));
        if (Sim::StartReplay(path, fromState, &err)) {
            req->Reply({ { "ok", true }, { "session", SessionJson() } });
        } else {
            req->Reply({ { "ok", false }, { "error", err } });
        }
    } else if (name == "replay.stop" || name == "session.stop") {
        Sim::StopSession();
        req->Reply({ { "ok", true }, { "session", SessionJson() } });
    } else if (name == "sim.pause") {
        Sim::SetPaused(true);
        req->Reply({ { "ok", true }, { "sim_tick", Sim::CurrentTick() } });
    } else if (name == "sim.resume") {
        Sim::SetPaused(false);
        req->Reply({ { "ok", true }, { "sim_tick", Sim::CurrentTick() } });
    } else if (name == "sim.run_until") {
        // Runs until `tick` ticks have been executed, then closes the tick gate.
        Sim::SetPauseAt(cmd.value("tick", 0u));
        req->Reply({ { "ok", true }, { "sim_tick", Sim::CurrentTick() } });
    } else if (name == "sim.speed") {
        Sim::SetSpeed(cmd.value("speed", 1));
        req->Reply({ { "ok", true } });
    } else if (name == "state.save" || name == "state.load") {
        std::string err, info;
        std::string path = std::filesystem::absolute(cmd.value("path", std::string("state.zmps"))).string();
        bool ok = name == "state.save" ? Sim::SaveStateFile(path, &err, &info) : Sim::LoadStateFile(path, &err, &info);
        req->Reply(
            { { "ok", ok }, { "path", path }, { "info", info }, { "error", err }, { "sim_tick", Sim::CurrentTick() } });
    } else if (name == "state.dump") {
        std::string err;
        std::string path = std::filesystem::absolute(cmd.value("path", std::string("state.txt"))).string();
        uint32_t t = Sim::CurrentTick() > 0 ? Sim::CurrentTick() - 1 : 0;
        bool ok = Sim::DumpState(path, t, &err);
        req->Reply({ { "ok", ok }, { "path", path }, { "tick", t }, { "error", err } });
    } else if (name == "state.census") {
        std::string summary;
        std::string path = std::filesystem::absolute(cmd.value("path", std::string("census.txt"))).string();
        bool ok = State::Census(path, &summary);
        req->Reply({ { "ok", ok }, { "path", path }, { "summary", summary } });
    } else if (name == "debug.dump_data") {
        // Desync hunting: raw copy of every writable section of the executable (its globals and file statics; the
        // image lives at a fixed address, D-016), to compare two instances at the same tick.
        std::string path = std::filesystem::absolute(cmd.value("path", std::string("data.bin"))).string();
        uint8_t* base = (uint8_t*)&__ImageBase;
        auto* dos = (IMAGE_DOS_HEADER*)base;
        auto* nt = (IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
        IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
        json sections = json::array();
        FILE* f = fopen(path.c_str(), "wb");
        bool ok = f != nullptr;
        for (int i = 0; ok && i < nt->FileHeader.NumberOfSections; i++) {
            if (!(sec[i].Characteristics & IMAGE_SCN_MEM_WRITE)) {
                continue;
            }
            uint32_t rva = sec[i].VirtualAddress;
            uint32_t size = sec[i].Misc.VirtualSize;
            fwrite(&rva, 4, 1, f);
            fwrite(&size, 4, 1, f);
            fwrite(base + rva, 1, size, f);
            char nm[9] = {};
            memcpy(nm, sec[i].Name, 8);
            sections.push_back({ { "name", nm }, { "rva", rva }, { "size", size } });
        }
        if (f != nullptr) {
            fclose(f);
        }
        // (where the parts that are not simulation live inside two big globals, for the test that classifies what
        // differs between instances)
        json layout = { { "save_rva", (uint64_t)((uint8_t*)&gSaveContext - base) },
                        { "save_size", sizeof(SaveContext) },
                        { "save_stats_off", offsetof(SaveContext, ship.stats) },
                        { "save_stats_size", sizeof(gSaveContext.ship.stats) },
                        { "sim_rva", (uint64_t)((uint8_t*)&gZmpSim - base) },
                        { "sim_size", sizeof(gZmpSim) },
                        { "sim_slots_off", offsetof(ZmpSimState, slots) },
                        { "sim_slot_size", sizeof(ZmpPlayerSlot) },
                        { "sim_slot_view_off", offsetof(ZmpPlayerSlot, hasView) },
                        { "sim_slot_target_off", offsetof(ZmpPlayerSlot, target) },
                        { "sim_slot_target_size", sizeof(TargetContext) },
                        { "sim_slot_camera_off", offsetof(ZmpPlayerSlot, camera) },
                        { "sim_slot_input_off", offsetof(ZmpPlayerSlot, input) },
                        // (the pad manager: what the state carries is `inputs`; controller presence and rumble
                        // are this PC's hardware)
                        { "pad_inputs_off", offsetof(PadMgr, inputs) },
                        { "pad_inputs_size", sizeof(gPadMgr.inputs) } };
        req->Reply(
            { { "ok", ok }, { "path", path }, { "sections", sections }, { "tick", sFrame }, { "layout", layout } });
    } else if (name == "debug.floor") {
        // Exploration tool (not simulation): floor heights under a list of points, [[x, y, z], ...].
        if (!InPlay()) {
            req->Reply({ { "ok", false }, { "error", "not in play" } });
        } else {
            json out = json::array();
            for (auto& pt : cmd.value("points", json::array())) {
                Vec3f pos = { pt[0].get<float>(), pt[1].get<float>(), pt[2].get<float>() };
                CollisionPoly* poly = nullptr;
                s32 bgId = 0;
                f32 y = BgCheck_EntityRaycastFloor3(&gPlayState->colCtx, &poly, &bgId, &pos);
                out.push_back(y);
            }
            req->Reply({ { "ok", true }, { "floors", out } });
        }
    } else if (name == "debug.teleport") {
        // Exploration tool: puts the player exactly at a position (never used in recordings).
        // In lockstep tests every instance applies the same teleport at the same paused tick (slot = player).
        Player* pl = nullptr;
        if (InPlay()) {
            pl = (Zmp_MultiActive() && cmd.contains("slot")) ? Zmp::Players::SlotPlayer(cmd["slot"].get<int>())
                                                             : GET_PLAYER(gPlayState);
        }
        if (pl == nullptr) {
            req->Reply({ { "ok", false }, { "error", "not in play or no such player" } });
        } else {
            Vec3f pos = { cmd["pos"][0].get<float>(), cmd["pos"][1].get<float>(), cmd["pos"][2].get<float>() };
            pl->actor.world.pos = pos;
            pl->actor.prevPos = pos;
            pl->actor.home.pos = pos;
            if (cmd.contains("yaw")) {
                pl->actor.shape.rot.y = pl->actor.world.rot.y = pl->yaw = (s16)cmd["yaw"].get<int>();
            }
            req->Reply({ { "ok", true } });
        }
    } else if (name == "debug.set_actor_health") {
        std::string err;
        bool ok =
            Sim::DebugSetActorHealth(cmd.value("category", 5), cmd.value("index", 0), cmd.value("health", 0), &err);
        req->Reply({ { "ok", ok }, { "error", err } });
    } else if (name == "debug.crash") {
        // A test instance that crashes on purpose (an invalid write on the game thread), to test what it leaves behind.
        req->Reply({ { "ok", true } });
        volatile int* nowhere = nullptr;
        *nowhere = 1;
    } else if (name == "debug.naming_stall") {
        // The game thread names statics for a while without starting a frame (finding X).
        int named = Zmp::State::TestNamingStall(cmd.value("ms", 7000));
        req->Reply({ { "ok", true }, { "named", named } });
    } else if (name == "debug.resource_race") {
        // Several threads load resources for the first time at once (finding T).
        ZmpResourceRace r = {};
        Zmp_TestResourceRace(cmd.value("threads", 4), cmd.value("made", 20000), &r);
        req->Reply({ { "ok", true },
                     { "loads", r.loads },
                     { "wrote", r.wrote },
                     { "made", r.made },
                     { "lost", r.lost },
                     { "phantom", r.phantom },
                     { "left", r.left },
                     { "held", r.held } });
    } else if (name == "debug.late_inputs") {
        // A machine whose input is late on purpose: it sends none for hold_ms, and its game stands still for
        // freeze_ms first (then it plays the ticks it already has, and reports their hashes, with no input sent yet).
        Zmp::Lockstep::TestHoldInputs(cmd.value("hold_ms", 0));
        std::this_thread::sleep_for(std::chrono::milliseconds(cmd.value("freeze_ms", 0)));
        req->Reply({ { "ok", true } });
    } else if (name == "debug.tick_view_lag") {
        // This machine decides its inputs as if the group's ticks reached it `ticks` ticks late (D-094).
        Zmp::Lockstep::TestTickViewLag(cmd.value("ticks", 0));
        req->Reply({ { "ok", true } });
    } else if (name == "screenshot") {
        std::string path = cmd.value("path", std::string("screenshot.png"));
        path = std::filesystem::absolute(path).string();
        PendingShot shot;
        shot.req = req;
        shot.path = path;
        int cw = cmd.value("width", CVarGetInteger(ZMP_CVAR_TEST_CAPTURE_W, 0));
        int ch = cmd.value("height", CVarGetInteger(ZMP_CVAR_TEST_CAPTURE_H, 0));
        // "tick": the picture of that simulation tick (several instances asked for the same tick show the same
        // moment, whatever the speed of the clock).
        shot.afterTick = cmd.value("tick", 0u);
        if (!sShots.empty()) {
            shot.frames = sShots.back().frames + 1; // (the window is already at the capture's size)
            sShots.push_back(shot);
        } else if (ResizeForCapture(cw, ch, &shot.restore)) {
            shot.frames = 4;
            sShots.push_back(shot);
        } else if (shot.afterTick != 0) {
            shot.frames = 1;
            sShots.push_back(shot);
        } else {
            TakeShot(req, path);
        }
    } else if (name == "wait.tick" || name == "wait.stable" || name == "wait.scene" || name == "wait.state" ||
               name == "wait.sim_tick" || name == "wait.replay_end") {
        Wait w;
        w.req = req;
        w.deadline = Deadline(cmd);
        if (name == "wait.tick") {
            w.kind = WaitKind::Tick;
            w.targetTick = cmd.value("tick", 0u);
        } else if (name == "wait.stable") {
            w.kind = WaitKind::Stable;
            w.targetTick = sFrame + cmd.value("frames", 1u);
        } else if (name == "wait.sim_tick") {
            w.kind = WaitKind::SimTick;
            w.targetTick = cmd.value("tick", 0u);
        } else if (name == "wait.replay_end") {
            w.kind = WaitKind::ReplayEnd;
        } else if (name == "wait.scene") {
            w.kind = WaitKind::Scene;
            w.scene = cmd.value("scene", -1);
        } else {
            w.kind = WaitKind::GameState;
            w.gameState = cmd.value("state", std::string());
        }
        sWaits.push_back(w);
    } else if (name == "game.new") {
        if (gGameState == nullptr) {
            req->Reply({ { "ok", false }, { "error", "no game state" } });
        } else {
            Sim::StartSpec spec;
            spec.entrance = cmd.value("entrance", 0x0EE);
            spec.adult = cmd.value("age", std::string("child")) == "adult";
            spec.dayTime = (uint16_t)cmd.value("day_time", 0x8000); // noon by default
            Sim::StartDebugGame(spec);
            req->Reply({ { "ok", true }, { "tick", sFrame } });
        }
    } else if (name == "cvar.get") {
        std::string var = cmd.value("name", std::string());
        auto cv = Ship::Context::GetRawInstance()->GetConsoleVariables()->Get(var.c_str());
        if (cv == nullptr) {
            req->Reply({ { "ok", true }, { "name", var }, { "value", nullptr } });
        } else if (cv->Type == Ship::ConsoleVariableType::Integer) {
            req->Reply({ { "ok", true }, { "name", var }, { "value", cv->Integer } });
        } else if (cv->Type == Ship::ConsoleVariableType::Float) {
            req->Reply({ { "ok", true }, { "name", var }, { "value", cv->Float } });
        } else if (cv->Type == Ship::ConsoleVariableType::String) {
            req->Reply({ { "ok", true }, { "name", var }, { "value", cv->String ? cv->String : "" } });
        } else {
            req->Reply({ { "ok", false }, { "error", "unsupported cvar type" } });
        }
    } else if (name == "cvar.set" && CVarProfile::Active() && CVarProfile::IsLocked(cmd.value("name", std::string()))) {
        req->Reply(
            { { "ok", false }, { "error", "locked CVar during a ZMP session: " + cmd.value("name", std::string()) } });
    } else if (name == "cvar.set") {
        std::string var = cmd.value("name", std::string());
        const json& v = cmd.contains("value") ? cmd["value"] : json();
        if (v.is_number_integer() || v.is_boolean()) {
            CVarSetInteger(var.c_str(), v.is_boolean() ? (v.get<bool>() ? 1 : 0) : v.get<int32_t>());
        } else if (v.is_number_float()) {
            CVarSetFloat(var.c_str(), v.get<float>());
        } else if (v.is_string()) {
            CVarSetString(var.c_str(), v.get<std::string>().c_str());
        } else {
            CVarClear(var.c_str());
        }
        req->Reply({ { "ok", true } });
    } else if (name == "net.status") {
        req->Reply(NetStatusJson());
    } else if (name == "net.connect") {
        std::string host = cmd.value("host", std::string(CVarGetString("gZmp.Server.Host", "127.0.0.1")));
        uint16_t port = (uint16_t)cmd.value("port", CVarGetInteger("gZmp.Server.Port", 47100));
        std::string room = cmd.value("room", std::string(CVarGetString("gZmp.Room", "zmp")));
        std::string pname = cmd.value("name", std::string(CVarGetString("gZmp.Name", "Player")));
        Zmp::Client::Get().Connect(host, port, room, pname);
        req->Reply({ { "ok", true } });
    } else if (name == "net.event") {
        Zmp::Lockstep::SendConsoleEvent(cmd.value("line", std::string()));
        req->Reply({ { "ok", true } });
    } else if (name == "net.leave") {
        Zmp::Lockstep::Leave();
        req->Reply({ { "ok", true } });
    } else if (name == "net.disconnect") {
        Zmp::Client::Get().Disconnect();
        req->Reply({ { "ok", true } });
    } else if (name == "quit") {
        sQuitRequested = true;
        req->Reply({ { "ok", true }, { "tick", sFrame } });
    } else {
        req->Reply({ { "ok", false }, { "error", "unknown command '" + name + "'" } });
    }
}

static void UpdateWaits() {
    auto now = std::chrono::steady_clock::now();
    for (auto it = sWaits.begin(); it != sWaits.end();) {
        bool done = false;
        switch (it->kind) {
            case WaitKind::Tick:
            case WaitKind::Stable:
                done = sFrame >= it->targetTick;
                break;
            case WaitKind::Scene:
                // Require the condition on two consecutive frames so the scene has settled.
                it->satisfiedFrames = SceneReady(it->scene) ? it->satisfiedFrames + 1 : 0;
                done = it->satisfiedFrames >= 2;
                break;
            case WaitKind::GameState:
                it->satisfiedFrames = GameStateName() == it->gameState ? it->satisfiedFrames + 1 : 0;
                done = it->satisfiedFrames >= 2;
                break;
            case WaitKind::SimTick:
                done = Sim::CurrentTick() >= it->targetTick;
                break;
            case WaitKind::ReplayEnd: {
                auto st = Sim::GetStatus();
                done = st.mode != Sim::Mode::Replaying || st.replayFinished;
                break;
            }
        }
        if (done) {
            it->req->Reply({ { "ok", true }, { "tick", sFrame }, { "sim_tick", Sim::CurrentTick() } });
            it = sWaits.erase(it);
        } else if (now > it->deadline) {
            it->req->Reply({ { "ok", false },
                             { "error", "timeout" },
                             { "tick", sFrame },
                             { "state", GameStateName() },
                             { "scene", InPlay() ? gPlayState->sceneNum : -1 } });
            it = sWaits.erase(it);
        } else {
            ++it;
        }
    }
}

static void UpdateShots() {
    for (auto it = sShots.begin(); it != sShots.end();) {
        if (--it->frames > 0 || (it->afterTick != 0 && Sim::CurrentTick() <= it->afterTick)) {
            ++it;
            continue;
        }
        TakeShot(it->req, it->path);
        CaptureRestore restore = it->restore;
        it = sShots.erase(it);
        if (sShots.empty()) {
            RestoreAfterCapture(restore);
        } else if (restore.valid) {
            sShots.front().restore = restore; // (whoever is last puts the window back)
        }
    }
}

void OnFrameBegin(uint32_t tick) {
    UpdateShots();
    // Wall time between two RunFrame iterations (one logic tick each): mean (EMA) and worst since the last query.
    {
        static std::chrono::steady_clock::time_point last;
        auto now = std::chrono::steady_clock::now();
        if (last.time_since_epoch().count() != 0) {
            float ms = std::chrono::duration<float, std::milli>(now - last).count();
            sFrameIntervalMs = sFrameIntervalMs == 0.0f ? ms : sFrameIntervalMs * 0.98f + ms * 0.02f;
            sFrameIntervalMaxMs = std::max(sFrameIntervalMaxMs, ms);
            if (ms > 90.0f && sFrame > 0) {
                // (diagnosis of pauses, phase 5b: what was going on is in the lines around this one)
                Zmp::Log("harness: slow frame, " + std::to_string((int)ms) + " ms before tick " +
                         std::to_string(Sim::CurrentTick()));
            }
        }
        last = now;
        uint32_t simTick = Sim::CurrentTick();
        // Only the ticks of a player who is playing count: not its own scene changes (its fade, its catch-up when it
        // enters a group), which are its own and hold nobody else.
        {
            auto lst = Zmp::Lockstep::GetStatus();
            bool playing = lst.phase != Zmp::Lockstep::Phase::Running ||
                           (!lst.covered && !lst.catchingUp && Zmp::Players::IsPresent(lst.slot));
            if (lst.phase != Zmp::Lockstep::Phase::Running && lst.phase != Zmp::Lockstep::Phase::Idle) {
                playing = false; // detached / joining
            }
            sGapSettled = playing ? std::min(sGapSettled + 1, 1000) : 0;
            if (sGapSettled < 20) {
                sGapValid = false;
            }
        }
        {
            float waited =
                (float)Zmp::Lockstep::GateWaitMs(); // (read every frame: waits of its own scene changes are dropped)
            if (sGapSettled >= 20) {
                sGateWaitMaxMs = std::max(sGateWaitMaxMs, waited);
            }
        }
        if (sGapSettled < 20) {
        } else if (!sGapValid || simTick < sGapLastTick) {
            sGapValid = true;
            sGapLastTick = simTick;
            sGapLastAt = now;
        } else if (simTick != sGapLastTick) {
            // (a client catching up runs several ticks per frame: each gap is the frame's time shared out)
            float ms = std::chrono::duration<float, std::milli>(now - sGapLastAt).count();
            if (sTickGaps.size() < 400000) {
                sTickGaps.push_back(ms);
            }
            sGapLastTick = simTick;
            sGapLastAt = now;
        }
    }
    sFrame = tick;
    for (auto& req : TakeRequests()) {
        try {
            Dispatch(req);
        } catch (const std::exception& e) {
            req->Reply({ { "ok", false }, { "error", std::string("exception: ") + e.what() } });
        }
    }
    UpdateWaits();
    if (sQuitRequested) {
        sQuitRequested = false;
        Log("harness: quit requested");
        Ship::Context::GetRawInstance()->GetWindow()->Close();
    }
}

void ApplyInput(void* padsV) {
    ApplyScriptInput((OSContPad*)padsV);
}

void ScriptStepRead() {
    if (sScriptActive) {
        sStepRead = true;
    }
}

bool ScriptPadForSend(uint32_t* buttons, int8_t* stickX, int8_t* stickY, int8_t* rStickX, int8_t* rStickY) {
    if (!sScriptActive || !ScriptBySend() || !ScriptGoesToTheGroup()) {
        return false;
    }
    const PadStep& step = sScript.front();
    *buttons = step.buttons;
    *stickX = step.stickX;
    *stickY = step.stickY;
    *rStickX = step.rStickX;
    *rStickY = step.rStickY;
    AdvanceScript();
    return true;
}

} // namespace Zmp::Harness

#endif
