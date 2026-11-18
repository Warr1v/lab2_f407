#include "wave_math.h"

bool Wave_Calculate(uint32_t clock_hz, uint32_t frequency_hz,
                    uint16_t duty_bp, WaveTiming *out)
{
    if (!out || !frequency_hz || !clock_hz || duty_bp > 10000U) return false;
    /* Keep N <= 65535, so CCR=N represents true 100% on a 16-bit timer. */
    const uint64_t maximum = (uint64_t)frequency_hz * 65535U;
    const uint32_t divider = (uint32_t)(((uint64_t)clock_hz + maximum - 1U) / maximum);
    if (!divider || divider > 65536U) return false;
    const uint64_t denominator = (uint64_t)frequency_hz * divider;
    const uint32_t counts = (uint32_t)(((uint64_t)clock_hz + denominator / 2U) / denominator);
    if (counts < 2U || counts > 65535U) return false;
    const uint32_t compare = (uint32_t)(((uint64_t)counts * duty_bp + 5000U) / 10000U);
    const uint64_t period = (uint64_t)divider * counts;
    *out = (WaveTiming){divider, counts, compare,
        (uint32_t)(((uint64_t)clock_hz * 1000U + period / 2U) / period),
        (uint16_t)(((uint64_t)compare * 10000U + counts / 2U) / counts)};
    return true;
}
