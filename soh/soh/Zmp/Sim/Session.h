#pragma once

// ZMP determinism session: tick counter, per-tick state hash, input recording and replay
// (.zmpinput), pause / run-until control used by the harness. Game thread only unless noted.

#include <cstdint>
#include <string>
#include <vector>

namespace Zmp::Sim {

enum class Mode { Off, Recording, Replaying, Net };

// How a recording starts. Cold: a new game with the debug save at an entrance (same as the map
// select), RNG seeded with `seed`. State: from a portable save state blob embedded in the file.
struct StartSpec {
    int entrance = 0x0EE; // ENTR_KOKIRI_FOREST_0
    bool adult = false;
    uint16_t dayTime = 0x8000;
    uint32_t seed = 0x5A4D5031; // "ZMP1"
};

struct PadRecord {
    uint32_t buttons = 0;
    int8_t stickX = 0;
    int8_t stickY = 0;
    int8_t rStickX = 0;
    int8_t rStickY = 0;
};
static_assert(sizeof(PadRecord) == 8, "PadRecord must stay 8 bytes (file format)");

struct Status {
    Mode mode = Mode::Off;
    bool armed = false;  // session started, waiting for the first Play tick
    bool paused = false; // tick gate closed by the harness
    uint32_t tick = 0;   // next tick to run (== number of ticks run in the session)
    bool hasHash = false;
    uint64_t lastHash = 0;     // hash at the end of tick - 1
    uint32_t replayLength = 0; // ticks in the replay file
    bool replayFinished = false;
    uint32_t checked = 0; // ticks compared against the reference
    uint32_t mismatches = 0;
    uint32_t firstMismatch = UINT32_MAX;
    uint32_t pauseAt = UINT32_MAX;
    int speed = 1;
    std::string file;
};

Status GetStatus();
uint32_t CurrentTick();
bool InSession();

// Recording. `fromState`: start from a save state of the current game instead of a new game.
bool StartRecording(const std::string& path, const StartSpec& spec, bool fromState, std::string* err);
bool StopRecording(std::string* err, uint32_t* ticks = nullptr);
// In-memory checkpoints while recording: rewind loads the checkpoint state and truncates the
// recording to its tick, so a failed attempt leaves no trace in the file.
bool Checkpoint(const std::string& name, std::string* err, uint32_t* tick = nullptr);
bool Rewind(const std::string& name, std::string* err, uint32_t* tick = nullptr);

// Recorded event: a console command executed at the start of the current tick (before the game
// update), in the recording and at the same tick in every replay. Used for scripted segment
// boundaries (a debug warp) that are not controller input.
bool RecordEvent(const std::string& consoleCommand, std::string* err, uint32_t* tick = nullptr);

// Replay. `fromStatePath` (optional): load that save state first and continue the replay from
// its tick (the resume demo); otherwise the file's own start (new game or embedded state).
bool StartReplay(const std::string& path, const std::string& fromStatePath, std::string* err);
// The next session start (first replayed/recorded tick) leaves the tick gate closed (tests).
void PauseOnActivate(bool pause);
void StopSession();

void SetPaused(bool paused);
void SetPauseAt(uint32_t tick);
void SetSpeed(int speed);

bool HashAt(uint32_t tick, uint64_t* out);
bool ReferenceHashAt(uint32_t tick, uint64_t* out);

// Save states within the session (tick continuity). Load resets the tick to the blob's tick.
bool SaveStateFile(const std::string& path, std::string* err, std::string* info);
bool LoadStateFile(const std::string& path, std::string* err, std::string* info);

// Harness sabotage (test builds): changes an actor's health outside the simulation.
bool DebugSetActorHealth(int category, int index, int health, std::string* err);

// New game with the debug save at an entrance, outside any session (harness game.new).
void StartDebugGame(const StartSpec& spec);

// Called by Zmp_OnPadRead after the harness injected its input: replaces or records the pads.
void OnPadRead(void* pads);
// Autostart from CVars (gZmp.Replay.Path) once the game reached the file select screen, pending
// session starts, CVar profile enforcement.
void OnFrameBegin();
// End of a logic tick: hash, replay check, pause-at.
void OnTickEnd();

// Lockstep (Net mode, Zmp/Net/Lockstep.cpp): the session tick continues from `tick`; the tick gate belongs
// to the lockstep; the CVar profile is locked.
void BeginNet(uint32_t tick);
void EndNet();
// After loading a state within a Net session (resync).
void SetTick(uint32_t tick);
// logs/desync-<tick>.txt and .zmps with the current state (RESYNC diagnostics).
void WriteDesyncDump(uint32_t badTick);

uint64_t HashState(uint32_t tick);
bool DumpState(const std::string& path, uint32_t tick, std::string* err);

const char* ModeName(Mode mode);

} // namespace Zmp::Sim
