"""Actual committed Wave + Input + Sweep ARM code with a controlled timer/DMA model.
This is cross-module software integration evidence; it is NOT electrical loopback.
The model derives captures from active TIM1 PSC/ARR/CCR, never requested Hz.
"""
import json
import struct
from arm_firmware import Firmware, ROOT, UC_HOOK_MEM_WRITE
from unicorn import UC_HOOK_MEM_READ
TIM1,TIM8,TIM5,DMA=0x40010000,0x40010400,0x40000C00,0x40026000
R,F=DMA+0x70,DMA+0x40
TMP=0x2001E000

class System(Firmware):
    def __init__(self,connected=True):
        super().__init__(ROOT/'build/Debug/lab2_f407.elf')
        self.hook('HAL_RCC_GetPCLK1Freq',lambda:42000000)
        self.hook('HAL_RCC_GetPCLK2Freq',lambda:84000000)
        self.hook('HAL_GPIO_Init',lambda:None)
        self.write(0x40023808,0x9400)
        self.flags={t:0 for t in (TIM1,TIM8)}
        self.active={t:{} for t in self.flags}
        self.due={t:0 for t in self.flags}
        self.rc=self.fc=0
        self.window_time=0
        self.connected=connected
        self.uc.hook_add(UC_HOOK_MEM_WRITE,self.store,begin=0x40000000,end=0x40026400)
        self.uc.hook_add(UC_HOOK_MEM_READ,self.load,begin=0x40010000,end=0x400104FF)
        assert self.call('Wave_Init')==0
        assert self.call('Spwm_Init')==0
        assert self.call('Input_Init')==0
        assert self.call('Sweep_Init')==0
    def latch(self,t):
        self.active[t]={offset:self.read(t+offset) for offset in (0x28,0x2C,0x34,0x38)}
        self.due[t]=self.now+self.period_ms(t)
    def period_ms(self,t):
        a=self.active[t]
        return max(1,((a.get(0x28,0)+1)*(a.get(0x2C,0)+1)+167999)//168000)
    def store(self,uc,access,address,size,value,data):
        for t in self.flags:
            if address==t+0x10: self.flags[t]&=value
            if address==t+0x14 and value&1: self.latch(t)
        if address==DMA+12: self.write(DMA+4,self.read(DMA+4)&~value)
        if address==DMA+8: self.write(DMA,self.read(DMA)&~value)
        if address==R+4 and value==512: self.rc=0; self.window_time=self.now
        if address==F+4 and value==512: self.fc=0
    def load(self,uc,access,address,size,value,data):
        for t in self.flags:
            if address==t+0x10: self.write(address,self.flags[t])
    def edge(self,rise,ticks):
        stream,count=(R,self.rc) if rise else (F,self.fc)
        self.write(self.read(stream+12)+4*(count%512),ticks)
        count+=1
        self.write(stream+4,512-count%512)
        if rise: self.rc=count
        else: self.fc=count
    def capture(self):
        a=self.active[TIM1]
        count=a.get(0x2C,0)+1
        compare=a.get(0x34,0)
        if not self.connected or not self.read(TIM1)&1 or not 0<compare<count: return
        if not (self.read(TIM5+0x20)&0x11)==0x11: return
        if self.now-self.window_time < 2*self.period_ms(TIM1): return
        # A complete suffix selected from the modeled pin waveform. DMA load /
        # producer concurrency is separately tested by verify_input_dma.py.
        period=((a.get(0x28,0)+1)*count+1)//2
        high=((a.get(0x28,0)+1)*compare+1)//2
        end=(self.now*84000)&0xFFFFFFFF
        self.edge(True,(end-period)&0xFFFFFFFF)
        self.edge(False,(end-period+high)&0xFFFFFFFF)
        self.edge(True,end)
        self.write(TIM5+0x24,end)
    def tick(self):
        self.now+=5
        for t in self.flags:
            if self.read(t)&1 and self.now>=self.due[t]:
                self.latch(t)
                self.flags[t]|=1
        self.capture()
        self.call('Wave_Process',self.now)
        self.call('Input_Process',self.now)
        self.call('Sweep_Process',self.now)
    def wave(self,ch=0):
        self.call('Wave_GetSnapshot',ch,TMP)
        return self.structure('WaveSnapshot',TMP)
    def sweep(self):
        self.call('Sweep_GetSnapshot',TMP)
        return self.structure('SweepSnapshot',TMP)
    def sweep_diag(self):
        self.call('Sweep_GetDiagnostics',TMP)
        return self.structure('SweepDiagnostics',TMP)
    def apply(self,ch,hz,duty):
        self.uc.mem_write(TMP,struct.pack('<IH2x',hz,duty))
        return self.call('Wave_Apply',ch,0,TMP,TMP+16)
    def run(self):
        for _ in range(10000):
            self.tick()
            s=self.sweep()
            if s['state'] not in (1,2): return s
        raise AssertionError('sweep did not terminate')

def main():
    sys=System()
    assert sys.apply(0,777,10000)==0
    assert sys.apply(1,1234,3300)==0
    assert sys.call('Wave_SetEnabled',1,0,1,TMP)==0
    assert sys.call('Spwm_Start')==0
    before_b=sys.wave(1)
    spwm_stream=bytes(sys.uc.mem_read(DMA+0x28,24))
    saved=sys.wave()
    assert sys.call('Sweep_Start')==0
    assert sys.call('Sweep_Start')==2
    assert sys.apply(0,1000,5000)==2 # manual owner cannot steal A
    s=sys.run()
    assert s['state']==3 and s['restored'] and not s['owns_a'] and s['completed']==10,s
    assert sys.wave()['applied']==saved['applied'] and not sys.wave()['enabled']
    assert sys.wave(1)==before_b
    assert bytes(sys.uc.mem_read(DMA+0x28,24))==spwm_stream
    sys.call('Sweep_GetSnapshot',TMP)
    points=[]
    for i in range(10):
        address=TMP+sys.layouts['SweepSnapshot']['points'][0]+i*sys.sizes['SweepPoint']
        p=sys.structure('SweepPoint',address)
        assert p['state']==1,p
        points.append({k:v for k,v in p.items() if k!='error_percent'})

    disconnected=System(False)
    assert disconnected.call('Sweep_Start')==0
    s=disconnected.run()
    assert s['state']==4 and s['restored'] and not s['owns_a'] and s['completed']==10,s
    cancel=System()
    assert cancel.apply(0,1,0)==0
    assert cancel.call('Wave_SetEnabled',0,0,1,TMP)==0 # original running DC low
    saved=cancel.wave()
    assert cancel.call('Sweep_Start')==0
    for _ in range(1000):
        cancel.tick()
        if cancel.wave()['pending']: break
    else: raise AssertionError('apply wait not reached')
    assert cancel.call('Sweep_Cancel')==0
    s=cancel.run()
    assert s['state']==5 and s['restored'] and not s['owns_a'],s
    assert cancel.wave()['applied']==saved['applied'] and cancel.wave()['enabled']

    result={'real_wave_sha':'e7a39eadb80bc81176baf88a160c5937945acda5',
            'actual_wave_input_sweep_arm_code':'PASS','normal':points,
            'disconnected':'FAIL with verified restore','cancel':'CANCELLED with verified restore',
            'B_and_SPWM_unchanged':True,'electrical_loopback':'not performed'}
    (ROOT/'build/real_sweep_integration_results.json').write_text(json.dumps(result,indent=2))
    print(json.dumps(result))


if __name__ == "__main__":
    main()
