#include "business_api.h"
#include "sweep_internal.h"
#include "sweep_config.h"
#include <string.h>

static SweepSnapshot result;
static SweepDiagnostics diag;
static WaveParams saved, expected;
static bool initialized, saved_enabled, target_enabled, all_passed;
static uint32_t old_period, input_sequence;
static const uint32_t frequencies[] = SWEEP_FREQUENCIES;
_Static_assert(sizeof(frequencies)/sizeof(frequencies[0]) == LAB2_SWEEP_POINT_COUNT,
               "P1 requires exactly ten sweep points");

static uint32_t period_ms(uint32_t hz) { return hz ? 1U + (999U / hz) : 1000U; }
static bool params_equal(WaveParams a, WaveParams b)
{ return a.frequency_hz == b.frequency_hz && a.duty_bp == b.duty_bp; }
static bool active(void) { return result.state == SWEEP_RUNNING || result.state == SWEEP_RESTORING; }
static void step(SweepStep next, uint32_t now, uint32_t budget)
{ diag.step = next; diag.phase_started_ms = now; diag.wait_budget_ms = budget; }
static bool timed_out(uint32_t now) { return now - diag.phase_started_ms >= diag.wait_budget_ms; }
static void remember(BizResult reason)
{
    if (diag.first_failure == BIZ_OK) diag.first_failure = reason;
    result.last_result = reason;
}
static void output_failure(BizResult reason, uint32_t now);
static void restore(uint32_t now)
{
    WaveSnapshot wave;
    Wave_GetSnapshot(WAVE_CHANNEL_A, &wave);
    result.state = SWEEP_RESTORING;
    step(SWEEP_STEP_RESTORE_DRAIN, now,
         period_ms(wave.applied.frequency_hz) + SWEEP_APPLY_MARGIN_MS);
}
static void output_failure(BizResult reason, uint32_t now)
{
    result.points[result.current_index].state = SWEEP_POINT_OUTPUT_ERROR;
    ++result.completed;
    all_passed = false;
    remember(reason);
    restore(now);
}
static void emergency(BizResult reason, uint32_t now)
{
    diag.restore_result = reason;
    result.last_result = reason;
    result.restored = false;
    result.state = SWEEP_RESTORING;
    step(SWEEP_STEP_EMERGENCY_OFF, now, 0U);
}
static void held_failure(BizResult reason)
{
    WaveSnapshot wave;
    Wave_GetSnapshot(WAVE_CHANNEL_A, &wave);
    result.owns_a = wave.owner == WAVE_OWNER_SELFTEST;
    result.state = SWEEP_FAIL;
    result.restored = false;
    result.last_result = diag.restore_result == BIZ_OK ? reason : diag.restore_result;
    diag.emergency_result = reason;
    diag.step = SWEEP_STEP_DONE;
}

BizResult Sweep_Init(void)
{
    if (initialized && (active() || result.owns_a)) return BIZ_BUSY;
    memset(&result, 0, sizeof(result));
    memset(&diag, 0, sizeof(diag));
    for (unsigned i = 0; i < LAB2_SWEEP_POINT_COUNT; ++i)
        result.points[i].set_frequency_hz = frequencies[i];
    result.state = SWEEP_IDLE;
    diag.step = SWEEP_STEP_DONE;
    initialized = true;
    return BIZ_OK;
}

BizResult Sweep_Start(void)
{
    if (!initialized) return BIZ_NOT_INITIALIZED;
    if (active() || result.owns_a) return BIZ_BUSY;
    WaveSnapshot wave;
    WaveSyncSnapshot sync;
    InputSnapshot input;
    Wave_GetSnapshot(WAVE_CHANNEL_A, &wave);
    Wave_GetSyncSnapshot(&sync);
    Input_GetSnapshot(&input);
    if (wave.state == WAVE_UNINITIALIZED || input.last_result == BIZ_NOT_INITIALIZED)
        return BIZ_NOT_INITIALIZED;
    if (wave.state == WAVE_FAULT || input.state == INPUT_ERROR)
        return BIZ_HARDWARE_ERROR;
    if (sync.state != WAVE_SYNC_OFF || wave.pending || wave.owner != WAVE_OWNER_MANUAL)
        return BIZ_BUSY;
    if (Wave_GetMode() != Input_GetMode()) return BIZ_INVALID_ARGUMENT;
    if (!wave.applied.frequency_hz) return BIZ_HARDWARE_ERROR;
    BizResult acquired = Wave_AcquireA(WAVE_OWNER_SELFTEST);
    if (acquired != BIZ_OK) return acquired; /* no output operation before lock */
    Wave_GetSnapshot(WAVE_CHANNEL_A, &wave);
    memset(&result, 0, sizeof(result));
    memset(&diag, 0, sizeof(diag));
    for (unsigned i = 0; i < LAB2_SWEEP_POINT_COUNT; ++i)
        result.points[i].set_frequency_hz = frequencies[i];
    saved = wave.applied;
    saved_enabled = wave.enabled;
    result.owns_a = true;
    result.state = SWEEP_RUNNING;
    all_passed = true;
    step(SWEEP_STEP_ENABLE, SweepPort_Now(), 0U);
    return BIZ_OK;
}

BizResult Sweep_Cancel(void)
{
    if (!initialized) return BIZ_NOT_INITIALIZED;
    if (!active()) return BIZ_BUSY;
    diag.cancelled = true;
    if (result.state == SWEEP_RUNNING) restore(SweepPort_Now());
    return BIZ_OK;
}

static void finish_point(SweepPointState state, BizResult reason, uint32_t now)
{
    result.points[result.current_index].state = state;
    if (state != SWEEP_POINT_PASS) { all_passed = false; remember(reason); }
    ++result.completed;
    if (result.completed == LAB2_SWEEP_POINT_COUNT) restore(now);
    else { result.current_index = result.completed; step(SWEEP_STEP_APPLY, now, 0U); }
}

/* An accepted request is not an applied request. All waits check its exact ID,
 * the full parameter/enable target, actual pending flag and cancellation. */
static BizResult applied(const WaveSnapshot *wave, bool enable_operation)
{
    if (wave->state == WAVE_FAULT || wave->owner != WAVE_OWNER_SELFTEST)
        return BIZ_HARDWARE_ERROR;
    if (!wave->pending && wave->applied_id == diag.request_id) {
        if (enable_operation ? wave->enabled != target_enabled : !params_equal(wave->applied, expected))
            return BIZ_HARDWARE_ERROR;
        return BIZ_OK;
    }
    if (!wave->pending && wave->cancelled_id == diag.request_id)
        return BIZ_CANCELLED;
    if (!wave->pending && wave->last_result != BIZ_OK && wave->last_result != BIZ_PENDING)
        return wave->last_result;
    return BIZ_PENDING;
}

void Sweep_Process(uint32_t now)
{
    if (!initialized || !active()) return;
    WaveSnapshot wave;
    Wave_GetSnapshot(WAVE_CHANNEL_A, &wave);
    if (result.state == SWEEP_RUNNING && (wave.state == WAVE_FAULT || wave.owner != WAVE_OWNER_SELFTEST)) {
        result.points[result.current_index].state = SWEEP_POINT_OUTPUT_ERROR;
        ++result.completed;
        all_passed = false;
        remember(BIZ_HARDWARE_ERROR);
        restore(now);
        return;
    }
    if (result.state == SWEEP_RESTORING && diag.step < SWEEP_STEP_EMERGENCY_OFF &&
        (wave.state == WAVE_FAULT || wave.owner != WAVE_OWNER_SELFTEST)) {
        emergency(BIZ_HARDWARE_ERROR, now);
        return;
    }
    BizResult status;
    switch (diag.step) {
    case SWEEP_STEP_ENABLE:
        if (wave.enabled) { step(SWEEP_STEP_APPLY, now, 0U); break; }
        target_enabled = true;
        status = Wave_SetEnabled(WAVE_CHANNEL_A, WAVE_OWNER_SELFTEST, true, &diag.request_id);
        if (status != BIZ_OK && status != BIZ_PENDING) { output_failure(status, now); break; }
        step(SWEEP_STEP_ENABLE_WAIT, now, period_ms(wave.applied.frequency_hz) + SWEEP_APPLY_MARGIN_MS);
        break;
    case SWEEP_STEP_ENABLE_WAIT:
    case SWEEP_STEP_APPLY_WAIT:
        status = applied(&wave, diag.step == SWEEP_STEP_ENABLE_WAIT);
        if (status == BIZ_OK) {
            if (diag.step == SWEEP_STEP_ENABLE_WAIT) step(SWEEP_STEP_APPLY, now, 0U);
            else step(SWEEP_STEP_SETTLE, now, old_period + 2U * period_ms(expected.frequency_hz) + SWEEP_SETTLE_MARGIN_MS);
        } else if (status != BIZ_PENDING || timed_out(now)) {
            result.points[result.current_index].state = SWEEP_POINT_OUTPUT_ERROR;
            ++result.completed;
            all_passed = false;
            remember(status == BIZ_PENDING ? BIZ_TIMEOUT : status);
            restore(now);
        }
        break;
    case SWEEP_STEP_APPLY:
        expected = (WaveParams){frequencies[result.current_index], SWEEP_DUTY_BP};
        old_period = period_ms(wave.applied.frequency_hz);
        status = Wave_Apply(WAVE_CHANNEL_A, WAVE_OWNER_SELFTEST, &expected, &diag.request_id);
        if (status != BIZ_OK && status != BIZ_PENDING) { output_failure(status, now); break; }
        step(SWEEP_STEP_APPLY_WAIT, now, old_period + SWEEP_APPLY_MARGIN_MS);
        break;
    case SWEEP_STEP_SETTLE:
        if (timed_out(now)) step(SWEEP_STEP_WINDOW, now, 0U);
        break;
    case SWEEP_STEP_WINDOW: {
        InputSnapshot input;
        Input_GetSnapshot(&input);
        input_sequence = input.sequence;
        status = Input_BeginWindow(&diag.acquisition_id);
        if (status != BIZ_OK) finish_point(SWEEP_POINT_CAPTURE_ERROR, status, now);
        else step(SWEEP_STEP_SAMPLE, now, SWEEP_CAPTURE_TIMEOUT_MS);
        break;
    }
    case SWEEP_STEP_SAMPLE: {
        InputSnapshot input;
        Input_GetSnapshot(&input);
        if (input.acquisition_id != diag.acquisition_id || input.mode != Wave_GetMode() ||
            input.state == INPUT_ERROR || input.state == INPUT_OUT_OF_RANGE) {
            finish_point(SWEEP_POINT_CAPTURE_ERROR, BIZ_HARDWARE_ERROR, now);
        } else if (input.state == INPUT_VALID && input.sequence != input_sequence &&
                   now - input.window_end_ms <= SWEEP_CAPTURE_TIMEOUT_MS &&
                   input.published_ms - diag.phase_started_ms <= now - diag.phase_started_ms) {
            SweepPoint *point = &result.points[result.current_index];
            uint32_t target = point->set_frequency_hz * 1000U;
            uint32_t error = input.frequency_millihz > target ? input.frequency_millihz - target : target - input.frequency_millihz;
            point->measured_frequency_millihz = input.frequency_millihz;
            point->error_percent = 100.0f * (float)error / (float)target;
            bool pass = (uint64_t)error * 10000U <= (uint64_t)target * SWEEP_TOLERANCE_BP;
            finish_point(pass ? SWEEP_POINT_PASS : SWEEP_POINT_OUT_OF_TOLERANCE,
                         pass ? BIZ_OK : BIZ_OUT_OF_RANGE, now);
        } else if (input.state == INPUT_NO_SIGNAL || timed_out(now))
            finish_point(SWEEP_POINT_NO_SIGNAL, BIZ_TIMEOUT, now);
        break;
    }
    case SWEEP_STEP_RESTORE_DRAIN:
        if (!wave.pending) step(SWEEP_STEP_RESTORE_PARAMS, now, 0U);
        else if (timed_out(now)) emergency(BIZ_TIMEOUT, now);
        break;
    case SWEEP_STEP_RESTORE_PARAMS:
        if (params_equal(wave.applied, saved)) { step(SWEEP_STEP_RESTORE_ENABLE, now, 0U); break; }
        expected = saved;
        status = Wave_Apply(WAVE_CHANNEL_A, WAVE_OWNER_SELFTEST, &saved, &diag.request_id);
        if (status != BIZ_OK && status != BIZ_PENDING) { emergency(status, now); break; }
        step(SWEEP_STEP_RESTORE_PARAMS_WAIT, now, period_ms(wave.applied.frequency_hz) + SWEEP_APPLY_MARGIN_MS);
        break;
    case SWEEP_STEP_RESTORE_PARAMS_WAIT:
        status = applied(&wave, false);
        if (status == BIZ_OK) step(SWEEP_STEP_RESTORE_ENABLE, now, 0U);
        else if (status != BIZ_PENDING || timed_out(now)) emergency(status == BIZ_PENDING ? BIZ_TIMEOUT : status, now);
        break;
    case SWEEP_STEP_RESTORE_ENABLE:
        if (wave.enabled == saved_enabled) { step(SWEEP_STEP_RELEASE, now, SWEEP_APPLY_MARGIN_MS); break; }
        target_enabled = saved_enabled;
        status = Wave_SetEnabled(WAVE_CHANNEL_A, WAVE_OWNER_SELFTEST, saved_enabled, &diag.request_id);
        if (status != BIZ_OK && status != BIZ_PENDING) { emergency(status, now); break; }
        step(SWEEP_STEP_RESTORE_ENABLE_WAIT, now, period_ms(wave.applied.frequency_hz) + SWEEP_APPLY_MARGIN_MS);
        break;
    case SWEEP_STEP_RESTORE_ENABLE_WAIT:
        status = applied(&wave, true);
        if (status == BIZ_OK) step(SWEEP_STEP_RELEASE, now, SWEEP_APPLY_MARGIN_MS);
        else if (status != BIZ_PENDING || timed_out(now)) emergency(status == BIZ_PENDING ? BIZ_TIMEOUT : status, now);
        break;
    case SWEEP_STEP_RELEASE:
        if (wave.pending || !params_equal(wave.applied, saved) || wave.enabled != saved_enabled) {
            emergency(BIZ_HARDWARE_ERROR, now); break;
        }
        status = Wave_ReleaseA(WAVE_OWNER_SELFTEST);
        if (status == BIZ_OK) {
            Wave_GetSnapshot(WAVE_CHANNEL_A, &wave);
            if (wave.owner == WAVE_OWNER_SELFTEST) { emergency(BIZ_HARDWARE_ERROR, now); break; }
            result.owns_a = false;
            result.restored = true;
            diag.restore_result = BIZ_OK;
            result.state = diag.cancelled ? SWEEP_CANCELLED : all_passed ? SWEEP_PASS : SWEEP_FAIL;
            result.last_result = diag.cancelled ? BIZ_CANCELLED : all_passed ? BIZ_OK : diag.first_failure;
            step(SWEEP_STEP_DONE, now, 0U);
        } else if (status != BIZ_BUSY || timed_out(now)) emergency(status == BIZ_BUSY ? BIZ_TIMEOUT : status, now);
        break;
    case SWEEP_STEP_EMERGENCY_OFF:
        status = Wave_SetEnabled(WAVE_CHANNEL_A, WAVE_OWNER_SELFTEST, false, &diag.request_id);
        diag.emergency_result = status;
        if (status != BIZ_OK && status != BIZ_PENDING) { held_failure(status); break; }
        target_enabled = false;
        step(SWEEP_STEP_EMERGENCY_WAIT, now, period_ms(wave.applied.frequency_hz) + SWEEP_APPLY_MARGIN_MS);
        break;
    case SWEEP_STEP_EMERGENCY_WAIT:
        status = applied(&wave, true);
        if (status == BIZ_OK) held_failure(BIZ_OK);
        else if (status != BIZ_PENDING || timed_out(now)) held_failure(status == BIZ_PENDING ? BIZ_TIMEOUT : status);
        break;
    default: emergency(BIZ_HARDWARE_ERROR, now); break;
    }
}

void Sweep_GetSnapshot(SweepSnapshot *out) { if (out) *out = result; }
void Sweep_GetDiagnostics(SweepDiagnostics *out) { if (out) *out = diag; }
