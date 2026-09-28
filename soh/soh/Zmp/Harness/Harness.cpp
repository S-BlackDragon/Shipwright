#ifdef ZMP_HARNESS

// Game-thread side of the harness (PLAN.md 5.2): executes commands between frames, answers
// waits, and overrides the port 0 pad right after the physical read.

#include "Harness.h"
#include "HarnessQueue.h"
#include "Screenshot.h"

#include <chrono>
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
#include "soh/Zmp/State/StateBlob.h"
#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/Enhancements/game-interactor/GameInteractor_Hooks.h"

extern "C" {
#include <z64.h>
#include "variables.h"
#include "functions.h"
#include "macros.h"
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
bool sScriptActive = false;
RequestPtr sScriptReq; // answered when the script ends (null for input.set)
uint32_t sInjectedFrames = 0;
std::vector<Wait> sWaits;
bool sQuitRequested = false;

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
    if (!sScriptActive && sScriptReq) {
        EndScript(true);
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
    sInjectedFrames++;
    if (step.frames <= 0) {
        return; // hold
    }
    if (--sStepLeft <= 0) {
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
        { "action_offset", (uint64_t)((uintptr_t)player->actionFunc - (uintptr_t)&__ImageBase) },
        { "bg_flags", a->bgCheckFlags },
        { "floor_y", a->floorHeight },
        { "spectating", spectate >= 0 ? json(spectate) : json(nullptr) },
        { "pause_local", Zmp::Pause::State() },
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
                { "health", a->colChkInfo.health },
                { "room", a->room },
                { "freeze_timer", a->freezeTimer },
                { "proj", Vec3(a->projectedPos) },
            });
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
        players.push_back(
            { { "slot", p.slot }, { "name", p.name }, { "state", p.state }, { "rtt", p.rtt }, { "leader", p.leader } });
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
             { "multi", Zmp_MultiActive() != 0 },
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
             { "oot_hash", hs.ootHash },
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
                      { "net", Zmp::Client::StateName(Zmp::Client::Get().GetStatus().state) },
                      { "lockstep", LockstepJson() },
                      { "fps", ImGui::GetCurrentContext() != nullptr ? ImGui::GetIO().Framerate : 0.0f },
                      { "frame_interval_ms", sFrameIntervalMs },
                      { "frame_interval_max_ms", sFrameIntervalMaxMs } };
        if (st.hasHash) {
            resp["hash"] = Hex(st.lastHash);
            resp["hash_tick"] = st.tick - 1;
        }
        req->Reply(resp);
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
        req->Reply({ { "ok", ok }, { "path", path }, { "sections", sections }, { "tick", sFrame } });
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
    } else if (name == "screenshot") {
        std::string path = cmd.value("path", std::string("screenshot.png"));
        path = std::filesystem::absolute(path).string();
        int w = 0, h = 0;
        std::string info;
        if (CaptureGameWindow(path, &w, &h, &info)) {
            req->Reply({ { "ok", true }, { "path", path }, { "width", w }, { "height", h }, { "method", info } });
        } else {
            req->Reply({ { "ok", false }, { "error", info } });
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

void OnFrameBegin(uint32_t tick) {
    // Wall time between two RunFrame iterations (one logic tick each): mean (EMA) and worst since the last query.
    {
        static std::chrono::steady_clock::time_point last;
        auto now = std::chrono::steady_clock::now();
        if (last.time_since_epoch().count() != 0) {
            float ms = std::chrono::duration<float, std::milli>(now - last).count();
            sFrameIntervalMs = sFrameIntervalMs == 0.0f ? ms : sFrameIntervalMs * 0.98f + ms * 0.02f;
            sFrameIntervalMaxMs = std::max(sFrameIntervalMaxMs, ms);
        }
        last = now;
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

} // namespace Zmp::Harness

#endif
