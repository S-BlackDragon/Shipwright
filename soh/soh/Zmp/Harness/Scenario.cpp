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
#include "overlays/actors/ovl_Boss_Tw/z_boss_tw.h"
#include "overlays/actors/ovl_Boss_Goma/z_boss_goma.h"
#include "overlays/actors/ovl_En_Horse/z_en_horse.h" // D-208
#include "overlays/actors/ovl_Boss_Sst/z_boss_sst.h"
#include "overlays/actors/ovl_Bg_Haka_Gate/z_bg_haka_gate.h"
#include "overlays/actors/ovl_Boss_Ganon/z_boss_ganon.h"   // ZMP phase 6 scenario tests (f6_ganon.py)
#include "overlays/actors/ovl_Boss_Ganon2/z_boss_ganon2.h" // ZMP phase 6 scenario tests (f6_ganon.py)
#include "overlays/actors/ovl_Demo_Kekkai/z_demo_kekkai.h" // ZMP phase 6 scenario tests (f6_castle_rooms.py)
extern PlayState* gPlayState;
s32 Object_Spawn(ObjectContext* objectCtx, s16 objectId);          // z_scene.c (not in functions.h)
void func_80837948(PlayState* play, Player* thisx, s32 arg2);      // z_player.c: a melee swing starts (the B button)
s32 Player_ZmpMount(PlayState* play, Player* thisx, Actor* horse); // z_player.c (test builds): mount a horse
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

// Salas 2026-10-18 (V5, V7): "zmp_stage hammer <slot>", the hammer swung as the B button swings it (the forward blow
// of func_80837818 with the stick released): the bots' B presses did not reach a blow the tests could see.
struct HammerArg {
    int ok;
};

void HammerInContext(void* p) {
    Player* player = GET_PLAYER(gPlayState);
    if (player->heldItemAction != PLAYER_IA_HAMMER) {
        return;
    }
    func_80837948(gPlayState, player, PLAYER_MWA_HAMMER_FORWARD);
    ((HammerArg*)p)->ok = 1;
}

// Salas 2026-10-18: "zmp_stage mount <slot>", that Link mounts the nearest free horse (within 300) from the side it
// stands on, as A does beside it.
void MountInContext(void* p) {
    Player* player = GET_PLAYER(gPlayState);
    Actor* best = nullptr;
    f32 bestD = 300.0f;
    for (Actor* a = gPlayState->actorCtx.actorLists[ACTORCAT_BG].head; a != nullptr; a = a->next) {
        // D-208: never a horse that is not in the world (ENHORSE_INACTIVE: Epona waiting for her song; it is not drawn,
        // so its rider's place was never worked out and the Link went to twice the horse's coordinates)
        if (a->id != ACTOR_EN_HORSE || (((EnHorse*)a)->stateFlags & ENHORSE_INACTIVE)) {
            continue;
        }
        f32 d = Actor_WorldDistXZToActor(a, &player->actor);
        if (d < bestD) {
            bestD = d;
            best = a;
        }
    }
    ((HammerArg*)p)->ok = best != nullptr && Player_ZmpMount(gPlayState, player, best);
}

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

// Bongo Bongo (f6_shadow.py): BossSst_ZmpStage in the context of a player.
void BongoInContext(void* p) {
    MorphaArg* a = (MorphaArg*)p;
    a->done = BossSst_ZmpStage(gPlayState, a->what);
}

// Twinrova (f6_spirit.py): BossTw_ZmpStage in the context of a player.
void TwinrovaInContext(void* p) {
    MorphaArg* a = (MorphaArg*)p;
    a->done = BossTw_ZmpStage(gPlayState, a->what);
}

// Ganondorf (f6_ganon.py): BossGanon_ZmpStage in the context of a player.
void GanondorfInContext(void* p) {
    MorphaArg* a = (MorphaArg*)p;
    a->done = BossGanon_ZmpStage(gPlayState, a->what);
}

// Ganon (f6_ganon.py): BossGanon2_ZmpStage in the context of a player.
void GanonInContext(void* p) {
    MorphaArg* a = (MorphaArg*)p;
    a->done = BossGanon2_ZmpStage(gPlayState, a->what, a->slot);
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
    if (scene < 0 || scene >= (int)ARRAY_COUNT(gSaveContext.sceneFlags) || flag < 0 || flag > 63) {
        return false;
    }
    bool current = gPlayState != nullptr && gPlayState->sceneNum == scene;
    if (flag > 31) {
        // Salas 2026-10-18 (F11): switch flags 32 to 63 are temporary (actorCtx.flags.tempSwch): they exist only in
        // the scene being played and are lost when it is left, as in the game ("swch" or "tswch").
        if ((type != "swch" && type != "tswch") || !current) {
            return false;
        }
        Flags_SetSwitch(gPlayState, flag);
        return true;
    }
    if (type == "swch" || type == "tswch") {
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
    } else if ((op == "hammer" || op == "mount") && w.size() == 3) {
        int target = Num(w[2]);
        if (target < 0 || target >= ZMP_MAX_PLAYERS || !gZmpSim.slots[target].present) {
            return Bad(cmd, "no such player");
        }
        HammerArg a{ 0 };
        Zmp::Players::RunInContext(target, op == "hammer" ? HammerInContext : MountInContext, &a);
        if (!a.ok) {
            return Bad(cmd, op == "hammer" ? "that player does not hold the hammer" : "no free horse within 300");
        }
    } else if (op == "epona" && w.size() == 2) {
        // D-208: "zmp_stage epona", Epona's Song heard (what z_message_PAL.c does when it is played): an Epona waiting
        // in this scene (params 2, inactive) comes running.
        DREG(53) = 1;
    } else if (op == "owned" && w.size() == 3) {
        // D-194 (block 9, a new game): "zmp_stage owned <mask>": the equipment the group owns (inventory.equipment).
        gSaveContext.inventory.equipment = (u16)Num(w[2]);
    } else if (op == "rupees" && w.size() == 3) {
        // D-194: "zmp_stage rupees <n>": the group's rupees (none coming).
        gSaveContext.rupees = (s16)Num(w[2]);
        gSaveContext.rupeeAccumulator = 0;
    } else if (op == "sclear" && w.size() == 4) {
        // D-194: "zmp_stage sclear <scene> <chest|swch|collect|clear>": every flag of that kind of a scene off (its
        // chests closed again, as in a new game). Not for the scene being played (its flags live in the play state).
        int scene = Num(w[2]);
        if (scene < 0 || scene >= (int)ARRAY_COUNT(gSaveContext.sceneFlags) ||
            (gPlayState != nullptr && gPlayState->sceneNum == scene)) {
            return Bad(cmd, "bad scene for sclear");
        }
        if (w[3] == "chest") {
            gSaveContext.sceneFlags[scene].chest = 0;
        } else if (w[3] == "swch") {
            gSaveContext.sceneFlags[scene].swch = 0;
        } else if (w[3] == "collect") {
            gSaveContext.sceneFlags[scene].collect = 0;
        } else if (w[3] == "clear") {
            gSaveContext.sceneFlags[scene].clear = 0;
        } else {
            return Bad(cmd, "bad flag kind for sclear");
        }
    } else if (op == "inf" && w.size() == 4) {
        if (Num(w[3])) {
            Flags_SetInfTable(Num(w[2]));
        } else {
            Flags_UnsetInfTable(Num(w[2]));
        }
    } else if (op == "iteminf" && w.size() == 4) {
        // D-189: "zmp_stage iteminf <flag> <0|1>" (a Great Fairy's spell is given once: ITEMGETINF_18 to 1A)
        if (Num(w[3])) {
            Flags_SetItemGetInf(Num(w[2]));
        } else {
            Flags_UnsetItemGetInf(Num(w[2]));
        }
    } else if (op == "item" && w.size() == 4) {
        // D-189: "zmp_stage item <inventory slot> <item id>" (255: none), e.g. a spell taken away before its fairy
        int at = Num(w[2]);
        if (at >= 0 && at < 24) {
            gSaveContext.inventory.items[at] = (u8)Num(w[3]);
        }
    } else if (op == "event" && w.size() == 4) {
        if (Num(w[3])) {
            Flags_SetEventChkInf(Num(w[2]));
        } else {
            Flags_UnsetEventChkInf(Num(w[2]));
        }
    } else if (op == "reload" && w.size() == 2) {
        // Block 7: "zmp_stage reload": the group's scene loads again by the entrance it came in through (a scene change
        // of the whole group, white fade, everybody put back where it stood), once no cutscene or menu is running. The
        // flags just set take effect (actors that exist or not by them, scene layers).
        gZmpSim.refreshPending = 1;
    } else if (op == "age" && w.size() == 2) {
        // Block 7: "zmp_stage age": the room's age changes as when the Master Sword is pulled or returned in another
        // group (D-068): this group reloads its scene in place with the other age, everybody where it stood.
        gZmpSim.agePending = (u8)(1 + (gSaveContext.linkAge == LINK_AGE_CHILD ? LINK_AGE_ADULT : LINK_AGE_CHILD));
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
    } else if (op == "bongo" && w.size() == 4) {
        // Bongo Bongo (f6_shadow.py), what the bots cannot aim:
        // "zmp_stage bongo stun <slot>": both hands and the open eye hit by that Link's arrows (the head knocked out,
        //  the hands stunned; whoever a hand held is let go), while the head waits or attacks;
        // "zmp_stage bongo eye <slot>": the knocked-out head hit by that Link's Master Sword.
        int what = w[2] == "stun" ? 0 : (w[2] == "eye" ? 1 : -1);
        int target = Num(w[3]);
        if (what < 0) {
            return Bad(cmd, "unknown bongo event");
        }
        if (target < 0 || target >= ZMP_MAX_PLAYERS || !gZmpSim.slots[target].present) {
            return Bad(cmd, "no such player");
        }
        MorphaArg a{ what, target, 0 };
        Zmp::Players::RunInContext(target, BongoInContext, &a);
        if (!a.done) {
            return Bad(cmd, "Bongo Bongo is not in a state where that applies");
        }
    } else if (op == "gohma" && w.size() == 3) {
        // Gohma (f6_gohma.py, D-170), what the bots cannot aim: "zmp_stage gohma stun": a slingshot seed reaches her
        // open eye (struck down from the ceiling, or stunned on the floor, open to the sword).
        if (w[2] != "stun") {
            return Bad(cmd, "unknown gohma event");
        }
        Actor* goma = nullptr;
        for (Actor* a = gPlayState->actorCtx.actorLists[ACTORCAT_BOSS].head; a != nullptr; a = a->next) {
            if (a->id == ACTOR_BOSS_GOMA) {
                goma = a;
            }
        }
        if (goma == nullptr || !BossGoma_ZmpStage(gPlayState, goma, 0)) {
            return Bad(cmd, "Gohma is not in a state where that applies");
        }
    } else if (op == "twinrova" && w.size() == 4) {
        // Twinrova (f6_spirit.py, D-159), what the bots cannot aim:
        // "zmp_stage twinrova stun <slot>": the charged blast sent back by that Link's Mirror Shield reaches Twinrova
        //  (she falls stunned), while she flies, charges, shoots or spins.
        int what = w[2] == "stun" ? 0 : -1;
        int target = Num(w[3]);
        if (what < 0) {
            return Bad(cmd, "unknown twinrova event");
        }
        if (target < 0 || target >= ZMP_MAX_PLAYERS || !gZmpSim.slots[target].present) {
            return Bad(cmd, "no such player");
        }
        MorphaArg a{ what, target, 0 };
        Zmp::Players::RunInContext(target, TwinrovaInContext, &a);
        if (!a.done) {
            return Bad(cmd, "Twinrova is not in a state where that applies");
        }
    } else if ((op == "ganondorf" || op == "ganon") && w.size() == 4) {
        // Ganondorf and Ganon (f6_ganon.py), what the bots cannot aim:
        // "zmp_stage ganondorf ball <slot>": the energy ball sent back reaches Ganondorf (he hangs stunned);
        // "zmp_stage ganondorf light|stun <slot>": a light arrow of that Link reaches him while stunned (he falls, open
        //  to the sword; from the tennis it does the ball first);
        // "zmp_stage ganon hit <slot>": Ganon's tail hit by that Link's sword (2 off his health, as the original);
        // "zmp_stage ganon sword <slot>": Zelda's sword, lying where it fell, moved under that Link (its pickup, G5);
        // "zmp_stage ganon finish <slot>": while Ganon lies waiting for it, that Link deals the final blow (G6).
        int what = -1;
        if (op == "ganondorf") {
            what = w[2] == "ball" ? 0 : ((w[2] == "light" || w[2] == "stun") ? 1 : -1);
        } else {
            what = w[2] == "sword" ? 0 : (w[2] == "finish" ? 1 : (w[2] == "hit" ? 2 : -1));
        }
        int target = Num(w[3]);
        if (what < 0) {
            return Bad(cmd, "unknown ganondorf / ganon event");
        }
        if (target < 0 || target >= ZMP_MAX_PLAYERS || !gZmpSim.slots[target].present) {
            return Bad(cmd, "no such player");
        }
        MorphaArg a{ what, target, 0 };
        Zmp::Players::RunInContext(target, op == "ganondorf" ? GanondorfInContext : GanonInContext, &a);
        if (!a.done) {
            return Bad(cmd, "Ganondorf / Ganon is not in a state where that applies");
        }
    } else if (op == "kekkai" && w.size() == 4 && w[2] == "hit") {
        // "zmp_stage kekkai hit <slot>": the trial barrier nearest to that Link is hit as by its light arrow (the hit
        // its collider reports; the barrier's own update does the rest at its next update, row T1).
        int target = Num(w[3]);
        Player* p = (target >= 0 && target < ZMP_MAX_PLAYERS && gZmpSim.slots[target].present)
                        ? Zmp::Players::SlotPlayer(target)
                        : nullptr;
        if (p == nullptr) {
            return Bad(cmd, "no such player");
        }
        DemoKekkai* best = nullptr;
        f32 bestDist = 0.0f;
        for (int cat = 0; cat < ACTORCAT_MAX; cat++) {
            for (Actor* a = gPlayState->actorCtx.actorLists[cat].head; a != nullptr; a = a->next) {
                if (a->id != ACTOR_DEMO_KEKKAI || a->params < 1 || a->params > 6) {
                    continue;
                }
                f32 d = Actor_WorldDistXZToActor(a, &p->actor);
                if (best == nullptr || d < bestDist) {
                    best = (DemoKekkai*)a;
                    bestDist = d;
                }
            }
        }
        if (best == nullptr) {
            return Bad(cmd, "no trial barrier in the scene");
        }
        best->collider2.base.acFlags |= AC_HIT;
        best->collider2.base.ac = &p->actor; // (that Link broke it, D-188)
    } else if (op == "subtimer" && w.size() == 3) {
        // "zmp_stage subtimer <seconds>": the countdown that runs (the escape's, E1) jumps to <seconds>.
        if (gSaveContext.subTimerState == SUBTIMER_STATE_OFF) {
            return Bad(cmd, "no countdown running");
        }
        gSaveContext.subTimerSeconds = (s16)CLAMP(Num(w[2]), 1, 999);
    } else if (op == "haka_gate" && w.size() == 5 && w[2] == "turn") {
        // "zmp_stage haka_gate turn <slot> <1|-1>": the statue of the Shadow Temple's truth spinner turns one step
        // (60 degrees) that way, as when that Link pushes it (row B7). Only while it waits for a push.
        int target = Num(w[3]);
        if (target < 0 || target >= ZMP_MAX_PLAYERS || !gZmpSim.slots[target].present) {
            return Bad(cmd, "no such player");
        }
        if (!BgHakaGate_ZmpTurn(gPlayState, Num(w[4]))) {
            return Bad(cmd, "the statue is not waiting for a push");
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
