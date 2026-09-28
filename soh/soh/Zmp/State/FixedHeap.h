#pragma once

// ZMP: fixed virtual addresses shared by every process (portable save states, PLAN.md 8.3).

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Address of the system heap in every ZMP process. Heap pointers stored inside the heap and in the
// save state are absolute, so the heap must live at the same address everywhere.
#define ZMP_SYSTEM_HEAP_ADDR 0x0000100000000000ULL
// Base of the region where loaded game resources (skeletons, rooms, collision, display lists) are
// placed at a deterministic address per resource path (see ResourceSlots.h).
#define ZMP_RESOURCE_REGION_ADDR 0x0000200000000000ULL

// Reserves and commits `size` bytes at ZMP_SYSTEM_HEAP_ADDR (zero filled). Returns NULL if the
// address is not available; the caller then falls back to a normal allocation and save states can
// only be loaded by processes that got the same address.
void* Zmp_AllocFixedHeap(size_t size);
// Frees the heap if it was allocated by Zmp_AllocFixedHeap. Returns false otherwise.
int Zmp_FreeFixedHeap(void* heap);
// Whether the system heap got its fixed address.
int Zmp_FixedHeapActive(void);

#ifdef __cplusplus
}
#endif
