# 波输出模块集成与验证

本模块实现 P1/API v1 的全部 Wave 和 Spwm 函数：基础与扩展双路 PWM、同频相位、独立 DMA SPWM，以及输入 AI 的唯一扫频控制器所需的独占、应用确认和恢复接口。**现有波输出/输入页仍显示“未接入”，业务控件禁用**，秒表保留原 HAL tick 行为；正式 UI 和真实扫频接入由总负责人组织。

参考固定快照 `EDA_Task2-c0c4af479f2eeeec071a3e2e81a88b5b73179d09` 的分频计算与 B 同步余量思路，重新实现事务、启停、状态和同步切换，内部比较改为 TIM1_CH2。SPWM 按 P1 改为更新请求 DMA，不沿用参考的逐周期中断或其全局回调。没有复制参考主工程、时钟、屏幕或触摸。

## 文件与初始化所有权

- `wave_config.h` 集中范围、档位、相位与 SPWM 配置，编译断言防止零步进、无效端点或不合法默认值。
- `wave_math.c/.h` 进行独立整数计算；`wave.c` 管理应用事务、模式、同步和所有权。
- `wave_hw.c/.h` 独占初始化 TIM1/TIM8 和 PA8/PC7，使用 CMSIS 寄存器，调用 HAL 的时钟查询、GPIO 和 NVIC 工具。不引入 SPL，也不需要 HAL TIM 驱动或全局句柄。
- `spwm.c` 独占初始化 TIM2/PA3 和 DMA1 Stream1；不重置 DMA1，不碰 Stream2/4 或 TIM5。
- `wave_diagnostics.c/.h` 提供观察快照及显式测试命令。命令默认不编入正式固件。
- `FILTER.md` 是简易滤波接线及待执行仪器验收说明；`DELIVERY.md` 保留阶段结果。
- `App/Contracts/business_api.h` 原样复制公共模板，不依赖 HAL。

本分支未更改 `.ioc`、时钟、GPIO/FSMC 生成代码、MSP、HAL 配置、驱动、启动、链接、屏幕、游戏及模拟秒表。新增定时器/AF/DMA 由上述模块唯一初始化，公共入口只保留调用；不要同时启用第二套 `MX_TIMx_Init`/MSP 或 DMA 初始化覆盖这些寄存器。若总负责人后续决定由 CubeMX 管理新增资源，需一起迁移所有权，再重生成 `.ioc`，不能只额外添加初始化。

公共变更仅有 CMake、主入口 USER CODE 与共享 IRQ 转发。主入口在既有 GPIO/FSMC 之后执行 `Wave_Init → Spwm_Init → ScreenApp_Init`，循环执行 `Wave_Process → WaveTest_Process → ScreenApp_Process`。未来整机顺序为 `Wave_Process → Input_Process → Sweep_Process → WaveTest_Process → ScreenApp_Process`。检查业务初始化返回值和快照，不因屏幕切页再次初始化。

## 资源与中断

| 功能 | 资源 | 运行机制 |
| --- | --- | --- |
| A | PA8/JP3.33，AF1，TIM1_CH1，168 MHz | PWM1、ARR/CCR 预装载，16 位 N≤65535 |
| B | PC7/JP3.38，AF3，TIM8_CH2，168 MHz | 独立 PWM；同步时内部 ITR0 接 TIM1 TRGO |
| 相位 | TIM1 内部 CH2，无 PA9 输出 | 非零用 PWM2 OC2REF；零/360°用 UPDATE |
| SPWM | PA3/JP4.25，AF1，TIM2_CH4，84 MHz | 20 kHz、400 点、50 Hz，0.8 调制指数 |
| DMA | DMA1 Stream1 Channel3 → TIM2_CCR4 | UPDATE UDE，循环半字，800 B 普通 SRAM |

`TIM1_UP_TIM10_IRQHandler → Wave_TIM1_UpdateIRQ`、`TIM8_UP_TIM13_IRQHandler → Wave_TIM8_UpdateIRQ`，优先级 6；只在事务或同步边界待确认时开启 UIE，确认一次即关。不要因此占用 TIM10/TIM13；将来有共享用户必须分别转发有效标志。

`DMA1_Stream1_IRQHandler → Spwm_DMA1_Stream1IRQ`，优先级 10，DMA 硬件优先级 Medium。输入 Stream2/4 应使用更高优先级。SPWM 不启用 HT/TC 或 TIM2 周期中断，只有 DMA 错误进入 ISR。没有覆盖任何全局 HAL 回调。

硬件 ISR 最大时间尚未上板测量。`g_wave_runtime.a_updates/b_updates` 记录确认次数，`a_max_cycles/b_max_cycles` 用已有 DWT 计数器记录处理函数内观察到的最大周期；本模块不启用或清零共享 DWT。若 DWT 未启用，周期数为零，不表示耗时为零。除以 168 MHz 换算时间，另计异常入口/退出和排队延迟；观察最大值不是严格 WCET。需要最终与输入 DMA、高频输出和屏幕/游戏同时测量。

## 接口和单位

整数频率为 Hz，计算频率为 mHz（字段明确为 `millihz`），占空比 `duty_bp` 为 0～10000，1 bp=0.01 个百分点。相位请求整数度，计算相位为毫度。参数配置与快照归模块所有；调用者只操作公共 API 和复制出的快照。

| API | 行为 |
| --- | --- |
| Init | 默认标准模式，两路关闭且低，保存 1000 Hz/50%；幂等，不重置运行状态 |
| GetCapabilities/GetMode | 返回已初始化模块的真实能力与当前档位；SPWM 初始化成功后才报告 spwm=true |
| Apply | 完整组校验；关闭时保存后仍低；运行时整组在下个安全更新边界装载；不等待周期 |
| SetEnabled | 显式启停，保留已应用参数；重复开启不重启计数器；停止关低并取消未装载事务 |
| GetSnapshot | 一致复制，包含 requested/applied、计算值、状态、pending、请求/应用/取消 ID 和时间 |
| SetMode | 标准 200 kHz/1%，扩展全范围 500 kHz/5%；两路现值合法、无 pending、同步 OFF 且 A 未被自检占用才允许 |
| SetSync/SetPhase/GetSyncSnapshot | 独立/同频模式分开；B 上升沿相对 A 延迟；相位 0～360°、5° 一档，保留请求与实际量化值 |
| AcquireA/ReleaseA | SELFTEST 只在同步 OFF、A 无 pending 时独占；未完成事务不允许释放 |
| Spwm Init/Start/Stop/GetSnapshot | 独立于 A/B 及页面；默认低，启动从样本 0 开始；错误停止且低 |

返回值遵循模板：`BIZ_OK` 已完成，`BIZ_PENDING` 已接受待硬件确认；BUSY、非法参数、越界、步进错误、未初始化、硬件错误均不产生半组应用。请求 ID 仅接受时更新，0 保留为“尚无请求”，回绕从 UINT32_MAX 到 1；比较是否相同，不按大小推断先后。每路仅一笔待应用事务。

`accepted_ms` 是接受操作时的 HAL tick；`applied_ms` 是更新事件被 IRQ/后台确认时的 tick，包含确认延迟，**不是硬件边沿的精确时间戳**。实际预装载由硬件边界完成，最迟一个旧有效周期；CPU 不等该周期。停止在短临界区内先禁止更新、确认已有 UIF，再取消尚未装载者，避免把已生效但中断未处理的请求误记为取消。

0%/100% 是合法开启状态，分别报告 DC_LOW/DC_HIGH，不等于 STOPPED。用 CCR=0/N 实现，保持 PWM1 不切强制模式。运行中不软件 UG、不停止计数器来改频率或占空比。

同步要求同频、两路运行且有上升沿。同步中不能单路改频或关断，须先显式退出；占空比可以独立改到直流，`phase_applicable=false` 并保留相位，新的调相请求被拒绝。B 同步高电平按 3 个 A 计数粒度量化；标准上限理论占空比量化误差≤0.179 个百分点，扩展上限≤0.447，另有显示取整≤0.005，均不含硬件误差。

初次同步和退出都按边界衔接，不关计数器；允许用于调相的低电平延长。提前调相每步保留至少约半个原低电平时间，因此低频/99% 下大幅提前可能跨很多周期，保持 pending。显式退出可以终止剩余小步，只完成已经暂存的比较源边界，再恢复 B 独立计数；退出并不冒充剩余相位已全部应用。0°/非零切换先保证比较参考低，避免选择恒高参考生成错误触发沿。物理边界仍须示波器验证。

后台发现运行定时器意外失去 CEN/MOE 时关低、取消未生效事务并报告 FAULT；独立模式只停故障通道，同步模式停两路并解除同步，保留 A 所有权。显式停止清故障后才能重启。该检查不检测外部短路、波形失真、时钟漂移等模拟问题。DMA 错误由 SPWM 自己关低并计数。

## 扫频端的调用约定

输入 AI 唯一实现 `App/SelfTest`；这里没有 Sweep/Input 函数、链接桩或第二份正式扫频流程。测试脚本里的受控客户端只验证 Wave 契约，不产生测量 PASS 数据。

锁 A 后保存 `applied` 与 `enabled`。每次提交保存返回 ID，只在快照该 ID 真正 applied 且无 pending 后建立新输入采集窗口。普通 A 写操作、切模式和开启同步在独占期间返回 BUSY；B 独立输出与 SPWM 可以继续。

恢复时保持锁：提交原参数→等真正 applied→恢复原 enabled→确认完成→ReleaseA。用户取消可通过 SELFTEST 的 SetEnabled(false) 取消尚未装载的 A 事务；不要把被取消的 ID 当已应用。故障不自动释放所有权；失败恢复策略和最终 PASS/FAIL/CANCELLED 由唯一 Sweep 控制器负责。

## 默认关闭的测试入口

默认 Debug/Release 中 `g_wave_diag` 只读观察（magic=0x57564431），包含心跳、A/B/同步/SPWM 快照；命令邮箱及测试改参处理没有链接进固件，不会自动改输出。

需要专门测试版时在本 worktree 配置独立构建目录，不改变正式 preset：

```powershell
cmake -S . -B build/WaveDiagnostics -G Ninja '-DCMAKE_TOOLCHAIN_FILE=cmake/gcc-arm-none-eabi.cmake' '-DCMAKE_BUILD_TYPE=Debug' '-DWAVE_ENABLE_DIAGNOSTICS=ON'
cmake --build build/WaveDiagnostics
```

测试版增加 `g_wave_test_command`：先写 operation/channel/value/params，最后改变 sequence；等 `g_wave_diag.acknowledged_sequence` 相同再发下一条。命令表为 Apply=1、Enable=2、Mode=3、Sync=4、Phase=5、SPWM Start=6、SPWM Stop=7、AcquireA=8、ReleaseA=9、Case=10、Abort=11。channel A=0/B=1，Mode 标准=0/扩展=1；Enable/Sync 的 value=0/1。命令确认表示 API 返回，pending 的硬件完成仍看业务快照 ID。

Case 的 value 是 `g_wave_test_cases` 的 0～15 索引，包含 1 Hz↔200 kHz、直流/高低占空比、分频边界、非整除点及 500 kHz。单次显式命令按非阻塞状态机应用参数、等 ID、启动并确认；完成后保留输出，不自动循环。扩展频点需事先把现值调为合法 5% 档位并切模式；错误会报告，不偷偷修正参数。Abort 取消测试并请求关闭选定通道，不抢占 SELFTEST 或影响另一路。

建议演示：Case 0→1→2→3→4→7；B 用 Apply+Enable 异频运行；将两路同频并等 ID 后 Sync=1；Phase 按 0→355→5→360；Sync=0 并等 OFF；两路改为 50% 后 Mode=1、Case 12/13；SPWM Start=6；最后分别停止。每一步核对实际状态和错误，不依据模拟页数字判断。

`command_cycle` 和 `counter_before` 是命令入口采样，未改变 DWT。示波器单次采集覆盖旧波形→新波形，核对末个旧周期与首个新周期、窄脉冲和 DC 边界；结合计数器入口相位与已应用 ID 诊断。若要精确证明“API 接受到新波形”的端到端时间，需要独立请求标记输出和示波器另一通道；当前未分配额外 GPIO，须先由总负责人分配，不能拿 SPWM/其他模块脚代用。禁止用断点暂停业务来声称无阻塞过渡已验收。

## 可复现软件验证

在 P1 的 CMake/Ninja/ARM GCC 已入 PATH、Python 可用的终端运行 `powershell -File tests/run_wave_checks.ps1`。依赖局部安装在忽略的 build 目录，不改全局 Python。脚本构建三个配置、运行 Debug/Release/诊断版真实 ARM ELF 测试、原字库检查和既有游戏/屏幕/秒表回归。

受控 MMIO 只模拟更新事件、标志和 DMA 管线；不模拟硅片时序、GPIO 电气、IRQ 实际耗时或 DMA 仲裁。Unicorn 2.1.4 的宽 MMIO hook 会干扰优化 Thumb IT 状态，测试改为只拦截必须模拟的 SR/EGR/故障寄存器边界，不修改 ELF 指令。

仪器频率/占空比/相位误差、直流毛刺、滤波后幅度/偏置/纹波及输入+SPWM+游戏并发负载，均须完成共享板串行排程后另测。当前未烧录、未接线、未取得示波器读数。
