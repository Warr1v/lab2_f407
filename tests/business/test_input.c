#include "business_api.h"
#include "fake_input_port.h"

volatile uint32_t test_checks, test_failure;
#define CHECK(x) do { ++test_checks; if (!(x)) { test_failure = __LINE__; return 1; } } while (0)
int main(void)
{
    InputSnapshot s, previous;
    uint32_t id;
    CHECK(Input_BeginWindow(&id) == BIZ_NOT_INITIALIZED);
    CHECK(Input_Init() == BIZ_OK);
    CHECK(Input_Init() == BIZ_BUSY);
    CHECK(Input_BeginWindow(0) == BIZ_INVALID_ARGUMENT);
    static const uint32_t hz[] = {1,2,10,100,1000,10000,100000,200000};
    static const uint32_t bp[] = {100,1000,5000,9000,9900};
    uint32_t timer = 0xFE000000U;
    for (unsigned i = 0; i < sizeof(hz)/sizeof(hz[0]); ++i) {
        for (unsigned j = 0; j < sizeof(bp)/sizeof(bp[0]); ++j) {
            CHECK(Input_BeginWindow(&id) == BIZ_OK);
            uint32_t period = INPUT_TIMER_HZ / hz[i];
            uint32_t high = (uint32_t)(((uint64_t)period * bp[j] + 5000U) / 10000U);
            Fake_Period(timer, period, high, fake_now + 1000U);
            timer += period;
            Input_Process(fake_now);
            Input_GetSnapshot(&s);
            CHECK(s.state == INPUT_VALID && s.acquisition_id == id);
            CHECK(s.frequency_millihz == hz[i] * 1000U);
            CHECK(s.period_ticks == period && s.high_ticks == high);
            int error = (int)s.duty_bp - (int)bp[j];
            CHECK(error >= -200 && error <= 200);
            previous = s;
            Input_GetSnapshot(&s);
            CHECK(s.sequence == previous.sequence && s.published_ms == previous.published_ms);
            Input_Process(fake_now);
            Input_GetSnapshot(&s);
            CHECK(s.sequence == previous.sequence);
        }
    }
    /* Old queued data and a first half-period cannot survive BeginWindow. */
    CHECK(Input_BeginWindow(&id) == BIZ_OK);
    Input_GetSnapshot(&s);
    CHECK(s.state == INPUT_WAITING);
    fake_edges.rising_count = 1U;
    fake_edges.rising[0] = 100U;
    fake_edges.timer_now = 100U;
    fake_edges.observed_ms = fake_now;
    Input_Process(fake_now);
    Input_GetSnapshot(&s);
    CHECK(s.state == INPUT_WAITING && s.sequence == previous.sequence);
    fake_now += 1500U;
    Input_Process(fake_now);
    Input_GetSnapshot(&s);
    CHECK(s.state == INPUT_NO_SIGNAL);
    /* Reconnect starts with a new full interval. */
    Fake_Period(100U, 84000U, 42000U, fake_now + 1U);
    Input_Process(fake_now);
    Input_GetSnapshot(&s);
    CHECK(s.state == INPUT_VALID);
    fake_faults = INPUT_FAULT_OVERCAPTURE;
    Input_GetSnapshot(&s);
    CHECK(s.state == INPUT_ERROR);
    Input_Process(fake_now);
    Input_GetSnapshot(&s);
    CHECK(s.state == INPUT_ERROR && s.overcapture_count == 1U);
    Input_Process(fake_now);
    Input_GetSnapshot(&s);
    CHECK(s.state == INPUT_WAITING);
    fake_faults = INPUT_FAULT_OVERRUN | INPUT_FAULT_DMA;
    Input_Process(fake_now);
    Input_GetSnapshot(&s);
    CHECK(s.state == INPUT_ERROR && s.buffer_overrun_count == 1U);
    /* Two falls in one rising interval proves a dropped rise/mispair. */
    Fake_Period(100U, 84000U, 42000U, fake_now + 1U);
    fake_edges.falling_count = 2U;
    fake_edges.falling[1] = 200U;
    Input_Process(fake_now);
    Input_GetSnapshot(&s);
    CHECK(s.state == INPUT_ERROR && s.pairing_error_count == 1U);
    Fake_Period(100U, 210U, 105U, fake_now + 1U);
    Input_Process(fake_now);
    Input_GetSnapshot(&s);
    CHECK(s.state == INPUT_OUT_OF_RANGE);
    CHECK(Input_SetMode(BIZ_MODE_EXTENDED) == BIZ_OK);
    Input_GetSnapshot(&s);
    CHECK(s.state == INPUT_WAITING && s.mode == BIZ_MODE_EXTENDED);
    InputCapabilities capabilities;
    Input_GetCapabilities(&capabilities);
    CHECK(capabilities.maximum_frequency_hz == 500000U);
    static const uint32_t ext_hz[] = {250000,333333,400000,499258,500000};
    for (unsigned i = 0; i < sizeof(ext_hz)/sizeof(ext_hz[0]); ++i) {
        for (unsigned duty = 0; duty < 21U; ++duty) {
            uint32_t duty_bp = duty == 0U ? 100U : duty == 20U ? 9900U : duty * 500U;
            uint32_t period = (INPUT_TIMER_HZ + ext_hz[i] / 2U) / ext_hz[i];
            uint32_t high = (uint32_t)(((uint64_t)period * duty_bp + 5000U) / 10000U);
            Fake_Period(timer, period, high, fake_now + 1U);
            timer += period;
            Input_Process(fake_now);
            Input_GetSnapshot(&s);
            CHECK(s.state == INPUT_VALID);
            uint32_t reference = ext_hz[i] * 1000U;
            uint32_t error = s.frequency_millihz > reference ? s.frequency_millihz-reference : reference-s.frequency_millihz;
            CHECK((uint64_t)error * 100U <= reference);
            int duty_error = (int)s.duty_bp - (int)duty_bp;
            CHECK(duty_error >= -300 && duty_error <= 300);
        }
    }
    CHECK(Input_SetMode(BIZ_MODE_STANDARD) == BIZ_OK);
    Input_GetSnapshot(&s);
    CHECK(s.state == INPUT_WAITING && s.mode == BIZ_MODE_STANDARD);
    Input_GetCapabilities(&capabilities);
    CHECK(capabilities.maximum_frequency_hz == 200000U);
    Fake_Period(timer,419U,210U,fake_now+1U);
    Input_Process(fake_now);
    Input_GetSnapshot(&s);
    CHECK(s.state == INPUT_VALID && s.frequency_millihz > 200000000U);
    Fake_Period(timer+1000U,418U,209U,fake_now+1U);
    Input_Process(fake_now);
    Input_GetSnapshot(&s);
    CHECK(s.state == INPUT_OUT_OF_RANGE);
    CHECK(Input_SetMode((BizMode)99) == BIZ_INVALID_ARGUMENT);
    CHECK(Input_GetMode() == BIZ_MODE_STANDARD);
    /* A reader invalidates stale data without touching its time/sequence. */
    CHECK(Input_BeginWindow(&id) == BIZ_OK);
    Fake_Period(100U, 84000U, 42000U, fake_now + 1U);
    Input_Process(fake_now);
    Input_GetSnapshot(&previous);
    fake_now += INPUT_NO_EDGE_MS;
    Input_GetSnapshot(&s);
    CHECK(s.state == INPUT_NO_SIGNAL && s.sequence == previous.sequence);
    /* HAL tick wrap and TIM5 wrap are independent. */
    CHECK(Input_BeginWindow(&id) == BIZ_OK);
    Fake_Period(UINT32_MAX - 40000U, 84000U, 42000U, UINT32_MAX - 5U);
    Input_Process(fake_now);
    Input_GetSnapshot(&s);
    CHECK(s.state == INPUT_VALID && s.period_ticks == 84000U);
    fake_now = 1494U;
    Input_GetSnapshot(&s);
    CHECK(s.state == INPUT_NO_SIGNAL);
    fake_restart_ok = false;
    CHECK(Input_BeginWindow(&id) == BIZ_HARDWARE_ERROR);
    Input_GetSnapshot(&s);
    CHECK(s.state == INPUT_ERROR);
    return 0;
}
