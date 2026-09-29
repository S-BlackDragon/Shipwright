#pragma once

#include <string>

#include <ship/window/gui/GuiWindow.h>

#include "soh/Zmp/Net/Lockstep.h"

namespace Zmp {

// Room window (connection settings, status, player list) plus a small always-on overlay
// in the top-left corner with the player name and connection state. Local UI only.
class RoomWindow : public Ship::GuiWindow {
  public:
    using GuiWindow::GuiWindow;

    void InitElement() override {
    }
    void DrawElement() override;
    void Draw() override;
    void UpdateElement() override {
    }

    // Connection form shared by this window and the Network > ZMP menu.
    static void DrawConnectionForm();
    static void DrawOverlay();
    static void DrawLockstepOverlay(const Lockstep::Status& ls, float y);
};

// Text the downed / revive overlay drew in the last frame (tests).
std::string DownedOverlayText();

} // namespace Zmp
