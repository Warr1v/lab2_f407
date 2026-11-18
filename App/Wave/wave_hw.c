#include "wave_hw.h"
#include "wave_config.h"

TIM_TypeDef *WaveHW_Timer(WaveChannel channel)
{
    return channel == WAVE_CHANNEL_A ? TIM1 : TIM8;
}

static void write_timing(WaveChannel channel, const WaveTiming *t)
{
    TIM_TypeDef *tim = WaveHW_Timer(channel);
    tim->PSC = t->divider - 1U;
    tim->ARR = t->counts - 1U;
    if (channel == WAVE_CHANNEL_A) tim->CCR1 = t->compare;
    else tim->CCR2 = t->compare;
}

void WaveHW_Stage(WaveChannel channel, const WaveTiming *timing)
{
    /* Caller holds a short critical section. Never generate UG while running. */
    TIM_TypeDef *tim = WaveHW_Timer(channel);
    tim->CR1 |= TIM_CR1_UDIS;
    tim->SR = ~TIM_SR_UIF;
    write_timing(channel, timing);
    tim->DIER |= TIM_DIER_UIE; /* One acknowledgement interrupt, then disarm. */
    tim->CR1 &= ~TIM_CR1_UDIS;
}

void WaveHW_Start(WaveChannel channel, const WaveTiming *timing)
{
    TIM_TypeDef *tim = WaveHW_Timer(channel);
    tim->DIER = 0;
    write_timing(channel, timing);
    tim->EGR = TIM_EGR_UG;
    tim->SR = 0;
    tim->CR1 |= TIM_CR1_CEN;
}

void WaveHW_Stop(WaveChannel channel)
{
    TIM_TypeDef *tim = WaveHW_Timer(channel);
    tim->DIER = 0;
    tim->CR1 &= ~(TIM_CR1_CEN | TIM_CR1_UDIS);
    if (channel == WAVE_CHANNEL_A) tim->CCR1 = 0;
    else tim->CCR2 = 0;
    tim->EGR = TIM_EGR_UG;
    tim->SR = 0;
}

BizResult WaveHW_Init(void)
{
    if (HAL_RCC_GetPCLK2Freq() != WAVE_TIMER_HZ / 2U ||
        (RCC->CFGR & RCC_CFGR_PPRE2) != RCC_CFGR_PPRE2_DIV2) return BIZ_HARDWARE_ERROR;
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_TIM1_CLK_ENABLE();
    __HAL_RCC_TIM1_FORCE_RESET();
    __HAL_RCC_TIM1_RELEASE_RESET();
    TIM1->CR1 = TIM_CR1_ARPE | TIM_CR1_URS;
    TIM1->CCMR1 = TIM_CCMR1_OC1PE | (6U << TIM_CCMR1_OC1M_Pos);
    TIM1->CCER = TIM_CCER_CC1E;
    TIM1->BDTR = TIM_BDTR_MOE;
    const WaveTiming stopped = {3U, 56000U, 0U, 1000000U, 0U};
    WaveHW_Stop(WAVE_CHANNEL_A);
    write_timing(WAVE_CHANNEL_A, &stopped);
    TIM1->EGR = TIM_EGR_UG;
    TIM1->SR = 0;
    GPIO_InitTypeDef gpio = {0};
    gpio.Pin = GPIO_PIN_8;
    gpio.Mode = GPIO_MODE_AF_PP;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    gpio.Alternate = GPIO_AF1_TIM1;
    HAL_GPIO_Init(GPIOA, &gpio);
    HAL_NVIC_SetPriority(TIM1_UP_TIM10_IRQn, WAVE_IRQ_PRIORITY, 0);
    HAL_NVIC_ClearPendingIRQ(TIM1_UP_TIM10_IRQn);
    HAL_NVIC_EnableIRQ(TIM1_UP_TIM10_IRQn);
    return BIZ_OK;
}
