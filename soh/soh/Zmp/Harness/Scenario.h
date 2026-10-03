#pragma once

#ifdef ZMP_HARNESS

#include <string>

// ZMP phase 6: scenario events for the test harness (tools/harness/zmp/stage.py). A scenario takes a group of up to
// six players to a room of a dungeon or a boss with the age, items, upgrades, hearts, magic, flags and keys that room
// needs, without playing the way there. Every change is a lockstep event ("zmp_stage ..."): all the machines of the
// group apply it at the same tick, in the same order, so the state stays identical. Shared progression (items,
// upgrades, heart containers, magic meter size, keys, flags) is written once; the per-player block (health, magic,
// ammo) of every present player is written in that player's own context. Test builds only.
//
//   zmp_stage magic <0|1|2>                      magic meter: none, single, double (everybody's meter full)
//   zmp_stage hearts <containers> <0|1>          heart containers and double defense (everybody's health full)
//   zmp_stage give <item> [<item>...]            Item_Give without any cutscene (decimal or 0x item ids)
//   zmp_stage upgrade <type> <value>             Inventory_ChangeUpgrade (UPG_* type, absolute value)
//   zmp_stage keys <dungeon> <n>                 small keys of a dungeon (index of gSaveContext.inventory.dungeonKeys)
//   zmp_stage ditems <dungeon> <mask>            boss key / compass / map bits of a dungeon (absolute)
//   zmp_stage sflag <scene> <swch|chest|clear|collect> <flag>   a permanent flag of any scene (the live copy too when
//                                                               it is the current scene)
//   zmp_stage inf <flag> <0|1>                   an infTable flag
//   zmp_stage event <flag> <0|1>                 an eventChkInf flag (0 clears it: a boss intro plays again)
//   zmp_stage refill                             every present player: full health, full magic, ammo to its bag size
//   zmp_stage hp <slot> <health>                 one player's health (16 = one heart)
//   zmp_stage actor_hp <actor id> <params|-1> <health>   health of every actor with that id (and params): the
//                                                        collider's health, and King Dodongo's own counter
//   zmp_stage object <object id>                 load an object bank the room does not have (to spawn an actor there)
//   zmp_stage barinade skip 0                   supports, zappers and Bari gone; the body goes to its last phase
//   zmp_stage barinade last <hits>               the body's last round, with <hits> left
namespace Zmp::Harness {

// Applies one "zmp_stage ..." event sent by `slot`. Returns false (and logs why) when it was malformed.
bool ApplyStageEvent(int slot, const std::string& cmd);

} // namespace Zmp::Harness

#endif
