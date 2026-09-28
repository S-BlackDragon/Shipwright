#pragma once

// Locked CVar profile (PLAN.md 2.4 point 5, 8.4; list in docs/CVAR_PROFILE.md). While a ZMP
// session is active, CVars that change the simulation keep the value they had when the session
// started (plus a few forced values); any change is reverted and logged.

#include <cstdint>
#include <string>

namespace Zmp::CVarProfile {

bool IsLocked(const std::string& name);
// Applies the forced values, snapshots every locked CVar and computes the profile hash.
void BeginSession();
// Restores the values the forced CVars had before the session.
void EndSession();
bool Active();
// Called every frame; checks (every few frames) that no locked CVar changed.
void Enforce();
// Hash of the locked CVars (name=value, sorted). Computed at BeginSession; outside a session it is
// computed on demand.
uint64_t Hash();
uint32_t RevertCount();

} // namespace Zmp::CVarProfile
