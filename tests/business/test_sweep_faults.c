#include "business_api.h"
#include "sweep_internal.h"
#include "fake_sweep_business.h"
volatile uint32_t test_checks, test_failure;
#define CHECK(x) do { ++test_checks; if (!(x)) { test_failure=__LINE__; return 1; } } while (0)

int main(void)
{
    SweepSnapshot snapshot;
    SweepDiagnostics diag;
    Fake_Reset(false,777U,10000U);
    CHECK(Sweep_Init()==BIZ_OK && Sweep_Start()==BIZ_OK);
    bool injected=false, checked_pending=false, emergency_cancelled=false;
    for (unsigned i=0;i<20000U;++i) {
        Sweep_GetDiagnostics(&diag);
        if (!injected && diag.step==SWEEP_STEP_SAMPLE) {
#if SCENARIO == 1
            fake_reject_frequency=777U;
#elif SCENARIO == 2
            fake_stall_restore=true;
#elif SCENARIO == 3
            fake_reject_enable=true;
#elif SCENARIO == 4
            fake_reject_release=true;
#elif SCENARIO == 5
            fake_bad_id=true;
#elif SCENARIO == 6
            fake_a.state=WAVE_FAULT;
#endif
            injected=true;
            CHECK(Sweep_Cancel()==BIZ_OK);
        }
        if (diag.step==SWEEP_STEP_RESTORE_PARAMS_WAIT || diag.step==SWEEP_STEP_RESTORE_ENABLE_WAIT) {
            Sweep_GetSnapshot(&snapshot);
            CHECK(snapshot.state==SWEEP_RESTORING && !snapshot.restored && snapshot.owns_a);
            checked_pending=true;
        }
        if (diag.step==SWEEP_STEP_EMERGENCY_OFF && !emergency_cancelled) {
            CHECK(Sweep_Cancel()==BIZ_OK); /* failure must still be FAIL, never CANCELLED */
            emergency_cancelled=true;
        }
        Fake_Tick(5U);
        Sweep_Process(fake_now);
        Sweep_GetSnapshot(&snapshot);
        if (snapshot.state!=SWEEP_RUNNING && snapshot.state!=SWEEP_RESTORING) break;
    }
    CHECK(injected);
    CHECK(snapshot.state==SWEEP_FAIL && !snapshot.restored && snapshot.owns_a);
    CHECK(fake_a.owner==WAVE_OWNER_SELFTEST && !fake_b_calls);
    CHECK(Sweep_Start()==BIZ_BUSY && Sweep_Init()==BIZ_BUSY);
    Sweep_GetDiagnostics(&diag);
    CHECK(diag.restore_result!=BIZ_OK);
    (void)checked_pending;
#if SCENARIO == 2 || SCENARIO == 5
    CHECK(checked_pending && diag.restore_result==BIZ_TIMEOUT);
#endif
#if SCENARIO == 3
    CHECK(fake_a.enabled); /* do not claim a failed shutdown produced low */
#else
    CHECK(!fake_a.enabled);
#endif
    return 0;
}
