#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdint.h>
#include "cJSON.h"

#define VOICE_CONTROL_ID_BYTES 64U
#define VOICE_CONTROL_CACHE_SIZE 8U
#define VOICE_CONTROL_MAX_BYTES 768U
/* Bound parser stack use and reject embedded NUL before C-string comparisons. */
cJSON *voice_control_parse(const char *data, size_t len);
/* Owner-local. Eviction never lowers the sequence high water; old messages
 * cannot execute again after their detailed result has been reclaimed. */
typedef struct {
    char id[VOICE_CONTROL_ID_BYTES];
    char fingerprint[VOICE_CONTROL_MAX_BYTES + 1U];
    char response[512];
    uint32_t sequence;
} voice_control_result_t;
typedef struct {
    char interaction_id[VOICE_CONTROL_ID_BYTES];
    uint32_t interaction_seq;
    uint32_t high_water;
    bool active;
    unsigned used;
    unsigned next;
    voice_control_result_t results[VOICE_CONTROL_CACHE_SIZE];
} voice_control_guard_t;

/* Returns NULL for a valid new envelope, otherwise a non-sensitive diagnostic.
 * Pass interaction=NULL only for operations independent of the current dialog.
 * This checks identity only; command sequencing is enforced by evaluate(). */
const char *voice_control_guard_check(const voice_control_guard_t *guard,
    const cJSON *root, const char *device, const char *session, const char *interaction);
void voice_control_guard_reset(voice_control_guard_t *guard);
bool voice_control_uint(const cJSON *root, const char *name, uint32_t *out);
const char *voice_control_begin(voice_control_guard_t *guard, uint32_t seq, const char *id);
const char *voice_control_evaluate(const voice_control_guard_t *guard, const cJSON *root,
    const char *device, const char *session, int64_t now_ms, const char **cached);
bool voice_control_record(voice_control_guard_t *guard, const cJSON *root, const char *response);
