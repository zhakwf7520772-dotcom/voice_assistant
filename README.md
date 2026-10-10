# ESP32-S3 语音助手

基于 ESP-IDF 的语音采集与文字交互客户端：INMP441 采音 → ESP-SR 本地唤醒和 VAD → 腾讯云一句话识别 → 可选的 LLM HTTP 服务 → 串口输出文本。**本仓库不含 LLM 服务器或模型推理代码，也不提供 TTS、扬声器播放和设备控制。**

## 硬件与接线

- 开发板：Waveshare ESP32-S3-DEV-KIT-N16R8-M（ESP32-S3、16 MB Flash、8 MB PSRAM）。
- 麦克风：INMP441，3.3 V 供电。**不能把 VDD 接 5 V；L/R 不能悬空。**

| INMP441 | ESP32-S3 | 作用 |
| --- | --- | --- |
| VDD | 3V3 | 供电 |
| GND | GND | 共地 |
| SCK / BCLK | GPIO4 | 位时钟 |
| WS / LRCLK | GPIO5 | 声道时钟 |
| SD / DOUT | GPIO6 | 音频输入 |
| L/R | GND | 选择左声道 |

GPIO4/5/6 与当前程序一致；接线前仍应对照**手中板卡版本的引脚图和原理图**，确认引出和板载占用，不要按排针序号猜 GPIO。需要换脚时修改 `main/audio/microphone.h`。I2S 采用 16 kHz、32-bit 接收槽位，取高 16 位有符号 PCM 供 ESP-SR 使用。

## 软件依赖与工程结构

- ESP-IDF **v5.5.5**，目标 `esp32s3`；组件声明见 `main/idf_component.yml`。
- ESP-SR **2.6.0**；本地模型为 `wn9_hilexin`（唤醒词“嗨，乐鑫”）和 `vadnet1_medium`。组件管理器在首次构建时可能需要联网下载依赖。
- `sdkconfig.defaults` 设置 16 MB Flash、Octal PSRAM、自定义分区和模型；`partitions.csv` 包含应用与 `model` 分区。`flash` 会按 ESP-SR 组件的构建规则打包、烧录所选模型。

```text
main/
├─ main.c                     启动入口
├─ audio/                     INMP441 I2S 采集
├─ voice/                     WakeNet、VAD、录音及状态机
└─ cloud/
   ├─ wifi/                   Wi-Fi STA 连接状态
   ├─ asr/                    腾讯云 ASR、SNTP 校时、TC3 签名
   └─ llm/                    独立任务的 LLM HTTP 客户端
```

流程为 `WAIT_WAKE → WAKE_DETECTED → RECORDING → ASR_PROCESSING → LLM_PROCESSING → WAIT_WAKE`。唤醒、VAD 在本地运行；录音最长 15 秒，连续静音结束阈值默认 800 ms；ASR 与 LLM 均通过独立任务运行，不在 I2S 采集任务中执行 HTTP。LLM 不保存聊天历史：会话 ID 在本次启动内维持，可接受服务端更新。

## 首次配置（Windows / PowerShell）

在**已激活 ESP-IDF v5.5.5** 的终端中：

```powershell
cd E:\esp-projects\voice_assistant
Copy-Item .\main\cloud\asr\tencent_credentials.h.example .\main\cloud\asr\tencent_credentials.h
idf.py menuconfig
```

如果本机已有填好密钥的 `tencent_credentials.h`，**不要执行覆盖用的 `Copy-Item`**。在本机编辑这个头文件，填入自己的腾讯云 SecretId / SecretKey；代码调用的是腾讯云**一句话识别**，密钥不会从环境变量读取。不要把凭据写入 README、Issue 或串口日志。

在 `menuconfig → Voice assistant settings` 中填写可上网的 **2.4 GHz Wi-Fi** 名称及密码。LLM 服务端若尚未部署，可保持 `LLM server base URL` 为空：唤醒和 ASR 仍能运行，LLM 阶段会报告配置错误并返回待唤醒；不会产生模拟回复。其他选项：

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
cd E:\esp-projects\voice_assistant
idf.py -p COM4 build flash monitor
```

根据实际串口修改 `COM4`；按 `Ctrl+]` 退出监视器。初次构建会生成 `build/` 与 `managed_components/`。更换模型配置后应重新执行完整的 `build flash`，不要只烧录旧应用映像。

测试顺序：说“嗨，乐鑫”观察 `[WAKE] detected`，再说一句中文并停顿，观察 `[VAD] Speech started/ended` 与实际 `[ASR] result`；已配置且可访问 LLM 服务器时，还会显示 `[LLM] HTTP status`、`[LLM] response`、`[LLM] elapsed`。断网、ASR 错误、LLM 连接失败或空 URL 均应报告错误并返回 `[WAKE] Waiting for wake word...`。若看见 `AFE ringbuff_free_pct` 指标但仍正常唤醒和识别，不应单凭它判断音频丢帧；结合 `AFE feed failed/overflow` 等日志诊断。

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

`.gitignore` 排除本机 `sdkconfig`、`main/cloud/asr/tencent_credentials.h`、构建目录和下载的组件；公开仓库只保留无密钥的 `sdkconfig.defaults` 与 `tencent_credentials.h.example`。**不要提交编译后的 `.bin`/`.elf`、完整构建目录或含凭据的工程压缩包**：密钥会编入固件，仍可能被提取。若密钥曾曝光，请在腾讯云控制台轮换。使用 SecretId/SecretKey 的账号权限应限定为所需的 ASR 操作。
