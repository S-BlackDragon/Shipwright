#ifdef ZMP_HARNESS

#include "Screenshot.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace Zmp::Harness {

namespace {

uint32_t Crc32(const uint8_t* data, size_t len, uint32_t crc = 0) {
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) {
                c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            }
            table[i] = c;
        }
        init = true;
    }
    crc = ~crc;
    for (size_t i = 0; i < len; i++) {
        crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    }
    return ~crc;
}

void PutU32BE(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back((uint8_t)(v >> 24));
    out.push_back((uint8_t)(v >> 16));
    out.push_back((uint8_t)(v >> 8));
    out.push_back((uint8_t)v);
}

void WriteChunk(FILE* f, const char* type, const std::vector<uint8_t>& data) {
    std::vector<uint8_t> buf;
    PutU32BE(buf, (uint32_t)data.size());
    buf.insert(buf.end(), type, type + 4);
    buf.insert(buf.end(), data.begin(), data.end());
    uint32_t crc = Crc32(buf.data() + 4, buf.size() - 4);
    PutU32BE(buf, crc);
    fwrite(buf.data(), 1, buf.size(), f);
}

} // namespace

bool WritePngRgb(const std::string& path, int width, int height, const uint8_t* rgb) {
    // Raw scanlines with filter byte 0.
    std::vector<uint8_t> raw;
    raw.reserve((size_t)height * (width * 3 + 1));
    for (int y = 0; y < height; y++) {
        raw.push_back(0);
        raw.insert(raw.end(), rgb + (size_t)y * width * 3, rgb + (size_t)(y + 1) * width * 3);
    }
    // zlib stream made of stored (uncompressed) deflate blocks: no external dependency.
    std::vector<uint8_t> z;
    z.push_back(0x78);
    z.push_back(0x01);
    size_t pos = 0;
    do {
        size_t n = std::min<size_t>(65535, raw.size() - pos);
        bool last = pos + n == raw.size();
        z.push_back(last ? 1 : 0);
        z.push_back((uint8_t)(n & 0xFF));
        z.push_back((uint8_t)(n >> 8));
        z.push_back((uint8_t)(~n & 0xFF));
        z.push_back((uint8_t)((~n >> 8) & 0xFF));
        z.insert(z.end(), raw.begin() + pos, raw.begin() + pos + n);
        pos += n;
    } while (pos < raw.size());
    uint32_t a = 1, b = 0;
    for (uint8_t c : raw) {
        a = (a + c) % 65521;
        b = (b + a) % 65521;
    }
    PutU32BE(z, (b << 16) | a);

    std::error_code ec;
    auto parent = std::filesystem::path(path).parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
    }
    FILE* f = fopen(path.c_str(), "wb");
    if (f == nullptr) {
        return false;
    }
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    fwrite(sig, 1, 8, f);
    std::vector<uint8_t> ihdr;
    PutU32BE(ihdr, (uint32_t)width);
    PutU32BE(ihdr, (uint32_t)height);
    ihdr.push_back(8); // bit depth
    ihdr.push_back(2); // color type RGB
    ihdr.push_back(0);
    ihdr.push_back(0);
    ihdr.push_back(0);
    WriteChunk(f, "IHDR", ihdr);
    WriteChunk(f, "IDAT", z);
    WriteChunk(f, "IEND", {});
    bool ok = ferror(f) == 0;
    fclose(f);
    return ok;
}

#ifdef _WIN32

namespace {

struct FindCtx {
    HWND best = nullptr;
    long bestArea = 0;
};

BOOL CALLBACK EnumProc(HWND hwnd, LPARAM lp) {
    auto* ctx = reinterpret_cast<FindCtx*>(lp);
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != GetCurrentProcessId() || !IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER) != nullptr) {
        return TRUE;
    }
    RECT rc;
    if (!GetClientRect(hwnd, &rc)) {
        return TRUE;
    }
    long area = (rc.right - rc.left) * (rc.bottom - rc.top);
    if (area > ctx->bestArea) {
        ctx->bestArea = area;
        ctx->best = hwnd;
    }
    return TRUE;
}

bool AllBlack(const std::vector<uint8_t>& bgra) {
    for (size_t i = 0; i + 3 < bgra.size(); i += 4) {
        if (bgra[i] > 8 || bgra[i + 1] > 8 || bgra[i + 2] > 8) {
            return false;
        }
    }
    return true;
}

} // namespace

bool ResizeForCapture(int width, int height, CaptureRestore* saved) {
    FindCtx ctx;
    EnumWindows(EnumProc, reinterpret_cast<LPARAM>(&ctx));
    if (ctx.best == nullptr || width <= 0 || height <= 0) {
        return false;
    }
    HWND hwnd = ctx.best;
    RECT client, outer;
    if (!GetClientRect(hwnd, &client) || !GetWindowRect(hwnd, &outer)) {
        return false;
    }
    if (client.right - client.left == width && client.bottom - client.top == height) {
        return false;
    }
    long ow = (outer.right - outer.left) + (width - (client.right - client.left));
    long oh = (outer.bottom - outer.top) + (height - (client.bottom - client.top));
    long x = outer.left, y = outer.top;
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfo(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi)) {
        // (the bigger window stays inside the monitor its tile is in)
        x = std::max<long>(mi.rcWork.left, std::min<long>(x, mi.rcWork.right - ow));
        y = std::max<long>(mi.rcWork.top, std::min<long>(y, mi.rcWork.bottom - oh));
    }
    saved->x = outer.left;
    saved->y = outer.top;
    saved->w = outer.right - outer.left;
    saved->h = outer.bottom - outer.top;
    saved->valid = true;
    SetWindowPos(hwnd, nullptr, x, y, ow, oh, SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER);
    return true;
}

void RestoreAfterCapture(const CaptureRestore& saved) {
    if (!saved.valid) {
        return;
    }
    FindCtx ctx;
    EnumWindows(EnumProc, reinterpret_cast<LPARAM>(&ctx));
    if (ctx.best != nullptr) {
        SetWindowPos(ctx.best, nullptr, saved.x, saved.y, saved.w, saved.h,
                     SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER);
    }
}

bool CaptureGameWindow(const std::string& path, int* outW, int* outH, std::string* err) {
    FindCtx ctx;
    EnumWindows(EnumProc, reinterpret_cast<LPARAM>(&ctx));
    if (ctx.best == nullptr) {
        *err = "game window not found";
        return false;
    }
    HWND hwnd = ctx.best;
    RECT rc;
    GetClientRect(hwnd, &rc);
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) {
        *err = "window has no client area (minimized?)";
        return false;
    }

    HDC winDc = GetDC(hwnd);
    HDC memDc = CreateCompatibleDC(winDc);
    HBITMAP bmp = CreateCompatibleBitmap(winDc, w, h);
    HGDIOBJ old = SelectObject(memDc, bmp);

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h; // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    std::vector<uint8_t> bgra((size_t)w * h * 4);

    // PW_CLIENTONLY | PW_RENDERFULLCONTENT: asks DWM for the composed content, which works
    // for DXGI flip-model swap chains and does not need the window to be in front.
    BOOL printed = PrintWindow(hwnd, memDc, 0x1 | 0x2);
    bool got = printed && GetDIBits(memDc, bmp, 0, h, bgra.data(), &bi, DIB_RGB_COLORS) == h;
    std::string method = "printwindow";
    if (!got || AllBlack(bgra)) {
        // Fallback: copy the visible screen area of the client rect.
        POINT origin = { 0, 0 };
        ClientToScreen(hwnd, &origin);
        HDC screenDc = GetDC(nullptr);
        BitBlt(memDc, 0, 0, w, h, screenDc, origin.x, origin.y, SRCCOPY);
        ReleaseDC(nullptr, screenDc);
        got = GetDIBits(memDc, bmp, 0, h, bgra.data(), &bi, DIB_RGB_COLORS) == h;
        method = "screen";
    }

    SelectObject(memDc, old);
    DeleteObject(bmp);
    DeleteDC(memDc);
    ReleaseDC(hwnd, winDc);

    if (!got) {
        *err = "GetDIBits failed";
        return false;
    }
    std::vector<uint8_t> rgb((size_t)w * h * 3);
    for (size_t i = 0, j = 0; i < bgra.size(); i += 4, j += 3) {
        rgb[j] = bgra[i + 2];
        rgb[j + 1] = bgra[i + 1];
        rgb[j + 2] = bgra[i];
    }
    if (!WritePngRgb(path, w, h, rgb.data())) {
        *err = "cannot write " + path;
        return false;
    }
    *outW = w;
    *outH = h;
    *err = method;
    return true;
}

#else

bool CaptureGameWindow(const std::string& path, int* outW, int* outH, std::string* err) {
    *err = "screenshot is only implemented on Windows";
    return false;
}

bool ResizeForCapture(int, int, CaptureRestore*) {
    return false;
}

void RestoreAfterCapture(const CaptureRestore&) {
}

#endif

} // namespace Zmp::Harness

#endif
