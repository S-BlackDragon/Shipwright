// Mutation test of the test suite (see Mutants.h). Test builds only.

#ifdef ZMP_HARNESS

#include "Mutants.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>

#include <libultraship/bridge/consolevariablebridge.h>

#include "soh/Zmp/ZmpCVars.h"
#include "soh/Zmp/ZmpLog.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

std::string sMutant;
bool sRead = false;

const std::string& Current() {
    if (!sRead) {
        // The environment first: one mutant acts before the CVars are loaded.
        const char* env = std::getenv("ZMP_MUTANT");
        if (env != nullptr && env[0] != '\0') {
            sMutant = env;
            sRead = true;
        }
    }
    return sMutant;
}

} // namespace

extern "C" int Zmp_TestMutant(const char* name) {
    static int sMutantAge = 0;
    static bool sLogged = false;
    if (Current().empty() && !sRead && ++sMutantAge >= 60) {
        // (no environment variable: the CVar, looked up now and then)
        sMutantAge = 0;
        const char* cv = CVarGetString(ZMP_CVAR_TEST_MUTANT, "");
        if (cv != nullptr && cv[0] != '\0') {
            sMutant = cv;
            sRead = true;
        }
    }
    bool on = !sMutant.empty() && sMutant == name;
    if (on && !sLogged) {
        sLogged = true;
        Zmp::Log("zmp: MUTANT " + sMutant + " active (a past bug brought back on purpose: mutation test)");
    }
    return on ? 1 : 0;
}

// robo_foco. The bug: a test window became the PC's active window when it opened. The mutant lets that happen (it
// does not install the hook that refuses the activation) and, so that the person using the PC loses the focus for
// an instant and not until they click, hands it back to the window that had it.
#ifdef _WIN32
namespace {
HWND sFocusBefore = nullptr;
bool sFocusReturned = false;
std::chrono::steady_clock::time_point sFocusTakenAt;
bool sFocusTaken = false;
} // namespace
#endif

extern "C" void Zmp_TestFocusMutantStart(void) {
#ifdef _WIN32
    sFocusBefore = GetForegroundWindow();
#endif
}

extern "C" void Zmp_TestFocusMutantFrame(void) {
#ifdef _WIN32
    if (sFocusReturned || !Zmp_TestMutant("robo_foco")) {
        return;
    }
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    if (fg != nullptr) {
        GetWindowThreadProcessId(fg, &pid);
    }
    if (pid != GetCurrentProcessId()) {
        return;
    }
    auto now = std::chrono::steady_clock::now();
    if (!sFocusTaken) {
        sFocusTaken = true;
        sFocusTakenAt = now;
        return;
    }
    if (now - sFocusTakenAt > std::chrono::milliseconds(300)) {
        sFocusReturned = true;
        if (sFocusBefore != nullptr && IsWindow(sFocusBefore)) {
            SetForegroundWindow(sFocusBefore); // (the only call of this kind in the project: giving the focus back)
        }
    }
#endif
}

#endif
