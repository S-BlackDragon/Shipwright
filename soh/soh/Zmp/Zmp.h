#pragma once

// ZMP: entry points called from the game (C) side.

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Called once at the end of InitOTR, after the GUI and console exist.
void Zmp_Init(void);
void Zmp_InstallBootLog(void);       // finding R: a log of the start, written before anything else exists
void Zmp_BootMark(const char* what); // one line in that log
// Finding R: what the window object's "which backend" field held before anybody set it (a line in the boot log).
void Zmp_BootGuiBackendField(int value);
// Finding T, test builds: the mutants of the fixes made in the library (see Zmp.cpp).
void Zmp_ResourceTestSetup(void);
// Called at the start of DeinitOTR.
void Zmp_Deinit(void);
// Called once per game frame right after the physical controllers are read into `pads`
// (an OSContPad array with one entry per port). This is where the harness replaces the
// port 0 pad and processes its command queue, so injected input follows the same path
// as a real controller.
void Zmp_OnPadRead(void* pads);
// Number of game frames (logic ticks) run since boot.
uint32_t Zmp_GetFrameCount(void);

#ifdef __cplusplus
}
// Audio thread: true while the instance's audio must be silent (window without focus and
// gZmp.Audio.MuteWhenUnfocused set). Presentation only.
// Test (finding T): several threads use the resource table at once (see Zmp.cpp).
struct ZmpResourceRace {
    int loads;   // alternative paths of the archives' files loaded
    int wrote;   // how many of those loads had to write into the table
    int made;    // made-up paths, in no archive, loaded for the first time
    int lost;    // made-up paths that are not in the table afterwards
    int phantom; // made-up paths the file table says it has afterwards
    int left;    // made-up paths still in the table after being unloaded
    int held;    // 1: a resource was destroyed with the table's lock held; -1: it was not destroyed
};
void Zmp_TestResourceRace(int threads, int made, ZmpResourceRace* result);
bool Zmp_AudioMuted();
// Audio thread: true while the speakers must be silent. Always the real window focus (a test focus override never
// unmutes them, D-063).
bool Zmp_AudioOutputMuted();
#endif
