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

// Called on the game thread once per game frame, right after the physical pads were read.
// Executes queued commands, resolves waits, and replaces the port 0 pad when a script or
// an input.set override is active. `pads` is an OSContPad array.
void Frame(void* pads, uint32_t frame);

} // namespace Zmp::Harness

#endif
