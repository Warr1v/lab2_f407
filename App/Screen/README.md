# 屏幕菜单固件 T03 + 贪吃蛇

2026-11-18，ILI9341、240×320 竖屏、电阻触摸。贪吃蛇接入后 Debug/Release 构建及 ARM 模拟执行检查通过；用户随后授权烧录，Debug 已下载、回读校验成功并复位运行，SWD 读取屏幕 stage=4、selftest=0x7f、页面=6。完整物理游戏操作仍待验收。屏幕基线交接：`D:/code/Project/实验室/lab2/ai_tasks/reports/screen_handoff.md`；本次接入交接：`D:/code/Project/实验室/lab2/ai_tasks/reports/snake_handoff.md`。

## 操作

- 上电直接进入中文菜单，删除色条启动页、调试页和交互校准页。
- “扩展”有效点击后切换一次，底部显示“扩展已开启/扩展已关闭”。默认关闭，切页保留，复位恢复关闭。
- “贪吃蛇”进入 10×12 棋盘。深绿为蛇头，绿为蛇身，红为食物；每吃一份食物得一分，117 分吃满棋盘获胜。
- 默认手动模式，按“开始”或有效方向启动；右侧“上/左/右/下”松开转向，拒绝反向及同一步内第二次转向。每 180ms 前进一步，碰墙/蛇身失败。
- 顶部“开始/暂停/继续”控制运行；暂停时方向键禁用。“自动/手动”按钮显示将切换到的模式，切换时重开；自动模式立即沿闭环运行，方向键禁用。
- “重置”保留当前模式重开；“返回”在离开时暂停并保留棋盘、分数和模式，再次进入按“继续”。就绪/失败/胜利状态保持；不追赶切页或卡顿期间遗漏的步数。
- 波输出/输入检测页显示“未接入”和 `--`，移除固定假数值及信号状态循环；未接入的输出控件禁用。
- 秒表保留原有 HAL tick 软件计时、开始/暂停、重置、圈/段和六条滚动记录；切页继续计时。尚未接入独立计时模块，未验收精度。
- 全部页面移除“模拟”角标和 MOCK 英文提示。数字键盘继续暂缓。

触摸按下高亮，松开触发一次；移出取消，切页防穿透。电阻屏建议使用触笔或指甲轻按。

## 触摸映射

删除校准页前，通过 SWD HotPlug 读取 T02 有效 RAM 映射，保存到 `App/Screen/screen_touch_profile.h`。十六进制浮点常量精确保留原六个系数；固件开机加载，因此断电/复位后也可直接使用菜单，无需 EEPROM。

此配置仅适用于当前物理面板及 240×320 竖屏，更换屏幕或方向需重新生成映射。尺寸不匹配时拒绝映射，诊断 stage=0xE2。本版无交互校准恢复入口，保留拟合/映射算法及七项自检供维护。

必须保留 T02 的 `ScreenTouch_Init()` 首次无条件 ADC 读取和每 200ms 空闲探测，不能恢复为仅 PEN 低才首次读取。GPIO/FSMC 由 CubeMX 独占初始化；屏幕服务不新增业务定时器、DMA、串口、EEPROM 或外部 SRAM。

## 接口与维护

GPIO/FSMC 初始化后调用 `ScreenApp_Init()`，主循环调用 `ScreenApp_Process(HAL_GetTick())`。新增 `bool ScreenApp_ExtensionsEnabled(void)` 可在主循环读取扩展选项；当前仅管理 UI 状态，未连接 PWM、捕获、相位等实际业务扩展。

`App/Mock/screen_mock.c/.h` 暂保留为旧秒表 UI 的 HAL tick 状态容器，仅计时相关接口仍被使用。其频率、占空比、输入状态及输出开关字段不再用于页面显示或操作，后续接入真实计时模块时可移除此依赖。

加入既有工具链 PATH 后，在工程目录运行：

```powershell
cmake --build --preset Debug
cmake --build --preset Release
python BSP/Screen/tools/verify_assets.py
python tests/verify_snake.py
```

游戏由 `App/Game/snake.c/.h` 提供，不依赖 HAL、LCD 或触摸；CMake 新增该源码及头文件目录，不增加硬件资源。`Snake_Reset()`、`Snake_SetDirection()`、`Snake_SetRunning()`、`Snake_Step()`、`Snake_GetState()` 仅在主循环使用。`Snake_SetRunning(false)` 暂停，`true` 启动或继续；终局状态必须重置。保持 10×12 棋盘，改尺寸需重新验证自动闭环及字节索引。

`tests/verify_snake.py` 用 Unicorn 执行实际 Debug ELF，模拟 LCD/触摸边界；依赖 `unicorn==2.1.4`、`pyelftools==0.32`，可安装到 `build/snake_checks/python`。它检查游戏边界和真实 UI 触摸流程，输出截图、`result.json` 到 `build/snake_checks/`；不连接开发板，不验证物理触摸和 LCD 时序。

`BSP/Screen/tools/make_font.ps1` 从主机 SimSun 离线生成字库；新增文案需先补字模。当前 57 个 UI 汉字由 110 个有效字模覆盖，原始例程资源哈希检查通过。

RAM 诊断保留 `g_screen_diag` 和 `g_touch_diag`，地址以对应 ELF 为准。stage=4、selftest=0x7f 表示初始化及算法检查通过，不替代手触验证。页面编号：菜单=2、波输出=3、输入=4、秒表=5、贪吃蛇=6。
