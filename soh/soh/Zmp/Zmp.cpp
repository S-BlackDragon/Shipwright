#include "Zmp.h"
#include "Sim/ZmpSim.h"

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
#include "macros.h"
extern PlayState* gPlayState;
}

static uint32_t sFrameCount = 0;
static std::shared_ptr<Zmp::RoomWindow> sRoomWindow;
static std::atomic<bool> sAudioMuted{ false };

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
                            { "yaw", Ship::ArgumentType::TEXT, true } } });
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

// Audio of an instance plays only while one of its windows has the focus (presentation only; the
// mixer output is silenced, the game-side audio state is untouched).
static void UpdateFocusMute() {
    bool mute = false;
    if (CVarGetInteger(ZMP_CVAR_MUTE_UNFOCUSED, 0)) {
#ifdef _WIN32
        HWND fg = GetForegroundWindow();
        DWORD pid = 0;
        if (fg != nullptr) {
            GetWindowThreadProcessId(fg, &pid);
        }
        mute = pid != GetCurrentProcessId();
#endif
    }
    if (mute != sAudioMuted.load()) {
        sAudioMuted.store(mute);
    }
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
#ifdef ZMP_HARNESS
    Zmp::Harness::Stop();
#endif
    Zmp::Client::Get().Disconnect();
    sRoomWindow = nullptr;
    Zmp::LogClose();
}

extern "C" void Zmp_OnFrameBegin(void) {
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
