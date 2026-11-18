#include "business_api.h"
#include "input_port.h"
#include <string.h>

static InputSnapshot result;
static bool initialized, have_end, have_edge, no_signal_rearmed;
static uint32_t previous_end, last_edge_ms, armed_ms;

static uint32_t next_id(uint32_t id)
{
    ++id;
    return id ? id : 1U;
}

static void invalidate(InputState state, BizResult reason)
{
    result.state = state;
    result.last_result = reason;
}

static bool restart(void)
{
    have_end = have_edge = false;
    armed_ms = InputPort_Now();
    return InputPort_Restart();
}

BizResult Input_Init(void)
{
    if (initialized) return BIZ_BUSY;
    memset(&result, 0, sizeof(result));
    result.mode = BIZ_MODE_STANDARD;
    result.acquisition_id = 1U;
    invalidate(INPUT_WAITING, BIZ_OK);
    if (!InputPort_Init() || !restart()) {
        invalidate(INPUT_ERROR, BIZ_HARDWARE_ERROR);
        return BIZ_HARDWARE_ERROR;
    }
    initialized = true;
    no_signal_rearmed = false;
    return BIZ_OK;
}

BizResult Input_BeginWindow(uint32_t *id)
{
    if (!id) return BIZ_INVALID_ARGUMENT;
    if (!initialized) return BIZ_NOT_INITIALIZED;
    if (!restart()) {
        invalidate(INPUT_ERROR, BIZ_HARDWARE_ERROR);
        return BIZ_HARDWARE_ERROR;
    }
    result.acquisition_id = next_id(result.acquisition_id);
    no_signal_rearmed = false;
    invalidate(INPUT_WAITING, BIZ_OK);
    *id = result.acquisition_id;
    return BIZ_OK;
}

BizResult Input_SetMode(BizMode mode)
{
    if (mode != BIZ_MODE_STANDARD && mode != BIZ_MODE_EXTENDED)
        return BIZ_INVALID_ARGUMENT;
    if (!initialized) return BIZ_NOT_INITIALIZED;
    if (mode == result.mode) return BIZ_OK;
    uint32_t id;
    BizResult status = Input_BeginWindow(&id);
    if (status == BIZ_OK) result.mode = mode;
    return status;
}

BizMode Input_GetMode(void) { return result.mode; }

void Input_GetCapabilities(InputCapabilities *out)
{
    if (!out) return;
    *out = (InputCapabilities){INPUT_MIN_HZ,
        result.mode == BIZ_MODE_EXTENDED ? INPUT_EXTENDED_MAX_HZ : INPUT_STANDARD_MAX_HZ,
        INPUT_NO_EDGE_MS};
}

static void capture_error(uint32_t faults)
{
    if (faults & INPUT_FAULT_OVERCAPTURE) ++result.overcapture_count;
    if (faults & (INPUT_FAULT_OVERRUN | INPUT_FAULT_DMA)) ++result.buffer_overrun_count;
    if (!faults) ++result.pairing_error_count;
    /* Preserve ERROR for this service call; next service may reacquire. */
    (void)restart();
    no_signal_rearmed = false;
    invalidate(INPUT_ERROR, BIZ_HARDWARE_ERROR);
}

void Input_Process(uint32_t now_ms)
{
    if (!initialized) return;
    InputPort_Service();
    uint32_t faults = InputPort_Faults();
    if (faults) { capture_error(faults); return; }
    InputEdges edges;
    InputPort_Read(&edges);
    /* A fresh edge may be either polarity; constant levels have neither. */
    uint32_t youngest = UINT32_MAX;
    if (edges.rising_count) youngest = edges.timer_now - edges.rising[0];
    if (edges.falling_count) {
        uint32_t age = edges.timer_now - edges.falling[0];
        if (age < youngest) youngest = age;
    }
    if (youngest != UINT32_MAX && youngest / (INPUT_TIMER_HZ / 1000U) < INPUT_NO_EDGE_MS) {
        last_edge_ms = edges.observed_ms - youngest / (INPUT_TIMER_HZ / 1000U);
        have_edge = true;
        no_signal_rearmed = false;
    }
    if (now_ms - (have_edge ? last_edge_ms : armed_ms) >= INPUT_NO_EDGE_MS) {
        if (!no_signal_rearmed) {
            if (!restart()) { invalidate(INPUT_ERROR, BIZ_HARDWARE_ERROR); return; }
            no_signal_rearmed = true;
        }
        invalidate(INPUT_NO_SIGNAL, BIZ_TIMEOUT);
        return;
    }
    if (result.state == INPUT_ERROR || (result.state == INPUT_NO_SIGNAL && have_edge))
        invalidate(INPUT_WAITING, BIZ_OK);
    if (edges.rising_count < 2U || !edges.falling_count) return;
    uint32_t end = edges.rising[0], start = edges.rising[1];
    if (have_end && end == previous_end) return;
    uint32_t period = end - start, high = 0U, matches = 0U;
    if (!period || period >= INPUT_TIMER_HZ * 2U) { capture_error(0U); return; }
    for (unsigned i = 0; i < edges.falling_count; ++i) {
        uint32_t delta = edges.falling[i] - start;
        if (delta && delta < period) { high = delta; ++matches; }
    }
    if (matches != 1U) {
        /* A falling DMA transfer may still be in flight. Wait until a later
         * fall proves that the interval is complete, otherwise retry later. */
        if (matches > 1U || (uint32_t)(edges.falling[0] - end) < 0x80000000U)
            capture_error(0U);
        else invalidate(INPUT_WAITING, BIZ_OK);
        return;
    }
    uint32_t age_ticks = edges.timer_now - end;
    if (age_ticks / (INPUT_TIMER_HZ / 1000U) >= INPUT_NO_EDGE_MS) return;
    have_end = true;
    previous_end = end;
    uint32_t max_hz = result.mode == BIZ_MODE_EXTENDED ? INPUT_EXTENDED_MAX_HZ : INPUT_STANDARD_MAX_HZ;
    uint32_t frequency = (uint32_t)(((uint64_t)INPUT_TIMER_HZ * 1000U + period / 2U) / period);
    result.period_ticks = period;
    result.high_ticks = high;
    result.frequency_millihz = frequency;
    result.duty_bp = (uint16_t)(((uint64_t)high * 10000U + period / 2U) / period);
    result.window_end_ms = edges.observed_ms - age_ticks / (INPUT_TIMER_HZ / 1000U);
    /* Conservative millisecond bounds, with up to one ms quantization. */
    result.window_start_ms = edges.observed_ms -
        (age_ticks + period + INPUT_TIMER_HZ / 1000U - 1U) / (INPUT_TIMER_HZ / 1000U);
    result.published_ms = now_ms;
    ++result.sequence;
    if ((uint64_t)(period + INPUT_RANGE_QUANTIZATION_TICKS) * max_hz < INPUT_TIMER_HZ ||
        (uint64_t)(period - (period > INPUT_RANGE_QUANTIZATION_TICKS ? INPUT_RANGE_QUANTIZATION_TICKS : 0U)) * INPUT_MIN_HZ > INPUT_TIMER_HZ)
        invalidate(INPUT_OUT_OF_RANGE, BIZ_OUT_OF_RANGE);
    else invalidate(INPUT_VALID, BIZ_OK);
}

void Input_GetSnapshot(InputSnapshot *out)
{
    if (!out) return;
    *out = result;
    if (!initialized) { out->state = INPUT_ERROR; out->last_result = BIZ_NOT_INITIALIZED; return; }
    if (InputPort_Faults()) {
        out->state = INPUT_ERROR;
        out->last_result = BIZ_HARDWARE_ERROR;
    } else if (out->state == INPUT_VALID && InputPort_Now() - out->window_end_ms >= INPUT_NO_EDGE_MS) {
        out->state = INPUT_NO_SIGNAL;
        out->last_result = BIZ_TIMEOUT;
    }
}
