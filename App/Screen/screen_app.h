#ifndef SCREEN_APP_H
#define SCREEN_APP_H
#include <stdbool.h>
#include <stdint.h>
void ScreenApp_Init(void);
void ScreenApp_Process(uint32_t now_ms);
/* Wave/Input mode coordinated by the UI; main-loop access only. */
bool ScreenApp_ExtensionsEnabled(void);
/* Inspectable through SWD; real screen/touch validation is performed by the user. */
typedef struct {
    uint32_t magic, stage, heartbeat, lcd_id, page, calibration_result;
    uint32_t raw_x, raw_y, pen_down, sample_valid, x, y, touch_count;
    uint32_t selftest, max_scan_us, max_service_us;
} ScreenDiagnostics;
extern volatile ScreenDiagnostics g_screen_diag;
typedef enum {
    SCREEN_SLIDER_IDLE, SCREEN_SLIDER_PRESSED, SCREEN_SLIDER_DRAGGING,
    SCREEN_SLIDER_RELEASED, SCREEN_SLIDER_CANCELLED
} ScreenSliderPhase;
typedef struct {
    uint32_t phase, value_bp, presses, moves, releases, cancellations;
} ScreenSliderDiagnostics;
extern volatile ScreenSliderDiagnostics g_screen_slider_diag;
#endif
