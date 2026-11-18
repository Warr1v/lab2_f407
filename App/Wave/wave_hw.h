#ifndef LAB2_WAVE_HW_H
#define LAB2_WAVE_HW_H
#include "stm32f4xx_hal.h"
#include "wave_math.h"
#include "business_api.h"

/* Module-owned registers; no HAL global callback, no extra tick. */
BizResult WaveHW_Init(void);
TIM_TypeDef *WaveHW_Timer(WaveChannel channel);
void WaveHW_Stage(WaveChannel channel, const WaveTiming *timing);
void WaveHW_Start(WaveChannel channel, const WaveTiming *timing);
void WaveHW_Stop(WaveChannel channel);
void Wave_TIM1_UpdateIRQ(void);
void Wave_TIM8_UpdateIRQ(void);
#endif
