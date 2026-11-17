#include "screen_mock.h"
#include <string.h>
/* UI demonstration only. No PWM/capture/timebase peripheral is configured. */
void ScreenMock_Init(ScreenMock *m)
{
    memset(m, 0, sizeof(*m));
    m->frequency_hz = 1000; m->duty_percent = 50;
}
uint32_t ScreenMock_Time(const ScreenMock *m, uint32_t now)
{
    return m->elapsed_ms + (m->stopwatch_running ? (uint32_t)(now - m->started_ms) : 0);
}
void ScreenMock_ToggleTimer(ScreenMock *m, uint32_t now)
{
    if (m->stopwatch_running) m->elapsed_ms = ScreenMock_Time(m, now);
    else m->started_ms = now;
    m->stopwatch_running = !m->stopwatch_running;
}
void ScreenMock_ResetTimer(ScreenMock *m, uint32_t now)
{
    m->elapsed_ms = 0; m->started_ms = now; m->segment_base_ms = 0;
    m->stopwatch_running = false; m->record_count = 0; m->record_serial = 0;
}
void ScreenMock_Record(ScreenMock *m, uint32_t now, bool segment)
{
    if (!m->stopwatch_running) return;
    const uint32_t total = ScreenMock_Time(m, now);
    if (m->record_count == SCREEN_MOCK_RECORDS) {
        memmove(m->records, m->records + 1, sizeof(m->records[0]) * (SCREEN_MOCK_RECORDS - 1));
        --m->record_count;
    }
    ScreenMockRecord *r = &m->records[m->record_count++];
    r->number = ++m->record_serial;
    r->kind = segment ? 'S' : 'L';
    r->ms = segment ? total - m->segment_base_ms : total;
    if (segment) m->segment_base_ms = total;
}
