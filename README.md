# ESP32-S3 语音助手

基于 **ESP32-S3 / ESP-IDF v5.5.5** 的语音采集、文字交互客户端与格力空调红外实验项目。开发板使用 16 MB Flash、8 MB PSRAM；下文命令以 Windows PowerShell、`COM4`、115200 波特率为例，请按本机环境调整。

语音链路为：INMP441 采音 → ESP-SR 本地唤醒和 VAD → 腾讯云一句话识别 → 可选的 LLM HTTP 服务 → 串口输出真实返回的文本 → 可选的 HTTPS TTS 服务生成 WAV → MAX98357 I2S 放音。红外链路为：TSOP38238 接收 → 原生 RMT RX 捕获完整波形 → 离线日志解析与字段对比 → 已录制波形的 RMT TX 重放（发射模块尚未到货，未实机验证）。

| 功能 | 当前状态 |
| --- | --- |
| 语音采集、唤醒、VAD、云端 ASR 与 LLM 客户端 | 已实现，语音转写及 LLM HTTP 200 回复均有实机日志验证；服务器需自行部署并配置地址 |
| TTS 下载与 MAX98357 放音 | 实机日志已验证完整 WAV 下载与 I2S 写入完成；实际出声仍需确认功放 SD 使能及接线 |
| GPIO7 红外接收 | 已实机采集格力遥控器的开关、温度和风速报文，支持连续接收 |
| 红外日志离线分析 | 已实现，保存原始采集、操作标注、分析结果与录制目录 |
| 红外发射与空调状态接口 | 代码已编译验证，采用已录制波形重放；发射模块未到货，尚未实机验证 |
| 动态协议生成、语音联动空调 | 待实现；当前不会根据语音或 LLM 回复自动发送空调指令 |

本仓库包含设备端 TTS 客户端与扬声器播放代码，不包含 LLM 或 TTS 服务器。TTS 需另行部署、配置 HTTPS 地址和令牌；不配置时仍输出 AI 文本，但跳过语音播放。当前上电启动 Wi-Fi、红外接收和语音任务，**不会自动初始化红外发射或发送空调指令**。

## 硬件与接线

- 开发板：Waveshare ESP32-S3-DEV-KIT-N16R8-M（ESP32-S3、16 MB Flash、8 MB PSRAM）。
- 麦克风：INMP441，3.3 V 供电。**不能把 VDD 接 5 V；L/R 不能悬空。**
- 功放与喇叭：MAX98357 I2S 模块，当前接 3V3，喇叭两线接模块差分输出。
- 红外接收头：TSOP38238（38 kHz），VS 接 3V3，OUT 接 GPIO7。
- 红外发射模块：尚未购买，供电、触发极性与发射 GPIO 待根据实际模块确认。

### 当前 GPIO 分配

| ESP32-S3 GPIO | 外设信号 | 方向（相对 ESP32） | 配置位置 |
| --- | --- | --- | --- |
| GPIO4 | INMP441 SCK / BCLK | 输出 | `main/audio/microphone.h`：`MIC_GPIO_BCLK` |
| GPIO5 | INMP441 WS / LRCLK | 输出 | `main/audio/microphone.h`：`MIC_GPIO_WS` |
| GPIO6 | INMP441 SD / DOUT | 输入 | `main/audio/microphone.h`：`MIC_GPIO_DIN` |
| GPIO7 | TSOP38238 OUT | 输入，RMT RX | `main/ir/receiver/ir_receiver.c`：`IR_RX_GPIO` |
| GPIO15 | MAX98357 LRC / LRCLK / WS | 输出，I2S TX | `main/audio/speaker.h`：`SPEAKER_GPIO_WS` |
| GPIO16 | MAX98357 BCLK | 输出，I2S TX | `main/audio/speaker.h`：`SPEAKER_GPIO_BCLK` |
| GPIO17 | MAX98357 DIN | 输出，I2S TX | `main/audio/speaker.h`：`SPEAKER_GPIO_DOUT` |
| 待定 | 红外发射模块输入 | 输出，RMT TX | 初始化时传入 `ir_transmitter_config_t.gpio_num` |

**GPIO4/5/6 已用于麦克风，GPIO7 专用于红外接收，GPIO15/16/17 已用于喇叭。** 红外发射初始化接口会拒绝 GPIO4/5/6/7；选发射引脚时还须自行避开 GPIO15/16/17。表中的编号为 GPIO 编号，不是排针位置或物理脚号。

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

### MAX98357 与喇叭接线

| MAX98357 模块丝印 | ESP32-S3 / 喇叭 | 说明 |
| --- | --- | --- |
| VIN | 3V3 | 当前按 3.3 V 供电；先核对模块规格与板卡引脚 |
| GND | GND | 与 ESP32 共地 |
| LRC / LRCLK / WS | GPIO15 | 声道时钟；这些名称表示同一信号，只接一个对应引脚 |
| BCLK | GPIO16 | I2S 位时钟 |
| DIN | GPIO17 | I2S 输出数据送入功放 |
| SPK+ / OUT+ | 喇叭红线 | 功放差分输出正端 |
| SPK− / OUT− | 喇叭蓝线 | 功放差分输出负端，**不是 GND** |
| GAIN | 暂不接 | 使用模块默认增益；不要误当作 SD |
| SD | 播放测试时接 3V3 | SD 接地会关断功放；软件目前未通过 GPIO 控制使能 |

所有接线应在**断电后**操作。面包板红/蓝电源轨只是标记，需确认分别接到了 3V3/GND，并留意分段不连通或接触不良；不要把喇叭任一根线接面包板电源轨或 ESP32 GND。若蓝色电源轨为 GND，SD 接蓝轨会持续关断功放，即使日志显示播放完成也不会出声。使用当前 VIN=3V3 的 MAX98357A 模块测试时，将 SD 接同一个 3V3 电源轨以使能功放，勿将 SD、GAIN 混接。SD 未受程序控制，软件音量不能消除功放待机噪音。

可在 `menuconfig → Voice assistant settings` 启用 `Play two local speaker test tones at startup`（`SPEAKER_STARTUP_TEST`），设备将在启动语音识别前通过同一 I2S 播放路径播放两声 800 Hz 提示音，无需等待网络或 TTS。确认出声后可关闭此项。播放日志会显示 PCM 峰值和平均绝对幅度，辅助区分静音 WAV 与功放接线问题；I2S 写入成功不代表喇叭实际发声。

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
├─ audio/                     INMP441 I2S 采集、WAV 解析与 MAX98357 I2S 放音
├─ voice/                     WakeNet、VAD、录音及 ASR/LLM/TTS 状态机
├─ ir/
│  ├─ receiver/               GPIO7 原始红外接收
│  ├─ transmitter/            RMT TX、发射包络转换及原始重放
│  └─ gree/                   格力空调状态接口及已采集波形目录
└─ cloud/
   ├─ wifi/                   Wi-Fi STA 连接状态
   ├─ asr/                    腾讯云 ASR、SNTP 校时、TC3 签名
   ├─ llm/                    独立任务的 LLM HTTP 客户端
   └─ tts/                    HTTPS 合成请求、WAV 下载并调用扬声器播放
```

流程为 `WAIT_WAKE → WAKE_DETECTED → RECORDING → ASR_PROCESSING → LLM_PROCESSING → TTS_PROCESSING → WAIT_WAKE`；未配置 TTS、ASR/LLM 失败时会跳过相应后续步骤并恢复等待。唤醒、VAD 在本地运行；录音最长 15 秒，连续静音结束阈值默认 500 ms；ASR、LLM 与 TTS 均通过独立任务运行，不在 I2S 采集任务中执行 HTTP。TTS 完成下载及播放后才恢复唤醒。LLM 不保存聊天历史：会话 ID 在本次启动内维持，可接受服务端更新。

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
cd E:\esp-projects\voice_assistant
idf.py menuconfig
```

首次克隆后，本机需自行创建 `main/cloud/asr/tencent_credentials.h`（该文件被 Git 忽略）；文件内容格式如下，双引号内的值请在**本机**填写：

```c
#pragma once
#define TENCENT_SECRET_ID ""
#define TENCENT_SECRET_KEY ""
```

如果本机已有填好密钥的文件，**不要覆盖**。代码调用的是腾讯云**一句话识别**，密钥不会从环境变量读取。不要把凭据写入 README、Issue 或串口日志，也不要将包含密钥的固件发布到仓库。

在 `menuconfig → Voice assistant settings` 中填写可上网的 **2.4 GHz Wi-Fi** 名称及密码。需要 LLM 时，在 `LLM server base URL` 中填写实际部署的服务根地址，保存后生成 `sdkconfig` 中的 `CONFIG_LLM_SERVER_URL`。LLM 服务端若尚未部署，可保持该项为空：启动时提示 LLM 未启用，唤醒和 ASR 仍能运行，识别成功后输出 `[LLM] skipped` 并返回待唤醒；不会产生模拟回复。需要播报时，另填 `TTS server base URL`（HTTPS 根地址，**不加** `/tts` 或 `/health`）及 `TTS bearer token`（仅填令牌本身，**不加** `Bearer `）；两者任一为空时仅打印 AI 回复，并输出 `[TTS] skipped`。其他选项：

| 配置 | 默认值 | 说明 |
| --- | --- | --- |
| `VOICE_HTTP_TIMEOUT_MS` | 45000 | ASR 使用的 Wi-Fi 等待和 HTTP 超时 |
| `VOICE_END_SILENCE_MS` | 500 | 连续静音结束阈值（毫秒） |
| `LLM_SERVER_URL` | 空 | 服务根 URL，**不带** `/api/chat`；客户端会自动追加 |
| `LLM_REQUEST_TIMEOUT_MS` | 15000 | LLM HTTP 超时（毫秒） |
| `LLM_DEVICE_ID` | 空 | 空值按芯片 MAC 生成稳定设备 ID |
| `LLM_SESSION_ID` | 空 | 空值每次启动生成会话 ID |
| `LLM_MAX_RESPONSE_BYTES` | 4096 | 响应体最大字节数 |
| `TTS_SERVER_URL` | 空 | HTTPS 服务根 URL，客户端自动追加 `/tts`；不同于 LLM 地址 |
| `TTS_BEARER_TOKEN` | 空 | Bearer 认证令牌，保存在本机 `sdkconfig` 并编入固件 |
| `TTS_REQUEST_TIMEOUT_MS` | 150000 | TTS 请求超时（毫秒） |
| `SPEAKER_VOLUME_PERCENT` | 15 | 16-bit PCM 软件音量百分比，建议首次保持较低音量 |

`LLM_SERVER_URL` 推荐配置受信任证书的 HTTPS 地址；客户端用证书包验证服务端。`http://` 会明文传输语音文字；仅建议在可信局域网临时调试。若服务部署在公网，应使用 HTTPS、服务端鉴权与访问控制，不能把无认证的测试接口长期开放。配置的服务必须能被 ESP32 所连 Wi-Fi 访问；`localhost` 指 ESP32 自身，不能用于远程服务器。

## 编译、烧录与串口监视

```powershell
cd E:\esp-projects\voice_assistant
idf.py -p COM4 build flash monitor
```

根据实际串口修改 `COM4`；按 `Ctrl+]` 退出监视器。初次构建会生成 `build/` 与 `managed_components/`。更换模型配置后应重新执行完整的 `build flash`，不要只烧录旧应用映像。

红外接收独立于云端识别运行，不需要先等 Wi-Fi 或 ASR 就绪。启动后应看到 `IR: [IR] Ready: GPIO7, active-low`。遥控器对准接收头按键后，应持续输出 `[IR] Signal received`、`[IR] Symbols` 和每个 Symbol 的微秒时长；默认开启的 `[IR][DIAG]` 日志用于检查 GPIO7 边沿、接收完成和丢弃计数。具体输出与故障排查见 [红外接收说明](IR_RECEIVER.md)。

测试顺序：先确认 `Wi-Fi connected, IP=...` 和 `ASR network ready`，再说“嗨，乐鑫”观察 `[WAKE] detected`，接着说一句中文并停顿，观察 `[VAD] Speech started/ended` 与实际 `[ASR] result`；已配置且可访问 LLM 服务器时，还会显示 `llm: HTTP status`、`[LLM] response`、`[LLM] elapsed`。已配置 TTS 时，依次检查 `[TTS] synthesizing and playing...`、`speaker: WAV...`、`[TTS] playback complete`；这些日志出现前不能断言喇叭已播放。ASR/LLM/TTS 失败后均应返回 `[WAKE] Waiting for wake word...`。`HTTP 0` 表示尚未收到 HTTP 状态码；TTS 的 `getaddrinfo()` 错误表示域名尚未解析成功，先检查同一 Wi-Fi 下的 DNS 与临时隧道地址，而不是查喇叭。若看见 `AFE ringbuff_free_pct` 指标但仍正常唤醒和识别，不应单凭它判断音频丢帧；结合 `AFE feed failed/overflow` 等日志诊断。

## LLM 服务器接口约定

本仓库**只实现客户端**，不包含服务器。服务器应接收 `POST <LLM_SERVER_URL>/api/chat`，请求和成功响应都是 UTF-8 JSON：

```json
{"device_id":"esp32_001","session_id":"session_123","request_id":"req_001","text":"你好"}
```

```json
{"code":0,"session_id":"session_123","reply":"你好！","elapsed_ms":1250}
```

示例仅说明**协议格式**，不是固件预置的识别结果或回复。成功需 HTTP 2xx、数值型 `code` 为 `0`、非空 `session_id` 与 `reply`。设备会在下一轮使用服务端返回的 `session_id`；没有服务器端历史时，设备本身不会补建聊天上下文。请求 JSON 上限 2048 字节、识别文本缓冲区 512 字节、回复文本缓冲区 1024 字节；服务端应返回不超过配置上限的响应体。实际串口耗时由客户端测量。

## TTS 服务与 WAV 放音

设备在收到完整的 AI `reply` 后，以 `Authorization: Bearer <TTS_BEARER_TOKEN>` 和 `Content-Type: application/json; charset=utf-8` 请求 `POST <TTS_SERVER_URL>/tts`，发送 UTF-8 JSON `{"text":"AI 回复文本","speed":1.0}`。音频写入 PSRAM；响应包含 Content-Length 时并行播放，长度未知时完整下载后播放。单次 WAV 响应最大 3 MiB；仅支持 RIFF/WAVE 中 8–48 kHz 的 16-bit little-endian PCM 单/双声道，不支持 MP3 或压缩 WAV。单声道复制到两路 I2S 槽位，音量由 `SPEAKER_VOLUME_PERCENT` 调节，使用独立的 I2S1 控制器。

若服务返回的 WAV 格式不符、PSRAM 不足、域名不可解析或 HTTP 请求失败，串口会报告 `[TTS] failed`；并行播放时已经收到的部分音频可能已经播出，错误发生后停止播放。`trycloudflare.com` 一类临时域名可能随隧道重启变化：应确认运行中的服务地址并重新配置、构建、烧录。用户已确认启动提示音出声，后续日志验证了并行播放完成。

Wi-Fi 启动后关闭 modem sleep 以减少接收延迟（耗电相应增加）。当前 `sdkconfig.defaults` 将 TCP 接收窗口设为 65535 字节、接收队列设为 48；已有本机 `sdkconfig` 需同步这些配置，因为 defaults 不会覆盖已存在的配置。WAV 按 4 KiB 分块读取，首块和每隔 5 秒报告一次进度。下载读取等待上限为 15 秒；先缓冲 `TTS_PLAYBACK_BUFFER_MS`（默认 1500 ms）对应的 PCM 再播放，网络断供时输出静音并等待补缓冲。播放任务或并行播放内存分配失败时回退到完整下载后播放。

网络分配启用 `SPIRAM_TRY_ALLOCATE_WIFI_LWIP`，使可放入外部 RAM 的 Wi-Fi/LwIP 缓冲优先使用 PSRAM；Wi-Fi DMA TX 使用 8 个静态内部缓冲。下载日志记录内部 RAM 的空闲量、最大连续块和历史最低量，用于确认是否存在接收缓冲分配压力。IDF 5.5.5 的 HTTP 超时 setter 不更新已经连接的 TLS socket，因此在开始下载时同时设置底层 `SO_RCVTIMEO`，避免 TLS 收包继续沿用连接阶段的 150 秒超时；每次读取返回后另检查下载总耗时。

TTS 任务保持核心 1、优先级 4。提高其优先级未解决实机慢速，反而出现 AFE 喂入溢出，已撤回该调整。ASR、LLM、TTS 云端处理期间暂停 AFE 喂入和取数，持续读取并丢弃 I2S RX 数据，仅轮询云端结果；结束或失败后清空 AFE 缓冲并恢复唤醒。暂停时不调用会触发空缓冲警告的 `fetch_with_delay(..., 0)`，避免无用推理与 TLS 争抢 CPU。

下载日志显示分段接收速率、运行核心、RSSI、实际省电状态（0 表示关闭）、TCP 接收窗口、公网连接 IP 和 `CF-Ray` 节点标识。比较电脑与设备时应关闭电脑代理再测同一公网接口，并区分等待响应头的合成时间与响应正文的下载时间；电脑的 TUN/透明代理仍可能影响路由。配置优化是否改善实机速度仍须结合新的完整下载日志确认。

接收诊断通过 `esp_netif_tcpip_exec` 在 TCP/IP 线程内只读采集这条连接的当前接收窗口、待通告窗口、下一字节序号和乱序段队列，并在返回 TTS 线程后打印。`LWIP_STATS` 提供整个 TCP 栈的接收、丢弃、内存分配错误和校验错误计数，不能将这些计数直接等同于本次连接的丢包率或远端重传次数。这个诊断使用 IDF 5.5.5 的 lwIP 私有 PCB 结构，升级 SDK 时须重新检查兼容性。

实机诊断显示接收窗口未被占满、内存和校验错误为 0，但响应开始时乱序队列已达到原先 4 段上限。启用 `LWIP_TCP_SACK_OUT`，允许设备向支持 SACK 的远端报告已经收到的数据区间；将 `LWIP_TCP_OOSEQ_MAX_PBUFS` 增至 12，以减少缺失数据包之后的乱序数据被提前丢弃。`TCP receive` 日志中的 `SACK=yes` 表示这条连接已协商启用，而非仅配置开启。此调整针对缺包/乱序恢复能力，是否解决慢速仍须验证实际下载时间。

2026-10-13 的一次实机验证：`SACK=yes`，乱序队列曾达到 8–10 段；715564 字节 WAV 在 16815 ms 内完整下载，平均 42553 字节/秒。连接及合成等待约 9 秒，从 TTS 请求开始到进入播放共 25903 ms；返回音频为 32000 Hz、PCM16、单声道，已进入 `speaker` 播放阶段。该次日志未包含最终播放完成行，不能据此确认完整播放或实际听感。此前未启用 SACK、乱序缓冲上限为 4 时，多次出现下载停滞及超时；这次完整下载表明组合调整后的表现明显改善，仍需更多请求验证稳定性。

上述测试后，接收配置进一步同步到 PSRAM 模式：Wi-Fi 静态 RX 缓冲和 BA 窗口均设为 16（IDF 在此模式下的默认值），动态 RX 缓冲上限为 64。乱序 PBUF 上限改为 0（此 PSRAM 模式下的官方默认值，表示不额外限制乱序 PBUF 数量），配合有限的 TCP 接收窗口及 Wi-Fi 动态 RX 缓冲上限。相比原先保留的静态 RX=10、BA=6 和乱序上限=12，这允许缓存更大的接收突发并减少提前丢弃乱序数据；增大的静态 RX 缓冲会额外占用约 10 KiB 内部 RAM。新配置的提速幅度尚未实机验证，以后续 `Receive buffers` 和完整下载日志为准。

后续实机测量：完整下载 701484 字节用时 5287 ms（132671 字节/秒），1159724 字节用时 13172 ms（88041 字节/秒）。再改为并行播放后，一轮语音结束于 83720 ms，首次音频排入 I2S 于 93460 ms，总等待约 9.74 秒；完整下载结束于 98090 ms，提前约 4.63 秒开始播放。692524 字节下载用时 5450 ms，`rebuffer count=0`，播放完成并恢复唤醒。本轮未验证 TLS 会话恢复效果。

设备端进一步优化：CPU 默认 240 MHz，录音结束的静音阈值从 800 ms 调为 500 ms；较长的说话中途停顿可能提前结束录音，可通过 menuconfig 调回。认证 GET `/health` 完整消费响应后，同一连接用于 POST `/tts`；目前已进一步改为后台准备并跨轮保持连接，具体超时及恢复策略见下文。队列容量为 2，避免预热消息尚未取出时丢掉正式请求。正式请求在发送正文前遇到失效连接时仅重连一次，正文发送后不自动重发。

后续实机反馈出现 `mbedtls_ssl_setup returned -0x7F00`（内存分配失败）和空 TLS 会话错误，已撤回会话票据缓存。ASR、LLM 请求结束后清理客户端。TLS 使用 `MBEDTLS_EXTERNAL_MEM_ALLOC` 分配到 PSRAM，避免大块 TLS 收发缓冲占用内部 RAM；已有本机 sdkconfig 也已同步。启动日志 `TLS allocator: PSRAM; session ticket cache disabled` 可确认配置，后续两轮反馈没有再出现内存分配错误。

`Connection prewarm: ready` 表示健康请求成功且服务器支持持久连接；`prewarmed connection: yes` 表示尝试复用，失效重连单独记录。`Speech ended to first audio` 记录语音结束至首次 I2S 写入的总等待，`First audio queued` 从正式请求入队开始计时，包含等待预热的排队时间。240 MHz 增加运行功耗。服务端仍完整合成后返回 WAV，设备优化无法消除服务端合成等待。

主机测试使用真实 WAV 解析和扬声器实现、模拟 I2S 与下载进度，覆盖不完整 WAV 头、未完成下载时启动播放、断供补缓冲、下载错误、截断数据和单/双声道 PCM 转换；不替代实际硬件验证。

后续测试中第一轮完整播放成功，第二轮复用了预热连接，但 WAV 全部下载后校验失败。根因是 `/health` 的短 JSON 正文可在读取响应头时全部进入 SDK 缓存，`is_complete_data_received()` 只表示解析器收齐正文，不表示应用读取了缓存。预热现在无条件读取至 read 返回 0 且解析器完成，确保没有健康响应残留混入 WAV。新增主机回归测试覆盖已收齐但未消费的缓存、分块缓存、长度上限、截断和超时；WAV 首块明显无效时提前停止下载。修复尚待实机验证。

后续日志中 ASR 用时 759 ms 并返回有效文字，未再出现 TLS 内存分配错误；LLM 在建连阶段被重置，电脑关闭代理直连同一服务也无法连接，用户正在检查服务启动状态。LLM 仅在建连快速失败（小于 2 秒）、正文尚未发送且 Wi-Fi 仍连接时等待 150 ms 后重试一次，不重试慢速超时或已经发送正文的请求。TTS 的 `Prewarm detail` 补充预热失败阶段、错误码、HTTP 状态与持久连接支持情况，不能把本次预热失败直接认定为 TLS 内存问题。

最新连接稳定性调整（待实机验证）：联网并完成时钟同步后，TTS 任务在空闲期间主动准备一条公网 HTTPS 连接。完整下载且服务器支持持久连接时不再关闭 TLS，跨轮复用；每 30 秒用认证 `/health` 检查连接，完整消费健康响应后继续保留。每秒非阻塞探测 TCP 的关闭状态，不读取 TLS 数据；Wi-Fi 掉线或请求失败时清理连接，后台重连采用 3、6、12、24、30 秒上限的退避。冷建连超时单独设为 10 秒，健康响应预算为 4 秒，合成响应仍使用配置的 TTS 超时。正式请求在正文发送前遇到失效连接时只重连一次，正文发送后不自动重发。

`Persistent HTTPS ready` 表示后台准备成功，`Persistent HTTPS retained for next turn` 表示播放结束后保留连接；`TTS connection: reused=...; open=... ms; queue wait=... ms` 分别记录是否尝试复用、建连及发送请求头耗时、请求开始前的排队与准备耗时。云端仍可能关闭空闲连接，启动第一轮或断网恢复期间仍有冷建连等待，不能保证所有请求均为毫秒级。保持 TLS 会持续占用 PSRAM，需用连续对话和空闲后对话验证 ASR、TTS 与播放资源能共存。新增主机测试验证心跳时序、退避上限和成功恢复后重置退避。

电脑关闭代理直连公网 `/health` 的连续两次测试均返回 HTTP 200，curl 的 `num_connects` 分别为 1、0（总耗时约 1.26 秒、0.48 秒），验证服务支持连接复用；此结果不是 ESP32 时延或长时间稳定性的测量。

## 凭据与仓库安全

`.gitignore` 排除本机 `sdkconfig`、`main/cloud/asr/tencent_credentials.h`、构建目录和下载的组件；公开仓库保留无密钥的 `sdkconfig.defaults`，不提交本机凭据。TTS Bearer token 也存入本机 `sdkconfig` 并编入固件，**不要提交编译后的 `.bin`/`.elf`、完整构建目录或含凭据的工程压缩包**：密钥和令牌仍可能被提取。若密钥或令牌曾曝光，应在相应服务端轮换；腾讯云账号权限应限定为所需的 ASR 操作。
