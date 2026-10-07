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
    } else {
        return Bad(cmd, "unknown or malformed");
    }
    Zmp::Log("zmp: stage (slot " + std::to_string(slot) + "): " + cmd);
    return true;
}

} // namespace Zmp::Harness

#endif
