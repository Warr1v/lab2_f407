#ifndef SCREEN_CALIBRATION_H
#define SCREEN_CALIBRATION_H
#include <stdbool.h>
#include <stdint.h>
enum { SCREEN_CAL_POINTS = 5 };
typedef struct { uint16_t raw_x, raw_y; int16_t x, y; } ScreenCalPoint;
typedef struct { float x[3], y[3]; bool valid; } ScreenCalibration;
/* Fit is transactional: rejected data leave the previous calibration unchanged. */
bool ScreenCal_Fit(ScreenCalibration *cal, const ScreenCalPoint points[SCREEN_CAL_POINTS]);
bool ScreenCal_Map(const ScreenCalibration *cal, uint16_t raw_x, uint16_t raw_y,
                   int16_t width, int16_t height, int16_t *x, int16_t *y);
#endif
