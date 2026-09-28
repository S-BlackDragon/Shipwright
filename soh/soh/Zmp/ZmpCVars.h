#pragma once

// ZMP console variables. All of them are local client settings (presentation or
// connection), none of them affects the simulation.
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

#define ZMP_DEFAULT_HOST "127.0.0.1"
#define ZMP_DEFAULT_PORT 47100
#define ZMP_DEFAULT_ROOM "zmp"
