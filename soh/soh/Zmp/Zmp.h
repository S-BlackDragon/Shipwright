#pragma once

// ZMP: entry points called from the game (C) side.

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Called once at the end of InitOTR, after the GUI and console exist.
void Zmp_Init(void);
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
#endif
