#include "Session.h"
#include "ZmpSim.h"
#include "CVarProfile.h"

#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>

#include <ship/Context.h>
#include <ship/debug/Console.h>
#include <libultraship/bridge/consolevariablebridge.h>
#include <libultraship/libultra/controller.h>

#include "soh/Zmp/ZmpCVars.h"
#include "soh/Zmp/ZmpLog.h"
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
void Play_Main(GameState* thisx);
void Play_Init(GameState* thisx);
void Sram_InitDebugSave(void);
}

namespace Zmp::Sim {

namespace {

constexpr uint32_t kInputMagic = 0x49504D5A; // "ZMPI"
constexpr uint32_t kInputVersion = 1;

#pragma pack(push, 1)
struct InputHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t headerSize;
    uint32_t startKind; // 0 = new game (debug save), 1 = embedded save state
    int32_t entrance;
    uint8_t adult;
    uint8_t reserved0;
    uint16_t dayTime;
    uint32_t seed;
    uint32_t startTick;
    uint32_t tickCount;
    uint64_t cvarProfileHash;
    uint32_t blobSize;
    uint32_t flags;
    char note[64];
};
#pragma pack(pop)

enum class Pending { None, ColdStart, StateStart };

Mode sMode = Mode::Off;
Pending sPending = Pending::None;
bool sArmed = false;
bool sPaused = false;
uint32_t sPauseAt = UINT32_MAX;
int sSpeed = 1;
uint32_t sPendingFrames = 0;

StartSpec sSpec;
bool sFromState = false;
std::vector<uint8_t> sStartBlob; // recording/replay start state (StateStart)
std::string sFile;

uint32_t sTick = 0;      // next tick to run
uint32_t sStartTick = 0; // tick of the first recorded/replayed pad
std::vector<uint64_t> sHashes;
uint32_t sHashBase = 0;
bool sHasHash = false;
uint64_t sLastHash = 0;

std::vector<PadRecord> sPads;     // recorded pads, or the replay's pads
std::vector<uint64_t> sRefHashes; // replay reference hashes (index = tick - sStartTick)
std::vector<uint64_t> sRecHashes; // hashes recorded with the pads
struct Event {
    uint32_t tick;
    std::string command;
};
std::vector<Event> sEvents; // recorded or replayed events, sorted by tick
uint64_t sFileProfileHash = 0;
bool sReplayFinished = false;
uint32_t sChecked = 0;
uint32_t sMismatches = 0;
uint32_t sFirstMismatch = UINT32_MAX;

struct CheckpointData {
    std::vector<uint8_t> blob;
    uint32_t tick;
};
std::map<std::string, CheckpointData> sCheckpoints;

bool sAutostartDone = false;
bool sReturnRequested = false;

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

void ResetHashes(uint32_t base) {
    sHashes.clear();
    sHashBase = base;
    sHasHash = false;
}

// Same steps as the map select (z_select.c Select_LoadGame) with the debug save. The whole save
// context is cleared first (except per-client settings) so a new game starts from the same bytes
// whatever ran before in this process.
void StartDebugGameImpl(const StartSpec& spec, bool clearSave) {
    if (clearSave) {
        u8 language = gSaveContext.language;
        u8 audioSetting = gSaveContext.audioSetting;
        u8 zTargetSetting = gSaveContext.zTargetSetting;
        memset(&gSaveContext, 0, sizeof(gSaveContext));
        gSaveContext.language = language;
        gSaveContext.audioSetting = audioSetting;
        gSaveContext.zTargetSetting = zTargetSetting;
    }
    gSaveContext.fileNum = 0xFF;
    Sram_InitDebugSave();
    gSaveContext.magicFillTarget = gSaveContext.magic;
    gSaveContext.magic = 0;
    gSaveContext.magicCapacity = 0;
    gSaveContext.magicLevel = gSaveContext.magic;
    gSaveContext.linkAge = spec.adult ? LINK_AGE_ADULT : LINK_AGE_CHILD;
    gSaveContext.dayTime = spec.dayTime;
    gSaveContext.nightFlag = (spec.dayTime >= 0xC001 || spec.dayTime < 0x4555) ? 1 : 0;
    GameInteractor_ExecuteOnLoadGame(gSaveContext.fileNum);
    for (int i = 0; i < ARRAY_COUNT(gSaveContext.buttonStatus); i++) {
        gSaveContext.buttonStatus[i] = BTN_ENABLED;
    }
    gSaveContext.forceRisingButtonAlphas = 0;
    gSaveContext.nextHudVisibilityMode = 0;
    gSaveContext.hudVisibilityMode = 0;
    gSaveContext.hudVisibilityModeTimer = 0;
    gSaveContext.entranceIndex = spec.entrance;
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
}

void ReturnToFileSelect() {
    gSaveContext.gameMode = GAMEMODE_FILE_SELECT;
    SET_NEXT_GAMESTATE(gGameState, FileChoose_Init, FileChooseContext);
    gGameState->running = false;
}

bool WriteInputFile(std::string* err) {
    uint32_t count = (uint32_t)std::min(sPads.size(), sRecHashes.size());
    InputHeader h = {};
    h.magic = kInputMagic;
    h.version = kInputVersion;
    h.headerSize = sizeof(InputHeader);
    h.startKind = sFromState ? 1 : 0;
    h.entrance = sSpec.entrance;
    h.adult = sSpec.adult ? 1 : 0;
    h.dayTime = sSpec.dayTime;
    h.seed = sSpec.seed;
    h.startTick = sStartTick;
    h.tickCount = count;
    h.cvarProfileHash = CVarProfile::Hash();
    h.blobSize = sFromState ? (uint32_t)sStartBlob.size() : 0;
    snprintf(h.note, sizeof(h.note), "ZMP input recording");
    std::vector<uint8_t> out(sizeof(h));
    memcpy(out.data(), &h, sizeof(h));
    if (sFromState) {
        out.insert(out.end(), sStartBlob.begin(), sStartBlob.end());
    }
    const uint8_t* pads = (const uint8_t*)sPads.data();
    out.insert(out.end(), pads, pads + count * sizeof(PadRecord));
    const uint8_t* hashes = (const uint8_t*)sRecHashes.data();
    out.insert(out.end(), hashes, hashes + count * sizeof(uint64_t));
    // Events: u32 count, then per event u32 tick, u16 length, command bytes.
    uint32_t nEvents = 0;
    for (auto& e : sEvents) {
        if (e.tick < sStartTick + count) {
            nEvents++;
        }
    }
    auto put = [&out](const void* p, size_t n) {
        const uint8_t* b = (const uint8_t*)p;
        out.insert(out.end(), b, b + n);
    };
    put(&nEvents, sizeof(nEvents));
    for (auto& e : sEvents) {
        if (e.tick < sStartTick + count) {
            uint16_t len = (uint16_t)e.command.size();
            put(&e.tick, sizeof(e.tick));
            put(&len, sizeof(len));
            put(e.command.data(), len);
        }
    }
    return State::WriteFile(sFile, out, err);
}

bool ReadInputFile(const std::string& path, std::string* err) {
    std::vector<uint8_t> data;
    if (!State::ReadFile(path, data, err)) {
        return false;
    }
    if (data.size() < sizeof(InputHeader)) {
        *err = path + " is not a .zmpinput file";
        return false;
    }
    InputHeader h;
    memcpy(&h, data.data(), sizeof(h));
    if (h.magic != kInputMagic || h.version != kInputVersion) {
        *err = path + ": not a ZMP input recording (or another version)";
        return false;
    }
    size_t need = h.headerSize + (size_t)h.blobSize + (size_t)h.tickCount * (sizeof(PadRecord) + sizeof(uint64_t));
    if (data.size() < need) {
        *err = path + " is truncated";
        return false;
    }
    sFromState = h.startKind == 1;
    sSpec.entrance = h.entrance;
    sSpec.adult = h.adult != 0;
    sSpec.dayTime = h.dayTime;
    sSpec.seed = h.seed;
    sStartTick = h.startTick;
    sFileProfileHash = h.cvarProfileHash;
    size_t off = h.headerSize;
    sStartBlob.assign(data.begin() + off, data.begin() + off + h.blobSize);
    off += h.blobSize;
    sPads.resize(h.tickCount);
    memcpy(sPads.data(), data.data() + off, h.tickCount * sizeof(PadRecord));
    off += h.tickCount * sizeof(PadRecord);
    sRefHashes.resize(h.tickCount);
    memcpy(sRefHashes.data(), data.data() + off, h.tickCount * sizeof(uint64_t));
    off += h.tickCount * sizeof(uint64_t);
    sEvents.clear();
    uint32_t nEvents = 0;
    if (off + sizeof(nEvents) <= data.size()) {
        memcpy(&nEvents, data.data() + off, sizeof(nEvents));
        off += sizeof(nEvents);
        for (uint32_t i = 0; i < nEvents && off + 6 <= data.size(); i++) {
            Event e;
            uint16_t len = 0;
            memcpy(&e.tick, data.data() + off, 4);
            memcpy(&len, data.data() + off + 4, 2);
            off += 6;
            if (off + len > data.size()) {
                break;
            }
            e.command.assign((const char*)data.data() + off, len);
            off += len;
            sEvents.push_back(e);
        }
    }
    return true;
}

void BeginActive(uint32_t tick) {
    sArmed = false;
    sTick = tick;
    ResetHashes(tick);
    sReplayFinished = false;
    sChecked = 0;
    sMismatches = 0;
    sFirstMismatch = UINT32_MAX;
    Log(std::string("zmp: session ") + ModeName(sMode) + " active at tick " + std::to_string(tick) + " (" + sFile +
        ")");
    if (sMode == Mode::Replaying && sFileProfileHash != 0 && sFileProfileHash != CVarProfile::Hash()) {
        Log("zmp: AVISO: el perfil de CVars bloqueadas difiere del de la grabacion (" + Hex(sFileProfileHash) + " vs " +
            Hex(CVarProfile::Hash()) + ")");
    }
}

bool LoadBlobAndActivate(const std::vector<uint8_t>& blob, std::string* err) {
    State::BlobInfo info;
    if (!State::Load(blob, err, &info)) {
        return false;
    }
    BeginActive(info.tick);
    uint64_t h = HashState(info.tick - 1);
    Log("zmp: state loaded at tick " + std::to_string(info.tick) + " in " + std::to_string(info.ms) + " ms, hash " +
        (h == info.hash ? "OK" : ("MISMATCH " + Hex(h) + " vs " + Hex(info.hash))) +
        (info.notes.empty() ? "" : (" | " + info.notes)));
    return true;
}

void DesyncDump(uint32_t tick) {
    std::error_code ec;
    std::filesystem::create_directories("logs", ec);
    std::string base = "logs/desync-" + std::to_string(tick);
    std::string err;
    DumpState(base + ".txt", tick, &err);
    std::vector<uint8_t> blob;
    if (InPlay() && State::Save(blob, tick + 1, sLastHash, &err)) {
        State::WriteFile(base + ".zmps", blob, &err);
    }
    Log("zmp: HASH MISMATCH at tick " + std::to_string(tick) + ", dump in " + base + ".txt");
}

void HandlePending() {
    std::string err;
    if (sPending == Pending::ColdStart) {
        if (InPlay()) {
            if (!sReturnRequested) {
                ReturnToFileSelect();
                sReturnRequested = true;
            }
            sPendingFrames = 0;
            return;
        }
        if (InFileSelect()) {
            if (++sPendingFrames < 3) {
                return;
            }
            sPending = Pending::None;
            CVarProfile::BeginSession();
            StartDebugGameImpl(sSpec, true);
            sArmed = true;
            sTick = 0;
            ResetHashes(0);
            sPads.resize(sMode == Mode::Recording ? 0 : sPads.size());
            sRecHashes.clear();
            Log(std::string("zmp: ") + ModeName(sMode) + " armed: new game at entrance " +
                std::to_string(sSpec.entrance) + ", seed " + std::to_string(sSpec.seed));
        } else {
            sPendingFrames++;
        }
    } else if (sPending == Pending::StateStart) {
        if (InFileSelect()) {
            StartSpec plain;
            StartDebugGameImpl(plain, true);
            sPendingFrames = 0;
            return;
        }
        if (!InPlay()) {
            return;
        }
        if (++sPendingFrames < 5) {
            return;
        }
        sPending = Pending::None;
        CVarProfile::BeginSession();
        if (!LoadBlobAndActivate(sStartBlob, &err)) {
            Log("zmp: replay start failed: " + err);
            sMode = Mode::Off;
            CVarProfile::EndSession();
        }
    }
}

} // namespace

void StartDebugGame(const StartSpec& spec) {
    StartDebugGameImpl(spec, false);
}

const char* ModeName(Mode mode) {
    switch (mode) {
        case Mode::Recording:
            return "recording";
        case Mode::Replaying:
            return "replaying";
        default:
            return "off";
    }
}

Status GetStatus() {
    Status s;
    s.mode = sMode;
    s.armed = sArmed || sPending != Pending::None;
    s.paused = sPaused;
    s.tick = sTick;
    s.hasHash = sHasHash;
    s.lastHash = sLastHash;
    s.replayLength = sMode == Mode::Replaying ? (uint32_t)sPads.size() : 0;
    s.replayFinished = sReplayFinished;
    s.checked = sChecked;
    s.mismatches = sMismatches;
    s.firstMismatch = sFirstMismatch;
    s.pauseAt = sPauseAt;
    s.speed = sSpeed;
    s.file = sFile;
    if (sMode == Mode::Recording) {
        s.replayLength = (uint32_t)sPads.size();
    }
    return s;
}

uint32_t CurrentTick() {
    return sTick;
}

bool InSession() {
    return sMode != Mode::Off;
}

bool StartRecording(const std::string& path, const StartSpec& spec, bool fromState, std::string* err) {
    if (gGameState == nullptr) {
        *err = "the game has not started yet";
        return false;
    }
    StopSession();
    sFile = path;
    sSpec = spec;
    sFromState = fromState;
    sPads.clear();
    sRecHashes.clear();
    sRefHashes.clear();
    sEvents.clear();
    sCheckpoints.clear();
    if (fromState) {
        if (!InPlay()) {
            *err = "recording from the current state needs a game in progress";
            return false;
        }
        CVarProfile::BeginSession();
        sStartBlob.clear();
        if (!State::Save(sStartBlob, sTick, sLastHash, err)) {
            CVarProfile::EndSession();
            return false;
        }
        sMode = Mode::Recording;
        sStartTick = sTick;
        BeginActive(sTick);
        return true;
    }
    sStartBlob.clear();
    sStartTick = 0;
    sMode = Mode::Recording;
    sPending = Pending::ColdStart;
    sPendingFrames = 0;
    sReturnRequested = false;
    return true;
}

bool StopRecording(std::string* err, uint32_t* ticks) {
    if (sMode != Mode::Recording) {
        *err = "not recording";
        return false;
    }
    bool ok = WriteInputFile(err);
    if (ticks != nullptr) {
        *ticks = (uint32_t)std::min(sPads.size(), sRecHashes.size());
    }
    Log("zmp: recording stopped: " + std::to_string(std::min(sPads.size(), sRecHashes.size())) + " ticks -> " + sFile +
        (ok ? "" : (" FAILED: " + *err)));
    StopSession();
    return ok;
}

bool Checkpoint(const std::string& name, std::string* err, uint32_t* tick) {
    if (sMode != Mode::Recording || sArmed || sPending != Pending::None) {
        *err = "checkpoints need an active recording";
        return false;
    }
    CheckpointData cp;
    cp.tick = sTick;
    if (!State::Save(cp.blob, sTick, sLastHash, err)) {
        return false;
    }
    sCheckpoints[name] = std::move(cp);
    if (tick != nullptr) {
        *tick = sTick;
    }
    Log("zmp: checkpoint '" + name + "' at tick " + std::to_string(sTick));
    return true;
}

bool Rewind(const std::string& name, std::string* err, uint32_t* tick) {
    auto it = sCheckpoints.find(name);
    if (sMode != Mode::Recording || it == sCheckpoints.end()) {
        *err = "unknown checkpoint '" + name + "'";
        return false;
    }
    State::BlobInfo info;
    if (!State::Load(it->second.blob, err, &info)) {
        return false;
    }
    uint32_t keep = it->second.tick - sStartTick;
    sPads.resize(std::min<size_t>(sPads.size(), keep));
    sRecHashes.resize(std::min<size_t>(sRecHashes.size(), keep));
    while (!sEvents.empty() && sEvents.back().tick >= it->second.tick) {
        sEvents.pop_back();
    }
    sTick = it->second.tick;
    ResetHashes(sTick);
    if (tick != nullptr) {
        *tick = sTick;
    }
    Log("zmp: rewound to checkpoint '" + name + "' (tick " + std::to_string(sTick) + ")");
    return true;
}

bool RecordEvent(const std::string& consoleCommand, std::string* err, uint32_t* tick) {
    if (sMode != Mode::Recording || sArmed || sPending != Pending::None) {
        *err = "events need an active recording";
        return false;
    }
    sEvents.push_back({ sTick, consoleCommand });
    if (tick != nullptr) {
        *tick = sTick;
    }
    Log("zmp: event at tick " + std::to_string(sTick) + ": " + consoleCommand);
    return true;
}

bool StartReplay(const std::string& path, const std::string& fromStatePath, std::string* err) {
    if (gGameState == nullptr) {
        *err = "the game has not started yet";
        return false;
    }
    StopSession();
    if (!ReadInputFile(path, err)) {
        return false;
    }
    sFile = path;
    sMode = Mode::Replaying;
    sCheckpoints.clear();
    if (!fromStatePath.empty()) {
        std::vector<uint8_t> blob;
        if (!State::ReadFile(fromStatePath, blob, err)) {
            sMode = Mode::Off;
            return false;
        }
        sStartBlob = std::move(blob);
        sFromState = true;
    }
    if (sFromState) {
        if (InPlay()) {
            CVarProfile::BeginSession();
            if (!LoadBlobAndActivate(sStartBlob, err)) {
                sMode = Mode::Off;
                CVarProfile::EndSession();
                return false;
            }
        } else {
            sPending = Pending::StateStart;
            sPendingFrames = 0;
        }
    } else {
        sPending = Pending::ColdStart;
        sPendingFrames = 0;
        sReturnRequested = false;
    }
    Log("zmp: replay of " + path + " (" + std::to_string(sPads.size()) + " ticks from tick " +
        std::to_string(sStartTick) + ")");
    return true;
}

void StopSession() {
    if (sMode != Mode::Off) {
        Log(std::string("zmp: session ") + ModeName(sMode) + " stopped at tick " + std::to_string(sTick));
    }
    sMode = Mode::Off;
    sPending = Pending::None;
    sArmed = false;
    sPaused = false;
    sPauseAt = UINT32_MAX;
    CVarProfile::EndSession();
}

void SetPaused(bool paused) {
    sPaused = paused;
}

void SetPauseAt(uint32_t tick) {
    sPauseAt = tick;
    if (tick <= sTick && tick != UINT32_MAX) {
        sPaused = true;
        sPauseAt = UINT32_MAX;
    } else {
        sPaused = false;
    }
}

void SetSpeed(int speed) {
    sSpeed = speed < 0 ? 1 : speed;
}

bool HashAt(uint32_t tick, uint64_t* out) {
    if (tick < sHashBase || tick - sHashBase >= sHashes.size()) {
        return false;
    }
    *out = sHashes[tick - sHashBase];
    return true;
}

bool ReferenceHashAt(uint32_t tick, uint64_t* out) {
    if (sMode != Mode::Replaying || tick < sStartTick || tick - sStartTick >= sRefHashes.size()) {
        return false;
    }
    *out = sRefHashes[tick - sStartTick];
    return true;
}

bool SaveStateFile(const std::string& path, std::string* err, std::string* info) {
    std::vector<uint8_t> blob;
    State::BlobInfo bi;
    if (!State::Save(blob, sTick, sLastHash, err, &bi)) {
        return false;
    }
    if (!State::WriteFile(path, blob, err)) {
        return false;
    }
    char buf[256];
    snprintf(buf, sizeof(buf), "tick %u, %" PRIu64 " bytes (raw %" PRIu64 "), %u resources, %.1f ms", bi.tick,
             bi.compressedSize, bi.rawSize, bi.resources, bi.ms);
    *info = buf;
    Log("zmp: state saved to " + path + ": " + *info);
    return true;
}

bool LoadStateFile(const std::string& path, std::string* err, std::string* info) {
    std::vector<uint8_t> blob;
    if (!State::ReadFile(path, blob, err)) {
        return false;
    }
    State::BlobInfo bi;
    if (!State::Load(blob, err, &bi)) {
        return false;
    }
    sTick = bi.tick;
    ResetHashes(bi.tick);
    uint64_t h = HashState(bi.tick - 1);
    char buf[512];
    snprintf(buf, sizeof(buf), "tick %u, %.1f ms, hash %s%s%s", bi.tick, bi.ms, h == bi.hash ? "OK" : "MISMATCH",
             bi.notes.empty() ? "" : " | ", bi.notes.c_str());
    *info = buf;
    Log("zmp: state loaded from " + path + ": " + *info);
    return true;
}

bool DebugSetActorHealth(int category, int index, int health, std::string* err) {
    if (!InPlay() || category < 0 || category >= ACTORCAT_MAX) {
        *err = "not in play or bad category";
        return false;
    }
    Actor* a = gPlayState->actorCtx.actorLists[category].head;
    for (int i = 0; a != nullptr && i < index; i++) {
        a = a->next;
    }
    if (a == nullptr) {
        *err = "no such actor";
        return false;
    }
    a->colChkInfo.health = (u8)health;
    Log("zmp: SABOTAGE actor " + std::to_string(category) + "." + std::to_string(index) +
        " health = " + std::to_string(health));
    return true;
}

void OnPadRead(void* padsV) {
    OSContPad* pads = (OSContPad*)padsV;
    if (sMode == Mode::Off) {
        return;
    }
    if (sPending != Pending::None || sArmed) {
        if (sArmed && InPlay()) {
            BeginActive(0);
        } else {
            memset(pads, 0, sizeof(OSContPad) * 4);
            return;
        }
    }
    // Only port 0 is simulation input during a session; the other ports and the gyro are local.
    memset(&pads[1], 0, sizeof(OSContPad) * 3);
    pads[0].gyro_x = 0;
    pads[0].gyro_y = 0;
    pads[0].err_no = 0;
    // Events of this tick run before the game update, in the recording and in every replay.
    for (auto& e : sEvents) {
        if (e.tick == sTick) {
            std::string output;
            Ship::Context::GetRawInstance()->GetConsole()->Run(e.command, &output);
            Log("zmp: event at tick " + std::to_string(sTick) + ": " + e.command);
        }
    }
    uint32_t idx = sTick - sStartTick;
    if (sMode == Mode::Replaying) {
        if (idx < sPads.size()) {
            const PadRecord& r = sPads[idx];
            pads[0].button = (CONTROLLERBUTTONS_T)r.buttons;
            pads[0].stick_x = r.stickX;
            pads[0].stick_y = r.stickY;
            pads[0].right_stick_x = r.rStickX;
            pads[0].right_stick_y = r.rStickY;
        } else {
            pads[0] = OSContPad{};
            if (!sReplayFinished) {
                sReplayFinished = true;
                Log("zmp: replay finished at tick " + std::to_string(sTick) + ": " + std::to_string(sChecked) +
                    " ticks checked, " + std::to_string(sMismatches) + " mismatches");
            }
        }
    } else if (sMode == Mode::Recording) {
        PadRecord r;
        r.buttons = (uint32_t)pads[0].button;
        r.stickX = pads[0].stick_x;
        r.stickY = pads[0].stick_y;
        r.rStickX = pads[0].right_stick_x;
        r.rStickY = pads[0].right_stick_y;
        if (idx == sPads.size()) {
            sPads.push_back(r);
        }
    }
}

void OnFrameBegin() {
    if (!sAutostartDone && gGameState != nullptr && InFileSelect()) {
        sAutostartDone = true;
        std::string path = CVarGetString(ZMP_CVAR_REPLAY_PATH, "");
        if (!path.empty()) {
            std::string err;
            sSpeed = CVarGetInteger(ZMP_CVAR_REPLAY_SPEED, 1);
            if (!StartReplay(path, CVarGetString(ZMP_CVAR_REPLAY_FROM_STATE, ""), &err)) {
                Log("zmp: autostart replay failed: " + err);
            }
        }
    }
    if (sPending != Pending::None) {
        HandlePending();
    }
    CVarProfile::Enforce();
}

void OnTickEnd() {
    uint32_t t = sTick;
    bool counting = !(sPending != Pending::None || sArmed);
    if (!counting) {
        return; // waiting for the session's first tick: the tick counter stays where it is
    }
    uint64_t h = InPlay() ? HashState(t) : 0;
    if (t != sHashBase + sHashes.size()) {
        ResetHashes(t);
    }
    sHashes.push_back(h);
    if (sMode == Mode::Off && sHashes.size() > 200000) {
        sHashes.erase(sHashes.begin(), sHashes.begin() + 100000);
        sHashBase += 100000;
    }
    sHasHash = true;
    sLastHash = h;
    uint32_t idx = t - sStartTick;
    std::string refNote;
    if (sMode == Mode::Recording) {
        if (idx == sRecHashes.size()) {
            sRecHashes.push_back(h);
        }
    } else if (sMode == Mode::Replaying && idx < sRefHashes.size()) {
        sChecked++;
        if (sRefHashes[idx] != h) {
            sMismatches++;
            if (sFirstMismatch == UINT32_MAX) {
                sFirstMismatch = t;
                DesyncDump(t);
            }
            refNote = " ref=" + Hex(sRefHashes[idx]) + " MISMATCH";
        } else {
            refNote = " ref OK";
        }
    }
    int interval = CVarGetInteger(ZMP_CVAR_HASH_LOG_INTERVAL, 20);
    if (interval > 0 && InPlay() && (t % (uint32_t)interval) == 0) {
        Log("tick=" + std::to_string(t) + " hash=" + Hex(h) + refNote);
    }
    sTick = t + 1;
    if (sPauseAt != UINT32_MAX && sTick >= sPauseAt) {
        sPaused = true;
        sPauseAt = UINT32_MAX;
        Log("zmp: paused at tick " + std::to_string(sTick));
    }
}

} // namespace Zmp::Sim

extern "C" uint32_t Zmp_PlaySeed(uint32_t timeSeed) {
    using namespace Zmp::Sim;
    if (sMode == Mode::Off) {
        return timeSeed;
    }
    if (sArmed) {
        return sSpec.seed;
    }
    // In a session the RNG is never reseeded from the clock: keep its current state.
    return Rand_ZmpGetState(nullptr);
}

extern "C" int32_t Zmp_IgnoreUpdateCulling(void) {
    return 1;
}

extern "C" int32_t Zmp_ShouldRunTick(void) {
    return Zmp::Sim::sPaused ? 0 : 1;
}

extern "C" int32_t Zmp_PresentationSpeed(void) {
    return Zmp::Sim::sSpeed;
}
