#include "screen_app.h"
#include "screen_calibration.h"
#include "screen_mock.h"
#include "tftlcd.h"
#include "touch.h"
#include "delay.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { PAGE_DEBUG, PAGE_CALIBRATE, PAGE_MENU, PAGE_PWM, PAGE_INPUT, PAGE_TIMER };
enum { CAL_NONE, CAL_RUNNING, CAL_SUCCESS, CAL_FAILED, CAL_CANCELLED, CAL_TIMEOUT };
enum { ACT_CLEAR=1, ACT_CALIBRATE, ACT_MENU, ACT_DEBUG, ACT_PWM, ACT_INPUT, ACT_TIMER,
       ACT_FREQ, ACT_DUTY, ACT_OUTPUT, ACT_SIGNAL, ACT_START, ACT_RESET, ACT_LAP, ACT_SEGMENT, ACT_RECORDS };
enum { BG=0xFFFF, FG=0x1082, ACCENT=0x051F, PALE=0xE73C, PRESSED=0xBDF7, ALERT=0xF800 };
typedef struct { int16_t x,y,w,h; const char *label; uint8_t action; bool enabled; } Button;

volatile ScreenDiagnostics g_screen_diag = {.magic=0x53435231}; /* SCR1 */
static ScreenCalibration calibration;
static ScreenCalPoint cal_points[5];
static ScreenMock mock;
static Button buttons[7];
static unsigned button_count, cal_index, record_page;
static int16_t width, height, last_x, last_y;
static int captured = -1;
static uint8_t page;
static bool held, wait_release, gesture_cancelled, have_xy;
static uint32_t pressed_at, pen_up_at, last_good_at, last_scan, last_refresh;
static uint32_t cal_target_at, cal_stable_at, cal_n, cal_sum_x, cal_sum_y;
static uint16_t cal_anchor_x, cal_anchor_y;
static uint16_t raw_x, raw_y;
static uint32_t touch_count;
static char field_cache[10][40];
static uint16_t field_colors[10];
static const char *event_label = "UP";
static uint32_t event_at;
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
    if(chinese) text(x,y,s,color,BG); else ascii(x,y,s,color);
}
static int text_width(const char *s)
{
    int n=0;
    const unsigned char *p=(const unsigned char *)s;
    while(*p) { if(*p<128) { n+=8; ++p; } else { n+=32; p+=3; } }
    return n;
}
static void cross(int x,int y,int radius,uint16_t color)
{
    fill(x-radius,y,2*radius+1,1,color);
    fill(x,y-radius,1,2*radius+1,color);
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
static void title(const char *s,bool is_mock)
{
    text(8,10,s,FG,BG);
    if(is_mock) text(width-72,10,"模拟",ALERT,BG);
}
static void edge_points(void)
{
    const int xs[3]={7,width/2,width-8}, ys[3]={7,height/2,height-8};
    for(unsigned y=0;y<3;++y) for(unsigned x=0;x<3;++x)
        cross(xs[x],ys[y],5,(x==1 && y==1)?GREEN:ACCENT);
}
static const char *cal_status(void)
{
    switch(g_screen_diag.calibration_result) {
    case CAL_SUCCESS:return "校准成功";
    case CAL_FAILED:return "校准失败";
    case CAL_CANCELLED:return "已取消";
    case CAL_TIMEOUT:return "校准超时";
    default:return calibration.valid?"校准有效":"未校准";
    }
}
static void cal_target(unsigned index,int16_t *x,int16_t *y)
{
    const int16_t positions[5][2]={{24,48},{width-25,48},{width-25,height-80},
                                  {24,height-80},{width/2,height/2-16}};
    *x=positions[index][0]; *y=positions[index][1];
}
static void draw_calibration(void)
{
    title("内存校准",false);
    for(unsigned i=0;i<5;++i) {
        int16_t x,y;cal_target(i,&x,&y);
        cross(x,y,10,i==cal_index?ALERT:PALE);
        if(i==cal_index) { FRONT_COLOR=ALERT; LCD_Draw_Circle(x,y,12); }
    }
    char msg[16]; snprintf(msg,sizeof(msg),"%u / 5",cal_index+1);
    ascii(96,92,msg,FG);
    text(56,184,"点按十字",FG,BG);
    ascii(20,218,"Hold 100ms, then release",FG);
    text(56,276,"长按取消",ALERT,BG);
    ascii(24,306,"2s cancel / 30s timeout",FG);
}
static void draw_page(void)
{
    LCD_Clear(BG);button_count=0;captured=-1;
    memset(field_cache,0,sizeof(field_cache));
    memset(field_colors,0,sizeof(field_colors));
    switch(page) {
    case PAGE_DEBUG:
        title("屏幕调试",false);
        ascii(width-48,20,"T02",ACCENT);
        text(8,202,cal_status(),calibration.valid?ACCENT:ALERT,BG);
        ascii(8,238,"Hold 1.5s: RAM calibration",FG);
        add_button(8,270,72,40,"清除",ACT_CLEAR,true);
        add_button(84,270,72,40,"校准",ACT_CALIBRATE,calibration.valid);
        add_button(160,270,72,40,"菜单",ACT_MENU,calibration.valid);
        frame();edge_points();break;
    case PAGE_CALIBRATE:
        draw_calibration();frame();break;
    case PAGE_MENU:
        title("功能菜单",true);
        add_button(16,64,208,44,"波输出",ACT_PWM,true);
        add_button(16,116,208,44,"输入检测",ACT_INPUT,true);
        add_button(16,168,208,44,"秒表",ACT_TIMER,true);
        add_button(8,232,104,44,"调试",ACT_DEBUG,true);
        add_button(128,232,104,44,"校准",ACT_CALIBRATE,true);
        ascii(8,296,"ALL BUSINESS DATA: MOCK",ALERT);frame();break;
    case PAGE_PWM:
        title("波输出",true);
        text(8,56,"频率",FG,BG);text(8,96,"占空比",FG,BG);
        add_button(8,184,104,44,"频率",ACT_FREQ,true);
        add_button(128,184,104,44,"占空比",ACT_DUTY,true);
        add_button(8,240,104,44,mock.output_enabled?"关闭":"开启",ACT_OUTPUT,true);
        add_button(128,240,104,44,"返回",ACT_MENU,true);
        ascii(8,300,"MOCK ONLY - no PWM signal",ALERT);frame();break;
    case PAGE_INPUT:
        title("输入检测",true);
        text(8,56,"频率",FG,BG);text(8,96,"占空比",FG,BG);
        add_button(8,232,104,44,"信号",ACT_SIGNAL,true);
        add_button(128,232,104,44,"返回",ACT_MENU,true);
        ascii(8,296,"MOCK ONLY - no capture",ALERT);frame();break;
    case PAGE_TIMER:
        title("秒表",true);
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
    page=next;g_screen_diag.page=page;
    wait_release=true;gesture_cancelled=true;have_xy=false;
    draw_page();
}
static void begin_calibration(uint32_t now)
{
    cal_index=0;cal_n=0;cal_target_at=now;cal_stable_at=0;
    g_screen_diag.calibration_result=CAL_RUNNING;
    navigate(PAGE_CALIBRATE);
}
static void action(uint8_t id,uint32_t now)
{
    switch(id) {
    case ACT_CLEAR:touch_count=0;g_screen_diag.touch_count=0;draw_page();break;
    case ACT_CALIBRATE:begin_calibration(now);return;
    case ACT_MENU:navigate(PAGE_MENU);return;
    case ACT_DEBUG:navigate(PAGE_DEBUG);return;
    case ACT_PWM:navigate(PAGE_PWM);return;
    case ACT_INPUT:navigate(PAGE_INPUT);return;
    case ACT_TIMER:navigate(PAGE_TIMER);return;
    case ACT_FREQ:
        mock.frequency_hz=mock.frequency_hz==1000?10000:mock.frequency_hz==10000?200000:mock.frequency_hz==200000?1:1000;break;
    case ACT_DUTY:mock.duty_percent=mock.duty_percent==50?0:mock.duty_percent==0?100:50;break;
    case ACT_OUTPUT:mock.output_enabled=!mock.output_enabled;draw_page();break;
    case ACT_SIGNAL:mock.input_state=(mock.input_state+1)%3;break;
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
    if(page==PAGE_DEBUG) {
        snprintf(line,sizeof(line),"RAW X:%4u Y:%4u",raw_x,raw_y);field(0,8,48,224,16,line,FG,false);
        if(calibration.valid && have_xy) snprintf(line,sizeof(line),"XY  X:%3d  Y:%3d",last_x,last_y);
        else snprintf(line,sizeof(line),"XY  -- not calibrated --");
        field(1,8,68,224,16,line,calibration.valid?ACCENT:ALERT,false);
        snprintf(line,sizeof(line),"PEN:%lu %s %s N:%lu",g_screen_diag.pen_down?0UL:1UL,event_label,g_screen_diag.sample_valid?"OK":"--",(unsigned long)touch_count);
        field(2,8,88,224,16,line,FG,false);
        snprintf(line,sizeof(line),"LCD:%04lX RAM:%s",(unsigned long)g_screen_diag.lcd_id,calibration.valid?"OK":"NONE");
        field(3,8,116,224,16,line,FG,false);
        fill(width-12,16,5,5,(now/500)%2?GREEN:ACCENT);
    } else if(page==PAGE_CALIBRATE) {
        snprintf(line,sizeof(line),"RAW:%4u,%4u  %s",raw_x,raw_y,cal_n>=5?"OK":"WAIT");
        field(0,8,250,224,16,line,FG,false);
    } else if(page==PAGE_PWM) {
        snprintf(line,sizeof(line),"%lu Hz",(unsigned long)mock.frequency_hz);field(0,112,60,120,20,line,ACCENT,false);
        snprintf(line,sizeof(line),"%u %%",mock.duty_percent);field(1,136,100,96,20,line,ACCENT,false);
        field(2,8,136,224,29,mock.output_enabled?"开启":"关闭",mock.output_enabled?ACCENT:FG,true);
    } else if(page==PAGE_INPUT) {
        field(0,112,60,120,20,mock.input_state?"-- Hz":"1000.00 Hz",ACCENT,false);
        field(1,136,100,96,20,mock.input_state?"-- %":"50.00 %",ACCENT,false);
        field(2,8,144,224,29,mock.input_state==0?"有效":mock.input_state==1?"无信号":"超时",mock.input_state?ALERT:ACCENT,true);
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
}
static void touch_marker(int x,int y,bool first)
{
    if(first) {
        cross(x,y,5,ALERT);
        char number[12];snprintf(number,sizeof(number),"%lu",(unsigned long)touch_count);
        int tx=x+7,ty=y+4;
        if(tx+(int)strlen(number)*8>=width) tx=x-(int)strlen(number)*8-2;
        if(ty+16>=height) ty=y-17;
        if(tx>=0 && ty>=0) ascii(tx,ty,number,ALERT);
    } else fill(x-1,y-1,3,3,ALERT);
}

void ScreenApp_Init(void)
{
    g_screen_diag.stage=1;
    g_screen_diag.selftest=ScreenCal_SelfTest();
    ScreenDelay_Init();ScreenTouch_Init();ScreenMock_Init(&mock);
    g_screen_diag.stage=2;
    TFTLCD_Init();
    LCD_LED=1; /* GPIO configuration belongs to CubeMX; enable the module backlight. */
    width=(int16_t)tftlcd_data.width;height=(int16_t)tftlcd_data.height;
    g_screen_diag.lcd_id=tftlcd_data.id;
    g_screen_diag.stage=3;
    /* Boot color bars and edge pixels provide an immediate visual wiring check. */
    LCD_Clear(BG);
    const uint16_t colors[4]={RED,GREEN,BLUE,WHITE};
    for(int n=0;n<4;++n) fill(n*width/4,0,width/4,height,colors[n]);
    HAL_Delay(700);
    page=PAGE_DEBUG;g_screen_diag.page=page;
    draw_page();
    g_screen_diag.stage=g_screen_diag.selftest==0x7f?4:0xE1;
}

void ScreenApp_Process(uint32_t now)
{
    if((uint32_t)(now-last_scan)<8) return;
    last_scan=now;
    const uint32_t service_start=DWT->CYCCNT;
    const uint32_t scan_start=DWT->CYCCNT;
    const ScreenTouchSample sample=ScreenTouch_Read();
    uint32_t us=(DWT->CYCCNT-scan_start)/(SystemCoreClock/1000000U);
    if(us>g_screen_diag.max_scan_us)g_screen_diag.max_scan_us=us;
    g_screen_diag.heartbeat=now;
    g_screen_diag.pen_down=sample.pressed;g_screen_diag.sample_valid=sample.valid;
    if(sample.pressed) {
        if(held && sample.valid && (abs((int)raw_x-sample.raw_x)+abs((int)raw_y-sample.raw_y)>=12)) {
            event_label="MOVE";event_at=now;
        }
        raw_x=sample.raw_x;raw_y=sample.raw_y;
        g_screen_diag.raw_x=raw_x;g_screen_diag.raw_y=raw_y;
        pen_up_at=0;
        if(!held) {
            held=true;pressed_at=now;gesture_cancelled=false;captured=-1;have_xy=false;
            ++touch_count;g_screen_diag.touch_count=touch_count;
            last_good_at=now;cal_n=0;cal_stable_at=0;
            event_label="DOWN";event_at=now;
        }
        if(!wait_release) {
            if(page==PAGE_CALIBRATE) {
                if((uint32_t)(now-pressed_at)>=2000) {
                    g_screen_diag.calibration_result=CAL_CANCELLED;navigate(PAGE_DEBUG);
                } else if(sample.valid) {
                    if(!cal_n || abs((int)raw_x-cal_anchor_x)>80 || abs((int)raw_y-cal_anchor_y)>80) {
                        cal_n=0;cal_sum_x=0;cal_sum_y=0;cal_stable_at=now;
                        cal_anchor_x=raw_x;cal_anchor_y=raw_y;
                    }
                    cal_sum_x+=raw_x;cal_sum_y+=raw_y;++cal_n;
                } else { cal_n=0;cal_stable_at=0; }
            } else if(page==PAGE_DEBUG && (uint32_t)(now-pressed_at)>=1500) {
                begin_calibration(now);
            } else {
                int16_t x,y;
                if(sample.valid && ScreenCal_Map(&calibration,raw_x,raw_y,width,height,&x,&y)) {
                    const bool first=!have_xy;
                    if(first) {
                        for(unsigned i=0;i<button_count;++i) if(buttons[i].enabled && inside(&buttons[i],x,y)) {
                            captured=(int)i;draw_button(i,true);break;
                        }
                    }
                    if(captured>=0 && !inside(&buttons[captured],x,y)) {
                        gesture_cancelled=true;draw_button((unsigned)captured,false);captured=-1;
                    }
                    if(page==PAGE_DEBUG && captured<0 && !gesture_cancelled
                       && (first || abs(x-last_x)+abs(y-last_y)>=2)) touch_marker(x,y,first);
                    if(!first && (abs(x-last_x)+abs(y-last_y)>=2)) {event_label="MOVE";event_at=now;}
                    last_x=x;last_y=y;have_xy=true;last_good_at=now;
                    g_screen_diag.x=(uint32_t)x;g_screen_diag.y=(uint32_t)y;
                } else if((uint32_t)(now-last_good_at)>120) {
                    gesture_cancelled=true;
                    if(captured>=0){draw_button((unsigned)captured,false);captured=-1;}
                    event_label="CANCEL";event_at=now;
                }
            }
        }
    } else if(held) {
        if(!pen_up_at) pen_up_at=now;
        if((uint32_t)(now-pen_up_at)>=16) {
            held=false;pen_up_at=0;event_label="UP";event_at=now;
            if(wait_release) wait_release=false;
            else if(page==PAGE_CALIBRATE) {
                if(cal_n>=5 && (uint32_t)(now-cal_stable_at)>=80) {
                    int16_t x,y;cal_target(cal_index,&x,&y);
                    cal_points[cal_index]=(ScreenCalPoint){(uint16_t)(cal_sum_x/cal_n),(uint16_t)(cal_sum_y/cal_n),x,y};
                    if(++cal_index==5) {
                        g_screen_diag.calibration_result=ScreenCal_Fit(&calibration,cal_points)?CAL_SUCCESS:CAL_FAILED;
                        navigate(PAGE_DEBUG);
                    } else { cal_target_at=now;draw_page(); }
                }
                cal_n=0;cal_stable_at=0;
            } else if(captured>=0) {
                const unsigned index=(unsigned)captured;captured=-1;draw_button(index,false);
                if(!gesture_cancelled && have_xy && (uint32_t)(now-last_good_at)<100)
                    action(buttons[index].action,now);
            }
        }
    } else {
        if(wait_release) wait_release=false;
        if(page==PAGE_DEBUG && !calibration.valid) { raw_x=sample.raw_x;raw_y=sample.raw_y; }
    }
    if(page==PAGE_CALIBRATE && (uint32_t)(now-cal_target_at)>=30000) {
        g_screen_diag.calibration_result=CAL_TIMEOUT;navigate(PAGE_DEBUG);
    }
    if(!held && (uint32_t)(now-event_at)>800) event_label="UP";
    refresh(now);
    us=(DWT->CYCCNT-service_start)/(SystemCoreClock/1000000U);
    if(us>g_screen_diag.max_service_us)g_screen_diag.max_service_us=us;
}
