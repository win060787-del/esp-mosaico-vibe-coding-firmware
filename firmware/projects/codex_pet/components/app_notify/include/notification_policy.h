#pragma once
#include <stdbool.h>
#include <stdint.h>
#define NOTIFICATION_KEY_BYTES 65
#define NOTIFICATION_SEEN 16
/* Boot-lifetime policy. Initial server snapshot establishes a silent baseline. */
typedef struct {
    bool armed;
    uint32_t baseline;
    unsigned next;
    char seen[NOTIFICATION_SEEN][NOTIFICATION_KEY_BYTES];
} notification_policy_t;
bool notification_policy_is_new(const notification_policy_t *policy,const char *key,uint32_t at);
void notification_policy_record(notification_policy_t *policy,const char *key);
