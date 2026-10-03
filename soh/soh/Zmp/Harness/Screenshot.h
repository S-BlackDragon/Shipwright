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

// The frame itself (finding AD, D-102). A capture of the window asks the desktop for it: which windows this process
// has, whether they are shown, what the compositor has of them. The frame this process draws does not depend on any
// of that. With DirectX 11 the next frame presented is copied from the swap chain, after the game and the menus have
// been drawn on it, and written to `path`. Called on the game thread.
void InstallFrameCapture();   // (at start: the renderer tells this module about every frame it presents)
bool FrameCaptureAvailable(); // this process has presented a frame through DXGI
int RequestFrameCapture(const std::string& path);
// False while that frame has not been presented yet; then `ok`, the size and what happened (or why it failed).
bool FrameCaptureResult(int id, bool* ok, int* outW, int* outH, std::string* info);
// What the desktop says of this process's windows: shown, minimized, sizes (diagnosis of a window capture that failed).
std::string DescribeWindows();

} // namespace Zmp::Harness

#endif
