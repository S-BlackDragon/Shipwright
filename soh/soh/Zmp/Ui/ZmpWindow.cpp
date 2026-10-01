#include "ZmpWindow.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <string>

#include <imgui.h>
#include <ship/Context.h>
#include <libultraship/bridge/consolevariablebridge.h>

#include "soh/Zmp/ZmpCVars.h"
#include "soh/Zmp/Net/ZmpClient.h"
#include "soh/Zmp/Sim/Session.h"
#include "soh/Zmp/Net/Lockstep.h"
#include "soh/Zmp/Sim/ZmpPlayers.h"
#include "soh/util.h"
#include "soh/Zmp/Net/Autosave.h"
#include "soh/Zmp/Ui/ChatBox.h"
#include <chrono>
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

static void CenteredLine(float y, const std::string& text, ImU32 color, float scale) {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImDrawList* dl = ImGui::GetForegroundDrawList(vp);
    float fontSize = ImGui::GetFontSize() * scale;
    ImVec2 size = ImGui::GetFont()->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, text.c_str());
    ImVec2 pos(vp->Pos.x + std::max(6.0f, (vp->Size.x - size.x) * 0.5f), y);
    dl->AddRectFilled(ImVec2(pos.x - 4, pos.y - 2), ImVec2(pos.x + size.x + 4, pos.y + size.y + 2),
                      IM_COL32(0, 0, 0, 170), 3.0f);
    dl->AddText(ImGui::GetFont(), fontSize, pos, color, text.c_str());
}

static void ReviveBar(float y, float frac, ImU32 color) {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImDrawList* dl = ImGui::GetForegroundDrawList(vp);
    float w = std::min(320.0f, vp->Size.x * 0.6f);
    float h = std::max(8.0f, ImGui::GetFontSize() * 0.6f);
    ImVec2 a(vp->Pos.x + (vp->Size.x - w) * 0.5f, y);
    ImVec2 b(a.x + w, a.y + h);
    dl->AddRectFilled(ImVec2(a.x - 2, a.y - 2), ImVec2(b.x + 2, b.y + 2), IM_COL32(0, 0, 0, 190), 3.0f);
    dl->AddRectFilled(a, ImVec2(a.x + w * std::clamp(frac, 0.0f, 1.0f), b.y), color, 2.0f);
    dl->AddRect(a, b, IM_COL32(255, 255, 255, 200), 2.0f);
}

// Lines the downed overlay drew in the last frame (tests read them through the harness).
static std::string sDownedText;
// When this client first saw each player downed (the short "X ha caido" notice).
static std::chrono::steady_clock::time_point sDownedSince[ZMP_MAX_PLAYERS];
static bool sDownedSeen[ZMP_MAX_PLAYERS];

// Phase 5b: what the downed state has to say goes to the message box (once per situation); only the revive bar is
// drawn over the game. The text of the current situation is kept for the tests.
static void DownedLine(float y, const std::string& text, ImU32 color, float scale) {
    Chat::Post(Chat::Category::Players, text, "downed:" + text, 4.0);
    sDownedText += text + '\n';
}

std::string DownedOverlayText() {
    return sDownedText;
}

// Downed players, revive progress, spectator target (phase 3). Reads the simulation, draws nothing into it.
static void DrawDownedOverlay(int local, float y) {
    float line = ImGui::GetFontSize() * 1.2f + 8.0f;
    sDownedText.clear();
    auto now = std::chrono::steady_clock::now();
    for (int k = 0; k < ZMP_MAX_PLAYERS; k++) {
        bool down = Players::SlotDowned(k) && Players::SlotPlayer(k) != nullptr;
        if (down && !sDownedSeen[k]) {
            sDownedSince[k] = now;
        }
        sDownedSeen[k] = down;
    }
    if (Players::GroupDefeat()) {
        return; // everybody is down: the game over screen says it all
    }
    if (Players::SlotDowned(local)) {
        int who = Players::SlotReviver(local);
        int prog = Players::SlotReviveProgress(local);
        DownedLine(y, "Has caido. Un companero puede revivirte manteniendo A a tu lado.", IM_COL32(255, 120, 120, 255),
                   1.1f);
        y += line;
        int t = Players::SlotSpectate(local);
        if (t >= 0) {
            DownedLine(y, "Viendo a " + Lockstep::SlotName(t) + " (C-izquierda / C-derecha: cambiar)",
                       IM_COL32(220, 220, 220, 255), 0.9f);
            y += line;
        }
        if (who >= 0 && prog > 0) {
            DownedLine(y, Lockstep::SlotName(who) + " te esta reviviendo", IM_COL32(140, 255, 140, 255), 1.0f);
            ReviveBar(y, (float)prog / ZMP_REVIVE_TICKS, IM_COL32(90, 220, 90, 255));
        }
        return;
    }
    for (int k = 0; k < ZMP_MAX_PLAYERS; k++) {
        if (k == local || !Players::SlotDowned(k) || Players::SlotPlayer(k) == nullptr) {
            continue;
        }
        int who = Players::SlotReviver(k);
        int prog = Players::SlotReviveProgress(k);
        // Distance as the simulation measures it for the revive (Players.cpp), read only.
        Player* me = Players::SlotPlayer(local);
        Player* down = Players::SlotPlayer(k);
        bool inRange = false;
        if (me != nullptr) {
            float dx = me->actor.world.pos.x - down->actor.world.pos.x;
            float dy = me->actor.world.pos.y - down->actor.world.pos.y;
            float dz = me->actor.world.pos.z - down->actor.world.pos.z;
            inRange = dx * dx + dy * dy + dz * dz <= ZMP_REVIVE_RANGE * ZMP_REVIVE_RANGE;
        }
        if (who == local && prog > 0) {
            DownedLine(y, "Reviviendo a " + Lockstep::SlotName(k) + "... sigue manteniendo A",
                       IM_COL32(140, 255, 140, 255), 1.1f);
            ReviveBar(y, (float)prog / ZMP_REVIVE_TICKS, IM_COL32(90, 220, 90, 255));
            y += line;
        } else if (inRange) {
            DownedLine(y, "Manten A para revivir a " + Lockstep::SlotName(k), IM_COL32(255, 170, 120, 255), 1.0f);
            y += line;
        } else if (std::chrono::duration<double>(now - sDownedSince[k]).count() < 3.0) {
            // Out of range: only a short notice when the player falls.
            DownedLine(y, Lockstep::SlotName(k) + " ha caido", IM_COL32(255, 200, 170, 220), 0.85f);
            y += line;
        }
    }
}

void RoomWindow::DrawLockstepOverlay(const Lockstep::Status& ls, float y) {
    using Clock = std::chrono::steady_clock;
    using Chat::Category;
    bool debug = CVarGetInteger(ZMP_CVAR_DEBUG_OVERLAY, 0) != 0;
    static uint32_t sSeenResyncs = 0;
    static Clock::time_point sResyncShownAt;
    if (ls.resyncsSeen != sSeenResyncs) {
        sSeenResyncs = ls.resyncsSeen;
        sResyncShownAt = Clock::now();
        Chat::Post(Category::Debug, "Resincronizado con el grupo (tick " + std::to_string(ls.lastResyncTick) + ")");
    }
    bool recentResync = ls.resyncsSeen > 0 &&
                        std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - sResyncShownAt).count() < 8;
    static int sSeenDelay = -1;
    if (ls.phase == Lockstep::Phase::Running && ls.delay != sSeenDelay) {
        sSeenDelay = ls.delay;
        Chat::Post(Category::Debug, "Retraso de entrada: " + std::to_string(ls.delay) + " ticks");
    }
    std::string line;
    ImU32 color = IM_COL32(120, 255, 120, 255);
    switch (ls.phase) {
        case Lockstep::Phase::WaitingGroup:
            line = "Esperando a que alguien cargue una partida para crear el grupo";
            color = IM_COL32(255, 230, 120, 255);
            Chat::Post(Category::System, line, "phase:waiting", 3.0);
            break;
        case Lockstep::Phase::Joining:
            line = ls.freeRun ? "Entrando en " + Lockstep::SceneName(ls.detachScene) + "..."
                              : std::string("Entrando en la partida del grupo...");
            color = IM_COL32(255, 230, 120, 255);
            if (!ls.freeRun) {
                Chat::Post(Category::System, line, "phase:joining", 3.0);
            }
            break;
        case Lockstep::Phase::Detached:
            // Phase 5: on the way to another scene (no countdown: nobody waits for anybody).
            line = "Entrando en " + Lockstep::SceneName(ls.detachScene) + "...";
            color = IM_COL32(170, 220, 255, 255);
            break;
        case Lockstep::Phase::Running: {
            std::string me = Lockstep::SlotName(ls.slot);
            line = me + " | tick " + std::to_string(ls.tick) + " | D=" + std::to_string(ls.delay) + " | " +
                   (recentResync ? "DESYNC: resincronizado" : "HASH OK");
            if (recentResync) {
                color = IM_COL32(255, 160, 60, 255);
            }
            break;
        }
        default:
            break;
    }
    ImGuiViewport* vp = ImGui::GetMainViewport();
    {
        // Phase 5b: entering the group of a scene. The group did not stop: this client loads its state behind the
        // black of its own scene change and catches up; the picture fades in once its Link is in the scene.
        static Clock::time_point sUncoveredAt;
        static bool sWasCovered = false;
        int alpha = 0;
        if (ls.covered) {
            sWasCovered = true;
            alpha = 255;
        } else if (sWasCovered) {
            sWasCovered = false;
            sUncoveredAt = Clock::now();
        }
        if (!ls.covered && sUncoveredAt.time_since_epoch().count() != 0) {
            double ms = std::chrono::duration<double, std::milli>(Clock::now() - sUncoveredAt).count();
            alpha = ms < 300.0 ? (int)(255.0 * (1.0 - ms / 300.0)) : 0;
        }
        if (alpha > 0) {
            ImGui::GetForegroundDrawList(vp)->AddRectFilled(
                vp->Pos, ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y), IM_COL32(0, 0, 0, alpha));
        }
    }
    if (debug) {
        CenteredLine(y, line, color, 1.0f); // the diagnostic line (test instances; off in the demos)
    }
    if (ls.phase == Lockstep::Phase::Running && ls.waiting) {
        // (only real connection drops get here: nobody waits for a slow player)
        std::string who = ls.waitingFor.empty() ? "el servidor" : ls.waitingFor;
        Chat::Post(Category::System, "Sin conexion con el grupo: esperando a " + who + "...", "waiting", 3.0);
    }
    if (ls.phase == Lockstep::Phase::Running) {
        // Phase 5: who just arrived somewhere (the ZMP window lists where everybody is).
        std::string me = Client::Get().GetStatus().name;
        for (auto& n : ls.sceneNotices) {
            if (n.age < 4.0 && n.name != me) {
                Chat::Post(Category::Players, n.name + " ha entrado en " + Lockstep::SceneName(n.scene),
                           "scene:" + n.name + ":" + std::to_string(n.scene), 6.0);
            }
        }
        // Phase 5b: somebody of this scene travels with a warp song; holding L goes along. Short and small, over
        // the game (it asks for a decision in a few seconds).
        Players::WarpInvite inv = Players::Invite();
        if (inv.by >= 0 && inv.by != ls.slot) {
            std::string text =
                Lockstep::SlotName(inv.by) + " va a " + Lockstep::SceneName(inv.scene) + ": manten L para ir tambien";
            if (inv.hold > 0) {
                text += " (" + std::to_string(inv.hold * 100 / ZMP_INVITE_HOLD_TICKS) + " %)";
            }
            CenteredLine(vp->Pos.y + vp->Size.y * 0.74f, text, IM_COL32(255, 230, 120, 255), 0.9f);
        }
    }
    if (ls.phase == Lockstep::Phase::Running && Zmp_MultiActive()) {
        DrawDownedOverlay(ls.slot, vp->Pos.y + vp->Size.y * 0.66f);
        // Group game over (D-058): the menu reads the anchor's pad; the others see who decides.
        std::string chooser = Lockstep::GameOverChooser();
        if (!chooser.empty()) {
            Chat::Post(Category::System, chooser + " (el anfitrion) elige si guardar y continuar", "chooser", 4.0, 2.0f);
        }
        // A save from the pause menu (or the game over menu) of a player that is not the leader writes nothing.
        if (ls.saveSkipped) {
            Chat::Post(Category::System, "Solo el anfitrion guarda la partida", "saveskipped", 4.0);
        }
        // Phase 4: "Guardado" on the screen of the PC that wrote the autosave (D-054; phase 5: the save owner).
        if (Autosave::SecondsSinceSave() < 2.0) {
            Chat::Post(Category::System, "Partida guardada", "saved", 3.0, 0.5f);
        }
        // Phase 4: this player's heat / deep water timer (the original's timer is not used in multiplayer). Part of
        // the game's HUD, not a message.
        int heat = Players::SlotHeatSeconds(ls.slot);
        if (heat >= 0) {
            ImDrawList* hdl = ImGui::GetForegroundDrawList(vp);
            char buf[32];
            snprintf(buf, sizeof(buf), "%d:%02d", heat / 60, heat % 60);
            float fs = ImGui::GetFontSize() * 1.8f;
            ImVec2 hp(vp->Pos.x + vp->Size.x * 0.06f, vp->Pos.y + vp->Size.y * 0.20f);
            hdl->AddRectFilled(ImVec2(hp.x - 6, hp.y - 3), ImVec2(hp.x + fs * 3.0f, hp.y + fs + 3),
                               IM_COL32(0, 0, 0, 150), 4.0f);
            hdl->AddText(ImGui::GetFont(), fs, hp,
                         heat <= 10 ? IM_COL32(255, 80, 60, 255) : IM_COL32(255, 200, 90, 255), buf);
        }
        // Phase 4: what the other players got (their text box is not shown here).
        for (auto& n : Players::RecentNotices(5.0)) {
            if (n.slot == ls.slot) {
                continue;
            }
            Chat::Post(Category::Items, Lockstep::SlotName(n.slot) + " ha obtenido: " + SohUtils::GetItemName(n.itemId),
                       "item:" + std::to_string(n.slot) + ":" + std::to_string(n.itemId), 5.5);
        }
    }
    if (!ls.lastError.empty() && ls.phase != Lockstep::Phase::Running) {
        Chat::Post(Category::System, ls.lastError, "error:" + ls.lastError, 10.0, 2.0f);
    }
}

void RoomWindow::DrawOverlay() {
    if (!CVarGetInteger(ZMP_CVAR_OVERLAY, 1)) {
        return;
    }
    using Chat::Category;
    bool debug = CVarGetInteger(ZMP_CVAR_DEBUG_OVERLAY, 0) != 0;
    NetStatus st = Client::Get().GetStatus();
    std::string name = CVarGetString(ZMP_CVAR_NAME, "Player");
    std::string text = "ZMP " + name + " | " + StatusLine(st);
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImDrawList* dl = ImGui::GetForegroundDrawList(vp);
    ImVec2 pos(vp->Pos.x + 6.0f, vp->Pos.y + 6.0f); // x is centred below, once the text size is known
    float fontSize = ImGui::GetFontSize() * 0.85f;
    // Wrap long messages (a rejection explains the reason) inside the window.
    float wrap = vp->Size.x - 16.0f;
    ImVec2 size = ImGui::GetFont()->CalcTextSizeA(fontSize, FLT_MAX, wrap, text.c_str());
    if (debug) {
        // Top centre: the hearts and the magic bar of the HUD are on the left, the buttons on the right.
        pos.x = vp->Pos.x + std::max(6.0f, (vp->Size.x - size.x) * 0.5f);
        dl->AddRectFilled(ImVec2(pos.x - 3, pos.y - 2), ImVec2(pos.x + size.x + 3, pos.y + size.y + 2),
                          IM_COL32(0, 0, 0, 150), 3.0f);
        ImVec4 c = StatusColor(st.state);
        dl->AddText(ImGui::GetFont(), fontSize, pos, ImGui::ColorConvertFloat4ToU32(c), text.c_str(), nullptr, wrap);
    }
    {
        // The connection, when it changes (a rejection explains why and stays longer).
        static int sSeenState = -1;
        static size_t sSeenPlayers = 0;
        static std::string sSeenLine;
        std::string now = StatusLine(st);
        bool bad = st.state == NetState::Rejected || st.state == NetState::Error;
        if ((int)st.state != sSeenState || (bad && now != sSeenLine)) {
            if (sSeenState != -1 || st.state != NetState::Disconnected) {
                Chat::Post(Category::System, now, "", 0.0, bad ? 3.0f : 1.0f);
            }
            sSeenState = (int)st.state;
            sSeenLine = now;
            sSeenPlayers = st.players.size();
        } else if (st.state == NetState::Connected && st.players.size() != sSeenPlayers) {
            Chat::Post(Category::Players, st.players.size() > sSeenPlayers ? "Un jugador ha entrado en la sala (" +
                                                                                  std::to_string(st.players.size()) + ")"
                                                                            : "Un jugador ha salido de la sala (" +
                                                                                  std::to_string(st.players.size()) + ")");
            sSeenPlayers = st.players.size();
        }
    }

    // Lockstep (multiplayer) or determinism session (replay, recording, pause).
    auto ls = Lockstep::GetStatus();
    if (!ls.endNotice.empty()) {
        // After a group game over "no" (D-058), on the file select screen.
        Chat::Post(Category::System, ls.endNotice, "end:" + ls.endNotice, 20.0, 2.5f);
    }
    if (ls.phase != Lockstep::Phase::Idle) {
        DrawLockstepOverlay(ls, pos.y + size.y + 6.0f);
        Chat::Draw();
        return;
    }
    Chat::Draw();
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
    ImVec2 size2 = ImGui::GetFont()->CalcTextSizeA(bigSize, FLT_MAX, 0.0f, line.c_str());
    ImVec2 pos2(vp->Pos.x + std::max(6.0f, (vp->Size.x - size2.x) * 0.5f), pos.y + size.y + 6.0f);
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

    // Autosave of the leader and resume of the last session of the room (phase 4, D-054).
    int minutes = CVarGetInteger(ZMP_CVAR_AUTOSAVE_MINUTES, 5);
    ImGui::Text("Autoguardado cada (minutos, 0 = nunca)");
    if (ImGui::InputInt("##ZmpAutosave", &minutes)) {
        CVarSetInteger(ZMP_CVAR_AUTOSAVE_MINUTES, std::clamp(minutes, 0, 60));
        Ship::Context::GetRawInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
    }
    bool resume = CVarGetInteger(ZMP_CVAR_RESUME_SESSION, 0) != 0;
    ImGui::BeginDisabled(active);
    if (ImGui::Checkbox("Reanudar sesion (el que crea el grupo carga la ultima sesion guardada de esta sala)",
                        &resume)) {
        CVarSetInteger(ZMP_CVAR_RESUME_SESSION, resume ? 1 : 0);
        Ship::Context::GetRawInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
    }
    ImGui::EndDisabled();

    // Phase 5: whom to join when connecting (empty: the group of your scene, or the biggest one), and starting the
    // room over with your own game (the room keeps its game on the server).
    std::string follow = CVarGetString(ZMP_CVAR_FOLLOW, "");
    ImGui::BeginDisabled(active);
    ImGui::Text("Unirse a (nombre de un jugador, opcional)");
    if (UIWidgets::InputString("##ZmpFollow", &follow)) {
        CVarSetString(ZMP_CVAR_FOLLOW, follow.c_str());
        Ship::Context::GetRawInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
    }
    bool overwrite = CVarGetInteger(ZMP_CVAR_OVERWRITE_ROOM_GAME, 0) != 0;
    if (ImGui::Checkbox("Empezar la sala con mi partida (borra el progreso guardado de esta sala en el servidor)",
                        &overwrite)) {
        CVarSetInteger(ZMP_CVAR_OVERWRITE_ROOM_GAME, overwrite ? 1 : 0);
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
    Chat::DrawSettings();
    ImGui::Separator();
    ImGui::TextColored(StatusColor(st.state), "%s", StatusLine(st).c_str());
    if (st.state == NetState::Connected) {
        auto ls = Lockstep::GetStatus();
        ImGui::Text("RTT: %d ms | Retraso D: %d ticks | %s", st.rttMs, ls.delay, Lockstep::PhaseName(ls.phase));
        if (ls.phase == Lockstep::Phase::Running) {
            ImGui::Text("Tick %u | esperas >250 ms: %u | resincronizaciones: %u", ls.tick, ls.stalls, ls.resyncsSeen);
        }
        if (ImGui::BeginTable("##ZmpPlayers", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn("Jugador");
            ImGui::TableSetupColumn("Estado");
            ImGui::TableSetupColumn("Zona");
            ImGui::TableSetupColumn("RTT");
            ImGui::TableSetupColumn("");
            ImGui::TableHeadersRow();
            std::map<std::string, Lockstep::SlotInfo> zones;
            for (auto& z : ls.players) {
                zones[z.name] = z;
            }
            for (auto& p : st.players) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                bool me = p.id == st.playerId;
                if (me) {
                    ImGui::TextColored(ImVec4(0.6f, 1.0f, 0.6f, 1.0f), "%s (tu)", p.name.c_str());
                } else {
                    ImGui::Text("%s", p.name.c_str());
                }
                ImGui::TableNextColumn();
                const char* state = p.state == "playing"        ? "jugando"
                                    : p.state == "joining"      ? "entrando"
                                    : p.state == "disconnected" ? "desconectado"
                                    : p.state == "waiting"      ? "esperando"
                                                                : "en sala";
                ImGui::Text("%s", state);
                ImGui::TableNextColumn();
                auto zit = zones.find(p.name);
                if (zit != zones.end() && zit->second.scene >= 0) {
                    bool other = zit->second.group != 0 && zit->second.group != ls.groupId;
                    std::string zone = Lockstep::SceneName(zit->second.scene) + (other && !me ? " (otra zona)" : "");
                    ImGui::Text("%s", zone.c_str());
                } else {
                    ImGui::Text("-");
                }
                ImGui::TableNextColumn();
                if (p.rtt >= 0) {
                    ImGui::Text("%d ms", p.rtt);
                } else {
                    ImGui::Text("-");
                }
                ImGui::TableNextColumn();
                ImGui::Text("%s", zit != zones.end() && zit->second.leader ? "lider" : "");
            }
            ImGui::EndTable();
        }
        if (ls.phase == Lockstep::Phase::Running && ImGui::Button("Salir de la partida del grupo")) {
            Lockstep::Leave();
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
