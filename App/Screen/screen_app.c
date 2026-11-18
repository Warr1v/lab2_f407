#include "screen_app.h"
#include "screen_calibration.h"
#include "screen_touch_profile.h"
#include "screen_mock.h"
#include "snake.h"
#include "tftlcd.h"
#include "touch.h"
#include "delay.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Preserve existing business page numbers for SWD diagnostics. */
enum { PAGE_MENU=2, PAGE_PWM, PAGE_INPUT, PAGE_TIMER, PAGE_SNAKE };
enum { ACT_MENU=1, ACT_PWM, ACT_INPUT, ACT_TIMER, ACT_EXTENSIONS, ACT_SNAKE,
       ACT_START, ACT_RESET, ACT_LAP, ACT_SEGMENT, ACT_RECORDS,
       ACT_SNAKE_RUN, ACT_SNAKE_MODE, ACT_SNAKE_RESET,
       ACT_SNAKE_UP, ACT_SNAKE_RIGHT, ACT_SNAKE_DOWN, ACT_SNAKE_LEFT };
enum { BG=0xFFFF, FG=0x1082, ACCENT=0x051F, PALE=0xE73C, PRESSED=0xBDF7, ALERT=0xF800 };
typedef struct { int16_t x,y,w,h; const char *label; uint8_t action; bool enabled; } Button;

volatile ScreenDiagnostics g_screen_diag = {.magic=0x53435231}; /* SCR1 */
static ScreenCalibration calibration;
static bool extensions_enabled;
static ScreenMock mock;
static Button buttons[8];
static unsigned button_count, record_page;
static int16_t width, height;
static int captured = -1;
static uint8_t page;
static bool held, wait_release, gesture_cancelled, have_xy;
static uint32_t pen_up_at, last_good_at, last_scan, last_refresh;
static uint16_t raw_x, raw_y;
static uint32_t touch_count;
static char field_cache[10][40];
static uint16_t field_colors[10];
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
    if(chinese) text(x,y,s,color,BG); else ascii(x,y,s,color);
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
static void draw_page(void)
{
    LCD_Clear(BG);button_count=0;captured=-1;
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
    case PAGE_PWM:
        title("波输出");
        text(8,56,"频率",FG,BG);ascii(112,64,"-- Hz",FG);
        text(8,96,"占空比",FG,BG);ascii(144,104,"-- %",FG);
        text(8,140,"未接入",ALERT,BG);
        add_button(8,184,104,44,"频率",0,false);
        add_button(128,184,104,44,"占空比",0,false);
        add_button(8,240,104,44,"开启",0,false);
        add_button(128,240,104,44,"返回",ACT_MENU,true);
        frame();break;
    case PAGE_INPUT:
        title("输入检测");
        text(8,56,"频率",FG,BG);ascii(112,64,"-- Hz",FG);
        text(8,96,"占空比",FG,BG);ascii(144,104,"-- %",FG);
        text(8,144,"未接入",ALERT,BG);
        add_button(128,232,104,44,"返回",ACT_MENU,true);
        frame();break;
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
    if(page==PAGE_SNAKE && next!=PAGE_SNAKE) Snake_SetRunning(false);
    last_snake_step=HAL_GetTick();
    page=next;g_screen_diag.page=page;
    wait_release=true;gesture_cancelled=true;have_xy=false;
    draw_page();
}
static void action(uint8_t id,uint32_t now)
{
    switch(id) {
    case ACT_MENU:navigate(PAGE_MENU);return;
    case ACT_PWM:navigate(PAGE_PWM);return;
    case ACT_INPUT:navigate(PAGE_INPUT);return;
    case ACT_TIMER:navigate(PAGE_TIMER);return;
    case ACT_SNAKE:navigate(PAGE_SNAKE);return;
    case ACT_EXTENSIONS:extensions_enabled=!extensions_enabled;break;
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
        field(0,8,284,224,29,extensions_enabled?"扩展已开启":"扩展已关闭",
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
    extensions_enabled=false;
    page=PAGE_MENU;g_screen_diag.page=page;
    draw_page();
    g_screen_diag.stage=g_screen_diag.selftest!=0x7f?0xE1:calibration.valid?4:0xE2;
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
                if(first) {
                    for(unsigned i=0;i<button_count;++i) if(buttons[i].enabled && inside(&buttons[i],x,y)) {
                        captured=(int)i;draw_button(i,true);break;
                    }
                }
                if(captured>=0 && !inside(&buttons[captured],x,y)) {
                    gesture_cancelled=true;draw_button((unsigned)captured,false);captured=-1;
                }
                have_xy=true;last_good_at=now;
                g_screen_diag.x=(uint32_t)x;g_screen_diag.y=(uint32_t)y;
            } else if((uint32_t)(now-last_good_at)>120) {
                gesture_cancelled=true;
                if(captured>=0){draw_button((unsigned)captured,false);captured=-1;}
            }
        }
    } else if(held) {
        if(!pen_up_at) pen_up_at=now;
        if((uint32_t)(now-pen_up_at)>=16) {
            held=false;pen_up_at=0;
            if(wait_release) wait_release=false;
            else if(captured>=0) {
                const unsigned index=(unsigned)captured;captured=-1;draw_button(index,false);
                if(!gesture_cancelled && have_xy && (uint32_t)(now-last_good_at)<100)
                    action(buttons[index].action,now);
            }
        }
    } else {
        if(wait_release) wait_release=false;
    }
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
