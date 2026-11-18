"""Production input.c + production DMA port in ARM ELF, with explicit DMA model.
No claims about electrical filtering, arbitration latency or physical accuracy.
"""
import json
import struct
from arm_firmware import Firmware, ROOT, UC_HOOK_MEM_WRITE

DMA=0x40026000
R=DMA+0x70  # Stream4
F=DMA+0x40  # Stream2
TIM=0x40000C00
SNAP=0x2001F000
fw=Firmware()
fw.hook('HAL_RCC_GetPCLK1Freq',lambda:42000000)
fw.hook('HAL_GPIO_Init',lambda:None)
fw.write(0x40023808,5<<10) # APB1 /4
def w1c(uc,access,address,size,value,data):
    if address==DMA+12: fw.write(DMA+4,fw.read(DMA+4)&~value)
    if address==DMA+8: fw.write(DMA,fw.read(DMA)&~value)
fw.uc.hook_add(UC_HOOK_MEM_WRITE,w1c,begin=DMA+8,end=DMA+15)
assert fw.call('Input_Init')==0
assert fw.read(R)&(7<<25)==6<<25
assert fw.read(F)&(7<<25)==6<<25
assert fw.read(R+8)==TIM+0x38 and fw.read(F+8)==TIM+0x34
assert fw.read(TIM+0x18)&0x303==0x102
assert fw.read(TIM+0x20)&0x33==0x13
rbase,fbase=fw.read(R+12),fw.read(F+12)

def snap():
    fw.call('Input_GetSnapshot',SNAP)
    raw=fw.structure('InputSnapshot',SNAP)
    names={'frequency_millihz':'hz_m','duty_bp':'duty','period_ticks':'period',
           'high_ticks':'high','sequence':'seq','acquisition_id':'id',
           'overcapture_count':'overcap','buffer_overrun_count':'overrun',
           'pairing_error_count':'pairing'}
    return {names.get(k,k):v for k,v in raw.items()}

rc=fc=0
def restart():
    global rc,fc
    assert fw.call('Input_BeginWindow',SNAP+80)==0
    rc=fc=0
    return fw.read(SNAP+80)

def edge(rising,ticks):
    global rc,fc
    stream,base,count=(R,rbase,rc) if rising else (F,fbase,fc)
    fw.write(base+(count%512)*4,ticks)
    count+=1
    fw.write(stream+4,512-count%512)
    if rising:
        rc=count
        if count%512==256: fw.write(DMA+4,fw.read(DMA+4)|(1<<4))
        if count%512==0: fw.write(DMA+4,fw.read(DMA+4)|(1<<5))
    else:
        fc=count
        if count%512==256: fw.write(DMA,fw.read(DMA)|(1<<20))
        if count%512==0: fw.write(DMA,fw.read(DMA)|(1<<21))
    fw.write(TIM+0x24,ticks)

def process(): fw.call('Input_Process',fw.now)

# Low frequency suffix before either DMA block interrupt, including TIM5 wrap.
identity=restart()
edge(True,0xFE000000)
edge(False,(0xFE000000+42000000)&0xFFFFFFFF)
fw.now=1000
edge(True,(0xFE000000+84000000)&0xFFFFFFFF)
process()
s=snap()
assert s['state']==1 and s['hz_m']==1000 and s['duty']==5000 and s['id']==identity,s
# Fence flushes a pending, already filled old producer suffix.
identity=restart()
process()
assert snap()['state']==0
edge(False,200)
edge(True,300)
process()
assert snap()['state']==0
edge(False,350)
edge(True,400)
process()
assert snap()['state']==3  # real 840 kHz is outside current capability

# Many DMA wraps while main/UI is stalled: block IRQ keeps latest coherent data.
restart()
t=10000
period=420
for i in range(5000):
    edge(True,t)
    if fw.read(DMA+4)&0x30:
        fw.call('Input_DMA1_Stream4_IRQHandler')
    edge(False,t+42)
    t+=period
fw.now+=10
process()
s=snap()
assert s['state']==1 and s['hz_m']==200000000 and s['duty']==1000,s
# Both latched boundary flags expose ambiguous overwrite immediately.
fw.write(DMA+4,0x30)
assert snap()['state']==4
process()
assert snap()['overrun']==1 and snap()['state']==4
# Fresh low-frequency half-period after error cannot reuse pre-fault data.
process()
assert snap()['state']==0
edge(True,t)
process()
assert snap()['state']==0
# Overcapture and transfer error are surfaced before Process too.
fw.write(TIM+0x10,1<<10)
assert snap()['state']==4
process()
assert snap()['overcap']==1
fw.write(DMA,1<<19)
assert snap()['state']==4
process()
assert snap()['overrun']==2

assert fw.call('Input_SetMode',1)==0
assert snap()['state']==0
rc=fc=0
t=10000000
for i in range(8192):
    edge(True,t)
    if fw.read(DMA+4)&0x30: fw.call('Input_DMA1_Stream4_IRQHandler')
    edge(False,t+17) # ~10.119%, not an output setting step
    t+=168
fw.now+=17
process()
s=snap()
assert s['state']==1 and s['hz_m']==500000000 and s['duty']==1012,s
assert fw.call('Input_SetMode',0)==0
assert snap()['state']==0

output={'production_input_and_dma_port':'PASS','mode':'modeled DMA registers',
        'cycles_streamed':13192,'dma_wraps':25,'electrical_hardware':'not tested'}
(ROOT/'build/input_dma_results.json').write_text(json.dumps(output,indent=2))
print(json.dumps(output))
