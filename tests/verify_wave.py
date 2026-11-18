"""Execute real ARM API code. MMIO events are controlled, not a board model.

python -m pip install --target build/wave_checks/python unicorn==2.1.4 pyelftools==0.32
python tests/verify_wave.py [--stage W1|W2|W3|W4|W5]
No simulated result is ever linked into the firmware or the real sweep.
"""
from pathlib import Path
import argparse
import ctypes as C
import hashlib
import json
import random
import struct
import sys

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/wave_checks'
sys.path.insert(0, str(OUT / 'python'))
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS, UC_HOOK_CODE, UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE
from unicorn.arm_const import UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3, UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_PC

class Params(C.Structure):
    _fields_ = [('frequency_hz', C.c_uint32), ('duty_bp', C.c_uint16)]
class Snapshot(C.Structure):
    # ARM AAPCS uses the compiler's small-enum ABI here (checked against DWARF).
    _fields_ = [('state', C.c_uint8), ('owner', C.c_uint8), ('requested', Params), ('applied', Params),
                ('frequency_millihz', C.c_uint32), ('duty_bp', C.c_uint16), ('enabled', C.c_bool), ('pending', C.c_bool),
                ('request_id', C.c_uint32), ('applied_id', C.c_uint32), ('cancelled_id', C.c_uint32),
                ('accepted_ms', C.c_uint32), ('applied_ms', C.c_uint32), ('last_result', C.c_uint8)]
class Timing(C.Structure):
    _fields_ = [('divider', C.c_uint32), ('counts', C.c_uint32), ('compare', C.c_uint32),
                ('frequency_millihz', C.c_uint32), ('duty_bp', C.c_uint16)]

TIM1, TIM8, TIM2 = 0x40010000, 0x40010400, 0x40000000
OK, PENDING, BUSY, INVALID, RANGE, STEP, UNSUPPORTED, NOT_INIT = range(8)

class Firmware:
    def __init__(self):
        self.uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
        for base, size in [(0x08000000,0x100000),(0x20000000,0x20000),(0x40000000,0x30000),
                           (0xE0000000,0x100000),(0x30000000,0x10000)]:
            self.uc.mem_map(base,size)
        with (ROOT / 'build/Debug/lab2_f407.elf').open('rb') as f:
            elf = ELFFile(f)
            for s in elf.iter_segments():
                if s['p_type'] == 'PT_LOAD' and s['p_filesz']: self.uc.mem_write(s['p_vaddr'], s.data())
            self.syms = {s.name:s['st_value'] for s in elf.get_section_by_name('.symtab').iter_symbols()}
        self.now = 1000
        self.flags = {t:0 for t in (TIM1,TIM8,TIM2)}
        self.active = {t:{} for t in self.flags}
        self.hook('HAL_GetTick', lambda: self.now)
        self.hook('HAL_RCC_GetPCLK2Freq', lambda: 84000000)
        self.hook('HAL_RCC_GetPCLK1Freq', lambda: 42000000)
        self.hook('HAL_GPIO_Init', lambda: 0)
        self.hook('HAL_NVIC_SetPriority', lambda: 0)
        self.hook('HAL_NVIC_EnableIRQ', lambda: 0)
        self.write(0x40023808, 0x9400)  # Actual P1 APB divider fields.
        self.uc.hook_add(UC_HOOK_MEM_WRITE, self.store, begin=0x40000000,end=0x40011000)
        self.uc.hook_add(UC_HOOK_MEM_READ, self.load, begin=0x40000000,end=0x40011000)
    def read(self,a): return int.from_bytes(self.uc.mem_read(a,4),'little')
    def write(self,a,v): self.uc.mem_write(a,struct.pack('<I',v & 0xffffffff))
    def hook(self,name,callback):
        if name not in self.syms: return
        def intercept(uc,a,s,d):
            uc.reg_write(UC_ARM_REG_R0,callback())
            uc.reg_write(UC_ARM_REG_PC,uc.reg_read(UC_ARM_REG_LR))
        a=self.syms[name]&~1
        self.uc.hook_add(UC_HOOK_CODE,intercept,begin=a,end=a)
    def store(self,uc,access,a,size,value,data):
        for t in self.flags:
            if a == t+0x10: self.flags[t] &= value  # TIM SR is rc_w0.
            if a == t+0x14 and value&1: self.event(t, software=True)
    def load(self,uc,access,a,size,value,data):
        for t in self.flags:
            if a == t+0x10: self.write(a,self.flags[t])
    def event(self,t,software=False):
        if self.read(t)&2: return False  # UDIS suppresses shadow transfer.
        self.active[t]={o:self.read(t+o) for o in (0x28,0x2c,0x34,0x38,0x40)}
        if not software or not self.read(t)&4: self.flags[t]|=1
        return True
    def update(self,ch=0,irq=True):
        t=(TIM1,TIM8)[ch]
        self.event(t)
        self.now=(self.now+1)&0xffffffff
        if irq: self.call(('Wave_TIM1_UpdateIRQ','Wave_TIM8_UpdateIRQ')[ch])
    def call(self,name,*args):
        self.uc.reg_write(UC_ARM_REG_SP,0x2001f000)
        self.uc.reg_write(UC_ARM_REG_LR,0x30000001)
        regs=(UC_ARM_REG_R0,UC_ARM_REG_R1,UC_ARM_REG_R2,UC_ARM_REG_R3)
        for i,v in enumerate(args):
            if i<4: self.uc.reg_write(regs[i],v)
            else: self.write(0x2001f000+4*(i-4),v)
        self.uc.emu_start(self.syms[name]|1,0x30000000,count=100000)
        assert self.uc.reg_read(UC_ARM_REG_PC)==0x30000000, name+' did not return'
        return self.uc.reg_read(UC_ARM_REG_R0)
    def snapshot(self,ch=0):
        self.call('Wave_GetSnapshot',ch,0x30001000)
        return Snapshot.from_buffer_copy(self.uc.mem_read(0x30001000,C.sizeof(Snapshot)))
    def apply(self,ch,hz,duty,owner=0):
        self.uc.mem_write(0x30001100,bytes(Params(hz,duty)))
        return self.call('Wave_Apply',ch,owner,0x30001100,0x30001200)

def base_checks(fw):
    assert fw.snapshot().state==0
    assert fw.apply(0,1000,5000)==NOT_INIT
    assert fw.call('Wave_Init')==OK
    s=fw.snapshot()
    assert (s.state,s.enabled,s.applied.frequency_hz,s.applied.duty_bp)==(1,False,1000,5000)
    assert fw.read(TIM1)&1==0 and fw.active[TIM1][0x34]==0
    for hz,duty,error in [(0,5000,RANGE),(200001,5000,RANGE),(1,10100,RANGE),(1000,51,STEP)]:
        before=bytes(fw.snapshot())
        assert fw.apply(0,hz,duty)==error
        assert bytes(fw.snapshot())==before
    assert fw.apply(0,1,9900)==OK and not fw.snapshot().enabled
    assert fw.call('Wave_SetEnabled',0,0,True,0)==OK
    old=fw.active[TIM1].copy()
    for hz,duty in [(200000,100),(1,10000),(199999,0),(2563,9900),(2564,4900),(1000,5000)]:
        assert fw.apply(0,hz,duty)==PENDING
        pending=fw.snapshot()
        assert pending.pending and pending.request_id!=pending.applied_id
        assert fw.active[TIM1]==old  # An accepted preload is NOT applied.
        assert fw.apply(0,100,5000)==BUSY
        fw.update()
        s=fw.snapshot()
        assert not s.pending and s.applied_id==pending.request_id
        assert (s.applied.frequency_hz,s.applied.duty_bp)==(hz,duty)
        assert fw.read(TIM1+0x0c)&1==0  # No persistent per-period IRQ.
        n=fw.active[TIM1][0x2c]+1
        assert fw.active[TIM1][0x34]==round(n*duty/10000) or abs(fw.active[TIM1][0x34]-n*duty/10000)<=0.5
        old=fw.active[TIM1].copy()
    # Duplicate START must not reset the counter or restart the waveform.
    fw.write(TIM1+0x24,123)
    before=fw.snapshot().request_id
    assert fw.call('Wave_SetEnabled',0,0,True,0)==OK
    assert fw.read(TIM1+0x24)==123 and fw.snapshot().request_id==before
    # Cancel a request that has not latched; preserve old parameters.
    assert fw.apply(0,10,100)==PENDING
    cancelled=fw.snapshot().request_id
    assert fw.call('Wave_SetEnabled',0,0,False,0)==OK
    s=fw.snapshot()
    assert s.cancelled_id==cancelled and s.applied.frequency_hz==1000 and not s.enabled
    assert fw.active[TIM1][0x34]==0 and fw.read(TIM1)&1==0
    # A real UEV preceding a delayed IRQ must not be misreported as cancelled.
    fw.call('Wave_SetEnabled',0,0,True,0)
    assert fw.apply(0,100,200)==PENDING
    committed=fw.snapshot().request_id
    fw.update(irq=False)
    assert fw.call('Wave_SetEnabled',0,0,False,0)==OK
    assert fw.snapshot().applied.frequency_hz==100 and fw.snapshot().cancelled_id!=committed
    # Explicit A ownership and pending-aware release.
    assert fw.call('Wave_AcquireA',1)==OK
    assert fw.apply(0,200,5000)==BUSY
    assert fw.call('Wave_SetMode',0)==BUSY
    assert fw.call('Wave_SetEnabled',0,0,False,0)==BUSY
    assert fw.apply(0,200,5000,owner=1)==OK
    assert fw.call('Wave_ReleaseA',1)==OK
    # Initialization is idempotent; no page-driven reset.
    before=bytes(fw.snapshot())
    assert fw.call('Wave_Init')==OK and bytes(fw.snapshot())==before

def math_checks(fw):
    rng=random.Random(407)
    frequencies={1,2,3,10,100,1000,10000,50000,100000,199999,200000,250000,400000,499258,500000}
    for divider in range(1,2565):
        boundary=168000000//(65535*divider)
        frequencies.update(f for f in (boundary-1,boundary,boundary+1) if 1<=f<=500000)
    frequencies.update(rng.randint(1,500000) for _ in range(1500))
    worst_f=worst_d=0
    for f in sorted(frequencies):
        for duty in (0,100,5000,9900,10000):
            assert fw.call('Wave_Calculate',168000000,f,duty,0x30001300)==1
            t=Timing.from_buffer_copy(fw.uc.mem_read(0x30001300,C.sizeof(Timing)))
            actual=168000000/(t.divider*t.counts)
            worst_f=max(worst_f,abs(actual/f-1)*100)
            worst_d=max(worst_d,abs(100*t.compare/t.counts-duty/100))
            assert 1<=t.divider<=65536 and 2<=t.counts<=65535 and 0<=t.compare<=t.counts
            assert abs(actual/f-1)<.01 and abs(100*t.compare/t.counts-duty/100)<1
            assert duty!=0 or t.compare==0
            assert duty!=10000 or t.compare==t.counts
    return {'frequency_cases':len(frequencies)*5,'max_frequency_error_percent':worst_f,
            'max_duty_error_points_sampled':worst_d,
            'analytic_bound':'N>=336 at 500kHz: rounding <=0.1491% frequency, <=0.14881 duty points. N<=65535 keeps CCR=N representable.'}

if __name__=='__main__':
    parser=argparse.ArgumentParser()
    parser.add_argument('--stage',default='W1')
    args=parser.parse_args()
    fw=Firmware()
    base_checks(fw)
    result={'stage':args.stage,'api':'PASS','math':math_checks(fw),'hardware':'NOT TESTED',
            'elf_sha256':hashlib.sha256((ROOT/'build/Debug/lab2_f407.elf').read_bytes()).hexdigest()}
    OUT.mkdir(parents=True,exist_ok=True)
    (OUT/(args.stage+'.json')).write_text(json.dumps(result,indent=2),encoding='utf-8')
    print(json.dumps(result,indent=2))
