#include "fake_sweep_business.h"
#include "sweep_internal.h"
#include <string.h>

uint32_t fake_now, fake_writes, fake_b_calls, fake_next_id;
uint32_t fake_bad_frequency, fake_reject_frequency;
bool fake_stall, fake_stall_restore, fake_bad_id, fake_reject_enable, fake_reject_release;
unsigned fake_capture;
WaveSnapshot fake_a, fake_b;
WaveSyncSnapshot fake_sync;
BizMode fake_wave_mode, fake_input_mode;
bool fake_input_ready;
static InputSnapshot input;
static WaveParams original, pending_params;
static bool pending_enable, enable_operation;
static uint32_t operation_ms, delay_ms, window_ms;
static void capture(void);
static uint32_t period_ms(uint32_t hz) { return 1U + 999U/hz; }
static void state(WaveSnapshot *w)
{
    w->state = !w->enabled ? WAVE_STOPPED : !w->applied.duty_bp ? WAVE_DC_LOW :
        w->applied.duty_bp == 10000U ? WAVE_DC_HIGH : WAVE_PWM;
}
void Fake_Reset(bool enabled, uint32_t hz, uint16_t duty)
{
    fake_now = 100U;
    fake_writes = fake_b_calls = fake_next_id = fake_bad_frequency = fake_reject_frequency = 0U;
    fake_stall = fake_stall_restore = fake_bad_id = fake_reject_enable = fake_reject_release = false;
    fake_capture = 0U;
    fake_wave_mode = fake_input_mode = BIZ_MODE_STANDARD;
    fake_input_ready = true;
    fake_sync = (WaveSyncSnapshot){.state = WAVE_SYNC_OFF};
    original = (WaveParams){hz,duty};
    fake_a = (WaveSnapshot){.applied = original, .requested = original,
        .owner = WAVE_OWNER_MANUAL, .enabled = enabled};
    fake_b = (WaveSnapshot){.applied = {1234U,3300U}, .enabled = true, .state = WAVE_PWM};
    state(&fake_a);
    input = (InputSnapshot){.state = INPUT_WAITING};
}
uint32_t SweepPort_Now(void) { return fake_now; }
void Fake_Tick(uint32_t delta)
{
    fake_now += delta;
    if (fake_a.pending && !fake_stall &&
        !(fake_stall_restore && !enable_operation && pending_params.frequency_hz == original.frequency_hz) &&
        fake_now - operation_ms >= delay_ms) {
        if (enable_operation) fake_a.enabled = pending_enable;
        else fake_a.applied = pending_params;
        fake_a.pending = false;
        fake_a.applied_id = fake_bad_id ? fake_a.request_id + 1U : fake_a.request_id;
        fake_a.applied_ms = fake_now;
        fake_a.last_result = BIZ_OK;
        state(&fake_a);
    }
    capture();
}
void Wave_GetSnapshot(WaveChannel c, WaveSnapshot *out)
{
    if (c == WAVE_CHANNEL_B) ++fake_b_calls;
    *out = c == WAVE_CHANNEL_A ? fake_a : fake_b;
}
void Wave_GetSyncSnapshot(WaveSyncSnapshot *out) { *out = fake_sync; }
BizMode Wave_GetMode(void) { return fake_wave_mode; }
BizMode Input_GetMode(void) { return fake_input_mode; }
BizResult Wave_AcquireA(WaveOwner owner)
{
    if (owner != WAVE_OWNER_SELFTEST || fake_a.owner != WAVE_OWNER_MANUAL || fake_a.pending || fake_sync.state != WAVE_SYNC_OFF)
        return BIZ_BUSY;
    fake_a.owner = owner;
    return BIZ_OK;
}
BizResult Wave_ReleaseA(WaveOwner owner)
{
    if (fake_reject_release) return BIZ_HARDWARE_ERROR;
    if (owner != fake_a.owner || fake_a.pending) return BIZ_BUSY;
    fake_a.owner = WAVE_OWNER_MANUAL;
    return BIZ_OK;
}
static BizResult accept(uint32_t *id)
{
    ++fake_writes;
    fake_a.request_id = ++fake_next_id;
    *id = fake_a.request_id;
    fake_a.accepted_ms = operation_ms = fake_now;
    delay_ms = period_ms(fake_a.applied.frequency_hz);
    fake_a.pending = true;
    fake_a.last_result = BIZ_PENDING;
    return BIZ_PENDING;
}
BizResult Wave_Apply(WaveChannel c, WaveOwner owner, const WaveParams *p, uint32_t *id)
{
    if (c != WAVE_CHANNEL_A) { ++fake_b_calls; return BIZ_INVALID_ARGUMENT; }
    if (owner != fake_a.owner || fake_a.pending) return BIZ_BUSY;
    if (p->frequency_hz == fake_reject_frequency) return BIZ_HARDWARE_ERROR;
    pending_params = *p;
    fake_a.requested = *p;
    enable_operation = false;
    BizResult r = accept(id);
    if (!fake_a.enabled) { delay_ms = 0U; Fake_Tick(0U); r = BIZ_OK; }
    return r;
}
BizResult Wave_SetEnabled(WaveChannel c, WaveOwner owner, bool enabled, uint32_t *id)
{
    if (c != WAVE_CHANNEL_A) { ++fake_b_calls; return BIZ_INVALID_ARGUMENT; }
    if (owner != fake_a.owner) return BIZ_BUSY;
    if (fake_reject_enable) return BIZ_HARDWARE_ERROR;
    if (enabled && fake_a.pending) return BIZ_BUSY;
    if (!enabled && fake_a.pending) { fake_a.cancelled_id = fake_a.request_id; fake_a.pending = false; }
    pending_enable = enabled;
    enable_operation = true;
    BizResult r = accept(id);
    if (!enabled) { delay_ms = 0U; bool stalled=fake_stall; fake_stall=false; Fake_Tick(0U); fake_stall=stalled; r=BIZ_OK; }
    return r;
}
BizResult Input_BeginWindow(uint32_t *id)
{
    input.acquisition_id = input.acquisition_id + 1U;
    *id = input.acquisition_id;
    input.state = INPUT_WAITING;
    window_ms = fake_now;
    return BIZ_OK;
}
void Input_GetSnapshot(InputSnapshot *out)
{
    *out = input;
    out->mode = fake_input_mode;
    if (!fake_input_ready) { out->state=INPUT_ERROR; out->last_result=BIZ_NOT_INITIALIZED; return; }
}
static void capture(void)
{
    if (!input.acquisition_id || input.state!=INPUT_WAITING || fake_now-window_ms < 2U*period_ms(fake_a.applied.frequency_hz)) return;
    unsigned mode = fake_bad_frequency && fake_bad_frequency != fake_a.applied.frequency_hz ? 0U : fake_capture;
    if (mode == 1U) return;
    InputSnapshot candidate=input;
    InputSnapshot *out=&candidate;
    out->state = mode == 3U ? INPUT_ERROR : mode == 7U ? INPUT_OUT_OF_RANGE : mode == 8U ? INPUT_NO_SIGNAL : INPUT_VALID;
    if (mode != 6U) out->sequence = input.sequence + 1U;
    if (mode == 4U) --out->acquisition_id;
    out->frequency_millihz = fake_a.applied.frequency_hz * 1000U;
    if (mode == 2U) out->frequency_millihz += out->frequency_millihz / 50U;
    if (mode == 9U || mode == 10U) out->frequency_millihz += out->frequency_millihz/100U + (mode==10U ? 1U : 0U);
    out->duty_bp = 5000U;
    out->window_start_ms = window_ms;
    out->window_end_ms = mode == 5U ? fake_now-1000U : fake_now;
    out->published_ms = fake_now;
    input=candidate;
}
