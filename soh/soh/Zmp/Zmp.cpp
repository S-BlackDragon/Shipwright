#include "Zmp.h"
#include "soh/Zmp/Test/Mutants.h"
#include "Sim/ZmpSim.h"
#include "Sim/ZmpPlayers.h"

#include <atomic>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include <ship/Context.h>
#include <ship/window/Window.h>
#include <libultraship/bridge/consolevariablebridge.h>

#include "ZmpCVars.h"
#include "ZmpLog.h"
#include "Harness/Harness.h"
#include "Net/ZmpClient.h"
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

static void LogGameThreadStack() {
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

extern "C" void Zmp_Init(void) {
    Zmp::Log("zmp: init (name=" + std::string(CVarGetString(ZMP_CVAR_NAME, "Player")) + ")");

    auto gui = Ship::Context::GetRawInstance()->GetWindow()->GetGui();
    sRoomWindow = std::make_shared<Zmp::RoomWindow>(ZMP_CVAR_ROOM_WINDOW, "Sala ZMP");
    gui->AddGuiWindow(sRoomWindow);
    RegisterConsoleCommands();

#ifdef ZMP_HARNESS
    int harnessPort = CVarGetInteger(ZMP_CVAR_HARNESS_PORT, 0);
    if (harnessPort > 0 && harnessPort < 65536) {
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
}
