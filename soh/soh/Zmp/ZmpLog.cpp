#include "ZmpLog.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <mutex>

namespace Zmp {

static std::mutex sLogMutex;
static FILE* sLogFile = nullptr;
static bool sLogOpenFailed = false;

void Log(const std::string& line) {
    std::lock_guard<std::mutex> lock(sLogMutex);
    if (sLogFile == nullptr && !sLogOpenFailed) {
        std::error_code ec;
        std::filesystem::create_directories("logs", ec);
        sLogFile = fopen("logs/zmp.log", "a");
        if (sLogFile == nullptr) {
            sLogOpenFailed = true;
            return;
        }
    }
    if (sLogFile == nullptr) {
        return;
    }
    // Wall clock is only used for the log line prefix (presentation, not simulation).
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    int ms = (int)(std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000);
    std::tm tmNow{};
#ifdef _WIN32
    localtime_s(&tmNow, &t);
#else
    localtime_r(&t, &tmNow);
#endif
    char prefix[32];
    std::strftime(prefix, sizeof(prefix), "%Y-%m-%d %H:%M:%S", &tmNow);
    fprintf(sLogFile, "[%s.%03d] %s\n", prefix, ms, line.c_str());
    fflush(sLogFile);
}

void LogClose() {
    std::lock_guard<std::mutex> lock(sLogMutex);
    if (sLogFile != nullptr) {
        fclose(sLogFile);
        sLogFile = nullptr;
    }
}

} // namespace Zmp
