#pragma once

// ZMP: periodic autosave of the group leader and exact session state (phase 4; docs/DECISIONES.md D-054).

#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace Zmp::Autosave {

// Ticks between two autosaves from the settings (0 = off).
uint32_t IntervalTicks();
// Session state file of a room (Save/zmp_session_<room>.zmps) and its description (.json next to it).
std::string SessionPath(const std::string& room);
// Writes the session blob (rotating the last three copies) and its description on a worker thread. Returns false
// when the previous write is still running (this one is skipped).
bool WriteSessionAsync(const std::string& room, std::vector<uint8_t>&& blob, nlohmann::json&& info);
bool ReadSession(const std::string& room, std::vector<uint8_t>& blob, nlohmann::json& info, std::string* err);
// Waits for the worker (tests, shutdown).
void Flush();
// The "Guardado" indicator: seconds since the last autosave of this client (a large value if none).
void NoteSaved();
double SecondsSinceSave();
uint32_t SavedCount();

} // namespace Zmp::Autosave
