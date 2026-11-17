#ifndef SCREEN_PRIVATE_SYS_H
#define SCREEN_PRIVATE_SYS_H
#include "stm32f4xx_hal.h"
#include <stdint.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
/* F407 peripheral bit-band aliases, retained for the supplied LCD backlight. */
#define SCREEN_BIT(addr, bit) (*(volatile uint32_t *)(0x42000000UL + (((uintptr_t)(addr) - 0x40000000UL) * 32UL) + ((bit) * 4UL)))
#define PBout(n) SCREEN_BIT(&GPIOB->ODR, (n))
#endif
