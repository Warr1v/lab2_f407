#include "business_api.h"
#include "wave_config.h"
#include "spwm_private.h"
#include "stm32f4xx_hal.h"
#include <math.h>

_Static_assert(SPWM_SAMPLE_COUNT * SPWM_SINE_HZ == SPWM_CARRIER_HZ, "SPWM table/rate mismatch");
_Static_assert(SPWM_TIMER_HZ % SPWM_CARRIER_HZ == 0U, "Carrier must divide timer clock");
_Static_assert(SPWM_MODULATION_BP <= 10000U, "Invalid modulation index");
_Static_assert(SPWM_SAMPLE_COUNT >= 4U && SPWM_SAMPLE_COUNT <= 65535U, "DMA length");

/* Ordinary .bss SRAM, 800 bytes, no CCM/heap. Rotated by two samples because
 * CCR preload is a pipeline: active[0], preload[1], DMA[2..399,0,1]. */
static uint16_t spwm_dma[SPWM_SAMPLE_COUNT] __attribute__((aligned(4)));
static uint16_t first_sample, second_sample;
static SpwmSnapshot status;
static bool stopping;
volatile uint32_t g_spwm_dma_error_count;
#define SPWM_DMA_FLAGS (DMA_LIFCR_CFEIF1 | DMA_LIFCR_CDMEIF1 | DMA_LIFCR_CTEIF1 | DMA_LIFCR_CHTIF1 | DMA_LIFCR_CTCIF1)
#define SPWM_DMA_ERRORS (DMA_LISR_FEIF1 | DMA_LISR_DMEIF1 | DMA_LISR_TEIF1)

static uint32_t lock(void) { uint32_t p = __get_PRIMASK(); __disable_irq(); return p; }
static void unlock(uint32_t p) { __set_PRIMASK(p); }
static void low(void)
{
    TIM2->DIER = 0;
    TIM2->CR1 &= ~TIM_CR1_CEN;
    TIM2->CCR4 = 0;
    TIM2->EGR = TIM_EGR_UG;
    TIM2->SR = 0;
    DMA1_Stream1->CR &= ~DMA_SxCR_EN;
    status.running = false;
}
bool Spwm_Ready(void) { return status.initialized; }
void Spwm_Service(void)
{
    if (stopping && !(DMA1_Stream1->CR & DMA_SxCR_EN)) {
        stopping = false;
        if (status.last_result == BIZ_PENDING) status.last_result = BIZ_OK;
    }
}
BizResult Spwm_Init(void)
{
    if (status.initialized) return BIZ_OK;
    if (HAL_RCC_GetPCLK1Freq() != SPWM_TIMER_HZ / 2U ||
        (RCC->CFGR & RCC_CFGR_PPRE1) != RCC_CFGR_PPRE1_DIV4) {
        status.last_result = BIZ_HARDWARE_ERROR;
        return status.last_result;
    }
    __HAL_RCC_DMA1_CLK_ENABLE(); /* Never reset shared DMA1 (TIM5 uses it). */
    if (DMA1_Stream1->CR & DMA_SxCR_EN) { status.last_result = BIZ_BUSY; return BIZ_BUSY; }
    __HAL_RCC_TIM2_CLK_ENABLE();
    __HAL_RCC_TIM2_FORCE_RESET();
    __HAL_RCC_TIM2_RELEASE_RESET();
    __HAL_RCC_GPIOA_CLK_ENABLE();
    TIM2->CR1 = TIM_CR1_ARPE | TIM_CR1_URS;
    TIM2->PSC = 0;
    TIM2->ARR = SPWM_TIMER_HZ / SPWM_CARRIER_HZ - 1U;
    TIM2->CCMR2 = TIM_CCMR2_OC4PE | (6U << TIM_CCMR2_OC4M_Pos);
    TIM2->CCER = TIM_CCER_CC4E;
    low();
    GPIO_InitTypeDef gpio = {0};
    gpio.Pin = GPIO_PIN_3;
    gpio.Mode = GPIO_MODE_AF_PP;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    gpio.Alternate = GPIO_AF1_TIM2;
    HAL_GPIO_Init(GPIOA, &gpio);
    for (uint32_t i = 0; i < SPWM_SAMPLE_COUNT; ++i) {
        const float angle = 6.28318530718f * (float)i / (float)SPWM_SAMPLE_COUNT;
        const float duty = 0.5f + ((float)SPWM_MODULATION_BP / 20000.0f) * sinf(angle);
        const uint16_t value = (uint16_t)((float)(TIM2->ARR + 1U) * duty + 0.5f);
        if (i == 0U) first_sample = value;
        if (i == 1U) second_sample = value;
        spwm_dma[(i + SPWM_SAMPLE_COUNT - 2U) % SPWM_SAMPLE_COUNT] = value;
    }
    DMA1->LIFCR = SPWM_DMA_FLAGS;
    HAL_NVIC_SetPriority(DMA1_Stream1_IRQn, SPWM_DMA_IRQ_PRIORITY, 0);
    HAL_NVIC_ClearPendingIRQ(DMA1_Stream1_IRQn);
    HAL_NVIC_EnableIRQ(DMA1_Stream1_IRQn);
    status = (SpwmSnapshot){true, false, SPWM_CARRIER_HZ, SPWM_SINE_HZ, SPWM_MODULATION_BP, BIZ_OK};
    return BIZ_OK;
}
BizResult Spwm_Start(void)
{
    if (!status.initialized) return BIZ_NOT_INITIALIZED;
    Spwm_Service();
    if (status.running) return BIZ_OK;
    if (stopping || (DMA1_Stream1->CR & DMA_SxCR_EN)) return BIZ_BUSY;
    const uint32_t p = lock();
    TIM2->DIER = 0;
    TIM2->CCR4 = first_sample;
    TIM2->EGR = TIM_EGR_UG;
    TIM2->CCR4 = second_sample;
    TIM2->SR = 0;
    DMA1_Stream1->CR = (3U << DMA_SxCR_CHSEL_Pos) | DMA_SxCR_PL_0 |
        DMA_SxCR_MSIZE_0 | DMA_SxCR_PSIZE_0 | DMA_SxCR_MINC | DMA_SxCR_CIRC |
        DMA_SxCR_DIR_0 | DMA_SxCR_TEIE | DMA_SxCR_DMEIE;
    DMA1_Stream1->NDTR = SPWM_SAMPLE_COUNT;
    DMA1_Stream1->PAR = (uint32_t)&TIM2->CCR4;
    DMA1_Stream1->M0AR = (uint32_t)spwm_dma;
    DMA1_Stream1->FCR = DMA_SxFCR_FEIE; /* Direct mode, errors only, no TC/HT IRQ. */
    DMA1->LIFCR = SPWM_DMA_FLAGS;
    __DMB();
    DMA1_Stream1->CR |= DMA_SxCR_EN;
    TIM2->DIER = TIM_DIER_UDE; /* TIM2 UPDATE request, NOT CC4DE. */
    status.running = true;
    status.last_result = BIZ_OK;
    TIM2->CR1 |= TIM_CR1_CEN;
    unlock(p);
    return BIZ_OK;
}
BizResult Spwm_Stop(void)
{
    if (!status.initialized) return BIZ_NOT_INITIALIZED;
    const uint32_t p = lock();
    low();
    stopping = (DMA1_Stream1->CR & DMA_SxCR_EN) != 0U;
    DMA1->LIFCR = SPWM_DMA_FLAGS;
    status.last_result = stopping ? BIZ_PENDING : BIZ_OK;
    const BizResult result = status.last_result;
    unlock(p);
    return result;
}
void Spwm_GetSnapshot(SpwmSnapshot *out)
{
    if (!out) return;
    const uint32_t p = lock();
    *out = status;
    if (!status.initialized && status.last_result == BIZ_OK) out->last_result = BIZ_NOT_INITIALIZED;
    unlock(p);
}
void Spwm_DMA1_Stream1IRQ(void)
{
    const uint32_t flags = DMA1->LISR & SPWM_DMA_FLAGS;
    if (flags) DMA1->LIFCR = flags;
    if ((flags & SPWM_DMA_ERRORS) && status.running) {
        low();
        ++g_spwm_dma_error_count;
        stopping = (DMA1_Stream1->CR & DMA_SxCR_EN) != 0U;
        status.last_result = BIZ_HARDWARE_ERROR;
    }
}
