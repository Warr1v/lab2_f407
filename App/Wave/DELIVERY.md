# Wave staged delivery

Base: `0df814ca1bb0c6a713d38eae72504c2ba12749af`; branch `feature/wave-output`.
Only this worktree is changed. The shared report outside the repository is not part of commits.

## W1

A: PA8 AF1 TIM1_CH1. Direct CMSIS timer registers behind `wave_hw.c`, HAL clock/GPIO/NVIC utilities. No HAL callback overrides. Public timebase is unchanged. Only a pending update arms TIM1_UP_TIM10 at priority 6; the first update acknowledges and disables UIE. Actual preload transfer occurs in hardware regardless of UI service latency. `applied_ms` is the HAL tick at acknowledgement (not a scope timestamp); `accepted_ms` is the request acceptance tick.

Stopped output remains physically low while parameters are saved. 0/100% use CCR=0/N with N<=65535, no forced PWM-mode switch. A running update never generates UG or restarts CNT. Stop cancels only an unlatched request; it drains an already-latched update before cancellation, then forces low. Duplicate start is idempotent. No pending overwrite; manual and SELFTEST A ownership is enforced.

Build Debug/Release using the P1 toolchain. `tests/verify_wave.py --stage W1` runs real ARM code in Unicorn with explicit MMIO update injection. It checks transactional semantics and numeric bounds, not silicon, signal edges, IRQ latency or instrument accuracy. B/sync/extended/SPWM are not advertised in this stage.

2026-11-18 checks: Debug 49124 B Flash / 3480 B RAM; Release 38292 B Flash / 3456 B RAM; no warnings. W1 real-ELF API checks passed; 8410 compiled calculation cases passed (worst sampled frequency 0.148621%, duty 0.148368 points). Existing snake/UI/stopwatch emulator regression passed, including 12 automatic wins in 42137 steps. No flashing or physical measurements.

Public differences: CMake source/includes/API retention, main Wave_Init/Process, shared IRQ forwarding. CubeMX is NOT run automatically; final resource configuration and regeneration ownership are described with later stages.
