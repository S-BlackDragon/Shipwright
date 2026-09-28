#include "z64.h"
#include <assert.h>
#if !defined(__APPLE__) && !defined(__OpenBSD__)
#include <malloc.h>
#endif
#include <stdlib.h>
#include <string.h>                  // ZMP: memset
#include "soh/Zmp/State/FixedHeap.h" // ZMP: system heap at a fixed virtual address

#ifndef _MSC_VER
#include <unistd.h>
#endif

u8* gAudioHeap;
u8* gSystemHeap;

void Heaps_Alloc(void) {
#ifdef _MSC_VER
    gAudioHeap = (u8*)_aligned_malloc(AUDIO_HEAP_SIZE, 0x10);
    // ZMP: the system heap lives at the same virtual address in every process so a save state (a copy of
    // the heap, whose internal pointers are absolute) can be loaded by another process (PLAN.md 8.3).
    gSystemHeap = (u8*)Zmp_AllocFixedHeap(SYSTEM_HEAP_SIZE);
    if (gSystemHeap == NULL) {
        gSystemHeap = (u8*)_aligned_malloc(SYSTEM_HEAP_SIZE, 0x10);
    }
#elif defined(_POSIX_VERSION) && (_POSIX_VERSION >= 200112L)
    if (posix_memalign((void**)&gAudioHeap, 0x10, AUDIO_HEAP_SIZE) != 0)
        gAudioHeap = NULL;
    if (posix_memalign((void**)&gSystemHeap, 0x10, SYSTEM_HEAP_SIZE) != 0)
        gSystemHeap = NULL;
#else
    gAudioHeap = (u8*)memalign(0x10, AUDIO_HEAP_SIZE);
    gSystemHeap = (u8*)memalign(0x10, SYSTEM_HEAP_SIZE);
#endif

    // ZMP: start both heaps zeroed so uninitialized reads are identical on every machine
    // (determinism, PLAN.md 2.4 point 2).
    if (gAudioHeap != NULL) {
        memset(gAudioHeap, 0, AUDIO_HEAP_SIZE);
    }
    if (gSystemHeap != NULL) {
        memset(gSystemHeap, 0, SYSTEM_HEAP_SIZE);
    }

    assert(gAudioHeap != NULL);
    assert(gSystemHeap != NULL);
}

void Heaps_Free(void) {
#ifdef _MSC_VER
    _aligned_free(gAudioHeap);
    if (!Zmp_FreeFixedHeap(gSystemHeap)) { // ZMP
        _aligned_free(gSystemHeap);
    }
#else
    free(gAudioHeap);
    free(gSystemHeap);
#endif
}
