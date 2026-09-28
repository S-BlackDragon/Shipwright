#include "Zmp.h"

#include <memory>
#include <string>

#include <ship/Context.h>
#include <ship/window/Window.h>
#include <libultraship/bridge/consolevariablebridge.h>

#include "ZmpCVars.h"
#include "ZmpLog.h"
#include "Harness/Harness.h"
#include "Net/ZmpClient.h"
#include "Ui/ZmpWindow.h"

static uint32_t sFrameCount = 0;
static std::shared_ptr<Zmp::RoomWindow> sRoomWindow;

extern "C" uint32_t Zmp_GetFrameCount(void) {
    return sFrameCount;
}

extern "C" void Zmp_Init(void) {
    Zmp::Log("zmp: init (name=" + std::string(CVarGetString(ZMP_CVAR_NAME, "Player")) + ")");

    auto gui = Ship::Context::GetRawInstance()->GetWindow()->GetGui();
    sRoomWindow = std::make_shared<Zmp::RoomWindow>(ZMP_CVAR_ROOM_WINDOW, "Sala ZMP");
    gui->AddGuiWindow(sRoomWindow);

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

extern "C" void Zmp_OnPadRead(void* pads) {
    sFrameCount++;
#ifdef ZMP_HARNESS
    if (Zmp::Harness::IsRunning()) {
        Zmp::Harness::Frame(pads, sFrameCount);
    }
#endif
}
