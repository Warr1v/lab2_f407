# Input integration and validation

Baseline: `0df814ca1bb0c6a713d38eae72504c2ba12749af`, branch
`feature/input-capture`. Public header is an unchanged P1 copy.

## I1 implementation

`Input_Init()` configures PA1 AF2 / TIM5 at 84 MHz, free-running 32-bit,
CH2 rising direct TI2 and CH1 falling indirect TI2. CMSIS timer/DMA configuration
is private to `input_port_stm32.c`; HAL owns RCC/GPIO and the existing tick.
No HAL timer handle, global HAL callbacks or other timer is used. The `.ioc`
baseline is deliberately unchanged: these runtime-owned resources must not be
initialized a second time by a future CubeMX regeneration.

DMA1 S4/C6 and S2/C6 transfer every rising/falling capture into independent
512-word circular SRAM rings (4096 bytes total), word alignment, very high
priority, direct mode. Rising half/full events service a bounded four-edge
suffix; falling stream interrupts only on DMA errors. Vector entries forward
to `Input_DMA1_Stream4_IRQHandler` / `Input_DMA1_Stream2_IRQHandler`. Priority 4
is reserved for both, above HAL tick priority 15; no per-edge interrupt is used.
At 200 kHz the block IRQ rate is 781.25/s; at 500 kHz it is 1953.125/s.

Both NDTR advances are checked after copying the suffix (three attempts max).
DMA may continue writing subsequent slots; neither producer may reach a copied
slot. It is unnecessary to freeze both producers within a 500 kHz half-period.
No DMA samples are read as a coherent pair merely because two CCRs were read in
sequence. Core selects adjacent rising timestamps and exactly one fall strictly
inside that interval. Modular subtraction handles TIM5 wrap. Rise/fall transfer
latency at the suffix boundary defers publication. A missing/extra edge,
overcapture, DMA error or missed half/full boundary invalidates data and rearms.
Stop masks only the two owned DMA vectors until Restart clears late flags and
pending IRQs, preventing a late transfer-error IRQ from starving main recovery.

Only the newest coherent suffix is retained, intentionally decimating complete
cycles rather than queuing all measurements for UI. Main-loop stalls do not
stop DMA/block IRQs. If interrupt service itself misses a half-buffer interval,
both latched boundary flags or the wrong boundary order cause a reported error.
Consecutive block flags cannot count arbitrarily many missed wraps: ambiguous
history is rejected and rearmed, never interpreted as a correct stream.

`Input_BeginWindow` gates both captures/requests, disables both streams with a
bounded EN-clear check, clears flags/pending IRQs and cursors, then rearms both
capture enables together. No pre-call DMA record survives. Only a subsequent
rise/fall/rise cycle can publish for the new acquisition ID. Mode changes use
the same fence; automatic error/no-edge recovery flushes incomplete records.

Input is serviced before the existing ScreenApp in main, regardless of page.
Public reads copy results; sequence/time never advance on read. The 1500 ms
no-edge policy also rearms before reconnect. Millisecond sampling bounds derive
from an observed TIM5 count and HAL tick, with up to one millisecond timestamp
quantization; `published_ms` is separate. `Input_GetSnapshot` additionally rejects
latched hardware errors and aged VALID snapshots even before the next Process.

The hot port is compiled `-O2` in Debug as well as Release to keep DMA snapshots
short; `-g3` remains. IRQ does no floating point, division, drawing or allocation.
PRIMASK covers only bounded register/suffix copies; measure actual worst-case
cycles on the shared board before claiming an interrupt/tick timing guarantee.

## Software checks and remaining hardware acceptance

Configure Debug with `-DLAB2_BUILD_BUSINESS_TESTS=ON` and build. Run
`python tests/business/run_arm_tests.py build/Debug/input_logic_tests.elf`.
This isolated target replaces the hardware port only; production never links it.
I1 tests execute real ARM input core: full base frequency/duty matrix, counter
wrap, stale read, window fence contract, no-edge/reconnect, pairing and DMA faults.
`tests/business/verify_input_dma.py` exercises the production DMA port with a
register/DMA model; this remains simulated hardware evidence.

Independent 0..3.3 V signal generator, actual PA1/JP4.23 wiring, absolute accuracy,
narrow-pulse detection, 1 Hz worst-phase response <=2 s, interrupt duration and
real UI/concurrent hardware tests are still required. Confirm offset, load and
common ground, and disconnect PA8 loopback before an independent source drives
PA1. Firmware is not flashed without the shared-board coordinator's time slot.

## I2 extension

Extended mode preserves the same 84 MHz clock and capture hardware and changes
the capability to 500000 Hz. The ABI, snapshot precision and raw measured duty
remain unchanged. Both direction mode switches discard old records via the same
hardware fence; standard mode retains its original limit. Range classification
allows a one-tick capture quantization interval to straddle a capability endpoint,
without clamping the reported result or widening the advertised capability.

At 500 kHz there are 168 timer ticks/period. One period-tick quantization is
approximately 0.60% frequency uncertainty, within the 1% budget before clock and
electrical errors. This analysis does not establish physical narrow-pulse capture
or total accuracy. Two word transfers/cycle require 4 MB/s of DMA payload; at
200 kHz it is 1.6 MB/s, excluding bus overhead. At 500 kHz the half-buffer service
deadline is 512 microseconds. Measured IRQ duration and arbitration with FSMC/
SPWM are pending physical tests. Short main pauses are handled by the block IRQ
mailbox, but long interrupt masking is deliberately diagnosed as overrun.
