# 红外日志解析、字段验证与重放准备

## 当前结论

这次附件包含三个拼接的会话，Capture 编号有重置，不能将全文件直接按编号配对。保存的 `data/ir/captures.log` 只保留红外日志，未复制 Wi-Fi、网络地址或凭据信息。

| 会话 | 捕获数 | 内容 |
| --- | ---: | --- |
| 1 | 17 | 25℃一挡风开关实验，以及额外短采集；Capture 16 未出现在文本中 |
| 2 | 28 | 25℃开机，26→27→26→25 循环三轮，25℃关机 |
| 3 | 28 | 25℃一挡风启动，2→3→4→5→自动→1 循环两轮，以及最后一组额外采集 |

会话3的风速操作说明只有13次，日志中却有14组。最后 Capture 27/28 的载荷与已有25℃、一挡风关机样本匹配，但用户没有为这次操作明确标注，分析工具保留其为未标注，不拿推断结果充当验证标签。

共73个 Capture，其中70个符合完整的观测帧结构；另3个是25、2、1 symbol 的短采集，不能用于生成重放样本。第一会话的编号缺口也单独报告。因此“全文件都完整”不成立，最新风速会话自身的28个 Capture 则都是70个 symbol。

对33次有独立操作标注且具有完整两段波形的操作进行字段核对，没有标注冲突。同一已标注状态的16字节载荷一致；原始微秒脉宽仍会变化。

## 每个 Capture 的结构

解析工具先从数据推断短、长高电平时间簇，再区分引导、数据、长间隔和结束标记。没有把70个 symbol 全部当成数据位。

```text
Symbol[0]        引导：约9 ms低 + 4.5 ms高
Symbol[1..32]   32个数据位，组成4字节，字节内低位先发
Symbol[33..35]  三位分隔序列 010
Symbol[36]      低脉冲 + 约20 ms高电平间隔
Symbol[37..68]  第二个32位数据块，组成另外4字节
Symbol[69]      最后低脉冲 + duration1=0的结束边界
```

一个 Capture 是8字节数据，而一次已观测按键包含两个不同的 Capture，共16字节，中间约40 ms空闲。工具从接收完成时间及下一段波形持续时间计算间隔，未使用串口打印时间。估计公式：

```text
gap = next.completed_us - previous.completed_us - next.waveform_duration_us
```

该估计要求两段使用相同的RMT空闲阈值；仍可能包含ISR完成延迟差，不能声称是示波器级的绝对测量。分组默认将80 ms以内、同会话且编号连续的 Capture 归在一起，长按、多次迅速按键或不同协议时需检查分组结果。

## 风速字段核对

第一段所谓“第4～6位”包含 **开关位 + 两个基础风速位**。开关是第4位，基础风速是第5、6位。基础风速值为 `min(完整风速, 3)`，所以3、4、5挡均为3。

第二段完整风速字段的位置是：

- 第二次 Capture 的内部第二个32位块，第21～23位；
- 等价于该 Capture 的64位有效载荷，第53～55位；
- 等价于该 Capture 的 RMT `Symbol[57..59]`；
- 等价于该 Capture 的字节6，bit4～bit6（字节、bit均从0开始）；
- 等价于一次操作16字节整体中的字节14，bit4～bit6。

**第二次 Capture 的第一个32位块中的第21～23位不是风速字段，在这些样本中一直是010。**

| 风速 | 第一段第4～6位，发送顺序 | 第二段第二块第21～23位，发送顺序 | 完整风速值 |
| --- | --- | --- | ---: |
| 自动 | 100 | 000 | 0 |
| 1挡 | 110 | 100 | 1 |
| 2挡 | 101 | 010 | 2 |
| 3挡 | 111 | 110 | 3 |
| 4挡 | 111 | 001 | 4 |
| 5挡 | 111 | 101 | 5 |

25℃、制冷、开机时的完整字节样本如下，均由日志推导而非预填模板：

| 风速 | 第一 Capture | 第二 Capture |
| --- | --- | --- |
| 自动 | `09 09 20 50 00 40 00 00` | `09 09 20 70 00 00 00 C0` |
| 1挡 | `19 09 20 50 00 40 00 00` | `19 09 20 70 00 00 10 D0` |
| 2挡 | `29 09 20 50 00 40 00 00` | `29 09 20 70 00 00 20 E0` |
| 3挡 | `39 09 20 50 00 40 00 00` | `39 09 20 70 00 00 30 F0` |
| 4挡 | `39 09 20 50 00 40 00 00` | `39 09 20 70 00 00 40 00` |
| 5挡 | `39 09 20 50 00 40 00 00` | `39 09 20 70 00 00 50 10` |

## 温度、开关与校验

- 温度候选：两次 Capture 都是字节1低四位，分别为 `09、0A、0B`，对应25、26、27℃，符合 `温度−16`。未覆盖完整温度范围。
- 开关候选：两次 Capture 都是字节0的bit3，与风速bit4/bit5独立。第一段从 `19` 到 `11` 是bit3清零。
- 模式相关低三位一直为1，实验模式固定为制冷，不能据此验证其他模式。
- 某些字节及位在此数据集内固定，自动报告给出按两段区分的固定掩码；固定掩码不代表这些位在其他模式下没有作用。

每个8字节块的高四位校验候选：

```text
C = (10 + Σ(B[0..3] & 0x0F) + Σ(B[4..6] >> 4)) & 0x0F
验证 C == B[7] >> 4
```

该公式在70个符合完整观测结构的 Capture 中全部通过；在此公式形式内穷举0～15的初值，仅10匹配所有样本。每次按键有两个分别校验的块。

最后字节的低四位在所有这些样本中为0，其用途及是否在其他变体中参与校验不能靠本次数据证明。完整模式、定时、摆风、灯光和未采集的温度/风速组合仍未验证。

独立提取数据后，与[IRremoteESP8266 的 Kelvinator 实现](https://github.com/crankyoldgit/IRremoteESP8266/blob/master/src/ir_Kelvinator.cpp)及[字段结构](https://github.com/crankyoldgit/IRremoteESP8266/blob/master/src/ir_Kelvinator.h)交叉核对，结构、五挡扩展风速和块校验相符。该实现也列出了部分格力遥控器。这里据此标为 **Kelvinator类协议候选**，没有断言你的遥控器具体型号。

## 离线工具

在项目根目录执行，使用Python标准库，不需要额外安装依赖：

```powershell
py tools/ir_log_analyzer.py data/ir/captures.log --annotations data/ir/annotations.json --out data/ir/analysis
```

输出：

- `data/ir/analysis.md`：全部操作、完整位串、十六进制、变化位、固定掩码、异常及校验结果。
- `data/ir/analysis.json`：原始symbol、结构、位串、字节和分组，可供后续工具读取。
- `data/ir/annotations.json`：独立人工操作标签，与解析出的预测状态分开保存。

解析其他日志可以不传标签文件，先检查分组再添加实际操作标签：

```powershell
py tools/ir_log_analyzer.py your_capture.log --out build/your_analysis
```

重新生成已采集状态的C重放数据：

```powershell
py tools/ir_log_analyzer.py data/ir/captures.log --annotations data/ir/annotations.json --out data/ir/analysis --export-c main/ir/gree
```

只有两段完整、字段与人工标签相符、校验通过且同状态载荷一致的已标注操作进入重放目录。程序拒绝根据不完整数据或字段冲突生成目录。

## 模块组织与重放接口

```text
main/ir/
  receiver/
    ir_receiver.c/.h       原始接收
  transmitter/
    ir_waveform.c/.h       原始接收波形→发射包络的转换
    ir_transmitter.c/.h    RMT TX、载波、重放与超时恢复
  gree/
    gree_raw_catalog.c/.h  从日志生成的9种已采集状态波形
    gree_ac.c/.h           空调状态设置及已采集组合选择
```

`receiver` 不依赖发射或空调状态模块；`transmitter` 提供通用波形重放，不依赖格力字段；`gree` 通过发射接口重放已采集的空调状态。离线日志解析位于项目根目录的 `tools/`，实验数据和报告位于 `data/ir/`。

当前目录支持 **制冷**：25℃开机风速自动/1～5挡，26℃与27℃开机一挡风，25℃关机一挡风，共9个组合。它按精确状态选择录制波形，不生成未实测组合；例如26℃五挡风返回 `ESP_ERR_NOT_FOUND`。温度设置接口当前仅接受25～27℃，其他模式返回 `ESP_ERR_NOT_SUPPORTED`。

发射硬件尚未准备，`main.c` 不初始化或启动TX。未来由调用方确认模块、GPIO及有效电平后，在普通任务中使用：

```c
#include "gree_ac.h"

// emitter_gpio由实际接线决定，没有默认发射引脚。
const ir_transmitter_config_t config = {
    .gpio_num = emitter_gpio,
    .carrier_hz = 38000,
    .duty_cycle = 0.33f,
    .active_low = false,
};
ESP_ERROR_CHECK(gree_ac_init(&config));
ESP_ERROR_CHECK(gree_ac_set_power(true));
ESP_ERROR_CHECK(gree_ac_set_mode(GREE_COOL));
ESP_ERROR_CHECK(gree_ac_set_temp(25));
ESP_ERROR_CHECK(gree_ac_set_fan(3));
ESP_ERROR_CHECK(gree_ac_send()); // 选择25℃、制冷、三挡风的完整录制波形。
```

GPIO7及当前麦克风GPIO4/5/6被TX初始化接口明确拒绝。新的GPIO还需核对板卡占用。38 kHz为未来实测的起始载波设置，接收日志不能测得遥控器原始载波频率。

重放把RX低电平转换为逻辑载波开启、高电平转换为关闭；模块有效电平通过输出反相配置选择。完整两段及保存的间隔放在 **一个RMT TX事务** 中，超过32767 μs的间隔拆成相同电平的连续段，避免两次软件启动带来的间隔误差。保留全部非零持续时间，剔除RX结束标记；如果总半symbol数量为奇数，末尾补1 μs关闭段。

使用本地ESP-IDF 5.5.5的 `rmt_new_tx_channel()`、`rmt_new_copy_encoder()`、`rmt_apply_carrier()`、`rmt_transmit()`、`rmt_tx_wait_all_done()`。载荷转换缓冲区仅在显式TX初始化时分配一次内部SRAM；事务完成或取消前不复用该缓冲区。超时取消失败则保留内存并将模块置为故障状态，避免仍被硬件读取的内存遭到覆盖或释放。

多个调用者应使用 `gree_ac_send_state()` 一次提交完整状态。多次setter加send适合单一任务调用，跨任务交错setter可能组合出非预期目标状态。实际发送用互斥锁串行化。

## 验证与后续硬件测试

```powershell
py -m unittest discover -s tests -p 'test_*.py' -v
New-Item -ItemType Directory -Path build/host_tests -Force | Out-Null
gcc -std=c11 -Wall -Wextra -Werror -Itests/host/include -Imain/ir/transmitter -Imain/ir/gree tests/host/test_ir_waveform.c main/ir/transmitter/ir_waveform.c main/ir/gree/gree_ac.c main/ir/gree/gree_raw_catalog.c -o build/host_tests/test_ir_waveform.exe
./build/host_tests/test_ir_waveform.exe
idf.py build
```

主机测试使用隔离头文件，只验证实际C转换逻辑和状态选择；它不模拟RMT寄存器、载波、FreeRTOS调度或空调硬件。离线测试覆盖会话重置、缺号、分隔符、完整标签核对、六种风速、校验破坏、输出截断和已采集目录。

购买发射模块后先验证录制重放：确认38 kHz载波、有效电平、约20 ms内部间隔及约40 ms两段间隔，依次试25℃开关和风速，再尝试26/27℃一挡风。真实空调接受这些波形后，再验证24/28℃、模式切换及交叉组合，最后考虑动态编码。当前尚未完成任何真实发射或空调响应验收。
