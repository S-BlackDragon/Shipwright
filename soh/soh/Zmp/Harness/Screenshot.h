#pragma once

#ifdef ZMP_HARNESS

#include <cstdint>
#include <string>

namespace Zmp::Harness {

// Writes an 8-bit RGB PNG (stored deflate, no compression library needed).
bool WritePngRgb(const std::string& path, int width, int height, const uint8_t* rgb);

// Captures the client area of this process's main window to a PNG. On success `err`
// holds the capture method used.
bool CaptureGameWindow(const std::string& path, int* outW, int* outH, std::string* err);

} // namespace Zmp::Harness

#endif
