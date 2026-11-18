#ifndef LAB2_WAVE_CONFIG_H
#define LAB2_WAVE_CONFIG_H

/* Limits are business configuration, not ABI. P1 uses percentage POINTS. */
#define WAVE_MIN_HZ             1U
#define WAVE_STANDARD_MAX_HZ    200000U
#define WAVE_EXTENDED_MAX_HZ    500000U
#define WAVE_STANDARD_STEP_BP   100U
#define WAVE_EXTENDED_STEP_BP   500U
#define WAVE_MAX_DUTY_BP        10000U
#define WAVE_PHASE_STEP_DEG     5U
#define WAVE_DEFAULT_HZ         1000U
#define WAVE_DEFAULT_DUTY_BP    5000U
#define WAVE_TIMER_HZ           168000000U
#define WAVE_IRQ_PRIORITY       6U
#define WAVE_SYNC_B_SCALE       3U

/* Release firmware never changes parameters automatically. */
#ifndef WAVE_DIAGNOSTIC_ENABLE
#define WAVE_DIAGNOSTIC_ENABLE 0
#endif

#endif
