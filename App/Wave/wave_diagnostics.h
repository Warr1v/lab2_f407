#ifndef LAB2_WAVE_DIAGNOSTICS_H
#define LAB2_WAVE_DIAGNOSTICS_H
#include "business_api.h"

typedef enum {
    WAVE_TEST_APPLY = 1, WAVE_TEST_ENABLE, WAVE_TEST_MODE, WAVE_TEST_SYNC,
    WAVE_TEST_PHASE, WAVE_TEST_SPWM_START, WAVE_TEST_SPWM_STOP,
    WAVE_TEST_ACQUIRE_A, WAVE_TEST_RELEASE_A, WAVE_TEST_CASE, WAVE_TEST_ABORT
} WaveTestOperation;
typedef struct {
    uint32_t sequence; /* Write args/operation first, sequence LAST; wait ack. */
    uint32_t operation, channel, value;
    WaveParams params;
} WaveTestCommand;
typedef struct {
    WaveChannel channel;
    WaveParams params;
} WaveTestCase;
typedef struct {
    uint32_t magic, api_version, heartbeat;
    bool commands_enabled, case_active;
    uint32_t acknowledged_sequence, request_id, command_cycle, counter_before;
    uint32_t case_index, case_started_ms;
    BizResult command_result, case_result;
    WaveSnapshot a, b;
    WaveSyncSnapshot sync;
    SpwmSnapshot spwm;
} WaveTestDiagnostic;
typedef struct { uint32_t a_updates, b_updates, a_max_cycles, b_max_cycles; } WaveRuntimeStats;

extern volatile WaveTestCommand g_wave_test_command;
extern volatile WaveTestDiagnostic g_wave_diag;
extern volatile WaveRuntimeStats g_wave_runtime;
extern const WaveTestCase g_wave_test_cases[];
extern const uint32_t g_wave_test_case_count;
void WaveTest_Process(uint32_t now_ms);
#endif
