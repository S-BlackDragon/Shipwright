#include "FixedHeap.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

static void* sFixedHeap = nullptr;

extern "C" void* Zmp_AllocFixedHeap(size_t size) {
#ifdef _WIN32
    void* p = VirtualAlloc((void*)(uintptr_t)ZMP_SYSTEM_HEAP_ADDR, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (p != (void*)(uintptr_t)ZMP_SYSTEM_HEAP_ADDR) {
        if (p != nullptr) {
            VirtualFree(p, 0, MEM_RELEASE);
        }
        return nullptr;
    }
    // VirtualAlloc returns zeroed pages.
    sFixedHeap = p;
    return p;
#else
    (void)size;
    return nullptr;
#endif
}

extern "C" int Zmp_FreeFixedHeap(void* heap) {
#ifdef _WIN32
    if (heap != nullptr && heap == sFixedHeap) {
        VirtualFree(heap, 0, MEM_RELEASE);
        sFixedHeap = nullptr;
        return 1;
    }
#endif
    (void)heap;
    return 0;
}

extern "C" int Zmp_FixedHeapActive(void) {
    return sFixedHeap != nullptr;
}
