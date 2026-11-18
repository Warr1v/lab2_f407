"""Actual integrated ARM firmware: touch -> UI -> real backends -> display.

Only LCD/touch boundaries and timer/DMA events are modeled. No board is touched.
Dependencies: unicorn==2.1.4, pyelftools==0.32 (build/test_python is supported).
"""
from pathlib import Path
import json
import sys

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'build/test_python'))
sys.path.insert(0,str(ROOT/'tests/business'))
from verify_real_sweep import System, TMP, TIM1, TIM8
from verify_snake import Firmware as ScreenFirmware


class ScreenSystem(System):
    def __init__(self,connected=True):
        super().__init__(connected)
        self.uc.mem_map(0x42000000,0x2000000)
        self.ui=ScreenFirmware()
        # Same exact Debug ELF, shared CPU with the real hardware-port model.
        self.ui.uc=self.uc
        u=self.ui
        u.hook('ScreenDelay_Init',lambda:None)
        u.hook('ScreenTouch_Init',lambda:None)
        from unicorn.arm_const import UC_ARM_REG_R0
        u.hook('ScreenCal_SelfTest',lambda:u.uc.reg_write(UC_ARM_REG_R0,0x7f))
        u.hook('TFTLCD_Init',lambda:u.write('tftlcd_data',bytes.fromhex('f000400141930000')))
        import struct
        u.hook('ScreenTouch_Read',lambda:u.uc.mem_write(u.arg(0),struct.pack('<??HH',*u.sample)))
        u.hook('ScreenCal_Map',u.map_touch)
        u.hook('LCD_Clear',lambda:u.rect(0,0,239,319,u.arg(0)))
        u.hook('LCD_Fill',lambda:u.rect(*(u.arg(i) for i in range(5))))
        u.hook('LCD_DrawRectangle',u.outline)
        self.drawn=[]
        def chinese():
            self.drawn.append(u.string(u.arg(2)));u.chinese()
        def ascii_text():
            self.drawn.append(u.string(u.arg(5)));u.ascii()
        u.hook('LCD_ShowFontHZ',chinese)
        u.hook('LCD_ShowString',ascii_text)
        self.call('ScreenApp_Init')
        self.scan(8)

    def scan(self,ms=8,pressed=False,x=0,y=0):
        self.now=(self.now+ms)&0xffffffff
        # The same order as production main; no backend is skipped on a page.
        for t in self.flags:
            if self.read(t)&1 and self.now>=self.due[t]:
                self.latch(t);self.flags[t]|=1
                # Explicit TRGO reset event for the UI binding checks. Detailed
                # phase timing/preload behavior is covered by verify_wave.py.
                if t==TIM1 and self.read(TIM8+8)&7==4:self.flags[TIM8]|=1<<6
        self.capture()
        self.call('Wave_Process',self.now)
        self.call('Input_Process',self.now)
        self.call('Sweep_Process',self.now)
        self.ui.sample=(pressed,pressed,x,y)
        self.ui.now=self.now
        self.call('ScreenApp_Process',self.now)

    def click(self,x,y):
        self.scan(8,True,x,y);self.scan(8);self.scan(16);self.scan(8)
        self.scan(100)

    def value(self,kind,value):
        # Use the actual keyboard via touch, including replace-on-first-key.
        self.click(50 if kind=='frequency' else 180,100 if kind=='phase' else 232)
        assert self.ui.number('page')==9
        for char in str(value):
            digit=int(char)
            if digit: self.click(44+((digit-1)%3)*76,142+((digit-1)//3)*42)
            else:self.click(120,268)
        self.click(196,268)

    def until_done(self):
        for _ in range(2000):
            self.scan(20)
            if self.sweep()['state'] not in (1,2):break
        else:raise AssertionError('UI sweep timed out')
        self.scan(100)
        return self.sweep()


def main():
    f=ScreenSystem();u=f.ui
    assert not f.wave()['enabled'] and not f.wave(1)['enabled']
    f.click(80,84)
    assert u.number('page')==3
    f.value('frequency',1234)
    assert f.wave()['applied'] & 0xffffffff==1234
    f.value('duty',37)
    f.click(40,284)
    assert f.wave()['enabled']
    f.scan(200)
    assert '1233.' in '|'.join(f.drawn) or any('1234.' in s for s in f.drawn)
    assert any('37.00 %' in s or '37.0' in s for s in f.drawn)
    u.snapshot('integrated_pwm.png')
    # B can be configured independently and does not silently overwrite A.
    f.click(198,24);f.value('frequency',2500);f.value('duty',37);f.click(40,284)
    assert f.wave(1)['enabled'] and f.wave(1)['applied'] & 0xffffffff==2500
    assert f.wave()['applied'] & 0xffffffff==1234
    # A duty slider uses mode step; cancel/invalid input preserve applied state.
    f.click(198,24);f.click(180,232);f.click(120,108)
    assert u.string(u.syms['edit_value'][0])=='50'
    f.click(198,268);f.scan(100)
    f.value('frequency',0)
    assert u.number('page')==9 and f.wave()['applied'] & 0xffffffff==1234
    u.snapshot('integrated_keyboard_invalid.png')
    f.click(196,24)
    # Input display uses the real capture backend, then removes stale numbers.
    f.click(196,284);f.click(80,136);f.scan(200)
    assert u.number('page')==4
    assert '信号有效' in f.drawn
    u.snapshot('integrated_input.png')
    f.connected=False;f.drawn=[];f.scan(1600)
    assert '无信号' in f.drawn and '-- Hz' in f.drawn
    u.snapshot('integrated_no_signal.png')
    # Mode transaction rejects incompatible 37% B duty without partial change.
    f.click(196,290);f.click(50,250)
    assert f.call('Wave_GetMode')==f.call('Input_GetMode')==0
    # Prepare both duty grids; entering extended and returning to standard work.
    f.click(80,84);f.click(198,24);f.value('duty',50)
    f.click(196,284);f.click(50,250)
    assert f.call('Wave_GetMode')==f.call('Input_GetMode')==1
    f.click(80,84);f.value('frequency',500000)
    assert f.wave(1)['applied'] & 0xffffffff==500000
    f.click(196,284);f.click(50,250)
    assert f.call('Wave_GetMode')==f.call('Input_GetMode')==1
    f.click(80,84);f.value('frequency',1000)
    f.click(196,284);f.click(50,250)
    assert f.call('Wave_GetMode')==f.call('Input_GetMode')==0
    # Sweep starts from the screen, locks A, displays ten real model captures,
    # paginates, and restores prior settings while B continues running.
    f.connected=True;f.click(80,136);f.click(180,242)
    before=f.wave();before_b=f.wave(1)
    f.click(44,284)
    assert f.sweep()['owns_a']
    assert f.call('Input_BeginWindow',0)==3 # no mutation with an invalid pointer
    result=f.until_done()
    assert result['state']==3 and result['completed']==10 and result['restored']
    assert f.wave()['applied']==before['applied'] and f.wave()['enabled']==before['enabled']
    assert f.wave(1)==before_b
    assert 'PASS' in f.drawn
    f.click(120,284);u.snapshot('integrated_sweep_pass.png')
    # Missing input can never be displayed as PASS; cancel remains available.
    f.connected=False;f.click(44,284)
    result=f.until_done()
    assert result['state']==4 and result['restored'] and 'FAIL' in f.drawn
    u.snapshot('integrated_sweep_fail.png')
    f.click(44,284);f.click(44,284)
    result=f.until_done()
    assert result['state']==5 and result['restored']
    # Real screen controls for synchronized PWM, phase editing and SPWM.
    g=ScreenSystem(False);g.click(80,84);g.click(44,284)
    g.click(198,24);g.click(44,284);g.click(120,284)
    assert g.ui.number('page')==7
    g.click(44,180)
    g.call('Spwm_GetSnapshot',TMP)
    assert g.structure('SpwmSnapshot',TMP)['running']
    g.click(44,100)
    for _ in range(20):g.scan(10)
    g.call('Wave_GetSyncSnapshot',TMP)
    assert g.uc.mem_read(TMP,1)[0]==2
    g.value('phase',90)
    assert g.ui.number('page')==7
    for _ in range(20):g.scan(10)
    assert any('B:90 deg' in s for s in g.drawn)
    g.ui.snapshot('integrated_sync_spwm.png')
    g.click(44,100)
    for _ in range(20):g.scan(10)
    g.call('Wave_GetSyncSnapshot',TMP)
    assert g.uc.mem_read(TMP,1)[0]==0
    g.click(44,180)
    g.call('Spwm_GetSnapshot',TMP)
    assert not g.structure('SpwmSnapshot',TMP)['running']
    print('PASS: real touch controls, keyboard/slider, A/B isolation, input freshness, coherent mode, sweep PASS/FAIL/cancel/restore, sync/phase/SPWM')
    (ROOT/'build/integrated_ui_results.json').write_text(json.dumps({
        'real_touch_ui_and_business':'PASS','physical_hardware':'not tested'},indent=2))


if __name__=='__main__':main()
