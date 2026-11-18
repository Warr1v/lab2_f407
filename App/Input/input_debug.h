#ifndef LAB2_INPUT_DEBUG_H
#define LAB2_INPUT_DEBUG_H
#include "business_api.h"
#include "input_port.h"
#include "sweep_internal.h"
typedef enum {
    INPUT_DEBUG_SNAPSHOT, INPUT_DEBUG_BEGIN_WINDOW, INPUT_DEBUG_STANDARD,
    INPUT_DEBUG_EXTENDED, INPUT_DEBUG_SWEEP_START, INPUT_DEBUG_SWEEP_CANCEL
} InputDebugCommand;
typedef struct {
    uint32_t magic, api_version;
    /* Debugger writes command first, request_id LAST. New ID triggers once. */
    uint32_t command, request_id, acknowledged_id;
    BizResult command_result;
    bool sweep_linked;
    InputSnapshot input;
    InputPortDiagnostics capture;
    SweepSnapshot sweep;
    SweepDiagnostics sweep_diagnostics;
} InputDebugMailbox;
extern volatile InputDebugMailbox g_input_debug;
void InputDebug_Init(void);
void InputDebug_Process(void);
#endif
