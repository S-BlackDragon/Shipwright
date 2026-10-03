#include "Zmp.h"
#include "soh/Zmp/Test/Mutants.h"
#include "Sim/ZmpSim.h"
#include "Sim/ZmpPlayers.h"

#include <atomic>
#include <cstring>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <variant>
#include <vector>
#include <chrono>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include <ship/Context.h>
#include <ship/debug/CrashHandler.h>
#include <ship/window/Window.h>
#include <ship/resource/ResourceManager.h>
#include <ship/resource/archive/ArchiveManager.h>
#include <libultraship/bridge/consolevariablebridge.h>

#include "ZmpCVars.h"
#include "ZmpLog.h"
#include "Harness/Harness.h"
#include "Net/ZmpClient.h"
#include "Net/Lockstep.h"
#include "Sim/Session.h"
#include "State/FixedHeap.h"
#include "State/ResourceSlots.h"
#include "State/StateBlob.h"
#include "Ui/ZmpWindow.h"

extern "C" {
#include <z64.h>
#include "functions.h"
#include "variables.h"
#include "macros.h"
extern PlayState* gPlayState;
}

extern "C" void gfx_texture_cache_clear();

static uint32_t sFrameCount = 0;

#ifdef _WIN32
#include <dbghelp.h>
#include <thread>
// Hang watchdog (diagnostics for testers and friends): if the game thread does not start a new frame for
// 5 s, its call stack goes to logs/zmp.log.
static std::atomic<uint32_t> sFrameBegins{ 0 };
static HANDLE sGameThread = nullptr;
static std::thread sWatchdog;
static std::atomic<bool> sWatchdogStop{ false };

static int BootLogFrame(char* out, size_t size, DWORD64 address);
static void BootLogWrite(const char* text);

// The return addresses of a thread that is stopped. While the thread is suspended NOTHING here may take a lock that
// thread could be holding: no memory allocation, no C runtime file call, no symbol library (finding X: the watchdog
// used the system's symbol library with the game thread suspended; when the game thread was inside that same
// library, which serializes its callers, the watchdog waited for a thread it had itself stopped and the game never
// moved again). Only the unwind tables are read, into an array on this thread's stack.
static int CaptureSuspendedStack(HANDLE thread, DWORD64* frames, int max) {
    if (SuspendThread(thread) == (DWORD)-1) {
        return -1;
    }
    int n = 0;
    __try {
        CONTEXT ctx;
        ctx.ContextFlags = CONTEXT_FULL;
        if (GetThreadContext(thread, &ctx)) {
            while (n < max && ctx.Rip != 0) {
                frames[n++] = ctx.Rip;
                DWORD64 imageBase = 0;
                PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &imageBase, nullptr);
                if (fn == nullptr) {
                    ctx.Rip = *(DWORD64*)ctx.Rsp;
                    ctx.Rsp += 8;
                } else {
                    void* handlerData = nullptr;
                    DWORD64 establisher = 0;
                    RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, ctx.Rip, fn, &ctx, &handlerData, &establisher,
                                     nullptr);
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    ResumeThread(thread);
    return n;
}

// (the watchdog before finding X; kept for the mutation test only)
static void LogGameThreadStackWithLibrary() {
    if (sGameThread == nullptr) {
        return;
    }
    static bool sSymInit = false;
    HANDLE proc = GetCurrentProcess();
    if (!sSymInit) {
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
        char exe[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, exe, MAX_PATH);
        std::string dir = exe;
        dir = dir.substr(0, dir.find_last_of("\\/"));
        std::string search = dir + ";" + dir + "\\debug";
        SymInitialize(proc, search.c_str(), TRUE);
        sSymInit = true;
    }
    if (SuspendThread(sGameThread) == (DWORD)-1) {
        return;
    }
    CONTEXT ctx = {};
    ctx.ContextFlags = CONTEXT_FULL;
    std::string out = "zmp: WATCHDOG game thread stuck, stack:";
    if (GetThreadContext(sGameThread, &ctx)) {
        STACKFRAME64 frame = {};
        frame.AddrPC.Offset = ctx.Rip;
        frame.AddrPC.Mode = AddrModeFlat;
        frame.AddrFrame.Offset = ctx.Rbp;
        frame.AddrFrame.Mode = AddrModeFlat;
        frame.AddrStack.Offset = ctx.Rsp;
        frame.AddrStack.Mode = AddrModeFlat;
        for (int i = 0; i < 40; i++) {
            if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, sGameThread, &frame, &ctx, nullptr,
                             SymFunctionTableAccess64, SymGetModuleBase64, nullptr) ||
                frame.AddrPC.Offset == 0) {
                break;
            }
            char buf[sizeof(SYMBOL_INFO) + 256];
            SYMBOL_INFO* sym = (SYMBOL_INFO*)buf;
            sym->SizeOfStruct = sizeof(SYMBOL_INFO);
            sym->MaxNameLen = 255;
            DWORD64 disp = 0;
            char off[32];
            snprintf(off, sizeof(off), "exe+0x%llX",
                     (unsigned long long)(frame.AddrPC.Offset - (DWORD64)GetModuleHandleA(nullptr)));
            std::string name = SymFromAddr(proc, frame.AddrPC.Offset, &disp, sym) ? sym->Name : off;
            IMAGEHLP_LINE64 line = {};
            line.SizeOfStruct = sizeof(line);
            DWORD ldisp = 0;
            std::string where;
            if (SymGetLineFromAddr64(proc, frame.AddrPC.Offset, &ldisp, &line)) {
                where = std::string(" ") + line.FileName + ":" + std::to_string(line.LineNumber);
            }
            out += "\n    " + name + where;
        }
    }
    ResumeThread(sGameThread);
    Zmp::Log(out);
}

// The stack is written as module+offset, first to the boot log (plain system calls) and then to logs/zmp.log; the
// names come afterwards from the .pdb: python tools/harness/suite/simbolos.py soh.exe <folder of soh.pdb> <offsets>.
static void LogGameThreadStack() {
    if (sGameThread == nullptr) {
        return;
    }
    if (Zmp_TestMutant("vigilante_con_simbolos")) { // (mutation test: the symbol library, with the thread stopped)
        LogGameThreadStackWithLibrary();
        return;
    }
    static DWORD64 frames[48];
    static char text[48 * (MAX_PATH + 32) + 128];
    int count = CaptureSuspendedStack(sGameThread, frames, 48);
    if (count < 0) {
        return;
    }
    // (the game thread runs again from here on)
    int n = snprintf(text, sizeof(text), "zmp: WATCHDOG game thread stuck, stack (module+offset):\n");
    for (int i = 0; i < count && n < (int)sizeof(text) - (MAX_PATH + 32); i++) {
        n += BootLogFrame(text + n, sizeof(text) - n, frames[i]);
    }
    BootLogWrite(text);
    Zmp::Log(text);
}

static void WatchdogLoop() {
    uint32_t last = 0;
    int stuck = 0;
    bool reported = false;
    while (!sWatchdogStop) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        uint32_t now = sFrameBegins.load();
        if (now == last && now != 0) {
            if (++stuck == 10 && !reported) { // 5 s
                reported = true;
                LogGameThreadStack();
            }
        } else {
            stuck = 0;
            reported = false;
        }
        last = now;
    }
}
#endif
static std::shared_ptr<Zmp::RoomWindow> sRoomWindow;
// sAudioMuted: what the mute logic decided (with the test focus override; reported to the harness).
// sAudioOutputMuted: what the speakers get. It always follows the REAL window focus, so a test that tells an instance
// "you have the focus" never makes it sound while Alex has not clicked on it (D-063).
static std::atomic<bool> sAudioMuted{ false };
static std::atomic<bool> sAudioOutputMuted{ false };

// Console commands run inside the ImGui pass (in the middle of rendering); state changes are queued
// and executed at the start of the next frame, between two ticks.
static std::mutex sConsoleMutex;
static std::deque<std::function<void()>> sConsoleQueue;

extern "C" uint32_t Zmp_GetFrameCount(void) {
    return sFrameCount;
}

bool Zmp_AudioMuted() {
    return sAudioMuted.load(std::memory_order_relaxed);
}

bool Zmp_AudioOutputMuted() {
    return sAudioOutputMuted.load(std::memory_order_relaxed);
}

static void QueueConsoleAction(std::function<void()> fn) {
    std::lock_guard<std::mutex> lock(sConsoleMutex);
    sConsoleQueue.push_back(std::move(fn));
}

static void RunConsoleActions() {
    std::deque<std::function<void()>> actions;
    {
        std::lock_guard<std::mutex> lock(sConsoleMutex);
        actions.swap(sConsoleQueue);
    }
    for (auto& fn : actions) {
        fn();
    }
}

static int32_t StateCommand(const std::vector<std::string>& args, std::string* output, bool save) {
    if (args.size() < 2) {
        if (output) {
            *output = std::string("usage: ") + args[0] + " <path>";
        }
        return 1;
    }
    std::string path = args[1];
    QueueConsoleAction([path, save]() {
        std::string err, info;
        bool ok = save ? Zmp::Sim::SaveStateFile(path, &err, &info) : Zmp::Sim::LoadStateFile(path, &err, &info);
        Zmp::Log(std::string(save ? "zmp_state_save " : "zmp_state_load ") + path + ": " + (ok ? info : err));
    });
    if (output) {
        *output = std::string(save ? "saving" : "loading") + " state " + path + " at the next frame";
    }
    return 0;
}

static void RegisterConsoleCommands() {
    auto console = Ship::Context::GetRawInstance()->GetConsole();
    console->AddCommand("zmp_state_save", { [](std::shared_ptr<Ship::Console>, std::vector<std::string> args,
                                               std::string* output) { return StateCommand(args, output, true); },
                                            "ZMP: save a portable save state to a file",
                                            { { "path", Ship::ArgumentType::TEXT } } });
    console->AddCommand("zmp_state_load", { [](std::shared_ptr<Ship::Console>, std::vector<std::string> args,
                                               std::string* output) { return StateCommand(args, output, false); },
                                            "ZMP: load a portable save state from a file",
                                            { { "path", Ship::ArgumentType::TEXT } } });
    console->AddCommand("zmp_state_dump",
                        { [](std::shared_ptr<Ship::Console>, std::vector<std::string> args, std::string* output) {
                             if (args.size() < 2) {
                                 return 1;
                             }
                             std::string path = args[1];
                             QueueConsoleAction([path]() {
                                 std::string err;
                                 uint32_t t = Zmp::Sim::CurrentTick();
                                 Zmp::Sim::DumpState(path, t > 0 ? t - 1 : 0, &err);
                             });
                             return 0;
                         },
                          "ZMP: write a readable dump of the simulation state",
                          { { "path", Ship::ArgumentType::TEXT } } });
    // Scripted segment boundary for recordings (docs/DECISIONES.md): puts the player at an exact position
    // (the vanilla `pos` command keeps the previous position, so collision stops the move halfway).
    console->AddCommand("zmp_teleport",
                        { [](std::shared_ptr<Ship::Console>, std::vector<std::string> args, std::string* output) {
                             if (args.size() < 4 || gPlayState == nullptr) {
                                 return 1;
                             }
                             Player* player = GET_PLAYER(gPlayState);
                             if (args.size() > 5 && Zmp_MultiActive()) { // optional slot (lockstep events)
                                 player = Zmp::Players::SlotPlayer(std::stoi(args[5]));
                                 if (player == nullptr) {
                                     return 1;
                                 }
                             }
                             Vec3f pos = { std::stof(args[1]), std::stof(args[2]), std::stof(args[3]) };
                             player->actor.world.pos = pos;
                             player->actor.prevPos = pos;
                             player->actor.home.pos = pos;
                             if (args.size() > 4) {
                                 s16 yaw = (s16)std::stoi(args[4]);
                                 player->actor.shape.rot.y = player->actor.world.rot.y = player->yaw = yaw;
                             }
                             return 0;
                         },
                          "ZMP: teleport the player to x y z [yaw] (used by scripted recordings)",
                          { { "x", Ship::ArgumentType::TEXT },
                            { "y", Ship::ArgumentType::TEXT },
                            { "z", Ship::ArgumentType::TEXT },
                            { "yaw", Ship::ArgumentType::TEXT, true },
                            { "slot", Ship::ArgumentType::TEXT, true } } });
    // Tests: the debug game has no save slot (0xFF, never written); this gives it one (a lockstep event, so the
    // save context stays identical on every machine).
    console->AddCommand("zmp_file_num",
                        { [](std::shared_ptr<Ship::Console>, std::vector<std::string> args, std::string* output) {
                             if (args.size() < 2) {
                                 return 1;
                             }
                             gSaveContext.fileNum = std::stoi(args[1]);
                             return 0;
                         },
                          "ZMP: set the save slot of the current game (tests)",
                          { { "slot", Ship::ArgumentType::TEXT } } });
    // Presentation only: drops every texture the renderer has cached (they are uploaded again when drawn).
    console->AddCommand("zmp_texcache_clear",
                        { [](std::shared_ptr<Ship::Console>, std::vector<std::string> args, std::string* output) {
                             gfx_texture_cache_clear();
                             return 0;
                         },
                          "ZMP: clear the renderer's texture cache",
                          {} });
    // Phase 4 tests (lockstep events, they run in the sender's context): a one-point cutscene of the sender, a
    // scripted cutscene at an entrance, an actor's health.
    console->AddCommand("zmp_onepoint",
                        { [](std::shared_ptr<Ship::Console>, std::vector<std::string> args, std::string* output) {
                             if (args.size() < 3 || gPlayState == nullptr) {
                                 return 1;
                             }
                             Player* player = GET_PLAYER(gPlayState);
                             OnePointCutscene_Init(gPlayState, (s16)std::stoi(args[1]), (s16)std::stoi(args[2]),
                                                   &player->actor, CAM_ID_MAIN);
                             return 0;
                         },
                          "ZMP: start a one-point cutscene of the player (tests)",
                          { { "cs_id", Ship::ArgumentType::TEXT }, { "timer", Ship::ArgumentType::TEXT } } });
    // Fast suite tests (a lockstep event): a quake of the game's own (Quake_Add), with a zoom, for some ticks.
    console->AddCommand("zmp_quake",
                        { [](std::shared_ptr<Ship::Console>, std::vector<std::string> args, std::string* output) {
                             if (args.size() < 3 || gPlayState == nullptr) {
                                 return 1;
                             }
                             s16 idx = Quake_Add(GET_ACTIVE_CAM(gPlayState), 3);
                             Quake_SetSpeed(idx, 20000);
                             Quake_SetQuakeValues(idx, 2, 0, (s16)std::stoi(args[1]), 0);
                             Quake_SetCountdown(idx, (s16)std::stoi(args[2]));
                             return 0;
                         },
                          "ZMP: start a quake with a zoom (tests)",
                          { { "zoom", Ship::ArgumentType::TEXT }, { "ticks", Ship::ArgumentType::TEXT } } });
    console->AddCommand("zmp_cutscene",
                        { [](std::shared_ptr<Ship::Console>, std::vector<std::string> args, std::string* output) {
                             if (args.size() < 3 || gPlayState == nullptr) {
                                 return 1;
                             }
                             gSaveContext.nextCutsceneIndex = (u16)std::stoi(args[2], nullptr, 16);
                             gPlayState->nextEntranceIndex = (s16)std::stoi(args[1], nullptr, 16);
                             gPlayState->transitionTrigger = TRANS_TRIGGER_START;
                             gPlayState->transitionType = TRANS_TYPE_FADE_BLACK;
                             return 0;
                         },
                          "ZMP: go to an entrance with a scripted cutscene (hex entrance, hex cutscene index)",
                          { { "entrance", Ship::ArgumentType::TEXT }, { "cutscene", Ship::ArgumentType::TEXT } } });
    // Phase 5 tests (lockstep events): a permanent flag of the current scene or an event flag set the way the game
    // sets it; the time of day set by this group (a jump the world clock adopts).
    console->AddCommand("zmp_flag",
                        { [](std::shared_ptr<Ship::Console>, std::vector<std::string> args, std::string* output) {
                             if (args.size() < 3 || gPlayState == nullptr) {
                                 return 1;
                             }
                             int flag = std::stoi(args[2], nullptr, 0);
                             if (args[1] == "swch") {
                                 Flags_SetSwitch(gPlayState, flag);
                             } else if (args[1] == "chest") {
                                 Flags_SetTreasure(gPlayState, flag);
                             } else if (args[1] == "clear") {
                                 Flags_SetClear(gPlayState, flag);
                             } else if (args[1] == "collect") {
                                 Flags_SetCollectible(gPlayState, flag);
                             } else if (args[1] == "event") {
                                 Flags_SetEventChkInf(flag);
                             } else {
                                 return 1;
                             }
                             return 0;
                         },
                          "ZMP: set a flag (swch|chest|clear|collect of the current scene, or event) (tests)",
                          { { "type", Ship::ArgumentType::TEXT }, { "flag", Ship::ArgumentType::TEXT } } });
    console->AddCommand("zmp_time",
                        { [](std::shared_ptr<Ship::Console>, std::vector<std::string> args, std::string* output) {
                             if (args.size() < 2) {
                                 return 1;
                             }
                             gSaveContext.dayTime = (u16)std::stoi(args[1], nullptr, 0);
                             return 0;
                         },
                          "ZMP: set the time of day (tests)",
                          { { "time", Ship::ArgumentType::TEXT } } });
    console->AddCommand("zmp_actor_hp",
                        { [](std::shared_ptr<Ship::Console>, std::vector<std::string> args, std::string* output) {
                             if (args.size() < 3 || gPlayState == nullptr) {
                                 return 1;
                             }
                             int id = std::stoi(args[1], nullptr, 0);
                             int hp = std::stoi(args[2]);
                             for (int cat = 0; cat < ACTORCAT_MAX; cat++) {
                                 for (Actor* a = gPlayState->actorCtx.actorLists[cat].head; a != nullptr; a = a->next) {
                                     if (a->id == id) {
                                         a->colChkInfo.health = (u8)hp;
                                         return 0;
                                     }
                                 }
                             }
                             return 1;
                         },
                          "ZMP: set the health of the first actor with that id (tests)",
                          { { "actor_id", Ship::ArgumentType::TEXT }, { "health", Ship::ArgumentType::TEXT } } });
    // Tests (a lockstep event): put the first actor with that id at x y z (finding Z: Gohma's body away from the
    // centre of her arena, so that where her blue warp appears is known).
    console->AddCommand("zmp_actor_pos",
                        { [](std::shared_ptr<Ship::Console>, std::vector<std::string> args, std::string* output) {
                             if (args.size() < 5 || gPlayState == nullptr) {
                                 return 1;
                             }
                             int id = std::stoi(args[1], nullptr, 0);
                             Vec3f pos = { std::stof(args[2]), std::stof(args[3]), std::stof(args[4]) };
                             for (int cat = 0; cat < ACTORCAT_MAX; cat++) {
                                 for (Actor* a = gPlayState->actorCtx.actorLists[cat].head; a != nullptr; a = a->next) {
                                     if (a->id == id) {
                                         a->world.pos = pos;
                                         a->prevPos = pos;
                                         return 0;
                                     }
                                 }
                             }
                             return 1;
                         },
                          "ZMP: put the first actor with that id at x y z (tests)",
                          { { "actor_id", Ship::ArgumentType::TEXT },
                            { "x", Ship::ArgumentType::TEXT },
                            { "y", Ship::ArgumentType::TEXT },
                            { "z", Ship::ArgumentType::TEXT } } });
    console->AddCommand("zmp_replay",
                        { [](std::shared_ptr<Ship::Console>, std::vector<std::string> args, std::string* output) {
                             if (args.size() < 2) {
                                 return 1;
                             }
                             std::string path = args[1];
                             std::string from = args.size() > 2 ? args[2] : "";
                             QueueConsoleAction([path, from]() {
                                 std::string err;
                                 if (!Zmp::Sim::StartReplay(path, from, &err)) {
                                     Zmp::Log("zmp_replay: " + err);
                                 }
                             });
                             return 0;
                         },
                          "ZMP: replay a .zmpinput recording (optional: save state to start from)",
                          { { "path", Ship::ArgumentType::TEXT }, { "state", Ship::ArgumentType::TEXT, true } } });
    console->AddCommand("zmp_record",
                        { [](std::shared_ptr<Ship::Console>, std::vector<std::string> args, std::string* output) {
                             if (args.size() < 2) {
                                 return 1;
                             }
                             std::string arg = args[1];
                             QueueConsoleAction([arg]() {
                                 std::string err;
                                 if (arg == "stop") {
                                     Zmp::Sim::StopRecording(&err);
                                 } else if (!Zmp::Sim::StartRecording(arg, Zmp::Sim::StartSpec{}, true, &err)) {
                                     Zmp::Log("zmp_record: " + err);
                                 }
                             });
                             return 0;
                         },
                          "ZMP: record input from the current state to a file ('stop' ends it)",
                          { { "path|stop", Ship::ArgumentType::TEXT } } });
}

// Test instances never take the focus from the person using the PC (D-063). Windows lets a freshly started process
// activate its first window, and the STARTUPINFO "show without activating" wish does not stop it (measured: the game
// window became the foreground window 1.6 s after launch). A CBT hook on the thread that creates the game window
// refuses every activation that the person did not ask for: a click (mouse button down) or Alt+Tab / Win still
// activate it, so a test window can be played by clicking on it. Enabled by the environment variable
// ZMP_NO_ACTIVATE=1, which only the test tools set (it has to be installed before the window exists, before the
// CVars are loaded). Called at the very start of main().
#ifdef _WIN32
static HHOOK sNoActivateHook = nullptr;

static bool UserAskedForActivation() {
    const int keys[] = { VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_MENU, VK_LWIN, VK_RWIN };
    for (int k : keys) {
        if (GetAsyncKeyState(k) & 0x8000) {
            return true;
        }
    }
    return false;
}

static LRESULT CALLBACK NoActivateCbtProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HCBT_ACTIVATE && !UserAskedForActivation()) {
        return 1; // refuse the activation
    }
    return CallNextHookEx(sNoActivateHook, code, wParam, lParam);
}
#endif

// Resource cache (finding T). libultraship's resource manager keeps what it has loaded in a table guarded by a lock,
// but it wrote "this file does not exist" into that table WITHOUT the lock. With alternative assets enabled (the
// default) the first load of every resource looks for "alt/<path>" first, does not find it, and made that unlocked
// write, on whichever thread was loading: the game's, the audio thread's or the loader pool's. Two threads loading
// new resources at the same moment (a scene load while the audio thread loads its samples) could then break the table
// under the thread that was reading it, and the game closed or hung inside the table's search. The lock is now in the
// library itself (libultraship/VENDORED.md). What is left here is the test of it.
namespace {
struct ResourceCacheAccess : Ship::ResourceManager {
    // (the table's lookup is protected and its "not found" value is a private type: neither is named here)
    template <class Line>
    static Line Look(Ship::ResourceManager& rm, Line (Ship::ResourceManager::*check)(const std::string&, bool),
                     const std::string& path) {
        return (rm.*check)(path, true);
    }
    // true: the table already says "not found" for this path (the load will write nothing)
    static bool SaysNotFound(Ship::ResourceManager& rm, const std::string& path) {
        auto line = Look(rm, &ResourceCacheAccess::CheckCache, path);
        const int kNotFound = 2; // ResourceLoadError { None, NotCached, NotFound }
        return line.index() == 0 && static_cast<int>(std::get<0>(line)) == kNotFound;
    }
    // true: the table has no entry for this path
    static bool SaysNotCached(Ship::ResourceManager& rm, const std::string& path) {
        auto line = Look(rm, &ResourceCacheAccess::CheckCache, path);
        const int kNotCached = 1;
        return line.index() == 0 && static_cast<int>(std::get<0>(line)) == kNotCached;
    }
};

// The "alt/" paths that do not exist, for every file of the archives.
std::vector<std::string> MissingAltPaths() {
    auto rm = Ship::Context::GetRawInstance()->GetResourceManager();
    auto files = rm->GetArchiveManager()->ListFiles();
    std::vector<std::string> out;
    const std::string& prefix = Ship::IResource::gAltAssetPrefix;
    for (const std::string& file : *files) {
        if (file.starts_with(prefix)) {
            continue;
        }
        std::string alt = prefix + file;
        if (rm->GetArchiveManager()->GetFilePriority(alt) < 0 &&
            rm->GetArchiveManager()->GetFilePriority(alt + ".meta") < 0) {
            out.push_back(std::move(alt));
        }
    }
    return out;
}
} // namespace

// Mutation test of the fixes made in the library's own code (libultraship/VENDORED.md): each mutant brings back one of
// the unguarded accesses (test builds only; in the demo Zmp_TestMutant is the constant 0).
static void ApplyResourceFaultMutants() {
    int faults = 0;
    faults |= Zmp_TestMutant("tabla_sin_cerrojo") ? 1 : 0;
    faults |= Zmp_TestMutant("descarga_sin_cerrojo") ? 2 : 0;
    faults |= Zmp_TestMutant("archivo_fantasma") ? 4 : 0;
    Ship::gZmpTestResourceFaults.store(faults);
}

extern "C" void Zmp_ResourceTestSetup(void) {
    ApplyResourceFaultMutants();
}

namespace {
// A resource that says, when it is destroyed, whether the resource table's lock was held at that moment: another
// thread asks the table a question and must get its answer at once.
struct LockProbe : Ship::IResource {
    Ship::ResourceManager* rm;
    std::atomic<int>* held;
    std::thread* asker;
    LockProbe(Ship::ResourceManager* rm, std::atomic<int>* held, std::thread* asker)
        : Ship::IResource(nullptr), rm(rm), held(held), asker(asker) {
    }
    void* GetRawPointer() override {
        return nullptr;
    }
    size_t GetPointerSize() override {
        return 0;
    }
    ~LockProbe() override {
        auto answered = std::make_shared<std::atomic<bool>>(false);
        Ship::ResourceManager* manager = rm;
        *asker = std::thread([manager, answered]() {
            manager->GetCachedResource(std::string("zmp_test/no_such_file_0"), true);
            answered->store(true);
        });
        for (int i = 0; i < 100 && !answered->load(); i++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        held->store(answered->load() ? 0 : 1);
    }
};
} // namespace

// Test (finding T): several threads use the resource table at the same time, like the game thread and the audio thread
// do when they load resources for the first time.
//   1. Every thread loads its share of the alternative path of every file of the archives, and of `made` paths that
//      exist in no version (neither the file nor its alternative): each of those loads writes "not found" into the
//      table. Afterwards every made-up path, and its alternative, must be in the table as "not found" (`lost` counts
//      the ones that are not: a write that another thread's write destroyed), and the file table must not have
//      learned any of them (`phantom`).
//   2. Half of the threads unload those made-up paths while the other half loads a second batch of them (the table
//      grows and moves under the unloads). Afterwards none of the first batch may be left (`left`).
//   3. A resource is put into the table and unloaded: it must be destroyed, and not while the table's lock is held
//      (`held`: a resource that loads or drops other resources when it dies would wait for itself).
// With the lock missing in the library, the game may also close or hang in here: that is the failure, too.
void Zmp_TestResourceRace(int threads, int made, ZmpResourceRace* r) {
    auto rm = Ship::Context::GetRawInstance()->GetResourceManager();
    auto am = rm->GetArchiveManager();
    const std::string& prefix = Ship::IResource::gAltAssetPrefix;
    std::vector<std::string> paths = MissingAltPaths();
    std::vector<std::string> first, second;
    for (int i = 0; i < made; i++) {
        first.push_back("zmp_test/no_such_file_" + std::to_string(i));
        second.push_back("zmp_test/no_such_file_b_" + std::to_string(i));
    }
    std::atomic<int> wrote{ 0 };
    std::vector<std::thread> pool;
    threads = threads < 2 ? 2 : (threads > 16 ? 16 : threads);
    for (int t = 0; t < threads; t++) {
        pool.emplace_back([&, t]() {
            for (size_t i = t; i < paths.size() || i < first.size(); i += threads) {
                if (i < first.size()) {
                    rm->LoadResourceProcess(first[i]);
                }
                if (i < paths.size()) {
                    if (!ResourceCacheAccess::SaysNotFound(*rm, paths[i])) {
                        wrote++;
                    }
                    rm->LoadResourceProcess(paths[i]);
                }
            }
        });
    }
    for (std::thread& th : pool) {
        th.join();
    }
    pool.clear();
    r->loads = (int)paths.size();
    r->wrote = wrote.load();
    r->made = made;
    r->lost = 0;
    r->phantom = 0;
    for (const std::string& path : first) {
        r->lost += ResourceCacheAccess::SaysNotFound(*rm, path) ? 0 : 1;
        r->lost += ResourceCacheAccess::SaysNotFound(*rm, prefix + path) ? 0 : 1;
        r->phantom += am->HasFile(path) ? 1 : 0;
        r->phantom += am->HasFile(prefix + path) ? 1 : 0;
    }
    for (int t = 0; t < threads; t++) {
        pool.emplace_back([&, t]() {
            int half = threads / 2;
            if (t < half) {
                for (size_t i = t; i < first.size(); i += half) {
                    rm->UnloadResource(first[i]);
                    rm->UnloadResource(prefix + first[i]);
                }
            } else {
                for (size_t i = t - half; i < second.size(); i += threads - half) {
                    rm->LoadResourceProcess(second[i]);
                }
            }
        });
    }
    for (std::thread& th : pool) {
        th.join();
    }
    r->left = 0;
    for (const std::string& path : first) {
        r->left += ResourceCacheAccess::SaysNotCached(*rm, path) ? 0 : 1;
        r->left += ResourceCacheAccess::SaysNotCached(*rm, prefix + path) ? 0 : 1;
    }
    for (const std::string& path : second) { // (the table ends as it started)
        rm->UnloadResource(path);
        rm->UnloadResource(prefix + path);
    }
    std::atomic<int> held{ -1 };
    std::thread asker;
    rm->CacheExternalResource("zmp_test/lock_probe", std::make_shared<LockProbe>(rm.get(), &held, &asker));
    rm->UnloadResource(std::string("zmp_test/lock_probe"));
    if (asker.joinable()) {
        asker.join();
    }
    r->held = held.load(); // (-1: the resource was not destroyed)
}

// Boot log (finding R): a file of its own, written with plain Win32 calls from the first line of main(), before the
// game's logger, its crash handler or its window exist. It records the steps of the start and every error-class
// exception the moment it is raised (first chance: also the ones somebody handles later, and the ones raised inside
// a window procedure or a hook, which Windows turns into exit code 0xC000041D without calling anybody). Each entry
// has the module and offset of every frame, so soh.pdb names them afterwards. Test instances also get a minidump of
// the first two. Presentation only: nothing here is read by the simulation.
#ifdef _WIN32
static char sBootLogPath[MAX_PATH] = "";
static char sBootDumpDir[MAX_PATH] = "";
static volatile LONG sBootLogEntries = 0;
static volatile LONG sBootDumps = 0;
static bool sBootIsTestInstance = false;
static SRWLOCK sBootLogLock = SRWLOCK_INIT;
static char sBootLogBuf[8192];

static void BootLogWrite(const char* text) {
    if (sBootLogPath[0] == 0) {
        return;
    }
    HANDLE h = CreateFileA(sBootLogPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(h, text, (DWORD)strlen(text), &written, nullptr);
        CloseHandle(h);
    }
}

static int BootLogStamp(char* out, size_t size) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    return snprintf(out, size, "[%04u-%02u-%02u %02u:%02u:%02u.%03u] pid=%lu tid=%lu ", t.wYear, t.wMonth, t.wDay,
                    t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, (unsigned long)GetCurrentProcessId(),
                    (unsigned long)GetCurrentThreadId());
}

static int BootLogFrame(char* out, size_t size, DWORD64 address) {
    HMODULE module = nullptr;
    char path[MAX_PATH] = "";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)address, &module) &&
        module != nullptr && GetModuleFileNameA(module, path, sizeof(path)) != 0) {
        const char* name = strrchr(path, '\\');
        return snprintf(out, size, "    %s+0x%llx\n", name != nullptr ? name + 1 : path,
                        (unsigned long long)(address - (DWORD64)module));
    }
    return snprintf(out, size, "    ?+0x%llx\n", (unsigned long long)address);
}

// (the layout of what the Visual C++ runtime passes with a C++ exception, to name its type)
static int BootLogCppException(char* out, size_t size, const EXCEPTION_RECORD* rec) {
    int n = 0;
    __try {
        if (rec->NumberParameters < 4 || rec->ExceptionInformation[2] == 0) {
            return 0;
        }
        const char* base = (const char*)rec->ExceptionInformation[3];
        const int* throwInfo = (const int*)rec->ExceptionInformation[2];
        const int* types = (const int*)(base + throwInfo[3]);
        for (int i = 0; i < types[0] && i < 6 && n < (int)size - 300; i++) {
            const int* catchable = (const int*)(base + types[1 + i]);
            const char* name = base + catchable[1] + 2 * sizeof(void*);
            n += snprintf(out + n, size - n, "    c++ type %.200s\n", name);
            if (strcmp(name, ".?AVexception@std@@") == 0) {
                auto e = (const std::exception*)((const char*)rec->ExceptionInformation[1] + catchable[2]);
                n += snprintf(out + n, size - n, "    what: %.250s\n", e->what());
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        n += snprintf(out + n, size - n, "    (c++ exception data unreadable)\n");
    }
    return n;
}

static int BootLogStack(char* out, size_t size, const CONTEXT* from) {
    int n = 0;
    __try {
        CONTEXT ctx = *from;
        for (int i = 0; i < 64 && ctx.Rip != 0 && n < (int)size - 400; i++) {
            n += BootLogFrame(out + n, size - n, ctx.Rip);
            DWORD64 imageBase = 0;
            PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &imageBase, nullptr);
            if (fn == nullptr) {
                ctx.Rip = *(DWORD64*)ctx.Rsp;
                ctx.Rsp += 8;
            } else {
                void* handlerData = nullptr;
                DWORD64 establisher = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, ctx.Rip, fn, &ctx, &handlerData, &establisher, nullptr);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { n += snprintf(out + n, size - n, "    (stack unreadable from here)\n"); }
    return n;
}

static void BootLogDump(PEXCEPTION_POINTERS ex) {
    char path[MAX_PATH + 64];
    snprintf(path, sizeof(path), "%s\\zmp_boot_%lu_%ld.dmp", sBootDumpDir, (unsigned long)GetCurrentProcessId(),
             (long)sBootDumps);
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return;
    }
    MINIDUMP_EXCEPTION_INFORMATION info = { GetCurrentThreadId(), ex, FALSE };
    MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), h,
                      (MINIDUMP_TYPE)(MiniDumpNormal | MiniDumpWithIndirectlyReferencedMemory), &info, nullptr,
                      nullptr);
    CloseHandle(h);
}

static LONG CALLBACK BootExceptionLog(PEXCEPTION_POINTERS ex) {
    const EXCEPTION_RECORD* rec = ex->ExceptionRecord;
    DWORD code = rec->ExceptionCode;
    bool cpp = code == 0xE06D7363;
    if ((code < 0xC0000000 && !cpp) || InterlockedIncrement(&sBootLogEntries) > 40) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    AcquireSRWLockExclusive(&sBootLogLock);
    char* out = sBootLogBuf;
    size_t size = sizeof(sBootLogBuf);
    int n = BootLogStamp(out, size);
    n += snprintf(out + n, size - n, "EXCEPTION 0x%08lx flags 0x%lx at %p", (unsigned long)code,
                  (unsigned long)rec->ExceptionFlags, rec->ExceptionAddress);
    for (DWORD i = 0; i < rec->NumberParameters && i < 4; i++) {
        n += snprintf(out + n, size - n, " p%lu=0x%llx", (unsigned long)i,
                      (unsigned long long)rec->ExceptionInformation[i]);
    }
    n += snprintf(out + n, size - n, "\n");
    if (cpp) {
        n += BootLogCppException(out + n, size - n, rec);
    }
    n += BootLogStack(out + n, size - n, ex->ContextRecord);
    BootLogWrite(out);
    if (!cpp && sBootIsTestInstance && InterlockedIncrement(&sBootDumps) <= 2) {
        BootLogDump(ex);
        BootLogWrite("    (minidump written next to this file)\n");
    }
    ReleaseSRWLockExclusive(&sBootLogLock);
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

extern "C" void Zmp_BootMark(const char* what) {
#ifdef _WIN32
    char line[320];
    int n = BootLogStamp(line, sizeof(line));
    snprintf(line + n, sizeof(line) - n, "%.200s\n", what);
    BootLogWrite(line);
#else
    (void)what;
#endif
}

extern "C" void Zmp_BootGuiBackendField(int value) {
    char line[96];
    snprintf(line, sizeof(line), "boot: gui backend field before anybody sets it = %d", value);
    Zmp_BootMark(line);
}

extern "C" void Zmp_InstallBootLog(void) {
#ifdef _WIN32
    char dir[MAX_PATH] = "";
    if (sBootLogPath[0] != 0 || GetCurrentDirectoryA(sizeof(dir) - 32, dir) == 0) {
        return;
    }
    snprintf(sBootDumpDir, sizeof(sBootDumpDir), "%s\\logs", dir);
    CreateDirectoryA(sBootDumpDir, nullptr);
    snprintf(sBootLogPath, sizeof(sBootLogPath), "%s\\zmp_boot.log", sBootDumpDir);
    WIN32_FILE_ATTRIBUTE_DATA attr;
    if (GetFileAttributesExA(sBootLogPath, GetFileExInfoStandard, &attr) &&
        (attr.nFileSizeHigh != 0 || attr.nFileSizeLow > 2 * 1024 * 1024)) {
        DeleteFileA(sBootLogPath); // (it only grows by a few lines per start: start again past 2 MB)
    }
    char value[8] = {};
    sBootIsTestInstance = GetEnvironmentVariableA("ZMP_NO_ACTIVATE", value, sizeof(value)) != 0 && value[0] == '1';
    AddVectoredExceptionHandler(1, BootExceptionLog);
    Zmp_BootMark(sBootIsTestInstance ? "boot: main (test instance)" : "boot: main");
#endif
}

extern "C" int Zmp_InstallNoActivateHook(void) {
#ifdef _WIN32
    char value[8] = {};
    if (GetEnvironmentVariableA("ZMP_NO_ACTIVATE", value, sizeof(value)) == 0 || strcmp(value, "1") != 0) {
        return 0;
    }
    if (Zmp_TestMutant("robo_foco")) {
        Zmp_TestFocusMutantStart(); // (mutation test: the window is allowed to take the focus when it opens)
        return 0;
    }
    if (sNoActivateHook == nullptr) {
        sNoActivateHook = SetWindowsHookExW(WH_CBT, NoActivateCbtProc, nullptr, GetCurrentThreadId());
    }
    return 1;
#else
    return 0;
#endif
}

// Audio of an instance plays only while one of its windows has the focus (presentation only; the
// mixer output is silenced, the game-side audio state is untouched).
static void UpdateFocusMute() {
    bool enabled = CVarGetInteger(ZMP_CVAR_MUTE_UNFOCUSED, 0) != 0;
    bool realMute = false;
#ifdef _WIN32
    if (enabled) {
        HWND fg = GetForegroundWindow();
        DWORD pid = 0;
        if (fg != nullptr) {
            GetWindowThreadProcessId(fg, &pid);
        }
        realMute = pid != GetCurrentProcessId();
    }
#endif
    // The speakers always follow the real focus.
    sAudioOutputMuted.store(realMute);
    // Tests: the decision with the focus the harness says this instance has (it never moves the real focus and never
    // unmutes the speakers, D-063).
    int override = CVarGetInteger(ZMP_CVAR_FOCUS_OVERRIDE, -1);
    if (enabled && override == 1 && Zmp_TestMutant("sonido_sin_foco")) {
        sAudioOutputMuted.store(false); // (mutation test: the simulated focus turns the speakers on)
    }
    Zmp_TestFocusMutantFrame();
    sAudioMuted.store(enabled && override >= 0 ? override == 0 : realMute);
}

#if defined(ZMP_HARNESS) && defined(_WIN32)
// Test instances: a game that crashes writes the same report as always and ends. The game's own handler then shows a
// "Crash" dialog that stays on the screen until somebody clicks it: a window of a test in front of whoever is using
// the PC (D-063), and a process that never ends by itself.
static LONG WINAPI TestCrashFilter(PEXCEPTION_POINTERS ex) {
    char line[64];
    snprintf(line, sizeof(line), "zmp: CRASH exception 0x%lx (test instance: no dialog)",
             (unsigned long)ex->ExceptionRecord->ExceptionCode);
    Zmp::Log(line);
    auto handler = Ship::Context::GetRawInstance()->GetCrashHandler();
    if (handler != nullptr) {
        handler->PrintStack(ex->ContextRecord);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

extern "C" void Zmp_Init(void) {
    Zmp::Log("zmp: init (name=" + std::string(CVarGetString(ZMP_CVAR_NAME, "Player")) + ")");

    auto gui = Ship::Context::GetRawInstance()->GetWindow()->GetGui();
    sRoomWindow = std::make_shared<Zmp::RoomWindow>(ZMP_CVAR_ROOM_WINDOW, "Sala ZMP");
    gui->AddGuiWindow(sRoomWindow);
    RegisterConsoleCommands();

#ifdef ZMP_HARNESS
    int harnessPort = CVarGetInteger(ZMP_CVAR_HARNESS_PORT, 0);
    if (harnessPort > 0 && harnessPort < 65536) {
#ifdef _WIN32
        if (!Zmp_TestMutant("cuadro_crash")) { // (mutation test: the game's own handler and its dialog)
            SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
            SetUnhandledExceptionFilter(TestCrashFilter);
        }
#endif
        if (!Zmp::Harness::Start((uint16_t)harnessPort)) {
            Zmp::Log("zmp: harness failed to start on port " + std::to_string(harnessPort));
        }
    }
#endif

    if (CVarGetInteger(ZMP_CVAR_AUTOCONNECT, 0)) {
        Zmp::Client::Get().Connect(CVarGetString(ZMP_CVAR_SERVER_HOST, ZMP_DEFAULT_HOST),
                                   (uint16_t)CVarGetInteger(ZMP_CVAR_SERVER_PORT, ZMP_DEFAULT_PORT),
                                   CVarGetString(ZMP_CVAR_ROOM, ZMP_DEFAULT_ROOM),
                                   CVarGetString(ZMP_CVAR_NAME, "Player"));
    }
}

extern "C" void Zmp_Deinit(void) {
    Zmp::Log("zmp: shutdown");
#ifdef _WIN32
    sWatchdogStop = true;
    if (sWatchdog.joinable()) {
        sWatchdog.join();
    }
#endif
#ifdef ZMP_HARNESS
    Zmp::Harness::Stop();
#endif
    Zmp::Client::Get().Disconnect();
    sRoomWindow = nullptr;
    Zmp::LogClose();
}

extern "C" void Zmp_OnFrameBegin(void) {
#ifdef _WIN32
    if (sGameThread == nullptr) {
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &sGameThread, 0, FALSE,
                        DUPLICATE_SAME_ACCESS);
        sWatchdogStop = false;
        sWatchdog = std::thread(WatchdogLoop);
    }
    sFrameBegins++;
#endif
    static bool sLoggedMemory = false;
    if (!sLoggedMemory) {
        sLoggedMemory = true;
        Zmp::Log(std::string("zmp: system heap at fixed address: ") + (Zmp_FixedHeapActive() ? "yes" : "NO") +
                 ", resource slots: " + (Zmp::ResSlots::IsActive() ? "yes" : "NO"));
    }
    UpdateFocusMute();
    RunConsoleActions();
#ifdef ZMP_HARNESS
    if (Zmp::Harness::IsRunning()) {
        Zmp::Harness::OnFrameBegin(sFrameCount);
    }
#endif
    Zmp::Sim::OnFrameBegin();
}

extern "C" void Zmp_OnPadRead(void* pads) {
    sFrameCount++;
#ifdef ZMP_HARNESS
    if (Zmp::Harness::IsRunning()) {
        Zmp::Harness::ApplyInput(pads);
    }
#endif
    Zmp::Sim::OnPadRead(pads);
}

extern "C" void Zmp_OnTickEnd(void) {
    Zmp::Sim::OnTickEnd();
}

// Frame interpolation replays the matrix operations of the tick on the game's own matrix stack
// while drawing the intermediate frames, so after rendering its contents depend on the
// interpolation frame rate. The stack is restored to its end-of-tick state after rendering.
static MtxF sMatrixBackup[20];
static MtxF* sMatrixStackBackup = nullptr;
static MtxF* sMatrixCurrentBackup = nullptr;

extern "C" void Zmp_BeforeRender(void) {
    Matrix_ZmpGetPointers(&sMatrixStackBackup, &sMatrixCurrentBackup);
    if (sMatrixStackBackup != nullptr) {
        memcpy(sMatrixBackup, sMatrixStackBackup, sizeof(sMatrixBackup));
    }
}

extern "C" void Zmp_AfterRender(void) {
    MtxF* stack = nullptr;
    MtxF* current = nullptr;
    Matrix_ZmpGetPointers(&stack, &current);
    if (sMatrixStackBackup != nullptr && stack == sMatrixStackBackup) {
        memcpy(sMatrixStackBackup, sMatrixBackup, sizeof(sMatrixBackup));
        Matrix_ZmpSetPointers(sMatrixStackBackup, sMatrixCurrentBackup);
    }
#ifdef ZMP_HARNESS
    Zmp::Lockstep::BurnTestFrameCost(); // (D-101: a test can make every frame of this machine cost more)
#endif
}
