#ifndef SCREEN_MOCK_H
#define SCREEN_MOCK_H
#include <stdbool.h>
#include <stdint.h>
enum { SCREEN_MOCK_RECORDS = 6 };
typedef struct { uint32_t ms; uint16_t number; char kind; } ScreenMockRecord;
typedef struct {
    uint32_t frequency_hz;
    uint8_t duty_percent, input_state;
    bool output_enabled, stopwatch_running;
    uint32_t elapsed_ms, started_ms, segment_base_ms;
    ScreenMockRecord records[SCREEN_MOCK_RECORDS];
    uint8_t record_count;
    uint16_t record_serial;
} ScreenMock;
void ScreenMock_Init(ScreenMock *m);
uint32_t ScreenMock_Time(const ScreenMock *m, uint32_t now);
void ScreenMock_ToggleTimer(ScreenMock *m, uint32_t now);
void ScreenMock_ResetTimer(ScreenMock *m, uint32_t now);
void ScreenMock_Record(ScreenMock *m, uint32_t now, bool segment);
#endif
