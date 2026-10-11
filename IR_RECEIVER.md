# GPIO7 原始红外波形采集

## 接线与启动

TSOP38238：OUT → GPIO7，GND → GND，VS → 3V3。现有麦克风 GPIO4/5/6 保持原配置。

红外功能按职责拆分为 `main/ir/receiver/`、`main/ir/transmitter/` 和 `main/ir/gree/`。接收模块的 `main/ir/receiver/ir_receiver.c` 使用 ESP-IDF 5.5.5 的 `driver/rmt_rx.h`；`main/ir/receiver/ir_receiver.h` 对外提供 `ir_receiver_start()`，由 `app_main()` 调用。Wi-Fi、语音和云端任务继续运行。接收模块只采集原始波形。

在已激活 ESP-IDF 5.5.5 的 PowerShell 中执行：

```powershell
idf.py build
idf.py -p COM4 -b 115200 flash monitor
```

波特率 115200 同时用于上述烧录命令及当前项目的串口监视器。启动时应看到：

```text
IR: [IR] Ready: GPIO7, active-low, 1us ticks, idle=30000us, 4 buffers x 2048 symbols
```

## 输出与比较

按遥控器按钮后，日志包含采集编号、接收完成时间（启动以来的微秒数）、symbol 数量，以及每个 symbol 的两个电平、两个持续时间和原始 32 位值。例如以下仅演示日志格式，不代表某个格力协议：

```text
IR: [IR] Signal received; capture=1
IR: [IR] Receive time (completion, since boot): 12345678us
IR: [IR] Symbols: 68
IR: Symbol[0]: level0=0 duration0=9000us level1=1 duration1=4500us raw=0x91942328
IR: Symbol[1]: level0=0 duration0=560us level1=1 duration1=560us raw=0x82300230
...
IR: [IR] Captured duration: ...us; end capture=1
```

RMT 时钟为 1 MHz，duration 数值直接对应微秒。保持原始极性：低电平通常表示红外载波包络，高电平通常表示空闲。记录完整驱动输出，零持续时间可能是结束标记，不应作为实际脉冲解析。

依次采集开机、关机、升温、降温。每次等待对应 `end capture` 后再按下一键，保存各采集的完整日志，比较 symbol 数量与电平持续时间。状态帧可能随温度、模式、风速等设置一起变化，不预设位含义或固定帧长度。

## 缓冲区与连续接收

- RMT 使用 96 个硬件 symbol（ESP32-S3 的两个 48-symbol 块），由原生 RX ping-pong 驱动持续搬运到应用缓冲区。因此 96 不是整帧容量。
- 四个内部 SRAM 固定缓冲区，每个容纳 2048 个 symbol（最多 4096 个电平持续时间），合计 32 KiB 波形存储；没有逐脉冲动态分配。
- ISR 只将完成事件的元数据副本与时间戳送入静态队列。驱动的 `edata` 指针不会保存到任务中使用。
- 采集任务先切换到空闲缓冲区并重新调用 `rmt_receive()`，再将完成缓冲区交给打印任务；打印结束才归还缓冲区。打印期间不会覆盖正在输出的数据。
- 打印任务优先级低于现有语音任务，采集任务只短暂运行以尽快重新启动接收。每次事务结束到任务重新启动 RX 之间仍有短暂硬件重启间隙，不承诺无间隙接收。
- 若串口长期跟不上输入，四个缓冲区用完后丢弃新完成的采集，并继续接收；输出 `UART backlog: dropped captures=...`。115200 波特率下逐项输出长帧需要时间，有限缓冲池无法保证无限连续输入不丢帧。
- 达到 2048-symbol 容量时输出 `Buffer capacity reached; capture may be truncated`。5.5.5 的完成回调不提供独立截断错误标志，因此满缓冲区不会被宣称为完整帧。可根据实测修改 `IR_RX_BUFFER_SYMBOLS`，同时核对内部 SRAM 余量。

## 接收边界、超时和错误

`IR_RX_IDLE_NS` 为 30 ms：任意电平持续超过该阈值会结束一次采集；它不是总报文时长限制。小于 30 ms 的报文内部间隔保留在波形中，大于该阈值的间隔会将报文分成多次采集。结尾空闲时间是硬件阈值边界，不能将它当作遥控器真实的完整帧间隔。当前阶段不将这些片段按未知协议拼接。

1 MHz 下 ESP32-S3 的单电平计数上限为 32767 us，不能直接把阈值调为 50 ms 或 100 ms。需要更长单电平测量范围时，应一起调整 RMT 分辨率、阈值和打印时的 tick→微秒换算。

毛刺滤波阈值为 1000 ns。此滤波器使用外设源时钟，与 symbol 的 1 MHz 时钟不同；不能照搬超过硬件范围的几十微秒滤波设置。

任务每秒检查完成队列。没有按键时等待超时是正常情况，保持当前 RX 事务运行，不报故障、不周期性重启接收。`rmt_receive()` 失败则在普通任务中报告错误、禁用/启用通道后重试；初始化失败返回错误并释放已创建的通道和任务。

该模块依据本地 ESP-IDF 5.5.5 的 `rmt_rx.h`、`rmt_types.h` 和 ESP32-S3 驱动实现核对 API。接口说明可参见[官方 RMT 文档](https://docs.espressif.com/projects/esp-idf/en/v5.5.5/esp32s3/api-reference/peripherals/rmt.html)。

## 硬件验收

默认启用 GPIO7 诊断，每五秒输出一行，所有打印都在普通任务中完成：

```text
[IR][DIAG] GPIO7=1 edges=0 (+0) rmt_done=0 empty=0 invalid=0 dropped=0
```

`GPIO7` 是日志打印时的瞬时电平，不是整个五秒窗口的电平；`edges` 是独立 GPIO 中断观察到的跳变累计数，括号是相对上次诊断的增量；`rmt_done` 是采集任务处理的 RMT 完成事件数；`empty` 是零 symbol 的完成次数。GPIO 中断只计数，不打印、不解析波形。它不会重新配置 RMT 引脚路由。

按键前后比较这些计数：

| 现象 | 排查方向 |
| --- | --- |
| 按键后 `edges` 始终为 0 | GPIO7 未观察到跳变；检查接线、接收头供电、遥控器和引脚位置 |
| 多次观察电平为 0，`edges` 不变 | 输入可能被拉低；核对接收头 OUT、VS、GND，不能仅凭一次瞬时读数断定故障 |
| `edges` 增长但 `rmt_done` 长时间不增长 | 引脚有活动，但 RMT 尚未完成；继续检查是否持续噪声、结束间隔或接收通道问题 |
| `rmt_done`、`empty` 同时增长 | RMT 回调工作，但没有有效 symbol；检查噪声、输入脉宽及接收配置 |
| `rmt_done` 增长并输出 `Symbol[...]` | 已捕获原始波形，可继续测试不同按键 |

该诊断仅用于定位输入与采集链路，不替代示波器或逻辑分析仪，不保证统计每个极短毛刺。确认接线和采集正常后，可通过 `idf.py menuconfig` → `Infrared receiver settings` 关闭 `Log GPIO7 edge counts and RMT receive status`，去掉额外 GPIO 中断和周期日志。GPIO 配置方式依据[ESP-IDF 5.5.5 GPIO 文档](https://docs.espressif.com/projects/esp-idf/en/v5.5.5/esp32s3/api-reference/peripherals/gpio.html)。

1. 确认启动出现 `[IR] Ready`，同时原语音初始化正常。
2. 对准接收头，分别按开机、关机、温度调整键，确认各次日志从 `Signal received` 到 `end capture` 完整输出。
3. 重复按不同键，检查采集编号递增、无需重启；正常手动采集时不应出现容量或丢帧提示。
4. 快速连按测试队列过载，确认出现明确丢帧计数而非数据覆盖或崩溃，放慢后继续采集。
5. 遥控器不发送信号时，确认没有重复报超时错误；恢复按键后仍能输出。
6. 本模块尚不解码，日志差异不能直接作为格力状态字段的结论。
