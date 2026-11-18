#include "input_port.h"
#include "main.h"
#include <string.h>

/* CMSIS registers are used for the DMA hot path; GPIO/RCC use HAL.
 * TIM5 owns only these two streams. Never reset the entire DMA1 controller. */
static volatile uint32_t rising[INPUT_DMA_LENGTH], falling[INPUT_DMA_LENGTH];
static InputEdges mailbox;
static volatile uint32_t latched_faults;
static bool running, rising_full, falling_full;
static uint32_t expected_boundary;
static InputPortDiagnostics diagnostics;
_Static_assert(INPUT_DMA_LENGTH >= 2U * INPUT_EDGE_HISTORY &&
               !(INPUT_DMA_LENGTH & (INPUT_DMA_LENGTH - 1U)), "DMA ring must be a power of two");

#define R_FLAGS (DMA_HISR_FEIF4 | DMA_HISR_DMEIF4 | DMA_HISR_TEIF4 | DMA_HISR_HTIF4 | DMA_HISR_TCIF4)
#define F_FLAGS (DMA_LISR_FEIF2 | DMA_LISR_DMEIF2 | DMA_LISR_TEIF2 | DMA_LISR_HTIF2 | DMA_LISR_TCIF2)
#define R_ERRORS (DMA_HISR_FEIF4 | DMA_HISR_DMEIF4 | DMA_HISR_TEIF4)
#define F_ERRORS (DMA_LISR_FEIF2 | DMA_LISR_DMEIF2 | DMA_LISR_TEIF2)
#define CAPTURE_ENABLE (TIM_CCER_CC1E | TIM_CCER_CC2E)
#define CAPTURE_REQUEST (TIM_DIER_CC1DE | TIM_DIER_CC2DE)

static uint32_t lock(void) { uint32_t p = __get_PRIMASK(); __disable_irq(); return p; }
static void unlock(uint32_t p) { __set_PRIMASK(p); }

void InputPort_Stop(void)
{
    /* A late DMA error can latch after flags were sampled. Mask these vectors
     * until Restart clears ALL flags, so a stopped fault cannot IRQ-storm and
     * starve the main-loop recovery. No other DMA stream is affected. */
    NVIC_DisableIRQ(DMA1_Stream4_IRQn);
    NVIC_DisableIRQ(DMA1_Stream2_IRQn);
    TIM5->CCER &= ~CAPTURE_ENABLE;
    TIM5->DIER &= ~CAPTURE_REQUEST;
    DMA1_Stream4->CR &= ~DMA_SxCR_EN;
    DMA1_Stream2->CR &= ~DMA_SxCR_EN;
    running = false;
}

static bool streams_disabled(void)
{
    /* Hardware EN clear is normally immediate after a single word transfer.
     * Strict bound avoids a dead peripheral freezing the UI/tick. */
    for (unsigned i = 0; i < 64U; ++i)
        if (!((DMA1_Stream4->CR | DMA1_Stream2->CR) & DMA_SxCR_EN)) return true;
    return false;
}

bool InputPort_Restart(void)
{
    uint32_t p = lock();
    InputPort_Stop();
    if (!streams_disabled()) {
        latched_faults |= INPUT_FAULT_DMA;
        unlock(p);
        return false;
    }
    DMA1->HIFCR = R_FLAGS;
    DMA1->LIFCR = F_FLAGS;
    NVIC_ClearPendingIRQ(DMA1_Stream4_IRQn);
    NVIC_ClearPendingIRQ(DMA1_Stream2_IRQn);
    TIM5->SR = 0U;
    memset(&mailbox, 0, sizeof(mailbox));
    latched_faults = 0U;
    rising_full = falling_full = false;
    expected_boundary = DMA_HISR_HTIF4;
    uint32_t common = (6U << DMA_SxCR_CHSEL_Pos) | DMA_SxCR_PL |
        DMA_SxCR_MSIZE_1 | DMA_SxCR_PSIZE_1 | DMA_SxCR_MINC |
        DMA_SxCR_CIRC | DMA_SxCR_TEIE | DMA_SxCR_DMEIE;
    DMA1_Stream4->CR = common | DMA_SxCR_HTIE | DMA_SxCR_TCIE;
    DMA1_Stream2->CR = common;
    DMA1_Stream4->FCR = DMA_SxFCR_FEIE;
    DMA1_Stream2->FCR = DMA_SxFCR_FEIE;
    DMA1_Stream4->PAR = (uint32_t)&TIM5->CCR2;
    DMA1_Stream2->PAR = (uint32_t)&TIM5->CCR1;
    DMA1_Stream4->M0AR = (uint32_t)rising;
    DMA1_Stream2->M0AR = (uint32_t)falling;
    DMA1_Stream4->NDTR = DMA1_Stream2->NDTR = INPUT_DMA_LENGTH;
    __DMB();
    DMA1_Stream2->CR |= DMA_SxCR_EN;
    DMA1_Stream4->CR |= DMA_SxCR_EN;
    TIM5->DIER = CAPTURE_REQUEST;
    running = true;
    TIM5->CCER |= CAPTURE_ENABLE; /* simultaneous arming; discard partial first cycle */
    NVIC_EnableIRQ(DMA1_Stream4_IRQn);
    NVIC_EnableIRQ(DMA1_Stream2_IRQn);
    unlock(p);
    return true;
}

bool InputPort_Init(void)
{
    uint32_t clock = HAL_RCC_GetPCLK1Freq();
    if ((RCC->CFGR & RCC_CFGR_PPRE1) != 0U) clock *= 2U;
    if (clock != INPUT_TIMER_HZ) return false;
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_DMA1_CLK_ENABLE();
    __HAL_RCC_TIM5_CLK_ENABLE();
    GPIO_InitTypeDef gpio = {0};
    gpio.Pin = GPIO_PIN_1;
    gpio.Mode = GPIO_MODE_AF_PP;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    gpio.Alternate = GPIO_AF2_TIM5;
    HAL_GPIO_Init(GPIOA, &gpio);
    TIM5->CR1 = 0U;
    TIM5->DIER = 0U;
    TIM5->CCER = TIM_CCER_CC1P;
    TIM5->SMCR = 0U; /* free running, no reset between rising edges */
    TIM5->PSC = 0U;
    TIM5->ARR = UINT32_MAX;
    /* CC1S=10: indirect TI2, falling; CC2S=01: direct TI2, rising.
     * No prescale/filter: a short pulse must not be silently filtered out. */
    TIM5->CCMR1 = TIM_CCMR1_CC1S_1 | TIM_CCMR1_CC2S_0;
    TIM5->EGR = TIM_EGR_UG;
    TIM5->SR = 0U;
    TIM5->CR1 = TIM_CR1_CEN;
    HAL_NVIC_SetPriority(DMA1_Stream4_IRQn, INPUT_DMA_IRQ_PRIORITY, 0U);
    HAL_NVIC_SetPriority(DMA1_Stream2_IRQn, INPUT_DMA_IRQ_PRIORITY, 0U);
    return true;
}

uint32_t InputPort_Now(void) { return HAL_GetTick(); }

uint32_t InputPort_Faults(void)
{
    uint32_t f = latched_faults;
    if (TIM5->SR & (TIM_SR_CC1OF | TIM_SR_CC2OF)) f |= INPUT_FAULT_OVERCAPTURE;
    if ((DMA1->HISR & R_ERRORS) || (DMA1->LISR & F_ERRORS)) f |= INPUT_FAULT_DMA;
    if (running && (!(DMA1_Stream4->CR & DMA_SxCR_EN) || !(DMA1_Stream2->CR & DMA_SxCR_EN) ||
        DMA1_Stream4->NDTR > INPUT_DMA_LENGTH || DMA1_Stream2->NDTR > INPUT_DMA_LENGTH))
        f |= INPUT_FAULT_DMA;
    if ((DMA1->HISR & (DMA_HISR_HTIF4 | DMA_HISR_TCIF4)) ==
        (DMA_HISR_HTIF4 | DMA_HISR_TCIF4)) f |= INPUT_FAULT_OVERRUN;
    return f;
}

static void service_locked(void)
{
    if (!running) return;
    uint32_t faults = InputPort_Faults();
    uint32_t rflags = DMA1->HISR & R_FLAGS;
    uint32_t fflags = DMA1->LISR & F_FLAGS;
    uint32_t boundary = rflags & (DMA_HISR_HTIF4 | DMA_HISR_TCIF4);
    if (boundary) {
        if (boundary != expected_boundary) faults |= INPUT_FAULT_OVERRUN;
        expected_boundary = boundary == DMA_HISR_HTIF4 ? DMA_HISR_TCIF4 : DMA_HISR_HTIF4;
    }
    if (rflags & DMA_HISR_TCIF4) rising_full = true;
    if (fflags & DMA_LISR_TCIF2) falling_full = true;
    DMA1->HIFCR = rflags;
    DMA1->LIFCR = fflags;
    if (faults) {
        latched_faults |= faults;
        InputPort_Stop();
        return;
    }
    /* DMA keeps writing NEXT slots while we read PRECEDING slots. Validate
     * that neither producer reached the suffix being copied, rather than
     * requiring the two high-rate DMA cursors to stand still together. */
    for (unsigned retry = 0; retry < INPUT_CURSOR_RETRIES; ++retry) {
        uint32_t rn = DMA1_Stream4->NDTR, fn = DMA1_Stream2->NDTR;
        if (!rn || !fn || rn > INPUT_DMA_LENGTH || fn > INPUT_DMA_LENGTH) continue;
        uint32_t rp = INPUT_DMA_LENGTH - rn, fp = INPUT_DMA_LENGTH - fn;
        InputEdges candidate = {0};
        candidate.rising_count = (uint8_t)(rising_full || rp >= INPUT_EDGE_HISTORY ? INPUT_EDGE_HISTORY : rp);
        candidate.falling_count = (uint8_t)(falling_full || fp >= INPUT_EDGE_HISTORY ? INPUT_EDGE_HISTORY : fp);
        __DMB();
        for (unsigned i = 0; i < candidate.rising_count; ++i)
            candidate.rising[i] = rising[(rp + INPUT_DMA_LENGTH - 1U - i) % INPUT_DMA_LENGTH];
        for (unsigned i = 0; i < candidate.falling_count; ++i)
            candidate.falling[i] = falling[(fp + INPUT_DMA_LENGTH - 1U - i) % INPUT_DMA_LENGTH];
        candidate.timer_now = TIM5->CNT;
        candidate.observed_ms = HAL_GetTick();
        __DMB();
        uint32_t ra = DMA1_Stream4->NDTR, fa = DMA1_Stream2->NDTR;
        if (!ra || !fa || ra > INPUT_DMA_LENGTH || fa > INPUT_DMA_LENGTH ||
            (rn + INPUT_DMA_LENGTH - ra) % INPUT_DMA_LENGTH > INPUT_DMA_LENGTH - INPUT_EDGE_HISTORY ||
            (fn + INPUT_DMA_LENGTH - fa) % INPUT_DMA_LENGTH > INPUT_DMA_LENGTH - INPUT_EDGE_HISTORY) {
            ++diagnostics.cursor_retry_count;
            continue;
        }
        if (InputPort_Faults()) { latched_faults |= InputPort_Faults(); InputPort_Stop(); return; }
        /* Preserve the time anchor of an unchanged suffix. Re-observing old
         * CCR history after a full TIM5 wrap must not rejuvenate its age. */
        if (candidate.rising_count == mailbox.rising_count &&
            candidate.falling_count == mailbox.falling_count &&
            (!candidate.rising_count || candidate.rising[0] == mailbox.rising[0]) &&
            (!candidate.falling_count || candidate.falling[0] == mailbox.falling[0])) return;
        mailbox = candidate;
        return;
    }
    /* Cursor motion is not lost data: keep the preceding coherent mailbox,
     * retry at the next block IRQ or Process call, without inventing a fault. */
    ++diagnostics.cursor_deferred_count;
}

static uint32_t cycle_now(void)
{
    return DWT->CYCCNT;
}
static void cycle_max(uint32_t start, uint32_t *maximum)
{
    if ((CoreDebug->DEMCR & CoreDebug_DEMCR_TRCENA_Msk) && (DWT->CTRL & DWT_CTRL_CYCCNTENA_Msk)) {
        uint32_t elapsed = cycle_now() - start;
        if (elapsed > *maximum) *maximum = elapsed;
        diagnostics.cycle_counter_available = true;
    }
}
void InputPort_Service(void)
{
    uint32_t start = cycle_now();
    uint32_t p = lock();
    ++diagnostics.service_count;
    service_locked();
    cycle_max(start, &diagnostics.max_service_cycles);
    unlock(p);
}
void InputPort_Read(InputEdges *out) { uint32_t p = lock(); *out = mailbox; unlock(p); }
void Input_DMA1_Stream4_IRQHandler(void)
{
    uint32_t start = cycle_now();
    ++diagnostics.rising_irq_count;
    InputPort_Service();
    cycle_max(start, &diagnostics.max_irq_cycles);
}
void Input_DMA1_Stream2_IRQHandler(void)
{
    uint32_t start = cycle_now();
    ++diagnostics.falling_irq_count;
    InputPort_Service();
    cycle_max(start, &diagnostics.max_irq_cycles);
}
void InputPort_GetDiagnostics(InputPortDiagnostics *out)
{
    if (!out) return;
    uint32_t p = lock();
    *out = diagnostics;
    out->rising_ndtr = DMA1_Stream4->NDTR;
    out->falling_ndtr = DMA1_Stream2->NDTR;
    out->latched_faults = InputPort_Faults();
    unlock(p);
}
