#include "CVarProfile.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <ship/Context.h>
#include <ship/config/Config.h>
#include <ship/config/ConsoleVariable.h>
#include <libultraship/bridge/consolevariablebridge.h>

#include "soh/ShipInit.hpp"
#include "soh/Network/CrowdControl/CrowdControl.h"
#include "soh/Network/Sail/Sail.h"
#include "soh/Network/Anchor/Anchor.h"
#include "soh/Zmp/ZmpLog.h"

namespace Zmp::CVarProfile {

namespace {

// docs/CVAR_PROFILE.md, section 6 ("Lista para el código").
const char* kLockedPrefixes[] = {
    "gEnhancements.",   "gCheats.", "gRandoSettings.", "gRandoEnhancements.",
    "gDeveloperTools.", "gRemote.", "gGeneral.",       "gCrowdControl",
};

const char* kFreeExceptions[] = {
    "gEnhancements.DisableBombBillboarding",
    "gEnhancements.DisableLinkSwordTrail",
    "gEnhancements.DynamicWalletIcon",
    "gEnhancements.NoHUDHeartAnimation",
    "gEnhancements.PulsateBossIcon",
    "gEnhancements.AlwaysShowDungeonMinimapIcon",
    "gEnhancements.FixDungeonMinimapIcon",
    "gEnhancements.DrawLineupTick",
    "gEnhancements.EnemyHealthBar",
    "gEnhancements.GerudoWarriorClothingFix",
    "gEnhancements.ShowDoorLocksOnBothSides",
    "gEnhancements.BetterAmmoRendering",
    "gGeneral.HasSeenPresetModal",
    "gGeneral.HideBuiltInPresets",
    "gDeveloperTools.LogLevel",
    "gDeveloperTools.ResourceLogging",
    "gRemote.CrowdControl.Host",
    "gRemote.CrowdControl.Port",
    "gRemote.Sail.Host",
    "gRemote.Sail.Port",
};

// ".*" at the end means "this prefix".
const char* kLockedExceptions[] = {
    "gCosmetics.Link.SwordScale.Changed",
    "gCosmetics.Link.SwordScale.Value",
    "gCosmetics.Link.HeadScale.Changed",
    "gCosmetics.Link.HeadScale.Value",
    "gCosmetics.Goron.NeckLength",
    "gCosmetics.Kak.Windmill_Speed.Changed",
    "gCosmetics.Kak.Windmill_Speed.Value",
    "gCosmetics.UnfixGoronSpin",
    "gAudioEditor.LostWoodsConsistentVolume",
    "gAudioEditor.EnemyBGMDisable",
    "gAudioEditor.RandomizeAudioGenModes",
    "gAudioEditor.ReplacedSequences.*",
    "gSettings.EnableMouse",
    "gSettings.DisableThirdPersonMouse",
    "gSettings.Controls.RightStickAim",
    "gSettings.Controls.InvertAimingXAxis",
    "gSettings.Controls.InvertAimingYAxis",
    "gSettings.Controls.InvertShieldAimingXAxis",
    "gSettings.Controls.InvertShieldAimingYAxis",
    "gSettings.Controls.InvertZAimingYAxis",
    "gSettings.FirstPersonCameraSensitivity.Enabled",
    "gSettings.FirstPersonCameraSensitivity.X",
    "gSettings.FirstPersonCameraSensitivity.Y",
    "gSettings.MoveInFirstPerson",
    "gSettings.DisableFirstPersonAutoCenterView",
    "gSettings.FreeLook.*",
    "gSettings.A11yDisableIdleCam",
    "gSettings.A11yNoHeatHaze",
    "gSettings.A11yNoJabuWobble",
    "gSettings.A11yNoScreenFlashForFinishingBlow",
    "gSettings.CustomOcarina.*",
    "gSettings.DpadInText",
    "gSettings.DPadOnPause",
    "gSettings.DpadHoldChange",
    "gSettings.ResetBtn",
    "gSettings.AltAssets",
    "gSettings.Mods.AlternateAssetsHotkey",
    "gSettings.EnabledMods",
    "gSettings.Languages",
    "gOpenWindows.SaveEditor",
    "gOpenWindows.ActorViewer",
    "gOpenWindows.SohConsole",
    "gOpenWindows.Console",
};

// Values forced for the whole session (besides being equal on every client).
struct Forced {
    const char* name;
    int32_t value;
};
const Forced kForced[] = {
    { "gEnhancements.WidescreenActorCulling", 0 },
    { "gEnhancements.DisableDrawDistance", 1 },
    { "gEnhancements.ExtendedCullingExcludeGlitchActors", 0 },
    { "gGeneral.LetItSnow", 0 },
    { "gCheats.TimeSync", 0 },
    { "gCheats.SaveStatesEnabled", 0 },
    { "gEnhancements.RandomizedEnemySizes", 0 },
    { "gEnhancements.Autosave", 0 },
    { "gSettings.EnableMouse", 0 },
    { "gSettings.Mods.AlternateAssetsHotkey", 0 },
    { "gDeveloperTools.DebugEnabled", 0 },
    { "gRemote.CrowdControl.Enabled", 0 },
    { "gRemote.Sail.Enabled", 0 },
    { "gRemote.Anchor.Enabled", 0 },
    { "gCrowdControl", 0 },
    { "gOpenWindows.SaveEditor", 0 },
    { "gOpenWindows.ActorViewer", 0 },
};

// Written by the game itself during play: locked, but not part of the profile or the check.
const char* kDerived[] = {
    "gEnhancements.MirroredWorld",
    "gGeneral.PrevTime",
    "gGeneral.PauseMenuAnimatedLinkTriforce",
};

bool sActive = false;
uint64_t sHash = 0;
uint32_t sReverts = 0;
uint32_t sFrame = 0;
std::map<std::string, std::string> sSnapshot; // locked name -> serialized value ("" = unset)
std::vector<std::pair<std::string, std::optional<int32_t>>> sForcedOriginals;

bool StartsWith(const std::string& s, const char* p) {
    return s.rfind(p, 0) == 0;
}

bool Matches(const std::string& name, const char* pattern) {
    std::string p = pattern;
    if (p.size() >= 2 && p.compare(p.size() - 2, 2, ".*") == 0) {
        return StartsWith(name, p.substr(0, p.size() - 1).c_str());
    }
    return name == p;
}

bool IsDerived(const std::string& name) {
    for (const char* d : kDerived) {
        if (name == d) {
            return true;
        }
    }
    return false;
}

std::string ValueOf(const std::string& name) {
    auto cv = Ship::Context::GetRawInstance()->GetConsoleVariables()->Get(name.c_str());
    if (cv == nullptr) {
        return "";
    }
    char buf[64];
    switch (cv->Type) {
        case Ship::ConsoleVariableType::Integer:
            snprintf(buf, sizeof(buf), "i%d", cv->Integer);
            return buf;
        case Ship::ConsoleVariableType::Float:
            snprintf(buf, sizeof(buf), "f%.9g", cv->Float);
            return buf;
        case Ship::ConsoleVariableType::String:
            return std::string("s") + (cv->String ? cv->String : "");
        case Ship::ConsoleVariableType::Color:
            snprintf(buf, sizeof(buf), "c%u,%u,%u,%u", cv->Color.r, cv->Color.g, cv->Color.b, cv->Color.a);
            return buf;
        case Ship::ConsoleVariableType::Color24:
            snprintf(buf, sizeof(buf), "c%u,%u,%u", cv->Color24.r, cv->Color24.g, cv->Color24.b);
            return buf;
    }
    return "?";
}

void Restore(const std::string& name, const std::string& value) {
    if (value.empty()) {
        CVarClear(name.c_str());
    } else if (value[0] == 'i') {
        CVarSetInteger(name.c_str(), std::stoi(value.substr(1)));
    } else if (value[0] == 'f') {
        CVarSetFloat(name.c_str(), std::stof(value.substr(1)));
    } else if (value[0] == 's') {
        CVarSetString(name.c_str(), value.substr(1).c_str());
    } else if (value[0] == 'c') {
        unsigned r = 0, g = 0, b = 0, a = 255;
        int n = sscanf(value.c_str() + 1, "%u,%u,%u,%u", &r, &g, &b, &a);
        if (n == 4) {
            CVarSetColor(name.c_str(), Color_RGBA8{ (uint8_t)r, (uint8_t)g, (uint8_t)b, (uint8_t)a });
        } else {
            CVarSetColor24(name.c_str(), Color_RGB8{ (uint8_t)r, (uint8_t)g, (uint8_t)b });
        }
    }
    ShipInit::Init(name);
}

// Names of every CVar that exists in memory (via the config JSON, which ConsoleVariable::Save
// fills from the in-memory table).
std::vector<std::string> AllCVarNames() {
    auto ctx = Ship::Context::GetRawInstance();
    ctx->GetConsoleVariables()->Save();
    nlohmann::json root = ctx->GetConfig()->GetNestedJson();
    std::vector<std::string> names;
    if (!root.contains("CVars")) {
        return names;
    }
    std::vector<std::pair<std::string, const nlohmann::json*>> stack = { { "", &root["CVars"] } };
    while (!stack.empty()) {
        auto [prefix, node] = stack.back();
        stack.pop_back();
        for (auto it = node->begin(); it != node->end(); ++it) {
            std::string name = prefix.empty() ? it.key() : prefix + "." + it.key();
            if (it->is_object()) {
                // Colors are objects with R/G/B(/A) members; everything else is a nested block.
                if (it->contains("R") && it->contains("G") && it->contains("B")) {
                    names.push_back(name);
                } else {
                    stack.push_back({ name, &(*it) });
                }
            } else {
                names.push_back(name);
            }
        }
    }
    return names;
}

uint64_t ComputeHash(const std::map<std::string, std::string>& snap) {
    uint64_t h = 0xCBF29CE484222325ULL;
    for (auto& [name, value] : snap) {
        // An empty string is the same as unset (SoH writes some string CVars as "" on first use).
        if (IsDerived(name) || value.empty() || value == "s") {
            continue;
        }
        std::string line = name + "=" + value + "\n";
        for (unsigned char c : line) {
            h ^= c;
            h *= 0x100000001B3ULL;
        }
    }
    return h;
}

std::map<std::string, std::string> Snapshot() {
    std::map<std::string, std::string> snap;
    for (auto& name : AllCVarNames()) {
        if (IsLocked(name)) {
            snap[name] = ValueOf(name);
        }
    }
    for (auto& f : kForced) {
        snap[f.name] = ValueOf(f.name);
    }
    return snap;
}

void DisableRemotes() {
    if (CrowdControl::Instance != nullptr) {
        CrowdControl::Instance->Disable();
    }
    if (Sail::Instance != nullptr) {
        Sail::Instance->Disable();
    }
    if (Anchor::Instance != nullptr) {
        Anchor::Instance->Disable();
    }
}

} // namespace

bool IsLocked(const std::string& name) {
    for (const char* p : kLockedExceptions) {
        if (Matches(name, p)) {
            return true;
        }
    }
    for (const char* p : kFreeExceptions) {
        if (name == p) {
            return false;
        }
    }
    for (const char* p : kLockedPrefixes) {
        if (StartsWith(name, p)) {
            return true;
        }
    }
    return false;
}

void BeginSession() {
    if (sActive) {
        return;
    }
    sForcedOriginals.clear();
    for (auto& f : kForced) {
        auto cv = Ship::Context::GetRawInstance()->GetConsoleVariables()->Get(f.name);
        std::optional<int32_t> copy;
        if (cv != nullptr && cv->Type == Ship::ConsoleVariableType::Integer) {
            copy = cv->Integer;
        }
        sForcedOriginals.push_back({ f.name, copy });
        if (CVarGetInteger(f.name, INT32_MIN) != f.value) {
            CVarSetInteger(f.name, f.value);
            ShipInit::Init(f.name);
        }
    }
    DisableRemotes();
    sSnapshot = Snapshot();
    sHash = ComputeHash(sSnapshot);
    sReverts = 0;
    sActive = true;
    char buf[160];
    snprintf(buf, sizeof(buf), "zmp: CVar profile locked (%zu CVars, hash %016llX)", sSnapshot.size(),
             (unsigned long long)sHash);
    Log(buf);
}

void EndSession() {
    if (!sActive) {
        return;
    }
    sActive = false;
    for (auto& [name, cv] : sForcedOriginals) {
        if (!cv.has_value()) {
            CVarClear(name.c_str());
        } else {
            CVarSetInteger(name.c_str(), *cv);
        }
        ShipInit::Init(name);
    }
    sForcedOriginals.clear();
    Log("zmp: CVar profile unlocked");
}

bool Active() {
    return sActive;
}

void Enforce() {
    if (!sActive) {
        return;
    }
    sFrame++;
    // Cheap check of the known locked CVars every 20 frames; full rescan (finds CVars created during
    // the session) every 200 frames.
    if (sFrame % 20 != 0) {
        return;
    }
    std::vector<std::string> extra;
    if (sFrame % 200 == 0) {
        for (auto& name : AllCVarNames()) {
            if (IsLocked(name) && !sSnapshot.count(name)) {
                extra.push_back(name);
            }
        }
    }
    for (auto& [name, value] : sSnapshot) {
        if (IsDerived(name)) {
            continue;
        }
        std::string now = ValueOf(name);
        if (now != value) {
            Restore(name, value);
            sReverts++;
            Log("zmp: CVar bloqueada durante la sesion, se restaura: " + name);
        }
    }
    for (auto& name : extra) {
        if (IsDerived(name)) {
            continue;
        }
        Restore(name, "");
        sReverts++;
        Log("zmp: CVar bloqueada creada durante la sesion, se elimina: " + name);
    }
}

uint64_t Hash() {
    if (sActive) {
        return sHash;
    }
    // Before a session: the profile the session will use (forced CVars at their forced value). The list
    // goes to logs/cvar_profile.txt so two clients that differ can be compared.
    auto snap = Snapshot();
    char buf[24];
    for (auto& f : kForced) {
        snprintf(buf, sizeof(buf), "i%d", f.value);
        snap[f.name] = buf;
    }
    FILE* out = fopen("logs/cvar_profile.txt", "w");
    if (out != nullptr) {
        for (auto& [name, value] : snap) {
            fprintf(out, "%s=%s\n", name.c_str(), value.c_str());
        }
        fclose(out);
    }
    return ComputeHash(snap);
}

uint32_t RevertCount() {
    return sReverts;
}

} // namespace Zmp::CVarProfile
