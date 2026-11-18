"""Continuous real touch/UI/Wave regression; only physical boundaries modeled."""
import json
from verify_integrated_ui import ScreenSystem, ROOT


def diag(f):
    address=f.ui.syms['g_screen_slider_diag'][0]
    return [int.from_bytes(f.uc.mem_read(address+i*4,4),'little') for i in range(6)]


def applied_duty(f,ch=0):return (f.wave(ch)['applied']>>32)&0xffff


def release(f):
    f.scan(8);f.scan(16);f.scan(8)


def visible_track(f,y=154):
    # Regression for the old generic button repaint erasing the whole bar.
    colors=f.ui.pixels[(y-3)*240+18:(y-3)*240+222]
    assert any(c==0x051f for c in colors), 'track/thumb vanished'


def drag(f,positions,y=154):
    f.scan(8,True,positions[0],y)
    assert diag(f)[0]==1 and f.ui.number('slider_active')
    values=[diag(f)[1]]
    for x in positions[1:]:
        f.scan(8,True,x,y);values.append(diag(f)[1]);visible_track(f,y)
        assert f.ui.number('page')==3
    assert diag(f)[0]==2
    release(f)
    assert diag(f)[0]==3 and not f.ui.number('slider_active')
    visible_track(f,y)
    return values


def main():
    f=ScreenSystem(False);f.click(80,84)
    # Before release, both visuals and the real saved output configuration move.
    values=drag(f,list(range(18,222,7)))
    assert values==sorted(values) and len(set(values))>20
    assert not f.wave()['enabled']
    assert applied_duty(f)==values[-1]
    assert any(f'{values[-1]//100}.00 %'==s for s in f.drawn)
    f.ui.snapshot('slider_main_released.png')
    # Running output changes without waiting for lift; frequency/B remain intact.
    f.click(44,284)
    saved_b=f.wave(1)
    f.scan(8,True,60,154)
    f.scan(8,True,100,154)
    first=applied_duty(f)
    f.scan(8,True,150,154)
    assert applied_duty(f)!=first and f.wave()['enabled']
    assert f.wave()['applied']&0xffffffff==1000 and f.wave(1)==saved_b
    assert any(f'{diag(f)[1]//100}.00 %'==s for s in f.drawn)
    f.ui.snapshot('slider_main_dragging.png')
    # Pointer capture allows leaving the bar and clamps 0/100, no button-through.
    f.scan(8,True,0,294);assert diag(f)[1]==0
    f.scan(8,True,239,294);assert diag(f)[1]==10000
    release(f);f.scan(100)
    assert f.ui.number('page')==3 and applied_duty(f)==10000
    assert f.wave()['enabled'] # enabled DC high is not Stop
    # Existing ADC filtering plus step hysteresis rejects boundary chatter.
    f.scan(8,True,120,154);assert diag(f)[1]==5000
    for x in [121,120,121,120]*8:
        f.scan(8,True,x,154);assert diag(f)[1]==5000
    # A brief PEN bounce must not create an extra press/release or a value jump.
    counts=diag(f)
    f.scan(8);f.scan(8,True,120,154)
    assert diag(f)[2:5]==counts[2:5]
    release(f)
    # A lost/invalid coordinate stream preserves the last good duty; it is not 0.
    f.scan(8,True,120,154)
    before=diag(f)[1]
    f.scan(130,True,0,0,valid=False)
    assert diag(f)[0]==4 and diag(f)[1]==before
    release(f);assert applied_duty(f)==before
    # Selftest owns A: dragging disabled, no writes/no ghost button activation.
    assert f.call('Wave_AcquireA',1)==0
    before=f.wave()
    count=diag(f)[2]
    f.scan(8,True,50,154);f.scan(8,True,200,294);release(f)
    assert f.wave()==before and diag(f)[2]==count and f.ui.number('page')==3
    assert f.call('Wave_ReleaseA',1)==0

    # 1 Hz: coalesce many movements while a hardware request is in flight.
    slow=ScreenSystem(False);slow.click(80,84);slow.value('frequency',1)
    slow.click(44,284)
    values=drag(slow,[60,80,100,130,160,180])
    latest=values[-1]
    assert slow.wave()['pending'] and applied_duty(slow)!=latest
    assert slow.ui.number('slider_active')==0
    assert slow.ui.string(slow.ui.syms['field_cache'][0]+64)==f'{latest//100}.00 %'
    slow.scan(1100)
    # First old request may latch, but the UI never jumps back to that value.
    assert slow.ui.string(slow.ui.syms['field_cache'][0]+64)==f'{latest//100}.00 %'
    slow.scan(1100)
    assert applied_duty(slow)==latest and not slow.wave()['pending']
    slow.ui.snapshot('slider_low_frequency_settled.png')
    # Stop cancels both the pending request and any newer queued position.
    drag(slow,[50,90,130])
    slow.click(44,284);slow.scan(2200)
    assert not slow.wave()['enabled'] and not slow.wave()['pending']

    # Extended mode keeps the 5% grid, and channels have independent target queues.
    extended=ScreenSystem(False);extended.click(50,250)
    assert extended.call('Wave_GetMode')==extended.call('Input_GetMode')==1
    extended.click(80,84)
    values=drag(extended,[18,35,50,80,110,140,170,200,221])
    assert all(v%500==0 for v in values) and values[0]==0 and values[-1]==10000
    a=extended.wave()
    extended.click(198,24);drag(extended,[221,180,120,60,18])
    assert extended.wave()==a and applied_duty(extended,1)==0

    # Editor uses the same continuous gesture, but still requires Confirm.
    editor=ScreenSystem(False);editor.click(80,84);editor.click(180,244)
    before=editor.wave()
    editor.scan(8,True,40,108)
    assert diag(editor)[0]==1
    editor.scan(8,True,150,108)
    assert diag(editor)[0]==2
    assert editor.ui.string(editor.ui.syms['edit_value'][0])==str(diag(editor)[1]//100)
    value=diag(editor)[1];visible_track(editor,108)
    assert editor.wave()==before
    release(editor);editor.click(196,268)
    assert applied_duty(editor)==value
    print('PASS: slider press/drag/release, live numeric+Wave updates, persistent graphics, hysteresis, capture/clamp, PEN bounce, invalid sample, ownership, 1Hz latest-value queue, Stop, 5% grid, editor')
    (ROOT/'build/slider_results.json').write_text(json.dumps({
        'real_ARM_UI_Wave_drag':'PASS','physical_touch_drag':'pending user test'},indent=2))


if __name__=='__main__':main()
