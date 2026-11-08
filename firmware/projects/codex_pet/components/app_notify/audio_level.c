#include "audio_level.h"
int16_t notification_scale_sample(int16_t sample,unsigned volume)
{
    return (int16_t)((int32_t)sample*(int32_t)volume*(int32_t)volume/20000);
}
