#include "wifi_profiles.h"
#include <string.h>
static bool credential(const char *ssid,const char *password)
{
    if(!ssid || !password)return false;
    size_t sn=strnlen(ssid,33),pn=strnlen(password,65);
    if(sn==0 || sn>32 || pn>64 || (pn>0 && pn<8))return false;
    for(size_t i=0;i<sn;++i)if((unsigned char)ssid[i]<32)return false;
    if(pn==64)for(size_t i=0;i<pn;++i)if(!((password[i]>='a' && password[i]<='f') || (password[i]>='A' && password[i]<='F') || (password[i]>='0' && password[i]<='9')))return false;
    return true;
}
int pet_wifi_find(const pet_wifi_profiles_t *list,const char *ssid)
{
    for(unsigned i=0;i<list->count;++i)if(strcmp(list->profiles[i].ssid,ssid)==0)return (int)i;
    return -1;
}
bool pet_wifi_valid(const pet_wifi_profiles_t *list)
{
    if(!list || list->version!=1 || list->count>PET_WIFI_MAX)return false;
    for(unsigned i=0;i<list->count;++i){
        if(!credential(list->profiles[i].ssid,list->profiles[i].password))return false;
        for(unsigned j=0;j<i;++j)if(strcmp(list->profiles[j].ssid,list->profiles[i].ssid)==0)return false;
    }
    return true;
}
bool pet_wifi_put(pet_wifi_profiles_t *list,const char *ssid,const char *password)
{
    if(!pet_wifi_valid(list) || !credential(ssid,password))return false;
    int index=pet_wifi_find(list,ssid);
    if(index<0){if(list->count==PET_WIFI_MAX)return false;index=(int)list->count++;}
    strcpy(list->profiles[index].ssid,ssid);strcpy(list->profiles[index].password,password);
    return true;
}
bool pet_wifi_remove(pet_wifi_profiles_t *list,unsigned index)
{
    if(!pet_wifi_valid(list) || index>=list->count)return false;
    memmove(&list->profiles[index],&list->profiles[index+1],(list->count-index-1)*sizeof(list->profiles[0]));
    memset(&list->profiles[--list->count],0,sizeof(list->profiles[0]));return true;
}
bool pet_wifi_promote(pet_wifi_profiles_t *list,unsigned index)
{
    if(!pet_wifi_valid(list) || index>=list->count)return false;
    struct pet_wifi_profile saved=list->profiles[index];
    memmove(&list->profiles[1],&list->profiles[0],index*sizeof(saved));list->profiles[0]=saved;return true;
}
int pet_wifi_next(const pet_wifi_profiles_t *list,unsigned visible,unsigned tried)
{
    if(!pet_wifi_valid(list))return -1;
    for(unsigned i=0;i<list->count;++i)if((visible&(1U<<i)) && !(tried&(1U<<i)))return (int)i;
    for(unsigned i=0;i<list->count;++i)if(!(tried&(1U<<i)))return (int)i;
    return -1;
}
