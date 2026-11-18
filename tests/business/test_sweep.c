#include "business_api.h"
#include "sweep_internal.h"
#include "fake_sweep_business.h"

volatile uint32_t test_checks, test_failure;
#define CHECK(x) do { ++test_checks; if (!(x)) { test_failure=__LINE__; return 1; } } while (0)
static SweepSnapshot snapshot;
static SweepDiagnostics diag;
static bool run(void)
{
    for (unsigned i=0;i<20000U;++i) {
        Fake_Tick(5U);
        Sweep_Process(fake_now);
        Sweep_GetSnapshot(&snapshot);
        if (snapshot.state!=SWEEP_RUNNING && snapshot.state!=SWEEP_RESTORING) return true;
    }
    return false;
}
static bool reach(SweepStep target)
{
    for (unsigned i=0;i<30000U;++i) {
        Sweep_GetDiagnostics(&diag);
        if (diag.step==target) return true;
        Fake_Tick(1U);
        Sweep_Process(fake_now);
        Sweep_GetSnapshot(&snapshot);
        if (snapshot.state!=SWEEP_RUNNING && snapshot.state!=SWEEP_RESTORING) return false;
    }
    return false;
}
int main(void)
{
    CHECK(Sweep_Start()==BIZ_NOT_INITIALIZED);
    CHECK(Sweep_Cancel()==BIZ_NOT_INITIALIZED);
    Fake_Reset(false,777U,10000U);
    CHECK(Sweep_Init()==BIZ_OK);
    fake_input_ready=false;
    CHECK(Sweep_Start()==BIZ_NOT_INITIALIZED && !fake_writes);
    fake_input_ready=true;
    fake_sync.state=WAVE_SYNC_ON;
    CHECK(Sweep_Start()==BIZ_BUSY && !fake_writes);
    fake_sync.state=WAVE_SYNC_OFF;
    fake_a.pending=true;
    CHECK(Sweep_Start()==BIZ_BUSY && !fake_writes);
    fake_a.pending=false;
    fake_input_mode=BIZ_MODE_EXTENDED;
    CHECK(Sweep_Start()==BIZ_INVALID_ARGUMENT && !fake_writes);
    fake_input_mode=BIZ_MODE_STANDARD;
    fake_a.state=WAVE_FAULT;
    CHECK(Sweep_Start()==BIZ_HARDWARE_ERROR && !fake_writes);
    fake_a.state=WAVE_STOPPED;
    CHECK(Sweep_Start()==BIZ_OK);
    CHECK(Sweep_Start()==BIZ_BUSY && Sweep_Init()==BIZ_BUSY);
    WaveParams manual={123U,5000U}; uint32_t request;
    CHECK(Wave_Apply(WAVE_CHANNEL_A,WAVE_OWNER_MANUAL,&manual,&request)==BIZ_BUSY);
    CHECK(run());
    CHECK(snapshot.state==SWEEP_PASS && snapshot.restored && !snapshot.owns_a && snapshot.completed==10U);
    CHECK(!fake_a.enabled && fake_a.applied.frequency_hz==777U && fake_a.applied.duty_bp==10000U);
    CHECK(fake_a.owner==WAVE_OWNER_MANUAL && !fake_b_calls);
    for (unsigned i=0;i<10U;++i) CHECK(snapshot.points[i].state==SWEEP_POINT_PASS);
    CHECK(Sweep_Cancel()==BIZ_BUSY);
    /* Original running low/high DC and PWM, disabled 1 Hz, all restored exactly. */
    static const uint16_t duties[]={0U,10000U,5000U,5000U};
    for (unsigned i=0;i<4U;++i) {
        Fake_Reset(i!=3U,i==3U?1U:777U,duties[i]);
        CHECK(Sweep_Init()==BIZ_OK && Sweep_Start()==BIZ_OK);
        CHECK(run() && snapshot.state==SWEEP_PASS);
        CHECK(fake_a.enabled==(i!=3U) && fake_a.applied.duty_bp==duties[i]);
        CHECK(fake_a.applied.frequency_hz==(i==3U?1U:777U));
    }
    /* Single point failures continue to all ten; no old sample can PASS. */
    for (unsigned mode=1U;mode<=10U;++mode) {
        Fake_Reset(true,777U,5000U);
        fake_capture=mode;
        fake_bad_frequency=500U;
        CHECK(Sweep_Init()==BIZ_OK && Sweep_Start()==BIZ_OK);
        CHECK(run());
        CHECK(snapshot.restored && !snapshot.owns_a && snapshot.completed==10U && !fake_b_calls);
        CHECK(snapshot.state==(mode==9U?SWEEP_PASS:SWEEP_FAIL));
        CHECK(snapshot.points[4].state==(mode==9U?SWEEP_POINT_PASS:mode==2U||mode==10U?SWEEP_POINT_OUT_OF_TOLERANCE:
            mode==3U||mode==4U||mode==7U?SWEEP_POINT_CAPTURE_ERROR:SWEEP_POINT_NO_SIGNAL));
    }
    /* Cancellation at every ordinary active state, including restoration. */
    for (unsigned s=SWEEP_STEP_ENABLE;s<=SWEEP_STEP_RELEASE;++s) {
        Fake_Reset(false,777U,10000U);
        CHECK(Sweep_Init()==BIZ_OK && Sweep_Start()==BIZ_OK);
        CHECK(reach((SweepStep)s));
        CHECK(Sweep_Cancel()==BIZ_OK);
        CHECK(run());
        CHECK(snapshot.state==SWEEP_CANCELLED && snapshot.restored && !snapshot.owns_a);
        CHECK(!fake_a.enabled && fake_a.applied.frequency_hz==777U && fake_a.applied.duty_bp==10000U);
    }
    Fake_Reset(true,777U,5000U);
    fake_now=UINT32_MAX-200U;
    fake_next_id=UINT32_MAX-10U;
    CHECK(Sweep_Init()==BIZ_OK && Sweep_Start()==BIZ_OK);
    CHECK(run() && snapshot.state==SWEEP_PASS && snapshot.restored);
    /* Stalled pending apply cannot be called restored; emergency off cancels it,
     * but ownership is intentionally retained for coordinator recovery. */
    Fake_Reset(true,777U,5000U);
    CHECK(Sweep_Init()==BIZ_OK && Sweep_Start()==BIZ_OK);
    CHECK(reach(SWEEP_STEP_APPLY_WAIT));
    fake_stall=true;
    CHECK(Sweep_Cancel()==BIZ_OK && run());
    CHECK(snapshot.state==SWEEP_FAIL && !snapshot.restored && snapshot.owns_a && !fake_a.enabled);
    CHECK(Sweep_Start()==BIZ_BUSY && Sweep_Init()==BIZ_BUSY);
    /* Simulate explicit coordinator repair between independent scenarios. */
    Fake_Reset(true,777U,5000U);
    /* A retained fault cannot be reset by Sweep_Init, even if another layer has
     * silently reset Wave; use fresh emulator process for held-fault scenarios. */
    return 0;
}
