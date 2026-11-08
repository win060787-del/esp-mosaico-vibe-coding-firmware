#pragma once
#include <stdint.h>
/* Volume is a UI level 0..100. Half-scale ceiling and quadratic taper keep
 * this small speaker gentle. 50 => one eighth of the source PCM amplitude. */
int16_t notification_scale_sample(int16_t sample,unsigned volume);
