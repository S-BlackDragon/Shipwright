// ZMP: per-player screen effects (phase 4; docs/DECISIONES.md D-053). See ZmpPlayers.h.
//
// Effects of the WORLD are simulation and everybody sees them where they happen (sword glow, sparks, particles,
// explosions, the spin attack's trail). Effects of the SCREEN belong to the player who causes them and only its own
// client draws them:
//   - Letterbox (black bars) and HUD visibility that a player's own cameras ask for (Z-targeting, aiming, first
//     person, the chest and item cameras...). In multiplayer every player's camera records its interface flags here
//     instead of changing the shared letterbox and HUD (Camera_UpdateInterface); only the cameras of cutscenes
//     everybody watches change the shared ones. Each client animates the local player's letterbox and HUD with the
//     game's own rules and draws with them (the shared ones while a global cutscene, the game over or the local
//     player's own text box are up). The letterbox is only used to cut the picture (scissor): nothing in the
//     simulation reads it. The HUD alphas are only drawn.
//   - The A button label and Navi's call: only the local player's update sets them (Interface_SetDoAction is not
//     read by the simulation).
//   - Quakes a player causes shake only that player's camera; quakes of the world shake everybody's (z_quake.c).
//   - Controller rumble of a player: only on its own controller. The hookshot / bow reticle: only on the aiming
//     player's screen.
//   - The white flash of a finishing blow: only on the screen of the player who struck (the freeze of the actors
//     that comes with it is simulation and stays for everybody).

#include "ZmpPlayers.h"
#include "soh/Zmp/Test/Mutants.h"

#include <algorithm>
#include <cstring>

extern "C" {
#include "variables.h"
#include "functions.h"
#include "macros.h"
extern PlayState* gPlayState;
u32 ShrinkWindow_GetCurrentVal(void);
void ShrinkWindow_SetCurrentVal(s32 currentVal);
void Interface_ZmpStepHudAlphas(PlayState* play);
}

#include <libultraship/bridge/consolevariablebridge.h>

namespace {

constexpr int kAlphaCount = 13;

struct LocalScreen {
    bool init = false;
    s32 lbTarget = 0;
    s32 lbCur = 0;
    s16 camAlpha = 0;
    u16 hudMode = 0;
    u16 nextHudMode = 0;
    u16 hudTimer = 0;
    u16 alphas[kAlphaCount] = {};
    u32 frame = 0xFFFFFFFF;
    bool wasEvent = false;
};
LocalScreen sL;

int sFlashOwner = -1;
// D-124 (F4): the player whose screen an actor's screen fill is (-1 none) and the frame it was asked for in
int sFillOwner = -1;
u32 sFillFrame = 0;
u32 sFillHiddenFrame = 0xFFFFFFFF; // (tests) the last frame whose picture was drawn without such a fill
s32 sDrawnBars = 0;                // the black bars the last picture was cut with (tests read it)

bool sBarsSwapped = false;
s32 sBarsKeep = 0;
bool sFlashSwapped = false;
u8 sFlashKeep = 0;
bool sHudSwapped = false;
u16 sHudKeep[kAlphaCount];

u16* Alpha(InterfaceContext* ic, int i) {
    u16* f[kAlphaCount] = { &ic->aAlpha,          &ic->bAlpha,         &ic->cLeftAlpha,  &ic->cDownAlpha,
                            &ic->cRightAlpha,     &ic->healthAlpha,    &ic->dpadUpAlpha, &ic->dpadDownAlpha,
                            &ic->dpadLeftAlpha,   &ic->dpadRightAlpha, &ic->magicAlpha,  &ic->minimapAlpha,
                            (u16*)&ic->startAlpha };
    return f[i];
}

int Local() {
    return Zmp::Players::LocalSlot();
}

void EnsureInit(PlayState* play) {
    if (sL.init) {
        return;
    }
    sL = LocalScreen{};
    sL.init = true;
    sL.hudMode = gSaveContext.hudVisibilityMode;
    sL.nextHudMode = gSaveContext.nextHudVisibilityMode;
    sL.hudTimer = gSaveContext.hudVisibilityModeTimer;
    for (int i = 0; i < kAlphaCount; i++) {
        sL.alphas[i] = *Alpha(&play->interfaceCtx, i);
    }
}

void LocalHudMode(u16 mode) {
    if (mode != sL.hudMode) {
        sL.hudMode = sL.nextHudMode = mode;
        sL.hudTimer = 1;
    }
}

// An event everybody watches with the shared letterbox and HUD: a global cutscene, the group's game over, a scene
// change.
// Another player is reviving with a fairy (finding AB): the game over context is busy with that player alone, and its
// black bars, darkening and black fill are not this screen's.
bool OtherRevives(PlayState* play) {
    int k = Zmp::Players::FairyReviver(play);
    return k >= 0 && k != Local() && Local() >= 0;
}

bool GroupEvent(PlayState* play) {
    return gZmpSim.globalCs || play->csCtx.state != CS_STATE_IDLE ||
           (play->gameOverCtx.state != GAMEOVER_INACTIVE && !OtherRevives(play)) ||
           play->transitionMode != TRANS_MODE_OFF;
}

// Once per tick: the local letterbox moves towards its target (ShrinkWindow_Update at R_UPDATE_RATE 3) and the local
// HUD alphas take one step of the game's own HUD alpha rules (Interface_ZmpStepHudAlphas) with the local player's
// buttons.
void StepLocal(PlayState* play) {
    if (sL.frame == play->gameplayFrames) {
        return;
    }
    sL.frame = play->gameplayFrames;
    // When an event everybody watched ends, the local screen goes back to the normal one, as the shared one does
    // (Camera_ZmpResetInterface): a player that joined during the event took its screen from the middle of it, and its
    // own camera does not ask again while its setting does not change (D-059).
    bool event = GroupEvent(play);
    if (sL.wasEvent && !event && !Zmp_TestMutant("barras_entrar")) {
        sL.lbTarget = 0;
        sL.camAlpha = 0;
        LocalHudMode(HUD_VISIBILITY_ALL);
    }
    sL.wasEvent = event;
    const s32 off = 10;
    if (sL.lbCur < sL.lbTarget) {
        sL.lbCur = std::min(sL.lbCur + off, sL.lbTarget);
    } else if (sL.lbCur > sL.lbTarget) {
        sL.lbCur = std::max(sL.lbCur - off, sL.lbTarget);
    }

    InterfaceContext* ic = &play->interfaceCtx;
    u16 keepAlpha[kAlphaCount];
    for (int i = 0; i < kAlphaCount; i++) {
        keepAlpha[i] = *Alpha(ic, i);
        *Alpha(ic, i) = sL.alphas[i];
    }
    u16 keepMode = gSaveContext.hudVisibilityMode;
    u16 keepNext = gSaveContext.nextHudVisibilityMode;
    u16 keepTimer = gSaveContext.hudVisibilityModeTimer;
    u8 keepButtons[9];
    memcpy(keepButtons, gSaveContext.buttonStatus, sizeof(keepButtons));
    ZmpPlayerBlock b = Zmp::Players::SlotBlock(Local());
    memcpy(gSaveContext.buttonStatus, b.buttonStatus, sizeof(keepButtons));
    gSaveContext.hudVisibilityMode = sL.hudMode;
    gSaveContext.nextHudVisibilityMode = sL.nextHudMode;
    gSaveContext.hudVisibilityModeTimer = sL.hudTimer;

    Interface_ZmpStepHudAlphas(play);

    sL.hudMode = gSaveContext.hudVisibilityMode;
    sL.nextHudMode = gSaveContext.nextHudVisibilityMode;
    sL.hudTimer = gSaveContext.hudVisibilityModeTimer;
    for (int i = 0; i < kAlphaCount; i++) {
        sL.alphas[i] = *Alpha(ic, i);
        *Alpha(ic, i) = keepAlpha[i];
    }
    gSaveContext.hudVisibilityMode = keepMode;
    gSaveContext.nextHudVisibilityMode = keepNext;
    gSaveContext.hudVisibilityModeTimer = keepTimer;
    memcpy(gSaveContext.buttonStatus, keepButtons, sizeof(keepButtons));
}

bool Ready(PlayState* play) {
    return Zmp_MultiActive() && play != nullptr && Zmp::Players::SlotPlayer(Local()) != nullptr;
}

// The shared HUD applies: cutscenes everybody watches, the group's game over, a scene change, and the local player's
// own text box.
bool SharedHud(PlayState* play) {
    return GroupEvent(play) || Zmp::Players::SlotMsgMode(Local()) != MSGMODE_NONE;
}

} // namespace

extern "C" void Zmp_RecordCameraInterface(s32 slot, s16 flags) {
    if (gPlayState == nullptr || slot != Local()) {
        return;
    }
    EnsureInit(gPlayState);
    // Same decoding as Camera_UpdateInterface.
    if ((flags & 0xF000) != 0xF000) {
        s32 val;
        switch (flags & 0x7000) {
            case 0x1000:
                val = 0x1A;
                break;
            case 0x2000:
                val = 0x1B;
                break;
            case 0x3000:
                val = 0x20;
                break;
            default:
                val = 0;
                break;
        }
        if (CVarGetInteger(CVAR_ENHANCEMENT("DisableBlackBars"), 0)) {
            val = 0;
        }
        if (flags & 0x8000) {
            sL.lbCur = val;
        }
        sL.lbTarget = val;
    }
    if ((flags & 0x0F00) != 0x0F00) {
        s16 a = (flags & 0x0F00) >> 8;
        if (a == 0) {
            a = 0x32;
        }
        if (a != sL.camAlpha) {
            sL.camAlpha = a;
            LocalHudMode((u16)a);
        }
    }
}

// D-124 (F4): an actor fills the screen this tick for one player only (a screen effect of what hit that player: Alex's
// rule of D-053); the other screens are drawn without it. Presentation: nothing in the simulation reads it.
extern "C" void Zmp_ScreenFillFor(PlayState* play, s32 slot) {
    if (!Zmp_MultiActive() || Zmp_TestMutant("destello_de_todos")) { // (mutant: every screen, as before)
        return;
    }
    sFillOwner = slot;
    sFillFrame = play->gameplayFrames;
}

extern "C" void Zmp_OnFinishingBlow(void) {
    if (Zmp_MultiActive()) {
        sFlashOwner = gZmpSim.ctx;
    }
}

extern "C" s32 Zmp_QuakeOwner(void) {
    if (!Zmp_MultiActive()) {
        return -1;
    }
    const Actor* a = Zmp::Players::CurrentActor();
    if (a != nullptr && a->id == ACTOR_PLAYER) {
        return Zmp::Players::SlotOf(a);
    }
    return -1;
}

extern "C" s32 Zmp_QuakeShakesCamera(s32 owner, Camera* camera) {
    if (!Zmp_MultiActive() || owner < 0 || camera->thisIdx != CAM_ID_MAIN) {
        return 1;
    }
    return gZmpSim.ctx == owner ? 1 : 0;
}

extern "C" void Zmp_DrawPresentBegin(PlayState* play) {
    sBarsSwapped = false;
    sFlashSwapped = false;
    if (!Ready(play)) {
        return;
    }
    EnsureInit(play);
    StepLocal(play);
    s32 shared = (s32)ShrinkWindow_GetCurrentVal();
    // (finding AB: the bars another player's fairy revive asks for are its screen's)
    bool otherRevives = OtherRevives(play) && !GroupEvent(play);
    s32 want = otherRevives ? sL.lbCur : std::max(shared, sL.lbCur);
    sDrawnBars = want;
    if (want != shared) {
        sBarsKeep = shared;
        ShrinkWindow_SetCurrentVal(want);
        sBarsSwapped = true;
    }
    // The white flash of another player's finishing blow, and the black fill of the game over lights of another
    // player's fairy revive in a room with a fixed camera (z_kankyo.c, Environment_FadeInGameOverLights), are not
    // drawn here.
    // The white flash of a lightning burst that hit another player (Phantom Ganon's energy ball, D-124) neither.
    bool otherFill = sFillOwner >= 0 && sFillFrame == play->gameplayFrames && sFillOwner != Local();
    if (play->envCtx.fillScreen &&
        ((play->actorCtx.freezeFlashTimer > 0 && sFlashOwner >= 0 && sFlashOwner != Local()) || otherRevives ||
         otherFill)) {
        sFlashKeep = play->envCtx.fillScreen;
        play->envCtx.fillScreen = false;
        sFlashSwapped = true;
        if (otherFill) {
            sFillHiddenFrame = play->gameplayFrames;
        }
    }
}

extern "C" void Zmp_DrawPresentEnd(PlayState* play) {
    if (sBarsSwapped) {
        ShrinkWindow_SetCurrentVal(sBarsKeep);
        sBarsSwapped = false;
    }
    if (sFlashSwapped) {
        play->envCtx.fillScreen = sFlashKeep;
        sFlashSwapped = false;
    }
}

namespace {
int sHudHealth = -1;
} // namespace

extern "C" void Zmp_HudBegin(PlayState* play) {
    sHudSwapped = false;
    sHudHealth = gSaveContext.health; // (the health the heart meter of this screen is drawn with: tests)
    if (!Ready(play) || !sL.init || SharedHud(play)) {
        return;
    }
    InterfaceContext* ic = &play->interfaceCtx;
    for (int i = 0; i < kAlphaCount; i++) {
        sHudKeep[i] = *Alpha(ic, i);
        *Alpha(ic, i) = sL.alphas[i];
    }
    sHudSwapped = true;
}

extern "C" void Zmp_HudEnd(PlayState* play) {
    if (!sHudSwapped) {
        return;
    }
    InterfaceContext* ic = &play->interfaceCtx;
    for (int i = 0; i < kAlphaCount; i++) {
        *Alpha(ic, i) = sHudKeep[i];
    }
    sHudSwapped = false;
}

extern "C" s32 Zmp_DoActionIsLocal(void) {
    return (!Zmp_MultiActive() || gZmpSim.ctx == Local()) ? 1 : 0;
}

namespace Zmp::Players {

int HudHealth() {
    return sHudHealth;
}

void PresentReset() {
    sL = LocalScreen{};
    sFlashOwner = -1;
    sFillOwner = -1;
    sFillHiddenFrame = 0xFFFFFFFF;
}

int FillHiddenFrame() {
    return (int)sFillHiddenFrame;
}

int LocalLetterbox() {
    return sL.init ? sL.lbCur : 0;
}

int DrawnLetterbox() {
    return sDrawnBars;
}

int LocalHudMode() {
    return sL.init ? sL.hudMode : -1;
}

} // namespace Zmp::Players
