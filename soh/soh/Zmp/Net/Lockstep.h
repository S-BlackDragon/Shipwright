#pragma once

// ZMP lockstep session (PLAN.md 2.2, docs/PROTOCOLO.md): the game ticks only when the server's bundle for
// the tick arrived; the local pad is sent for tick + D; founding, joining, rejoin and resync go through
// portable save states of the group leader.

#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace Zmp::Lockstep {

// Detached (phase 5): this client's player walked out of its group's scene alone. Its game goes on by itself through
// the scene change while it asks the server for the group of the destination (or founds it once it has arrived).
enum class Phase { Idle, WaitingGroup, Joining, Running, Detached };

struct SlotInfo {
    int slot = -1;
    std::string name;
    std::string state;
    int rtt = -1;
    bool leader = false;
    uint32_t id = 0;
    int scene = -1;     // phase 5: scene of the player's group (-1 unknown)
    uint32_t group = 0; // phase 5: its group (0: none)
};

// "X ha entrado en <scene>" (phase 5, presentation).
struct SceneNotice {
    std::string name;
    int scene = -1;
    double age = 0; // seconds
};

struct Status {
    Phase phase = Phase::Idle;
    int slot = -1;
    uint32_t tick = 0;
    int delay = 2;
    size_t queued = 0;
    bool waiting = false;   // tick gate closed for more than 250 ms
    int waitMs = 0;         // time the current tick has been waiting
    std::string waitingFor; // names from the server's WAIT message
    uint32_t stalls = 0;    // waits over 250 ms
    uint32_t maxStallMs = 0;
    uint32_t resyncs = 0;     // RESYNC messages that targeted this client
    uint32_t resyncsSeen = 0; // RESYNC messages received
    uint32_t lastResyncTick = 0;
    bool leader = false;
    uint32_t groupTick = 0; // last tick emitted by the server
    std::string lastError;
    std::vector<SlotInfo> players;
    int countdown = 0;       // phase 2-4 group scene change countdown; always 0 since phase 5
    uint32_t groupId = 0;    // phase 5: this client's group
    int groupScene = -1;     // phase 5: scene of this client's group
    int detachScene = -1;    // phase 5: scene this client is heading to while detached
    bool freeRun = false;    // phase 5: the local game runs on its own (detached, or joining after a detach)
    bool ownsSave = false;   // phase 5: this PC writes the save file (the player who started the room's game)
    uint32_t sharedSent = 0; // phase 5: patches of the shared game sent / received from other groups
    uint32_t sharedApplied = 0;
    std::string lastShared;  // description of the last patch received
    uint32_t groupJoins = 0; // phase 5: groups founded or joined in this connection
    int lastSpawnMs = -1;    // phase 5b: until this player's Link appeared in the group it entered
    int lastEntryLead = 0;   // D-094: how many of its inputs arrived late in that entry (one tick of lead each)
    int nameTags = 0;        // name tags over the Links shown on this screen
    // Diagnosis (harness): the buttons of the last input this machine sent, the tick the next one is for, the tick
    // after the newest one the server sent, and whether the local pad is blocked (own pause menu).
    uint32_t lastSentButtons = 0;
    uint32_t nextInputTick = 0;
    uint32_t nextServerTick = 0;
    bool localInputBlocked = false;
    uint64_t ticksPlayed = 0; // ticks this machine has played in groups since it started (the tests' game clock)
    uint32_t tickCostUs = 0;  // what one tick of the simulation takes on this machine now (moving average)
    bool inputsHeld = false;  // (test builds) this machine is not sending its inputs for a moment: TestHoldInputs
    // Phase 5b (D-073): group states loaded from a process that had the executable at another address, and the
    // pointers moved / left alone (unaligned look-alikes) in the last one.
    uint32_t relocatedLoads = 0;
    uint32_t lastRelocated = 0;
    uint32_t lastRelocatedOdd = 0;
    bool covered = false; // phase 5b: entering a scene's group, the screen stays black until this player's Link is in
    bool catchingUp = false; // phase 5b: the group's state is loaded, this player's Link is not in it yet
    int lastJoinMs = -1;     // phase 5: time from leaving a scene to playing in the next group (ms)
    std::vector<SceneNotice> sceneNotices;
    std::string endNotice;    // "X ha terminado la partida" for 15 s after a group game over "no" (D-058)
    bool saveSkipped = false; // this machine skipped a save in the last 3 s (not the leader)
};

const char* PhaseName(Phase phase);

// Network thread.
void OnConnected(bool rejoin);
void OnDisconnected();
void OnNetMessage(nlohmann::json&& msg);

// Game thread.
void OnFrameBegin();
bool ShouldRunTick();
void OnPadRead(void* pads);
void OnTickEnd(uint32_t tick, uint64_t hash);
// Presentation speed-up while this client is behind the group (queued bundles).
int CatchUpSpeed();
// Longest time the tick gate stayed closed (waiting for the server's next tick) since the last call.
int GateWaitMs();
// Fast test suite (test builds only; always 1 in the demo): how many times faster than real time the lockstep runs.
int TimeScale();
// Leaves the group (LEAVE_GROUP): the local Link disappears for the others.
void Leave();
// Console command applied by every client at the same tick (lockstep EVENT).
void SendConsoleEvent(const std::string& cmd);
// Applies a console event of `slot` (game events like "zmp_equip", or a console command run in that player's context).
void ApplyConsoleEvent(uint32_t tick, int slot, const std::string& cmd);
// The leader saved the game: the other players' blocks are written next to it (Save/zmp-players-fileN.txt).
void SaveGroupBlocks(int fileNum);
// True while this client's pause menu is open: its input to the group is neutral (PLAN.md 2.7).
void SetLocalInputBlocked(bool blocked);
// Test builds: this machine sends no input for `ms` milliseconds (its game goes on with the ticks it has): a player
// whose input is late, on purpose.
void TestHoldInputs(int ms);
// Test builds: this machine decides its inputs as if the group's ticks reached it `ticks` ticks late (a connection
// with that much latency, without any clock involved).
void TestTickViewLag(int ticks);
// Phase 5: the local player walked out of the scene alone (Zmp_TransitionGate): leave the group, let the scene change
// happen here and enter (or found) the group of the destination.
// `solo`: the destination starts with a scripted cutscene of this player (blue warp): it plays it in a group of its
// own and joins the others of that scene when it ends (phase 5b).
void DetachForTransition(int entrance, bool solo = false);
// Phase 5: groups are per scene (protocol 3). Always true in this build while in a group.
bool SceneGroups();
// Scene name for the player list and notices (Spanish for the common ones).
std::string SceneName(int scene);

Status GetStatus();
bool Active();
std::string SlotName(int slot);
// Name of the player that answers the group game over menu, when it is shown and that player is not this one.
std::string GameOverChooser();

} // namespace Zmp::Lockstep
