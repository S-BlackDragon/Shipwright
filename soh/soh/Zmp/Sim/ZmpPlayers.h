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
#define ZMP_CAM_GLOBAL (-1)
#define ZMP_MAX_EXTRA_ROOMS 6 // rooms kept loaded besides the engine's current and previous room (phase 4)
#define ZMP_MAX_QUEUED_ROOMS 4

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
    s16 activeCam;        // this player's active camera (play->activeCamera while it is the context, phase 4)
    s16 room;             // room this player is in (phase 4, several rooms loaded)
    // Heat / deep water timer of this player (phase 4; the original's timer in gSaveContext is the anchor's).
    s16 heatState;   // 0 off, 1 preview, 2 counting, 3 stopped (ran out)
    s16 heatSeconds; // seconds left
    s16 heatTicks;   // ticks to the next second
    s16 heatPreview; // ticks before the count starts (the original's preview and move animation)
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
    // Phase 4 (PLAN.md 2.9): scope of each sub camera. ZMP_CAM_GLOBAL: every player sees it; k: only player k.
    s8 camScope[4];   // indexed by camera id (0, the main camera, unused)
    s8 camCreator[4]; // slot whose context created the camera: its update runs in that context
    u8 globalCs;      // a cutscene everybody watches is running (scripted cutscene or global sub camera)
    s8 csTrigger;     // player the running global cutscene is about (the others are frozen around it)
    s8 csStarter;     // context that last started a scripted cutscene
    u8 skipRequested; // the host pressed START during a scripted cutscene (the skip happens at its next command)
    // Phase 4 (PLAN.md 2.10): several rooms loaded. The engine keeps two (curRoom, prevRoom); the others that some
    // player still stands in are kept here. While code runs for player k, curRoom is k's room.
    u8 extraRoomCount;
    s8 loadingRoom; // room whose load was requested and did not complete yet
    s8 setupRoom;   // room whose actor list the next Actor_UpdateAll spawns
    u8 queuedRoomCount;
    s8 queuedRooms[ZMP_MAX_QUEUED_ROOMS]; // rooms requested while another one was loading
    Room extraRooms[ZMP_MAX_EXTRA_ROOMS];
    ZmpPlayerSlot slots[ZMP_MAX_PLAYERS];
} ZmpSimState;

#ifdef __cplusplus
extern "C" {
#endif

extern ZmpSimState gZmpSim;
extern void* gZmpCtxPlayer;
// Camera_UpdateInterface records the flags for player (value - 1) instead of changing the shared letterbox and HUD
// while set (every player's own cameras, phase 4).
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
// Interface_Update: the original heat / deep water timer is off in multiplayer (each player has its own, Players.cpp).
s32 Zmp_OwnHazardTimers(void);
// Low health alarm (a sound, presentation): the local player's health, not the anchor's.
s32 Zmp_LocalHealthCritical(void);
// Player_DrawImpl: per-player tunic tone (cosmetic). Returns 1 and fills *out for the players after the first.
s32 Zmp_TunicColor(s32 tunic, const Color_RGB8* base, Color_RGB8* out);
// Kaleido: buffer for the menu's Link preview while it runs for the local menu (NULL otherwise).
void* Zmp_PauseScratch(void);
// Play_Update: group scene change with a countdown. Returns 1 while the transition must wait.
s32 Zmp_TransitionGate(PlayState* play);

// Cameras (phase 4, PLAN.md 2.9, Cameras.cpp). Each player has its own active camera; a sub camera is GLOBAL (every
// player's active camera while it runs: boss and scripted cutscenes) or belongs to one player (one-point cutscenes:
// chests, item get, crawlspaces...).
void Zmp_OnSubCameraCreated(PlayState* play, s16 camId);
void Zmp_OnePointBegin(PlayState* play, s16 csId, Actor* actor);
void Zmp_OnePointEnd(void);
// End of OnePointCutscene_Init (every path after Zmp_OnePointBegin).
void Zmp_OnePointInitDone(PlayState* play);
// OnePointCutscene_Init, after it copied play->view into the new camera: a private camera starts from its owner's view.
void Zmp_OnePointCameraStart(PlayState* play, Camera* subCam);
// OnePointCutscene_Init: 0 when the HUD must stay (a private cutscene of a player other than the anchor).
s32 Zmp_OnePointHidesHud(void);
// Play_ChangeCameraStatus replacement in multiplayer.
s16 Zmp_ChangeCameraStatus(PlayState* play, s16 camIdx, s16 status);
void Zmp_OnCameraCleared(PlayState* play, s16 camIdx);
void Zmp_OnAllSubCamerasCleared(PlayState* play);
// Play_Update camera loop replacement in multiplayer.
void Zmp_UpdateCameras(PlayState* play);
// Play_Draw tail: Camera_Finish of every player's active sub camera, in its context.
void Zmp_FinishCameras(PlayState* play);
// Cutscene_SetSegment / manual cutscene start: remembers who started a scripted cutscene.
void Zmp_OnCutsceneStart(void);
// Player_UpdateCommon: whether this player follows the scripted cutscene's Link cues (only the one it is about).
s32 Zmp_FollowsCutsceneScript(Player* player);
// Player_Update: a player a global cutscene is not about stands still (its input still reaches text and skip).
s32 Zmp_PlayerHeldByCutscene(Player* player);
// Cutscene_Command_Terminator: the host (anchor) skips a scripted cutscene with START in multiplayer.
s32 Zmp_HostSkipsCutscene(PlayState* play);

// Text boxes and item notices (phase 4): the text box of another player (not a global cutscene's) is drawn (its
// simulation side effects happen on every machine) but its picture is discarded; the others see a short notice.
void Zmp_MessageDrawBegin(PlayState* play);
void Zmp_MessageDrawEnd(PlayState* play);
void Zmp_OnItemGet(Player* player, s32 itemId);

// Rooms (phase 4, Rooms.cpp).
// OTRRoom_RequestNewRoom: returns -1 to let the original load the room, otherwise its return value.
s32 Zmp_RoomRequest(PlayState* play, RoomContext* roomCtx, s32 roomNum);
void Zmp_RoomLoadBegin(PlayState* play, RoomContext* roomCtx);
void Zmp_RoomLoadEnd(PlayState* play, RoomContext* roomCtx);
// Room_FinishRoomChange: 1 when handled (multiplayer: unloads only the rooms nobody stands in).
s32 Zmp_RoomFinish(PlayState* play, RoomContext* roomCtx);
// Kept loaded besides curRoom / prevRoom.
s32 Zmp_RoomKept(s32 room);
// Actor_Spawn: room of a new actor (its spawner's room in multiplayer).
s8 Zmp_SpawnRoom(PlayState* play);
// Actor_UpdateAll: spawning the actor list of a room that just loaded.
void Zmp_SetupSpawnBegin(PlayState* play);
void Zmp_SetupSpawnEnd(PlayState* play);
// Room ambience (phase 4, PLAN.md 2.10 point 5; presentation). In the simulation only the anchor's floor picks the
// light setting (deterministic, no flicker between players); each client draws with its local player's own light
// setting, reverb and combat music.
s32 Zmp_DrivesLighting(Player* player);
s32 Zmp_IsLocalAudioPlayer(Player* player);
void Zmp_DrawLightBegin(PlayState* play);
void Zmp_DrawLightEnd(PlayState* play);
// The simulation's fog distance (the canonical view's far plane) while the local lighting is applied.
f32 Zmp_SimFogFar(PlayState* play);
// Presentation (phase 4, Present.cpp): each player's letterbox and HUD come from its own cameras and are drawn only on
// its own screen; the shared ones only from cutscenes everybody watches. Another player's text box does not change
// this player's HUD. The white flash of a finishing blow is shown to the player who struck.
void Zmp_RecordCameraInterface(s32 slot, s16 flags);
void Zmp_OnFinishingBlow(void);
// Quakes: the player whose update requested it (-1: the world). Whether it shakes the camera being updated.
s32 Zmp_QuakeOwner(void);
// Interface_SetDoAction: the A button label is the local player's (called in its context).
s32 Zmp_DoActionIsLocal(void);
s32 Zmp_QuakeShakesCamera(s32 owner, Camera* camera);
// Play_Draw: a camera updated outside the camera loop drives its owner's letterbox and HUD.
void Zmp_CameraInterfaceOwnerBegin(Camera* camera);
void Zmp_CameraInterfaceOwnerEnd(void);
// z_camera.c: forget the cached camera interface values (start / end of a global cutscene).
void Camera_ZmpResetInterface(s32 end);
void Zmp_DrawPresentBegin(PlayState* play);
void Zmp_DrawPresentEnd(PlayState* play);
void Zmp_HudBegin(PlayState* play);
void Zmp_HudEnd(PlayState* play);
// Play_Draw: the rooms kept loaded besides curRoom and prevRoom.
void Zmp_DrawExtraRooms(PlayState* play, u32 flags);
// Actor_RemoveFromCategory: 1 when handled (the last enemy of a loaded room clears that room).
s32 Zmp_EnemyRemoved(PlayState* play, Actor* actor);
// Scene_CommandObjectList: 1 when the objects of every loaded room must be kept (a room loads while another one
// stays loaded for a player).
s32 Zmp_KeepObjects(void);

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
// Phase 4 (D-058): "continue? no" of the group game over. In a multiplayer session it notes that the group ends and
// returns 1 (the menu then goes to the file select instead of the title screen); 0 outside a session.
s32 Zmp_GameOverQuit(void);
// A save that this machine skipped because it is not the leader (the notice "Solo el anfitrion guarda la partida").
void Zmp_NoteSaveSkipped(void);

#ifdef __cplusplus
}

#include <string>
#include <vector>

namespace Zmp::Players {
// Debug RNG trace of the tick (desync hunting).
void RandTraceBegin(bool enabled);
std::string RandTraceText();
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
// Heat / deep water timer of a player: seconds left, or -1 when it does not run.
int SlotHeatSeconds(int slot);
bool GroupDefeat();
int SlotSpectate(int slot);
int SlotReviveProgress(int slot);
int SlotReviver(int slot);
// EQUIP event (from the pause menu of `slot`): absolute buttons and worn equipment.
void ApplyEquip(int slot, const ItemEquips& equips);
// Saved per-player block (loaded game): equipment, buttons, ammo, bottles; full health (PLAN.md 2.6).
void ApplySavedBlock(int slot, const ZmpPlayerBlock& block);
// Runs `fn` in the context of `slot` (console events run for the player who sent them).
void RunInContext(int slot, void (*fn)(void*), void* arg);
int PresentCount();
int SlotActiveCam(int slot);
int SlotRoom(int slot);
void SetSlotRoom(int slot, int room);
// Rooms.cpp: curRoom becomes this player's room when it is loaded (context switch).
void ArrangeRooms(PlayState* play, int slot);
std::vector<int> LoadedRooms(PlayState* play);
// Local ambience (presentation): light setting the local picture uses (-1: the simulation's).
int LocalLightSetting();
// Present.cpp: local screen state (presentation).
void PresentReset();
int LocalLetterbox();
int LocalHudMode();
// Cameras.cpp helpers: context switch (present slots only) and the local player's picture (NULL: canonical view).
void SwitchContext(PlayState* play, int slot);
// Actor whose update is running (NULL outside actor updates).
const Actor* CurrentActor();
bool IsPresent(int slot);
// Slot whose Player is "the player" of this actor in its update (read only, same rule as the update).
int ActorTargetSlot(const Actor* actor);
const View* LocalPicture(PlayState* play, int slot);
int CamScope(int camId);
bool GlobalCutscene();
int CutsceneTrigger();
// Item notices of the other players (presentation): slot, item, age in seconds.
struct Notice {
    int slot;
    int itemId;
    double age;
};
std::vector<Notice> RecentNotices(double maxAge);
bool LocalTextHidden();
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
