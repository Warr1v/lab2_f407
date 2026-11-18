# Wave staged delivery

Base: `0df814ca1bb0c6a713d38eae72504c2ba12749af`; branch `feature/wave-output`.
Only this worktree is changed. The shared report outside the repository is not part of commits.

## W1

A: PA8 AF1 TIM1_CH1. Direct CMSIS timer registers behind `wave_hw.c`, HAL clock/GPIO/NVIC utilities. No HAL callback overrides. Public timebase is unchanged. Only a pending update arms TIM1_UP_TIM10 at priority 6; the first update acknowledges and disables UIE. Actual preload transfer occurs in hardware regardless of UI service latency. `applied_ms` is the HAL tick at acknowledgement (not a scope timestamp); `accepted_ms` is the request acceptance tick.

Stopped output remains physically low while parameters are saved. 0/100% use CCR=0/N with N<=65535, no forced PWM-mode switch. A running update never generates UG or restarts CNT. Stop cancels only an unlatched request; it drains an already-latched update before cancellation, then forces low. Duplicate start is idempotent. No pending overwrite; manual and SELFTEST A ownership is enforced.

Build Debug/Release using the P1 toolchain. `tests/verify_wave.py --stage W1` runs real ARM code in Unicorn with explicit MMIO update injection. It checks transactional semantics and numeric bounds, not silicon, signal edges, IRQ latency or instrument accuracy. B/sync/extended/SPWM are not advertised in this stage.

2026-11-18 checks: Debug 49124 B Flash / 3480 B RAM; Release 38292 B Flash / 3456 B RAM; no warnings. W1 real-ELF API checks passed; 8410 compiled calculation cases passed (worst sampled frequency 0.148621%, duty 0.148368 points). Existing snake/UI/stopwatch emulator regression passed, including 12 automatic wins in 42137 steps. No flashing or physical measurements.

Public differences: CMake source/includes/API retention, main Wave_Init/Process, shared IRQ forwarding. CubeMX is NOT run automatically; final resource configuration and regeneration ownership are described with later stages.

## W2

B: PC7 AF3 TIM8_CH2, independent frequency/duty/start/stop. TIM1 CH2 is internal PWM2 (no PA9 configuration); nonzero phase TRGO=OC2REF; zero uses UPDATE. TIM8 ITR0 reset mode uses a 3x prescaler and large ARR, preserving complete high pulses while extending low intervals during delayed phase changes. B timing is quantized in 3 A ticks; calculated duty reports this, not the independent CCR/N ratio.

First join waits for B's natural update, then a complete A boundary, and connects only after that A event. The first reset occurs at the following A event, extending a low interval rather than inserting a short pulse. Exit preloads independent B timing, waits for a final hardware reset transfer, then disconnects. No running timer is stopped or software-UG restarted by these operations. Nonzero phase advances are limited to half the available B low interval per step. Large advances at 99%/low frequency may take many periods and stay pending; UI must poll, never wait. Zero transitions deliberately extend a low interval. OC2REF selection first installs an inactive active compare before preloading the target, avoiding a false edge if the CPU is slower than the requested phase delay.

One-shot TIM1/TIM8 update IRQs also progress synchronization. No persistent period IRQ or global HAL callback. Raw handler forwarding must coexist with any future TIM10/TIM13 sharing. Neither timer is allocated by P1.

2026-11-18: Debug 51356 B Flash / 3512 B RAM, Release 39804 B / 3512 B; no warnings. Actual ARM API tests pass dual independent operation, all 73 phase grid values forward/reverse, large advances/wraps, DC transitions and explicit sync exit. Maximum computed phase rounding in the 1000 Hz grid was 0.003 degrees. Hardware propagation delay, first join/exit and narrow-pulse behavior remain scope tests, not emulator results.

## W3

Standard remains 1..200000 Hz / 100 bp. Extended is 1..500000 Hz / 500 bp across its entire range. Both saved channel parameter sets are validated before mode changes. No clamping, rounding, pending overwrite, synchronization change or SELFTEST ownership bypass. Hardware timing calculation and CCR preload are the same proven 16-bit path; N=336 at 500 kHz.

2026-11-18: Debug 51452 B Flash / 3512 B RAM; Release 39872 B / 3512 B, no warnings. 336 dual-channel 5% duty cases across eight representative frequencies, invalid mode/step/range/pending transitions, and the entire forward/reverse phase grid at 500 kHz passed real-ELF controlled-MMIO tests. Maximum computed phase rounding for that 500 kHz grid was 0.357 degrees. Silicon output, ITR propagation, actual scope errors and concurrent input/SPWM load are still unmeasured.
