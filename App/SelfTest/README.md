# Unique sweep controller integration

This is the only sweep business controller. `App/Input` only measures; no capture
ISR calls Wave. This controller uses only the frozen P1 Wave/Input API, channel A
and the SELFTEST owner. It does not initialize Wave, alter modes, touch B/SPWM,
draw UI, or use output settings as measured data.

`Sweep_Init` creates an idle controller. `Sweep_Start` verifies readiness, sync
OFF, no pending A request and matching modes, acquires A, then snapshots the
applied parameters and enabled flag. Failed preconditions do not write output.
One Process call advances at most one bounded state; all waits are asynchronous.

The ten fixed points use 50% duty and 1% relative frequency tolerance. Enable and
apply operations wait for the accepted request's exact applied ID, pending=false,
and the expected actual target. Cancellation and fault results never count as
applied. Timeout is the pre-operation period plus 100 ms. The stable wait is the
old period plus two new periods plus 20 ms, after confirmation. BeginWindow then
flushes pre-point samples; measurement waits at most 500 ms for the exact returned
acquisition ID, a different sample sequence, consistent mode and a fresh window.
An out-of-tolerance, missing or invalid measurement is recorded and scanning
continues. Output failure aborts scanning into restoration.

All exits, including cancel during any active phase, restore while retaining A:
drain any pending operation, apply saved parameters and verify their applied ID,
restore the enabled flag and verify, then release A. Only verified restoration
permits restored=true and a PASS/CANCELLED terminal state. A failed restore enters
a bounded emergency-off attempt; FAIL retains the real SELFTEST ownership and
diagnostics for the coordinator. `Sweep_Init` and `Start` refuse to clear a held
fault. Coordinator repair/reinitialization of the system is needed; the public
ABI deliberately has no unsafe "forget ownership" escape hatch.

`Sweep_GetDiagnostics` supplies private step, request/window IDs, deadlines,
first failure, restore failure and emergency-off result without changing P1 ABI.

## Build isolation

`sweep_controller` always compiles real sweep.c. Without the other branch's
reviewed Wave implementation, it is not linked into production and there are no
fake Wave link stubs. `LAB2_BUILD_BUSINESS_TESTS=ON` builds separate test ELFs;
only those link `tests/business/fake_sweep_business.c`. Their PASS is logic-test
evidence, not a genuine PA8-to-PA1 sweep.

After reviewing a specific Wave SHA, integrate its real files and configure
`LAB2_ENABLE_SWEEP=ON`, `LAB2_WAVE_SOURCES` and `LAB2_WAVE_INCLUDE_DIRS`. An empty
source list is a configure error. The normal production entry order is
Wave_Init / Input_Init / Sweep_Init; then Wave_Process / Input_Process /
Sweep_Process / ScreenApp_Process. SPWM remains independent. The input branch
provides conditional main integration; no output initialization is executed in
the default input-only image.

## Fixed real-module software integration

On 2026-11-18, `prepare_wave_integration.py` extracted the immutable Wave commit
`8f300542703d8113984870726b86d1501683fb03` into an ignored build snapshot, with a
per-file SHA256 manifest. No output source was edited or merged. The Integration
ELF contains real Wave/Input/Sweep and no test doubles. `verify_real_sweep.py`
passed ten points, disconnected-input failure, cancellation with restoration,
manual-A rejection during ownership, and unchanged B/SPWM state. Captures are
derived from active TIM1 PSC/ARR/CCR in a controlled ARM timer/DMA model. This is
software evidence; physical signals and final coordinator acceptance remain
pending. See `App/Input/software_validation.json` for results and image hashes.

## Acceptance still pending physical hardware

With real output integrated and the coordinator's board slot, confirm safe
0..3.3 V PA8 output, common ground, disconnect the independent signal generator,
then connect PA8 to PA1. Start through the disabled-by-default debug mailbox or
the later real screen page. Observe ten actual captured results and restored
parameters/on-off state. Repeat with no wire, partial failures, every cancel
stage, original off/0%/100% states, B and SPWM running. Physical loopback, output
electrical state and full UI response are not established by the software tests.
