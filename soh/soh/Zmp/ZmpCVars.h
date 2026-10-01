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
// Test builds only: pretends the assets come from another ROM version (its CRC in hex, as in GameVersions.h): the
// handshake sends that version's name and another assets hash (test of the reject message, phase 5b).
#define ZMP_CVAR_DEBUG_ROM CVAR_ZMP("Debug.RomOverride")
// Debug: readable dump of every lockstep tick (desync hunting; RESYNC keeps the last 40 in logs/desync-<t>-ticks/).
#define ZMP_CVAR_DEBUG_TICK_DUMPS CVAR_ZMP("Debug.TickDumps")

// Replay (phase 1): a .zmpinput started automatically when the game reaches the file select
// screen; optional save state to start from; speed factor (presentation only).
#define ZMP_CVAR_REPLAY_PATH CVAR_ZMP("Replay.Path")
#define ZMP_CVAR_REPLAY_FROM_STATE CVAR_ZMP("Replay.FromState")
#define ZMP_CVAR_REPLAY_SPEED CVAR_ZMP("Replay.Speed")
// Ticks between two "tick=... hash=..." lines in logs/zmp.log (1 = every tick, 0 = off).
#define ZMP_CVAR_HASH_LOG_INTERVAL CVAR_ZMP("Sim.HashLogInterval")
// Mute the game audio while its window does not have the focus (presentation only).
#define ZMP_CVAR_MUTE_UNFOCUSED CVAR_ZMP("Audio.MuteWhenUnfocused")
// Tests (D-063): -1 = use the real window focus; 0 / 1 = behave as if this instance's window had not / had the focus.
// The harness never moves the real focus.
#define ZMP_CVAR_FOCUS_OVERRIDE CVAR_ZMP("Audio.FocusOverride")
// Test instances: their windows never become the active window (D-063).
#define ZMP_CVAR_TEST_NO_ACTIVATE CVAR_ZMP("Test.NoActivate")

// Periodic autosave of the group leader (phase 4): minutes of simulation between two (0 = off; the trigger counts
// simulation ticks, never the clock); tests can give the interval in ticks instead. Resume: the founder of a group
// loads the last session state of the room (Save/zmp_session_<room>.zmps).
#define ZMP_CVAR_AUTOSAVE_MINUTES CVAR_ZMP("Autosave.Minutes")
#define ZMP_CVAR_AUTOSAVE_TICKS CVAR_ZMP("Autosave.Ticks")
#define ZMP_CVAR_RESUME_SESSION CVAR_ZMP("ResumeSession")
// Phase 5: join the group of this player when connecting (empty: the group of the scene, or the biggest one); the
// founder's own game replaces the room's game kept on the server (start the room over).
#define ZMP_CVAR_FOLLOW CVAR_ZMP("Follow")
#define ZMP_CVAR_OVERWRITE_ROOM_GAME CVAR_ZMP("OverwriteRoomGame")

// Phase 5b: the message box at the bottom left (Ui/ChatBox.cpp). Local presentation settings.
#define ZMP_CVAR_CHAT_HIDDEN CVAR_ZMP("Chat.Hidden")
#define ZMP_CVAR_CHAT_X CVAR_ZMP("Chat.X")           // left edge, fraction of the picture's width
#define ZMP_CVAR_CHAT_BOTTOM CVAR_ZMP("Chat.Bottom") // bottom edge, fraction of the picture's height
#define ZMP_CVAR_CHAT_WIDTH CVAR_ZMP("Chat.Width")
#define ZMP_CVAR_CHAT_LINES CVAR_ZMP("Chat.Lines")
#define ZMP_CVAR_CHAT_FONT_SCALE CVAR_ZMP("Chat.FontScale")
#define ZMP_CVAR_CHAT_BG_ALPHA CVAR_ZMP("Chat.BackgroundAlpha")
#define ZMP_CVAR_CHAT_SECONDS CVAR_ZMP("Chat.Seconds")
#define ZMP_CVAR_CHAT_COLOR_R CVAR_ZMP("Chat.ColorR")
#define ZMP_CVAR_CHAT_COLOR_G CVAR_ZMP("Chat.ColorG")
#define ZMP_CVAR_CHAT_COLOR_B CVAR_ZMP("Chat.ColorB")
#define ZMP_CVAR_CHAT_CAT_PLAYERS CVAR_ZMP("Chat.ShowPlayers")
#define ZMP_CVAR_CHAT_CAT_ITEMS CVAR_ZMP("Chat.ShowItems")
#define ZMP_CVAR_CHAT_CAT_SYSTEM CVAR_ZMP("Chat.ShowSystem")
#define ZMP_CVAR_CHAT_CAT_DEBUG CVAR_ZMP("Chat.ShowDebug")
// The diagnostic lines at the top (connection, tick, delay, hash): off in the demos, on in the test instances.
#define ZMP_CVAR_DEBUG_OVERLAY CVAR_ZMP("DebugOverlay")

#define ZMP_DEFAULT_HOST "127.0.0.1"
#define ZMP_DEFAULT_PORT 47100
#define ZMP_DEFAULT_ROOM "zmp"
