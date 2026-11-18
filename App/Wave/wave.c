#include "business_api.h"
#include "wave_config.h"
#include "wave_hw.h"
#include "spwm_private.h"
#include "wave_diagnostics.h"
#include <string.h>

typedef struct {
    WaveSnapshot snapshot;
    WaveTiming active, staged;
} Channel;
static Channel channels[2];
static bool initialized;
static BizResult init_result = BIZ_NOT_INITIALIZED;
static BizMode mode;
static WaveSyncSnapshot sync_status;
typedef enum { SYNC_IDLE, SYNC_PREPARE_B, SYNC_JOIN_A, SYNC_WAIT_JOIN,
               SYNC_PHASE_LATCH, SYNC_ZERO_LATCH, SYNC_WAIT_PHASE, SYNC_DETACH } SyncAction;
static SyncAction sync_action;
static bool sync_exit_requested;
static WaveTiming sync_b_timing;
static uint32_t phase_count, staged_phase_count;
volatile WaveRuntimeStats g_wave_runtime;

static uint32_t lock(void) { uint32_t p = __get_PRIMASK(); __disable_irq(); return p; }
static void unlock(uint32_t p) { __set_PRIMASK(p); }
static bool valid_channel(WaveChannel c) { return c == WAVE_CHANNEL_A || c == WAVE_CHANNEL_B; }
static bool valid_owner(WaveOwner o) { return o == WAVE_OWNER_MANUAL || o == WAVE_OWNER_SELFTEST; }
static BizResult validate(const WaveParams *p, BizMode m)
{
    if (!p) return BIZ_INVALID_ARGUMENT;
    const uint32_t maximum = m == BIZ_MODE_STANDARD ? WAVE_STANDARD_MAX_HZ : WAVE_EXTENDED_MAX_HZ;
    const uint32_t step = m == BIZ_MODE_STANDARD ? WAVE_STANDARD_STEP_BP : WAVE_EXTENDED_STEP_BP;
    if (p->frequency_hz < WAVE_MIN_HZ || p->frequency_hz > maximum || p->duty_bp > WAVE_MAX_DUTY_BP)
        return BIZ_OUT_OF_RANGE;
    return p->duty_bp % step ? BIZ_INVALID_STEP : BIZ_OK;
}
static WaveState state_of(const WaveSnapshot *s)
{
    if (!s->enabled) return WAVE_STOPPED;
    return s->applied.duty_bp == 0U ? WAVE_DC_LOW :
           s->applied.duty_bp == 10000U ? WAVE_DC_HIGH : WAVE_PWM;
}
static void complete(Channel *c, uint32_t now)
{
    WaveSnapshot *s = &c->snapshot;
    c->active = c->staged;
    s->applied = s->requested;
    s->calculated_frequency_millihz = c->active.frequency_millihz;
    s->calculated_duty_bp = c->active.duty_bp;
    s->applied_id = s->request_id;
    s->applied_ms = now;
    s->pending = false;
    s->state = state_of(s);
    s->last_result = BIZ_OK;
}
static void accept(Channel *c, uint32_t *id)
{
    ++c->snapshot.request_id;
    if (!c->snapshot.request_id) ++c->snapshot.request_id; /* 0 means no request yet. */
    c->snapshot.accepted_ms = HAL_GetTick();
    c->snapshot.pending = true;
    c->snapshot.last_result = BIZ_PENDING;
    if (id) *id = c->snapshot.request_id;
}
static bool phase_applicable(void)
{
    return sync_status.state == WAVE_SYNC_ON &&
        channels[0].snapshot.state == WAVE_PWM && channels[1].snapshot.state == WAVE_PWM;
}
static WaveTiming synchronized_b(uint16_t duty)
{
    const WaveTiming *a = &channels[0].active;
    const uint32_t compare = duty == 10000U ? 65535U :
        (a->counts * duty + (10000U * WAVE_SYNC_B_SCALE) / 2U) / (10000U * WAVE_SYNC_B_SCALE);
    return (WaveTiming){a->divider * WAVE_SYNC_B_SCALE, 65535U, compare,
        a->frequency_millihz, duty == 10000U ? 10000U :
        (uint16_t)((compare * WAVE_SYNC_B_SCALE * 10000U + a->counts / 2U) / a->counts)};
}
static void begin_sync_exit(void)
{
    sync_status.phase_pending = false;
    sync_action = SYNC_DETACH;
    (void)Wave_Calculate(WAVE_TIMER_HZ, channels[1].snapshot.applied.frequency_hz,
        channels[1].snapshot.applied.duty_bp, &sync_b_timing);
    WaveHW_Stage(WAVE_CHANNEL_B, &sync_b_timing);
}
static bool check_faults(void)
{
    bool bad[2] = {false, false};
    bool any = false;
    for (unsigned i = 0; i < 2; ++i) {
        TIM_TypeDef *tim = WaveHW_Timer((WaveChannel)i);
        bad[i] = channels[i].snapshot.enabled &&
                 (!(tim->CR1 & TIM_CR1_CEN) || !(tim->BDTR & TIM_BDTR_MOE));
        any = any || bad[i];
    }
    if (!any) return false;
    const bool was_sync = sync_status.state != WAVE_SYNC_OFF;
    for (unsigned i = 0; i < 2; ++i) {
        if (!bad[i] && !was_sync) continue;
        Channel *c = &channels[i];
        WaveHW_Stop((WaveChannel)i);
        if (c->snapshot.pending) c->snapshot.cancelled_id = c->snapshot.request_id;
        c->snapshot.pending = false;
        c->snapshot.requested = c->snapshot.applied;
        c->snapshot.enabled = false;
        c->snapshot.state = WAVE_FAULT;
        c->snapshot.last_result = BIZ_HARDWARE_ERROR;
        (void)Wave_Calculate(WAVE_TIMER_HZ, c->snapshot.applied.frequency_hz,
            c->snapshot.applied.duty_bp, &c->active);
        c->staged = c->active;
        c->snapshot.calculated_frequency_millihz = c->active.frequency_millihz;
        c->snapshot.calculated_duty_bp = c->active.duty_bp;
    }
    if (was_sync) {
        TIM8->SMCR &= ~TIM_SMCR_SMS;
        TIM8->CR1 |= TIM_CR1_URS;
        sync_action = SYNC_IDLE;
        sync_exit_requested = false;
        sync_status.state = WAVE_SYNC_OFF;
        sync_status.phase_pending = false;
        sync_status.phase_applicable = false;
        sync_status.last_result = BIZ_HARDWARE_ERROR;
    }
    return true;
}
BizResult Wave_Init(void)
{
    if (initialized) return BIZ_OK; /* Page navigation must not reset running output. */
    const BizResult result = WaveHW_Init();
    if (result != BIZ_OK) { init_result = result; return result; }
    mode = BIZ_MODE_STANDARD;
    memset(channels, 0, sizeof(channels));
    for (unsigned i = 0; i < 2; ++i) {
        Channel *c = &channels[i];
        c->snapshot.applied = (WaveParams){WAVE_DEFAULT_HZ, WAVE_DEFAULT_DUTY_BP};
        c->snapshot.requested = c->snapshot.applied;
        (void)Wave_Calculate(WAVE_TIMER_HZ, WAVE_DEFAULT_HZ, WAVE_DEFAULT_DUTY_BP, &c->active);
        c->staged = c->active;
        c->snapshot.calculated_frequency_millihz = c->active.frequency_millihz;
        c->snapshot.calculated_duty_bp = c->active.duty_bp;
        c->snapshot.state = WAVE_STOPPED;
        c->snapshot.owner = WAVE_OWNER_MANUAL;
    }
    initialized = true;
    init_result = BIZ_OK;
    return BIZ_OK;
}
void Wave_Process(uint32_t now_ms)
{
    (void)now_ms;
    Spwm_Service();
    if (!initialized) return;
    const uint32_t p = lock();
    /* A main-loop fallback for delayed acknowledgement, never wave generation. */
    Wave_TIM1_UpdateIRQ();
    Wave_TIM8_UpdateIRQ();
    if (check_faults()) { unlock(p); return; }
    sync_status.phase_applicable = phase_applicable();
    if ((sync_action == SYNC_WAIT_JOIN || sync_action == SYNC_WAIT_PHASE) && (TIM8->SR & TIM_SR_TIF)) {
        TIM8->SR = ~TIM_SR_TIF;
        phase_count = staged_phase_count;
        if (sync_action == SYNC_WAIT_JOIN && !sync_exit_requested) sync_status.state = WAVE_SYNC_ON;
        sync_action = SYNC_IDLE;
        const uint32_t target = (channels[0].active.counts * (sync_status.requested_phase_deg % 360U) + 180U) / 360U;
        if (phase_count == target) {
            sync_status.applied_phase_deg = sync_status.requested_phase_deg;
            sync_status.phase_pending = false;
            sync_status.last_result = BIZ_OK;
        }
        sync_status.calculated_phase_millideg =
            (uint32_t)(((uint64_t)phase_count * 360000U + channels[0].active.counts / 2U) / channels[0].active.counts);
    }
    if (sync_exit_requested && sync_action == SYNC_IDLE) begin_sync_exit();
    if (sync_status.state == WAVE_SYNC_ON && sync_status.phase_pending && sync_action == SYNC_IDLE) {
        const uint32_t n = channels[0].active.counts;
        uint32_t next = (n * (sync_status.requested_phase_deg % 360U) + 180U) / 360U;
        if (!next) {
            /* Suppress OC2REF at the next boundary, select UPDATE only AFTER
             * that UEV. The next trigger extends a low interval, never adds
             * an immediate false edge by selecting a high reference. */
            staged_phase_count = 0;
            sync_action = SYNC_ZERO_LATCH;
            WaveHW_StageReference(n, false);
        } else {
            if (next < phase_count) {
                const uint32_t high = channels[1].active.compare * WAVE_SYNC_B_SCALE;
                const uint32_t room = n > high ? n - high : 0U;
                uint32_t advance = room / 2U;
                if (!advance) advance = 1U;
                if (phase_count - next > advance) next = phase_count - advance;
            }
            staged_phase_count = next;
            sync_action = SYNC_PHASE_LATCH;
            WaveHW_StageReference(next, phase_count == 0U);
        }
    }
    unlock(p);
}
BizMode Wave_GetMode(void) { return mode; }
void Wave_GetCapabilities(WaveCapabilities *out)
{
    if (!out) return;
    *out = (WaveCapabilities){0};
    if (!initialized) return;
    *out = (WaveCapabilities){WAVE_MIN_HZ,
        mode == BIZ_MODE_STANDARD ? WAVE_STANDARD_MAX_HZ : WAVE_EXTENDED_MAX_HZ,
        WAVE_MAX_DUTY_BP, mode == BIZ_MODE_STANDARD ? WAVE_STANDARD_STEP_BP : WAVE_EXTENDED_STEP_BP,
        WAVE_PHASE_STEP_DEG, true, true, Spwm_Ready()};
}
BizResult Wave_SetMode(BizMode m)
{
    if (!initialized) return BIZ_NOT_INITIALIZED;
    if (m != BIZ_MODE_STANDARD && m != BIZ_MODE_EXTENDED) return BIZ_INVALID_ARGUMENT;
    if (channels[0].snapshot.pending || channels[1].snapshot.pending ||
        sync_status.state != WAVE_SYNC_OFF || channels[0].snapshot.owner != WAVE_OWNER_MANUAL) return BIZ_BUSY;
    for (unsigned i = 0; i < 2; ++i) {
        const BizResult result = validate(&channels[i].snapshot.applied, m);
        if (result != BIZ_OK) return result;
    }
    mode = m;
    return BIZ_OK;
}
BizResult Wave_Apply(WaveChannel ch, WaveOwner owner, const WaveParams *params, uint32_t *id)
{
    if (!initialized) return BIZ_NOT_INITIALIZED;
    if (!valid_channel(ch) || !valid_owner(owner)) return BIZ_INVALID_ARGUMENT;
    Channel *c = &channels[ch];
    if (c->snapshot.owner != owner || c->snapshot.pending) return BIZ_BUSY;
    if (c->snapshot.state == WAVE_FAULT) return BIZ_HARDWARE_ERROR;
    if (sync_status.state == WAVE_SYNC_STARTING || sync_status.state == WAVE_SYNC_STOPPING || sync_status.phase_pending)
        return BIZ_BUSY;
    BizResult result = validate(params, mode);
    if (result != BIZ_OK) return result;
    if (sync_status.state == WAVE_SYNC_ON && params->frequency_hz != c->snapshot.applied.frequency_hz)
        return BIZ_BUSY;
    WaveTiming timing;
    if (!Wave_Calculate(WAVE_TIMER_HZ, params->frequency_hz, params->duty_bp, &timing)) return BIZ_OUT_OF_RANGE;
    if (ch == WAVE_CHANNEL_B && sync_status.state == WAVE_SYNC_ON) timing = synchronized_b(params->duty_bp);
    const uint32_t p = lock();
    c->snapshot.requested = *params;
    c->staged = timing;
    accept(c, id);
    if (c->snapshot.enabled) {
        WaveHW_Stage(ch, &timing);
        result = BIZ_PENDING;
    } else {
        /* Saved configuration changes while physical output stays low. */
        complete(c, HAL_GetTick());
        result = BIZ_OK;
    }
    unlock(p);
    return result;
}
BizResult Wave_SetEnabled(WaveChannel ch, WaveOwner owner, bool enabled, uint32_t *id)
{
    if (!initialized) return BIZ_NOT_INITIALIZED;
    if (!valid_channel(ch) || !valid_owner(owner)) return BIZ_INVALID_ARGUMENT;
    Channel *c = &channels[ch];
    if (c->snapshot.owner != owner || (enabled && c->snapshot.pending)) return BIZ_BUSY;
    if (enabled && c->snapshot.state == WAVE_FAULT) return BIZ_HARDWARE_ERROR;
    if (sync_status.state != WAVE_SYNC_OFF) return BIZ_BUSY;
    const uint32_t p = lock();
    if (!enabled) {
        /* Freeze commits before cancellation. A latched request must be
         * acknowledged, even if its IRQ has not yet been serviced. */
        WaveHW_Timer(ch)->CR1 |= TIM_CR1_UDIS;
        if (ch == WAVE_CHANNEL_A) Wave_TIM1_UpdateIRQ();
        else Wave_TIM8_UpdateIRQ();
    }
    if (!c->snapshot.pending && c->snapshot.enabled == enabled && c->snapshot.state != WAVE_FAULT) {
        WaveHW_Timer(ch)->CR1 &= ~TIM_CR1_UDIS;
        if (id) *id = c->snapshot.applied_id;
        unlock(p);
        return BIZ_OK;
    }
    if (!enabled && c->snapshot.pending) {
        c->snapshot.cancelled_id = c->snapshot.request_id;
        c->snapshot.pending = false;
        c->snapshot.requested = c->snapshot.applied;
        c->staged = c->active;
    }
    c->snapshot.requested = c->snapshot.applied;
    c->staged = c->active;
    accept(c, id);
    c->snapshot.enabled = enabled;
    if (enabled) WaveHW_Start(ch, &c->active);
    else WaveHW_Stop(ch);
    complete(c, HAL_GetTick());
    unlock(p);
    return BIZ_OK;
}
void Wave_GetSnapshot(WaveChannel ch, WaveSnapshot *out)
{
    if (!out) return;
    const uint32_t p = lock();
    *out = (WaveSnapshot){0};
    out->state = WAVE_UNINITIALIZED;
    out->last_result = init_result;
    if (initialized && valid_channel(ch)) *out = channels[ch].snapshot;
    unlock(p);
}
BizResult Wave_AcquireA(WaveOwner owner)
{
    if (!initialized) return BIZ_NOT_INITIALIZED;
    if (owner != WAVE_OWNER_SELFTEST) return BIZ_INVALID_ARGUMENT;
    if (sync_status.state != WAVE_SYNC_OFF || channels[0].snapshot.pending ||
        channels[0].snapshot.owner != WAVE_OWNER_MANUAL) return BIZ_BUSY;
    channels[0].snapshot.owner = owner;
    return BIZ_OK;
}
BizResult Wave_ReleaseA(WaveOwner owner)
{
    if (!initialized) return BIZ_NOT_INITIALIZED;
    if (owner != WAVE_OWNER_SELFTEST) return BIZ_INVALID_ARGUMENT;
    if (channels[0].snapshot.owner != owner) return BIZ_INVALID_ARGUMENT;
    if (channels[0].snapshot.pending) return BIZ_BUSY;
    channels[0].snapshot.owner = WAVE_OWNER_MANUAL;
    return BIZ_OK;
}
BizResult Wave_SetSync(bool enabled)
{
    if (!initialized) return BIZ_NOT_INITIALIZED;
    if (channels[0].snapshot.owner != WAVE_OWNER_MANUAL || channels[0].snapshot.pending ||
        channels[1].snapshot.pending) return BIZ_BUSY;
    if (!enabled) {
        if (sync_status.state == WAVE_SYNC_OFF) return BIZ_OK;
        if (sync_status.state == WAVE_SYNC_STOPPING) return BIZ_PENDING;
        const uint32_t p = lock();
        sync_exit_requested = true;
        sync_status.state = WAVE_SYNC_STOPPING;
        sync_status.last_result = BIZ_PENDING;
        /* Finish at most the staged reference transfer/trigger, not every
         * remaining small phase step. Then detach on a real B reset. */
        if (sync_action == SYNC_IDLE) begin_sync_exit();
        unlock(p);
        return BIZ_PENDING;
    }
    if (sync_status.phase_pending ||
        sync_status.state == WAVE_SYNC_STARTING || sync_status.state == WAVE_SYNC_STOPPING) return BIZ_BUSY;
    if (sync_status.state == WAVE_SYNC_ON) return BIZ_OK;
    if (enabled && (!channels[0].snapshot.enabled || !channels[1].snapshot.enabled ||
        channels[0].snapshot.applied.frequency_hz != channels[1].snapshot.applied.frequency_hz ||
        channels[0].snapshot.state != WAVE_PWM || channels[1].snapshot.state != WAVE_PWM)) return BIZ_INVALID_ARGUMENT;
    const uint32_t p = lock();
    if (enabled) {
        sync_status.state = WAVE_SYNC_STARTING;
        sync_exit_requested = false;
        sync_status.phase_pending = true;
        sync_action = SYNC_PREPARE_B;
        sync_b_timing = synchronized_b(channels[1].snapshot.applied.duty_bp);
        /* First finish a whole B pulse. Its natural UEV starts a longer
         * transition cycle. Join only AFTER the following A UEV, so the
         * first reset cannot cut that B high or create a short low. */
        MODIFY_REG(TIM1->CR2, TIM_CR2_MMS, TIM_CR2_MMS_1);
        WaveHW_Stage(WAVE_CHANNEL_B, &sync_b_timing);
    }
    sync_status.last_result = BIZ_PENDING;
    unlock(p);
    return BIZ_PENDING;
}
BizResult Wave_SetPhase(uint16_t phase)
{
    if (!initialized) return BIZ_NOT_INITIALIZED;
    if (phase > 360U) return BIZ_OUT_OF_RANGE;
    if (phase % WAVE_PHASE_STEP_DEG) return BIZ_INVALID_STEP;
    if (sync_status.state != WAVE_SYNC_ON || sync_status.phase_pending ||
        channels[0].snapshot.pending || channels[1].snapshot.pending) return BIZ_BUSY;
    if (!phase_applicable()) return BIZ_INVALID_ARGUMENT;
    const uint32_t p = lock();
    sync_status.requested_phase_deg = phase;
    const uint32_t target = (channels[0].active.counts * (phase % 360U) + 180U) / 360U;
    if (target == phase_count) {
        sync_status.applied_phase_deg = phase;
        sync_status.last_result = BIZ_OK;
    } else {
        sync_status.phase_pending = true;
        sync_status.last_result = BIZ_PENDING;
    }
    unlock(p);
    return sync_status.last_result;
}
void Wave_GetSyncSnapshot(WaveSyncSnapshot *out)
{
    if (!out) return;
    const uint32_t p = lock();
    *out = sync_status;
    out->phase_applicable = initialized && phase_applicable();
    if (!initialized) out->last_result = BIZ_NOT_INITIALIZED;
    unlock(p);
}
void Wave_TIM1_UpdateIRQ(void)
{
    if ((TIM1->DIER & TIM_DIER_UIE) && (TIM1->SR & TIM_SR_UIF)) {
        const uint32_t started = DWT->CYCCNT;
        ++g_wave_runtime.a_updates;
        TIM1->DIER &= ~TIM_DIER_UIE;
        TIM1->SR = ~TIM_SR_UIF;
        if (initialized && channels[0].snapshot.pending) complete(&channels[0], HAL_GetTick());
        if (sync_action == SYNC_JOIN_A) {
            TIM8->SR = ~TIM_SR_TIF;
            TIM8->CR1 &= ~TIM_CR1_URS; /* Reset events acknowledge B preloads. */
            MODIFY_REG(TIM8->SMCR, TIM_SMCR_TS | TIM_SMCR_SMS, TIM_SMCR_SMS_2); /* ITR0 */
            staged_phase_count = 0;
            sync_action = SYNC_WAIT_JOIN;
        } else if (sync_action == SYNC_PHASE_LATCH || sync_action == SYNC_ZERO_LATCH) {
            TIM8->SR = ~TIM_SR_TIF;
            if (sync_action == SYNC_ZERO_LATCH)
                MODIFY_REG(TIM1->CR2, TIM_CR2_MMS, TIM_CR2_MMS_1);
            sync_action = SYNC_WAIT_PHASE;
        }
        const uint32_t elapsed = DWT->CYCCNT - started;
        if (elapsed > g_wave_runtime.a_max_cycles) g_wave_runtime.a_max_cycles = elapsed;
    }
}
void Wave_TIM8_UpdateIRQ(void)
{
    if ((TIM8->DIER & TIM_DIER_UIE) && (TIM8->SR & TIM_SR_UIF)) {
        const uint32_t started = DWT->CYCCNT;
        ++g_wave_runtime.b_updates;
        TIM8->DIER &= ~TIM_DIER_UIE;
        TIM8->SR = ~TIM_SR_UIF;
        if (initialized && channels[1].snapshot.pending) complete(&channels[1], HAL_GetTick());
        if (sync_action == SYNC_PREPARE_B) {
            channels[1].active = sync_b_timing;
            channels[1].staged = sync_b_timing;
            channels[1].snapshot.calculated_duty_bp = sync_b_timing.duty_bp;
            sync_action = SYNC_JOIN_A;
            WaveHW_ArmAUpdate();
        } else if (sync_action == SYNC_DETACH) {
            TIM8->SMCR &= ~TIM_SMCR_SMS;
            TIM8->CR1 |= TIM_CR1_URS;
            channels[1].active = sync_b_timing;
            channels[1].staged = sync_b_timing;
            channels[1].snapshot.calculated_duty_bp = sync_b_timing.duty_bp;
            sync_status.state = WAVE_SYNC_OFF;
            sync_status.phase_applicable = false;
            sync_status.last_result = BIZ_OK;
            sync_action = SYNC_IDLE;
            sync_exit_requested = false;
        }
        const uint32_t elapsed = DWT->CYCCNT - started;
        if (elapsed > g_wave_runtime.b_max_cycles) g_wave_runtime.b_max_cycles = elapsed;
    }
}
