#pragma once

#include <stddef.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum SaveStateMode {
    SHIP_SAVESTATE_MEASURE,
    SHIP_SAVESTATE_SAVE,
    SHIP_SAVESTATE_LOAD,
} SaveStateMode;

typedef struct SaveStateCtx {
    unsigned char* buffer;
    size_t offset;
    SaveStateMode mode;
} SaveStateCtx;

static inline void SaveState_Blob(SaveStateCtx* ctx, void* data, size_t len) {
    switch (ctx->mode) {
        case SHIP_SAVESTATE_SAVE:
            memcpy(ctx->buffer + ctx->offset, data, len);
            break;
        case SHIP_SAVESTATE_LOAD:
            memcpy(data, ctx->buffer + ctx->offset, len);
            break;
        case SHIP_SAVESTATE_MEASURE:
        default:
            break;
    }
    ctx->offset += len;
}

#define SHIP_SAVESTATE_SERIALIZE_FIELD(field) SaveState_Blob(ctx, &(field), sizeof(field));
#define SHIP_SAVESTATE_DEFINE(Tag, FIELDS)     \
    void Tag##_SaveState(SaveStateCtx* ctx) {  \
        FIELDS(SHIP_SAVESTATE_SERIALIZE_FIELD) \
    }

// ZMP: a static that points to an actor travels as a reference that does not depend on where things are in memory
// (the actor's category and its place in that category's list), never as an address (docs/DECISIONES.md D-116).
// `field` is the static (an Actor* or a pointer to a struct that starts with an Actor). Implemented in
// soh/Zmp/State/StateBlob.cpp.
void Zmp_SaveStateActorRef(SaveStateCtx* ctx, void* field);
#define ZMP_SAVESTATE_ACTOR_REF(field) Zmp_SaveStateActorRef(ctx, (void*)&(field));
// ZMP: a field ZMP added to the statics that travel (D-116; the mutation test "estaticas_de_jefe" leaves it out).
void Zmp_SaveStateNewField(SaveStateCtx* ctx, void* data, size_t len);
#define ZMP_SAVESTATE_NEW_FIELD(field) Zmp_SaveStateNewField(ctx, &(field), sizeof(field));
// ZMP: like SHIP_SAVESTATE_DEFINE, plus the fields ZMP added (NEW) and the actor pointers (REFS).
#define ZMP_SAVESTATE_DEFINE(Tag, FIELDS, NEW, REFS) \
    void Tag##_SaveState(SaveStateCtx* ctx) {        \
        FIELDS(SHIP_SAVESTATE_SERIALIZE_FIELD)       \
        NEW(ZMP_SAVESTATE_NEW_FIELD)                 \
        REFS(ZMP_SAVESTATE_ACTOR_REF)                \
    }
#define ZMP_SAVESTATE_NONE(F)

#ifdef __cplusplus
}
#endif
