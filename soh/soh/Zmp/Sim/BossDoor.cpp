// ZMP, D-117 (docs/PETICIONES_ALEX.md 2026-10-03, "Puerta del jefe: esperar al grupo"): the door to a boss's room
// opens only when every player of the group (the players of this simulation: the ones in this dungeon) stands at it,
// and then the whole group enters the boss's room together (one scene change of the group, the boss's intro seen by
// everybody, D-111). While somebody is missing, the player who pushes the door reads "Esperando a X en la puerta del
// jefe" in the message box and the missing ones read "Te esperan en la puerta del jefe".
//
// Which door is a boss door: the scene's own collision says it. When a scene is loaded, every floor or wall of its
// collision whose exit leads into a boss's scene is found (the same data on every machine); a door that opens towards
// one of those exits, close behind it, is the boss door. That covers the locked boss doors of the temples and of
// Ganon's Tower, the door of the Deku Tree (it opens when the room is clear) and Jabu-Jabu's (a switch), without a
// list of doors per dungeon (and the Master Quest dungeons too, whose rooms are others).
//
// Everything decided here is simulation state (gZmpSim: bossWait*, bossPass*), decided with what every machine has
// in the same tick. The exits found in the collision are derived from the scene's data, the same on every machine,
// and found again after every scene load: nothing of it needs to travel.

#include "ZmpPlayers.h"
#include "soh/Zmp/Test/Mutants.h"

#include <cmath>
#include <string>

#include "soh/Zmp/ZmpLog.h"
#include "soh/Zmp/Net/Lockstep.h"

extern "C" {
#include "variables.h"
#include "functions.h"
#include "macros.h"
}

namespace {

// "Junto a la puerta" (D-117): in the same room as the player who pushes, within this distance of the door on the
// floor plane and this much above or below it: about three doors wide. Six Links fit in front of every boss door
// within it (the smallest landing, the Spirit Temple's, is 150 deep and 200 wide: the six stand there in rows of three,
// tests/f6_puerta.py), and a player on the floor below or on a ledge above does not count.
constexpr f32 kAtDoorXZ = 300.0f;
constexpr f32 kAtDoorY = 150.0f;
// A door is a boss door when an exit into a boss's scene is behind it within this distance. Measured in every
// dungeon with the harness (query "debug.boss_exits", 2026-10-07): each dungeon has one such exit, 80 to 160 behind
// its boss door (Deku Tree 80, Forest 80, Dodongo's Cavern and Water 120, Ganon's Tower 110, Spirit 140, Fire 140,
// Jabu-Jabu 140 with 80 of height, Shadow 100), and no other door has it within 300.
constexpr f32 kExitBehindXZ = 300.0f;
constexpr f32 kExitBehindY = 200.0f;
// Ticks the pass of a door opened by the whole group lasts: the first scene change into the boss's room in that time
// takes the whole group. (The unlocking, the door going up and the walk to the exit take 2 to 4 s.)
constexpr s16 kPassTicks = 300;
// Ticks the "waiting" notice stays in the state after a push of the door that did not open it.
constexpr u8 kWaitTicks = 40;

struct BossExit {
    s16 x, y, z;
    s16 entrance;
    s16 scene;
    s16 polys;
};
constexpr int kMaxExits = 8;
BossExit sExits[kMaxExits];
int sExitCount = 0;
u32 sScannedInit = 0xFFFFFFFF;
s16 sScannedScene = -1;

bool IsBossScene(int scene) {
    return scene >= SCENE_DEKU_TREE_BOSS && scene <= SCENE_GANONDORF_BOSS;
}

int EntranceScene(int entrance) {
    return (entrance >= 0 && entrance < ENTR_MAX) ? gEntranceTable[entrance].scene : -1;
}

// The exits of the scene's collision that lead into a boss's scene, grouped (one per opening: a door's exit is made
// of several triangles).
void Scan(PlayState* play) {
    u32 init = Zmp::Players::PlayInitCount();
    if (sScannedInit == init && sScannedScene == play->sceneNum) {
        return;
    }
    sScannedInit = init;
    sScannedScene = play->sceneNum;
    sExitCount = 0;
    if (IsBossScene(play->sceneNum) || play->colCtx.colHeader == nullptr || play->setupExitList == nullptr) {
        return;
    }
    CollisionHeader* h = play->colCtx.colHeader;
    float sum[kMaxExits][3] = {};
    for (int i = 0; i < h->numPolygons; i++) {
        CollisionPoly* poly = &h->polyList[i];
        s32 exitIndex = SurfaceType_GetSceneExitIndex(&play->colCtx, poly, BGCHECK_SCENE);
        if (exitIndex == 0) {
            continue;
        }
        int entrance = play->setupExitList[exitIndex - 1];
        int scene = EntranceScene(entrance);
        if (!IsBossScene(scene)) {
            continue;
        }
        float c[3] = { 0, 0, 0 };
        for (int v = 0; v < 3; v++) {
            const Vec3s& p = h->vtxList[COLPOLY_VTX_INDEX(poly->vtxData[v])];
            c[0] += p.x / 3.0f;
            c[1] += p.y / 3.0f;
            c[2] += p.z / 3.0f;
        }
        int found = -1;
        for (int e = 0; e < sExitCount; e++) {
            float n = sExits[e].polys;
            if (sExits[e].entrance == entrance && fabsf(sum[e][0] / n - c[0]) < 300.0f &&
                fabsf(sum[e][2] / n - c[2]) < 300.0f && fabsf(sum[e][1] / n - c[1]) < 300.0f) {
                found = e;
                break;
            }
        }
        if (found < 0) {
            if (sExitCount == kMaxExits) {
                continue;
            }
            found = sExitCount++;
            sExits[found] = { 0, 0, 0, (s16)entrance, (s16)scene, 0 };
        }
        for (int a = 0; a < 3; a++) {
            sum[found][a] += c[a];
        }
        sExits[found].polys++;
    }
    for (int e = 0; e < sExitCount; e++) {
        float n = sExits[e].polys;
        sExits[e].x = (s16)lroundf(sum[e][0] / n);
        sExits[e].y = (s16)lroundf(sum[e][1] / n);
        sExits[e].z = (s16)lroundf(sum[e][2] / n);
        Zmp::Log("zmp: D-117: exit into boss scene " + std::to_string(sExits[e].scene) + " (entrance " +
                 std::to_string(sExits[e].entrance) + ") at (" + std::to_string(sExits[e].x) + ", " +
                 std::to_string(sExits[e].y) + ", " + std::to_string(sExits[e].z) + ")");
    }
}

// An exit into a boss's scene behind `door` as seen from the side `side` (1: the door's +z side, -1: its -z side).
bool ExitBehind(PlayState* play, Actor* door, int side) {
    Scan(play);
    for (int e = 0; e < sExitCount; e++) {
        Vec3f w = { (f32)sExits[e].x, (f32)sExits[e].y, (f32)sExits[e].z };
        f32 dx = w.x - door->world.pos.x, dz = w.z - door->world.pos.z;
        if (dx * dx + dz * dz > kExitBehindXZ * kExitBehindXZ || fabsf(w.y - door->world.pos.y) > kExitBehindY) {
            continue;
        }
        Vec3f local;
        Actor_WorldToActorCoords(door, &local, &w);
        if ((side > 0 && local.z < 0.0f) || (side < 0 && local.z > 0.0f)) {
            return true;
        }
    }
    return false;
}

bool AtDoor(int j, int pusher, Actor* door) {
    Player* p = Zmp::Players::SlotPlayer(j);
    if (!Zmp::Players::IsPresent(j) || p == nullptr) {
        return false;
    }
    if (Zmp::Players::SlotRoom(j) != Zmp::Players::SlotRoom(pusher)) {
        return false;
    }
    f32 dx = p->actor.world.pos.x - door->world.pos.x, dz = p->actor.world.pos.z - door->world.pos.z;
    return dx * dx + dz * dz <= kAtDoorXZ * kAtDoorXZ && fabsf(p->actor.world.pos.y - door->world.pos.y) <= kAtDoorY;
}

std::string MaskText(int mask) {
    std::string s;
    for (int k = 0; k < ZMP_MAX_PLAYERS; k++) {
        if (mask & (1 << k)) {
            s += (s.empty() ? "" : ",") + std::to_string(k);
        }
    }
    return s;
}

} // namespace

// z_player.c, Player_ActionHandler_1: `player` pushes the sliding door `door` from its side `doorDirection`. Returns 1
// when the door may open as always; 0 when it stays shut (a boss door with somebody of the group missing).
extern "C" s32 Zmp_BossDoorMayOpen(PlayState* play, Player* player, Actor* door, s32 doorDirection) {
    if (!Zmp_MultiActive() || door == nullptr || door->category != ACTORCAT_DOOR || doorDirection == 0) {
        return 1;
    }
    int k = Zmp::Players::SlotOf(&player->actor);
    if (k < 0 || !ExitBehind(play, door, doorDirection > 0 ? 1 : -1)) {
        return 1;
    }
    // (mutation test: the players of the room that play elsewhere, in other groups, are waited for too)
    bool outsider = Zmp_TestMutant("puerta_cuenta_al_de_fuera") &&
                    (int)Zmp::Lockstep::GetStatus().players.size() > Zmp::Players::PresentCount();
    // Alone in the dungeon: the door works as always (and its player goes in alone, D-117 case e).
    // (mutation test: the door opens for whoever pushes it, as before D-117)
    if ((Zmp::Players::PresentCount() < 2 && !outsider) || Zmp_TestMutant("puerta_sin_esperar")) {
        return 1;
    }
    int missing = 0;
    for (int j = 0; j < ZMP_MAX_PLAYERS; j++) {
        // (a player whose connection dropped is not waited for: D-117 case b)
        if (j != k && Zmp::Players::IsPresent(j) && !gZmpSim.slots[j].away && !AtDoor(j, k, door)) {
            missing |= 1 << j;
        }
    }
    if (missing != 0 || outsider) {
        if (gZmpSim.bossWaitBy != k + 1 || gZmpSim.bossWaitMissing != missing || gZmpSim.bossWaitTicks == 0) {
            Zmp::Log("zmp: D-117: slot " + std::to_string(k) + " pushes the boss door; waiting for slots [" +
                     MaskText(missing) + "]" + (outsider ? " (and the players elsewhere: mutant)" : ""));
        }
        gZmpSim.bossWaitBy = (u8)(k + 1);
        gZmpSim.bossWaitMissing = (u8)missing;
        gZmpSim.bossWaitTicks = kWaitTicks;
        return 0;
    }
    gZmpSim.bossWaitBy = 0;
    gZmpSim.bossWaitMissing = 0;
    gZmpSim.bossWaitTicks = 0;
    gZmpSim.bossPassBy = (u8)(k + 1);
    gZmpSim.bossPassTicks = kPassTicks;
    Zmp::Log("zmp: D-117: slot " + std::to_string(k) + " opens the boss door with the whole group at it (" +
             std::to_string(Zmp::Players::PresentCount()) + " players)");
    return 1;
}

namespace Zmp::BossDoor {

void Tick() {
    if (gZmpSim.bossWaitTicks > 0 && --gZmpSim.bossWaitTicks == 0) {
        gZmpSim.bossWaitBy = 0;
        gZmpSim.bossWaitMissing = 0;
    }
    if (gZmpSim.bossPassTicks > 0 && --gZmpSim.bossPassTicks == 0) {
        gZmpSim.bossPassBy = 0;
    }
}

bool TakesGroup(PlayState* play, int k) {
    if (gZmpSim.bossPassBy == 0 || !Zmp::Players::IsPresent(k) || Zmp::Players::PresentCount() < 2) {
        return false;
    }
    return IsBossScene(EntranceScene(play->nextEntranceIndex)) && !IsBossScene(play->sceneNum);
}

void OnPlayInit() {
    gZmpSim.bossWaitBy = 0;
    gZmpSim.bossWaitMissing = 0;
    gZmpSim.bossWaitTicks = 0;
    gZmpSim.bossPassBy = 0;
    gZmpSim.bossPassTicks = 0;
}

Wait Waiting() {
    Wait w;
    if (Zmp_MultiActive() && gZmpSim.bossWaitBy != 0 && gZmpSim.bossWaitTicks > 0) {
        w.by = gZmpSim.bossWaitBy - 1;
        w.missing = gZmpSim.bossWaitMissing;
    }
    return w;
}

std::vector<ExitInfo> Exits(PlayState* play) {
    Scan(play);
    std::vector<ExitInfo> out;
    for (int e = 0; e < sExitCount; e++) {
        out.push_back({ sExits[e].x, sExits[e].y, sExits[e].z, sExits[e].entrance, sExits[e].scene, sExits[e].polys });
    }
    return out;
}

bool IsBossDoorFrom(PlayState* play, Actor* door, int side) {
    return door != nullptr && ExitBehind(play, door, side);
}

} // namespace Zmp::BossDoor
