#ifndef LAB2_BUSINESS_API_H
#define LAB2_BUSINESS_API_H

/* P1 contract template. Copy unchanged to App/Contracts in each worktree.
 * Semantics: ai_tasks/06_并行开发公共约定.md. No HAL dependency.
 * This header declares interfaces; it is not an implementation. */
#include <stdbool.h>
#include <stdint.h>

#define LAB2_BUSINESS_API_VERSION 1U
#define LAB2_SWEEP_POINT_COUNT 10U

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BIZ_OK, BIZ_PENDING, BIZ_BUSY, BIZ_INVALID_ARGUMENT,
    BIZ_OUT_OF_RANGE, BIZ_INVALID_STEP, BIZ_UNSUPPORTED,
    BIZ_NOT_INITIALIZED, BIZ_HARDWARE_ERROR, BIZ_TIMEOUT, BIZ_CANCELLED
} BizResult;

typedef enum { BIZ_MODE_STANDARD, BIZ_MODE_EXTENDED } BizMode;
typedef enum { WAVE_CHANNEL_A, WAVE_CHANNEL_B } WaveChannel;
typedef enum { WAVE_OWNER_MANUAL, WAVE_OWNER_SELFTEST } WaveOwner;
typedef enum {
    WAVE_UNINITIALIZED, WAVE_STOPPED, WAVE_PWM,
    WAVE_DC_LOW, WAVE_DC_HIGH, WAVE_FAULT
} WaveState;
typedef struct { uint32_t frequency_hz; uint16_t duty_bp; } WaveParams;
typedef struct {
    uint32_t minimum_frequency_hz, maximum_frequency_hz;
    uint16_t maximum_duty_bp, duty_step_bp, phase_step_deg;
    bool dual_pwm, phase, spwm;
} WaveCapabilities;
typedef struct {
    WaveState state;
    WaveOwner owner;
    WaveParams requested, applied;
    uint32_t calculated_frequency_millihz;
    uint16_t calculated_duty_bp;
    bool enabled, pending;
    uint32_t request_id, applied_id, cancelled_id;
    uint32_t accepted_ms, applied_ms;
    BizResult last_result;
} WaveSnapshot;
typedef enum { WAVE_SYNC_OFF, WAVE_SYNC_STARTING, WAVE_SYNC_ON, WAVE_SYNC_STOPPING } WaveSyncState;
typedef struct {
    WaveSyncState state;
    uint16_t requested_phase_deg, applied_phase_deg;
    uint32_t calculated_phase_millideg;
    bool phase_pending, phase_applicable;
    BizResult last_result;
} WaveSyncSnapshot;

BizResult Wave_Init(void);
void Wave_Process(uint32_t now_ms);
BizResult Wave_SetMode(BizMode mode);
BizMode Wave_GetMode(void);
void Wave_GetCapabilities(WaveCapabilities *out);
BizResult Wave_Apply(WaveChannel channel, WaveOwner owner, const WaveParams *params, uint32_t *request_id);
BizResult Wave_SetEnabled(WaveChannel channel, WaveOwner owner, bool enabled, uint32_t *request_id);
void Wave_GetSnapshot(WaveChannel channel, WaveSnapshot *out);
BizResult Wave_SetSync(bool enabled);
BizResult Wave_SetPhase(uint16_t phase_deg);
void Wave_GetSyncSnapshot(WaveSyncSnapshot *out);
BizResult Wave_AcquireA(WaveOwner owner);
BizResult Wave_ReleaseA(WaveOwner owner);

typedef struct {
    bool initialized, running;
    uint32_t carrier_hz, sine_hz;
    uint16_t modulation_bp;
    BizResult last_result;
} SpwmSnapshot;
BizResult Spwm_Init(void);
BizResult Spwm_Start(void);
BizResult Spwm_Stop(void);
void Spwm_GetSnapshot(SpwmSnapshot *out);

typedef enum { INPUT_WAITING, INPUT_VALID, INPUT_NO_SIGNAL, INPUT_OUT_OF_RANGE, INPUT_ERROR } InputState;
typedef struct {
    InputState state;
    BizMode mode;
    uint32_t frequency_millihz;
    uint16_t duty_bp;
    uint32_t period_ticks, high_ticks;
    uint32_t sequence, acquisition_id;
    uint32_t window_start_ms, window_end_ms, published_ms;
    uint32_t overcapture_count, buffer_overrun_count, pairing_error_count;
    BizResult last_result;
} InputSnapshot;
typedef struct {
    uint32_t minimum_frequency_hz, maximum_frequency_hz, no_edge_timeout_ms;
} InputCapabilities;
BizResult Input_Init(void);
void Input_Process(uint32_t now_ms);
BizResult Input_SetMode(BizMode mode);
BizMode Input_GetMode(void);
void Input_GetCapabilities(InputCapabilities *out);
BizResult Input_BeginWindow(uint32_t *acquisition_id);
void Input_GetSnapshot(InputSnapshot *out);

typedef enum {
    SWEEP_IDLE, SWEEP_RUNNING, SWEEP_RESTORING,
    SWEEP_PASS, SWEEP_FAIL, SWEEP_CANCELLED
} SweepState;
typedef enum {
    SWEEP_POINT_PENDING, SWEEP_POINT_PASS, SWEEP_POINT_OUT_OF_TOLERANCE,
    SWEEP_POINT_NO_SIGNAL, SWEEP_POINT_OUTPUT_ERROR, SWEEP_POINT_CAPTURE_ERROR
} SweepPointState;
typedef struct {
    uint32_t set_frequency_hz, measured_frequency_millihz;
    float error_percent;
    SweepPointState state;
} SweepPoint;
typedef struct {
    SweepState state;
    uint8_t completed, current_index;
    bool restored, owns_a;
    BizResult last_result;
    SweepPoint points[LAB2_SWEEP_POINT_COUNT];
} SweepSnapshot;
BizResult Sweep_Init(void);
BizResult Sweep_Start(void);
BizResult Sweep_Cancel(void);
void Sweep_Process(uint32_t now_ms);
void Sweep_GetSnapshot(SweepSnapshot *out);

#ifdef __cplusplus
}
#endif
#endif
