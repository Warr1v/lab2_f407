#ifndef LAB2_WAVE_MATH_H
#define LAB2_WAVE_MATH_H
#include <stdbool.h>
#include <stdint.h>
typedef struct {
    uint32_t divider, counts, compare;
    uint32_t frequency_millihz;
    uint16_t duty_bp;
} WaveTiming;
bool Wave_Calculate(uint32_t clock_hz, uint32_t frequency_hz,
                    uint16_t duty_bp, WaveTiming *out);
#endif
