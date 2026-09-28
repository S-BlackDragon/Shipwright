#include "ZmpWindow.h"

#include <string>

#include <imgui.h>
#include <ship/Context.h>
#include <libultraship/bridge/consolevariablebridge.h>

#include "soh/Zmp/ZmpCVars.h"
#include "soh/Zmp/Net/ZmpClient.h"
#include "soh/Zmp/Sim/Session.h"
#include "soh/SohGui/UIWidgets.hpp"
#include "soh/SohGui/SohMenu.h"

namespace SohGui {
extern std::shared_ptr<SohMenu> mSohMenu;
} // namespace SohGui

namespace Zmp {

static std::string StatusLine(const NetStatus& st) {
    std::string where = st.host + ":" + std::to_string(st.port);
    switch (st.state) {
        case NetState::Connected:
            return "Conectado a " + where + " (sala " + st.room + ", " + std::to_string(st.players.size()) +
                   (st.players.size() == 1 ? " jugador)" : " jugadores)");
        case NetState::Connecting:
            return "Conectando a " + where + "...";
        case NetState::Rejected:
            return "Rechazado por " + where + ": " + (st.detail.empty() ? st.reason : st.detail);
        case NetState::Error:
            return st.detail.empty() ? ("Error: " + st.reason) : st.detail;
        case NetState::Disconnected:
        default:
            return "Desconectado";
    }
}

static ImVec4 StatusColor(NetState state) {
    switch (state) {
        case NetState::Connected:
            return ImVec4(0.55f, 1.0f, 0.55f, 1.0f);
        case NetState::Connecting:
            return ImVec4(1.0f, 0.9f, 0.4f, 1.0f);
        case NetState::Rejected:
        case NetState::Error:
            return ImVec4(1.0f, 0.45f, 0.45f, 1.0f);
        default:
            return ImVec4(0.8f, 0.8f, 0.8f, 1.0f);
    }
}

void RoomWindow::DrawOverlay() {
    if (!CVarGetInteger(ZMP_CVAR_OVERLAY, 1)) {
        return;
    }
    NetStatus st = Client::Get().GetStatus();
    std::string name = CVarGetString(ZMP_CVAR_NAME, "Player");
    std::string text = "ZMP " + name + " | " + StatusLine(st);
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImDrawList* dl = ImGui::GetForegroundDrawList(vp);
    ImVec2 pos(vp->Pos.x + 6.0f, vp->Pos.y + 6.0f);
    float fontSize = ImGui::GetFontSize() * 0.85f;
    // Wrap long messages (a rejection explains the reason) inside the window.
    float wrap = vp->Size.x - 16.0f;
    ImVec2 size = ImGui::GetFont()->CalcTextSizeA(fontSize, FLT_MAX, wrap, text.c_str());
    dl->AddRectFilled(ImVec2(pos.x - 3, pos.y - 2), ImVec2(pos.x + size.x + 3, pos.y + size.y + 2),
                      IM_COL32(0, 0, 0, 150), 3.0f);
    ImVec4 c = StatusColor(st.state);
    dl->AddText(ImGui::GetFont(), fontSize, pos, ImGui::ColorConvertFloat4ToU32(c), text.c_str(), nullptr, wrap);

    // Second line: determinism session (replay against the recorded hashes, recording, pause).
    auto ss = Sim::GetStatus();
    if (ss.mode == Sim::Mode::Off) {
        return;
    }
    std::string line;
    ImU32 color = IM_COL32(230, 230, 230, 255);
    if (ss.armed) {
        line = ss.mode == Sim::Mode::Replaying ? "REPLAY preparando..." : "GRABACION preparando...";
    } else if (ss.mode == Sim::Mode::Recording) {
        line = "GRABANDO tick " + std::to_string(ss.tick);
        color = IM_COL32(255, 200, 80, 255);
    } else if (ss.mismatches > 0) {
        line = "HASH MISMATCH (tick " + std::to_string(ss.firstMismatch) + ") | tick " + std::to_string(ss.tick);
        color = IM_COL32(255, 80, 80, 255);
    } else {
        line = std::string(ss.replayFinished ? "HASH OK | FIN " : "HASH OK | tick ") + std::to_string(ss.tick) + " / " +
               std::to_string(ss.replayLength);
        color = IM_COL32(120, 255, 120, 255);
    }
    if (ss.paused) {
        line += " | PAUSA";
    }
    float bigSize = ImGui::GetFontSize() * 1.1f;
    ImVec2 pos2(pos.x, pos.y + size.y + 6.0f);
    ImVec2 size2 = ImGui::GetFont()->CalcTextSizeA(bigSize, FLT_MAX, 0.0f, line.c_str());
    dl->AddRectFilled(ImVec2(pos2.x - 3, pos2.y - 2), ImVec2(pos2.x + size2.x + 3, pos2.y + size2.y + 2),
                      IM_COL32(0, 0, 0, 170), 3.0f);
    dl->AddText(ImGui::GetFont(), bigSize, pos2, color, line.c_str());
}

void RoomWindow::DrawConnectionForm() {
    NetStatus st = Client::Get().GetStatus();
    bool active = st.state == NetState::Connected || st.state == NetState::Connecting || st.state == NetState::Error;

    std::string host = CVarGetString(ZMP_CVAR_SERVER_HOST, ZMP_DEFAULT_HOST);
    int port = CVarGetInteger(ZMP_CVAR_SERVER_PORT, ZMP_DEFAULT_PORT);
    std::string room = CVarGetString(ZMP_CVAR_ROOM, ZMP_DEFAULT_ROOM);
    std::string name = CVarGetString(ZMP_CVAR_NAME, "Player");

    ImGui::BeginDisabled(active);
    ImGui::Text("Servidor (host)");
    if (UIWidgets::InputString("##ZmpHost", &host)) {
        CVarSetString(ZMP_CVAR_SERVER_HOST, host.c_str());
        Ship::Context::GetRawInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
    }
    ImGui::Text("Puerto");
    if (ImGui::InputInt("##ZmpPort", &port)) {
        CVarSetInteger(ZMP_CVAR_SERVER_PORT, port);
        Ship::Context::GetRawInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
    }
    ImGui::Text("Sala");
    if (UIWidgets::InputString("##ZmpRoom", &room)) {
        CVarSetString(ZMP_CVAR_ROOM, room.c_str());
        Ship::Context::GetRawInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
    }
    ImGui::Text("Nombre");
    if (UIWidgets::InputString("##ZmpName", &name)) {
        CVarSetString(ZMP_CVAR_NAME, name.c_str());
        Ship::Context::GetRawInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
    }
    ImGui::EndDisabled();

    bool valid = !host.empty() && port > 0 && port < 65536 && !room.empty() && !name.empty();
    if (!active) {
        ImGui::BeginDisabled(!valid);
        if (ImGui::Button("Conectar")) {
            Client::Get().Connect(host, (uint16_t)port, room, name);
        }
        ImGui::EndDisabled();
    } else if (ImGui::Button("Desconectar")) {
        Client::Get().Disconnect();
    }

    ImGui::Separator();
    ImGui::TextColored(StatusColor(st.state), "%s", StatusLine(st).c_str());
    if (st.state == NetState::Connected) {
        ImGui::Text("RTT: %d ms", st.rttMs);
        for (auto& p : st.players) {
            if (p.id == st.playerId) {
                ImGui::TextColored(ImVec4(0.8f, 1.0f, 0.8f, 1.0f), "#%u %s (tu)", p.id, p.name.c_str());
            } else {
                ImGui::Text("#%u %s", p.id, p.name.c_str());
            }
        }
    }
}

void RoomWindow::DrawElement() {
    DrawConnectionForm();
}

void RoomWindow::Draw() {
    DrawOverlay();
    if (!IsVisible()) {
        return;
    }
    ImGui::SetNextWindowSize(ImVec2(320, 360), ImGuiCond_FirstUseEver);
    bool open = true;
    if (ImGui::Begin("Sala ZMP", &open, ImGuiWindowFlags_NoDocking)) {
        DrawElement();
    }
    ImGui::End();
    if (!open) {
        CVarSetInteger(ZMP_CVAR_ROOM_WINDOW, 0);
        Hide();
    }
}

} // namespace Zmp

// Network > ZMP entry in the SoH menu.
static void ZmpMainMenu(WidgetInfo& info) {
    ImGui::SeparatorText("ZMP (cooperativo)");
    Zmp::RoomWindow::DrawConnectionForm();
    ImGui::Spacing();
    if (ImGui::Button("Mostrar u ocultar ventana de sala")) {
        auto win = Ship::Context::GetRawInstance()->GetWindow()->GetGui()->GetGuiWindow("Sala ZMP");
        if (win != nullptr) {
            win->ToggleVisibility();
        }
    }
}

static void RegisterZmpMenu() {
    WidgetPath path = { "Network", "ZMP", SECTION_COLUMN_1 };
    SohGui::mSohMenu->AddWidget(path, "ZmpMainMenu", WIDGET_CUSTOM).CustomFunction(ZmpMainMenu).HideInSearch(true);
}

static RegisterMenuInitFunc sZmpMenuInit(RegisterZmpMenu);
