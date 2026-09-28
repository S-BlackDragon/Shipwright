#pragma once

// ZMP simulation hooks called from the game (C) side. A logic tick is one iteration of the
// RunFrame loop in graph.c: pad read + GameState_Update (docs/TICK.md).

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Top of every RunFrame iteration, on the game thread, before the pads are read: harness
// commands, session control (record, replay, pause) and save state requests run here, between
// two ticks.
void Zmp_OnFrameBegin(void);
// Whether this RunFrame iteration runs one logic tick. In phase 1 it is true unless the harness
// paused the simulation; in phase 2 the lockstep opens it when the bundle of the tick arrived.
int32_t Zmp_ShouldRunTick(void);
// Right after Graph_Update: the tick's GameState_Update is done. Computes the state hash, logs
// it, checks it against the replay reference and advances the tick counter.
void Zmp_OnTickEnd(void);
// Seed for Rand_Seed in Play_Init. Outside a ZMP session it returns timeSeed (vanilla behaviour);
// in a session the RNG is never reseeded from the clock.
uint32_t Zmp_PlaySeed(uint32_t timeSeed);
// Actor_UpdateAll: true when actors must update regardless of the camera culling volume.
int32_t Zmp_IgnoreUpdateCulling(void);
// Graph_ProcessGfxCommands: replay speed-up factor (presentation only). 1 = normal speed; N > 1 =
// N ticks per 50 ms with one drawn frame per tick; 0 = as fast as the machine can.
int32_t Zmp_PresentationSpeed(void);

// Around Graph_ProcessGfxCommands: keep the game's matrix stack as the tick left it (frame
// interpolation reuses it while drawing intermediate frames).
void Zmp_BeforeRender(void);
void Zmp_AfterRender(void);

// Hash of the simulation state at the end of the current tick (PLAN.md 8.2). Never covers
// pointers, render or audio state.
uint64_t Zmp_StateHash(void);
// Writes a readable dump of the simulation state (actors, shared save context, RNG) to path.
// Returns 0 on success.
int32_t Zmp_DumpState(const char* path);

#ifdef __cplusplus
}
#endif
