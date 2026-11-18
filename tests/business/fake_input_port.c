#include "fake_input_port.h"
#include <string.h>
InputEdges fake_edges;
uint32_t fake_now, fake_faults, fake_restarts;
bool fake_restart_ok = true;
bool InputPort_Init(void) { return true; }
bool InputPort_Restart(void)
{
    ++fake_restarts;
    if (!fake_restart_ok) return false;
    fake_faults = 0U;
    memset(&fake_edges, 0, sizeof(fake_edges));
    return true;
}
void InputPort_Stop(void) {}
void InputPort_Service(void) {}
void InputPort_Read(InputEdges *out) { *out = fake_edges; }
uint32_t InputPort_Faults(void) { return fake_faults; }
uint32_t InputPort_Now(void) { return fake_now; }
void Fake_Period(uint32_t start, uint32_t period, uint32_t high, uint32_t end_ms)
{
    fake_edges = (InputEdges){.rising = {start + period, start},
        .falling = {start + high}, .timer_now = start + period,
        .observed_ms = end_ms, .rising_count = 2U, .falling_count = 1U};
    fake_now = end_ms;
}
