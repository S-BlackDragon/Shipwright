#include "ChatBox.h"

#include <algorithm>
#include <chrono>
#include <deque>
#include <map>
#include <mutex>

#include <imgui.h>
#include <ship/Context.h>
#include <ship/window/Window.h>
#include <ship/window/gui/Gui.h>
#include <libultraship/bridge/consolevariablebridge.h>

#include "soh/Zmp/ZmpCVars.h"
#include "soh/Zmp/ZmpLog.h"

namespace Zmp::Chat {

namespace {

using Clock = std::chrono::steady_clock;

struct Message {
    Category category;
    std::string text;
    Clock::time_point at;
    float hold;
};

std::mutex sMutex;
std::deque<Message> sMessages; // the last ones (what the box can show)
std::vector<std::pair<int, std::string>> sHistory;
std::map<std::string, Clock::time_point> sLastByKey;
std::vector<std::string> sVisible;

const char* kCategoryCVar[] = { ZMP_CVAR_CHAT_CAT_PLAYERS, ZMP_CVAR_CHAT_CAT_ITEMS, ZMP_CVAR_CHAT_CAT_SYSTEM,
                                ZMP_CVAR_CHAT_CAT_DEBUG };
const int kCategoryDefault[] = { 1, 1, 1, 0 };
const char* kCategoryName[] = { "Jugadores (entra, sale, cae, revive)", "Objetos (lo que consiguen los demas)",
                                "Sistema (conexion, guardado, avisos)", "Depuracion (resincronizaciones, retraso)" };

bool CategoryOn(Category c) {
    int i = (int)c;
    return CVarGetInteger(kCategoryCVar[i], kCategoryDefault[i]) != 0;
}

void Save() {
    Ship::Context::GetRawInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
}

} // namespace

void Post(Category category, const std::string& text, const std::string& key, double quietSeconds, float hold) {
    if (text.empty()) {
        return;
    }
    auto now = Clock::now();
    std::lock_guard<std::mutex> lock(sMutex);
    if (!key.empty()) {
        auto it = sLastByKey.find(key);
        if (it != sLastByKey.end() && std::chrono::duration<double>(now - it->second).count() < quietSeconds) {
            it->second = now; // (still the same state: it stays quiet until the state has been gone for a while)
            return;
        }
        sLastByKey[key] = now;
    }
    sMessages.push_back({ category, text, now, hold });
    while (sMessages.size() > 64) {
        sMessages.pop_front();
    }
    if (sHistory.size() < 4096) {
        sHistory.push_back({ (int)category, text });
    }
    Log("chat[" + std::to_string((int)category) + "]: " + text);
}

void PostFrom(const std::string& player, const std::string& text) {
    Post(Category::Players, player + ": " + text);
}

void Draw() {
    std::vector<Message> show;
    double seconds = std::clamp((double)CVarGetFloat(ZMP_CVAR_CHAT_SECONDS, 6.0f), 1.0, 60.0);
    int lines = std::clamp(CVarGetInteger(ZMP_CVAR_CHAT_LINES, 6), 1, 20);
    auto now = Clock::now();
    {
        std::lock_guard<std::mutex> lock(sMutex);
        sVisible.clear();
        if (CVarGetInteger(ZMP_CVAR_CHAT_HIDDEN, 0)) {
            return;
        }
        for (auto& m : sMessages) {
            double age = std::chrono::duration<double>(now - m.at).count();
            if (age < seconds * m.hold && CategoryOn(m.category)) {
                show.push_back(m);
            }
        }
        if ((int)show.size() > lines) {
            show.erase(show.begin(), show.end() - lines);
        }
        for (auto& m : show) {
            sVisible.push_back(m.text);
        }
    }
    if (show.empty()) {
        return;
    }
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImDrawList* dl = ImGui::GetForegroundDrawList(vp);
    float fontSize = ImGui::GetFontSize() * std::clamp(CVarGetFloat(ZMP_CVAR_CHAT_FONT_SCALE, 0.8f), 0.4f, 3.0f);
    float left = vp->Pos.x + vp->Size.x * std::clamp(CVarGetFloat(ZMP_CVAR_CHAT_X, 0.02f), 0.0f, 0.95f);
    float bottom = vp->Pos.y + vp->Size.y * std::clamp(CVarGetFloat(ZMP_CVAR_CHAT_BOTTOM, 0.82f), 0.05f, 1.0f);
    float width = vp->Size.x * std::clamp(CVarGetFloat(ZMP_CVAR_CHAT_WIDTH, 0.38f), 0.1f, 1.0f);
    width = std::min(width, vp->Pos.x + vp->Size.x - left - 4.0f);
    float bg = std::clamp(CVarGetFloat(ZMP_CVAR_CHAT_BG_ALPHA, 0.35f), 0.0f, 1.0f);
    float r = std::clamp(CVarGetFloat(ZMP_CVAR_CHAT_COLOR_R, 1.0f), 0.0f, 1.0f);
    float g = std::clamp(CVarGetFloat(ZMP_CVAR_CHAT_COLOR_G, 1.0f), 0.0f, 1.0f);
    float b = std::clamp(CVarGetFloat(ZMP_CVAR_CHAT_COLOR_B, 1.0f), 0.0f, 1.0f);
    float pad = fontSize * 0.25f;
    // Laid out from the bottom up: the newest message is the lowest line.
    float y = bottom;
    ImFont* font = ImGui::GetFont();
    for (auto it = show.rbegin(); it != show.rend(); ++it) {
        double age = std::chrono::duration<double>(now - it->at).count();
        double left_s = seconds * it->hold - age;
        float alpha = (float)std::clamp(left_s / 0.6, 0.0, 1.0); // fades out at the end
        ImVec2 size = font->CalcTextSizeA(fontSize, FLT_MAX, width - 2 * pad, it->text.c_str());
        y -= size.y + 2 * pad;
        if (y < vp->Pos.y) {
            break;
        }
        if (bg > 0.0f) {
            dl->AddRectFilled(ImVec2(left, y), ImVec2(left + std::min(width, size.x + 2 * pad), y + size.y + 2 * pad),
                              IM_COL32(0, 0, 0, (int)(255 * bg * alpha)), 3.0f);
        } else {
            // without a background: a dark edge keeps the text readable over the game
            ImU32 shadow = IM_COL32(0, 0, 0, (int)(220 * alpha));
            dl->AddText(font, fontSize, ImVec2(left + pad + 1, y + pad + 1), shadow, it->text.c_str(), nullptr,
                        width - 2 * pad);
        }
        dl->AddText(font, fontSize, ImVec2(left + pad, y + pad),
                    IM_COL32((int)(255 * r), (int)(255 * g), (int)(255 * b), (int)(255 * alpha)), it->text.c_str(),
                    nullptr, width - 2 * pad);
        y -= 2.0f;
    }
}

void DrawSettings() {
    if (!ImGui::CollapsingHeader("Cuadro de mensajes")) {
        return;
    }
    bool hidden = CVarGetInteger(ZMP_CVAR_CHAT_HIDDEN, 0) != 0;
    if (ImGui::Checkbox("Ocultar el cuadro de mensajes", &hidden)) {
        CVarSetInteger(ZMP_CVAR_CHAT_HIDDEN, hidden ? 1 : 0);
        Save();
    }
    struct Slider {
        const char* label;
        const char* cvar;
        float def, lo, hi;
        const char* fmt;
    };
    const Slider sliders[] = {
        { "Posicion horizontal", ZMP_CVAR_CHAT_X, 0.02f, 0.0f, 0.9f, "%.2f" },
        { "Posicion vertical (borde inferior)", ZMP_CVAR_CHAT_BOTTOM, 0.82f, 0.1f, 1.0f, "%.2f" },
        { "Ancho", ZMP_CVAR_CHAT_WIDTH, 0.38f, 0.15f, 1.0f, "%.2f" },
        { "Tamano de letra", ZMP_CVAR_CHAT_FONT_SCALE, 0.8f, 0.5f, 2.5f, "%.2f" },
        { "Opacidad del fondo (0 = sin fondo, solo texto)", ZMP_CVAR_CHAT_BG_ALPHA, 0.35f, 0.0f, 1.0f, "%.2f" },
        { "Segundos que dura cada mensaje", ZMP_CVAR_CHAT_SECONDS, 6.0f, 2.0f, 30.0f, "%.0f" },
    };
    for (auto& s : sliders) {
        float v = CVarGetFloat(s.cvar, s.def);
        ImGui::Text("%s", s.label);
        if (ImGui::SliderFloat((std::string("##") + s.cvar).c_str(), &v, s.lo, s.hi, s.fmt)) {
            CVarSetFloat(s.cvar, v);
            Save();
        }
    }
    int lines = CVarGetInteger(ZMP_CVAR_CHAT_LINES, 6);
    ImGui::Text("Lineas visibles (alto)");
    if (ImGui::SliderInt("##ZmpChatLines", &lines, 1, 15)) {
        CVarSetInteger(ZMP_CVAR_CHAT_LINES, lines);
        Save();
    }
    float color[3] = { CVarGetFloat(ZMP_CVAR_CHAT_COLOR_R, 1.0f), CVarGetFloat(ZMP_CVAR_CHAT_COLOR_G, 1.0f),
                       CVarGetFloat(ZMP_CVAR_CHAT_COLOR_B, 1.0f) };
    ImGui::Text("Color del texto");
    if (ImGui::ColorEdit3("##ZmpChatColor", color, ImGuiColorEditFlags_NoInputs)) {
        CVarSetFloat(ZMP_CVAR_CHAT_COLOR_R, color[0]);
        CVarSetFloat(ZMP_CVAR_CHAT_COLOR_G, color[1]);
        CVarSetFloat(ZMP_CVAR_CHAT_COLOR_B, color[2]);
        Save();
    }
    ImGui::Text("Que mensajes ver");
    for (int i = 0; i < (int)Category::Count; i++) {
        bool on = CVarGetInteger(kCategoryCVar[i], kCategoryDefault[i]) != 0;
        if (ImGui::Checkbox(kCategoryName[i], &on)) {
            CVarSetInteger(kCategoryCVar[i], on ? 1 : 0);
            Save();
        }
    }
    bool debug = CVarGetInteger(ZMP_CVAR_DEBUG_OVERLAY, 0) != 0;
    if (ImGui::Checkbox("Mostrar la linea de diagnostico (conexion, tick, retraso, hash) arriba", &debug)) {
        CVarSetInteger(ZMP_CVAR_DEBUG_OVERLAY, debug ? 1 : 0);
        Save();
    }
    if (ImGui::Button("Probar el cuadro")) {
        Post(Category::System, "Mensaje de prueba del cuadro de ZMP");
    }
    ImGui::SameLine();
    if (ImGui::Button("Valores por defecto")) {
        for (auto& s : sliders) {
            CVarClear(s.cvar);
        }
        for (const char* c : { ZMP_CVAR_CHAT_HIDDEN, ZMP_CVAR_CHAT_LINES, ZMP_CVAR_CHAT_COLOR_R, ZMP_CVAR_CHAT_COLOR_G,
                               ZMP_CVAR_CHAT_COLOR_B, ZMP_CVAR_CHAT_CAT_PLAYERS, ZMP_CVAR_CHAT_CAT_ITEMS,
                               ZMP_CVAR_CHAT_CAT_SYSTEM, ZMP_CVAR_CHAT_CAT_DEBUG }) {
            CVarClear(c);
        }
        Save();
    }
}

std::vector<std::string> VisibleLines() {
    std::lock_guard<std::mutex> lock(sMutex);
    return sVisible;
}

std::vector<std::pair<int, std::string>> History() {
    std::lock_guard<std::mutex> lock(sMutex);
    return sHistory;
}

} // namespace Zmp::Chat
