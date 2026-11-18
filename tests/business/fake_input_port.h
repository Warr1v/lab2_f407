#ifndef FAKE_INPUT_PORT_H
#define FAKE_INPUT_PORT_H
#include "input_port.h"
extern InputEdges fake_edges;
extern uint32_t fake_now, fake_faults, fake_restarts;
extern bool fake_restart_ok;
void Fake_Period(uint32_t start, uint32_t period, uint32_t high, uint32_t end_ms);
#endif
