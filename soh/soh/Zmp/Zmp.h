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
// Finding R, test build only: 1 when the test asks (ZMP_TEST_DIRTY_WINDOW_FIELD=1) for that field to start with
// the value the memory happened to hold in the starts that died (heap contents cannot be forced from outside: filling
// freed blocks reached the object in only half of the starts).
int Zmp_TestDirtyWindowField(void);
// Finding T: called once while the game is still the only thread, after the archives are known (see Zmp.cpp).
void Zmp_WarmResourceCache(void);
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
// Test (finding T): several threads load alternative paths at once; returns how many loads wrote into the table.
int Zmp_TestResourceRace(int threads, int* loads);
bool Zmp_AudioMuted();
// Audio thread: true while the speakers must be silent. Always the real window focus (a test focus override never
// unmutes them, D-063).
bool Zmp_AudioOutputMuted();
#endif
