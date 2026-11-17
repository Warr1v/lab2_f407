#include "screen_calibration.h"
#include <string.h>

/* Executed on the MCU before LCD initialization; bitmask must be 0x7f. */
uint32_t ScreenCal_SelfTest(void)
{
    const int16_t targets[5][2] = {{24,48},{215,48},{215,240},{24,240},{120,144}};
    ScreenCalPoint p[5];
    ScreenCalibration c = {0};
    uint32_t passed = 0;
    for (unsigned i = 0; i < 5; ++i) {
        p[i] = (ScreenCalPoint){(uint16_t)(400 + 12 * targets[i][0]),
            (uint16_t)(450 + 11 * targets[i][1]), targets[i][0], targets[i][1]};
    }
    if (ScreenCal_Fit(&c, p)) passed |= 1;
    int16_t x = -1, y = -1;
    if (ScreenCal_Map(&c,1840,2034,240,320,&x,&y) && x == 120 && y == 144) passed |= 2;
    for (unsigned i = 0; i < 5; ++i) {
        p[i].raw_x = (uint16_t)(3900 - 11 * targets[i][1]);
        p[i].raw_y = (uint16_t)(300 + 12 * targets[i][0]);
    }
    if (ScreenCal_Fit(&c,p) && ScreenCal_Map(&c,2316,1740,240,320,&x,&y)
        && x == 120 && y == 144) passed |= 4;
    const ScreenCalibration previous = c;
    for (unsigned i = 0; i < 5; ++i) p[i].raw_x = p[i].raw_y = 1000;
    if (!ScreenCal_Fit(&c,p) && memcmp(&c,&previous,sizeof(c)) == 0) passed |= 8;
    for (unsigned i = 0; i < 5; ++i) {
        p[i].raw_x = (uint16_t)(400 + 12 * targets[i][0]);
        p[i].raw_y = (uint16_t)(450 + 11 * targets[i][1]);
    }
    p[4].raw_x = 3000;
    if (!ScreenCal_Fit(&c,p) && memcmp(&c,&previous,sizeof(c)) == 0) passed |= 16;
    ScreenCalibration invalid = {0};
    if (!ScreenCal_Map(&invalid,1000,1000,240,320,&x,&y)) passed |= 32;
    /* Real boundary fixture with an easy known affine relation. */
    c = (ScreenCalibration){.x={409.5f,0,-40}, .y={0,409.5f,-40}, .valid=true};
    if (ScreenCal_Map(&c,400,400,240,320,&x,&y) && x == 0 && y == 0
        && !ScreenCal_Map(&c,4095,4095,240,320,&x,&y)) passed |= 64;
    return passed;
}
