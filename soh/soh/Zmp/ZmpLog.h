#pragma once

#include <string>

namespace Zmp {
// Appends one line to logs/zmp.log (relative to the instance directory) with a timestamp.
// Thread safe. Never used by simulation code for decisions.
void Log(const std::string& line);
void LogClose();
} // namespace Zmp
