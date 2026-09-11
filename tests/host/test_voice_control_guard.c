#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "voice_control_guard.h"
static cJSON *message(unsigned round,unsigned seq,const char *iid){
 cJSON *j=cJSON_CreateObject();char id[64];snprintf(id,sizeof(id),"request-%u-%u",round,seq);
 cJSON_AddStringToObject(j,"type","intent_result");cJSON_AddStringToObject(j,"intent","normal");
 cJSON_AddStringToObject(j,"device_id","device-A");cJSON_AddStringToObject(j,"session_id","session-A");
 cJSON_AddStringToObject(j,"interaction_id",iid);cJSON_AddStringToObject(j,"request_id",id);
 cJSON_AddNumberToObject(j,"interaction_seq",round);cJSON_AddNumberToObject(j,"control_seq",seq);
 cJSON_AddNumberToObject(j,"expires_at_ms",10000);return j;
}
static const char *check(voice_control_guard_t *g,cJSON *j,int64_t now,const char **cached){
 return voice_control_evaluate(g,j,"device-A","session-A",now,cached);
}
int main(void){
 const char *deep="[[[[[[[[[0]]]]]]]]]",*nul="{\"type\":\"a\\u0000b\"}";
 assert(!voice_control_parse(deep,strlen(deep)));assert(!voice_control_parse(nul,strlen(nul)));
 assert(!voice_control_parse("{}{}",4));
 const char *duplicate="{\"type\":\"a\",\"type\":\"b\"}";
 assert(!voice_control_parse(duplicate,strlen(duplicate)));
 voice_control_guard_t a={0},b={0};const char *cached=NULL;
 assert(!voice_control_begin(&a,1,"turn-1"));
 cJSON *j=message(1,1,"turn-1");
 assert(!check(&a,j,0,&cached));assert(voice_control_evaluate(&b,j,"device-A","session-A",0,&cached)!=NULL);
 assert(!strcmp(voice_control_evaluate(&a,j,"device-B","session-A",0,&cached),"wrong_device"));
 assert(!strcmp(voice_control_evaluate(&a,j,"device-A","session-B",0,&cached),"stale_session"));
 assert(voice_control_record(&a,j,"original-result"));
 assert(!strcmp(check(&a,j,20000,&cached),"duplicate_request") && !strcmp(cached,"original-result"));
 cJSON_ReplaceItemInObjectCaseSensitive(j,"intent",cJSON_CreateString("goodnight"));
 assert(!strcmp(check(&a,j,0,&cached),"request_id_conflict"));cJSON_Delete(j);
 j=message(1,3,"turn-1");assert(!strcmp(check(&a,j,0,&cached),"out_of_order"));cJSON_Delete(j);
 for(unsigned seq=2;seq<=100;++seq){j=message(1,seq,"turn-1");assert(!check(&a,j,1,&cached));assert(voice_control_record(&a,j,"applied"));cJSON_Delete(j);}
 assert(a.used==VOICE_CONTROL_CACHE_SIZE && a.high_water==100);
 j=message(1,1,"turn-1");assert(!strcmp(check(&a,j,0,&cached),"stale_request"));cJSON_Delete(j);
 j=message(1,101,"turn-1");assert(!strcmp(check(&a,j,10001,&cached),"expired"));
 assert(voice_control_record(&a,j,"expired-result"));assert(!strcmp(check(&a,j,10002,&cached),"duplicate_request"));cJSON_Delete(j);
 assert(!voice_control_begin(&a,2,"turn-2"));assert(a.used==0 && a.high_water==0);
 j=message(1,102,"turn-1");assert(!strcmp(check(&a,j,1,&cached),"stale_interaction"));cJSON_Delete(j);
 assert(!strcmp(voice_control_begin(&a,1,"turn-1"),"stale_interaction"));
 assert(!strcmp(voice_control_begin(&a,2,"turn-2"),"duplicate_round"));
 a.active=false;assert(!strcmp(voice_control_begin(&a,2,"turn-2"),"round_closed"));
 for(unsigned round=3;round<=203;++round){
  char iid[64];snprintf(iid,sizeof(iid),"turn-%u",round);assert(!voice_control_begin(&a,round,iid));
  for(unsigned seq=1;seq<=50;++seq){j=message(round,seq,iid);assert(!check(&a,j,0,&cached));assert(voice_control_record(&a,j,"applied"));cJSON_Delete(j);}
  assert(a.used==VOICE_CONTROL_CACHE_SIZE && a.high_water==50);
 }
 j=message(203,51,"turn-203");cJSON_AddStringToObject(j,"device_id","device-B");
 assert(!strcmp(check(&a,j,0,&cached),"invalid_envelope"));cJSON_Delete(j);
 puts("PASS: bounded result cache, 200+ rounds, 10000+ controls, sequence eviction safety, expiry, conflict and device/session isolation");
 return 0;
}
