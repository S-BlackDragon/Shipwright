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

// Fast test suite: the windows are small tiles and a capture has its own size. The window's client area becomes
// width x height (kept inside its monitor, never activated, never raised) and `saved` receives what
// RestoreAfterCapture needs to put it back. False (nothing changed) if it already has that size or there is no window.
struct CaptureRestore {
    long x = 0, y = 0, w = 0, h = 0;
    bool valid = false;
};
bool ResizeForCapture(int width, int height, CaptureRestore* saved);
void RestoreAfterCapture(const CaptureRestore& saved);

} // namespace Zmp::Harness

#endif
