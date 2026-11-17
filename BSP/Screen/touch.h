#ifndef SCREEN_TOUCH_H
#define SCREEN_TOUCH_H
#include <stdbool.h>
#include <stdint.h>
typedef struct {
    bool pressed;
    bool valid;
    uint16_t raw_x, raw_y;
} ScreenTouchSample;
typedef struct {
    uint32_t polls, pen_low_samples, pen_edges, idle_probes;
    uint32_t probe_x, probe_y, valid_samples, rejected_samples;
} ScreenTouchDiagnostics;
extern volatile ScreenTouchDiagnostics g_touch_diag;
void ScreenTouch_Init(void);
ScreenTouchSample ScreenTouch_Read(void);
#endif
