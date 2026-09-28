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
#include <ship/Context.h>
#include <ship/debug/Console.h>
#include "soh/Zmp/ZmpLog.h"
#include "soh/Zmp/Sim/Session.h"
#include "soh/Zmp/Sim/ZmpPlayers.h"
#include "soh/Zmp/State/StateBlob.h"
#include "soh/Enhancements/nametag.h"

extern "C" {
#include <z64.h>
#include "variables.h"
#include "functions.h"
#include "macros.h"
extern PlayState* gPlayState;
void FileChoose_Main(GameState* thisx);
void Play_Main(GameState* thisx);
}

namespace Zmp::Lockstep {

namespace {

using json = nlohmann::json;
using Clock = std::chrono::steady_clock;

struct Event {
    std::string kind;
    int slot = -1;
    std::string cmd;
};

struct Bundle {
    uint32_t tick = 0;
    std::vector<std::pair<int, Sim::PadRecord>> pads;
    std::vector<Event> events;
    int delay = 2;
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
uint32_t sResyncs = 0;
uint32_t sResyncsSeen = 0;
uint32_t sLastResyncTick = 0;
bool sLeader = false;
uint32_t sDumpTicks[4] = { UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX };
Sim::PadRecord sLastLocal; // last pad read from the local controller (or the harness)
std::string sLastError;
std::map<int, Player*> sTagged;
std::map<int, std::string> sTagNames;

bool InPlay() {
    return gGameState != nullptr && gGameState->main == Play_Main && gPlayState != nullptr;
}

bool InFileSelect() {
    return gGameState != nullptr && gGameState->main == FileChoose_Main;
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

void SendJoin() {
    bool inPlay = InPlay();
    json msg = { { "t", "JOIN_GROUP" },
                 { "scene", inPlay ? (int)gPlayState->sceneNum : -1 },
                 { "entrance", (int)gSaveContext.entranceIndex },
                 { "in_play", inPlay } };
    Send(msg);
    sJoinSent = true;
    sJoinInPlaySent = inPlay;
    Log(std::string("net: JOIN_GROUP in_play=") + (inPlay ? "1" : "0"));
}

void ResetGame(const char* why) {
    if (sPhase == Phase::Running || sPhase == Phase::Joining) {
        Log(std::string("net: leaving lockstep (") + why + ")");
    }
    if (Sim::GetStatus().mode == Sim::Mode::Net) {
        Sim::EndNet();
    }
    Players::Reset();
    Players::SetLocalSlot(-1);
    State::SetKeepSharedSettings(false);
    sPhase = Phase::Idle;
    sBlobRequests.clear();
    sHavePendingBlob = false;
    sResyncHold = false;
    sWaitingNow = false;
    {
        std::lock_guard<std::mutex> lock(sMutex);
        sBundles.clear();
    }
}

void StartRunning(uint32_t tick) {
    sPhase = Phase::Running;
    sLastLocal = Sim::PadRecord{};
    sWaitingNow = false;
    Sim::BeginNet(tick);
    State::SetKeepSharedSettings(true);
    std::lock_guard<std::mutex> lock(sMutex);
    for (auto it = sBundles.begin(); it != sBundles.end();) {
        it = it->first < tick ? sBundles.erase(it) : std::next(it);
    }
}

void HandleControl(json& msg) {
    std::string t = msg.value("t", std::string());
    if (t == "GROUP_FOUND") {
        int slot = msg.value("slot", 0);
        sGroupId = msg.value("group_id", 0u);
        sDelay = msg.value("delay", 2);
        if (!InPlay()) {
            sLastError = "grupo fundado sin partida cargada";
            Log("net: GROUP_FOUND but not in play; leaving");
            Send({ { "t", "LEAVE_GROUP" } });
            return;
        }
        Players::Found(slot);
        Players::SetLocalSlot(slot);
        sSlot = slot;
        sNextInputTick = msg.value("tick0", 0u);
        StartRunning(sNextInputTick);
        Log("net: GROUP_FOUND group=" + std::to_string(sGroupId) + " slot=" + std::to_string(slot) +
            " (this client founded the shared game)");
    } else if (t == "GROUP_WAIT") {
        if (sPhase == Phase::Idle) {
            sPhase = Phase::WaitingGroup;
        }
        Log("net: GROUP_WAIT " + msg.value("reason", std::string()));
    } else if (t == "GROUP_JOIN_PENDING") {
        sSlot = msg.value("slot", -1);
        sGroupId = msg.value("group_id", 0u);
        sJoinTick = msg.value("spawn_tick", 0u);
        sJoinIsRejoin = msg.value("rejoin", false);
        if (sPhase == Phase::Running) {
            // Rejoin after a reconnection: the local simulation is replaced by the leader's.
            ResetGame("rejoin");
        }
        sPhase = Phase::Joining;
        sHavePendingBlob = false;
        sLoadFrames = 0;
        sStartedGameForLoad = false;
        Players::SetLocalSlot(sSlot);
        Log("net: GROUP_JOIN_PENDING slot=" + std::to_string(sSlot) + " T=" + std::to_string(sJoinTick) +
            (sJoinIsRejoin ? " (rejoin)" : ""));
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
        sResyncsSeen++;
        sLastResyncTick = bad;
        Sim::WriteDesyncDump(bad);
        for (int i = 0; i < 4; i++) {
            if (sDumpTicks[i] == bad) {
                std::error_code ec;
                std::filesystem::copy_file("logs/hashdump-" + std::to_string(i) + ".txt",
                                           "logs/desync-" + std::to_string(bad) + "-exact.txt",
                                           std::filesystem::copy_options::overwrite_existing, ec);
            }
        }
        bool target = false;
        if (msg.contains("targets") && msg["targets"].is_array()) {
            for (auto& x : msg["targets"]) {
                if (x.get<int>() == sSlot) {
                    target = true;
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

// Sends the local pad for every tick up to the current one + D. Runs every frame (also while the tick gate
// is closed, otherwise nobody could ever send the input the gate is waiting for). The pad is the last one
// read in a tick.
void SendPendingInputs() {
    if (sPhase != Phase::Running) {
        return;
    }
    uint32_t target = Sim::CurrentTick() + (uint32_t)std::max(1, sDelay);
    while (sNextInputTick <= target) {
        Send({ { "t", "INPUT" }, { "tick", sNextInputTick }, { "pad", json::binary(PadToBytes(sLastLocal)) } });
        sNextInputTick++;
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
        if (State::Save(blob, tick, st.lastHash, &err, &info)) {
            json msg = { { "t", "STATE_BLOB" },
                         { "group_id", it->groupId },
                         { "tick", tick },
                         { "for_slot", it->forSlot },
                         { "data", json::binary(blob) },
                         { "hash", st.lastHash } };
            Send(msg);
            Log("net: STATE_BLOB sent for slot " + std::to_string(it->forSlot) + " at tick " + std::to_string(tick) +
                " (" + std::to_string(blob.size()) + " bytes, " + std::to_string((int)info.ms) + " ms)");
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
    if (!InPlay()) {
        // A joiner outside a game first starts one (any), then loads the leader's state on it.
        // (from the file select, the title screen or the intro: any game state can start one)
        if (!sStartedGameForLoad && gGameState != nullptr) {
            Sim::StartSpec plain;
            Sim::StartDebugGame(plain);
            sStartedGameForLoad = true;
        }
        sLoadFrames = 0;
        return;
    }
    if (joining && ++sLoadFrames < 5) {
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
    Players::SetLocalSlot(sSlot);
    if (joining) {
        sNextInputTick = sJoinTick;
        StartRunning(info.tick);
        Log("net: joined the group at tick " + std::to_string(info.tick) + " in " + std::to_string((int)info.ms) +
            " ms" + (info.notes.empty() ? "" : " | " + info.notes));
    } else {
        Sim::SetTick(info.tick);
        sResyncHold = false;
        Log("net: resynchronized at tick " + std::to_string(info.tick) + " in " + std::to_string((int)info.ms) +
            " ms" + (info.notes.empty() ? "" : " | " + info.notes));
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

} // namespace

const char* PhaseName(Phase phase) {
    switch (phase) {
        case Phase::WaitingGroup:
            return "waiting_group";
        case Phase::Joining:
            return "joining";
        case Phase::Running:
            return "running";
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
        if (msg.contains("slots") && msg.contains("pads")) {
            auto& slots = msg["slots"];
            auto& pads = msg["pads"];
            for (size_t i = 0; i < slots.size() && i < pads.size(); i++) {
                b.pads.push_back({ slots[i].get<int>(), PadFromBytes(BinaryOf(pads[i])) });
            }
        }
        if (msg.contains("events") && msg["events"].is_array()) {
            for (auto& e : msg["events"]) {
                b.events.push_back({ e.value("k", std::string()), e.value("slot", -1), e.value("cmd", std::string()) });
            }
        }
        sServerDelay = b.delay;
        sGroupTick = std::max(sGroupTick, b.tick);
        sBundles[b.tick] = std::move(b);
    } else if (t == "PLAYER_LIST") {
        sPlayers.clear();
        if (msg.contains("players")) {
            for (auto& p : msg["players"]) {
                SlotInfo si;
                si.slot = p.value("slot", -1);
                si.name = p.value("name", std::string());
                si.state = p.value("state", std::string());
                si.rtt = p.value("rtt", -1);
                si.leader = p.value("leader", false);
                si.id = p.value("id", 0u);
                sPlayers.push_back(si);
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
            if (p.slot == sSlot && sSlot >= 0 && p.leader) {
                sLeader = true;
            }
        }
    }
    if (disconnected) {
        sConnected = false;
        sJoinSent = false;
        Log(std::string("net: disconnected (phase ") + PhaseName(sPhase) + ")");
        if (sPhase == Phase::WaitingGroup || sPhase == Phase::Joining) {
            ResetGame("disconnected");
        }
        // Running: the simulation stalls (no bundles) until the rejoin replaces it.
    }
    if (connected) {
        sConnected = true;
        sJoinSent = false;
    }
    for (auto& msg : control) {
        HandleControl(msg);
    }
    if (sConnected && !sJoinSent) {
        SendJoin();
    }
    if (sConnected && sPhase == Phase::WaitingGroup && !sJoinInPlaySent && InPlay()) {
        SendJoin(); // this client now has a game: it can found the group
    }
    LoadPendingBlob();
    ProcessBlobRequests();
    SendPendingInputs();
    UpdateNameTags();
    if (sPhase == Phase::Running && !InPlay() && gGameState != nullptr && gGameState->main == FileChoose_Main) {
        // The shared game went back to the file select screen (game over, reset): the session ends here.
        Send({ { "t", "LEAVE_GROUP" } });
        ResetGame("file select");
    }
}

bool ShouldRunTick() {
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
    sLastLocal = local;
    Bundle b;
    {
        std::lock_guard<std::mutex> lock(sMutex);
        auto it = sBundles.find(tick);
        if (it != sBundles.end()) {
            b = std::move(it->second);
            sBundles.erase(it);
        }
    }
    for (auto& e : b.events) {
        if (e.kind == "SPAWN") {
            Players::Spawn(e.slot);
        } else if (e.kind == "DESPAWN") {
            Players::Despawn(e.slot);
        } else if (e.kind == "CONSOLE" && !e.cmd.empty()) {
            // Same console command on every machine at the start of the same tick (PLAN.md 1.3.3).
            std::string output;
            Ship::Context::GetRawInstance()->GetConsole()->Run(e.cmd, &output);
            Log("net: event at tick " + std::to_string(tick) + " from slot " + std::to_string(e.slot) + ": " + e.cmd);
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

void OnTickEnd(uint32_t tick, uint64_t hash) {
    if (sPhase != Phase::Running) {
        return;
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
}

int CatchUpSpeed() {
    if (sPhase != Phase::Running) {
        return 0;
    }
    std::lock_guard<std::mutex> lock(sMutex);
    return sBundles.size() > (size_t)(sDelay + 3) ? 2 : 0;
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
    s.countdown = gZmpSim.transitionArmed ? gZmpSim.transitionCountdown : 0;
    if (sWaitingNow) {
        s.waitMs = (int)std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - sWaitStart).count();
        s.waiting = s.waitMs >= 250;
    }
    std::lock_guard<std::mutex> lock(sMutex);
    s.queued = sBundles.size();
    s.waitingFor = sWaitingFor;
    s.players = sPlayers;
    s.groupTick = sGroupTick;
    return s;
}

bool Active() {
    return sPhase == Phase::Running || sPhase == Phase::Joining;
}

std::string SlotName(int slot) {
    std::lock_guard<std::mutex> lock(sMutex);
    for (auto& p : sPlayers) {
        if (p.slot == slot) {
            return p.name;
        }
    }
    return "P" + std::to_string(slot + 1);
}

} // namespace Zmp::Lockstep

extern "C" s32 Zmp_AllowSaveWrite(void) {
    if (!gZmpSim.enabled) {
        return 1;
    }
    return Zmp::Lockstep::GetStatus().leader ? 1 : 0;
}
