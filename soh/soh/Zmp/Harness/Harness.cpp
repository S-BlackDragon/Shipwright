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
#include <ship/Context.h>
#include <ship/window/Window.h>
#include <libultraship/bridge/consolevariablebridge.h>
#include <libultraship/libultra/controller.h>

#include "soh/Zmp/ZmpLog.h"
#include "soh/Zmp/Net/ZmpClient.h"
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
void Sram_InitDebugSave(void);
}

using json = nlohmann::json;

namespace Zmp::Harness {

namespace {

struct PadStep {
    uint32_t buttons = 0;
    int8_t stickX = 0;
    int8_t stickY = 0;
    int8_t rStickX = 0;
    int8_t rStickY = 0;
    int32_t frames = 1; // <= 0 means hold until input.release
};

enum class WaitKind { Tick, Stable, Scene, GameState };

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
void ApplyInput(OSContPad* pads) {
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

json PlayerJson(Player* player) {
    Actor* a = &player->actor;
    json eq = json::object();
    eq["button_items"] = json::array();
    for (int i = 0; i < 8; i++) {
        eq["button_items"].push_back(gSaveContext.equips.buttonItems[i]);
    }
    eq["equipment"] = gSaveContext.equips.equipment;
    return {
        { "ok", true },
        { "index", 0 },
        { "pos", Vec3(a->world.pos) },
        { "rot", Rot3(a->world.rot) },
        { "shape_rot", Rot3(a->shape.rot) },
        { "yaw", player->yaw },
        { "speed", player->linearVelocity },
        { "health", gSaveContext.health },
        { "health_capacity", gSaveContext.healthCapacity },
        { "magic", gSaveContext.magic },
        { "rupees", gSaveContext.rupees },
        { "equips", eq },
        { "state_flags", json::array({ player->stateFlags1, player->stateFlags2, player->stateFlags3 }) },
        { "room", a->room },
        { "cur_room", gPlayState->roomCtx.curRoom.num },
        { "age", gSaveContext.linkAge == LINK_AGE_CHILD ? "child" : "adult" },
        { "downed", false },
        { "spectating", nullptr },
        { "tick", sFrame },
    };
}

json ActorsJson(const json& cmd) {
    int onlyCat = cmd.value("category", -1);
    int onlyId = cmd.value("id", -1);
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
            });
        }
    }
    return { { "ok", true }, { "tick", sFrame }, { "count", list.size() }, { "actors", list } };
}

// Same steps as the map select (z_select.c Select_LoadGame) with the debug save.
bool StartDebugGame(int entrance, bool adult, uint16_t dayTime, std::string* err) {
    if (gGameState == nullptr) {
        *err = "no game state";
        return false;
    }
    gSaveContext.fileNum = 0xFF;
    Sram_InitDebugSave();
    gSaveContext.magicFillTarget = gSaveContext.magic;
    gSaveContext.magic = 0;
    gSaveContext.magicCapacity = 0;
    gSaveContext.magicLevel = gSaveContext.magic;
    gSaveContext.linkAge = adult ? LINK_AGE_ADULT : LINK_AGE_CHILD;
    gSaveContext.dayTime = dayTime;
    gSaveContext.nightFlag = (dayTime >= 0xC001 || dayTime < 0x4555) ? 1 : 0;
    GameInteractor_ExecuteOnLoadGame(gSaveContext.fileNum);
    for (int i = 0; i < ARRAY_COUNT(gSaveContext.buttonStatus); i++) {
        gSaveContext.buttonStatus[i] = BTN_ENABLED;
    }
    gSaveContext.forceRisingButtonAlphas = 0;
    gSaveContext.nextHudVisibilityMode = 0;
    gSaveContext.hudVisibilityMode = 0;
    gSaveContext.hudVisibilityModeTimer = 0;
    gSaveContext.entranceIndex = entrance;
    gSaveContext.cutsceneIndex = 0;
    gSaveContext.respawnFlag = 0;
    gSaveContext.respawn[RESPAWN_MODE_DOWN].entranceIndex = ENTR_LOAD_OPENING;
    gSaveContext.seqId = (u8)NA_BGM_DISABLED;
    gSaveContext.natureAmbienceId = 0xFF;
    gSaveContext.showTitleCard = true;
    gSaveContext.gameMode = GAMEMODE_NORMAL;
    gWeatherMode = 0;
    gGameState->running = false;
    SET_NEXT_GAMESTATE(gGameState, Play_Init, PlayState);
    return true;
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

json NetStatusJson() {
    auto st = Zmp::Client::Get().GetStatus();
    json players = json::array();
    for (auto& p : st.players) {
        players.push_back({ { "id", p.id }, { "name", p.name } });
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
             { "cvar_profile_hash", hs.cvarProfileHash } };
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
        } else if (cmd.value("index", 0) != 0) {
            req->Reply({ { "ok", false }, { "error", "only player 0 exists in phase 0" } });
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
        req->Reply({ { "ok", true },
                     { "tick", sFrame },
                     { "state", GameStateName() },
                     { "scene", InPlay() ? gPlayState->sceneNum : -1 },
                     { "room", InPlay() ? gPlayState->roomCtx.curRoom.num : -1 },
                     { "day_time", gSaveContext.dayTime },
                     { "script_active", sScriptActive },
                     { "net", Zmp::Client::StateName(Zmp::Client::Get().GetStatus().state) } });
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
    } else if (name == "wait.tick" || name == "wait.stable" || name == "wait.scene" || name == "wait.state") {
        Wait w;
        w.req = req;
        w.deadline = Deadline(cmd);
        if (name == "wait.tick") {
            w.kind = WaitKind::Tick;
            w.targetTick = cmd.value("tick", 0u);
        } else if (name == "wait.stable") {
            w.kind = WaitKind::Stable;
            w.targetTick = sFrame + cmd.value("frames", 1u);
        } else if (name == "wait.scene") {
            w.kind = WaitKind::Scene;
            w.scene = cmd.value("scene", -1);
        } else {
            w.kind = WaitKind::GameState;
            w.gameState = cmd.value("state", std::string());
        }
        sWaits.push_back(w);
    } else if (name == "game.new") {
        std::string err;
        int entrance = cmd.value("entrance", 0x0EE);
        bool adult = cmd.value("age", std::string("child")) == "adult";
        uint16_t dayTime = (uint16_t)cmd.value("day_time", 0x8000); // noon by default
        if (StartDebugGame(entrance, adult, dayTime, &err)) {
            req->Reply({ { "ok", true }, { "tick", sFrame } });
        } else {
            req->Reply({ { "ok", false }, { "error", err } });
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
        }
        if (done) {
            it->req->Reply({ { "ok", true }, { "tick", sFrame } });
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

void Frame(void* padsV, uint32_t frame) {
    sFrame = frame;
    for (auto& req : TakeRequests()) {
        try {
            Dispatch(req);
        } catch (const std::exception& e) {
            req->Reply({ { "ok", false }, { "error", std::string("exception: ") + e.what() } });
        }
    }
    UpdateWaits();
    ApplyInput((OSContPad*)padsV);
    if (sQuitRequested) {
        sQuitRequested = false;
        Log("harness: quit requested");
        Ship::Context::GetRawInstance()->GetWindow()->Close();
    }
}

} // namespace Zmp::Harness

#endif
