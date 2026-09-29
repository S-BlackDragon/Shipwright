// ZMP: periodic autosave of the group leader and exact session state (phase 4). See Autosave.h and
// Lockstep.cpp (the trigger, the save on a copy of the save context and the resume).

#include "Autosave.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <thread>

#include <libultraship/bridge/consolevariablebridge.h>

#include "soh/Zmp/ZmpCVars.h"
#include "soh/Zmp/ZmpLog.h"
#include "soh/Zmp/State/StateBlob.h"

namespace Zmp::Autosave {

namespace {
std::mutex sMutex;
std::thread sWorker;
std::atomic<bool> sBusy{ false };
std::chrono::steady_clock::time_point sSavedAt;
bool sEverSaved = false;
std::atomic<uint32_t> sCount{ 0 };

std::string SafeName(const std::string& room) {
    std::string out;
    for (char c : room) {
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
        out += ok ? c : '_';
    }
    return out.empty() ? std::string("zmp") : out;
}
} // namespace

uint32_t IntervalTicks() {
    int ticks = CVarGetInteger(ZMP_CVAR_AUTOSAVE_TICKS, 0);
    if (ticks > 0) {
        return (uint32_t)ticks;
    }
    int minutes = CVarGetInteger(ZMP_CVAR_AUTOSAVE_MINUTES, 5);
    return minutes > 0 ? (uint32_t)minutes * 60u * 20u : 0u;
}

std::string SessionPath(const std::string& room) {
    return "Save/zmp_session_" + SafeName(room) + ".zmps";
}

bool WriteSessionAsync(const std::string& room, std::vector<uint8_t>&& blob, nlohmann::json&& info) {
    std::lock_guard<std::mutex> lock(sMutex);
    if (sBusy) {
        return false;
    }
    if (sWorker.joinable()) {
        sWorker.join();
    }
    sBusy = true;
    std::string path = SessionPath(room);
    sWorker = std::thread([path, blob = std::move(blob), info = std::move(info)]() {
        std::error_code ec;
        std::filesystem::create_directories("Save", ec);
        // Rotation: .zmps -> .1.zmps -> .2.zmps (the last three sessions).
        std::string base = path.substr(0, path.size() - 5);
        for (int i = 2; i >= 1; i--) {
            std::string from = i == 1 ? path : base + "." + std::to_string(i - 1) + ".zmps";
            std::string to = base + "." + std::to_string(i) + ".zmps";
            if (std::filesystem::exists(from, ec)) {
                std::filesystem::copy_file(from, to, std::filesystem::copy_options::overwrite_existing, ec);
                std::filesystem::copy_file(from.substr(0, from.size() - 5) + ".json",
                                           to.substr(0, to.size() - 5) + ".json",
                                           std::filesystem::copy_options::overwrite_existing, ec);
            }
        }
        std::string err;
        std::string tmp = path + ".tmp";
        bool ok = State::WriteFile(tmp, blob, &err);
        if (ok) {
            std::filesystem::rename(tmp, path, ec);
            ok = !ec;
        }
        std::string jpath = base + ".json";
        if (FILE* f = fopen((jpath + ".tmp").c_str(), "w")) {
            std::string text = info.dump(1);
            fwrite(text.data(), 1, text.size(), f);
            fclose(f);
            std::filesystem::rename(jpath + ".tmp", jpath, ec);
        }
        Log(std::string("autosave: session state ") + (ok ? "written to " + path : "not written: " + err) + " (" +
            std::to_string(blob.size()) + " bytes)");
        sBusy = false;
    });
    return true;
}

bool ReadSession(const std::string& room, std::vector<uint8_t>& blob, nlohmann::json& info, std::string* err) {
    std::string path = SessionPath(room);
    if (!State::ReadFile(path, blob, err)) {
        return false;
    }
    std::string jpath = path.substr(0, path.size() - 5) + ".json";
    FILE* f = fopen(jpath.c_str(), "r");
    if (f == nullptr) {
        *err = "no session description (" + jpath + ")";
        return false;
    }
    std::string text;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        text.append(buf, n);
    }
    fclose(f);
    try {
        info = nlohmann::json::parse(text);
    } catch (const std::exception& e) {
        *err = std::string("bad session description: ") + e.what();
        return false;
    }
    return true;
}

void Flush() {
    std::lock_guard<std::mutex> lock(sMutex);
    if (sWorker.joinable()) {
        sWorker.join();
    }
}

void NoteSaved() {
    sSavedAt = std::chrono::steady_clock::now();
    sEverSaved = true;
    sCount++;
}

double SecondsSinceSave() {
    if (!sEverSaved) {
        return 1e9;
    }
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - sSavedAt).count();
}

uint32_t SavedCount() {
    return sCount;
}

} // namespace Zmp::Autosave
