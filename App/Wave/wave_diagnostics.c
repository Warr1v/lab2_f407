#include "wave_diagnostics.h"
#include "wave_config.h"
#include "stm32f4xx_hal.h"

volatile WaveTestCommand g_wave_test_command;
volatile WaveTestDiagnostic g_wave_diag;
const WaveTestCase g_wave_test_cases[] = {
    {WAVE_CHANNEL_A, {1U, 5000U}}, {WAVE_CHANNEL_A, {200000U, 100U}},
    {WAVE_CHANNEL_A, {1U, 10000U}}, {WAVE_CHANNEL_A, {200000U, 0U}},
    {WAVE_CHANNEL_A, {199999U, 9900U}}, {WAVE_CHANNEL_A, {2563U, 4900U}},
    {WAVE_CHANNEL_A, {2564U, 5100U}}, {WAVE_CHANNEL_A, {1000U, 5000U}},
    {WAVE_CHANNEL_B, {1753U, 2500U}}, {WAVE_CHANNEL_B, {200000U, 9900U}},
    {WAVE_CHANNEL_A, {250000U, 5000U}}, {WAVE_CHANNEL_B, {400000U, 9500U}},
    {WAVE_CHANNEL_A, {500000U, 5000U}}, {WAVE_CHANNEL_B, {500000U, 5000U}},
    {WAVE_CHANNEL_A, {500000U, 0U}}, {WAVE_CHANNEL_B, {500000U, 10000U}}
};
const uint32_t g_wave_test_case_count = sizeof(g_wave_test_cases) / sizeof(g_wave_test_cases[0]);
#if WAVE_DIAGNOSTIC_ENABLE
static uint32_t case_request, case_timeout;
static uint8_t case_step;

static BizResult command(uint32_t now)
{
    const WaveTestCommand c = g_wave_test_command;
    if (g_wave_diag.case_active && c.operation != WAVE_TEST_ABORT) return BIZ_BUSY;
    uint32_t id = 0;
    BizResult result;
    g_wave_diag.counter_before = c.channel == WAVE_CHANNEL_B ? TIM8->CNT : TIM1->CNT;
    g_wave_diag.command_cycle = DWT->CYCCNT; /* Command-entry sample, not an edge timestamp. */
    switch (c.operation) {
    case WAVE_TEST_APPLY: result = Wave_Apply((WaveChannel)c.channel, WAVE_OWNER_MANUAL, &c.params, &id); break;
    case WAVE_TEST_ENABLE: result = Wave_SetEnabled((WaveChannel)c.channel, WAVE_OWNER_MANUAL, c.value != 0U, &id); break;
    case WAVE_TEST_MODE: result = Wave_SetMode((BizMode)c.value); break;
    case WAVE_TEST_SYNC: result = Wave_SetSync(c.value != 0U); break;
    case WAVE_TEST_PHASE: result = c.value > 360U ? BIZ_OUT_OF_RANGE : Wave_SetPhase((uint16_t)c.value); break;
    case WAVE_TEST_SPWM_START: result = Spwm_Start(); break;
    case WAVE_TEST_SPWM_STOP: result = Spwm_Stop(); break;
    case WAVE_TEST_ACQUIRE_A: result = Wave_AcquireA(WAVE_OWNER_SELFTEST); break;
    case WAVE_TEST_RELEASE_A: result = Wave_ReleaseA(WAVE_OWNER_SELFTEST); break;
    case WAVE_TEST_CASE:
        if (g_wave_diag.case_active) return BIZ_BUSY;
        if (c.value >= g_wave_test_case_count) return BIZ_OUT_OF_RANGE;
        g_wave_diag.case_index = c.value;
        g_wave_diag.case_started_ms = now;
        g_wave_diag.case_active = true;
        g_wave_diag.case_result = BIZ_PENDING;
        case_step = 0;
        result = BIZ_PENDING;
        break;
    case WAVE_TEST_ABORT:
        g_wave_diag.case_active = false;
        g_wave_diag.case_result = BIZ_CANCELLED;
        /* Does not steal SELFTEST ownership or stop the other channel. */
        result = Wave_SetEnabled((WaveChannel)c.channel, WAVE_OWNER_MANUAL, false, &id);
        break;
    default: result = BIZ_INVALID_ARGUMENT; break;
    }
    g_wave_diag.request_id = id;
    return result;
}
static void case_process(uint32_t now)
{
    if (!g_wave_diag.case_active) return;
    const WaveTestCase *c = &g_wave_test_cases[g_wave_diag.case_index];
    WaveSnapshot s;
    Wave_GetSnapshot(c->channel, &s);
    BizResult result = BIZ_PENDING;
    if (s.state == WAVE_UNINITIALIZED) result = BIZ_NOT_INITIALIZED;
    else if (case_step == 0U) {
        case_timeout = (1000U + s.applied.frequency_hz - 1U) / s.applied.frequency_hz + 100U;
        g_wave_diag.case_started_ms = now;
        result = Wave_Apply(c->channel, WAVE_OWNER_MANUAL, &c->params, &case_request);
        if (result == BIZ_OK || result == BIZ_PENDING) { case_step = 1; result = BIZ_PENDING; }
    } else if (s.state == WAVE_FAULT) result = BIZ_HARDWARE_ERROR;
    else if (s.cancelled_id == case_request && s.applied_id != case_request) result = BIZ_CANCELLED;
    else if (s.applied_id == case_request && !s.pending) {
        if (case_step == 1U) {
            result = Wave_SetEnabled(c->channel, WAVE_OWNER_MANUAL, true, &case_request);
            if (result == BIZ_OK || result == BIZ_PENDING) { case_step = 2; result = BIZ_PENDING; }
        } else result = BIZ_OK;
    } else if ((uint32_t)(now - g_wave_diag.case_started_ms) > case_timeout) result = BIZ_TIMEOUT;
    if (result != BIZ_PENDING) {
        g_wave_diag.case_result = result;
        g_wave_diag.case_active = false;
    }
    g_wave_diag.request_id = case_request;
}
#endif

void WaveTest_Process(uint32_t now_ms)
{
    g_wave_diag.magic = 0x57564431U;
    g_wave_diag.api_version = LAB2_BUSINESS_API_VERSION;
    ++g_wave_diag.heartbeat;
    g_wave_diag.commands_enabled = WAVE_DIAGNOSTIC_ENABLE != 0;
#if WAVE_DIAGNOSTIC_ENABLE
    const uint32_t sequence = g_wave_test_command.sequence;
    if (sequence != g_wave_diag.acknowledged_sequence) {
        __DMB();
        g_wave_diag.command_result = command(now_ms);
        g_wave_diag.acknowledged_sequence = sequence;
    }
    case_process(now_ms);
#else
    (void)now_ms;
    g_wave_diag.command_result = BIZ_UNSUPPORTED;
#endif
    WaveSnapshot a, b;
    WaveSyncSnapshot sync;
    SpwmSnapshot spwm;
    Wave_GetSnapshot(WAVE_CHANNEL_A, &a);
    Wave_GetSnapshot(WAVE_CHANNEL_B, &b);
    Wave_GetSyncSnapshot(&sync);
    Spwm_GetSnapshot(&spwm);
    g_wave_diag.a = a;
    g_wave_diag.b = b;
    g_wave_diag.sync = sync;
    g_wave_diag.spwm = spwm;
}
