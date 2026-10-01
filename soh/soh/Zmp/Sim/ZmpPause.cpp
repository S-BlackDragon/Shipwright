// ZMP: local pause menu (PLAN.md 2.7, phase 3).
//
// In the multiplayer simulation nobody pauses the world. Each client opens its own pause menu (Kaleido) on top of
// the running game; the menu is presentation, not simulation:
//   - The simulation's PauseContext stays closed on every machine. The local menu has its own PauseContext, its
//     own copy of the game registers (gGameInfo: R_PAUSE_MENU_MODE, R_UPDATE_RATE, the menu's WREG/XREG...) and of
//     the InterfaceContext, swapped in only while Kaleido runs.
//   - Kaleido runs in the context of the local player (its block of the save context is live) with the local pad
//     (not the lockstep input). Everything it writes to the save context, the interface, the registers, the
//     segment table and the local Player actor is restored afterwards, so the simulation never sees it.
//   - What the menu changes on purpose (C buttons, B button, worn equipment) becomes an EQUIP event that travels
//     through the lockstep and is applied to this player at the same tick everywhere. The menu shows it at once
//     (pending overlay) until the simulation applies it.
//   - While the menu is open this client's input to the group is neutral: its Link stands still.
// The game over menu is still the simulation's (everybody is down, the world is stopped anyway).

#include "ZmpPlayers.h"

#include <cstdio>
#include <cstring>
#include <string>

#include "soh/Zmp/ZmpLog.h"
#include "soh/Zmp/Net/Lockstep.h"

extern "C" {
#include "variables.h"
#include "functions.h"
#include "macros.h"
#include "regs.h"
extern PlayState* gPlayState;
}

namespace {

bool sActive = false;    // local menu state initialised for the current Play
bool sRunning = false;   // Kaleido is running for the local menu right now
PauseContext sPause;     // local menu
GameInfo sRegs;          // local copy of the game registers while the menu is open
InterfaceContext sIface; // local copy of the interface while the menu is open
bool sHaveMenuCopies = false;
KaleidoMgrOverlay* sOvl = nullptr; // Kaleido overlay "loaded" for the local menu
Input sMenuInput;                  // local pad, press / release computed per tick
OSContPad sLocalPad;
bool sPending = false; // EQUIP sent, not applied by the simulation yet
ItemEquips sPendingEquips;
int sPendingTicks = 0;
alignas(64) u8 sScratch[0x40000]; // the menu's Link preview and icon buffers (the original used object memory)

// Simulation snapshot taken before Kaleido runs for the local menu.
struct Snapshot {
    SaveContext save;
    InterfaceContext iface;
    PauseContext pause;
    GameInfo regs;
    uintptr_t segments[NUM_SEGMENTS];
    Input input0;
    View view;
    KaleidoMgrOverlay* curOvl;
    Player player;
};
Snapshot sSnap;

bool SameEquips(const ItemEquips& a, const ItemEquips& b) {
    return memcmp(a.buttonItems, b.buttonItems, sizeof(a.buttonItems)) == 0 &&
           memcmp(a.cButtonSlots, b.cButtonSlots, sizeof(a.cButtonSlots)) == 0 && a.equipment == b.equipment;
}

void StepMenuInput() {
    Input* in = &sMenuInput;
    in->prev = in->cur;
    in->cur = sLocalPad;
    in->cur.err_no = 0;
    u32 diff = in->prev.button ^ in->cur.button;
    in->press = OSContPad{};
    in->rel = OSContPad{};
    in->press.button = (u16)(diff & in->cur.button);
    in->rel.button = (u16)(diff & in->prev.button);
    PadUtils_UpdateRelXY(in);
    in->press.stick_x = (s8)(in->cur.stick_x - in->prev.stick_x);
    in->press.stick_y = (s8)(in->cur.stick_y - in->prev.stick_y);
}

Player* LocalPlayer() {
    return Zmp::Players::SlotPlayer(Zmp::Players::LocalSlot());
}

// Runs fn with the local menu swapped in; returns the equipment the menu left (for EQUIP events).
void RunLocal(PlayState* play, void (*fn)(PlayState*), ItemEquips* before, ItemEquips* after) {
    Player* player = LocalPlayer();
    Zmp_SetContext(play, Zmp::Players::LocalSlot());
    memcpy(&sSnap.save, &gSaveContext, sizeof(SaveContext));
    memcpy(&sSnap.iface, &play->interfaceCtx, sizeof(InterfaceContext));
    memcpy(&sSnap.pause, &play->pauseCtx, sizeof(PauseContext));
    memcpy(&sSnap.regs, gGameInfo, sizeof(GameInfo));
    memcpy(sSnap.segments, gSegments, sizeof(sSnap.segments));
    sSnap.input0 = play->state.input[0];
    sSnap.view = play->view;
    sSnap.curOvl = gKaleidoMgrCurOvl;
    memcpy(&sSnap.player, player, sizeof(Player));

    memcpy(&play->pauseCtx, &sPause, sizeof(PauseContext));
    if (sHaveMenuCopies) {
        memcpy(gGameInfo, &sRegs, sizeof(GameInfo));
        memcpy(&play->interfaceCtx, &sIface, sizeof(InterfaceContext));
        gKaleidoMgrCurOvl = sOvl;
    }
    if (sPending) {
        gSaveContext.equips = sPendingEquips;
    }
    play->state.input[0] = sMenuInput;
    *before = gSaveContext.equips;

    sRunning = true;
    fn(play);
    sRunning = false;

    *after = gSaveContext.equips;
    memcpy(&sPause, &play->pauseCtx, sizeof(PauseContext));
    memcpy(&sRegs, gGameInfo, sizeof(GameInfo));
    memcpy(&sIface, &play->interfaceCtx, sizeof(InterfaceContext));
    sOvl = gKaleidoMgrCurOvl;
    sHaveMenuCopies = true;

    memcpy(&gSaveContext, &sSnap.save, sizeof(SaveContext));
    memcpy(&play->interfaceCtx, &sSnap.iface, sizeof(InterfaceContext));
    memcpy(&play->pauseCtx, &sSnap.pause, sizeof(PauseContext));
    memcpy(gGameInfo, &sSnap.regs, sizeof(GameInfo));
    memcpy(gSegments, sSnap.segments, sizeof(sSnap.segments));
    play->state.input[0] = sSnap.input0;
    play->view = sSnap.view;
    gKaleidoMgrCurOvl = sSnap.curOvl;
    memcpy(player, &sSnap.player, sizeof(Player));
    Zmp_RestoreAnchor(play);
}

void SendEquip(const ItemEquips& e) {
    std::string cmd = "zmp_equip";
    for (int i = 0; i < 8; i++) {
        cmd += " " + std::to_string(e.buttonItems[i]);
    }
    for (int i = 0; i < 7; i++) {
        cmd += " " + std::to_string(e.cButtonSlots[i]);
    }
    cmd += " " + std::to_string(e.equipment);
    Zmp::Lockstep::SendConsoleEvent(cmd);
    sPending = true;
    sPendingEquips = e;
    sPendingTicks = 0;
    Zmp::Log("zmp: pause menu: " + cmd);
}

void ForceClose() {
    if (sPause.state != 0 || sPause.debugState != 0) {
        Zmp::Log("zmp: pause menu closed (the game needs the player)");
    }
    sPause.state = 0;
    sPause.debugState = 0;
    sHaveMenuCopies = false;
    Zmp::Lockstep::SetLocalInputBlocked(false);
}

bool MenuOpen() {
    return sActive && (sPause.state != 0 || sPause.debugState != 0);
}

} // namespace

namespace Zmp::Pause {

void OnLocalPad(const OSContPad& pad) {
    sLocalPad = pad;
}

void Reset() {
    sActive = false;
    sHaveMenuCopies = false;
    sPending = false;
    sRunning = false;
    memset(&sMenuInput, 0, sizeof(sMenuInput));
    Zmp::Lockstep::SetLocalInputBlocked(false);
}

bool IsOpen() {
    return MenuOpen();
}

int State() {
    return sActive ? sPause.state : 0;
}

} // namespace Zmp::Pause

extern "C" s32 Zmp_PauseRunningLocal(void) {
    return sRunning ? 1 : 0;
}

extern "C" void* Zmp_PauseScratch(void) {
    return sRunning ? (void*)sScratch : nullptr;
}

// Play_Update, once per tick after the simulation's menu / game over / text box update.
extern "C" void Zmp_PauseLocalUpdate(PlayState* play) {
    if (!Zmp_PauseIsLocal()) {
        return;
    }
    int local = Zmp::Players::LocalSlot();
    Player* player = LocalPlayer();
    if (!sActive) {
        memcpy(&sPause, &play->pauseCtx, sizeof(PauseContext)); // closed and initialised by Play_Init
        sPause.state = 0;
        sPause.debugState = 0;
        sActive = true;
        sHaveMenuCopies = false;
    }
    StepMenuInput();

    // The simulation applied the EQUIP event (or it was lost): stop showing the pending one.
    if (sPending) {
        ZmpPlayerBlock b = Zmp::Players::SlotBlock(local);
        if (SameEquips(b.equips, sPendingEquips) || ++sPendingTicks > 100) {
            sPending = false;
        }
    }

    bool canPause = player != nullptr && !Zmp::Players::SlotDowned(local) && gSaveContext.gameMode == GAMEMODE_NORMAL &&
                    play->pauseCtx.state == 0 && play->pauseCtx.debugState == 0 &&
                    play->gameOverCtx.state == GAMEOVER_INACTIVE && play->transitionTrigger == TRANS_TRIGGER_OFF &&
                    play->transitionMode == TRANS_MODE_OFF && Zmp::Players::SlotMsgMode(local) == MSGMODE_NONE;
    if (!canPause) {
        if (MenuOpen()) {
            ForceClose();
        }
        return;
    }

    ItemEquips before;
    ItemEquips after;
    if (!MenuOpen()) {
        // START opens it (KaleidoSetup_Update decides, with the local player's state and pad).
        RunLocal(play, KaleidoSetup_Update, &before, &after);
        if (MenuOpen()) {
            Zmp::Lockstep::SetLocalInputBlocked(true);
            Zmp::Log("zmp: pause menu opened (local)");
        }
        return;
    }
    // The menu waits for the pause background capture (R_PAUSE_MENU_MODE 1 -> 2 -> 3 in Play_Draw); the world is
    // drawn live instead.
    {
        GameInfo* real = gGameInfo;
        gGameInfo = &sRegs;
        if (R_PAUSE_MENU_MODE == 1 || R_PAUSE_MENU_MODE == 2) {
            R_PAUSE_MENU_MODE = 3;
        }
        gGameInfo = real;
    }
    RunLocal(play, KaleidoScopeCall_Update, &before, &after);
    if (!SameEquips(before, after)) {
        SendEquip(after);
    }
    if (!MenuOpen()) {
        sHaveMenuCopies = false;
        Zmp::Lockstep::SetLocalInputBlocked(false);
        Zmp::Log("zmp: pause menu closed (local)");
    }
}

// Play_DrawOverlayElements: draws the local menu (Kaleido also changes equipment while drawing).
extern "C" void Zmp_PauseLocalDraw(PlayState* play) {
    if (!Zmp_PauseIsLocal() || !MenuOpen() || LocalPlayer() == nullptr) {
        return;
    }
    ItemEquips before;
    ItemEquips after;
    RunLocal(play, KaleidoScopeCall_Draw, &before, &after);
    if (!SameEquips(before, after)) {
        SendEquip(after);
    }
}
