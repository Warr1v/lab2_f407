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
#define SPWM_TIMER_HZ           84000000U
#define SPWM_CARRIER_HZ         20000U
#define SPWM_SINE_HZ            50U
#define SPWM_SAMPLE_COUNT       400U
#define SPWM_MODULATION_BP      8000U
#define SPWM_DMA_IRQ_PRIORITY   10U

/* Release firmware never changes parameters automatically. */
#ifndef WAVE_DIAGNOSTIC_ENABLE
#define WAVE_DIAGNOSTIC_ENABLE 0
#endif

_Static_assert(WAVE_MIN_HZ > 0U && WAVE_MIN_HZ <= WAVE_STANDARD_MAX_HZ &&
               WAVE_STANDARD_MAX_HZ <= WAVE_EXTENDED_MAX_HZ, "Invalid Wave range");
_Static_assert(WAVE_STANDARD_STEP_BP > 0U && WAVE_EXTENDED_STEP_BP > 0U &&
               WAVE_MAX_DUTY_BP <= 10000U && WAVE_MAX_DUTY_BP % WAVE_STANDARD_STEP_BP == 0U &&
               WAVE_MAX_DUTY_BP % WAVE_EXTENDED_STEP_BP == 0U, "Invalid duty step/endpoints");
_Static_assert(WAVE_PHASE_STEP_DEG > 0U && 360U % WAVE_PHASE_STEP_DEG == 0U, "Invalid phase step");
_Static_assert(WAVE_DEFAULT_HZ >= WAVE_MIN_HZ && WAVE_DEFAULT_HZ <= WAVE_STANDARD_MAX_HZ &&
               WAVE_DEFAULT_DUTY_BP <= WAVE_MAX_DUTY_BP &&
               WAVE_DEFAULT_DUTY_BP % WAVE_STANDARD_STEP_BP == 0U, "Invalid startup parameters");

#endif
