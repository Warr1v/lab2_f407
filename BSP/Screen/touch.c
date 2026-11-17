/* Adapted from teaching HARDWARE/touch/touch.c: TP_Write_Byte,
 * TP_Read_AD, TP_Read_XOY and TP_Read_XY2. GPIO is initialized by CubeMX.
 * No EEPROM, automatic calibration, capacitive driver, or SPL dependencies.
 */
#include "touch.h"
#include "main.h"
#include "delay.h"

volatile ScreenTouchDiagnostics g_touch_diag;
static uint32_t last_idle_probe;
static bool previous_pen;
static uint16_t read_ad(uint8_t command);

static void pin(GPIO_TypeDef *port, uint16_t mask, bool high)
{
    port->BSRR = high ? mask : (uint32_t)mask << 16;
}

void ScreenTouch_Init(void)
{
    pin(TP_CS_N_GPIO_Port, TP_CS_N_Pin, true);
    pin(TP_SCK_GPIO_Port, TP_SCK_Pin, false);
    /* Match TP_Init's unconditional first conversion. PD1:PD0=00 returns
     * the ADC to power-down with PENIRQ enabled; do not gate this on PENIRQ. */
    delay_us(100);
    g_touch_diag.probe_x=read_ad(0xD0);
    g_touch_diag.probe_y=read_ad(0x90);
}

static uint16_t read_ad(uint8_t command)
{
    uint16_t value = 0;
    pin(TP_CS_N_GPIO_Port, TP_CS_N_Pin, false);
    pin(TP_SCK_GPIO_Port, TP_SCK_Pin, false);
    for (unsigned i = 0; i < 8; ++i) {
        pin(TP_SCK_GPIO_Port, TP_SCK_Pin, false);
        pin(TP_MOSI_GPIO_Port, TP_MOSI_Pin, (command & 0x80U) != 0);
        command <<= 1;
        delay_us(1);
        pin(TP_SCK_GPIO_Port, TP_SCK_Pin, true);
        delay_us(1);
    }
    delay_us(6);
    pin(TP_SCK_GPIO_Port, TP_SCK_Pin, false);
    delay_us(1);
    pin(TP_SCK_GPIO_Port, TP_SCK_Pin, true); /* Clear BUSY. */
    delay_us(1);
    pin(TP_SCK_GPIO_Port, TP_SCK_Pin, false);
    for (unsigned i = 0; i < 16; ++i) {
        pin(TP_SCK_GPIO_Port, TP_SCK_Pin, false);
        delay_us(1);
        pin(TP_SCK_GPIO_Port, TP_SCK_Pin, true);
        value = (uint16_t)((value << 1) |
            ((TP_MISO_GPIO_Port->IDR & TP_MISO_Pin) != 0));
        delay_us(1);
    }
    pin(TP_SCK_GPIO_Port, TP_SCK_Pin, false);
    pin(TP_CS_N_GPIO_Port, TP_CS_N_Pin, true);
    return value >> 4;
}

static uint16_t filtered(uint8_t command)
{
    uint16_t values[5];
    for (unsigned i = 0; i < 5; ++i) values[i] = read_ad(command);
    for (unsigned i = 1; i < 5; ++i) {
        uint16_t v = values[i];
        unsigned j = i;
        while (j && values[j - 1] > v) { values[j] = values[j - 1]; --j; }
        values[j] = v;
    }
    return (uint16_t)(((uint32_t)values[1] + values[2] + values[3]) / 3);
}

ScreenTouchSample ScreenTouch_Read(void)
{
    ScreenTouchSample sample = {0};
    ++g_touch_diag.polls;
    sample.pressed = (TP_PEN_GPIO_Port->IDR & TP_PEN_Pin) == 0;
    if (!sample.pressed) {
        const uint32_t now=HAL_GetTick();
        if (!last_idle_probe || (uint32_t)(now-last_idle_probe)>=200) {
            last_idle_probe=now;
            g_touch_diag.probe_x=read_ad(0xD0);
            g_touch_diag.probe_y=read_ad(0x90);
            ++g_touch_diag.idle_probes;
            sample.pressed=(TP_PEN_GPIO_Port->IDR & TP_PEN_Pin)==0;
        }
        sample.raw_x=(uint16_t)g_touch_diag.probe_x;
        sample.raw_y=(uint16_t)g_touch_diag.probe_y;
    }
    if(sample.pressed!=previous_pen) { ++g_touch_diag.pen_edges;previous_pen=sample.pressed; }
    if (!sample.pressed) return sample;
    ++g_touch_diag.pen_low_samples;
    const uint16_t x1 = filtered(0xD0), y1 = filtered(0x90);
    const uint16_t x2 = filtered(0xD0), y2 = filtered(0x90);
    const int dx = (int)x1 - x2, dy = (int)y1 - y2;
    sample.raw_x = (uint16_t)(((uint32_t)x1 + x2) / 2);
    sample.raw_y = (uint16_t)(((uint32_t)y1 + y2) / 2);
    sample.valid = (TP_PEN_GPIO_Port->IDR & TP_PEN_Pin) == 0
        && dx >= -80 && dx <= 80 && dy >= -80 && dy <= 80
        && sample.raw_x > 15 && sample.raw_x < 4080
        && sample.raw_y > 15 && sample.raw_y < 4080;
    g_touch_diag.probe_x=sample.raw_x;g_touch_diag.probe_y=sample.raw_y;
    if(sample.valid) ++g_touch_diag.valid_samples;
    else ++g_touch_diag.rejected_samples;
    return sample;
}
