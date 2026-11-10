# BadgePlayer-IDF 开发日志

> 记录每次改动的**动机、根因和验证方式**。只写"改了什么"的日志没有价值，
> 这里重点保留踩坑过程和判断依据，方便以后回查。

## 索引

| 日期 | 主题 | 主要文件 |
|---|---|---|
| 2026-11-10 | 停播啸叫根治、16MB 分区表、Windows 构建链路适配 | `main/audio_player.c`、`main/main.c`、`partitions.csv`、`sdkconfig.defaults` |

---

## 2026-11-10 停播啸叫根治与工程落地

### 摘要

| 问题 | 结论 |
|---|---|
| 拿走 NFC 卡后扬声器持续啸叫 | `i2s_chan_config_t.auto_clear` 默认 `false`，underrun 时硬件无限重发 DMA 里最后一段 PCM |
| 固件 1.7MB 超出默认 1MB app 分区 | 自定义 `partitions.csv`，factory 扩到 4MB |
| ccache 在中文路径下崩溃 | 构建加 `--no-ccache` |
| GNU ld 不认中文路径 | 工程整体迁到纯 ASCII 路径 |
| WiFi 连不上导致无限重启 | `ESP_ERROR_CHECK(init_wifi())` 改为只告警不 abort |

---

### 一、构建链路：三个连环坑

这三个坑的共同点是**真实报错都被 ninja 埋在 1300 行日志底部**，表面只看到
`ninja: build stopped: subcommand failed`。

#### 1.1 ccache 在中文路径下抛异常

```
FAILED: espressif__gmf_fft/.../fft_heap.c.obj
terminate called after throwing an instance of 'std::filesystem::__cxx11::filesystem_error'
  what():  filesystem error: Cannot convert character sequence: Illegal byte sequence
```

ccache 内部用 `std::filesystem` 转换路径，遇到非 ASCII 字符直接抛异常，
被 ccache 包装的编译任务全部失败。

**解决**：`idf.py --no-ccache build`

#### 1.2 GNU ld 不认中文路径

```
ld.exe: cannot find D:/work/.../AI吧唧嵌入�?/BadgePlayer-IDF/managed_components/
        espressif__esp_audio_effects/lib/esp32s3/libesp_audio_effects.a: Invalid argument
```

注意路径被截成 `AI吧唧嵌入�?`——Windows 上的 GNU 工具链走 ANSI 代码页，
非 ASCII 字符截断后变成非法字节。**这个问题没有编译选项可以绕开**，只有两条路：
改用 MSVC 工具链（不现实），或把工程放到纯 ASCII 路径。

**解决**：工程整体迁到 `D:\work\WorkBuddyProject\BadgePlayer-IDF`。

> 同一个根因在 PlatformIO 工程上表现为 `ld.exe: cannot open map file`。
> Windows + GNU 工具链 + 中文路径 = 不可用，记住这条。

#### 1.3 app 分区太小

```
Error: app partition is too small for binary BadgePlayer-IDF.bin size 0x1a22d0:
  - Part 'factory' 0/0 @ 0x10000 size 0x100000 (overflow 0xa22d0)
```

默认 `CONFIG_PARTITION_TABLE_SINGLE_APP` 只给 factory 1MB，而本工程带
音频解码栈（esp_audio_codec + esp_muxer + esp_audio_effects）+ mbedtls + WiFi，
固件 1.71MB。

板子实测（`esptool.py flash_id`）：**ESP32-S3 rev0.2 / 16MB Flash / 8MB PSRAM**。

**解决**：新增 `partitions.csv`，`sdkconfig` 切到自定义分区表：

| Name | Type | Offset | Size |
|---|---|---|---|
| nvs | data/nvs | 0x9000 | 24 KB |
| phy_init | data/phy | 0xf000 | 4 KB |
| factory | app/factory | 0x10000 | **4 MB** |
| storage | data/spiffs | 0x410000 | 11.9 MB |

---

### 二、停播啸叫：五轮迭代

现象：卡片靠近开始放歌正常，**拿走卡片后扬声器持续啸叫**；播放中在临界距离
晃动也会啸叫。同硬件同外围的 Arduino 版本（`ESP32-audioI2S`）完全正常。

这块板子的音频链路：ESP32-S3 → I2S（BCLK=10 / WS=11 / DIN=9）→ MAX98357A → 喇叭。

#### 2.1 排查过程（含失败尝试）

| 轮次 | 假设 | 改动 | 结果 |
|---|---|---|---|
| 1 | 卡片状态抖动导致反复起播 | 加去抖（连续 5 次读不到才算离开）、主循环加 20ms 延时、stop 同步等待 pipeline 落地、同 URL 幂等 | 明显改善：从"一直叫"变成"叫约 1 秒" |
| 2 | stop 等待期间 DMA 欠载 | 等待期间边等边灌静音；停播后 `i2s_channel_disable()` | 仍有啸叫 |
| 3 | 停 I2S 时钟方向错误 | 改为仿 Arduino：**不关时钟**，灌 60ms 静音冲掉残留 PCM | 仍有啸叫 |
| 4 | 输出幅度过大引起 D 类自激 | 加软件音量 34/64（对齐 Arduino `setVolume(15)`） | 音量明显变小，但**仍然啸叫** |
| 5 | **DMA underrun 时硬件重发旧数据** | `chan_cfg.auto_clear = true` + 常驻 `silence_keeper` 任务持续喂 0 | ✅ 解决 |

#### 2.2 决定性根因

```c
i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
chan_cfg.auto_clear = true;   // 默认 false，是"一直叫"的物理来源
```

`auto_clear = false` 时，DMA 发送缓冲被发空（underrun）后，**ESP32 的 I2S
硬件会把缓冲区里最后一段数据无限循环重发**，而不是补 0。

而最后一段数据恰好是音乐的高频残留。所以听到的不是"残留"而是"持续啸叫"——
它根本不会自己衰减。这也解释了为什么前四轮的软件灌静音都只能管一时：
pipeline 内部线程晚一点收尾、DMA 指针一回卷，旧数据又被顶出来了。

#### 2.3 为什么 Arduino 版不叫

`lib/ESP32-audioI2S/src/Audio.cpp` 的做法是**预先填零**：

```cpp
// Audio.cpp:2300  playI2Sremains()
m_validSamples = m_i2s_config.dma_buf_len * m_i2s_config.dma_buf_count;
while (m_validSamples) playChunk();        // 把整个 DMA 队列用 0 走完一遍
i2s_zero_dma_buffer((i2s_port_t) m_i2s_num);

// Audio.cpp:2274  stopSong()
memset(m_outBuff, 0, sizeof(m_outBuff));
i2s_zero_dma_buffer((i2s_port_t) m_i2s_num);
```

它从不 `i2s_stop` / `disable`，而是**让 DMA 里根本没有脏数据可重发**。
所以即使 underrun，重发的也是 0。和 `auto_clear = true` 殊途同归。

#### 2.4 与本工程的对应关系

| 维度 | Arduino (`ESP32-audioI2S`) | 本工程 (ESP-IDF) |
|---|---|---|
| 音量 | `volumetable[15] = 34`，`v[i] = (s[i] * 34) >> 6` | `pcm[i] = (pcm[i] * 34) >> 6`（逐字对齐） |
| 清 DMA | `playI2Sremains()` 预填 0 + `i2s_zero_dma_buffer()` | `auto_clear=true`（硬件补零）+ `i2s_write_silence(60)` |
| I2S 时钟 | 全程不 disable | 全程不 disable（仅采样率变化时短暂重配） |
| 停播标志 | `m_f_running = false` | `s_output_muted = true`（输出闸门） |

**只有音量算法是逐字照搬的**，其余是按"效果等价"重新实现。
这一点必须说清楚——IDF 的 `esp_asp` 是异步 pipeline，和 Arduino 的同步
单线程 loop 结构完全不同，不可能照抄代码结构。

#### 2.5 硬件侧的一个坑（尚未处理，留档）

从 Board2 的 PCB 网表（`PAD_NET` 记录）提取：

```
NET ['98357_SD']    -> 只有 H3.5 一个焊盘
NET ['98357_GAIN']  -> 只有 H3.4 一个焊盘
```

H3 是 MAX98357A 模块的排母（`1=LRC, 2=BCLK, 3=DIN, 4=GAIN, 5=SD, 6=GND, 7=3V3`）。
**`SD`（关断控制）和 `GAIN`（增益）在板上完全悬空**，各只连排母一个脚，
没有任何走线通往别处。

后果：功放的使能和增益状态 **MCU 完全无法控制**，只能靠模块小板上的默认电阻。
这也是为什么停掉 I2S 时钟会让芯片进入不确定状态（第 2 轮的失败尝试）。

如果将来仍有异常噪声，硬件兜底方案是给 H3.5 加下拉到 GND，
或飞线到空闲 GPIO 由固件控制关断。

#### 2.6 顺带记录：软件音量为什么必须加

Arduino 版 `setVolume(15)` 实际是 **34/64 ≈ 53%，即 -5.5dB** 的数字衰减。
IDF 版原先没有任何音量控制，PCM 满幅直出，输出功率高 3 倍以上。

MAX98357A 是 **filterless D 类功放**，没有输出滤波器，输出幅度越大，
喇叭线与 PCB 走线耦合回输入的能量越强，停播后越容易越过自激阈值。
这虽然不是你这次啸叫的主因（主因是 `auto_clear`），但确实是叠加因素之一。

当前值在 `main/audio_player.c:39`，对照表在 `Audio.h:463`：

```
volumetable = {0,1,2,3,4,6,8,10,12,14,17,20,23,27,30,34,38,43,48,52,58,64}
                                                                  ↑ 索引15
```

---

### 三、顺带修掉的两个致命问题

#### 3.1 WiFi 连不上导致无限重启

排查日志时发现每次输出都停在 `wifi:enable tsf` 之后一片空白。
**不是卡死，是在重启循环**：

```
ESP_ERROR_CHECK failed: esp_err_t 0x107 (ESP_ERR_TIMEOUT)
file: "./main/main.c" line 251
expression: init_wifi()
abort() was called at PC 0x4037f7b7
Rebooting...
rst:0xc (RTC_SW_CPU_RST)
```

`ESP_ERROR_CHECK(init_wifi())` 在 WiFi 超时后会 abort 重启，设备永远进不了主循环，
**NFC 根本没机会初始化**。

**解决**：改成只告警不 abort；`wifi_event_handler` 里两处
`ESP_ERROR_CHECK(esp_wifi_connect())` 也改掉（那里在 WiFi 任务上下文，
abort 会拖死整机）；等待时间 15s → 10s。

> 排查"启动卡住"类问题的通用手法：不要只看日志停在哪，一定要 grep
> `abort|Backtrace|Rebooting|rst:0x`——很可能设备在重启循环，
> 日志尾部永远是同一处，看起来像卡死。

#### 3.2 `silence_keeper` 任务触发看门狗

```
task_wdt: Task watchdog got triggered.
task_wdt: CPU 1: silence_keeper
```

第一版守护任务没有让出点，`i2s_channel_write` 一旦立即返回就变成忙等死循环。

**解决**：每次写完无条件 `vTaskDelay(4ms)`，保证一个 WDT 周期内必然休眠。

---

### 四、最终改动清单

#### `main/audio_player.c`

| 改动 | 作用 |
|---|---|
| `chan_cfg.auto_clear = true` | underrun 补 0 而非重发旧数据（**核心**） |
| init 时灌 60ms 静音且**不 disable 时钟** | 清掉未初始化 DMA，保持 BCLK 常驻 |
| `silence_keeper` 常驻任务（优先级 1） | muted 期间持续喂 0，兜住 pipeline 晚收尾 |
| `write_pcm` 软件音量 `(pcm * 34) >> 6` | 对齐 Arduino `setVolume(15)` |
| `write_pcm` 闸门分支返回 `size` 而非 `0` | 返回 0 会被上层当成写失败 |
| stop 等待期间边等边灌静音（600ms 上限） | 等 pipeline 落地期间不 underrun |
| `MUSIC_INFO` 只在采样率变化时重配时钟 | 避免每次切歌 BCLK 停摆再恢复导致爆音 |
| 同 URL 且仍在播时直接返回 | 防卡片抖动触发反复重建 pipeline |

#### `main/main.c`

| 改动 | 作用 |
|---|---|
| `CARD_ABSENCE_THRESHOLD 5` 去抖 | 连续 5 次读不到才判离开（≈600ms） |
| 主循环末尾 `vTaskDelay(20ms)` | 原版无延时，循环频率可达几百 Hz |
| `init_wifi` 失败不再 abort | 避免无 WiFi 时无限重启 |
| `audio_player_stop` 失败只告警 | 收尾动作不值得 abort 整机 |

#### 新增文件

- `partitions.csv` —— 16MB Flash 自定义分区表
- `sdkconfig.defaults` —— WiFi SSID/密码等默认配置

---

### 五、经验教训

1. **ninja 的 `subcommand failed` 是噪音，必须往下翻日志找真实报错。**
   `grep -i -E "error|fatal|FAILED:"` 是第一步。

2. **Windows + GNU 工具链 + 中文路径 = 一定出问题。**
   ccache 抛 `filesystem_error`，ld 报 `Invalid argument`，
   PlatformIO 报 `cannot open map file`，全是同一个根因。

3. **遇到"对方能跑我不能跑"，先去看对方源码，比自己猜快十倍。**
   这次五轮迭代里有三轮是在猜，直到把 `Audio.cpp` 的 `stopSong()`
   和 `playI2Sremains()` 读完，方向才真正对上。

4. **DMA 相关问题的默认值一定要查。**
   `auto_clear` 这种字段默认 `false`，文档里不显眼，但决定了 underrun 的行为。
   同类字段：`dma_desc_num`、`dma_frame_num`、`tx_desc_auto_clear`。

5. **"一直响"和"响一下"是两个完全不同的问题。**
   前者说明有东西在循环，后者才是残留。第 1 轮从"一直叫"变成"叫 1 秒"
   其实已经缩小了范围——1 秒恰好等于 stop 的轮询上限，这是很强的提示。

---

### 六、验证方式

#### 串口日志

```powershell
D:\esp\idf.bat monitor          # 退出：Ctrl + ]
D:\esp\idf.bat flash
D:\esp\idf.bat build
```

> `monitor` 会独占串口，烧录前必须先退出，否则报 `port is busy`。
> 若被残留进程占用：
> ```powershell
> Get-CimInstance Win32_Process -Filter "Name='python.exe'" |
>     Where-Object { $_.CommandLine -like '*idf_monitor*' } |
>     ForEach-Object { Stop-Process -Id $_.ProcessId -Force }
> ```

#### 正常启动日志

```
boot: Loaded app from partition at offset 0x10000
esp_psram: Found 8MB PSRAM device / SPI SRAM memory test OK
wifi:connected with <SSID>, channel 6, rssi: -12
esp_netif_handlers: sta ip: 10.40.213.41
badge_player: PN532 chip PN532 firmware 1.6
badge_player: NFC and audio ready; present a card to play a song
```

#### 停播时应看到

```
audio_player: stop: requesting pipeline stop (state=1)
audio_player: stop: pipeline stopped in 30ms, state=3
```

若出现 `stop: pipeline still not stopped after 600ms`，说明卡在 pipeline 内部
（HTTP 下载线程或解码器没退），需要另查。

#### 人工验证项

- [x] 贴着卡片不动，持续播放
- [x] 临界距离缓慢移动，不啸叫
- [x] **快速拿走卡片，声音立即停止**（本次修复目标）
- [x] 放回卡片能正常出声
- [x] 反复刷同一张卡，不重复起播

---

### 附：硬件参数

| 项 | 值 |
|---|---|
| 芯片 | ESP32-S3 (QFN56) rev v0.2 |
| Flash | 16MB (boya), DIO / 80MHz |
| PSRAM | 8MB Octal, 80MHz |
| I2S | BCLK=GPIO10, WS=GPIO11, DIN=GPIO9 |
| PN532 (SPI) | SCK=15, MISO=16, MOSI=17, SS=18, RST=8, IRQ=3 |
| TB6612 | PWMA=14, AIN1=13, AIN2=12（M_STBY 悬空） |
