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
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#endif

#include <map>

#include "soh/Zmp/ZmpLog.h"

#ifdef _WIN32
// (libultraship, Fast3dWindow.h: called with the DXGI swap chain right before every present)
extern void (*gZmpBeforePresent)(void* dxgiSwapChain1);
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

HWND sFrameSwapHwnd = nullptr; // the window the renderer presents to (from its swap chain)
uint64_t sFramePresents = 0;   // frames presented through DXGI
struct FrameRequest {
    std::string path;
    bool done = false;
    bool ok = false;
    int w = 0, h = 0;
    std::string info;
};
std::map<int, FrameRequest> sFrameRequests;
int sFrameNextRequest = 1;

// This process's window: the one its swap chain presents to; without one (OpenGL, or before the first frame), the
// largest shown top-level window of this process.
HWND GameWindow() {
    if (sFrameSwapHwnd != nullptr && IsWindow(sFrameSwapHwnd)) {
        return sFrameSwapHwnd;
    }
    FindCtx ctx;
    EnumWindows(EnumProc, reinterpret_cast<LPARAM>(&ctx));
    return ctx.best;
}

BOOL CALLBACK DescribeProc(HWND hwnd, LPARAM lp) {
    auto* out = reinterpret_cast<std::string*>(lp);
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != GetCurrentProcessId()) {
        return TRUE;
    }
    char cls[64] = {};
    GetClassNameA(hwnd, cls, sizeof(cls));
    RECT rc{}, wr{};
    GetClientRect(hwnd, &rc);
    GetWindowRect(hwnd, &wr);
    char line[256];
    snprintf(line, sizeof(line), "%s%p %s%s%s%s%s%s client %ldx%ld at (%ld, %ld)", out->empty() ? "" : "; ",
             (void*)hwnd, cls, hwnd == sFrameSwapHwnd ? " (the renderer's)" : "",
             IsWindowVisible(hwnd) ? " shown" : " hidden", IsIconic(hwnd) ? " minimized" : "",
             GetWindow(hwnd, GW_OWNER) != nullptr ? " owned" : "", IsHungAppWindow(hwnd) ? " not responding" : "",
             rc.right - rc.left, rc.bottom - rc.top, wr.left, wr.top);
    *out += line;
    return TRUE;
}

// The frame, before it is presented (libultraship calls this on the game thread, the one that drew it).
void OnBeforePresent(void* swapChain) {
    auto* swap = static_cast<IDXGISwapChain1*>(swapChain);
    sFramePresents++;
    HWND hw = nullptr;
    if (SUCCEEDED(swap->GetHwnd(&hw)) && hw != nullptr) {
        sFrameSwapHwnd = hw;
    }
    bool any = false;
    for (auto& [id, r] : sFrameRequests) {
        any = any || !r.done;
    }
    if (!any) {
        return;
    }
    using Microsoft::WRL::ComPtr;
    std::string err;
    std::vector<uint8_t> rgb;
    int w = 0, h = 0;
    ComPtr<ID3D11Texture2D> back;
    HRESULT hr = swap->GetBuffer(0, IID_PPV_ARGS(&back));
    if (FAILED(hr)) {
        err = "the swap chain gave no Direct3D 11 buffer";
    } else {
        D3D11_TEXTURE2D_DESC d{};
        back->GetDesc(&d);
        ComPtr<ID3D11Device> dev;
        ComPtr<ID3D11DeviceContext> ctx;
        back->GetDevice(&dev);
        dev->GetImmediateContext(&ctx);
        bool bgr = d.Format == DXGI_FORMAT_B8G8R8A8_UNORM || d.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        bool rgba = d.Format == DXGI_FORMAT_R8G8B8A8_UNORM || d.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        if (!bgr && !rgba) {
            err = "unexpected format of the swap chain: " + std::to_string((int)d.Format);
        } else {
            D3D11_TEXTURE2D_DESC sd = d;
            sd.Usage = D3D11_USAGE_STAGING;
            sd.BindFlags = 0;
            sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            sd.MiscFlags = 0;
            sd.MipLevels = 1;
            sd.ArraySize = 1;
            ComPtr<ID3D11Texture2D> staging;
            hr = dev->CreateTexture2D(&sd, nullptr, &staging);
            if (FAILED(hr)) {
                err = "no staging texture for the copy of the frame";
            } else {
                ctx->CopyResource(staging.Get(), back.Get());
                D3D11_MAPPED_SUBRESOURCE m{};
                hr = ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m);
                if (FAILED(hr)) {
                    err = "the copy of the frame could not be read";
                } else {
                    w = (int)d.Width;
                    h = (int)d.Height;
                    rgb.resize((size_t)w * h * 3);
                    for (int y = 0; y < h; y++) {
                        const uint8_t* row = static_cast<const uint8_t*>(m.pData) + (size_t)y * m.RowPitch;
                        uint8_t* o = &rgb[(size_t)y * w * 3];
                        for (int x = 0; x < w; x++) {
                            o[x * 3 + 0] = row[x * 4 + (bgr ? 2 : 0)];
                            o[x * 3 + 1] = row[x * 4 + 1];
                            o[x * 3 + 2] = row[x * 4 + (bgr ? 0 : 2)];
                        }
                    }
                    ctx->Unmap(staging.Get(), 0);
                }
            }
        }
    }
    if (err.empty()) {
        // (what the desktop is doing with the window when the frame is taken: the evidence for finding AD)
        HWND win = GameWindow();
        if (win == nullptr || !IsWindowVisible(win) || IsIconic(win)) {
            Log("harness: screenshot taken from the frame; the desktop has this process's windows as: " +
                DescribeWindows());
        }
    }
    for (auto& [id, r] : sFrameRequests) {
        if (r.done) {
            continue;
        }
        r.done = true;
        if (!err.empty()) {
            r.info = err + " (HRESULT " + std::to_string((long)hr) + ")";
            continue;
        }
        r.ok = WritePngRgb(r.path, w, h, rgb.data());
        r.w = w;
        r.h = h;
        r.info = r.ok ? "frame" : "cannot write " + r.path;
    }
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
    HWND hwnd = GameWindow();
    if (hwnd == nullptr || width <= 0 || height <= 0) {
        return false;
    }
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
    HWND hwnd = GameWindow();
    if (hwnd != nullptr) {
        SetWindowPos(hwnd, nullptr, saved.x, saved.y, saved.w, saved.h,
                     SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER);
    }
}

std::string DescribeWindows() {
    std::string out;
    EnumWindows(DescribeProc, reinterpret_cast<LPARAM>(&out));
    return out.empty() ? "none" : out;
}

void InstallFrameCapture() {
    gZmpBeforePresent = OnBeforePresent;
}

bool FrameCaptureAvailable() {
    return sFramePresents > 0;
}

int RequestFrameCapture(const std::string& path) {
    int id = sFrameNextRequest++;
    sFrameRequests[id].path = path;
    return id;
}

bool FrameCaptureResult(int id, bool* ok, int* outW, int* outH, std::string* info) {
    auto it = sFrameRequests.find(id);
    if (it == sFrameRequests.end()) {
        *ok = false;
        *info = "no such capture";
        return true;
    }
    if (!it->second.done) {
        return false;
    }
    *ok = it->second.ok;
    *outW = it->second.w;
    *outH = it->second.h;
    *info = it->second.info;
    sFrameRequests.erase(it);
    return true;
}

bool CaptureGameWindow(const std::string& path, int* outW, int* outH, std::string* err) {
    HWND hwnd = GameWindow();
    if (hwnd == nullptr) {
        *err = "game window not found; this process's windows: " + DescribeWindows();
        return false;
    }
    RECT rc;
    GetClientRect(hwnd, &rc);
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) {
        *err = "window has no client area; this process's windows: " + DescribeWindows();
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

void InstallFrameCapture() {
}

bool FrameCaptureAvailable() {
    return false;
}

int RequestFrameCapture(const std::string&) {
    return 0;
}

bool FrameCaptureResult(int, bool* ok, int*, int*, std::string* info) {
    *ok = false;
    *info = "screenshot is only implemented on Windows";
    return true;
}

std::string DescribeWindows() {
    return "";
}

#endif

} // namespace Zmp::Harness

#endif
