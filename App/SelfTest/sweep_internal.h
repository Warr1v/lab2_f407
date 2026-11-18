#ifndef LAB2_SWEEP_INTERNAL_H
#define LAB2_SWEEP_INTERNAL_H
#include "business_api.h"
typedef enum {
    SWEEP_STEP_ENABLE, SWEEP_STEP_ENABLE_WAIT,
    SWEEP_STEP_APPLY, SWEEP_STEP_APPLY_WAIT, SWEEP_STEP_SETTLE,
    SWEEP_STEP_WINDOW, SWEEP_STEP_SAMPLE,
    SWEEP_STEP_RESTORE_DRAIN, SWEEP_STEP_RESTORE_PARAMS,
    SWEEP_STEP_RESTORE_PARAMS_WAIT, SWEEP_STEP_RESTORE_ENABLE,
    SWEEP_STEP_RESTORE_ENABLE_WAIT, SWEEP_STEP_RELEASE,
    SWEEP_STEP_EMERGENCY_OFF, SWEEP_STEP_EMERGENCY_WAIT, SWEEP_STEP_DONE
} SweepStep;
typedef struct {
    SweepStep step;
    uint32_t request_id, acquisition_id, phase_started_ms, wait_budget_ms;
    BizResult first_failure, restore_result, emergency_result;
    bool cancelled;
} SweepDiagnostics;
/* Private diagnostics do not change P1 business ABI. Main-loop access only. */
void Sweep_GetDiagnostics(SweepDiagnostics *out);
uint32_t SweepPort_Now(void);
#endif
