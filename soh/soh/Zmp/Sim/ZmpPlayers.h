#pragma once

// ZMP: several real Player actors in one lockstep simulation (PLAN.md 2.3, phase 2).
//
// Every player k has a slot: its Player actor, its main camera, its Z-targeting context, its health and
// its input. The engine code keeps reading "the" player (GET_PLAYER), "the" main camera
// (play->mainCamera), "the" target context, gSaveContext.health and play->state.input[0]; ZMP swaps the
// slot of the *context player* into those places while code runs on behalf of that player (its own
// update, the update of an actor near it, its camera, the text box or menu it opened). Between two
// contexts the data of the previous one is parked in its slot. Outside any specific context the
// context is the *anchor* slot (the lowest present slot), whose camera is also the canonical camera of
// the simulation (the one Play_Draw uses for its simulation side effects, docs/DECISIONES.md D-025).
//
// Everything here is simulation state (identical on every machine) except the local slot and the
// render-only view helpers. gZmpSim is plain data: it travels in the portable save state.

#ifdef __cplusplus
extern "C" {
#endif
#include "z64.h"
#ifdef __cplusplus
}
#endif

#define ZMP_MAX_PLAYERS 6

// Per-player part of the save context (PLAN.md 2.6: progression is shared, this is not). While a slot is the
// context these values are live in gSaveContext; otherwise they are parked here.
typedef struct {
    s16 health;
    s16 healthAccumulator;
    s8 magic;
    u8 pad0;
    s16 magicState;
    s16 prevMagicState;
    s16 magicFillTarget;
    s16 magicTarget;
    s16 nayrusLoveTimer;
    ItemEquips equips; // B, C and D-pad buttons, worn sword / shield / tunic / boots
    s8 ammo[16];
    u8 bottles[4]; // contents of SLOT_BOTTLE_1..4 (having a bottle is shared: a new one appears for everybody)
    u8 buttonStatus[9];
    u8 pad1[3];
} ZmpPlayerBlock;

// Downed state (PLAN.md 2.8).
#define ZMP_REVIVE_TICKS 60 // a partner holds A this long (3 s)
#define ZMP_REVIVE_RANGE 100.0f

typedef struct {
    /* in the simulation */
    u8 active;          // slot belongs to the simulation (joined and not left)
    u8 present;         // its Player actor exists in the current scene
    u8 downed;          // health reached zero without a fairy: lying on the ground until a partner revives it
    s8 spectate;        // slot whose player the camera of a downed player follows (-1 none)
    s16 reviveProgress; // ticks of A held by a partner in range
    s8 reviver;         // slot reviving it (-1 none)
    u8 pad0;
    ZmpPlayerBlock block;
    Player* player;       // heap address (the heap lives at a fixed address, D-016)
    Camera camera;        // parked main camera (live in play->mainCamera while in context)
    TargetContext target; // parked Z-targeting context
    Input input;          // this tick's input of the player (press/rel computed in the simulation)
    /* render helper, not hashed: view computed by this slot's camera in the last tick */
    u8 hasView;
    u8 pad1[7];
    View view;
} ZmpPlayerSlot;

typedef struct {
    u32 magic;
    u8 enabled;      // multiplayer lockstep simulation running
    u8 inPlay;       // a Play game state exists (between Play_Init and Play_Destroy)
    s8 anchor;       // lowest present slot
    s8 ctx;          // current context slot (-1 none)
    s8 msgOwner;     // slot that opened the current text box / ocarina
    s8 pauseOwner;   // slot that opened the pause menu
    s8 spawningSlot; // Player_Init of this slot is running
    u8 transitionArmed;
    s16 transitionCountdown; // ticks left before a group scene change
    s16 transitionEntrance;
    u8 groupDefeat; // every player was downed: the original game over runs for the group
    u8 pad2;
    s16 lastMagicCapacity; // shared magic meter size seen last tick (a new meter fills everybody's)
    u32 audioTaskCount;    // deterministic replacement of gAudioContext.totalTaskCnt in a ZMP session
    u32 audioRandom;       // deterministic replacement of Audio_NextRandom in a ZMP session
    u32 metronomeAt;       // audioTaskCount when the metronome sound was last requested
    ZmpPlayerSlot slots[ZMP_MAX_PLAYERS];
} ZmpSimState;

#ifdef __cplusplus
extern "C" {
#endif

extern ZmpSimState gZmpSim;
extern void* gZmpCtxPlayer;
// Camera_UpdateInterface does nothing while set (main cameras of the non-anchor players).
extern s32 gZmpCameraInterfaceMuted;

// True while the multiplayer simulation runs in a Play state.
s32 Zmp_MultiActive(void);

// Play_Init / Play_Destroy
void Zmp_PlayInitBegin(PlayState* play);
void Zmp_PlayInitPlayers(PlayState* play, s32 startBgCamIndex);
void Zmp_PlayDestroy(PlayState* play);
// Player_Init / Player_Destroy
void Zmp_OnPlayerInit(Player* player, PlayState* play);
void Zmp_OnPlayerDestroy(Player* player);

// Context switching.
void Zmp_SetContext(PlayState* play, s32 slot);
void Zmp_RestoreAnchor(PlayState* play);
// Actor_UpdateAll: switches to the context of `actor` (its own slot for a Player, the slot of its
// parent Player, else the nearest present player) and returns that player.
Player* Zmp_ContextForActor(PlayState* play, Actor* actor);
// Actor_UpdateAll tail: lock-on and Z-target context of every player.
void Zmp_UpdateAttentionAll(PlayState* play);
// Play_Update camera loop: updates the main camera of every player (anchor last).
void Zmp_UpdateMainCameras(PlayState* play);
// Play_Update: any player can open the pause menu; it becomes its owner.
void Zmp_KaleidoSetupAll(PlayState* play);
// Context of the owner of the text box (which = 0) or of the pause / game over menu (which = 1).
void Zmp_EnterOwner(PlayState* play, s32 which);
void Zmp_OnMessageStart(void);
// Play_Update after Interface_Update: health refills and magic meter of the players out of context, downed
// players, revive, spectator targets, group defeat.
void Zmp_UpdateHealthAccumulators(PlayState* play);
// Player death (z_player.c): returns 1 when the dying player becomes downed instead of starting the game over
// (multiplayer, no fairy, somebody else still standing).
s32 Zmp_OnPlayerDeath(PlayState* play, Player* player);
// True for a downed player (enemies ignore it, it is not "the nearest player").
s32 Zmp_IsDowned(Player* player);
// OnePointCutscene on death / fairy revive: 0 in multiplayer (a personal camera must not take everybody's view).
s32 Zmp_AllowDeathCamera(void);
// Pause menu: the multiplayer simulation never opens it (it is local to each client, ZmpPause.cpp).
s32 Zmp_PauseIsLocal(void);
// Play_Update (once per tick) and Play_DrawOverlayElements: the local pause menu.
void Zmp_PauseLocalUpdate(PlayState* play);
void Zmp_PauseLocalDraw(PlayState* play);
// Kaleido: true while it runs for the local menu (it must not touch simulation memory: object space, collision).
s32 Zmp_PauseRunningLocal(void);
// Player_DrawImpl: per-player tunic tone (cosmetic). Returns 1 and fills *out for the players after the first.
s32 Zmp_TunicColor(s32 tunic, const Color_RGB8* base, Color_RGB8* out);
// Kaleido: buffer for the menu's Link preview while it runs for the local menu (NULL otherwise).
void* Zmp_PauseScratch(void);
// Play_Update: group scene change with a countdown. Returns 1 while the transition must wait.
s32 Zmp_TransitionGate(PlayState* play);

// Play_Draw: the simulation side effects of drawing use the canonical (anchor) camera; the display
// list is built with the local player's camera.
void Zmp_DrawBeginView(PlayState* play);
MtxF* Zmp_SimViewProjection(PlayState* play);
s32 Zmp_DrawAllActors(void);
void Zmp_DrawActorContext(PlayState* play, Actor* actor);
void Zmp_DrawEndView(PlayState* play);
void Zmp_OverlayBegin(PlayState* play);
void Zmp_OverlayEnd(PlayState* play);

// GameState_ReqPadData: the anchor's input goes to input[0] at the start of the tick.
void Zmp_OnInputCopied(GameState* gameState);
// AudioOcarina_ReadControllerInput: input of the ocarina owner. Returns 0 outside the simulation.
s32 Zmp_OcarinaInput(Input* out);

// Audio values read by game logic, made deterministic in a ZMP session (docs/DETERMINISMO_AUDITORIA.md 5).
s32 Zmp_AudioSession(void);
void Zmp_AudioTick(void);
u32 Zmp_AudioTaskCount(u32 real);
u32 Zmp_AudioRandom(void);
void Zmp_AudioSfxPlayed(u16 sfxId);
// Returns 1 and sets *playing when the answer comes from the simulation (lockstep, metronome).
s32 Zmp_AudioSfxPlaying(u32 sfxId, s32* playing);
// GameState_Update: gSaveContext.language comes from the simulation, not the local setting.
s32 Zmp_LanguageLocked(void);
// Save files are written only by the group leader (a joiner never overwrites its own slot with the
// shared game).
s32 Zmp_AllowSaveWrite(void);

#ifdef __cplusplus
}

namespace Zmp::Players {
// Local slot (presentation only; not simulation state).
void SetLocalSlot(int slot);
int LocalSlot();
// Founding: the current game becomes the simulation with the current Player in `slot`.
bool Found(int slot);
// Leaves the multiplayer simulation (session end).
void Reset();
// Lockstep events (applied at the start of their tick, before the update).
void Spawn(int slot);
void Despawn(int slot);
// Per-player input of this tick (pad of the bundle).
void StepInput(int slot, const OSContPad& pad);
Player* SlotPlayer(int slot);
int SlotOf(const Actor* actor);
int SlotHealth(int slot);
// Per-player block of a slot (live values when it is the context).
ZmpPlayerBlock SlotBlock(int slot);
bool SlotDowned(int slot);
bool GroupDefeat();
int SlotSpectate(int slot);
int SlotReviveProgress(int slot);
int SlotReviver(int slot);
// EQUIP event (from the pause menu of `slot`): absolute buttons and worn equipment.
void ApplyEquip(int slot, const ItemEquips& equips);
// Runs `fn` in the context of `slot` (console events run for the player who sent them).
void RunInContext(int slot, void (*fn)(void*), void* arg);
int PresentCount();
// After a portable save state load.
void AfterStateLoad();
} // namespace Zmp::Players

namespace Zmp::Pause {
// Local pad of this tick (before the lockstep replaces it).
void OnLocalPad(const OSContPad& pad);
// New Play / session end.
void Reset();
bool IsOpen();
int State();
} // namespace Zmp::Pause
#endif
