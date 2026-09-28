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

} // namespace Zmp::Harness

#endif
