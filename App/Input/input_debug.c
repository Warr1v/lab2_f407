#include "input_debug.h"
#include <string.h>
volatile InputDebugMailbox g_input_debug;
void InputDebug_Init(void)
{
    memset((void *)&g_input_debug, 0, sizeof(g_input_debug));
    g_input_debug.magic = 0x494E5031U; /* INP1 */
    g_input_debug.api_version = LAB2_BUSINESS_API_VERSION;
#if defined(LAB2_ENABLE_SWEEP)
    g_input_debug.sweep_linked = true;
#endif
}
void InputDebug_Process(void)
{
    uint32_t id = g_input_debug.request_id;
    if (id != g_input_debug.acknowledged_id) {
        BizResult status;
        uint32_t acquisition;
        switch ((InputDebugCommand)g_input_debug.command) {
        case INPUT_DEBUG_SNAPSHOT: status = BIZ_OK; break;
        case INPUT_DEBUG_BEGIN_WINDOW: status = Input_BeginWindow(&acquisition); break;
        case INPUT_DEBUG_STANDARD: status = Input_SetMode(BIZ_MODE_STANDARD); break;
        case INPUT_DEBUG_EXTENDED: status = Input_SetMode(BIZ_MODE_EXTENDED); break;
#if defined(LAB2_ENABLE_SWEEP)
        case INPUT_DEBUG_SWEEP_START: status = Sweep_Start(); break;
        case INPUT_DEBUG_SWEEP_CANCEL: status = Sweep_Cancel(); break;
#else
        case INPUT_DEBUG_SWEEP_START:
        case INPUT_DEBUG_SWEEP_CANCEL: status = BIZ_UNSUPPORTED; break;
#endif
        default: status = BIZ_INVALID_ARGUMENT; break;
        }
        g_input_debug.command_result = status;
        g_input_debug.acknowledged_id = id;
    }
    InputSnapshot input;
    InputPortDiagnostics capture;
    Input_GetSnapshot(&input);
    InputPort_GetDiagnostics(&capture);
    g_input_debug.input = input;
    g_input_debug.capture = capture;
#if defined(LAB2_ENABLE_SWEEP)
    SweepSnapshot sweep;
    SweepDiagnostics diagnostics;
    Sweep_GetSnapshot(&sweep);
    Sweep_GetDiagnostics(&diagnostics);
    g_input_debug.sweep = sweep;
    g_input_debug.sweep_diagnostics = diagnostics;
#endif
}
