#pragma once

#ifdef ZMP_HARNESS

#include <cstdint>

// ZMP test harness (PLAN.md 5.2). A TCP server on 127.0.0.1:<port> that accepts JSON
// commands delimited by '\n'. Only compiled when the CMake option ZMP_HARNESS is ON; the
// distribution build does not contain it.
namespace Zmp::Harness {

// Starts the listener thread. Returns false if the port could not be bound.
bool Start(uint16_t port);
void Stop();
bool IsRunning();
uint16_t GetPort();

// Called on the game thread at the top of every RunFrame iteration (between two ticks, also while
// the simulation is paused): executes queued commands and resolves waits. `tick` is the number of
// logic ticks run since boot.
void OnFrameBegin(uint32_t tick);
// Called once per logic tick right after the physical pads were read: replaces the port 0 pad when
// a script or an input.set override is active. `pads` is an OSContPad array.
void ApplyInput(void* pads);
// A scripted input (input.script) in a lockstep group: one step of the script is one tick of the GROUP. The lockstep
// asks for the pad of the next input it sends and the script advances then, not when this machine simulates a tick:
// a machine that is behind for a moment (a hiccup of a PC running many test instances) still gives each step of the
// script to exactly as many ticks as the test wrote. False when no script is running (the pad read is used).
bool ScriptPadForSend(uint32_t* buttons, int8_t* stickX, int8_t* stickY, int8_t* rStickX, int8_t* rStickY);

} // namespace Zmp::Harness

#endif
