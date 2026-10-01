#include "Lockstep.h"

#include <chrono>
#include <cinttypes>
#include <cstring>
#include <deque>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>

#include <libultraship/bridge/consolevariablebridge.h>
#include <libultraship/libultra/controller.h>

#include "ZmpClient.h"
#include "soh/Zmp/ZmpCVars.h"
#include <ship/Context.h>
#include <ship/debug/Console.h>
#include "soh/Zmp/ZmpLog.h"
#include "soh/Zmp/Sim/Session.h"
#include "soh/Zmp/Sim/ZmpPlayers.h"
#include "soh/Zmp/State/StateBlob.h"
#include "soh/Enhancements/nametag.h"
#include "soh/util.h"
#include "Autosave.h"
#include "SharedGame.h"

extern "C" {
#include <z64.h>
#include "variables.h"
#include "functions.h"
#include "macros.h"
extern PlayState* gPlayState;
extern u16 gTimeSpeed;
void FileChoose_Main(GameState* thisx);
void Play_Main(GameState* thisx);
void Play_PerformSave(PlayState* play);
}

namespace Zmp::Lockstep {

namespace {

using json = nlohmann::json;
using Clock = std::chrono::steady_clock;

struct Event {
    std::string kind;
    int slot = -1;
    std::string cmd;
    std::vector<uint8_t> data; // SHARED: a patch of the shared game from another group
};

struct Bundle {
    uint32_t tick = 0;
    std::vector<std::pair<int, Sim::PadRecord>> pads;
    std::vector<Event> events;
    int delay = 2;
    int32_t dayTime = -1; // world clock (phase 5), -1 none
};

struct BlobRequest {
    uint32_t tick;
    int forSlot;
    uint32_t groupId;
};

// ---- shared with the network thread (sMutex)
std::mutex sMutex;
std::map<uint32_t, Bundle> sBundles;
std::deque<json> sControl;
bool sConnectedEvent = false;
bool sDisconnectedEvent = false;
bool sRejoin = false;
std::vector<SlotInfo> sPlayers;
int sServerDelay = 2;
uint32_t sGroupTick = 0;
std::string sWaitingFor;
uint32_t sWaitTick = 0;
// Phase 5: last scene seen per player name, and the "X ha entrado en Y" notices.
std::map<std::string, int> sLastScenes;
struct Notice {
    std::string name;
    int scene;
    std::chrono::steady_clock::time_point at;
};
std::deque<Notice> sNotices;

// ---- game thread
Phase sPhase = Phase::Idle;
bool sConnected = false;
bool sJoinSent = false;
bool sJoinInPlaySent = false;
int sSlot = -1;
uint32_t sGroupId = 0;
uint32_t sJoinTick = 0;
bool sJoinIsRejoin = false;
uint32_t sNextInputTick = 0;
int sDelay = 2;
std::vector<BlobRequest> sBlobRequests;
std::vector<uint8_t> sPendingBlob;
uint32_t sPendingBlobTick = 0;
bool sHavePendingBlob = false;
bool sResyncHold = false;
uint32_t sResyncTick = 0;
int sLoadFrames = 0;
bool sStartedGameForLoad = false;
bool sWaitingNow = false;
Clock::time_point sWaitStart;
uint32_t sStalls = 0;
uint32_t sMaxStallMs = 0;
uint32_t sGateWaitMax = 0; // longest wait at the tick gate since GateWaitMs() was last read
uint32_t sResyncs = 0;
uint32_t sResyncsSeen = 0;
uint32_t sLastResyncTick = 0;
bool sLeader = false;
uint32_t sDumpTicks[4] = { UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX };
uint32_t sTickDumpTicks[64] = {};
Sim::PadRecord sLastLocal; // last pad read from the local controller (or the harness)
bool sLocalInputBlocked = false;
std::string sLastError;
std::map<int, Player*> sTagged;
// Per-player blocks saved with the leader's game (name -> zmp_block payload after the slot), loaded when this
// client founds the group; sent as an event when that player appears.
std::map<std::string, std::string> sSavedBlocks;
std::map<int, std::string> sTagNames;
// Autosave (phase 4, D-054): forced by a lockstep event; players of a resumed session (name -> their saved block,
// health, position and room), sent as events when they appear.
bool sForceAutosave = false;
std::map<std::string, nlohmann::json> sResumePlayers;
// Group game over (D-058): the anchor answered "continue? no"; the session ends at the file select with a notice.
bool sGameOverQuit = false;
std::string sGameOverQuitBy;
bool sGameOverQuitMine = false;
std::string sEndNotice;
std::chrono::steady_clock::time_point sEndNoticeAt;
std::chrono::steady_clock::time_point sSaveSkippedAt;
bool sSaveSkipped = false;
// Phase 5: groups per scene.
bool sFreeRun = false;       // detached, or joining after a detach: the local game ticks on its own
int sDetachScene = -1;       // scene this client heads to
int sDetachEntrance = -1;    // entrance it arrives by
uint32_t sDetachInits = 0;   // Play inits counted when it left (arrived = a new scene loaded since)
bool sDetachArrived = false; // already in the destination (regroup)
bool sDetachWaiting = false; // the server has no group there yet: found it once arrived
// Phase 5b: arriving with a scripted cutscene of this player's own (blue warp): it plays in a group of its own, and
// when the cutscene is over the client opens the group (the server then sends it to the others of its scene).
bool sDetachSolo = false;
bool sSoloGroup = false;
int sSoloIdleFrames = 0;
Clock::time_point sDetachAt;      // when it left (time to enter the next group)
int sGroupScene = -1;             // scene of this client's group (reported to the server when the group moves)
std::vector<uint8_t> sSentShared; // shared game sent with the last JOIN_GROUP in play
bool sOwnsSave = false;           // this PC writes the save file (founded the room's game with its own save)
bool sOwnershipKnown = false;
int sLastRate = -1; // last world clock rate reported
uint32_t sSharedSent = 0;
uint32_t sSharedApplied = 0;
std::string sLastShared;
uint32_t sGroupJoins = 0;
int sLastJoinMs = -1;
// Phase 5b: from leaving the scene (or asking to enter) until this player's Link is in the group.
std::chrono::steady_clock::time_point sInputAt; // when the next input is due (one per 50 ms)
uint32_t sNextServerTick = 0;                   // the tick after the newest one received from the server (0: none yet)
int sLastSpawnMs = -1;
uint32_t sRelocatedLoads = 0;
uint32_t sLastRelocated = 0;
uint32_t sLastRelocatedOdd = 0;
std::chrono::steady_clock::time_point sJoinStart;
bool sJoinStartValid = false;

bool InPlay() {
    return gGameState != nullptr && gGameState->main == Play_Main && gPlayState != nullptr;
}

// A game a group can be founded on: Play in normal mode (not the title screen's attract demo, not the
// credits).
bool InRealGame() {
    return InPlay() && gSaveContext.gameMode == GAMEMODE_NORMAL;
}

bool InFileSelect() {
    return gGameState != nullptr && gGameState->main == FileChoose_Main;
}

std::string BlocksPath(int fileNum) {
    return "Save/zmp-players-file" + std::to_string(fileNum + 1) + ".txt";
}

// "name<TAB>payload" lines, payload = what follows the slot in a zmp_block event.
void LoadSavedBlocks() {
    sSavedBlocks.clear();
    if (gSaveContext.fileNum < 0 || gSaveContext.fileNum >= 3) {
        return;
    }
    FILE* f = fopen(BlocksPath(gSaveContext.fileNum).c_str(), "r");
    if (f == nullptr) {
        return;
    }
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        std::string l(line);
        while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) {
            l.pop_back();
        }
        size_t tab = l.find('\t');
        if (tab != std::string::npos && tab > 0) {
            sSavedBlocks[l.substr(0, tab)] = l.substr(tab + 1);
        }
    }
    fclose(f);
    Log("net: " + std::to_string(sSavedBlocks.size()) + " saved player blocks for this game");
}

std::string BlockPayload(const ZmpPlayerBlock& b) {
    std::string p = std::to_string(b.magic);
    for (int i = 0; i < 8; i++) {
        p += " " + std::to_string(b.equips.buttonItems[i]);
    }
    for (int i = 0; i < 7; i++) {
        p += " " + std::to_string(b.equips.cButtonSlots[i]);
    }
    p += " " + std::to_string(b.equips.equipment);
    for (int i = 0; i < 16; i++) {
        p += " " + std::to_string(b.ammo[i]);
    }
    for (int i = 0; i < 4; i++) {
        p += " " + std::to_string(b.bottles[i]);
    }
    return p;
}

std::string Hex(uint64_t v) {
    char b[24];
    snprintf(b, sizeof(b), "%016" PRIX64, v);
    return b;
}

Sim::PadRecord PadFromBytes(const std::vector<uint8_t>& b) {
    Sim::PadRecord r;
    if (b.size() >= 8) {
        r.buttons = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
        r.stickX = (int8_t)b[4];
        r.stickY = (int8_t)b[5];
        r.rStickX = (int8_t)b[6];
        r.rStickY = (int8_t)b[7];
    }
    return r;
}

std::vector<uint8_t> PadToBytes(const Sim::PadRecord& r) {
    return { (uint8_t)(r.buttons & 0xFF),
             (uint8_t)((r.buttons >> 8) & 0xFF),
             (uint8_t)((r.buttons >> 16) & 0xFF),
             (uint8_t)((r.buttons >> 24) & 0xFF),
             (uint8_t)r.stickX,
             (uint8_t)r.stickY,
             (uint8_t)r.rStickX,
             (uint8_t)r.rStickY };
}

OSContPad ToPad(const Sim::PadRecord& r) {
    OSContPad p = {};
    p.button = (CONTROLLERBUTTONS_T)r.buttons;
    p.stick_x = r.stickX;
    p.stick_y = r.stickY;
    p.right_stick_x = r.rStickX;
    p.right_stick_y = r.rStickY;
    return p;
}

std::vector<uint8_t> BinaryOf(const json& j) {
    if (j.is_binary()) {
        return std::vector<uint8_t>(j.get_binary().begin(), j.get_binary().end());
    }
    if (j.is_array()) {
        std::vector<uint8_t> out;
        for (auto& x : j) {
            out.push_back((uint8_t)x.get<int>());
        }
        return out;
    }
    return {};
}

void Send(const json& msg) {
    Client::Get().Send(msg);
}

// Phase 5: the client of a detached player has arrived in the destination scene (a new scene finished loading).
bool DetachArrived() {
    if (!InRealGame() || gPlayState->transitionMode != TRANS_MODE_OFF ||
        gPlayState->transitionTrigger != TRANS_TRIGGER_OFF) {
        return false;
    }
    return sDetachArrived || Players::PlayInitCount() != sDetachInits;
}

// Phase 5b: the scene this client walked into exists (its Play was created); its fade-in may still be running. From
// here on the state of the scene's group can replace the local one (not before: D-064).
bool DetachLoaded() {
    return InRealGame() && (sDetachArrived || Players::PlayInitCount() != sDetachInits);
}

void SendJoin() {
    bool inPlay = InRealGame();
    int scene = inPlay ? (int)gPlayState->sceneNum : -1;
    int entrance = (int)gSaveContext.entranceIndex;
    bool place = inPlay; // a client already in a game (reconnection) appears where it is if that room is loaded
    bool solo = false;
    if (sPhase == Phase::Detached) {
        // Walking into another scene: join its group right away (or found it once arrived).
        solo = sDetachSolo;
        inPlay = DetachArrived();
        scene = inPlay ? (int)gPlayState->sceneNum : sDetachScene;
        entrance = sDetachEntrance;
        place = sDetachArrived;
    }
    json msg = { { "t", "JOIN_GROUP" }, { "scene", scene }, { "entrance", entrance }, { "in_play", inPlay } };
    if (InRealGame()) {
        // (not from the title screen's attract demo: that Link and its equipment are nobody's)
        msg["spawn"] = Players::LocalSpawnInfo(entrance, place);
    }
    if (inPlay) {
        sSentShared = SharedGame::Snapshot();
        msg["shared"] = json::binary(sSentShared);
        msg["shared_layout"] = SharedGame::LayoutHash();
        if (CVarGetInteger(ZMP_CVAR_OVERWRITE_ROOM_GAME, 0)) {
            msg["overwrite"] = true;
        }
    }
    if (solo && inPlay) {
        msg["solo"] = true;
    }
    std::string follow = CVarGetString(ZMP_CVAR_FOLLOW, "");
    if (!follow.empty() && sPhase != Phase::Detached) {
        msg["follow"] = follow;
    }
    Send(msg);
    sJoinSent = true;
    sJoinInPlaySent = inPlay;
    Log(std::string("net: JOIN_GROUP scene=") + std::to_string(scene) + " entrance=" + std::to_string(entrance) +
        " in_play=" + (inPlay ? "1" : "0") + (follow.empty() ? "" : " follow=" + follow));
}

void ResetGame(const char* why, bool keepTicks = false) {
    if (sPhase == Phase::Running || sPhase == Phase::Joining) {
        Log(std::string("net: leaving lockstep (") + why + ")");
    }
    if (Sim::GetStatus().mode == Sim::Mode::Net) {
        Sim::EndNet();
    }
    Players::Reset();
    Players::SetLocalSlot(-1);
    State::SetKeepSharedSettings(false);
    SharedGame::ClearBaseline();
    sPhase = Phase::Idle;
    sFreeRun = false;
    sDetachSolo = false;
    sSoloGroup = false;
    sBlobRequests.clear();
    sHavePendingBlob = false;
    sResyncHold = false;
    sWaitingNow = false;
    if (!keepTicks) {
        std::lock_guard<std::mutex> lock(sMutex);
        sBundles.clear();
    }
}

void StartRunning(uint32_t tick) {
    if (sFreeRun) {
        sLastJoinMs = (int)std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - sDetachAt).count();
        Log("net: in the next group " + std::to_string(sLastJoinMs) + " ms after leaving the scene");
    }
    sPhase = Phase::Running;
    sSoloGroup = sFreeRun && sDetachSolo;
    sSoloIdleFrames = 0;
    sDetachSolo = false;
    sFreeRun = false;
    sDetachWaiting = false;
    sLastRate = -1;
    sGroupJoins++;
    sGroupScene = InPlay() ? (int)gPlayState->sceneNum : -1;
    sLastLocal = Sim::PadRecord{};
    sWaitingNow = false;
    Sim::BeginNet(tick);
    State::SetKeepSharedSettings(true);
    std::lock_guard<std::mutex> lock(sMutex);
    for (auto it = sBundles.begin(); it != sBundles.end();) {
        it = it->first < tick ? sBundles.erase(it) : std::next(it);
    }
}

// ---------------------------------------------------------------------------------------------------
// Autosave and resume (phase 4, docs/DECISIONES.md D-054)

std::string RoomName() {
    return Client::Get().GetStatus().room;
}

// The normal save of the game, written from a copy: the save's own preparation (scene flags, saved scene, the B
// button restore) is done on the live save context and undone right after, so the simulation never sees it. The
// copy is written to disk by SoH's save thread; the other players' blocks go next to it (D-045).
void SaveOnCopy(void*) {
    SaveContext* keep = new SaveContext;
    memcpy(keep, &gSaveContext, sizeof(SaveContext));
    Play_PerformSave(gPlayState);
    memcpy(&gSaveContext, keep, sizeof(SaveContext));
    delete keep;
}

nlohmann::json SessionPlayers() {
    nlohmann::json players = nlohmann::json::array();
    for (int k = 0; k < ZMP_MAX_PLAYERS; k++) {
        Player* p = Players::SlotPlayer(k);
        if (p == nullptr) {
            continue;
        }
        nlohmann::json pos =
            nlohmann::json::array({ (int)p->actor.world.pos.x, (int)p->actor.world.pos.y, (int)p->actor.world.pos.z });
        players.push_back({ { "slot", k },
                            { "name", SlotName(k) },
                            { "health", Players::SlotHealth(k) },
                            { "pos", pos },
                            { "yaw", p->actor.shape.rot.y },
                            { "room", Players::SlotRoom(k) },
                            { "block", BlockPayload(Players::SlotBlock(k)) } });
    }
    return players;
}

// End of a tick of the leader: every IntervalTicks() of simulation (or when an event forces it) the game is saved
// normally and the exact state of the session is written for the resume.
// Phase 5: every player of the room is in this client's group (the exact session state describes everybody).
bool WholeRoomInGroup() {
    std::lock_guard<std::mutex> lock(sMutex);
    for (auto& p : sPlayers) {
        if (p.group != sGroupId && p.state != "disconnected") {
            return false;
        }
    }
    return true;
}

void MaybeAutosave(uint32_t tick, uint64_t hash) {
    uint32_t every = Autosave::IntervalTicks();
    bool due = sForceAutosave || (every > 0 && (tick + 1) % every == 0);
    sForceAutosave = false;
    // Phase 5: the save file is written by the PC of the player who started the room's game (whatever group it is in);
    // the exact session state by the leader of a group that has everybody.
    bool writeGame = sOwnsSave && gSaveContext.fileNum >= 0 && gSaveContext.fileNum < 3;
    bool writeSession = sLeader && WholeRoomInGroup();
    if (!due || (!writeGame && !writeSession) || !InPlay() || !Zmp_MultiActive() ||
        gPlayState->transitionMode != TRANS_MODE_OFF || gPlayState->gameOverCtx.state != GAMEOVER_INACTIVE) {
        return;
    }
    auto t0 = Clock::now();
    bool wroteGame = false;
    if (writeGame) {
        Players::RunInContext(sSlot, SaveOnCopy, nullptr);
        wroteGame = true;
    }
    std::vector<uint8_t> blob;
    std::string err;
    State::BlobInfo info;
    bool wroteSession = false;
    if (writeSession && State::Save(blob, tick + 1, hash, &err, &info)) {
        nlohmann::json desc = { { "version", 1 },
                                { "room", RoomName() },
                                { "tick", tick + 1 },
                                { "scene", gPlayState->sceneNum },
                                { "leader_slot", sSlot },
                                { "file_num", gSaveContext.fileNum },
                                { "players", SessionPlayers() } };
        wroteSession = Autosave::WriteSessionAsync(RoomName(), std::move(blob), std::move(desc));
    } else if (writeSession) {
        Log("autosave: session state not saved: " + err);
    }
    Autosave::NoteSaved();
    double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    Log("autosave at tick " + std::to_string(tick + 1) +
        (wroteGame ? ": game file " + std::to_string(gSaveContext.fileNum + 1) : std::string(": no game file")) +
        (wroteSession ? ", session state" : ", session state skipped") + " (" + std::to_string((int)ms) +
        " ms on the game thread)");
}

// Founding with the resume option: the founder loads the last session state of the room. The others' Links are
// taken out; each gets back its block, health, place and room when that player joins again (events of the leader).
bool TryResume(int slot) {
    if (!CVarGetInteger(ZMP_CVAR_RESUME_SESSION, 0)) {
        return false;
    }
    std::vector<uint8_t> blob;
    nlohmann::json desc;
    std::string err;
    if (!Autosave::ReadSession(RoomName(), blob, desc, &err)) {
        Log("resume: no session state for room " + RoomName() + " (" + err + "): normal game");
        return false;
    }
    if (desc.value("leader_slot", -1) != slot) {
        Log("resume: the session was saved by slot " + std::to_string(desc.value("leader_slot", -1)) +
            ", this founder is slot " + std::to_string(slot) + ": normal game");
        return false;
    }
    State::BlobInfo info;
    if (!State::Peek(blob, &info, &err)) {
        Log("resume: session state not usable (" + err + "): normal game");
        return false;
    }
    State::SetKeepSharedSettings(true);
    if (!State::Load(blob, &err, &info)) {
        Log("resume: session state not loaded (" + err + "): normal game");
        return false;
    }
    sResumePlayers.clear();
    for (auto& p : desc["players"]) {
        int k = p.value("slot", -1);
        if (k == slot || k < 0 || k >= ZMP_MAX_PLAYERS) {
            continue;
        }
        Players::Despawn(k);
        sResumePlayers[p.value("name", std::string())] = p;
    }
    Log("resume: session of room " + RoomName() + " loaded (tick " + std::to_string(info.tick) + ", scene " +
        std::to_string(info.scene) + ", " + std::to_string(sResumePlayers.size()) + " other players to come back)");
    return true;
}

void HandleControl(json& msg) {
    std::string t = msg.value("t", std::string());
    if (t == "GROUP_FOUND") {
        int slot = msg.value("slot", 0);
        sGroupId = msg.value("group_id", 0u);
        sDelay = msg.value("delay", 2);
        if (!InRealGame()) {
            sLastError = "grupo fundado sin partida cargada";
            Log("net: GROUP_FOUND but not in play; leaving");
            Send({ { "t", "LEAVE_GROUP" } });
            return;
        }
        // Phase 5: this client's shared game as the group and the server last agreed on it (kept across a scene
        // change or a lost connection), merged with the room's canonical game below.
        std::vector<uint8_t> oldBase = SharedGame::HasBaseline() ? SharedGame::Baseline() : std::vector<uint8_t>();
        bool resumed = TryResume(slot);
        if (!resumed) {
            Players::Found(slot);
        }
        Players::SetLocalSlot(slot);
        sSlot = slot;
        if (!resumed) {
            LoadSavedBlocks();
        } else {
            sSavedBlocks.clear();
        }
        if (!resumed && !oldBase.empty()) {
            SharedGame::SetBaseline(oldBase);
        }
        std::vector<uint8_t> canonical = msg.contains("shared") ? BinaryOf(msg["shared"]) : std::vector<uint8_t>();
        SharedGame::MergeOnFound(canonical, sSentShared);
        int day = msg.value("day_time", -1);
        if (day >= 0) {
            gSaveContext.dayTime = (u16)day; // the world clock
        }
        if (!sOwnershipKnown) {
            // The first time this client plays in the room it started the game with its own save: its PC writes it.
            sOwnsSave = true;
            sOwnershipKnown = true;
        }
        sNextInputTick = msg.value("tick0", 0u);
        StartRunning(sNextInputTick);
        Log("net: GROUP_FOUND group=" + std::to_string(sGroupId) + " slot=" + std::to_string(slot) +
            " scene=" + std::to_string(msg.value("scene", -1)) + " (this client founded the group" +
            (sOwnsSave ? ", its PC writes the save file)" : ")"));
    } else if (t == "GROUP_WAIT") {
        if (sPhase == Phase::Idle) {
            sPhase = Phase::WaitingGroup;
        } else if (sPhase == Phase::Detached) {
            sDetachWaiting = true; // nobody plays there: found the group once arrived
        }
        Log("net: GROUP_WAIT " + msg.value("reason", std::string()));
    } else if (t == "GROUP_JOIN_PENDING") {
        sSlot = msg.value("slot", -1);
        sGroupId = msg.value("group_id", 0u);
        sJoinTick = msg.value("spawn_tick", 0u);
        sJoinIsRejoin = msg.value("rejoin", false);
        sJoinStart = sPhase == Phase::Detached ? sDetachAt : Clock::now();
        sJoinStartValid = true;
        if (sPhase == Phase::Running) {
            // Rejoin after a reconnection: the local simulation is replaced by the leader's.
            // (the ticks already received are the new group's: it plays on while this client enters, phase 5b)
            ResetGame("rejoin", true);
        }
        bool fromDetach = sPhase == Phase::Detached;
        sPhase = Phase::Joining;
        sHavePendingBlob = false;
        sLoadFrames = 0;
        sStartedGameForLoad = false;
        if (!fromDetach) {
            // (a detached client keeps driving its own Link until the group's state arrives)
            Players::SetLocalSlot(sSlot);
        }
        if (!sOwnershipKnown) {
            sOwnsSave = false; // entered somebody else's game: its PC never writes a save file of this room
            sOwnershipKnown = true;
        }
        Log("net: GROUP_JOIN_PENDING slot=" + std::to_string(sSlot) + " T=" + std::to_string(sJoinTick) +
            (sJoinIsRejoin ? " (rejoin)" : "") + (fromDetach ? " (from another scene)" : ""));
    } else if (t == "REGROUP") {
        // Phase 5: this group moved into a scene where another group already plays: join that one.
        if (sPhase == Phase::Running && InPlay()) {
            Log("net: REGROUP into the group of scene " + std::to_string(msg.value("scene", -1)));
            Send({ { "t", "LEAVE_GROUP" } });
            Players::KeepOnlyLocal();
            sPhase = Phase::Detached;
            sFreeRun = true;
            sDetachScene = gPlayState->sceneNum;
            sDetachEntrance = gSaveContext.entranceIndex;
            sDetachInits = Players::PlayInitCount();
            sDetachArrived = true;
            sDetachWaiting = false;
            sDetachSolo = false;
            sSoloGroup = false;
            sDetachAt = Clock::now();
            sBlobRequests.clear();
            sResyncHold = false;
            sJoinSent = false;
        }
    } else if (t == "STATE_BLOB") {
        int forSlot = msg.value("for_slot", -1);
        if (forSlot != sSlot) {
            return;
        }
        sPendingBlob = BinaryOf(msg["data"]);
        sPendingBlobTick = msg.value("tick", 0u);
        sHavePendingBlob = true;
        sLoadFrames = 0;
        Log("net: STATE_BLOB for tick " + std::to_string(sPendingBlobTick) + " (" +
            std::to_string(sPendingBlob.size()) + " bytes)");
    } else if (t == "STATE_BLOB_REQUEST") {
        BlobRequest r{ msg.value("tick", 0u), msg.value("for_slot", -1), msg.value("group_id", 0u) };
        sBlobRequests.push_back(r);
        Log("net: STATE_BLOB_REQUEST tick " + std::to_string(r.tick) + " for slot " + std::to_string(r.forSlot));
    } else if (t == "RESYNC") {
        uint32_t bad = msg.value("bad_tick", 0u);
        uint32_t T = msg.value("tick", 0u);
        bool target = false;
        if (msg.contains("targets") && msg["targets"].is_array()) {
            for (auto& x : msg["targets"]) {
                if (x.get<int>() == sSlot) {
                    target = true;
                }
            }
        }
        sResyncsSeen++;
        sLastResyncTick = bad;
        // Phase 5b: only the player that differs and the leader it is compared with write their dumps (it costs a
        // frame); the others just go on.
        bool involved = target || sLeader;
        if (involved) {
            Sim::WriteDesyncDump(bad);
        }
        for (int i = 0; involved && i < 4; i++) {
            if (sDumpTicks[i] == bad) {
                std::error_code ec;
                std::filesystem::copy_file("logs/hashdump-" + std::to_string(i) + ".txt",
                                           "logs/desync-" + std::to_string(bad) + "-exact.txt",
                                           std::filesystem::copy_options::overwrite_existing, ec);
            }
        }
        if (involved && CVarGetInteger(ZMP_CVAR_DEBUG_TICK_DUMPS, 0)) {
            std::error_code ec;
            std::string dir = "logs/desync-" + std::to_string(bad) + "-ticks";
            std::filesystem::create_directories(dir, ec);
            for (int i = 0; i < 64; i++) {
                uint32_t t = sTickDumpTicks[i];
                if (t != 0 && t + 40 >= bad && t <= bad + 2) {
                    std::filesystem::copy_file("logs/tickdump-" + std::to_string(i) + ".txt",
                                               dir + "/" + std::to_string(t) + ".txt",
                                               std::filesystem::copy_options::overwrite_existing, ec);
                }
            }
        }
        Log("net: RESYNC bad_tick=" + std::to_string(bad) + " T=" + std::to_string(T) +
            (target ? " (this client reloads the leader's state)" : ""));
        if (target && sPhase == Phase::Running) {
            sResyncs++;
            sResyncHold = true;
            sResyncTick = T;
        }
    }
}

// Sends the local pad, one input per tick of real time (20 Hz), for the ticks the group has still to play. Runs every
// frame (also while the tick gate is closed). The pad is the last one read in a tick.
//
// Phase 5b ("nobody waits for anybody"): the input no longer follows the tick this client is simulating. A client
// that is behind (it just loaded the group's state, or its PC hiccuped) catches up by itself (CatchUpSpeed) and
// its input is on time meanwhile, so the group never has to wait for it. The pace is this client's clock, never the
// ticks that arrive (a group cannot feed itself ticks faster than real time); the server emits each tick when the
// last input for it arrives, so the group runs at the pace of its slowest clock, as before.
void SendPendingInputs() {
    if (sPhase != Phase::Running) {
        return;
    }
    uint32_t d = (uint32_t)std::max(1, sDelay);
    uint32_t next; // the tick after the newest one the server sent
    size_t queued;
    {
        std::lock_guard<std::mutex> lock(sMutex);
        next = sNextServerTick;
        queued = sBundles.size();
    }
    if (Zmp_MultiActive() && !Players::IsPresent(sSlot) && queued > d + 3) {
        // Entering a group that kept playing: this player's Link appears at the tick of its first input, so it sends
        // none until it has (nearly) caught up.
        return;
    }
    if (queued > d + 40) {
        // More than two seconds behind (a PC that cannot keep up): its player would act on what it saw long ago.
        // No input until it has caught up: its Link stands still and the group goes on.
        return;
    }
    auto now = Clock::now();
    if (sNextInputTick < next) {
        // The group played ticks without this client's input (it is entering, or its input was late and the server
        // went on): the ticks already played need none; the next one it can still be in time for is D ahead.
        sNextInputTick = next + d - 1;
        sInputAt = now;
    }
    if (now - sInputAt > std::chrono::seconds(1)) {
        sInputAt = now;
    }
    uint32_t cap = next + d + 3; // (while the server waits for somebody else, a few inputs ahead are enough)
    while (sNextInputTick <= cap && now >= sInputAt) {
        Send({ { "t", "INPUT" }, { "tick", sNextInputTick }, { "pad", json::binary(PadToBytes(sLastLocal)) } });
        sNextInputTick++;
        sInputAt += std::chrono::milliseconds(50);
    }
}

void ProcessBlobRequests() {
    if (sBlobRequests.empty() || sPhase != Phase::Running || !InPlay()) {
        return;
    }
    uint32_t tick = Sim::CurrentTick();
    for (auto it = sBlobRequests.begin(); it != sBlobRequests.end();) {
        if (it->tick > tick) {
            ++it;
            continue;
        }
        if (it->tick < tick) {
            Log("net: blob request for tick " + std::to_string(it->tick) + " arrived late (at " + std::to_string(tick) +
                "), sending the current state");
        }
        std::vector<uint8_t> blob;
        std::string err;
        State::BlobInfo info;
        auto st = Sim::GetStatus();
        if (CVarGetInteger(ZMP_CVAR_DEBUG_TICK_DUMPS, 0)) {
            std::string derr;
            Sim::DumpState("logs/blob-save-" + std::to_string(tick) + ".txt", tick - 1, &derr);
        }
        auto t0 = Clock::now();
        if (State::Save(blob, tick, st.lastHash, &err, &info)) {
            json msg = { { "t", "STATE_BLOB" },       { "group_id", it->groupId },    { "tick", tick },
                         { "for_slot", it->forSlot }, { "data", json::binary(blob) }, { "hash", st.lastHash } };
            Send(msg);
            double total = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
            Log("net: STATE_BLOB sent for slot " + std::to_string(it->forSlot) + " at tick " + std::to_string(tick) +
                " (" + std::to_string(blob.size()) + " bytes, " + std::to_string((int)info.ms) + " ms to save, " +
                std::to_string((int)total) + " ms in all on the game thread)");
        } else {
            Log("net: state save failed: " + err);
        }
        it = sBlobRequests.erase(it);
    }
}

void LoadPendingBlob() {
    if (!sHavePendingBlob) {
        return;
    }
    bool joining = sPhase == Phase::Joining;
    bool resync = sPhase == Phase::Running && sResyncHold;
    if (!joining && !resync) {
        sHavePendingBlob = false;
        return;
    }
    if (!InRealGame()) {
        // A joiner outside a game (or in the title screen's attract demo) first starts one, then loads the
        // leader's state on it.
        // (from the file select, the title screen or the intro: any game state can start one)
        if (!sStartedGameForLoad && gGameState != nullptr) {
            Sim::StartSpec plain;
            Sim::StartDebugGame(plain);
            sStartedGameForLoad = true;
        }
        sLoadFrames = 0;
        return;
    }
    if (joining && sFreeRun && !DetachLoaded()) {
        // Phase 5: on its way from another scene, the group's state is loaded once the local scene change finished
        // (loading it in the middle of leaving a pre-rendered room, a house, crashed the renderer).
        sLoadFrames = 0;
        return;
    }
    if (joining && ++sLoadFrames < (sFreeRun ? 2 : 5)) {
        return;
    }
    std::string err;
    State::BlobInfo info;
    State::SetKeepSharedSettings(true);
    if (!State::Load(sPendingBlob, &err, &info)) {
        sLastError = "no se pudo cargar el estado del grupo: " + err;
        Log("net: STATE_BLOB load failed: " + err);
        sHavePendingBlob = false;
        return;
    }
    sHavePendingBlob = false;
    if (info.relocated > 0 || info.relocatedOdd > 0) {
        sRelocatedLoads++;
        sLastRelocated = info.relocated;
        sLastRelocatedOdd = info.relocatedOdd;
        info.notes +=
            (info.notes.empty() ? "" : "; ") + std::to_string(info.relocated) + " pointers to the executable relocated";
    }
    {
        // The loaded state must be exactly the sender's at the end of the previous tick (same hash).
        uint64_t h = Sim::HashState(info.tick - 1);
        if (h != info.hash) {
            Log("net: state loaded for tick " + std::to_string(info.tick) + " hashes " + Hex(h) + ", the sender had " +
                Hex(info.hash) + " (the load differs)");
        }
        if (CVarGetInteger(ZMP_CVAR_DEBUG_TICK_DUMPS, 0)) {
            std::string err;
            Sim::DumpState("logs/blob-load-" + std::to_string(info.tick) + ".txt", info.tick - 1, &err);
        }
    }
    Players::SetLocalSlot(sSlot);
    if (joining) {
        sNextInputTick = sJoinTick;
        StartRunning(info.tick);
        Log("net: joined the group at tick " + std::to_string(info.tick) + " in " + std::to_string((int)info.ms) +
            " ms" + (info.notes.empty() ? "" : " | " + info.notes));
    } else {
        Sim::SetTick(info.tick);
        sResyncHold = false;
        Log("net: resynchronized at tick " + std::to_string(info.tick) + " in " + std::to_string((int)info.ms) + " ms" +
            (info.notes.empty() ? "" : " | " + info.notes));
        std::lock_guard<std::mutex> lock(sMutex);
        for (auto it = sBundles.begin(); it != sBundles.end();) {
            it = it->first < info.tick ? sBundles.erase(it) : std::next(it);
        }
    }
}

void UpdateNameTags() {
    if (!InPlay() || !Zmp_MultiActive()) {
        sTagged.clear();
        return;
    }
    // The local pause menu covers the world: no names on top of it (phase 3 pending item).
    if (Zmp::Pause::IsOpen()) {
        for (auto& [k, p] : sTagged) {
            if (Players::SlotPlayer(k) == p) {
                NameTag_RemoveAllForActor(&p->actor);
            }
        }
        sTagged.clear();
        return;
    }
    for (int k = 0; k < ZMP_MAX_PLAYERS; k++) {
        Player* p = Players::SlotPlayer(k);
        std::string name = SlotName(k);
        if (p == nullptr) {
            sTagged.erase(k);
            continue;
        }
        auto it = sTagged.find(k);
        if (it != sTagged.end() && it->second == p && sTagNames[k] == name) {
            continue;
        }
        if (it != sTagged.end() && it->second == p) {
            NameTag_RemoveAllForActor(&p->actor);
        }
        NameTagOptions opt = {};
        opt.tag = "zmp";
        opt.yOffset = 0;
        opt.textColor = k == sSlot ? Color_RGBA8{ 120, 255, 120, 255 } : Color_RGBA8{ 255, 255, 255, 255 };
        opt.noZBuffer = true;
        NameTag_RegisterForActorWithOptions(&p->actor, name.c_str(), opt);
        sTagged[k] = p;
        sTagNames[k] = name;
    }
}

struct ConsoleArg {
    const std::string* cmd;
};

void RunConsoleInContext(void* p) {
    std::string output;
    Ship::Context::GetRawInstance()->GetConsole()->Run(*((ConsoleArg*)p)->cmd, &output);
}

// Game events that travel as console events (the server stamps the sender's slot; docs/PROTOCOLO.md, phase 3).
// "zmp_equip b0..b7 c0..c6 equipment": the pause menu of the sender changed its buttons or its worn equipment
// (PLAN.md 2.7, EQUIP with absolute values).
std::vector<int> ParseInts(const std::string& cmd, size_t pos) {
    std::vector<int> v;
    while (pos < cmd.size()) {
        size_t next = cmd.find(' ', pos);
        std::string tok = cmd.substr(pos, next == std::string::npos ? std::string::npos : next - pos);
        if (!tok.empty()) {
            v.push_back(atoi(tok.c_str()));
        }
        if (next == std::string::npos) {
            break;
        }
        pos = next + 1;
    }
    return v;
}

struct ResumeArg {
    int slot;
    int health;
};
void ResumeHealthInContext(void* p) {
    ResumeArg* a = (ResumeArg*)p;
    gSaveContext.health = (s16)std::min<int>(a->health, gSaveContext.healthCapacity);
    gSaveContext.healthAccumulator = 0;
}

bool ApplyGameEvent(int slot, const std::string& cmd) {
    if (cmd.rfind("zmp_exit ", 0) == 0) {
        // Tests (phase 5): the sender's Link walks into an exit to that entrance (same effect as the collision exit:
        // the transition is its own).
        int entrance = (int)strtol(cmd.c_str() + 9, nullptr, 0);
        Player* p = Players::SlotPlayer(slot);
        if (p != nullptr && gPlayState != nullptr && gPlayState->transitionTrigger == TRANS_TRIGGER_OFF) {
            gPlayState->nextEntranceIndex = (s16)entrance;
            gPlayState->transitionTrigger = TRANS_TRIGGER_START;
            gPlayState->transitionType = TRANS_TYPE_FADE_BLACK;
            gSaveContext.nextTransitionType = TRANS_TYPE_FADE_BLACK;
            p->stateFlags1 |= PLAYER_STATE1_LOADING | PLAYER_STATE1_IN_CUTSCENE;
            Zmp_NoteTransitionBy(p);
        }
        return true;
    }
    if (cmd.rfind("zmp_lock_exits ", 0) == 0) {
        // Tests: scene exits act as walls while set (random play stays in the scene; PLAN.md 1.3.3, a lockstep event).
        gZmpSim.exitsLocked = (u8)(atoi(cmd.c_str() + 15) != 0);
        return true;
    }
    if (cmd.rfind("zmp_bottle_clear ", 0) == 0) {
        // Tests (phase 5b): the group has no bottle in that slot (the debug game starts with the four of them).
        Players::ClearBottle(atoi(cmd.c_str() + 17));
        return true;
    }
    if (cmd == "zmp_age") {
        // Tests (phase 5b): what pulling or returning the Master Sword does to its group: the scene loads again with
        // the other age (a scene change of the world, like the cutscene's).
        if (gPlayState != nullptr && gPlayState->transitionTrigger == TRANS_TRIGGER_OFF) {
            gPlayState->linkAgeOnLoad = gSaveContext.linkAge == LINK_AGE_CHILD ? LINK_AGE_ADULT : LINK_AGE_CHILD;
            gPlayState->nextEntranceIndex = gSaveContext.entranceIndex;
            gPlayState->transitionTrigger = TRANS_TRIGGER_START;
            gPlayState->transitionType = TRANS_TYPE_FADE_WHITE;
            gSaveContext.nextTransitionType = TRANS_TYPE_FADE_WHITE;
        }
        return true;
    }
    if (cmd == "zmp_autosave") {
        // Forced autosave (tests): the leader saves at the end of this tick.
        sForceAutosave = true;
        return true;
    }
    if (cmd.rfind("zmp_resume ", 0) == 0) {
        // "zmp_resume target health x y z yaw room": a player of a resumed session gets back its health, its place and
        // its room (docs/DECISIONES.md D-054).
        std::vector<int> v = ParseInts(cmd, 11);
        if (v.size() != 7) {
            Log("net: malformed resume event: " + cmd);
            return true;
        }
        int target = v[0];
        Player* p = Players::SlotPlayer(target);
        if (p == nullptr || gPlayState == nullptr) {
            return true;
        }
        ResumeArg a{ target, v[1] };
        Players::RunInContext(target, ResumeHealthInContext, &a);
        bool roomLoaded = false;
        for (int r : Players::LoadedRooms(gPlayState)) {
            roomLoaded = roomLoaded || r == v[6];
        }
        if (roomLoaded) {
            Vec3f pos = { (f32)v[2], (f32)v[3], (f32)v[4] };
            p->actor.world.pos = pos;
            p->actor.prevPos = pos;
            p->actor.home.pos = pos;
            p->actor.shape.rot.y = p->actor.world.rot.y = p->yaw = (s16)v[5];
            Players::SetSlotRoom(target, v[6]);
        }
        Log("net: slot " + std::to_string(target) + " resumed (health " + std::to_string(v[1]) +
            (roomLoaded ? ", back in room " + std::to_string(v[6]) : ", room not loaded: stays at the entrance") + ")");
        return true;
    }
    if (cmd.rfind("zmp_block ", 0) == 0) {
        // "zmp_block target magic b0..b7 c0..c6 equipment ammo0..15 bottle0..3" (saved per-player block)
        std::vector<int> v = ParseInts(cmd, 10);
        if (v.size() != 38) {
            Log("net: malformed block event: " + cmd);
            return true;
        }
        ZmpPlayerBlock b{};
        b.magic = (s8)v[1];
        for (int i = 0; i < 8; i++) {
            b.equips.buttonItems[i] = (u8)v[2 + i];
        }
        for (int i = 0; i < 7; i++) {
            b.equips.cButtonSlots[i] = (u8)v[10 + i];
        }
        b.equips.equipment = (u16)v[17];
        for (int i = 0; i < 16; i++) {
            b.ammo[i] = (s8)v[18 + i];
        }
        for (int i = 0; i < 4; i++) {
            b.bottles[i] = (u8)v[34 + i];
        }
        Players::ApplySavedBlock(v[0], b);
        return true;
    }
    if (cmd.rfind("zmp_equip ", 0) != 0) {
        return false;
    }
    std::vector<int> v;
    size_t pos = 10;
    while (pos < cmd.size()) {
        size_t next = cmd.find(' ', pos);
        std::string tok = cmd.substr(pos, next == std::string::npos ? std::string::npos : next - pos);
        if (!tok.empty()) {
            v.push_back(atoi(tok.c_str()));
        }
        if (next == std::string::npos) {
            break;
        }
        pos = next + 1;
    }
    if (v.size() != 16) {
        Log("net: malformed equip event from slot " + std::to_string(slot) + ": " + cmd);
        return true;
    }
    ItemEquips eq{};
    for (int i = 0; i < 8; i++) {
        eq.buttonItems[i] = (u8)v[i];
    }
    for (int i = 0; i < 7; i++) {
        eq.cButtonSlots[i] = (u8)v[8 + i];
    }
    eq.equipment = (u16)v[15];
    Players::ApplyEquip(slot, eq);
    return true;
}

} // namespace

void ApplyConsoleEvent(uint32_t tick, int slot, const std::string& cmd) {
    // Same command on every machine at the start of the same tick (PLAN.md 1.3.3), in the context of the player
    // who sent it (its health, ammo and equipment are the ones a console command like "addammo" changes).
    if (!ApplyGameEvent(slot, cmd)) {
        ConsoleArg arg{ &cmd };
        Players::RunInContext(slot, RunConsoleInContext, &arg);
    }
    Log("net: event at tick " + std::to_string(tick) + " from slot " + std::to_string(slot) + ": " + cmd);
}

const char* PhaseName(Phase phase) {
    switch (phase) {
        case Phase::WaitingGroup:
            return "waiting_group";
        case Phase::Joining:
            return "joining";
        case Phase::Running:
            return "running";
        case Phase::Detached:
            return "detached";
        default:
            return "idle";
    }
}

void OnConnected(bool rejoin) {
    std::lock_guard<std::mutex> lock(sMutex);
    sConnectedEvent = true;
    sRejoin = rejoin;
}

void OnDisconnected() {
    std::lock_guard<std::mutex> lock(sMutex);
    sDisconnectedEvent = true;
}

void OnNetMessage(json&& msg) {
    std::string t = msg.value("t", std::string());
    std::lock_guard<std::mutex> lock(sMutex);
    if (t == "TICK") {
        Bundle b;
        b.tick = msg.value("tick", 0u);
        b.delay = msg.value("delay", 2);
        b.dayTime = msg.value("day_time", -1);
        if (msg.contains("slots") && msg.contains("pads")) {
            auto& slots = msg["slots"];
            auto& pads = msg["pads"];
            for (size_t i = 0; i < slots.size() && i < pads.size(); i++) {
                b.pads.push_back({ slots[i].get<int>(), PadFromBytes(BinaryOf(pads[i])) });
            }
        }
        if (msg.contains("events") && msg["events"].is_array()) {
            for (auto& e : msg["events"]) {
                Event ev{ e.value("k", std::string()), e.value("slot", -1), e.value("cmd", std::string()) };
                if (e.contains("data")) {
                    ev.data = BinaryOf(e["data"]);
                }
                b.events.push_back(std::move(ev));
            }
        }
        sServerDelay = b.delay;
        sGroupTick = std::max(sGroupTick, b.tick);
        sNextServerTick = std::max(sNextServerTick, b.tick + 1);
        sBundles[b.tick] = std::move(b);
    } else if (t == "PLAYER_LIST") {
        sPlayers.clear();
        std::string me = Client::Get().GetStatus().name;
        if (msg.contains("players")) {
            for (auto& p : msg["players"]) {
                SlotInfo si;
                si.slot = p.value("slot", -1);
                si.name = p.value("name", std::string());
                si.state = p.value("state", std::string());
                si.rtt = p.value("rtt", -1);
                si.leader = p.value("leader", false);
                si.id = p.value("id", 0u);
                si.scene = p.value("scene", -1);
                si.group = p.value("group", 0u);
                sPlayers.push_back(si);
                // Phase 5: "X ha entrado en Y" when another player's group is in a new scene.
                if (si.name != me && si.group != 0 && si.scene >= 0) {
                    auto it = sLastScenes.find(si.name);
                    if (it != sLastScenes.end() && it->second != si.scene) {
                        sNotices.push_back({ si.name, si.scene, std::chrono::steady_clock::now() });
                        while (sNotices.size() > 6) {
                            sNotices.pop_front();
                        }
                    }
                    sLastScenes[si.name] = si.scene;
                }
            }
        }
        sServerDelay = msg.value("delay", sServerDelay);
    } else if (t == "WAIT") {
        sWaitTick = msg.value("tick", 0u);
        std::string names;
        if (msg.contains("missing")) {
            for (auto& n : msg["missing"]) {
                names += (names.empty() ? "" : ", ") + n.get<std::string>();
            }
        }
        sWaitingFor = names;
    } else {
        if (t == "GROUP_JOIN_PENDING" || t == "GROUP_FOUND") {
            // Another group from here on: the ticks of the one this client was in must not be taken for this one's
            // (phase 5b: the ticks of the new group arrive while its state is still on the way).
            sBundles.clear();
            sGroupTick = 0;
            sNextServerTick = 0;
        }
        sControl.push_back(std::move(msg));
    }
}

void OnFrameBegin() {
    std::deque<json> control;
    bool connected = false;
    bool disconnected = false;
    {
        std::lock_guard<std::mutex> lock(sMutex);
        control.swap(sControl);
        connected = sConnectedEvent;
        disconnected = sDisconnectedEvent;
        sConnectedEvent = sDisconnectedEvent = false;
        sDelay = sServerDelay;
        sLeader = false;
        for (auto& p : sPlayers) {
            // (slots are per group since phase 5: the entry of this group)
            if (p.slot == sSlot && sSlot >= 0 && p.leader && (p.group == 0 || p.group == sGroupId)) {
                sLeader = true;
            }
        }
    }
    if (disconnected) {
        sConnected = false;
        sJoinSent = false;
        Log(std::string("net: disconnected (phase ") + PhaseName(sPhase) + ")");
        if (sPhase == Phase::WaitingGroup || (sPhase == Phase::Joining && !sFreeRun)) {
            ResetGame("disconnected");
        } else if (sPhase == Phase::Joining) {
            sPhase = Phase::Detached; // on its way to another scene: ask again once reconnected
            sHavePendingBlob = false;
        }
        // Running: the simulation stalls (no bundles) until the rejoin replaces it (or, after a server restart, it
        // founds the group of its scene again). Detached: its game goes on.
    }
    if (connected) {
        sConnected = true;
        sJoinSent = false;
    }
    for (auto& msg : control) {
        HandleControl(msg);
    }
    if (sConnected && !sJoinSent && !(sPhase == Phase::Detached && sDetachSolo && !DetachArrived()) &&
        !(sPhase == Phase::Detached && !sDetachSolo && !DetachLoaded())) {
        // (a player arriving with its own cutscene asks only once it is there: it founds a group of its own)
        // (phase 5b: a player walking into another scene asks once that scene is loaded, still behind its fade: the
        // group it enters gives its state of that moment and plays on, so the newcomer has little to catch up with)
        SendJoin();
    }
    if (sPhase == Phase::Running && sSoloGroup && InRealGame()) {
        // The cutscene this player arrived with is over: open the group (the others of the scene, if any, get it).
        bool cs = gSaveContext.cutsceneIndex >= 0xFFF0 || gPlayState->csCtx.state != CS_STATE_IDLE ||
                  gPlayState->transitionMode != TRANS_MODE_OFF || gPlayState->transitionTrigger != TRANS_TRIGGER_OFF;
        sSoloIdleFrames = cs ? 0 : sSoloIdleFrames + 1;
        if (sSoloIdleFrames > 40) {
            sSoloGroup = false;
            Send({ { "t", "GROUP_OPEN" } });
            Log("net: own cutscene over, the group is open");
        }
    }
    if (sConnected && sPhase == Phase::WaitingGroup && !sJoinInPlaySent && InRealGame()) {
        SendJoin(); // this client now has a game: it can found the group
    }
    if (sConnected && sPhase == Phase::Detached && sDetachWaiting && !sJoinInPlaySent && DetachArrived()) {
        SendJoin(); // arrived in a scene where nobody plays: this client founds its group
    }
    if (sConnected && sPhase == Phase::Running && InRealGame() && gPlayState->transitionMode == TRANS_MODE_OFF &&
        (int)gPlayState->sceneNum != sGroupScene) {
        // The whole group changed scene inside the simulation (a cutscene, a warp song, the game over).
        sGroupScene = gPlayState->sceneNum;
        Send({ { "t", "GROUP_SCENE" }, { "scene", sGroupScene } });
        Log("net: the group is now in scene " + std::to_string(sGroupScene));
    }
    LoadPendingBlob();
    ProcessBlobRequests();
    SendPendingInputs();
    UpdateNameTags();
    if (sPhase == Phase::Running && !InPlay() && gGameState != nullptr && gGameState->main == FileChoose_Main) {
        // The shared game went back to the file select screen (game over, reset): the session ends here.
        Send({ { "t", "LEAVE_GROUP" } });
        ResetGame("file select");
        if (sGameOverQuit) {
            // Game over, "continue? no" (D-058): every client leaves the room in order; nobody waits for anybody.
            sGameOverQuit = false;
            sEndNotice = sGameOverQuitMine ? std::string("Has terminado la partida del grupo")
                                           : sGameOverQuitBy + " (el anfitrion) ha terminado la partida";
            sEndNoticeAt = Clock::now();
            Log("net: group game over, the session ends (" + sEndNotice + ")");
            Client::Get().Disconnect();
        }
    }
}

bool ShouldRunTick() {
    if (sFreeRun && (sPhase == Phase::Detached || sPhase == Phase::Joining)) {
        return true; // on its way to another scene: the local game goes on by itself
    }
    if (sPhase != Phase::Running) {
        return false;
    }
    uint32_t tick = Sim::CurrentTick();
    if (sResyncHold && tick >= sResyncTick) {
        return false;
    }
    bool have;
    {
        std::lock_guard<std::mutex> lock(sMutex);
        have = sBundles.count(tick) != 0;
    }
    auto now = Clock::now();
    if (!have) {
        if (!sWaitingNow) {
            sWaitingNow = true;
            sWaitStart = now;
        }
        return false;
    }
    if (sWaitingNow) {
        sWaitingNow = false;
        uint32_t ms = (uint32_t)std::chrono::duration_cast<std::chrono::milliseconds>(now - sWaitStart).count();
        if (ms >= 250) {
            sStalls++;
            std::string who;
            {
                std::lock_guard<std::mutex> lock(sMutex);
                who = sWaitingFor;
            }
            Log("net: stall tick=" + std::to_string(tick) + " ms=" + std::to_string(ms) +
                (who.empty() ? "" : " waiting_for=" + who));
        }
        sMaxStallMs = std::max(sMaxStallMs, ms);
        sGateWaitMax = std::max(sGateWaitMax, ms);
    }
    return true;
}

void OnPadRead(void* padsV) {
    OSContPad* pads = (OSContPad*)padsV;
    uint32_t tick = Sim::CurrentTick();
    // Local pad (physical or harness), sent for every tick up to tick + D.
    Sim::PadRecord local;
    local.buttons = (uint32_t)pads[0].button;
    local.stickX = pads[0].stick_x;
    local.stickY = pads[0].stick_y;
    local.rStickX = pads[0].right_stick_x;
    local.rStickY = pads[0].right_stick_y;
    Pause::OnLocalPad(pads[0]);
    Players::RandTraceBegin(CVarGetInteger(ZMP_CVAR_DEBUG_TICK_DUMPS, 0) != 0);
    if (sLocalInputBlocked) {
        local = Sim::PadRecord{}; // the pause menu is open: this player's Link stands still (PLAN.md 2.7)
    }
    if (sPhase == Phase::Running && Zmp_MultiActive() && !Players::IsPresent(sSlot)) {
        local = Sim::PadRecord{}; // entering a group (screen still covered): nothing pressed blind reaches its Link
    }
    sLastLocal = local;
    if (sFreeRun && sPhase != Phase::Running) {
        // Detached: only this player's Link moves (the others it left behind stand still until the scene unloads).
        int L = Players::LocalSlot();
        for (int k = 0; k < ZMP_MAX_PLAYERS; k++) {
            if (Players::SlotPlayer(k) != nullptr || gZmpSim.slots[k].active) {
                Players::StepInput(k, k == L ? ToPad(local) : OSContPad{});
            }
        }
        memset(pads, 0, sizeof(OSContPad) * 4);
        pads[0] = gZmpSim.anchor == L ? ToPad(local) : OSContPad{};
        return;
    }
    Bundle b;
    {
        std::lock_guard<std::mutex> lock(sMutex);
        auto it = sBundles.find(tick);
        if (it != sBundles.end()) {
            b = std::move(it->second);
            sBundles.erase(it);
        }
    }
    if (b.dayTime >= 0 && gZmpSim.enabled) {
        // Phase 5: the world clock. A time this group set itself (a cutscene) waits for the server to report it back.
        if (gZmpSim.clockHold) {
            u16 diff = (u16)((u16)b.dayTime - gZmpSim.clockHoldValue);
            if (diff < 0x400 || diff > 0xFC00 || ++gZmpSim.clockHoldTicks > 200) {
                gZmpSim.clockHold = 0;
            }
        }
        if (!gZmpSim.clockHold) {
            gSaveContext.dayTime = (u16)b.dayTime;
        }
    }
    gZmpSim.dayAtTickStart = gSaveContext.dayTime;
    for (auto& e : b.events) {
        if (e.kind == "SHARED") {
            // Phase 5: progress made by another group (flags, items, rupees...), on every machine of this group at the
            // start of the same tick; the baseline moves too, so it is never sent back.
            std::string sum;
            if (SharedGame::ApplyToGame(e.data, &sum)) {
                std::vector<uint8_t> base = SharedGame::Baseline();
                if (!base.empty() && SharedGame::ApplyToBuffer(base, e.data)) {
                    SharedGame::SetBaseline(base);
                }
                sSharedApplied++;
                sLastShared = sum;
                Log("net: shared game changed by another group at tick " + std::to_string(tick) + ": " + sum);
            } else {
                Log("net: malformed shared game patch at tick " + std::to_string(tick));
            }
        }
    }
    for (auto& e : b.events) {
        if (e.kind == "SPAWN") {
            Players::Spawn(e.slot, e.cmd);
            if (e.slot == sSlot && sJoinStartValid) {
                sJoinStartValid = false;
                sLastSpawnMs =
                    (int)std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - sJoinStart).count();
                Log("net: this player's Link appears in the group at tick " + std::to_string(tick) + ", " +
                    std::to_string(sLastSpawnMs) + " ms after asking to enter");
            }
            if (sLeader && e.slot != sSlot) {
                // A player of a resumed session: its block, health, place and room (D-054).
                auto rit = sResumePlayers.find(SlotName(e.slot));
                if (rit != sResumePlayers.end()) {
                    auto& p = rit->second;
                    SendConsoleEvent("zmp_block " + std::to_string(e.slot) + " " + p.value("block", std::string()));
                    auto& pos = p["pos"];
                    SendConsoleEvent("zmp_resume " + std::to_string(e.slot) + " " +
                                     std::to_string(p.value("health", 0)) + " " + std::to_string(pos[0].get<int>()) +
                                     " " + std::to_string(pos[1].get<int>()) + " " + std::to_string(pos[2].get<int>()) +
                                     " " + std::to_string(p.value("yaw", 0)) + " " +
                                     std::to_string(p.value("room", -1)));
                    sResumePlayers.erase(rit);
                }
            }
            if (sLeader) {
                auto it = sSavedBlocks.find(SlotName(e.slot));
                if (it != sSavedBlocks.end() && e.slot != sSlot) {
                    // This player's equipment, buttons, ammo and bottles from the saved game (PLAN.md 2.6).
                    SendConsoleEvent("zmp_block " + std::to_string(e.slot) + " " + it->second);
                    sSavedBlocks.erase(it);
                }
            }
        } else if (e.kind == "DESPAWN") {
            Players::Despawn(e.slot);
        } else if (e.kind == "CONSOLE" && !e.cmd.empty()) {
            ApplyConsoleEvent(tick, e.slot, e.cmd);
        }
    }
    bool seen[ZMP_MAX_PLAYERS] = {};
    Sim::PadRecord anchorPad;
    for (auto& [slot, pad] : b.pads) {
        if (slot >= 0 && slot < ZMP_MAX_PLAYERS) {
            Players::StepInput(slot, ToPad(pad));
            seen[slot] = true;
            if (slot == gZmpSim.anchor) {
                anchorPad = pad;
            }
        }
    }
    for (int k = 0; k < ZMP_MAX_PLAYERS; k++) {
        if (!seen[k] && gZmpSim.slots[k].active) {
            Players::StepInput(k, OSContPad{});
        }
    }
    // The pad manager sees the anchor's pad (its history is part of the save state); ports 2-4 are empty.
    memset(pads, 0, sizeof(OSContPad) * 4);
    pads[0] = ToPad(anchorPad);
}

// Phase 5: end of a tick of a group member. The patch of the shared game (every member computes and sends the same;
// the server takes the first) and the world clock: the rate time runs at in this scene, or a time the group set.
void ReportSharedAndClock(uint32_t tick) {
    std::vector<uint8_t> patch = SharedGame::TakeTickPatch();
    if (!patch.empty()) {
        Send({ { "t", "SHARED" }, { "tick", tick }, { "data", json::binary(patch) } });
        sSharedSent++;
        if (sLeader) {
            Log("net: shared game changed here at tick " + std::to_string(tick) + ": " + SharedGame::Describe(patch));
        }
    }
    if (!InPlay() || gPlayState->transitionMode != TRANS_MODE_OFF) {
        return;
    }
    int rate = (int)gTimeSpeed * ((gSaveContext.nightFlag == 0 || gTimeSpeed >= 0x190) ? 1 : 2);
    u16 inc = (u16)(gSaveContext.dayTime - gZmpSim.dayAtTickStart);
    int off = (int)(s16)(u16)(inc - (u16)rate);
    if (inc != 0 && (off > 0x40 || off < -0x40)) {
        gZmpSim.clockHold = 1;
        gZmpSim.clockHoldValue = gSaveContext.dayTime;
        gZmpSim.clockHoldTicks = 0;
        Send({ { "t", "CLOCK" }, { "tick", tick }, { "rate", rate }, { "set", (int)gSaveContext.dayTime } });
        sLastRate = rate;
        Log("net: this group set the time to " + std::to_string(gSaveContext.dayTime) + " at tick " +
            std::to_string(tick));
    } else if (rate != sLastRate) {
        Send({ { "t", "CLOCK" }, { "tick", tick }, { "rate", rate }, { "set", -1 } });
        sLastRate = rate;
    }
}

void OnTickEnd(uint32_t tick, uint64_t hash) {
    if (sPhase != Phase::Running) {
        return;
    }
    ReportSharedAndClock(tick);
    if (CVarGetInteger(ZMP_CVAR_DEBUG_TICK_DUMPS, 0)) {
        // Debug (desync hunting): a readable dump of every tick, 64 rotating files; a RESYNC keeps the ones before
        // the bad tick.
        std::string err;
        std::string tpath = "logs/tickdump-" + std::to_string(tick % 64) + ".txt";
        Sim::DumpState(tpath, tick, &err);
        if (FILE* f = fopen(tpath.c_str(), "a")) {
            std::string tr = Players::RandTraceText();
            fwrite(tr.data(), 1, tr.size(), f);
            fclose(f);
        }
        sTickDumpTicks[tick % 64] = tick;
    }
    if (tick % 20 == 0) {
        // Readable dump of every hashed tick (4 rotating files): a RESYNC names the tick whose hash
        // differed, and the dumps of that exact tick on each machine are what explains it.
        std::string err;
        std::string path = "logs/hashdump-" + std::to_string((tick / 20) % 4) + ".txt";
        Sim::DumpState(path, tick, &err);
        sDumpTicks[(tick / 20) % 4] = tick;
        Send({ { "t", "HASH" }, { "tick", tick }, { "hash", hash } });
        size_t queued;
        {
            std::lock_guard<std::mutex> lock(sMutex);
            queued = sBundles.size();
        }
        Log("net tick=" + std::to_string(tick) + " hash=" + Hex(hash) + " D=" + std::to_string(sDelay) +
            " queue=" + std::to_string(queued) + " players=" + std::to_string(Players::PresentCount()) +
            " stalls=" + std::to_string(sStalls));
    }
    MaybeAutosave(tick, hash);
}

// How long the tick gate has been closed right now, waiting for the server's next tick (0: open). The time a player
// is held by the others; a slow frame of its own PC is not in it.
int GateWaitMs() {
    int ms = (int)sGateWaitMax;
    sGateWaitMax = 0;
    if (sPhase == Phase::Running && sWaitingNow) {
        ms =
            std::max(ms, (int)std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - sWaitStart).count());
    }
    return ms;
}

int CatchUpSpeed() {
    if (sPhase != Phase::Running) {
        return 0;
    }
    // Phase 5b: whoever is behind catches up by itself (after loading the group's state while the group played on,
    // after a hiccup of its own PC or connection); nobody waits for it.
    std::lock_guard<std::mutex> lock(sMutex);
    size_t behind = sBundles.size();
    size_t d = (size_t)std::max(0, sDelay);
    return behind > d + 12 ? 8 : behind > d + 6 ? 4 : behind > d + 1 ? 2 : 0;
}

void SaveGroupBlocks(int fileNum) {
    if (sPhase != Phase::Running || !Zmp_MultiActive() || fileNum < 0 || fileNum >= 3) {
        return;
    }
    std::string out;
    for (int k = 0; k < ZMP_MAX_PLAYERS; k++) {
        if (!gZmpSim.slots[k].active || k == sSlot) {
            continue; // the leader's own block is in the save file itself
        }
        out += SlotName(k) + "\t" + BlockPayload(Players::SlotBlock(k)) + "\n";
    }
    // Players saved earlier who are not here now keep their entry.
    for (auto& [name, payload] : sSavedBlocks) {
        if (out.find(name + "\t") == std::string::npos) {
            out += name + "\t" + payload + "\n";
        }
    }
    if (FILE* f = fopen(BlocksPath(fileNum).c_str(), "w")) {
        fwrite(out.data(), 1, out.size(), f);
        fclose(f);
        Log("net: player blocks saved with the game (" + BlocksPath(fileNum) + ")");
    }
}

void SetLocalInputBlocked(bool blocked) {
    sLocalInputBlocked = blocked;
}

void SendConsoleEvent(const std::string& cmd) {
    Send({ { "t", "EVENT" }, { "kind", "CONSOLE" }, { "payload", cmd } });
}

void Leave() {
    if (sPhase == Phase::Idle) {
        return;
    }
    Send({ { "t", "LEAVE_GROUP" } });
    ResetGame("leave");
}

void DetachForTransition(int entrance, bool solo) {
    if (sPhase != Phase::Running && sPhase != Phase::Detached) {
        return;
    }
    // Leave now (inside the tick, before this tick's hash could be sent): the others' simulations dropped this Link at
    // the same tick. Then ask for the destination's group right away (OnFrameBegin).
    if (sPhase == Phase::Running) {
        Send({ { "t", "LEAVE_GROUP" } });
        sDetachAt = Clock::now();
    }
    int scene = -1;
    if (entrance >= 0 && entrance < ENTR_MAX) {
        scene = gEntranceTable[entrance].scene;
    }
    Log("net: left the group through entrance " + std::to_string(entrance) + " to scene " + std::to_string(scene));
    sPhase = Phase::Detached;
    sFreeRun = true;
    sDetachScene = scene;
    sDetachEntrance = entrance;
    sDetachInits = Players::PlayInitCount();
    sDetachArrived = false;
    sDetachWaiting = solo; // solo: nobody to join, found once arrived
    sDetachSolo = solo;
    sBlobRequests.clear();
    sResyncHold = false;
    sHavePendingBlob = false;
    sJoinSent = false;
    sJoinInPlaySent = false;
}

bool SceneGroups() {
    return sPhase == Phase::Running || sPhase == Phase::Detached;
}

std::string SceneName(int scene) {
    static const std::map<int, const char*> kNames = {
        { SCENE_DEKU_TREE, "Gran Arbol Deku" },
        { SCENE_DODONGOS_CAVERN, "Cueva de los Dodongos" },
        { SCENE_JABU_JABU, "Tripa de Jabu-Jabu" },
        { SCENE_FOREST_TEMPLE, "Templo del Bosque" },
        { SCENE_FIRE_TEMPLE, "Templo del Fuego" },
        { SCENE_WATER_TEMPLE, "Templo del Agua" },
        { SCENE_SPIRIT_TEMPLE, "Templo del Espiritu" },
        { SCENE_SHADOW_TEMPLE, "Templo de las Sombras" },
        { SCENE_BOTTOM_OF_THE_WELL, "Fondo del Pozo" },
        { SCENE_ICE_CAVERN, "Caverna de Hielo" },
        { SCENE_DEKU_TREE_BOSS, "Guarida de Gohma" },
        { SCENE_KOKIRI_FOREST, "Bosque Kokiri" },
        { SCENE_KOKIRI_SHOP, "Tienda Kokiri" },
        { SCENE_LINKS_HOUSE, "Casa de Link" },
        { SCENE_MIDOS_HOUSE, "Casa de Mido" },
        { SCENE_SARIAS_HOUSE, "Casa de Saria" },
        { SCENE_TWINS_HOUSE, "Casa de las gemelas" },
        { SCENE_KNOW_IT_ALL_BROS_HOUSE, "Casa de los sabelotodo" },
        { SCENE_LOST_WOODS, "Bosque Perdido" },
        { SCENE_SACRED_FOREST_MEADOW, "Pradera Sagrada" },
        { SCENE_HYRULE_FIELD, "Campo de Hyrule" },
        { SCENE_KAKARIKO_VILLAGE, "Aldea Kakariko" },
        { SCENE_GRAVEYARD, "Cementerio" },
        { SCENE_ZORAS_RIVER, "Rio Zora" },
        { SCENE_LAKE_HYLIA, "Lago Hylia" },
        { SCENE_ZORAS_DOMAIN, "Region de los Zora" },
        { SCENE_ZORAS_FOUNTAIN, "Fuente Zora" },
        { SCENE_GERUDO_VALLEY, "Valle Gerudo" },
        { SCENE_GERUDOS_FORTRESS, "Fortaleza Gerudo" },
        { SCENE_HAUNTED_WASTELAND, "Desierto Encantado" },
        { SCENE_DESERT_COLOSSUS, "Coloso del Desierto" },
        { SCENE_HYRULE_CASTLE, "Castillo de Hyrule" },
        { SCENE_DEATH_MOUNTAIN_TRAIL, "Montana de la Muerte" },
        { SCENE_DEATH_MOUNTAIN_CRATER, "Crater de la Montana" },
        { SCENE_GORON_CITY, "Ciudad Goron" },
        { SCENE_LON_LON_RANCH, "Rancho Lon Lon" },
        { SCENE_TEMPLE_OF_TIME, "Templo del Tiempo" },
        { SCENE_MARKET_DAY, "Mercado" },
        { SCENE_MARKET_NIGHT, "Mercado" },
    };
    auto it = kNames.find(scene);
    if (it != kNames.end()) {
        return it->second;
    }
    if (scene < 0) {
        return "?";
    }
    return SohUtils::GetSceneName(scene);
}

Status GetStatus() {
    Status s;
    s.phase = sPhase;
    s.slot = sSlot;
    s.tick = Sim::CurrentTick();
    s.delay = sDelay;
    s.stalls = sStalls;
    s.maxStallMs = sMaxStallMs;
    s.resyncs = sResyncs;
    s.resyncsSeen = sResyncsSeen;
    s.lastResyncTick = sLastResyncTick;
    s.leader = sLeader;
    s.lastError = sLastError;
    s.countdown = 0;
    s.groupId = sGroupId;
    s.groupScene = sGroupScene;
    s.detachScene = sPhase == Phase::Detached || sFreeRun ? sDetachScene : -1;
    s.freeRun = sFreeRun;
    s.ownsSave = sOwnsSave;
    s.sharedSent = sSharedSent;
    s.sharedApplied = sSharedApplied;
    s.lastShared = sLastShared;
    s.groupJoins = sGroupJoins;
    s.nameTags = (int)sTagged.size();
    s.lastJoinMs = sLastJoinMs;
    s.lastSpawnMs = sLastSpawnMs;
    s.relocatedLoads = sRelocatedLoads;
    s.lastRelocated = sLastRelocated;
    s.lastRelocatedOdd = sLastRelocatedOdd;
    s.catchingUp = sPhase == Phase::Running && Zmp_MultiActive() && !Players::IsPresent(sSlot) && !sResyncHold;
    // The screen stays covered (the scene change's own black) from the moment this client asks the scene's group for
    // its state until its Link is in that group; if nobody plays there, the game's fade-in just goes on.
    s.covered =
        s.catchingUp || (sFreeRun && sPhase == Phase::Joining && DetachLoaded()) ||
        (sFreeRun && sPhase == Phase::Detached && sJoinSent && !sDetachWaiting && !sDetachSolo && DetachLoaded());
    if (sWaitingNow) {
        s.waitMs = (int)std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - sWaitStart).count();
        // (phase 5b: a short wait is not worth a notice; it is for real connection drops)
        s.waiting = s.waitMs >= 1000;
    }
    std::lock_guard<std::mutex> lock(sMutex);
    s.queued = sBundles.size();
    s.waitingFor = sWaitingFor;
    s.players = sPlayers;
    s.groupTick = sGroupTick;
    if (!sEndNotice.empty() && std::chrono::duration<double>(Clock::now() - sEndNoticeAt).count() < 15.0) {
        s.endNotice = sEndNotice;
    }
    s.saveSkipped = sSaveSkipped && std::chrono::duration<double>(Clock::now() - sSaveSkippedAt).count() < 3.0;
    for (auto& n : sNotices) {
        s.sceneNotices.push_back({ n.name, n.scene, std::chrono::duration<double>(Clock::now() - n.at).count() });
    }
    return s;
}

std::string GameOverChooser() {
    // The group game over menu is the simulation's and reads the anchor's pad (D-036): the others see who decides.
    if (sPhase != Phase::Running || !InPlay() || !gZmpSim.enabled) {
        return std::string();
    }
    if (gPlayState->gameOverCtx.state != GAMEOVER_DEATH_MENU || sSlot == gZmpSim.anchor) {
        return std::string();
    }
    return SlotName(gZmpSim.anchor);
}

bool Active() {
    return sPhase == Phase::Running || sPhase == Phase::Joining;
}

std::string SlotName(int slot) {
    std::lock_guard<std::mutex> lock(sMutex);
    // Slots are per group (phase 5): the player of this slot in this client's group.
    for (auto& p : sPlayers) {
        if (p.slot == slot && (p.group == 0 || p.group == sGroupId)) {
            return p.name;
        }
    }
    return "P" + std::to_string(slot + 1);
}

} // namespace Zmp::Lockstep

extern "C" s32 Zmp_GameOverQuit(void) {
    using namespace Zmp::Lockstep;
    if (!gZmpSim.enabled || sPhase != Phase::Running) {
        return 0;
    }
    sGameOverQuit = true;
    sGameOverQuitMine = sSlot == gZmpSim.anchor;
    sGameOverQuitBy = SlotName(gZmpSim.anchor);
    return 1;
}

extern "C" void Zmp_NoteSaveSkipped(void) {
    using namespace Zmp::Lockstep;
    sSaveSkipped = true;
    sSaveSkippedAt = std::chrono::steady_clock::now();
}

extern "C" s32 Zmp_AllowSaveWrite(void) {
    if (!gZmpSim.enabled) {
        return 1;
    }
    // Phase 5: the PC of the player who started the room's game with its own save (not the leader of each group).
    return Zmp::Lockstep::GetStatus().ownsSave ? 1 : 0;
}
