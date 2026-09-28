#pragma once

// ZMP console variables. All of them are local client settings (presentation, connection,
// test tooling); none of them changes what the simulation computes.
#define CVAR_ZMP(var) "gZmp." var

#define ZMP_CVAR_HARNESS_PORT CVAR_ZMP("Harness.Port")
#define ZMP_CVAR_SERVER_HOST CVAR_ZMP("Server.Host")
#define ZMP_CVAR_SERVER_PORT CVAR_ZMP("Server.Port")
#define ZMP_CVAR_ROOM CVAR_ZMP("Room")
#define ZMP_CVAR_NAME CVAR_ZMP("Name")
#define ZMP_CVAR_AUTOCONNECT CVAR_ZMP("AutoConnect")
#define ZMP_CVAR_OVERLAY CVAR_ZMP("Overlay")
#define ZMP_CVAR_ROOM_WINDOW "gOpenWindows.ZmpRoom"
// Test builds only: replaces the build hash sent in the handshake (used by the
// handshake rejection test).
#define ZMP_CVAR_DEBUG_BUILD_HASH CVAR_ZMP("Debug.BuildHashOverride")

// Replay (phase 1): a .zmpinput started automatically when the game reaches the file select
// screen; optional save state to start from; speed factor (presentation only).
#define ZMP_CVAR_REPLAY_PATH CVAR_ZMP("Replay.Path")
#define ZMP_CVAR_REPLAY_FROM_STATE CVAR_ZMP("Replay.FromState")
#define ZMP_CVAR_REPLAY_SPEED CVAR_ZMP("Replay.Speed")
// Ticks between two "tick=... hash=..." lines in logs/zmp.log (1 = every tick, 0 = off).
#define ZMP_CVAR_HASH_LOG_INTERVAL CVAR_ZMP("Sim.HashLogInterval")
// Mute the game audio while its window does not have the focus (presentation only).
#define ZMP_CVAR_MUTE_UNFOCUSED CVAR_ZMP("Audio.MuteWhenUnfocused")

#define ZMP_DEFAULT_HOST "127.0.0.1"
#define ZMP_DEFAULT_PORT 47100
#define ZMP_DEFAULT_ROOM "zmp"
