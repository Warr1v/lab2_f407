#include "business_api.h"
#include "wave_config.h"
#include "wave_hw.h"
#include <string.h>

typedef struct {
    WaveSnapshot snapshot;
    WaveTiming active, staged;
} Channel;
static Channel channels[2];
static bool initialized;
static BizMode mode;
static WaveSyncSnapshot sync_status;

static uint32_t lock(void) { uint32_t p = __get_PRIMASK(); __disable_irq(); return p; }
static void unlock(uint32_t p) { __set_PRIMASK(p); }
static bool valid_channel(WaveChannel c) { return c == WAVE_CHANNEL_A; }
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
    c->snapshot.accepted_ms = HAL_GetTick();
    c->snapshot.pending = true;
    c->snapshot.last_result = BIZ_PENDING;
    if (id) *id = c->snapshot.request_id;
}
BizResult Wave_Init(void)
{
    if (initialized) return BIZ_OK; /* Page navigation must not reset running output. */
    const BizResult result = WaveHW_Init();
    if (result != BIZ_OK) return result;
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
        c->snapshot.state = i == 0U ? WAVE_STOPPED : WAVE_UNINITIALIZED;
        c->snapshot.owner = WAVE_OWNER_MANUAL;
    }
    initialized = true;
    return BIZ_OK;
}
void Wave_Process(uint32_t now_ms) { (void)now_ms; }
BizMode Wave_GetMode(void) { return mode; }
void Wave_GetCapabilities(WaveCapabilities *out)
{
    if (!out) return;
    *out = (WaveCapabilities){0};
    if (!initialized) return;
    *out = (WaveCapabilities){WAVE_MIN_HZ, WAVE_STANDARD_MAX_HZ,
        WAVE_MAX_DUTY_BP, WAVE_STANDARD_STEP_BP, WAVE_PHASE_STEP_DEG, false, false, false};
}
BizResult Wave_SetMode(BizMode m)
{
    if (!initialized) return BIZ_NOT_INITIALIZED;
    if (m != BIZ_MODE_STANDARD && m != BIZ_MODE_EXTENDED) return BIZ_INVALID_ARGUMENT;
    if (m == BIZ_MODE_EXTENDED) return BIZ_UNSUPPORTED; /* W3 enables after validation. */
    if (channels[0].snapshot.pending || channels[0].snapshot.owner != WAVE_OWNER_MANUAL) return BIZ_BUSY;
    mode = m;
    return BIZ_OK;
}
BizResult Wave_Apply(WaveChannel ch, WaveOwner owner, const WaveParams *params, uint32_t *id)
{
    if (!initialized) return BIZ_NOT_INITIALIZED;
    if (!valid_channel(ch) || !valid_owner(owner)) return BIZ_INVALID_ARGUMENT;
    Channel *c = &channels[ch];
    if (c->snapshot.owner != owner || c->snapshot.pending) return BIZ_BUSY;
    BizResult result = validate(params, mode);
    if (result != BIZ_OK) return result;
    WaveTiming timing;
    if (!Wave_Calculate(WAVE_TIMER_HZ, params->frequency_hz, params->duty_bp, &timing)) return BIZ_OUT_OF_RANGE;
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
    const uint32_t p = lock();
    if (!enabled) {
        /* Freeze commits before cancellation. A latched request must be
         * acknowledged, even if its IRQ has not yet been serviced. */
        WaveHW_Timer(ch)->CR1 |= TIM_CR1_UDIS;
        if (ch == WAVE_CHANNEL_A) Wave_TIM1_UpdateIRQ();
        else Wave_TIM8_UpdateIRQ();
    }
    if (!c->snapshot.pending && c->snapshot.enabled == enabled) {
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
    out->last_result = BIZ_NOT_INITIALIZED;
    if (initialized && valid_channel(ch)) *out = channels[ch].snapshot;
    unlock(p);
}
BizResult Wave_AcquireA(WaveOwner owner)
{
    if (!initialized) return BIZ_NOT_INITIALIZED;
    if (owner != WAVE_OWNER_SELFTEST) return BIZ_INVALID_ARGUMENT;
    if (channels[0].snapshot.pending || channels[0].snapshot.owner != WAVE_OWNER_MANUAL) return BIZ_BUSY;
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
BizResult Wave_SetSync(bool enabled) { (void)enabled; return initialized ? BIZ_UNSUPPORTED : BIZ_NOT_INITIALIZED; }
BizResult Wave_SetPhase(uint16_t phase) { (void)phase; return initialized ? BIZ_UNSUPPORTED : BIZ_NOT_INITIALIZED; }
void Wave_GetSyncSnapshot(WaveSyncSnapshot *out)
{
    if (out) { *out = sync_status; out->last_result = initialized ? BIZ_UNSUPPORTED : BIZ_NOT_INITIALIZED; }
}
void Wave_TIM1_UpdateIRQ(void)
{
    if ((TIM1->DIER & TIM_DIER_UIE) && (TIM1->SR & TIM_SR_UIF)) {
        TIM1->DIER &= ~TIM_DIER_UIE;
        TIM1->SR = ~TIM_SR_UIF;
        if (initialized && channels[0].snapshot.pending) complete(&channels[0], HAL_GetTick());
    }
}
void Wave_TIM8_UpdateIRQ(void)
{
    if ((TIM8->DIER & TIM_DIER_UIE) && (TIM8->SR & TIM_SR_UIF)) {
        TIM8->DIER &= ~TIM_DIER_UIE;
        TIM8->SR = ~TIM_SR_UIF;
        if (initialized && channels[1].snapshot.pending) complete(&channels[1], HAL_GetTick());
    }
}
