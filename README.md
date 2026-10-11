# ESP32-S3 语音助手

基于 **ESP32-S3 / ESP-IDF v5.5.5** 的语音助手与格力空调红外实验项目。开发板使用 16 MB Flash、8 MB PSRAM，开发环境为 Windows + VS Code，当前串口为 COM4、波特率为 115200。

语音链路为：INMP441 采音 → ESP-SR 本地唤醒和 VAD → 腾讯云一句话识别 → 可选的 LLM HTTP 服务 → 串口输出文本。红外链路为：TSOP38238 接收 → 原生 RMT RX 捕获完整波形 → 离线日志解析与字段对比 → 已录制波形的 RMT TX 重放。

| 功能 | 当前状态 |
| --- | --- |
| 语音采集、唤醒、VAD、云端 ASR | 已实现；LLM 需要另行部署服务并配置地址 |
| GPIO7 红外接收 | 已实机采集格力遥控器的开关、温度和风速报文，支持连续接收 |
| 红外日志离线分析 | 已实现，保存原始采集、操作标注、分析结果与录制目录 |
| 红外发射与空调状态接口 | 代码已编译验证，采用已录制波形重放；发射模块未到货，尚未实机验证 |
| 动态协议生成、语音联动空调 | 待实现；当前不会根据语音或 LLM 回复自动发送空调指令 |

本仓库包含设备端代码，不包含 LLM 服务器、TTS 或扬声器播放。当前上电启动 Wi-Fi、红外接收和语音任务，**不会自动初始化红外发射或发送空调指令**。

## 硬件与接线

- 开发板：Waveshare ESP32-S3-DEV-KIT-N16R8-M（ESP32-S3、16 MB Flash、8 MB PSRAM）。
- 麦克风：INMP441，3.3 V 供电。**不能把 VDD 接 5 V；L/R 不能悬空。**
- 红外接收头：TSOP38238（38 kHz），VS 接 3V3，OUT 接 GPIO7。
- 红外发射模块：尚未购买，供电、触发极性与发射 GPIO 待根据实际模块确认。

### 当前 GPIO 分配

| ESP32-S3 GPIO | 外设信号 | 方向（相对 ESP32） | 配置位置 |
| --- | --- | --- | --- |
| GPIO4 | INMP441 SCK / BCLK | 输出 | `main/audio/microphone.h`：`MIC_GPIO_BCLK` |
| GPIO5 | INMP441 WS / LRCLK | 输出 | `main/audio/microphone.h`：`MIC_GPIO_WS` |
| GPIO6 | INMP441 SD / DOUT | 输入 | `main/audio/microphone.h`：`MIC_GPIO_DIN` |
| GPIO7 | TSOP38238 OUT | 输入，RMT RX | `main/ir/receiver/ir_receiver.c`：`IR_RX_GPIO` |
| 待定 | 红外发射模块输入 | 输出，RMT TX | 初始化时传入 `ir_transmitter_config_t.gpio_num` |

**GPIO4/5/6 已用于麦克风，GPIO7 专用于红外接收，不能用作红外发射。** 发射初始化接口会拒绝这四个引脚。表中的编号为 GPIO 编号，不是排针位置或物理脚号。

### INMP441 接线

| INMP441 | ESP32-S3 | 作用 |
| --- | --- | --- |
| VDD | 3V3 | 供电 |
| GND | GND | 共地 |
| SCK / BCLK | GPIO4 | 位时钟 |
| WS / LRCLK | GPIO5 | 声道时钟 |
| SD / DOUT | GPIO6 | 音频输入 |
| L/R | GND | 选择左声道 |

GPIO4/5/6 与当前程序一致；接线前仍应对照**手中板卡版本的引脚图和原理图**，确认引出和板载占用，不要按排针序号猜 GPIO。需要换脚时修改 `main/audio/microphone.h`。I2S 采用 16 kHz、32-bit 接收槽位，取高 16 位有符号 PCM 供 ESP-SR 使用。

### TSOP38238 接线

| TSOP38238 | ESP32-S3 | 作用 |
| --- | --- | --- |
| VS | 3V3 | 供电 |
| GND | GND | 共地 |
| OUT | GPIO7 | 解调后的数字红外输入 |

按实际器件或模块标注确认 VS/GND/OUT，不要根据外壳朝向猜引脚顺序。接收到载波时 OUT 通常为低电平，空闲时为高电平；接收头已经完成载波解调，软件只采集高低电平持续时间。

后续发射模块须与 ESP32 共地，供电电压、3.3 V 输入兼容性及高/低电平触发方式由具体型号决定。发射 GPIO 确认后应同步更新本表；当前没有预留一个已经确定的发射引脚。

## 软件依赖与工程结构

- ESP-IDF **v5.5.5**，目标 `esp32s3`；组件声明见 `main/idf_component.yml`。
- ESP-SR **2.6.0**；本地模型为 `wn9_hilexin`（唤醒词“嗨，乐鑫”）和 `vadnet1_medium`。组件管理器在首次构建时可能需要联网下载依赖。
- `sdkconfig.defaults` 设置 16 MB Flash、Octal PSRAM、自定义分区和模型；`partitions.csv` 包含应用与 `model` 分区。`flash` 会按 ESP-SR 组件的构建规则打包、烧录所选模型。

```text
项目根目录/
├─ README.md                  项目概述、接线与运行说明
├─ IR_RECEIVER.md             原始红外采集说明
├─ IR_PROTOCOL_ANALYSIS.md    协议分析结论与重放准备
├─ tools/ir_log_analyzer.py   离线日志解析、比较及目录导出
├─ data/ir/                   采集日志、操作标注与分析结果
├─ tests/                     离线解析与波形转换测试
└─ main/                     固件源码（展开如下）

main/
├─ main.c                     启动入口
├─ audio/                     INMP441 I2S 采集
├─ voice/                     WakeNet、VAD、录音及状态机
├─ ir/
│  ├─ receiver/               GPIO7 原始红外接收
│  ├─ transmitter/            RMT TX、发射包络转换及原始重放
│  └─ gree/                   格力空调状态接口及已采集波形目录
└─ cloud/
   ├─ wifi/                   Wi-Fi STA 连接状态
   ├─ asr/                    腾讯云 ASR、SNTP 校时、TC3 签名
   └─ llm/                    独立任务的 LLM HTTP 客户端
```

流程为 `WAIT_WAKE → WAKE_DETECTED → RECORDING → ASR_PROCESSING → LLM_PROCESSING → WAIT_WAKE`。唤醒、VAD 在本地运行；录音最长 15 秒，连续静音结束阈值默认 800 ms；ASR 与 LLM 均通过独立任务运行，不在 I2S 采集任务中执行 HTTP。LLM 不保存聊天历史：会话 ID 在本次启动内维持，可接受服务端更新。

程序上电后立即启动 Wi-Fi 连接，不阻塞本地语音初始化。ASR 后台任务提前等待联网并完成 SNTP 校时，成功后输出 `ASR network ready`；预热失败时，后续识别请求会再次尝试联网和校时。如果在网络准备完成前讲话，第一次请求仍可能需要等待。

## 红外功能与当前支持范围

接收模块使用 ESP-IDF 5.5.5 新版 RMT RX API，1 MHz 分辨率（每 tick 为 1 μs），四个静态接收缓冲区，每个容量 2048 个 Symbol。ISR 只通知任务，普通任务处理和打印采集编号、接收完成时间、Symbol 数量、每个 Symbol 的电平、持续时间及原始值。接收任务先重新启动下一次接收，再交给日志任务打印；串口处理不过来时会报告丢弃计数。

离线工具按实际引导和长间隔拆分数据，不把每个 Symbol 都当成数据位。已观测到的遥控器指令通常包含两次约 70 Symbol 的 Capture；两次内容不同，属于同一操作的完整报文。温度、开关及风速候选字段和校验规律见 [协议分析说明](IR_PROTOCOL_ANALYSIS.md)，这些结论不代表所有格力遥控器型号或所有状态均已验证。

当前 `main/ir/gree/` 的状态接口只查找并重放已采集的九种状态，均为制冷模式：

| 开关状态 | 温度 | 风速 |
| --- | --- | --- |
| 关机 | 25℃ | 1档 |
| 开机 | 25℃ | 自动、1～5档（六种状态） |
| 开机 | 26℃、27℃ | 1档（两种状态） |

尚未录制的组合（例如 26℃、5档）返回 `ESP_ERR_NOT_FOUND`，不能将单独验证过的温度和风速任意组合。发射模块到货后，需要选定 GPIO、确认触发极性，调用 `gree_ac_init()` 和发送接口，并通过接收头回读及空调实际响应验证。当前没有串口发送菜单，也没有提前启用硬件发射。

重新分析仓库内的采集数据：

```powershell
py tools/ir_log_analyzer.py data/ir/captures.log --annotations data/ir/annotations.json --out data/ir/analysis
```

## 首次配置（Windows / PowerShell）

在**已激活 ESP-IDF v5.5.5** 的终端中：

```powershell
cd G:\esp-projects\voice_assistant
idf.py menuconfig
```

腾讯云配置位于本机的 `main/cloud/asr/tencent_credentials.h`，该文件被 Git 忽略，当前仓库没有 `.example` 模板。构建前确认本机配置文件存在，并配置自己的腾讯云 SecretId / SecretKey；已有配置不要覆盖。代码调用的是腾讯云**一句话识别**，密钥不会从环境变量读取。不要把凭据写入 README、Issue 或串口日志。

在 `menuconfig → Voice assistant settings` 中填写可上网的 **2.4 GHz Wi-Fi** 名称及密码。需要 LLM 时，在同一菜单的 `LLM server base URL` 中填写实际部署的服务根地址，保存后生成 `sdkconfig` 中的 `CONFIG_LLM_SERVER_URL`。LLM 服务端若尚未部署，可保持该项为空：启动时提示 LLM 未启用，唤醒和 ASR 仍能运行，识别成功后输出 `[LLM] skipped` 并返回待唤醒；不会产生模拟回复。其他选项：

| 配置 | 默认值 | 说明 |
| --- | --- | --- |
| `VOICE_HTTP_TIMEOUT_MS` | 45000 | ASR 使用的 Wi-Fi 等待和 HTTP 超时 |
| `VOICE_END_SILENCE_MS` | 800 | 连续静音结束阈值（毫秒） |
| `LLM_SERVER_URL` | 空 | 服务根 URL，**不带** `/api/chat`；客户端会自动追加 |
| `LLM_REQUEST_TIMEOUT_MS` | 15000 | LLM HTTP 超时（毫秒） |
| `LLM_DEVICE_ID` | 空 | 空值按芯片 MAC 生成稳定设备 ID |
| `LLM_SESSION_ID` | 空 | 空值每次启动生成会话 ID |
| `LLM_MAX_RESPONSE_BYTES` | 4096 | 响应体最大字节数 |

`LLM_SERVER_URL` 推荐配置受信任证书的 HTTPS 地址；客户端用证书包验证服务端。`http://` 仅限**可信局域网**调试，因为语音文本会明文传输。配置的服务必须能被 ESP32 所连 Wi-Fi 访问；不能直接把未知服务器地址或 `localhost` 当成可达地址。

## 编译、烧录与串口监视

```powershell
cd G:\esp-projects\voice_assistant
idf.py build
idf.py -p COM4 -b 115200 flash monitor
```

根据实际串口修改 `COM4`；按 `Ctrl+]` 退出监视器。初次构建会生成 `build/` 与 `managed_components/`。更换模型配置后应重新执行完整的 `build flash`，不要只烧录旧应用映像。

红外接收独立于云端识别运行，不需要先等 Wi-Fi 或 ASR 就绪。启动后应看到 `IR: [IR] Ready: GPIO7, active-low`。遥控器对准接收头按键后，应持续输出 `[IR] Signal received`、`[IR] Symbols` 和每个 Symbol 的微秒时长；默认开启的 `[IR][DIAG]` 日志用于检查 GPIO7 边沿、接收完成和丢弃计数。具体输出与故障排查见 [红外接收说明](IR_RECEIVER.md)。

测试顺序：先确认 `Wi-Fi connected, IP=...` 和 `ASR network ready`，再说“嗨，乐鑫”观察 `[WAKE] detected`，接着说一句中文并停顿，观察 `[VAD] Speech started/ended` 与实际 `[ASR] result`；已配置且可访问 LLM 服务器时，还会显示 `llm: HTTP status`、`[LLM] response`、`[LLM] elapsed`。断网、ASR 错误或 LLM 连接失败均应报告错误并返回 `[WAKE] Waiting for wake word...`，空 URL 则提示跳过 LLM。`HTTP 0` 表示尚未获得 HTTP 状态码；检查此前的 LLM 初始化或网络错误日志。若看见 `AFE ringbuff_free_pct` 指标但仍正常唤醒和识别，不应单凭它判断音频丢帧；结合 `AFE feed failed/overflow` 等日志诊断。

## LLM 服务器接口约定

本仓库**只实现客户端**，不包含服务器。服务器应接收 `POST <LLM_SERVER_URL>/api/chat`，请求和成功响应都是 UTF-8 JSON：

```json
{"device_id":"esp32_001","session_id":"session_123","request_id":"req_001","text":"你好"}
```

```json
{"code":0,"session_id":"session_123","reply":"你好！","elapsed_ms":1250}
```

示例仅说明**协议格式**，不是固件预置的识别结果或回复。成功需 HTTP 2xx、数值型 `code` 为 `0`、非空 `session_id` 与 `reply`。设备会在下一轮使用服务端返回的 `session_id`；没有服务器端历史时，设备本身不会补建聊天上下文。请求 JSON 上限 2048 字节、识别文本缓冲区 512 字节、回复文本缓冲区 1024 字节；服务端应返回不超过配置上限的响应体。实际串口耗时由客户端测量。

## 凭据与仓库安全

`.gitignore` 排除本机 `sdkconfig`、`main/cloud/asr/tencent_credentials.h`、构建目录和下载的组件；公开仓库保留无密钥的 `sdkconfig.defaults`，不提交本机凭据。**不要提交编译后的 `.bin`/`.elf`、完整构建目录或含凭据的工程压缩包**：密钥会编入固件，仍可能被提取。若密钥曾曝光，请在腾讯云控制台轮换。使用 SecretId/SecretKey 的账号权限应限定为所需的 ASR 操作。
