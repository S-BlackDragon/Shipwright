#pragma once

// ZMP portable save state (PLAN.md 2.5 and 8.3): the simulation state serialized to bytes, zstd
// compressed, loadable by another process of the same build. Audio is not included.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace Zmp::State {

struct BlobInfo {
    uint32_t version = 0;
    uint32_t tick = 0; // next tick to run after loading
    int32_t scene = -1;
    uint64_t hash = 0; // state hash at the end of tick - 1
    uint64_t rawSize = 0;
    uint64_t compressedSize = 0;
    uint32_t resources = 0;
    double ms = 0.0;   // time spent saving or loading (including compression)
    std::string notes; // warnings (e.g. statics that did not match after loading)
    // Phase 5b: pointers to the executable moved because the saving process had it at another address (0 when
    // both have it at the same one), and values that looked like such pointers at unaligned offsets (left alone).
    uint32_t relocated = 0;
    uint32_t relocatedOdd = 0;
};

// Serializes the current state. Must be called between ticks on the game thread, in Play.
bool Save(std::vector<uint8_t>& out, uint32_t tick, uint64_t hash, std::string* err, BlobInfo* info = nullptr);
// Loads a blob produced by Save (in this or another process). The current game state must be
// Play. On success the caller sets the session tick to info->tick.
bool Load(const std::vector<uint8_t>& blob, std::string* err, BlobInfo* info = nullptr);
// Reads only the header.
bool Peek(const std::vector<uint8_t>& blob, BlobInfo* info, std::string* err);

// Test (finding X): the game thread names statics for `ms` milliseconds (see StateBlob.cpp).
int TestNamingStall(int ms);
bool ReadFile(const std::string& path, std::vector<uint8_t>& out, std::string* err);
bool WriteFile(const std::string& path, const std::vector<uint8_t>& data, std::string* err);

// Diagnostic: words in the system heap that look like pointers outside the heap, the executable
// image and the resource region (they would not survive a load in another process), grouped by
// memory region. Written to `path`.
bool Census(const std::string& path, std::string* summary);

} // namespace Zmp::State

namespace Zmp::State {
// Lockstep sessions: loads keep the language and Z-target setting of the blob (shared game).
void SetKeepSharedSettings(bool keep);
} // namespace Zmp::State
