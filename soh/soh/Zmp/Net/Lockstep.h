#pragma once

// ZMP lockstep session (PLAN.md 2.2, docs/PROTOCOLO.md): the game ticks only when the server's bundle for
// the tick arrived; the local pad is sent for tick + D; founding, joining, rejoin and resync go through
// portable save states of the group leader.

#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace Zmp::Lockstep {

enum class Phase { Idle, WaitingGroup, Joining, Running };

struct SlotInfo {
    int slot = -1;
    std::string name;
    std::string state;
    int rtt = -1;
    bool leader = false;
    uint32_t id = 0;
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
    int countdown = 0;        // group scene change countdown (ticks)
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

Status GetStatus();
bool Active();
std::string SlotName(int slot);
// Name of the player that answers the group game over menu, when it is shown and that player is not this one.
std::string GameOverChooser();

} // namespace Zmp::Lockstep
