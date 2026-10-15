#ifndef Z_BG_HAKA_MEGANE_H
#define Z_BG_HAKA_MEGANE_H

#include <libultraship/libultra.h>
#include "global.h"

struct BgHakaMegane;

typedef void (*BgHakaMeganeActionFunc)(struct BgHakaMegane*, PlayState*);

typedef struct BgHakaMegane {
    /* 0x0000 */ DynaPolyActor dyna;
    /* 0x0164 */ BgHakaMeganeActionFunc actionFunc;
    /* 0x0168 */ char unk_168[0x1];
    /* 0x0169 */ s8 objBankIndex;
    /* 0x016A */ char unk_16A[0x2];
} BgHakaMegane; // size = 0x016C

// ZMP phase 6 scenario tests (f6_shadow_rooms.py, row B10): whether this piece switches its collision with the Lens.
s32 BgHakaMegane_ZmpReacts(const BgHakaMegane* megane);

#endif
