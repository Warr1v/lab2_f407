"""Check actual default-off diagnostic build commands and idempotent mailbox."""
import json
from arm_firmware import Firmware, ROOT
fw=Firmware(ROOT/'build/Diagnostic/lab2_f407.elf')
fw.hook('HAL_RCC_GetPCLK1Freq',lambda:42000000)
fw.hook('HAL_GPIO_Init',lambda:None)
fw.write(0x40023808,5<<10)
assert fw.call('Input_Init')==0
fw.call('InputDebug_Init')
base=fw.syms['g_input_debug']
layout=fw.layouts['InputDebugMailbox']
def value(name):
    offset,size=layout[name]
    return int.from_bytes(fw.uc.mem_read(base+offset,size),'little')
def write(name,data):
    offset,size=layout[name]
    fw.uc.mem_write(base+offset,data.to_bytes(size,'little'))
assert value('magic')==0x494E5031 and value('api_version')==1 and not value('sweep_linked')
write('command',3)
write('request_id',1)
fw.call('InputDebug_Process')
assert value('acknowledged_id')==1 and value('command_result')==0
assert fw.call('Input_GetMode')==1
input_base=base+layout['input'][0]
before=fw.structure('InputSnapshot',input_base)
fw.call('InputDebug_Process')
assert fw.structure('InputSnapshot',input_base)['acquisition_id']==before['acquisition_id']
write('command',4)
write('request_id',2)
fw.call('InputDebug_Process')
assert value('command_result')==6 # BIZ_UNSUPPORTED; no fake PASS without Wave
write('command',99)
write('request_id',3)
fw.call('InputDebug_Process')
assert value('command_result')==3
write('command',1)
write('request_id',4)
fw.call('InputDebug_Process')
assert value('command_result')==0
assert fw.structure('InputSnapshot',input_base)['acquisition_id']!=before['acquisition_id']
print(json.dumps({'debug_mailbox':'PASS','automatic_output':False,'fake_sweep_linked':False}))
