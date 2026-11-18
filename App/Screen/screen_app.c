#include "screen_app.h"
#include "screen_calibration.h"
#include "screen_touch_profile.h"
#include "screen_mock.h"
#include "snake.h"
#include "business_api.h"
#include "tftlcd.h"
#include "touch.h"
#include "delay.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Preserve existing business page numbers for SWD diagnostics. */
enum { PAGE_MENU=2, PAGE_PWM, PAGE_INPUT, PAGE_TIMER, PAGE_SNAKE,
       PAGE_WAVE_MORE, PAGE_SWEEP, PAGE_NUMBER };
enum { ACT_MENU=1, ACT_PWM, ACT_INPUT, ACT_TIMER, ACT_EXTENSIONS, ACT_SNAKE,
       ACT_START, ACT_RESET, ACT_LAP, ACT_SEGMENT, ACT_RECORDS,
       ACT_SNAKE_RUN, ACT_SNAKE_MODE, ACT_SNAKE_RESET,
       ACT_SNAKE_UP, ACT_SNAKE_RIGHT, ACT_SNAKE_DOWN, ACT_SNAKE_LEFT,
       ACT_CHANNEL, ACT_FREQUENCY, ACT_DUTY, ACT_OUTPUT, ACT_MORE,
       ACT_SYNC, ACT_PHASE, ACT_SPWM, ACT_SWEEP, ACT_SWEEP_RUN,
       ACT_SWEEP_PAGE, ACT_REACQUIRE, ACT_BACK, ACT_CLEAR, ACT_DELETE,
       ACT_CONFIRM, ACT_DIGIT=100 };
enum { BG=0xFFFF, FG=0x1082, ACCENT=0x051F, PALE=0xE73C, PRESSED=0xBDF7, ALERT=0xF800 };
typedef struct { int16_t x,y,w,h; const char *label; uint8_t action; bool enabled; } Button;

volatile ScreenDiagnostics g_screen_diag = {.magic=0x53435231}; /* SCR1 */
volatile ScreenSliderDiagnostics g_screen_slider_diag;
static ScreenCalibration calibration;
static bool extensions_enabled;
static ScreenMock mock;
static Button buttons[20];
static unsigned button_count, record_page;
static int16_t width, height;
static int captured = -1;
static uint8_t page;
static bool held, wait_release, gesture_cancelled, have_xy;
static uint32_t pen_up_at, last_good_at, last_scan, last_refresh;
static uint16_t raw_x, raw_y;
static uint32_t touch_count;
static char field_cache[16][64];
static uint16_t field_colors[16];
static WaveChannel channel;
static unsigned sweep_page;
static BizResult ui_result, mode_result;
enum { EDIT_FREQUENCY, EDIT_DUTY, EDIT_PHASE };
static unsigned edit_kind;
static uint8_t edit_parent;
static char edit_value[8];
static bool edit_replace;
/* One latest-value queue per channel: never enqueue a backlog of finger positions. */
typedef struct { bool valid; uint16_t duty_bp; uint32_t request_id; } DutyTarget;
static DutyTarget duty_targets[2];
static bool slider_active;
static int16_t slider_down_x;
static uint16_t slider_value_bp;
static uint32_t slider_painted = UINT32_MAX;
enum { SLIDER_X=12, SLIDER_W=216, SLIDER_TRACK_X=18, SLIDER_SPAN=203,
       SLIDER_PWM_Y=138, SLIDER_PWM_H=32, SLIDER_NUMBER_Y=98, SLIDER_NUMBER_H=20 };
static void draw_page(void);
static void navigate(uint8_t next);
enum { SNAKE_X=8, SNAKE_Y=48, SNAKE_PIXELS=16, SNAKE_STEP_MS=180 };
static uint8_t snake_drawn[SNAKE_CELL_COUNT];
static uint32_t last_snake_step;
extern uint32_t ScreenCal_SelfTest(void);

static void fill(int x,int y,int w,int h,uint16_t color)
{
    if (w<=0 || h<=0 || x>=width || y>=height) return;
    int ex=x+w-1, ey=y+h-1;
    if (ex<0 || ey<0) return;
    if (x<0) x=0;
    if (y<0) y=0;
    if (ex>=width) ex=width-1;
    if (ey>=height) ey=height-1;
    LCD_Fill((u16)x,(u16)y,(u16)ex,(u16)ey,color);
}
static void text(int x,int y,const char *s,uint16_t fg,uint16_t bg)
{
    FRONT_COLOR=fg; BACK_COLOR=bg;
    LCD_ShowFontHZ((u16)x,(u16)y,(u8 *)s);
}
static void ascii(int x,int y,const char *s,uint16_t color)
{
    FRONT_COLOR=color; BACK_COLOR=BG;
    LCD_ShowString((u16)x,(u16)y,(u16)(width-x),16,16,(u8 *)s);
}
static void field(unsigned slot,int x,int y,int w,int h,const char *s,uint16_t color,bool chinese)
{
    if(strcmp(field_cache[slot],s)==0 && field_colors[slot]==color) return;
    snprintf(field_cache[slot],sizeof(field_cache[slot]),"%s",s);
    field_colors[slot]=color;
    fill(x,y,w,h,BG);
    if(chinese) text(x,y,s,color,BG);
    else {
        char clipped[64];snprintf(clipped,sizeof(clipped),"%.*s",w/8,s);
        ascii(x,y,clipped,color);
    }
}
static int text_width(const char *s)
{
    int n=0;
    const unsigned char *p=(const unsigned char *)s;
    while(*p) { if(*p<128) { n+=8; ++p; } else { n+=32; p+=3; } }
    return n;
}
static void frame(void)
{
    FRONT_COLOR=ACCENT;
    LCD_DrawRectangle(0,0,(u16)(width-1),(u16)(height-1));
    LCD_DrawRectangle(2,2,(u16)(width-3),(u16)(height-3));
}
static bool inside(const Button *b,int x,int y)
{
    return x>=b->x && y>=b->y && x<b->x+b->w && y<b->y+b->h;
}
static void draw_button(unsigned index,bool down)
{
    Button *b=&buttons[index];
    const uint16_t bg=down?PRESSED:PALE;
    fill(b->x,b->y,b->w,b->h,bg);
    FRONT_COLOR=b->enabled?ACCENT:GRAY;
    LCD_DrawRectangle((u16)b->x,(u16)b->y,(u16)(b->x+b->w-1),(u16)(b->y+b->h-1));
    text(b->x+(b->w-text_width(b->label))/2,b->y+(b->h-29)/2,b->label,b->enabled?FG:GRAY,bg);
}
static void add_button(int x,int y,int w,int h,const char *label,int action,bool enabled)
{
    if(button_count>=sizeof(buttons)/sizeof(buttons[0])) return;
    buttons[button_count]=(Button){x,y,w,h,label,(uint8_t)action,enabled};
    draw_button(button_count++,false);
}
static void title(const char *s)
{
    text(8,10,s,FG,BG);
}
static void snake_controls(void)
{
    const SnakeState *game=Snake_GetState();
    const bool active=game->status==SNAKE_READY || game->status==SNAKE_RUNNING || game->status==SNAKE_PAUSED;
    for(unsigned i=0;i<button_count;++i) {
        Button *b=&buttons[i];
        if(b->action==ACT_SNAKE_RUN) {
            b->label=game->status==SNAKE_RUNNING?"暂停":game->status==SNAKE_PAUSED?"继续":"开始";
            b->enabled=active;
        } else if(b->action>=ACT_SNAKE_UP && b->action<=ACT_SNAKE_LEFT) {
            b->enabled=!game->automatic && (game->status==SNAKE_READY || game->status==SNAKE_RUNNING);
        } else continue;
        /* Keep press feedback intact when the game changes state under a pen. */
        draw_button(i,captured==(int)i);
    }
}
static void refresh_snake(void)
{
    static SnakeStatus shown_status;
    static bool status_valid;
    const SnakeState *game=Snake_GetState();
    if(!status_valid || shown_status!=game->status || !field_cache[0][0]) {
        snake_controls();shown_status=game->status;status_valid=true;
    }
    static const uint16_t colors[]={PALE,0x05A0,0x02E0,ALERT};
    for(unsigned i=0;i<SNAKE_CELL_COUNT;++i) {
        if(snake_drawn[i]==game->cells[i]) continue;
        snake_drawn[i]=game->cells[i];
        fill(SNAKE_X+(i%SNAKE_COLUMNS)*SNAKE_PIXELS,
             SNAKE_Y+(i/SNAKE_COLUMNS)*SNAKE_PIXELS,
             SNAKE_PIXELS-1,SNAKE_PIXELS-1,colors[game->cells[i]]);
    }
    const char *status=game->status==SNAKE_READY?"就绪":game->status==SNAKE_RUNNING?"运行":
        game->status==SNAKE_PAUSED?"暂停":game->status==SNAKE_WON?"胜利":"失败";
    field(0,8,242,64,29,status,game->status==SNAKE_LOST?ALERT:FG,true);
    char score[24];
    snprintf(score,sizeof(score),"%s %u/117",game->automatic?"AUTO":"MAN",game->score);
    field(1,88,248,144,16,score,FG,false);
}
static const char *result_text(BizResult result)
{
    static const char *const labels[]={"OK", "PENDING", "BUSY: wait / sync / sweep",
        "INVALID: check PWM A/B", "OUT OF RANGE", "STEP: duty 1%/5%, phase 5",
        "UNSUPPORTED", "NOT INITIALIZED", "HARDWARE ERROR", "TIMEOUT", "CANCELLED"};
    return (unsigned)result<sizeof(labels)/sizeof(labels[0])?labels[result]:"ERROR";
}
static bool accepted(BizResult result) { return result==BIZ_OK || result==BIZ_PENDING; }
static bool sweep_locked(void)
{
    SweepSnapshot s;Sweep_GetSnapshot(&s);
    return s.owns_a || s.state==SWEEP_RUNNING || s.state==SWEEP_RESTORING;
}
static void set_button(unsigned action_id,const char *label,bool enabled)
{
    for(unsigned i=0;i<button_count;++i) if(buttons[i].action==action_id) {
        Button *b=&buttons[i];
        if(b->enabled!=enabled || strcmp(b->label,label)!=0) {
            b->label=label;b->enabled=enabled;draw_button(i,captured==(int)i);
        }
    }
}
static void format_frequency(char *s,size_t n,uint32_t millihz)
{
    snprintf(s,n,"%lu.%03lu Hz",(unsigned long)(millihz/1000U),(unsigned long)(millihz%1000U));
}
static bool slider_visible(void)
{
    return page==PAGE_PWM || (page==PAGE_NUMBER && edit_kind==EDIT_DUTY);
}
static int slider_y(void) { return page==PAGE_PWM?SLIDER_PWM_Y:SLIDER_NUMBER_Y; }
static int slider_height(void) { return page==PAGE_PWM?SLIDER_PWM_H:SLIDER_NUMBER_H; }
static bool slider_hit(int x,int y)
{
    return slider_visible() && x>=SLIDER_X && x<SLIDER_X+SLIDER_W &&
        y>=slider_y() && y<slider_y()+slider_height();
}
static bool slider_editable(void)
{
    if(page==PAGE_NUMBER) return edit_kind==EDIT_DUTY;
    WaveSnapshot w;WaveSyncSnapshot sync;
    Wave_GetSnapshot(channel,&w);Wave_GetSyncSnapshot(&sync);
    return w.owner==WAVE_OWNER_MANUAL && w.state!=WAVE_UNINITIALIZED && w.state!=WAVE_FAULT &&
        (!w.pending || (duty_targets[channel].valid && duty_targets[channel].request_id==w.request_id)) &&
        !sync.phase_pending && (sync.state==WAVE_SYNC_OFF || sync.state==WAVE_SYNC_ON);
}
static uint16_t slider_display_value(void)
{
    if(slider_active) return slider_value_bp;
    if(page==PAGE_NUMBER) {
        unsigned long value=strtoul(edit_value,NULL,10);
        return value>100U?10000U:(uint16_t)(value*100U);
    }
    if(duty_targets[channel].valid) return duty_targets[channel].duty_bp;
    WaveSnapshot w;Wave_GetSnapshot(channel,&w);
    return w.pending?w.requested.duty_bp:w.applied.duty_bp;
}
static void draw_slider(void)
{
    if(!slider_visible()) return;
    const bool enabled=slider_editable();
    const uint16_t value=slider_display_value();
    const uint32_t signature=value | (slider_active?0x10000U:0) | (enabled?0x20000U:0);
    if(signature==slider_painted) return;
    const int y=slider_y(), h=slider_height(), middle=y+h/2;
    const int knob=SLIDER_TRACK_X+(int)(((uint32_t)value*SLIDER_SPAN+5000U)/10000U);
    /* Erase only the old thumb outside the track. Never blank the entire bar
     * between touch frames, and never run generic button highlighting here. */
    if(slider_painted!=UINT32_MAX) {
        const int old_knob=SLIDER_TRACK_X+(int)(((slider_painted&0xffffU)*SLIDER_SPAN+5000U)/10000U);
        fill(old_knob-5,middle-8,11,5,BG);
        fill(old_knob-5,middle+3,11,5,BG);
    }
    slider_painted=signature;
    fill(SLIDER_TRACK_X,middle-3,knob-SLIDER_TRACK_X,6,enabled?ACCENT:GRAY);
    fill(knob,middle-3,SLIDER_TRACK_X+SLIDER_SPAN+1-knob,6,PALE);
    fill(knob-5,middle-8,11,16,enabled?(slider_active?PRESSED:ACCENT):GRAY);
    FRONT_COLOR=enabled?ACCENT:GRAY;
    LCD_DrawRectangle((u16)(knob-5),(u16)(middle-8),(u16)(knob+5),(u16)(middle+7));
}
static void slider_update(int x,bool first)
{
    int position=x-SLIDER_TRACK_X;
    if(position<0)position=0;
    if(position>SLIDER_SPAN)position=SLIDER_SPAN;
    const int step=Wave_GetMode()==BIZ_MODE_EXTENDED?500:100;
    const int levels=10000/step, scaled=position*levels;
    const int previous=slider_value_bp/step;
    int value=(scaled+SLIDER_SPAN/2)/SLIDER_SPAN;
    /* Half-pixel hysteresis around quantization boundaries, with exact ends.
     * ADC already median-filters each coordinate; no long smoothing delay. */
    if(!first && position>0 && position<SLIDER_SPAN) {
        const int margin=levels/2;
        if(value>previous && scaled<previous*SLIDER_SPAN+SLIDER_SPAN/2+margin) value=previous;
        if(value<previous && scaled>previous*SLIDER_SPAN-SLIDER_SPAN/2-margin) value=previous;
    }
    if(!first && abs(x-slider_down_x)>=2 && g_screen_slider_diag.phase!=SCREEN_SLIDER_DRAGGING)
        g_screen_slider_diag.phase=SCREEN_SLIDER_DRAGGING;
    const uint16_t bp=(uint16_t)(value*step);
    if(!first && bp==slider_value_bp) return;
    slider_value_bp=bp;g_screen_slider_diag.value_bp=bp;
    if(!first) ++g_screen_slider_diag.moves;
    if(page==PAGE_NUMBER) {
        snprintf(edit_value,sizeof(edit_value),"%u",bp/100U);edit_replace=false;
    } else {
        DutyTarget *target=&duty_targets[channel];
        if(!target->valid) target->request_id=0;
        target->duty_bp=bp;target->valid=true;
    }
    ui_result=BIZ_OK;last_refresh=0;
}
static void slider_begin(int x)
{
    slider_value_bp=slider_display_value();slider_active=true;slider_down_x=(int16_t)x;
    g_screen_slider_diag.phase=SCREEN_SLIDER_PRESSED;++g_screen_slider_diag.presses;
    slider_update(x,true);
}
static void slider_finish(bool cancelled)
{
    if(!slider_active) return;
    slider_active=false;
    g_screen_slider_diag.phase=cancelled?SCREEN_SLIDER_CANCELLED:SCREEN_SLIDER_RELEASED;
    if(cancelled) ++g_screen_slider_diag.cancellations;else ++g_screen_slider_diag.releases;
    last_refresh=0;
}
static void service_duty_targets(void)
{
    for(unsigned i=0;i<2;++i) {
        DutyTarget *target=&duty_targets[i];if(!target->valid)continue;
        WaveSnapshot w;Wave_GetSnapshot((WaveChannel)i,&w);
        if(w.owner!=WAVE_OWNER_MANUAL || w.state==WAVE_FAULT || w.state==WAVE_UNINITIALIZED) {
            target->valid=false;
            if(page==PAGE_PWM && channel==i) {slider_finish(true);ui_result=BIZ_BUSY;}
            continue;
        }
        if(w.pending) {
            if(w.request_id!=target->request_id) {
                target->valid=false;
                if(page==PAGE_PWM && channel==i) {slider_finish(true);ui_result=BIZ_BUSY;}
            }
            continue;
        }
        if(w.applied.duty_bp==target->duty_bp) {
            target->valid=false;
            if(page==PAGE_PWM && channel==i && ui_result==BIZ_PENDING)ui_result=w.last_result;
            continue;
        }
        WaveParams params=w.applied;params.duty_bp=target->duty_bp;
        const BizResult result=Wave_Apply((WaveChannel)i,WAVE_OWNER_MANUAL,&params,&target->request_id);
        if(page==PAGE_PWM && channel==i)ui_result=result;
        if(!accepted(result) && result!=BIZ_BUSY) {
            target->valid=false;
            if(page==PAGE_PWM && channel==i)slider_finish(true);
        }
    }
}
static void begin_edit(unsigned kind,uint8_t parent)
{
    WaveSnapshot w;WaveSyncSnapshot sync;
    Wave_GetSnapshot(channel,&w);Wave_GetSyncSnapshot(&sync);
    edit_kind=kind;edit_parent=parent;edit_replace=true;ui_result=BIZ_OK;
    unsigned long value=kind==EDIT_PHASE?sync.applied_phase_deg:
        kind==EDIT_DUTY?w.applied.duty_bp/100U:w.applied.frequency_hz;
    snprintf(edit_value,sizeof(edit_value),"%lu",value);
    navigate(PAGE_NUMBER);
}
static void change_mode(void)
{
    const BizMode old=Wave_GetMode();
    const BizMode next=old==BIZ_MODE_STANDARD?BIZ_MODE_EXTENDED:BIZ_MODE_STANDARD;
    ui_result=Wave_SetMode(next);
    if(ui_result==BIZ_OK) {
        ui_result=Input_SetMode(next);
        if(ui_result!=BIZ_OK) {
            /* No other business call can interleave this main-loop transaction. */
            const BizResult rollback=Wave_SetMode(old);
            if(rollback!=BIZ_OK) ui_result=rollback;
        }
    }
    extensions_enabled=Wave_GetMode()==BIZ_MODE_EXTENDED && Input_GetMode()==BIZ_MODE_EXTENDED;
    mode_result=ui_result;
}
static bool business_action(uint8_t id)
{
    WaveSnapshot w;WaveSyncSnapshot sync;SpwmSnapshot spwm;
    if(id>=ACT_DIGIT && id<ACT_DIGIT+10) {
        size_t n=strlen(edit_value);
        if(edit_replace || (n==1 && edit_value[0]=='0')) {n=0;edit_replace=false;}
        if(n<6) {edit_value[n]=(char)('0'+id-ACT_DIGIT);edit_value[n+1]=0;}
        ui_result=BIZ_OK;return true;
    }
    switch(id) {
    case ACT_CHANNEL:channel=channel==WAVE_CHANNEL_A?WAVE_CHANNEL_B:WAVE_CHANNEL_A;draw_page();return true;
    case ACT_FREQUENCY:begin_edit(EDIT_FREQUENCY,PAGE_PWM);return true;
    case ACT_DUTY:begin_edit(EDIT_DUTY,PAGE_PWM);return true;
    case ACT_PHASE:begin_edit(EDIT_PHASE,PAGE_WAVE_MORE);return true;
    case ACT_MORE:navigate(PAGE_WAVE_MORE);return true;
    case ACT_BACK:navigate(page==PAGE_NUMBER?edit_parent:PAGE_PWM);return true;
    case ACT_SWEEP:sweep_page=0;ui_result=BIZ_OK;navigate(PAGE_SWEEP);return true;
    case ACT_SWEEP_PAGE:sweep_page^=1U;draw_page();return true;
    case ACT_SWEEP_RUN: {
        SweepSnapshot s;Sweep_GetSnapshot(&s);
        ui_result=s.state==SWEEP_RUNNING?Sweep_Cancel():Sweep_Start();return true;
    }
    case ACT_REACQUIRE: {
        uint32_t id_out;
        ui_result=sweep_locked()?BIZ_BUSY:Input_BeginWindow(&id_out);return true;
    }
    case ACT_OUTPUT:
        Wave_GetSnapshot(channel,&w);
        if(w.enabled) duty_targets[channel].valid=false; /* Stop also cancels queued finger positions. */
        ui_result=Wave_SetEnabled(channel,WAVE_OWNER_MANUAL,!w.enabled,NULL);return true;
    case ACT_SYNC:
        Wave_GetSyncSnapshot(&sync);
        ui_result=Wave_SetSync(sync.state==WAVE_SYNC_OFF);return true;
    case ACT_SPWM:
        Spwm_GetSnapshot(&spwm);ui_result=spwm.running?Spwm_Stop():Spwm_Start();return true;
    case ACT_CLEAR:edit_value[0]=0;edit_replace=false;ui_result=BIZ_OK;return true;
    case ACT_DELETE: {
        const size_t n=strlen(edit_value);if(n) edit_value[n-1]=0;
        edit_replace=false;ui_result=BIZ_OK;return true;
    }
    case ACT_CONFIRM: {
        if(!edit_value[0]) {ui_result=BIZ_INVALID_ARGUMENT;return true;}
        const uint32_t value=(uint32_t)strtoul(edit_value,NULL,10);
        if(edit_kind==EDIT_PHASE) ui_result=value>360U?BIZ_OUT_OF_RANGE:Wave_SetPhase((uint16_t)value);
        else {
            Wave_GetSnapshot(channel,&w);
            WaveParams params=w.applied;
            if(edit_kind==EDIT_DUTY) {
                if(value>100U) {ui_result=BIZ_OUT_OF_RANGE;return true;}
                params.duty_bp=(uint16_t)(value*100U);
            } else params.frequency_hz=value;
            ui_result=Wave_Apply(channel,WAVE_OWNER_MANUAL,&params,NULL);
        }
        if(accepted(ui_result)) navigate(edit_parent);
        return true;
    }
    default:return false;
    }
}
static void draw_business_page(void)
{
    switch(page) {
    case PAGE_PWM:
        title("波输出");
        add_button(166,6,66,36,channel==WAVE_CHANNEL_A?"A":"B",ACT_CHANNEL,true);
        ascii(8,48,channel==WAVE_CHANNEL_A?"A: PA8 / JP3.33":"B: PC7 / JP3.38",FG);
        text(8,72,"频率",FG,BG);text(8,106,"占空比",FG,BG);
        add_button(8,226,104,36,"频率",ACT_FREQUENCY,true);
        add_button(128,226,104,36,"占空比",ACT_DUTY,true);
        add_button(8,274,72,36,"开启",ACT_OUTPUT,true);
        add_button(84,274,72,36,"扩展",ACT_MORE,true);
        add_button(160,274,72,36,"返回",ACT_MENU,true);
        break;
    case PAGE_INPUT:
        title("输入检测");
        text(8,56,"频率",FG,BG);text(8,96,"占空比",FG,BG);
        ascii(8,180,"PA1 / JP4.23 | GND 41/42",FG);
        add_button(8,224,104,38,"重测",ACT_REACQUIRE,true);
        add_button(128,224,104,38,"自检",ACT_SWEEP,true);
        add_button(128,272,104,38,"返回",ACT_MENU,true);
        break;
    case PAGE_WAVE_MORE:
        title("扩展");
        add_button(8,78,104,42,"同步",ACT_SYNC,true);
        add_button(128,78,104,42,"相位",ACT_PHASE,true);
        add_button(8,158,104,42,"SPWM",ACT_SPWM,true);
        add_button(128,158,104,42,"自检",ACT_SWEEP,true);
        ascii(8,208,"SYNC: equal Hz, PWM A+B ON",FG);
        ascii(8,228,"SPWM PA3: 20kHz -> 50Hz",FG);
        add_button(128,270,104,38,"返回",ACT_BACK,true);
        break;
    case PAGE_SWEEP:
        title("扫频自检");
        ascii(8,78,"Set Hz / Meas Hz / Err %",FG);
        ascii(8,228,"PA8 -> PA1; source OFF",FG);
        add_button(8,266,72,42,"开始",ACT_SWEEP_RUN,true);
        add_button(84,266,72,42,"下页",ACT_SWEEP_PAGE,true);
        add_button(160,266,72,42,"返回",ACT_MENU,true);
        break;
    case PAGE_NUMBER: {
        title(edit_kind==EDIT_FREQUENCY?"频率":edit_kind==EDIT_DUTY?"占空比":"相位");
        add_button(80,6,72,36,"清除",ACT_CLEAR,true);
        add_button(160,6,72,36,"取消",ACT_BACK,true);
        const char *range=edit_kind==EDIT_PHASE?"0..360 deg, step 5":edit_kind==EDIT_DUTY?
            (Wave_GetMode()==BIZ_MODE_EXTENDED?"0..100 %, step 5":"0..100 %, step 1"):
            (Wave_GetMode()==BIZ_MODE_EXTENDED?"1..500000 Hz":"1..200000 Hz");
        ascii(8,78,range,FG);
        static const char *const digits[]={"1","2","3","4","5","6","7","8","9"};
        for(unsigned i=0;i<9;++i) add_button(8+(int)(i%3)*76,124+(int)(i/3)*42,72,36,digits[i],ACT_DIGIT+i+1,true);
        add_button(8,250,72,36,"<-",ACT_DELETE,true);
        add_button(84,250,72,36,"0",ACT_DIGIT,true);
        add_button(160,250,72,36,"确认",ACT_CONFIRM,true);
        break;
    }
    }
    draw_slider();frame();
}
static void refresh_business(uint32_t now)
{
    char line[64];
    if(page==PAGE_PWM) {
        WaveSnapshot w;WaveSyncSnapshot sync;Wave_GetSnapshot(channel,&w);Wave_GetSyncSnapshot(&sync);
        if(ui_result==BIZ_PENDING && !w.pending && !sync.phase_pending &&
           (sync.state==WAVE_SYNC_OFF || sync.state==WAVE_SYNC_ON))
            ui_result=w.last_result;
        format_frequency(line,sizeof(line),w.calculated_frequency_millihz);
        field(0,104,80,128,16,line,ACCENT,false);
        const uint16_t displayed_duty=slider_display_value();
        snprintf(line,sizeof(line),"%u.%02u %%",displayed_duty/100,displayed_duty%100);
        field(1,112,114,120,16,line,ACCENT,false);
        const char *state=w.state==WAVE_UNINITIALIZED?"初始化失败":w.state==WAVE_FAULT?"输出故障":
            w.owner==WAVE_OWNER_SELFTEST?"自检占用":w.pending?"等待生效":!w.enabled?"已关闭":
            w.state==WAVE_DC_LOW?"恒低电平":w.state==WAVE_DC_HIGH?"恒高电平":"运行";
        field(2,8,174,224,29,state,w.state==WAVE_FAULT?ALERT:FG,true);
        if(accepted(ui_result)) snprintf(line,sizeof(line),"ACT %u.%02u%%  %s",w.calculated_duty_bp/100,
            w.calculated_duty_bp%100,(w.pending || duty_targets[channel].valid)?"WAIT":"OK");
        else snprintf(line,sizeof(line),"%s",result_text(ui_result));
        field(3,8,206,224,16,line,accepted(ui_result)?FG:ALERT,false);
        const bool manual=w.owner==WAVE_OWNER_MANUAL && w.state!=WAVE_UNINITIALIZED && w.state!=WAVE_FAULT;
        const bool ready=manual && !w.pending && !sync.phase_pending &&
            (sync.state==WAVE_SYNC_OFF || sync.state==WAVE_SYNC_ON);
        set_button(ACT_FREQUENCY,"频率",ready && !duty_targets[channel].valid && sync.state==WAVE_SYNC_OFF);
        set_button(ACT_DUTY,"占空比",ready && !duty_targets[channel].valid);
        set_button(ACT_OUTPUT,w.enabled?"关闭":"开启",manual && sync.state==WAVE_SYNC_OFF && (!w.pending || w.enabled));
    } else if(page==PAGE_INPUT) {
        InputSnapshot in;Input_GetSnapshot(&in);
        const bool valid=in.state==INPUT_VALID;
        if(valid) format_frequency(line,sizeof(line),in.frequency_millihz);else strcpy(line,"-- Hz");
        field(0,104,64,128,16,line,ACCENT,false);
        if(valid) snprintf(line,sizeof(line),"%u.%02u %%",in.duty_bp/100,in.duty_bp%100);else strcpy(line,"-- %");
        field(1,112,104,120,16,line,ACCENT,false);
        const char *state=in.state==INPUT_VALID?"信号有效":in.state==INPUT_WAITING?"等待信号":
            in.state==INPUT_NO_SIGNAL?"无信号":in.state==INPUT_OUT_OF_RANGE?"频率超范围":"采集错误";
        field(2,8,140,224,29,state,in.state>=INPUT_OUT_OF_RANGE?ALERT:FG,true);
        snprintf(line,sizeof(line),"%s age:%lums",in.mode==BIZ_MODE_EXTENDED?"500k":"200k",
            (unsigned long)(valid?(uint32_t)(now-in.published_ms):0));
        field(3,8,202,224,16,line,FG,false);
        set_button(ACT_REACQUIRE,"重测",!sweep_locked());
        field(4,8,276,112,32,result_text(ui_result),accepted(ui_result)?FG:ALERT,false);
    } else if(page==PAGE_WAVE_MORE) {
        WaveSyncSnapshot sync;SpwmSnapshot spwm;Wave_GetSyncSnapshot(&sync);Spwm_GetSnapshot(&spwm);
        if(ui_result==BIZ_PENDING && !sync.phase_pending &&
           (sync.state==WAVE_SYNC_OFF || sync.state==WAVE_SYNC_ON) && spwm.last_result!=BIZ_PENDING)
            ui_result=sync.last_result!=BIZ_OK?sync.last_result:spwm.last_result;
        snprintf(line,sizeof(line),"SYNC:%s B:%u deg%s",sync.last_result==BIZ_HARDWARE_ERROR?"ERR":
            sync.state==WAVE_SYNC_OFF?"OFF":sync.state==WAVE_SYNC_ON?"ON":"WAIT",
            sync.applied_phase_deg,sync.phase_pending?" *":"");
        field(0,8,50,224,16,line,FG,false);
        snprintf(line,sizeof(line),"SPWM:%s 50Hz / PA3",spwm.last_result==BIZ_HARDWARE_ERROR?"ERROR":spwm.running?"ON":"OFF");
        field(1,8,134,224,16,line,FG,false);
        field(2,8,248,224,16,result_text(ui_result),accepted(ui_result)?FG:ALERT,false);
        set_button(ACT_SYNC,sync.state==WAVE_SYNC_OFF?"同步":"退出",!sweep_locked() && sync.state!=WAVE_SYNC_STOPPING);
        set_button(ACT_PHASE,"相位",sync.state==WAVE_SYNC_ON && sync.phase_applicable && !sync.phase_pending);
        set_button(ACT_SPWM,spwm.running?"关闭":"SPWM",spwm.initialized);
    } else if(page==PAGE_SWEEP) {
        SweepSnapshot s;Sweep_GetSnapshot(&s);
        const char *state=s.state==SWEEP_IDLE?"就绪":s.state==SWEEP_RUNNING?"运行":s.state==SWEEP_RESTORING?"恢复中":
            s.state==SWEEP_PASS?"PASS":s.state==SWEEP_FAIL?"FAIL":"已取消";
        field(0,8,44,160,29,state,s.state==SWEEP_FAIL?ALERT:FG,true);
        snprintf(line,sizeof(line),"%u/10",s.completed);field(1,176,52,56,16,line,FG,false);
        for(unsigned row=0;row<5;++row) {
            const SweepPoint *p=&s.points[sweep_page*5+row];
            if(p->state==SWEEP_POINT_PASS || p->state==SWEEP_POINT_OUT_OF_TOLERANCE) {
                const uint64_t target=(uint64_t)p->set_frequency_hz*1000U;
                const uint64_t diff=p->measured_frequency_millihz>target?p->measured_frequency_millihz-target:target-p->measured_frequency_millihz;
                const unsigned err=target?(unsigned)((diff*10000U+target/2)/target):0;
                char measured[16];
                if(p->measured_frequency_millihz<10000000U)
                    snprintf(measured,sizeof(measured),"%lu.%02lu",(unsigned long)(p->measured_frequency_millihz/1000U),
                        (unsigned long)(p->measured_frequency_millihz%1000U/10U));
                else snprintf(measured,sizeof(measured),"%lu",(unsigned long)(p->measured_frequency_millihz/1000U));
                snprintf(line,sizeof(line),"%6lu %8s %u.%02u%c",(unsigned long)p->set_frequency_hz,measured,
                    err/100,err%100,p->state==SWEEP_POINT_PASS?'P':'F');
            } else snprintf(line,sizeof(line),"%6lu %8s %s",(unsigned long)p->set_frequency_hz,"--",
                p->state==SWEEP_POINT_PENDING?"WAIT":p->state==SWEEP_POINT_NO_SIGNAL?"NO SIG":
                p->state==SWEEP_POINT_OUTPUT_ERROR?"OUT ERR":"IN ERR");
            field(2+row,8,104+(int)row*24,224,18,line,
                p->state>SWEEP_POINT_PASS?ALERT:FG,false);
        }
        snprintf(line,sizeof(line),"%s %s",s.restored?"RESTORED":s.owns_a?"A LOCKED":"A FREE",result_text(ui_result));
        field(7,8,246,224,16,line,s.state==SWEEP_FAIL?ALERT:FG,false);
        set_button(ACT_SWEEP_RUN,s.state==SWEEP_RUNNING?"取消":"开始",
            s.state==SWEEP_RUNNING || (!s.owns_a && s.state!=SWEEP_RESTORING));
        set_button(ACT_SWEEP_PAGE,sweep_page?"上页":"下页",true);
    } else if(page==PAGE_NUMBER) {
        snprintf(line,sizeof(line),"%s %s",edit_value,edit_kind==EDIT_FREQUENCY?"Hz":edit_kind==EDIT_DUTY?"%":"deg");
        field(0,8,50,224,20,line,ACCENT,false);
        field(1,8,296,224,16,result_text(ui_result),accepted(ui_result)?FG:ALERT,false);
    }
    draw_slider();
}
static void draw_page(void)
{
    LCD_Clear(BG);button_count=0;captured=-1;
    slider_painted=UINT32_MAX;
    memset(field_cache,0,sizeof(field_cache));
    memset(field_colors,0,sizeof(field_colors));
    switch(page) {
    case PAGE_MENU:
        title("功能菜单");
        add_button(16,64,208,44,"波输出",ACT_PWM,true);
        add_button(16,116,208,44,"输入检测",ACT_INPUT,true);
        add_button(16,168,208,44,"秒表",ACT_TIMER,true);
        add_button(8,232,104,44,"扩展",ACT_EXTENSIONS,true);
        add_button(128,232,104,44,"贪吃蛇",ACT_SNAKE,true);
        frame();break;
    case PAGE_PWM:case PAGE_INPUT:case PAGE_WAVE_MORE:case PAGE_SWEEP:case PAGE_NUMBER:
        draw_business_page();break;
    case PAGE_SNAKE:
        title("贪吃蛇");
        memset(snake_drawn,0xff,sizeof(snake_drawn));
        fill(SNAKE_X-1,SNAKE_Y-1,SNAKE_COLUMNS*SNAKE_PIXELS+1,SNAKE_ROWS*SNAKE_PIXELS+1,FG);
        add_button(128,8,104,36,"开始",ACT_SNAKE_RUN,true);
        add_button(176,48,56,40,"上",ACT_SNAKE_UP,true);
        add_button(176,98,56,40,"左",ACT_SNAKE_LEFT,true);
        add_button(176,148,56,40,"右",ACT_SNAKE_RIGHT,true);
        add_button(176,198,56,40,"下",ACT_SNAKE_DOWN,true);
        add_button(8,274,72,38,Snake_GetState()->automatic?"手动":"自动",ACT_SNAKE_MODE,true);
        add_button(84,274,72,38,"重置",ACT_SNAKE_RESET,true);
        add_button(160,274,72,38,"返回",ACT_MENU,true);
        refresh_snake();
        frame();break;
    case PAGE_TIMER:
        title("秒表");
        add_button(8,216,72,40,mock.stopwatch_running?"暂停":"开始",ACT_START,true);
        add_button(84,216,72,40,"记圈",ACT_LAP,mock.stopwatch_running);
        add_button(160,216,72,40,"记段",ACT_SEGMENT,mock.stopwatch_running);
        add_button(8,264,72,40,"重置",ACT_RESET,true);
        add_button(84,264,72,40,"记录",ACT_RECORDS,mock.record_count>3);
        add_button(160,264,72,40,"返回",ACT_MENU,true);
        frame();break;
    }
    last_refresh=0;
}
static void navigate(uint8_t next)
{
    slider_finish(true);
    if(page==PAGE_SNAKE && next!=PAGE_SNAKE) Snake_SetRunning(false);
    last_snake_step=HAL_GetTick();
    page=next;g_screen_diag.page=page;
    wait_release=true;gesture_cancelled=true;have_xy=false;
    draw_page();
}
static void action(uint8_t id,uint32_t now)
{
    if(business_action(id)) {last_refresh=0;return;}
    switch(id) {
    case ACT_MENU:navigate(PAGE_MENU);return;
    case ACT_PWM:ui_result=BIZ_OK;navigate(PAGE_PWM);return;
    case ACT_INPUT:ui_result=BIZ_OK;navigate(PAGE_INPUT);return;
    case ACT_TIMER:navigate(PAGE_TIMER);return;
    case ACT_SNAKE:navigate(PAGE_SNAKE);return;
    case ACT_EXTENSIONS:change_mode();break;
    case ACT_SNAKE_RUN:
        Snake_SetRunning(Snake_GetState()->status!=SNAKE_RUNNING);last_snake_step=now;break;
    case ACT_SNAKE_MODE:case ACT_SNAKE_RESET:
        Snake_Reset(id==ACT_SNAKE_MODE?!Snake_GetState()->automatic:Snake_GetState()->automatic,now);
        last_snake_step=now;draw_page();break;
    case ACT_SNAKE_UP:case ACT_SNAKE_RIGHT:case ACT_SNAKE_DOWN:case ACT_SNAKE_LEFT: {
        const bool ready=Snake_GetState()->status==SNAKE_READY;
        Snake_SetDirection((SnakeDirection)(id-ACT_SNAKE_UP));
        if(ready) last_snake_step=now;
        break;
    }
    case ACT_START:ScreenMock_ToggleTimer(&mock,now);draw_page();break;
    case ACT_RESET:ScreenMock_ResetTimer(&mock,now);record_page=0;draw_page();break;
    case ACT_LAP:case ACT_SEGMENT:ScreenMock_Record(&mock,now,id==ACT_SEGMENT);record_page=0;draw_page();break;
    case ACT_RECORDS:record_page^=1;break;
    }
    last_refresh=0;
}
static void format_time(char *buffer,size_t size,uint32_t ms)
{
    snprintf(buffer,size,"%02lu:%02lu:%02lu.%03lu",(unsigned long)(ms/3600000),
        (unsigned long)(ms/60000%60),(unsigned long)(ms/1000%60),(unsigned long)(ms%1000));
}
static void refresh(uint32_t now)
{
    if(last_refresh && (uint32_t)(now-last_refresh)<100) return;
    last_refresh=now;
    char line[40];
    if(page==PAGE_MENU) {
        field(1,8,44,224,16,mode_result!=BIZ_OK?result_text(mode_result):
            extensions_enabled?"1..500kHz | duty step 5%":"1..200kHz | duty step 1%",mode_result!=BIZ_OK?ALERT:FG,false);
        field(0,8,284,224,29,mode_result!=BIZ_OK?"切换失败":extensions_enabled?"扩展已开启":"扩展已关闭",
              extensions_enabled?ACCENT:FG,true);
    } else if(page==PAGE_TIMER) {
        char time[24];format_time(time,sizeof(time),ScreenMock_Time(&mock,now));
        /* Keep the large time field dirty only when its formatted value changes. */
        if(strcmp(field_cache[0],time)!=0) {
            snprintf(field_cache[0],sizeof(field_cache[0]),"%s",time);
            fill(8,56,224,32,BG);FRONT_COLOR=ACCENT;BACK_COLOR=BG;
            LCD_ShowString(8,56,224,24,24,(u8 *)time);
        }
        field(1,8,96,224,29,mock.stopwatch_running?"运行":"暂停",FG,true);
        for(unsigned n=0;n<3;++n) {
            int index=(int)mock.record_count-1-(int)(record_page*3+n);
            line[0]=0;
            if(index>=0) {
                const ScreenMockRecord *r=&mock.records[index];format_time(time,sizeof(time),r->ms);
                snprintf(line,sizeof(line),"%c%03u %s",r->kind,r->number,time);
            }
            field(2+n,8,136+20*n,224,20,line,FG,false);
        }
    }
    refresh_business(now);
}
bool ScreenApp_ExtensionsEnabled(void)
{
    return extensions_enabled;
}

void ScreenApp_Init(void)
{
    g_screen_diag.stage=1;
    g_screen_diag.selftest=ScreenCal_SelfTest();
    ScreenDelay_Init();ScreenTouch_Init();ScreenMock_Init(&mock);
    Snake_Reset(false,HAL_GetTick());
    g_screen_diag.stage=2;
    TFTLCD_Init();
    LCD_LED=1; /* GPIO configuration belongs to CubeMX; enable the module backlight. */
    width=(int16_t)tftlcd_data.width;height=(int16_t)tftlcd_data.height;
    g_screen_diag.lcd_id=tftlcd_data.id;
    g_screen_diag.stage=3;
    /* Direct menu boot using this panel's previously validated mapping. */
    calibration=screen_touch_profile;
    calibration.valid=calibration.valid && width==240 && height==320;
    g_screen_diag.calibration_result=calibration.valid?2:3;
    extensions_enabled=Wave_GetMode()==BIZ_MODE_EXTENDED && Input_GetMode()==BIZ_MODE_EXTENDED;
    ui_result=BIZ_OK;channel=WAVE_CHANNEL_A;
    memset(duty_targets,0,sizeof(duty_targets));slider_active=false;
    memset((void *)&g_screen_slider_diag,0,sizeof(g_screen_slider_diag));
    page=PAGE_MENU;g_screen_diag.page=page;
    draw_page();
    g_screen_diag.stage=g_screen_diag.selftest!=0x7f?0xE1:calibration.valid?4:0xE2;
}

void ScreenApp_Process(uint32_t now)
{
    if((uint32_t)(now-last_scan)<8) return;
    last_scan=now;
    service_duty_targets();
    const uint32_t service_start=DWT->CYCCNT;
    const uint32_t scan_start=DWT->CYCCNT;
    const ScreenTouchSample sample=ScreenTouch_Read();
    uint32_t us=(DWT->CYCCNT-scan_start)/(SystemCoreClock/1000000U);
    if(us>g_screen_diag.max_scan_us)g_screen_diag.max_scan_us=us;
    g_screen_diag.heartbeat=now;
    g_screen_diag.pen_down=sample.pressed;g_screen_diag.sample_valid=sample.valid;
    if(sample.pressed) {
        raw_x=sample.raw_x;raw_y=sample.raw_y;
        g_screen_diag.raw_x=raw_x;g_screen_diag.raw_y=raw_y;
        pen_up_at=0;
        if(!held) {
            held=true;gesture_cancelled=false;captured=-1;have_xy=false;
            ++touch_count;g_screen_diag.touch_count=touch_count;
            last_good_at=now;
        }
        if(!wait_release) {
            int16_t x,y;
            if(sample.valid && ScreenCal_Map(&calibration,raw_x,raw_y,width,height,&x,&y)) {
                const bool first=!have_xy;
                g_screen_diag.x=(uint32_t)x;g_screen_diag.y=(uint32_t)y;
                if(first) {
                    if(slider_hit(x,y) && slider_editable()) slider_begin(x);
                    else {
                        for(unsigned i=0;i<button_count;++i) if(buttons[i].enabled && inside(&buttons[i],x,y)) {
                            captured=(int)i;draw_button(i,true);break;
                        }
                    }
                }
                /* A slider captures this entire gesture. Leaving its rectangle
                 * clamps X; it cannot turn into a button click beneath a finger. */
                if(slider_active && !first) slider_update(x,false);
                if(!slider_active && captured>=0 && !inside(&buttons[captured],x,y)) {
                    gesture_cancelled=true;draw_button((unsigned)captured,false);captured=-1;
                }
                have_xy=true;last_good_at=now;
            } else if((uint32_t)(now-last_good_at)>120) {
                gesture_cancelled=true;
                slider_finish(true);
                if(captured>=0){draw_button((unsigned)captured,false);captured=-1;}
            }
        }
    } else if(held) {
        if(!pen_up_at) pen_up_at=now;
        if((uint32_t)(now-pen_up_at)>=16) {
            held=false;pen_up_at=0;
            slider_finish(gesture_cancelled || !have_xy || (uint32_t)(now-last_good_at)>=120);
            if(wait_release) wait_release=false;
            else if(captured>=0) {
                const unsigned index=(unsigned)captured;captured=-1;draw_button(index,false);
                if(buttons[index].enabled && !gesture_cancelled && have_xy && (uint32_t)(now-last_good_at)<100)
                    action(buttons[index].action,now);
            }
        }
    } else {
        if(wait_release) wait_release=false;
    }
    service_duty_targets();
    if(page==PAGE_SNAKE) {
        if((uint32_t)(now-last_snake_step)>=SNAKE_STEP_MS) {
            /* One step per service; never burst through delayed frames. */
            last_snake_step=now;Snake_Step();
        }
        refresh_snake();
    }
    refresh(now);
    us=(DWT->CYCCNT-service_start)/(SystemCoreClock/1000000U);
    if(us>g_screen_diag.max_service_us)g_screen_diag.max_service_us=us;
}
