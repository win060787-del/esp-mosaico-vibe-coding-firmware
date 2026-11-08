#pragma once
#include <stdbool.h>
#include <stdint.h>
#define PET_WIFI_MAX 8
struct pet_wifi_profile {char ssid[33];char password[65];};
typedef struct {uint32_t version,count;struct pet_wifi_profile profiles[PET_WIFI_MAX];} pet_wifi_profiles_t;
bool pet_wifi_valid(const pet_wifi_profiles_t *list);
int pet_wifi_find(const pet_wifi_profiles_t *list,const char *ssid);
bool pet_wifi_put(pet_wifi_profiles_t *list,const char *ssid,const char *password);
bool pet_wifi_remove(pet_wifi_profiles_t *list,unsigned index);
bool pet_wifi_promote(pet_wifi_profiles_t *list,unsigned index);
int pet_wifi_next(const pet_wifi_profiles_t *list,unsigned visible,unsigned tried);
