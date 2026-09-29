#pragma once

// Shared game between groups (phase 5, PLAN.md 2.11, docs/DECISIONES.md D-061).
//
// The progression every player shares (PLAN.md 2.6) is described as a compact buffer: owned items and equipment,
// upgrades, quest items, dungeon items and keys, heart containers, magic, rupees, gold skulltula tokens, the
// permanent flags of every scene and the event / item / info flags. The flags of the current scene are the live ones
// (play->actorCtx.flags), not the copy the save context gets when the scene ends.
//
// Every group keeps a baseline of that buffer in its simulation (gZmpSim, it travels in the state blob). At the end of
// each tick every member computes the same patch (changes since the baseline), sends it (the server takes the first)
// and moves the baseline. Patches of other groups arrive as TICK events and are applied at the start of the tick, to
// the game and to the baseline, so they are never sent back. Bit fields merge bit by bit and counters by their
// difference, so two groups changing the same field at the same time both keep their change.

#include <cstdint>
#include <string>
#include <vector>

namespace Zmp::SharedGame {

// Identifies the layout (fields, sizes, operations, save context size). A canonical game of another layout is
// replaced instead of merged.
uint32_t LayoutHash();
size_t Size();

// Current shared game (live flags of the current scene included).
std::vector<uint8_t> Snapshot();

// Changes from `base` to `cur` (same size), as a patch (server format, see server/internal/server/shared.go).
std::vector<uint8_t> Diff(const std::vector<uint8_t>& base, const std::vector<uint8_t>& cur);

// Applies a patch to the game (save context and the live flags of the current scene). Returns false if malformed.
bool ApplyToGame(const std::vector<uint8_t>& patch, std::string* summary = nullptr);
// Applies a patch to a buffer of the compact layout.
bool ApplyToBuffer(std::vector<uint8_t>& buf, const std::vector<uint8_t>& patch);

// Baseline kept in the simulation (gZmpSim).
bool HasBaseline();
std::vector<uint8_t> Baseline();
void SetBaseline(const std::vector<uint8_t>& buf);
void ClearBaseline();

// End of a tick of a group member: patch since the baseline (empty: nothing changed); the baseline moves.
std::vector<uint8_t> TakeTickPatch();

// Founding a group: merges the room's canonical game into the current one keeping this client's changes made since
// its baseline (none for a fresh game). `canonical` empty: the server took this client's game (`sent`).
void MergeOnFound(const std::vector<uint8_t>& canonical, const std::vector<uint8_t>& sent);

// Short readable description of a patch (logs, harness).
std::string Describe(const std::vector<uint8_t>& patch);

} // namespace Zmp::SharedGame
