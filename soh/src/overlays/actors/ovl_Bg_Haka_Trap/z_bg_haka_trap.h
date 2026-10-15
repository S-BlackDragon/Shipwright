#ifndef Z_BG_HAKA_TRAP_H
#define Z_BG_HAKA_TRAP_H

#include <libultraship/libultra.h>
#include "global.h"

typedef enum {
    /* 0x00 */ HAKA_TRAP_GUILLOTINE_SLOW,
    /* 0x01 */ HAKA_TRAP_SPIKED_BOX,
    /* 0x02 */ HAKA_TRAP_SPIKED_WALL,
    /* 0x03 */ HAKA_TRAP_SPIKED_WALL_2,
    /* 0x04 */ HAKA_TRAP_PROPELLER,
    /* 0x05 */ HAKA_TRAP_GUILLOTINE_FAST
} HakaTrapType;

struct BgHakaTrap;

typedef void (*BgHakaTrapActionFunc)(struct BgHakaTrap*, PlayState*);

typedef struct BgHakaTrap {
    /* 0x0000 */ DynaPolyActor dyna;
    /* 0x0164 */ BgHakaTrapActionFunc actionFunc;
    /* 0x0168 */ u8 timer;
    /* 0x0169 */ u8 isSpikedCrusherStationary;
    /* 0x016A */ s16 unk_16A; // used as boolean for HAKA_TRAP_GUILLOTINE_SLOW/FAST, s16 for HAKA_TRAP_SPIKED_BOX
    /* 0x016C */ Vec3f chainLiftSfxPos;
    /* 0x0178 */ ColliderCylinder colliderCylinder;
    /* 0x01C4 */ ColliderTris colliderSpikes;
    /* 0x01E4 */ ColliderTrisElement colliderSpikesItem[2];
    // ZMP (rows B8 and B11 of the Shadow Temple, D-149): the blade and the spikes put their body cylinder next to
    // "the player" only. In a group each other present player gets its own copy of it (by slot), and the walls
    // crush the players between them one per tick (bits by slot).
    ColliderCylinder zmpCylinders[6];
    u8 zmpCylinderAt;
    u8 zmpCrushMask;
} BgHakaTrap; // size = 0x029C

#endif
