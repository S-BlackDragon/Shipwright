// ZMP phase 6: scenario events of the test harness (see Scenario.h). Test builds only.
//
// Nothing here keeps state of its own: every event writes the simulation (gSaveContext, the per-player blocks, actors)
// at the tick the lockstep applies it, identically on every machine. No static variable changes at run time (the
// statics test of the fast suite would otherwise have to classify it).

#ifdef ZMP_HARNESS

#include "Scenario.h"

#include <cstdlib>
#include <string>
#include <vector>

#include "soh/Zmp/ZmpLog.h"
#include "soh/Zmp/Sim/ZmpPlayers.h"

extern "C" {
#include <z64.h>
#include "variables.h"
#include "functions.h"
#include "macros.h"
#include "overlays/actors/ovl_Boss_Dodongo/z_boss_dodongo.h"
#include "overlays/actors/ovl_Boss_Va/z_boss_va.h"
#include "overlays/actors/ovl_Boss_Ganondrof/z_boss_ganondrof.h"
#include "overlays/actors/ovl_En_fHG/z_en_fhg.h"
#include "overlays/actors/ovl_En_Fhg_Fire/z_en_fhg_fire.h"
#include "overlays/actors/ovl_Boss_Fd/z_boss_fd.h"
#include "overlays/actors/ovl_Boss_Fd2/z_boss_fd2.h"
#include "overlays/actors/ovl_Boss_Mo/z_boss_mo.h"
extern PlayState* gPlayState;
s32 Object_Spawn(ObjectContext* objectCtx, s16 objectId); // z_scene.c (not in functions.h)
}

namespace Zmp::Harness {

namespace {

std::vector<std::string> Words(const std::string& cmd) {
    std::vector<std::string> w;
    size_t pos = 0;
    while (pos < cmd.size()) {
        size_t next = cmd.find(' ', pos);
        std::string tok = cmd.substr(pos, next == std::string::npos ? std::string::npos : next - pos);
        if (!tok.empty()) {
            w.push_back(tok);
        }
        if (next == std::string::npos) {
            break;
        }
        pos = next + 1;
    }
    return w;
}

int Num(const std::string& s) {
    return (int)strtol(s.c_str(), nullptr, 0);
}

// Runs fn in the context of every present player (its block is live in gSaveContext while fn runs).
void ForEachPresent(void (*fn)(void*), void* arg) {
    for (int k = 0; k < ZMP_MAX_PLAYERS; k++) {
        if (gZmpSim.slots[k].active && gZmpSim.slots[k].present) {
            Zmp::Players::RunInContext(k, fn, arg);
        }
    }
}

void FullHealthInContext(void*) {
    gSaveContext.health = gSaveContext.healthCapacity;
    gSaveContext.healthAccumulator = 0;
}

void FullMagicInContext(void*) {
    s16 full = gSaveContext.isMagicAcquired
                   ? (gSaveContext.isDoubleMagicAcquired ? MAGIC_DOUBLE_METER : MAGIC_NORMAL_METER)
                   : 0;
    gSaveContext.magic = (s8)full;
    gSaveContext.magicState = MAGIC_STATE_IDLE;
    gSaveContext.prevMagicState = MAGIC_STATE_IDLE;
    gSaveContext.magicFillTarget = full;
    gSaveContext.magicTarget = 0;
}

void FullAmmoInContext(void*) {
    // Only the ammo of items the group has (having the item is shared, the ammo is each player's).
    if (INV_CONTENT(ITEM_STICK) == ITEM_STICK) {
        AMMO(ITEM_STICK) = (s8)CUR_CAPACITY(UPG_STICKS);
    }
    if (INV_CONTENT(ITEM_NUT) == ITEM_NUT) {
        AMMO(ITEM_NUT) = (s8)CUR_CAPACITY(UPG_NUTS);
    }
    if (INV_CONTENT(ITEM_BOMB) == ITEM_BOMB) {
        AMMO(ITEM_BOMB) = (s8)CUR_CAPACITY(UPG_BOMB_BAG);
    }
    if (INV_CONTENT(ITEM_BOW) == ITEM_BOW) {
        AMMO(ITEM_BOW) = (s8)CUR_CAPACITY(UPG_QUIVER);
    }
    if (INV_CONTENT(ITEM_SLINGSHOT) == ITEM_SLINGSHOT) {
        AMMO(ITEM_SLINGSHOT) = (s8)CUR_CAPACITY(UPG_BULLET_BAG);
    }
    if (INV_CONTENT(ITEM_BOMBCHU) == ITEM_BOMBCHU) {
        AMMO(ITEM_BOMBCHU) = 50;
    }
}

void RefillInContext(void* arg) {
    FullHealthInContext(arg);
    FullMagicInContext(arg);
    FullAmmoInContext(arg);
}

struct HpArg {
    int health;
};

void HpInContext(void* p) {
    gSaveContext.health = (s16)CLAMP(((HpArg*)p)->health, 0, gSaveContext.healthCapacity);
    gSaveContext.healthAccumulator = 0;
}

struct RoomArg {
    int room;
};

// Morpha (f6_water.py, test_f6_morpha.py): BossMo_ZmpStage in the context of a player.
struct MorphaArg {
    int what;
    int slot;
    int done;
};

void MorphaInContext(void* p) {
    MorphaArg* a = (MorphaArg*)p;
    a->done = BossMo_ZmpStage(gPlayState, a->what, a->slot);
}

// D-117 tests: the player in context walks into a room as through a door (the room loads for it if nobody is there).
void RoomInContext(void* p) {
    Room_RequestNewRoom(gPlayState, &gPlayState->roomCtx, ((RoomArg*)p)->room);
}

// D-117 tests: what a door does once its player is through (the rooms nobody stands in go, the doors of the rooms that
// stay are spawned).
void RoomDoneInContext(void* p) {
    (void)p;
    Room_FinishRoomChange(gPlayState, &gPlayState->roomCtx);
}

bool SceneFlag(int scene, const std::string& type, int flag) {
    if (scene < 0 || scene >= (int)ARRAY_COUNT(gSaveContext.sceneFlags) || flag < 0 || flag > 31) {
        return false;
    }
    bool current = gPlayState != nullptr && gPlayState->sceneNum == scene;
    if (type == "swch") {
        gSaveContext.sceneFlags[scene].swch |= 1u << flag;
        if (current) {
            Flags_SetSwitch(gPlayState, flag);
        }
    } else if (type == "chest") {
        gSaveContext.sceneFlags[scene].chest |= 1u << flag;
        if (current) {
            Flags_SetTreasure(gPlayState, flag);
        }
    } else if (type == "clear") {
        gSaveContext.sceneFlags[scene].clear |= 1u << flag;
        if (current) {
            Flags_SetClear(gPlayState, flag);
        }
    } else if (type == "collect") {
        gSaveContext.sceneFlags[scene].collect |= 1u << flag;
        if (current) {
            Flags_SetCollectible(gPlayState, flag);
        }
    } else {
        return false;
    }
    return true;
}

bool Bad(const std::string& cmd, const char* why) {
    Zmp::Log(std::string("zmp: stage: ") + why + ": " + cmd);
    return false;
}

} // namespace

bool ApplyStageEvent(int slot, const std::string& cmd) {
    std::vector<std::string> w = Words(cmd);
    if (w.size() < 2 || w[0] != "zmp_stage") {
        return Bad(cmd, "malformed");
    }
    if (gPlayState == nullptr || !Zmp_MultiActive()) {
        return Bad(cmd, "no group simulation running");
    }
    const std::string& op = w[1];
    if (op == "magic" && w.size() == 3) {
        int level = CLAMP(Num(w[2]), 0, 2);
        gSaveContext.isMagicAcquired = level >= 1;
        gSaveContext.isDoubleMagicAcquired = level >= 2;
        gSaveContext.magicLevel = (s8)level;
        gSaveContext.magicCapacity = (s16)(level * MAGIC_NORMAL_METER);
        // (the meter is already the new size: nothing for Zmp_UpdateHealthAccumulators to fill)
        gZmpSim.lastMagicCapacity = gSaveContext.magicCapacity;
        ForEachPresent(FullMagicInContext, nullptr);
    } else if (op == "hearts" && w.size() == 4) {
        int containers = CLAMP(Num(w[2]), 3, 20);
        gSaveContext.healthCapacity = (s16)(containers * 0x10);
        gSaveContext.isDoubleDefenseAcquired = Num(w[3]) != 0;
        gSaveContext.inventory.defenseHearts = gSaveContext.isDoubleDefenseAcquired ? 20 : 0;
        ForEachPresent(FullHealthInContext, nullptr);
    } else if (op == "give" && w.size() >= 3) {
        // In the sender's context (the event runs there): an ammo item fills only its ammo; "refill" does everybody.
        for (size_t i = 2; i < w.size(); i++) {
            Item_Give(gPlayState, (u8)Num(w[i]));
        }
    } else if (op == "quest" && w.size() == 4) {
        // "zmp_stage quest <mask> <0|1>": quest items (medallions, stones, songs) off or on, for the group. The debug
        // save has every medallion: a test of a boss's medallion takes it away first (f6_forest.py).
        u32 mask = (u32)strtoul(w[2].c_str(), nullptr, 0);
        if (Num(w[3])) {
            gSaveContext.inventory.questItems |= mask;
        } else {
            gSaveContext.inventory.questItems &= ~mask;
        }
    } else if (op == "upgrade" && w.size() == 4) {
        Inventory_ChangeUpgrade((s16)Num(w[2]), (s16)Num(w[3]));
    } else if (op == "keys" && w.size() == 4) {
        int d = Num(w[2]);
        if (d < 0 || d >= (int)ARRAY_COUNT(gSaveContext.inventory.dungeonKeys)) {
            return Bad(cmd, "no such dungeon");
        }
        gSaveContext.inventory.dungeonKeys[d] = (s8)Num(w[3]);
    } else if (op == "ditems" && w.size() == 4) {
        int d = Num(w[2]);
        if (d < 0 || d >= (int)ARRAY_COUNT(gSaveContext.inventory.dungeonItems)) {
            return Bad(cmd, "no such dungeon");
        }
        gSaveContext.inventory.dungeonItems[d] = (u8)Num(w[3]);
    } else if (op == "sflag" && w.size() == 5) {
        if (!SceneFlag(Num(w[2]), w[3], Num(w[4]))) {
            return Bad(cmd, "bad scene flag");
        }
    } else if (op == "inf" && w.size() == 4) {
        if (Num(w[3])) {
            Flags_SetInfTable(Num(w[2]));
        } else {
            Flags_UnsetInfTable(Num(w[2]));
        }
    } else if (op == "event" && w.size() == 4) {
        if (Num(w[3])) {
            Flags_SetEventChkInf(Num(w[2]));
        } else {
            Flags_UnsetEventChkInf(Num(w[2]));
        }
    } else if (op == "refill" && w.size() == 2) {
        ForEachPresent(RefillInContext, nullptr);
    } else if (op == "hp" && w.size() == 4) {
        int target = Num(w[2]);
        if (target < 0 || target >= ZMP_MAX_PLAYERS || !gZmpSim.slots[target].present) {
            return Bad(cmd, "no such player");
        }
        HpArg a{ Num(w[3]) };
        Zmp::Players::RunInContext(target, HpInContext, &a);
    } else if (op == "room" && w.size() == 4) {
        // D-117 tests: "zmp_stage room <slot> <room>": that player is in room <room> from now on, as if it had come
        // through a door (the room loads for it); a teleport then puts it there. (A boss door far from the entrance
        // that leads to it: Jabu-Jabu's, the Water Temple's.)
        int target = Num(w[2]);
        int room = Num(w[3]);
        if (target < 0 || target >= ZMP_MAX_PLAYERS || !gZmpSim.slots[target].present) {
            return Bad(cmd, "no such player");
        }
        if (room < 0 || room >= gPlayState->numRooms) {
            return Bad(cmd, "no such room");
        }
        RoomArg a{ room };
        Zmp::Players::RunInContext(target, RoomInContext, &a);
    } else if (op == "room_done" && w.size() == 3) {
        // D-117 tests: "zmp_stage room_done <slot>", after "room": the room change ends as when a door closes behind
        // that player (its doors appear).
        int target = Num(w[2]);
        if (target < 0 || target >= ZMP_MAX_PLAYERS || !gZmpSim.slots[target].present) {
            return Bad(cmd, "no such player");
        }
        if (gPlayState->roomCtx.status != 0) {
            return Bad(cmd, "a room is still loading");
        }
        Zmp::Players::RunInContext(target, RoomDoneInContext, nullptr);
    } else if (op == "actor_hp" && w.size() == 5) {
        int id = Num(w[2]);
        int params = Num(w[3]);
        int hp = Num(w[4]);
        int n = 0;
        for (int cat = 0; cat < ACTORCAT_MAX; cat++) {
            for (Actor* a = gPlayState->actorCtx.actorLists[cat].head; a != nullptr; a = a->next) {
                if (a->id != id || (params != -1 && a->params != params)) {
                    continue;
                }
                a->colChkInfo.health = (u8)hp;
                if (id == ACTOR_BOSS_DODONGO) {
                    ((BossDodongo*)a)->health = (s16)hp; // King Dodongo counts its health in a field of its own
                }
                n++;
            }
        }
        if (n == 0) {
            return Bad(cmd, "no such actor");
        }
    } else if (op == "object" && w.size() == 3) {
        // An object bank the room does not load (an actor spawned by a test where the game never puts it: Ruto in the
        // first room of Jabu-Jabu). Loaded at the end of the bank list, like the room's own; a room change reloads
        // the list and may drop it (a test uses it in one room).
        s16 id = (s16)Num(w[2]);
        if (Object_GetIndex(&gPlayState->objectCtx, id) < 0) {
            if (gPlayState->objectCtx.num >= OBJECT_EXCHANGE_BANK_MAX - 1) {
                return Bad(cmd, "the object bank is full");
            }
            Object_Spawn(&gPlayState->objectCtx, id);
        }
    } else if (op == "barinade" && w.size() == 4) {
        // "zmp_stage barinade skip 0": supports cut, Bari gone (the zappers stay), the body goes to its last phase;
        // "zmp_stage barinade last <n>": in the last phase, the last round with <n> hits left;
        // "zmp_stage barinade stun <ticks>": in the last phase, the body stunned as by the boomerang (the fight's
        // progress is in statics of the overlay, z_boss_va.c).
        int what = w[2] == "skip" ? 0 : (w[2] == "last" ? 1 : (w[2] == "stun" ? 2 : -1));
        if (what < 0 || !BossVa_ZmpStage(gPlayState, what, Num(w[3]))) {
            return Bad(cmd, "Barinade is not in a state where that applies");
        }
    } else if (op == "ganondrof" && w.size() == 4) {
        // Phantom Ganon (f6_forest.py), what the bots cannot aim by themselves:
        // "zmp_stage ganondrof painting_hit 0": what an arrow does to the boss out of its painting (z_boss_ganondrof.c,
        //   BossGanondrof_CollisionCheck, painting branch): 2 off its health, invincible 10 ticks, its horse hit;
        // "zmp_stage ganondrof return <n>": its energy ball, returned <n> times, hits it (what EnFhgFire_EnergyBall
        //   does on impact): it is stunned at its next update.
        BossGanondrof* g = nullptr;
        for (Actor* a = gPlayState->actorCtx.actorLists[ACTORCAT_BOSS].head; a != nullptr; a = a->next) {
            if (a->id == ACTOR_BOSS_GANONDROF && a->params == GND_REAL_BOSS) {
                g = (BossGanondrof*)a;
            }
        }
        if (g == nullptr || g->actor.child == nullptr || g->deathState != NOT_DEAD) {
            return Bad(cmd, "no Phantom Ganon fighting");
        }
        EnfHG* horse = (EnfHG*)g->actor.child;
        if (w[2] == "painting_hit") {
            if (g->flyMode != GND_FLY_PAINTING || horse->bossGndInPainting || g->work[GND_INVINC_TIMER] != 0) {
                return Bad(cmd, "Phantom Ganon is not out of its painting");
            }
            g->work[GND_INVINC_TIMER] = 10;
            g->actor.colChkInfo.health -= 2;
            horse->hitTimer = 20;
        } else if (w[2] == "return") {
            if (g->flyMode == GND_FLY_PAINTING) {
                return Bad(cmd, "Phantom Ganon is still in the paintings");
            }
            g->returnCount = (u8)CLAMP(Num(w[3]), 1, 100);
        } else {
            return Bad(cmd, "unknown ganondrof event");
        }
    } else if (op == "volvagia" && w.size() == 4) {
        // Volvagia (f6_fire.py, test_f6_volvagia.py), what the bots cannot aim by themselves:
        // "zmp_stage volvagia hammer 0": the head out of its hole is hit as by a hammer (its face exposed);
        // "zmp_stage volvagia sword 0": the knocked-out head is hit as by the Master Sword;
        // "zmp_stage volvagia flame <slot>": one still flame of the breath where that Link stands (row V2).
        if (w[2] == "hammer" || w[2] == "sword") {
            if (!BossFd2_ZmpStage(gPlayState, w[2] == "hammer" ? 0 : 1)) {
                return Bad(cmd, "Volvagia's head is not where that applies");
            }
        } else if (w[2] == "flame") {
            int target = Num(w[3]);
            Player* p = (target >= 0 && target < ZMP_MAX_PLAYERS && gZmpSim.slots[target].present)
                            ? Zmp::Players::SlotPlayer(target)
                            : nullptr;
            if (p == nullptr) {
                return Bad(cmd, "no such player");
            }
            Vec3f at = { p->actor.world.pos.x, p->actor.world.pos.y + 30.0f, p->actor.world.pos.z };
            if (!BossFd_ZmpStage(gPlayState, 0, &at)) {
                return Bad(cmd, "Volvagia is not in the room");
            }
        } else {
            return Bad(cmd, "unknown volvagia event");
        }
    } else if (op == "morpha" && w.size() == 4) {
        // Morpha (f6_water.py, test_f6_morpha.py), what the bots cannot aim or wait for:
        // "zmp_stage morpha grab <slot>": the first tentacle grabs that Link (rows A1, A2 and family T1, D-134);
        // "zmp_stage morpha hook <slot>": the core inside the tentacle is hit as by that Link's hookshot;
        // "zmp_stage morpha sword <slot>": the core out of the tentacle is hit as by that Link's Master Sword;
        // "zmp_stage morpha cut <slot>": the first tentacle is cut as by Din's Fire.
        int what = w[2] == "grab" ? 0 : (w[2] == "hook" ? 1 : (w[2] == "sword" ? 2 : (w[2] == "cut" ? 3 : -1)));
        int target = Num(w[3]);
        if (what < 0) {
            return Bad(cmd, "unknown morpha event");
        }
        if (target < 0 || target >= ZMP_MAX_PLAYERS || !gZmpSim.slots[target].present) {
            return Bad(cmd, "no such player");
        }
        MorphaArg a{ what, target, 0 };
        Zmp::Players::RunInContext(target, MorphaInContext, &a);
        if (!a.done) {
            return Bad(cmd, "Morpha is not in a state where that applies");
        }
    } else if (op == "fhg_burst" && w.size() == 3) {
        // "zmp_stage fhg_burst <slot>": the lightning burst of Phantom Ganon's energy ball when it hits that Link (what
        // EnFhgFire_EnergyBall spawns on BALL_BURST, owner included: D-124). Needs the room's object (its arena).
        int target = Num(w[2]);
        Player* p = (target >= 0 && target < ZMP_MAX_PLAYERS && gZmpSim.slots[target].present)
                        ? Zmp::Players::SlotPlayer(target)
                        : nullptr;
        if (p == nullptr) {
            return Bad(cmd, "no such player");
        }
        Actor* b = Actor_Spawn(&gPlayState->actorCtx, gPlayState, ACTOR_EN_FHG_FIRE, p->actor.world.pos.x,
                               p->actor.world.pos.y + 20.0f, p->actor.world.pos.z, 0xC8, 0, 0, FHGFIRE_LIGHTNING_BURST);
        if (b == nullptr) {
            return Bad(cmd, "the burst could not be spawned");
        }
        ((EnFhgFire*)b)->work[FHGFIRE_US_2] = (s16)(target + 1);
    } else if (op == "gather" && w.size() >= 6) {
        // "zmp_stage gather <x> <y> <z> <radius> [slot ...]": these present Links (all of them without a list) on a
        // ring of <radius> around (x, z), facing its centre, in ONE tick (several players stepping into a blue warp at
        // once, f6_portales.py).
        f32 x = strtof(w[2].c_str(), nullptr);
        f32 y = strtof(w[3].c_str(), nullptr);
        f32 z = strtof(w[4].c_str(), nullptr);
        f32 r = strtof(w[5].c_str(), nullptr);
        std::vector<int> who;
        for (size_t i = 6; i < w.size(); i++) {
            who.push_back(Num(w[i]));
        }
        if (who.empty()) {
            for (int k = 0; k < ZMP_MAX_PLAYERS; k++) {
                who.push_back(k);
            }
        }
        std::vector<Player*> links;
        for (int k : who) {
            Player* p =
                (k >= 0 && k < ZMP_MAX_PLAYERS && gZmpSim.slots[k].present) ? Zmp::Players::SlotPlayer(k) : nullptr;
            if (p != nullptr) {
                links.push_back(p);
            }
        }
        if (links.empty()) {
            return Bad(cmd, "nobody to gather");
        }
        for (size_t i = 0; i < links.size(); i++) {
            s16 a = (s16)(i * 0x10000 / links.size());
            Vec3f pos = { x + Math_SinS(a) * r, y, z + Math_CosS(a) * r };
            Player* p = links[i];
            p->actor.world.pos = p->actor.prevPos = p->actor.home.pos = pos;
            p->actor.shape.rot.y = p->actor.world.rot.y = p->yaw = (s16)(a + 0x8000);
            p->actor.speedXZ = 0.0f;
        }
    } else if (op == "invincible" && w.size() == 4) {
        // "zmp_stage invincible <slot> <ticks>": that Link cannot be hurt for <ticks> (as just after a hit).
        int target = Num(w[2]);
        Player* p = (target >= 0 && target < ZMP_MAX_PLAYERS && gZmpSim.slots[target].present)
                        ? Zmp::Players::SlotPlayer(target)
                        : nullptr;
        if (p == nullptr) {
            return Bad(cmd, "no such player");
        }
        p->invincibilityTimer = (s8)CLAMP(Num(w[3]), 0, 100);
    } else {
        return Bad(cmd, "unknown or malformed");
    }
    Zmp::Log("zmp: stage (slot " + std::to_string(slot) + "): " + cmd);
    return true;
}

} // namespace Zmp::Harness

#endif
