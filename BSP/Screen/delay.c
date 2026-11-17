#include "delay.h"

void ScreenDelay_Init(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk; /* Do not reset a shared counter. */
}

void delay_us(u32 us)
{
    const uint32_t ticks = us * (SystemCoreClock / 1000000U);
    const uint32_t start = DWT->CYCCNT;
    while ((uint32_t)(DWT->CYCCNT - start) < ticks) { __NOP(); }
}

void delay_ms(u16 ms)
{
    HAL_Delay(ms); /* Initial LCD power-up only; UI service never calls this. */
}
