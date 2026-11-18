#ifndef SCREEN_TOUCH_PROFILE_H
#define SCREEN_TOUCH_PROFILE_H
#include "screen_calibration.h"
/* This board, ILI9341 portrait 240x320. Valid T02 mapping read over SWD
 * before reset on 2026-11-18. Hex floats preserve the six RAM words exactly:
 * 438364A5 C0B6EFE4 C16F079C C0ACD420 C3C130E3 43AF7EEE; valid=1.
 * Coordinates are normalized by 4095 in ScreenCal_Map. No EEPROM access.
 * Replace this profile when changing the panel or its orientation. */
static const ScreenCalibration screen_touch_profile = {
    .x = {0x1.06c94ap+8f, -0x1.6ddfc8p+2f, -0x1.de0f38p+3f},
    .y = {-0x1.59a84p+2f, -0x1.8261c6p+8f, 0x1.5efddcp+8f},
    .valid = true
};
#endif
