#ifndef LAB2_SPWM_PRIVATE_H
#define LAB2_SPWM_PRIVATE_H
#include <stdbool.h>
bool Spwm_Ready(void);
void Spwm_Service(void);
void Spwm_DMA1_Stream1IRQ(void);
#endif
