#ifndef LAB2_INPUT_PORT_H
#define LAB2_INPUT_PORT_H
#include <stdbool.h>
#include <stdint.h>
#include "input_config.h"

enum { INPUT_FAULT_OVERCAPTURE = 1U, INPUT_FAULT_OVERRUN = 2U,
       INPUT_FAULT_DMA = 4U };
/* Newest first. Each ring contains absolute free-running TIM5 timestamps.
 * This is a private hardware boundary, never a second business ABI. */
typedef struct {
    uint32_t rising[INPUT_EDGE_HISTORY], falling[INPUT_EDGE_HISTORY];
    uint32_t timer_now, observed_ms;
    uint8_t rising_count, falling_count;
} InputEdges;
typedef struct {
    uint32_t rising_irq_count, falling_irq_count, service_count;
    uint32_t cursor_retry_count, cursor_deferred_count;
    uint32_t max_service_cycles, max_irq_cycles;
    uint32_t rising_ndtr, falling_ndtr, latched_faults;
    bool cycle_counter_available;
} InputPortDiagnostics;

bool InputPort_Init(void);
bool InputPort_Restart(void);
void InputPort_Stop(void);
void InputPort_Service(void);
void InputPort_Read(InputEdges *out);
uint32_t InputPort_Faults(void);
uint32_t InputPort_Now(void);
void InputPort_GetDiagnostics(InputPortDiagnostics *out);
/* Forward only these from the corresponding shared vector entries. */
void Input_DMA1_Stream4_IRQHandler(void);
void Input_DMA1_Stream2_IRQHandler(void);
#endif
