#ifndef FAKE_SWEEP_BUSINESS_H
#define FAKE_SWEEP_BUSINESS_H
#include "business_api.h"
extern uint32_t fake_now, fake_writes, fake_b_calls, fake_next_id;
extern uint32_t fake_bad_frequency, fake_reject_frequency;
extern bool fake_stall, fake_stall_restore, fake_bad_id, fake_reject_enable, fake_reject_release;
extern unsigned fake_capture;
extern WaveSnapshot fake_a, fake_b;
extern WaveSyncSnapshot fake_sync;
extern BizMode fake_wave_mode, fake_input_mode;
extern bool fake_input_ready;
void Fake_Reset(bool enabled, uint32_t hz, uint16_t duty);
void Fake_Tick(uint32_t delta);
#endif
