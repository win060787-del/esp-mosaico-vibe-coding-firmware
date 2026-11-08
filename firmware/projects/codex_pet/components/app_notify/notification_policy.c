#include "notification_policy.h"
#include <string.h>
bool notification_policy_is_new(const notification_policy_t *p,const char *key,uint32_t at)
{
    if(!p || !p->armed || !key || strlen(key)!=64 || at<=p->baseline)return false;
    for(unsigned i=0;i<64;++i)if(!((key[i]>='0' && key[i]<='9') || (key[i]>='a' && key[i]<='f')))return false;
    for(unsigned i=0;i<NOTIFICATION_SEEN;++i)if(strcmp(p->seen[i],key)==0)return false;
    return true;
}
void notification_policy_record(notification_policy_t *p,const char *key)
{
    if(!p || !key || strlen(key)!=64)return;
    memcpy(p->seen[p->next],key,65);p->next=(p->next+1)%NOTIFICATION_SEEN;
}
