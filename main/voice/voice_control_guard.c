#include "voice_control_guard.h"
#include <string.h>
#include <stdio.h>

cJSON *voice_control_parse(const char *data, size_t len)
{
    if (!data || !len || len > VOICE_CONTROL_MAX_BYTES || memchr(data, 0, len)) return NULL;
    unsigned depth = 0;
    bool quoted = false;
    for (size_t i = 0; i < len; ++i) {
        char c = data[i];
        if (quoted && c == '\\') {
            if (i + 5U < len && !memcmp(data + i, "\\u0000", 6)) return NULL;
            ++i;
        } else if (c == '"') quoted = !quoted;
        else if (!quoted && (c == '{' || c == '[')) {
            if (++depth > 8U) return NULL;
        } else if (!quoted && (c == '}' || c == ']')) {
            if (!depth) return NULL;
            --depth;
        }
    }
    if (quoted || depth) return NULL;
    char json[VOICE_CONTROL_MAX_BYTES + 1U];
    memcpy(json, data, len);
    json[len] = 0;
    cJSON *root=cJSON_ParseWithLengthOpts(json, len + 1U, NULL, true);
    if(cJSON_IsObject(root)) {
        const cJSON *a,*b;
        cJSON_ArrayForEach(a,root) for(b=a->next;b;b=b->next) {
            if(a->string && b->string && !strcmp(a->string,b->string)) {
                cJSON_Delete(root);return NULL;
            }
        }
    }
    return root;
}

static bool valid_id(const char *id)
{
    if (!id || !*id || strlen(id) >= VOICE_CONTROL_ID_BYTES) return false;
    for (const char *p = id; *p; ++p)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '_' || *p == '-' ||
              *p == '.' || *p == ':')) return false;
    return true;
}

const char *voice_control_guard_check(const voice_control_guard_t *guard,
    const cJSON *root, const char *device, const char *session, const char *interaction)
{
    if (!guard || !cJSON_IsObject(root)) return "invalid_envelope";
    const char *keys[] = {"type", "device_id", "session_id", "interaction_id", "request_id"};
    const char *values[5] = {0};
    unsigned counts[5] = {0};
    const cJSON *item;
    cJSON_ArrayForEach(item, root) {
        for (unsigned k = 0; k < 5; ++k) {
            if (item->string && !strcmp(item->string, keys[k])) {
                if (++counts[k] != 1 || !cJSON_IsString(item) ||
                    (!(k == 3 && item->valuestring[0] == 0) && !valid_id(item->valuestring)))
                    return "invalid_envelope";
                values[k] = item->valuestring;
            }
        }
    }
    for (unsigned k = 0; k < 5; ++k) if (!values[k]) return "invalid_envelope";
    if (!device || strcmp(device, values[1])) return "wrong_device";
    if (!session || !*session || strcmp(session, values[2])) return "stale_session";
    if (interaction && (!*interaction || strcmp(interaction, values[3]))) return "stale_interaction";
    return NULL;
}

bool voice_control_uint(const cJSON *root, const char *name, uint32_t *out)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(root, name);
    if (!cJSON_IsNumber(v) || !(v->valuedouble >= 0 && v->valuedouble <= UINT32_MAX)) return false;
    *out = (uint32_t)v->valuedouble;
    return v->valuedouble == (double)*out;
}

const char *voice_control_begin(voice_control_guard_t *g, uint32_t seq, const char *id)
{
    if (!g || !valid_id(id) || seq == 0) return "invalid_envelope";
    if (seq == g->interaction_seq && !strcmp(id, g->interaction_id))
        return g->active ? "duplicate_round" : "round_closed";
    if (g->interaction_seq == UINT32_MAX || seq != g->interaction_seq + 1U)
        return "stale_interaction";
    voice_control_guard_reset(g);
    g->interaction_seq = seq;
    strcpy(g->interaction_id, id);
    g->active = true;
    return NULL;
}

static bool fingerprint(const cJSON *root, char *out, size_t size)
{
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    const cJSON *business = cJSON_GetObjectItemCaseSensitive(root,
        cJSON_IsString(type) && !strcmp(type->valuestring,"command") ? "command" : "intent");
    const cJSON *expires = cJSON_GetObjectItemCaseSensitive(root,"expires_at_ms");
    if (!cJSON_IsString(type) || !cJSON_IsString(business) || strlen(business->valuestring)>139 ||
        !cJSON_IsNumber(expires) || !(expires->valuedouble>=0 && expires->valuedouble<=9007199254740991.0) ||
        expires->valuedouble != (double)(int64_t)expires->valuedouble) return false;
    /* Duplicate JSON keys are ambiguous even if the parser selects the first. */
    const cJSON *a, *b;
    cJSON_ArrayForEach(a,root) for (b=a->next;b;b=b->next)
        if (a->string && b->string && !strcmp(a->string,b->string)) return false;
    char *canonical=cJSON_PrintUnformatted(root);
    if (!canonical) return false;
    bool fits=strlen(canonical)<size;
    if(fits) strcpy(out,canonical);
    cJSON_free(canonical);
    return fits;
}

const char *voice_control_evaluate(const voice_control_guard_t *g, const cJSON *root,
    const char *device, const char *session, int64_t now_ms, const char **cached)
{
    *cached=NULL;
    const cJSON *type=cJSON_GetObjectItemCaseSensitive(root,"type");
    const cJSON *command=cJSON_GetObjectItemCaseSensitive(root,"command");
    bool file=cJSON_IsString(type) && !strcmp(type->valuestring,"command") &&
        cJSON_IsString(command) && !strncmp(command->valuestring,"FILE_SEND ",10);
    const char *error=voice_control_guard_check(g,root,device,session,file ? NULL : g->interaction_id);
    if (error) return error;
    if(strcmp(cJSON_GetObjectItemCaseSensitive(root,"interaction_id")->valuestring,g->interaction_id))
        return "stale_interaction";
    uint32_t round,seq;
    char fp[VOICE_CONTROL_MAX_BYTES+1U];
    if (!voice_control_uint(root,"interaction_seq",&round) ||
        !voice_control_uint(root,"control_seq",&seq) || !seq || !fingerprint(root,fp,sizeof(fp)))
        return "invalid_envelope";
    if (!g->active || round!=g->interaction_seq) return "stale_interaction";
    const char *id=cJSON_GetObjectItemCaseSensitive(root,"request_id")->valuestring;
    for(unsigned i=0;i<g->used;++i) {
        const voice_control_result_t *r=&g->results[i];
        if(r->sequence==seq || !strcmp(r->id,id)) {
            if(r->sequence!=seq || strcmp(r->id,id) || strcmp(r->fingerprint,fp)) return "request_id_conflict";
            *cached=r->response;
            return "duplicate_request";
        }
    }
    if(seq<=g->high_water) return "stale_request";
    if(g->high_water==UINT32_MAX || seq!=g->high_water+1U) return "out_of_order";
    double deadline=cJSON_GetObjectItemCaseSensitive(root,"expires_at_ms")->valuedouble;
    if(deadline < (double)now_ms) return "expired";
    if(deadline > (double)now_ms+10000.0) return "invalid_deadline";
    return NULL;
}

bool voice_control_record(voice_control_guard_t *g, const cJSON *root, const char *response)
{
    uint32_t seq;
    char fp[VOICE_CONTROL_MAX_BYTES+1U];
    if(!voice_control_uint(root,"control_seq",&seq) || !seq || g->high_water==UINT32_MAX ||
       seq!=g->high_water+1U || !fingerprint(root,fp,sizeof(fp)) || strlen(response)>=512U) return false;
    const cJSON *id=cJSON_GetObjectItemCaseSensitive(root,"request_id");
    if(!cJSON_IsString(id) || !valid_id(id->valuestring)) return false;
    voice_control_result_t *r=&g->results[g->next];
    strcpy(r->id,id->valuestring);strcpy(r->fingerprint,fp);strcpy(r->response,response);r->sequence=seq;
    g->high_water=seq;g->next=(g->next+1U)%VOICE_CONTROL_CACHE_SIZE;
    if(g->used<VOICE_CONTROL_CACHE_SIZE)++g->used;
    return true;
}

void voice_control_guard_reset(voice_control_guard_t *guard)
{
    if (guard) memset(guard, 0, sizeof(*guard));
}
