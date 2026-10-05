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
#define ZMP_SHARED_MAX 4096 // compact shared game (phase 5, SharedGame.cpp)

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
// Warp song invitation (phase 5b): how long it lasts, when the player who played leaves, how long L must be held.
#define ZMP_INVITE_TICKS 200
#define ZMP_INVITE_LEAVE_TICKS 60
#define ZMP_INVITE_HOLD_TICKS 15
#define ZMP_MSG_STATICS_SIZE 96
#define ZMP_OCA_STATICS_SIZE 512

typedef struct {
    /* in the simulation */
    u8 active;          // slot belongs to the simulation (joined and not left)
    u8 present;         // its Player actor exists in the current scene
    u8 downed;          // health reached zero without a fairy: lying on the ground until a partner revives it
    s8 spectate;        // slot whose player the camera of a downed player follows (-1 none)
    s16 reviveProgress; // ticks of A held by a partner in range
    s8 reviver;         // slot reviving it (-1 none)
    u8 warpPending;     // phase 5b: leaves through the warp song of the current invitation at the next tick
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
    s16 inviteHold;  // phase 5b: ticks this player has held L during a warp song invitation
    // Phase 5b, age: the buttons and worn equipment this player had at the other age (what the original keeps in
    // childEquips / adultEquips for its one Link). Not valid: the game's defaults for that age.
    u8 otherValid;
    // Phase 5b: where this player stood when its scene was loaded again in place (a change of age, progress made
    // elsewhere that changes the scene); it is put back there once the scene is up.
    u8 restoreValid;
    ItemEquips otherEquips;
    s16 restoreRoom;
    s16 restoreYaw;
    // Phase 5b: the room on the other side of the open passage this player stands at (kept loaded: it is seen
    // through the passage); -1 none.
    s16 room2;
    s16 room2Fresh; // ticks left before room2 is dropped if the passage does not ask for it again
    // Finding Y: the light setting of the water this player's camera is under (valid while its main camera has the
    // "eye under water" flag). Each player's own picture is drawn with it; the scene's lights are not touched.
    s16 waterLight;
    s16 pad8;
    Vec3f restorePos;
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
    s8 msgOwner;     // slot that last started the ocarina (one instrument for everybody; text boxes are per player)
    s8 pauseOwner;   // slot that opened the pause menu
    s8 spawningSlot; // Player_Init of this slot is running
    s8 transitionBy; // phase 5: player whose own action started the pending scene change (-1: none or the world)
    u8 undoValid;    // the undo values below were taken
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
    // Phase 5 (PLAN.md 2.11): a player who walks out of the scene alone leaves the group. What its exit changed in the
    // save context is put back on the machines that stay (values of the last tick without a pending scene change).
    s32 undoRespawnFlag;
    RespawnData undoRespawn[RESPAWN_MODE_MAX];
    f32 undoEntranceSpeed;
    u8 undoNextTransitionType;
    u8 undoRetainWeather;
    u8 undoSeqId;
    u8 undoNatureId;
    u8 undoHaltAll;
    u8 pad4[3];
    // World clock: the server's time adopted at the start of each tick; a time the group set itself (a cutscene) is
    // kept until the server's clock reports it back.
    u16 dayAtTickStart;
    u16 clockHoldValue;
    s16 clockHoldTicks;
    u8 clockHold;
    // Tests (lockstep event "zmp_lock_exits 1"): scene exits act as walls, so random play never takes anybody out.
    u8 exitsLocked;
    // Phase 5b: the player floating in the blue warp (-1 none): the warp acts on it, and only it leaves the scene.
    s8 warpOwner;
    // The pending scene change of one player goes on with its scripted cutscene (blue warp): that player alone.
    u8 transitionSolo;
    u16 undoNextCutsceneIndex;
    // Phase 5b, warp songs: only the player who played travels. For a few seconds the others of its scene are invited
    // ("X va a ...: manten L para ir tambien"): whoever holds L travels too. Decided by lockstep input, never by clock.
    s8 inviteBy;        // slot that played the song (-1: no invitation)
    s8 inviteSong;      // song index (respawn data of the warp)
    s16 inviteEntrance; // warp pad entrance
    s16 inviteTicks;    // ticks the invitation still lasts
    s16 inviteLeaveIn;  // ticks until the player who played leaves
    // Phase 5b, one text box per player: play->msgCtx (and the statics of the text box code) hold the ones of the
    // slot msgLoaded; the others wait here and are swapped in with the context. The glyphs (Font) only travel while
    // that slot's text box is open. Entry ZMP_MAX_PLAYERS is the idle state a new player starts from.
    s8 msgLoaded;
    u8 msgReady;
    // Phase 5b, age: another group changed the room's age (1 + new linkAge; 0: nothing pending). This group reloads
    // its scene with it as soon as no cutscene is running.
    u8 agePending;
    // Phase 5b: whose arrival started the title card with the name of the scene (-1: the whole group arrived, as
    // when the scene loads). The card runs in the simulation as always; only that player's screen draws it.
    s8 titleFor;
    // Phase 5b: the player whose own effect darkens the scene's lights (a fairy that revives it, its spin attack
    // charge, its ocarina effect); -1: an effect of the world, for everybody. The lights change in the simulation
    // as always; the other players' screens draw them without that change.
    s8 adjOwner;
    // Phase 5b: this group's scene has to be loaded again in place (see SharedGame.cpp, kSceneFlags).
    u8 refreshPending;
    // Finding AB: 1 + the player reviving with a fairy (0: nobody). The darkening of the game over lights, their black
    // fill in a room with a fixed camera and the black bars are its screen's; the point lights follow its Link. Only
    // read while the game over context is in a revive state. Stored as 1 + slot so that "nobody" is the zero every
    // reset of this structure leaves (the trap of finding Z).
    u8 reviveBy;
    u8 pad6;
    // Phase 5b: rupees the room spent beyond what it had (two groups buying at the same moment); shown as zero
    // rupees and paid off by the next ones.
    s16 rupeeDebt;
    s16 pad7[3];
    u8 msgStatics[ZMP_MAX_PLAYERS + 1][ZMP_MSG_STATICS_SIZE];
    u8 ocaStatics[ZMP_MAX_PLAYERS + 1][ZMP_OCA_STATICS_SIZE]; // each player's ocarina (code_800EC960.c)
    u8 msgSegment[ZMP_MAX_PLAYERS][0x2200]; // each slot's text box background and icon (msgCtx.textboxSegment)
    MessageContext msgStore[ZMP_MAX_PLAYERS + 1];
    // Shared game baseline (SharedGame.cpp): what the other groups and the server already know.
    u8 sharedValid;
    u16 sharedSize;
    u8 pad5[6];
    u8 sharedBase[ZMP_SHARED_MAX];
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
// Phase 5b, one text box per player. z_play.c: the text box of every player runs (and draws; only the local player's
// is shown, or the anchor's during a cutscene everybody watches). Return 0 outside a session (the original call runs).
// Phase 5b, age (z_play.c, Play_Destroy with a change of age): every player's buttons and equipment are swapped,
// each with its own of the other age. Returns 0 outside a session (the original swap runs).
s32 Zmp_AgeSwapAll(PlayState* play);
// Phase 5b: the name of the scene is shown to who enters it, not to the ones already there. z_player.c (the card
// starts) and z_actor.c (TitleCard_Draw).
// z_kankyo.c, Environment_AdjustLights: whose effect it is (see adjOwner).
void Zmp_OnLightAdjust(PlayState* play);
// z_player.c: `player` dies with a fairy in a bottle and the game over context starts its revive (finding AB). The
// original darkens the scene's lights with the game over lights (z_kankyo.c, Environment_FadeInGameOverLights), not
// with Environment_AdjustLights, so nothing said whose it was: everybody saw it once the group had changed scene.
void Zmp_OnFairyRevive(PlayState* play, Player* player);
// z_kankyo.c, the game over lights: the Link they light (the one reviving with a fairy; else GET_PLAYER).
Player* Zmp_GameOverLightPlayer(PlayState* play);
// z_camera.c, Camera_UpdateWater, instead of Environment_EnableUnderwaterLights / Environment_DisableUnderwaterLights
// (finding Y). The original has one camera: when its eye goes under water the scene's lights change to the water's,
// and when it comes out they go back to what a file static remembered. In multiplayer every player has a camera and
// they all ran that code on the one set of lights: everybody saw the water's lights when anybody's camera dipped, two
// cameras under at once left them on for good, and the static (outside the state that travels) made who had just come
// in differ from the rest. In a session the lights of the water are each player's own picture (Rooms.cpp,
// Zmp_DrawLightBegin): this records the setting for the player in context (-1: its eye came out) and returns 1, and the
// scene's lights are not touched. Returns 0 outside a session (the original code runs).
s32 Zmp_OnUnderwaterLights(PlayState* play, s32 waterLightsIndex);
// z_camera.c, Camera_UpdateWater: the muffled sound of a camera under water is heard on the PC of the player whose
// camera it is (before, on every PC, and it was the camera of the first player of the scene that decided).
s32 Zmp_HearsCameraWater(Camera* camera);
// z_play.c: while a player revives with a fairy (the game over context is busy with it) the text boxes of the other
// players go on. Returns 0 outside a session. actorsFrozen: the actors did not run in this tick (a hit's or a finishing
// blow's freeze): the text boxes wait too (finding AG, D-104).
s32 Zmp_MessageUpdateDuringRevive(PlayState* play, s32 actorsFrozen);
void Zmp_OnTitleCard(void);
s32 Zmp_TitleCardHidden(void);
s32 Zmp_MessageUpdateAll(PlayState* play, s32 actorsFrozen);
s32 Zmp_MessageDrawAll(PlayState* play);
// Phase 5b, one ocarina per player (code_800EC960.c): every player's ocarina is updated with its own input (returns
// 0 outside a session), and whether the ocarina in context is the one this machine's audio plays.
s32 Zmp_OcarinaUpdateAll(void);
s32 Zmp_OcarinaAudible(void);
s32 AudioOcarina_ZmpStatics(u8* buf, s32 load);
s32 AudioOcarina_ZmpIsOn(void);
// z_message_PAL.c: saves (load = 0) / restores (load = 1) the statics of the text box code that belong to one text
// box, into a buffer of ZMP_MSG_STATICS_SIZE bytes.
void Message_ZmpStatics(u8* buf, s32 load);
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
// Play_Update, every tick before the scene change check (phase 5). Returns 0 when a pending scene change must not
// happen in this simulation: the player who walked out leaves the group and goes on alone on its own machine.
s32 Zmp_TransitionGate(PlayState* play);
// z_player.c: this player's own action (an exit, a void, Farore's Wind) started the scene change.
void Zmp_NoteTransitionBy(Player* player);
// Phase 5b, blue warp (z_door_warp1.c): a player enters it (the warp is its own until it leaves); the warp's scene
// change takes only that player, with the cutscene of the destination if there is one. Zmp_WarpIsShared: 1 while other
// players stay in the scene (the warp's screen effects on the world are skipped).
void Zmp_WarpBegin(Player* player);
void Zmp_WarpTransition(Player* player);
s32 Zmp_WarpIsShared(void);
// Gohma's blue warp (z_boss_goma.c) is placed away from "the player"; this says whether another present player stands
// within `half` of (x, z) along both axes, so that it does not appear under that one either (finding Z, D-099).
s32 Zmp_OtherPlayerInBoxXZ(Player* except, f32 x, f32 z, f32 half);
// Phase 5b, barred doors (z_door_shutter.c): 1 when this player stands in another room than `room` (the room whose
// fight closed the bars): the door lets it in.
s32 Zmp_PlayerOutsideRoom(Player* player, s32 room);
// Doors (z_door_shutter.c, z_en_door.c): a door offers itself to every player standing at it, not only to the nearest
// one (D-086). Loop: `s32 it = -1; while (Zmp_DoorNextPlayer(play, door, &it)) { ... GET_PLAYER(play) ... }`. The
// first pass is the door's own context (the only pass outside a ZMP session); the next ones are the other players
// near the door, each as "the player"; when it returns 0 the door's own context is back. Never leave the loop early.
s32 Zmp_DoorNextPlayer(PlayState* play, Actor* door, s32* it);
// Phase 5b, warp songs (z_player.c): with other players in the scene the song takes only its player (no scripted warp
// cutscene, which would freeze everybody) and invites the others. Returns 0 when the original warp must run.
s32 Zmp_WarpSongStart(Player* player, PlayState* play);
// z_player.c: scene exits act as walls (tests, "zmp_lock_exits").
s32 Zmp_ExitsLocked(void);

// Cameras (phase 4, PLAN.md 2.9, Cameras.cpp). Each player has its own active camera; a sub camera is GLOBAL (every
// player's active camera while it runs: boss and scripted cutscenes) or belongs to one player (one-point cutscenes:
// chests, item get, crawlspaces...).
void Zmp_OnSubCameraCreated(PlayState* play, s16 camId);
// z_camera.c read its table of camera values with a mode the camera's setting does not have (a bug: see there).
void Zmp_OnInvalidCameraMode(s16 setting, s16 mode);
// How many times since the game started (diagnostics and tests; never part of the simulation).
u32 Zmp_InvalidCameraModeCount(void);
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
// Phase 5b, open passages between rooms (z_en_holl.c): the player in context stands near one (its room is `room`,
// the one on the other side is loaded too), or walked away from it into `room`.
void Zmp_HollNear(PlayState* play, s32 room, s32 otherRoom);
void Zmp_HollSettle(PlayState* play, s32 room);
void Zmp_HollTick(PlayState* play);
// Present players in slot order: the next one after `after` (-1 to start), or -1.
s32 Zmp_NextPresentSlot(s32 after);
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
// Diagnostics (state dumps): the quakes in progress (z_quake.c keeps them in statics of its own).
s32 Zmp_QuakeInfo(s32 idx, s32* callback, s32* countdown, s32* zoom, s32* owner);
s32 Zmp_QuakeCount(void);
// The quakes in progress as plain values for the state hash of a group (no pointers); returns how many it wrote.
s32 Zmp_QuakeHashData(s16* out, s32 max);
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
// Phase 5b: the view an actor's projected position is taken from (the player who has it best in sight), a box test
// against every player's picture, and the pre-rendered room pictures per player (z_actor.c, z_boss_goma.c, z_room.c).
MtxF* Zmp_ActorViewProjection(PlayState* play, Actor* actor);
s32 Zmp_AnyViewBox(PlayState* play, Actor* actor, f32 maxX, f32 maxY, f32 minZ, f32 maxZ);
void Zmp_RoomImageBegin(PlayState* play, Room* room);
void Zmp_RoomImageEnd(PlayState* play);
// Phase 5b, 3D sound: sound positions are actor positions projected with the canonical camera (simulation state,
// the same on every machine). The audio code reads them through this: on a machine whose picture comes from another
// camera, the coordinate (axis 0..2 of the Vec3f at posX) as seen from that local camera. Presentation only: the
// result goes to the mixer (pan, volume, priority of the sound bank), never back into the simulation.
f32 Zmp_SfxCoord(const f32* posX, s32 axis);
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
// Lockstep events (applied at the start of their tick, before the update). `info`: where and with what the player
// appears (phase 5, "zmp_spawn ..." from its JOIN_GROUP; empty: next to the entrance with the anchor's equipment).
void Spawn(int slot, const std::string& info = std::string());
void Despawn(int slot);
// Phase 5: this machine's player left the group through a scene change; the other Links stay behind in the old scene
// and are not brought to the next one.
void KeepOnlyLocal();
// Phase 5: "zmp_spawn ..." describing the local player (entrance it arrives by, place, health and block).
std::string LocalSpawnInfo(int entrance, bool withPlace);
// Phase 5b: the warp song invitation, for the overlay (by < 0: none). hold: ticks of L of the local player.
struct WarpInvite {
    int by = -1;
    int scene = -1;
    int ticks = 0;
    int hold = 0;
};
WarpInvite Invite();
// Phase 5: number of Play inits so far (a detached client has arrived when a new scene loaded).
uint32_t PlayInitCount();
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
// The light setting of the water this PC's picture was last drawn with (its player's camera is under water), or -1.
int LocalWaterLight();
// The colours this PC's last picture was drawn with: fog (3 bytes) and ambient light (3 bytes).
const u8* LocalDrawnLight();
// The light setting of the water the camera of this PC's picture is under now (-1: it is not under water).
int PictureWaterLight(PlayState* play);
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
// Diagnostics (state dumps): the view-projection matrix the last draw used for "what does slot k see" (16 floats),
// or nullptr if it had none; slot -1: the canonical view.
const float* SlotViewProjectionForDump(int slot);
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
// Phase 5b: msgMode of the text box of a slot (MSGMODE_NONE: closed).
int SlotMsgMode(int slot);
// Phase 5b: the last picture ignored the light effect of another player (its fairy, its spin attack charge).
bool LightEffectHidden();
bool LightEffectDrawn();
// The owner of the light effect the last picture was judged with (-1: the world's, everybody sees it).
int LightEffectOwner();
// The player reviving with a fairy now (finding AB), or -1 (nobody, or a game over of the whole group).
int FairyReviver(PlayState* play);
// The black bars the last picture was cut with.
int DrawnLetterbox();
int LocalBgImage();
Vec3f PictureEye();
Vec3f PictureAt();
int HudHealth();
// Phase 5b: which of the four bottles this group has (bit i: bottle i), and "everybody gets bottle i" (empty).
int BottleMask();
void ShareBottle(int i);
// Tests: nobody of this group has bottle i any more; a world position as the picture of a player has it.
void ClearBottle(int i);
bool ProjectForSlot(int slot, const Vec3f& world, Vec3f* proj, f32* w);
const MessageContext* SlotMessage(int slot);
int Anchor();
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
// Stage of the save question of the local menu (the original's unk_1EC: 4 is the "Game saved." screen).
int SaveStage();
} // namespace Zmp::Pause
#endif
