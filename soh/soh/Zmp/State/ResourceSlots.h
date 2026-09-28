#pragma once

// ZMP: deterministic addresses for game resources (PLAN.md 8.3, docs/DECISIONES.md D-017).
//
// Game memory (the system heap and the static data of the game code) holds raw pointers to loaded
// resources: skeleton limb tables, room and scene data, collision headers, display lists, paths.
// Those resources are allocated by the C++ resource manager, so in a normal process their addresses
// depend on load order and on the allocator. To make a save state loadable by another process, every
// resource is parsed into its own "slot": a fixed virtual address range derived from the resource
// path (its index in the sorted list of files of the loaded archives). Parsing the same file with the
// same binary performs the same allocations in the same order, so the resource ends up at the same
// address in every process. The slot allocator is a bump allocator; memory freed while parsing is
// not reused (it is small) and resources are never moved.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace Ship {
class ResourceFactory;
class IResource;
} // namespace Ship

namespace Zmp::ResSlots {

// Wraps a resource factory so that everything it allocates while parsing goes to the slot of the
// resource being loaded. Used where OTRGlobals registers the game resource factories.
std::shared_ptr<Ship::ResourceFactory> Wrap(std::shared_ptr<Ship::ResourceFactory> factory);

// Loads a resource from inside a factory (a skeleton loading its limbs, a scene its collision
// header). The nested load must not allocate in the parent's slot: whether the child is already
// cached differs between processes and would shift the parent's allocations.
std::shared_ptr<Ship::IResource> NestedLoad(const std::string& path);

struct SlotInfo {
    std::string path;
    uint32_t index = 0;
    uint64_t base = 0;
    uint64_t used = 0;
    uint32_t loads = 0; // number of parses that used this slot (1 in the normal case)
};

// Slots in use, sorted by index (for the save state manifest).
std::vector<SlotInfo> ListSlots();
// Hash of the sorted archive file list the slot indices come from (two processes can only share
// resource addresses if this matches).
uint64_t FileListHash();
bool IsActive();
bool Owns(const void* p);
// Number of allocations that could not use a slot (slot overflow, index missing).
uint64_t FallbackCount();

} // namespace Zmp::ResSlots
