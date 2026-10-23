# PocketPlan —— 基于 ESP32-S3-Touch-LCD-2.8B 的「随身阅读 + 计划」设备

> 设计文档 v0.2 · 2026-10-23（已锁定三个决策，见 §0.1）
> 硬件：Waveshare ESP32-S3-Touch-LCD-2.8B（480×640 RGB 屏 / TCA9554 / GT911 / PCF85063 / QMI8658 / TF 卡槽）
> 软件基座：ESP-IDF v6.1（本机已装好、已验证可编译可烧录）

---

## 0. 一句话架构

**一块由 480×640 触摸屏承载的「本地内容终端」**：文件全部在 TF 卡上，计划与配置在内部 LittleFS 上；设备自己开 WiFi 配网热点，连上后同时提供 **WebDAV（映射成电脑上的网络驱动器）** 和一个 **Web 管理页**；设备端用 LVGL 渲染计划清单、阅读器、播放器、设置；AI 走 OpenAI 兼容接口，Key 从 Web 端写入。

```
                        ┌──────────────────────────────┐
   TF 卡 (/sdcard)  ───▶│  文件服务  WebDAV + REST      │◀──▶ 电脑 / 手机
   ├─ books/            │  esp_http_server (port 80)   │    (映射网络驱动器)
   ├─ music/            └──────────┬───────────────────┘
   └─ pics/                        │
                        ┌──────────┴───────────────────┐
   内部 LittleFS ──────▶│  业务层  plans / settings / AI │
   ├─ plans.json        └──────────┬───────────────────┘
   ├─ config.json                  │
   └─ www/ (Web UI)     ┌──────────┴───────────────────┐
                        │  UI 层   LVGL 9 @ 480×640     │──▶ 触摸 GT911
                        └──────────┬───────────────────┘
                        ┌──────────┴───────────────────┐
   TF 卡 music/ ───────▶│  音频层  MP3/AAC 解码 → I2S   │──▶ 外接 DAC（见 §9）
                        └──────────────────────────────┘
```

---

## 0.1 已拍板的决策

| # | 决策点 | 结论 |
|---|---|---|
| 1 | 音频路线 | **A：I2S DAC（PCM5102A）+ 3.5mm 耳机孔**，不追求蓝牙 |
| 2 | 电子书格式 | **TXT + Markdown**（含 GBK 编码探测）；EPUB 暂不做，留接口 |
| 3 | AI 服务 | **DeepSeek 兼容接口**作默认，`base_url / model / api_key` 网页可改 |

由此产生的删减：不做 A2DP 相关工作量、不做 EPUB 解压与 XHTML 解析、不做 BLE 音频，M 阶段表里对应项已标为「暂缓」。

---

## 1. 先说三条硬约束（会改变你的预期）

### ① ESP32-S3 **不支持经典蓝牙**，所以「直连蓝牙耳机」做不到

ESP32-S3 只有 **BLE 5.0**，没有 Classic BT，而 A2DP（耳机音频）跑在 Classic BT 上。这不是固件问题，是芯片能力边界。三条可行路线：

| 方案 | 做法 | 额外硬件 | 代价 |
|---|---|---|---|
| **A ✅ 已选定** | ESP32 → I2S DAC → 3.5mm 耳机口 | PCM5102A 模块（~15 元） | 插线听，音质最好、延迟最低 |
| B（以后可选） | A 的方案 + 3.5mm 蓝牙发射器插在耳机口上 | PCM5102A + 蓝牙发射器（~40 元） | 多一级转接、多一个要充电的模块 |
| C（零硬件兜底） | 设备只当文件服务器，手机浏览器/播放器串流播放 | 无 | 不是「设备自己放」 |

**决定走 A**。硬件到位前，C 方案本来就会随第 3 条需求（网络文件服务）自动可用，所以不影响先把软件做起来。

### ② 没有麦克风，所以 AI 只能是**文字对话**

板上无 ES8311/ES7210、无 I2S 走线、无 MEMS 麦克风（原理图 + I2C 实扫已三重确认）。AI 交互靠屏上软键盘输入 → 请求 → 文字回复。想语音对话需要外接 I2S 麦克风，且会再占 2~3 个 GPIO。

### ③ 可用的空闲 GPIO 只剩 3 个，而且 TF 卡和屏共用引脚

已占用：`0,1,2,3,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,38,39,40,41,42,45,46,47,48`（含 USB、PSRAM、Flash）。

仅剩：

| GPIO | 现状 | 若挪用 |
|---|---|---|
| 43 | UART0 TXD（已引出排针） | 可做 I2S BCLK（控制台仍走 USB-Serial-JTAG，不影响调试） |
| 44 | UART0 RXD（已引出排针） | 可做 I2S LRCK |
| 4 | 电池 ADC 分压 | 可做 I2S DOUT（牺牲电量显示） |

→ **音频外设就用 43/44/4，一次性用光**。这也是「蓝牙走外挂发射器」更省事的原因。

**TF 卡**：`D0=GPIO42, CMD=GPIO1, CLK=GPIO2, CS/D3=EXIO4`。GPIO1/2 同时是 ST7701 的三线 SPI（仅初始化时用）。所以启动顺序必须是：**LCD 初始化完 → 销毁 panel_io → 再把 1/2/42 交给 SDMMC（1-bit 模式）**，EXIO4 常置高。不能用 SPI 模式挂 SD（CS 在扩展器上，sdspi 要真实 GPIO）。

---

## 2. 软件基座选择

**不 fork xiaozhi**，新开一个干净工程，但把已验证的零件搬过来：

| 直接复用 | 来源 | 为什么 |
|---|---|---|
| 板级初始化（ST7701 55 条序列 / TCA9554 方向顺序 / GT911 双地址 / 背光） | 我们已写好的 `xz/.../esp32-s3-touch-lcd-2.8b/` | 这块最费时、最容易翻车，已经在这块板子上验证过 |
| 热点配网 + 网页表单 | `managed_components/78__esp-wifi-connect`（独立组件，可直接 vendor） | 就是「参考小智的配网方式」的标准实现，含 AP + 网页 + `remember_bssid` |
| 联网看门狗（坏 AP 自动切换） | 我们写的 `NetCheckTask` | 你家有两个同名 AP，实测必要 |
| `WIFI_PS_NONE` + `BW20` | 同上 | 不加会随机 TLS 超时 |
| LVGL 移植层 | `esp_lvgl_port` | 官方组件 |

新工程 `pocket/`，独立于 `xz/`（xiaozhi 源码留着做参考）。

---

## 3. 功能拆解与模块

```
main/
├── app_main.c            启动编排（见 §4 启动顺序）
├── board/                板级（搬运 + 精简）
│   ├── board.c/h         I2C、TCA9554、背光、电源
│   ├── lcd_st7701.c      面板 + RGB 时序
│   ├── touch_gt911.c
│   └── rtc_qmi8658.c     PCF85063 / QMI8658
├── storage/
│   ├── sd_mount.c        SDMMC 1-bit 挂载 /sdcard
│   ├── lfs_mount.c       LittleFS 挂载 /lfs
│   └── fs_util.c         原子写、递归遍历、路径校验（防 ../ 穿越）
├── net/
│   ├── wifi_prov.c       复用 esp-wifi-connect
│   ├── netcheck.c        连通性看门狗
│   ├── http_server.c     esp_http_server 注册
│   ├── webdav.c          PROPFIND/GET/PUT/DELETE/MKCOL/MOVE
│   ├── api_files.c       /api/files/*
│   ├── api_plans.c       /api/plans/*
│   ├── api_settings.c    /api/settings
│   └── api_ai.c          /api/ai/chat
├── ui/
│   ├── ui.c              LVGL 初始化、页面栈、状态栏
│   ├── page_home.c       计划列表（主页）
│   ├── page_reader.c     阅读器
│   ├── page_music.c      播放器
│   ├── page_settings.c   设置
│   ├── kb.c              软键盘（WiFi 密码 / API Key）
│   └── font/             CJK 子集字体（见 §7）
├── logic/
│   ├── plans.c           计划 / 打卡 / 连击统计
│   ├── settings.c        config.json 读写
│   ├── reader.c          TXT/MD/EPUB 解析与分页
│   ├── audio.c           解码 → I2S
│   └── ai.c              OpenAI 兼容客户端
└── CMakeLists.txt
```

---

## 4. 启动顺序（顺序错了会挂）

```
1. NVS init
2. I2C 总线（GPIO15/7）
3. TCA9554：set_dir(OUTPUT 0x8F) → set_level → BUZZER=0, LCD_CS=1, SD_CS=1, TP_RST=1, LCD_RST=1
4. LCD：3-wire SPI 走 GPIO1/2 + CS(EXIO3) 发 55 条初始化 → RGB 面板 → esp_lcd_panel_init → 删除 panel_io（释放 GPIO1/2）
5. 背光 PWM（GPIO6）
6. GT911（复位：INT 低 → EXIO2 低 10ms → 高 200ms → INT 高 → INT 转输入）
7. **SDMMC 1-bit**（CMD=1, CLK=2, D0=42, EXIO4 高）→ 挂 /sdcard
8. LittleFS → 挂 /lfs
9. settings_load() / plans_load()
10. LVGL 初始化（缓冲用 PSRAM，双缓冲 480×40）→ 显示首屏
11. WiFi：NVS 有凭据 → 直连；否则开 AP 配网（SSID `PocketPlan-XXXX`，门户页 192.168.4.1）
12. GOT_IP → netcheck → SNTP 对时 → RTC 写入
13. 启动 HTTP 服务（80）
```

---

## 5. 存储设计

### 分区表（16MB Flash）

| 分区 | 类型 | 偏移 | 大小 | 用途 |
|---|---|---|---|---|
| nvs | data/nvs | 0x9000 | 24K | WiFi 凭据、设备 ID |
| otadata | data/ota | 0xf000 | 8K | |
| phy_init | data/phy | 0x11000 | 4K | |
| ota_0 | app | 0x20000 | 3M | 固件 A |
| ota_1 | app | 0x320000 | 3M | 固件 B（OTA 回滚） |
| storage | data/spiffs | 0x620000 | 1.2M | LittleFS：`plans.json`、`config.json`、`www/` |
| assets | data/spiffs | 0x740000 | 4M | 字体（CJK 子集）、图标 |

### 目录约定

```
/sdcard/
  books/     *.txt  *.md  *.epub
  music/     *.mp3  *.wav  *.flac
  pics/      封面、开机图
/lfs/
  plans.json
  config.json
  www/index.html, app.js, style.css
```

### 原子写

`plans.json` / `config.json` 一律 **写 `*.tmp` → `fsync` → `rename`**，保留 `*.bak`。写入防抖 2 秒（多次打卡合并一次落盘），避免 LittleFS 频繁擦写。

---

## 6. 网络服务设计（核心）

### 6.1 端口与挂载

- HTTP 服务监听 **80**（可选 8080 备用）。
- **WebDAV 根路径 `/dav/`**，映射成系统驱动器：

| 客户端 | 挂载方式 |
|---|---|
| Windows | 「映射网络驱动器」→ `http://192.168.x.x/dav`（资源管理器走 `\\ip@80\DavWWWRoot`） |
| macOS | Finder → 连接服务器 → `http://192.168.x.x/dav` |
| Android | Solid Explorer / Documents 添加 WebDAV |
| iOS | 文件 App → 连接服务器 → WebDAV |

> 为什么不用 SMB：SMB2 服务端要 100KB+ RAM、445 端口常被运营商/防火墙挡、且在 IDF 上没有成熟成熟度足够高的实现。WebDAV 是纯 HTTP，能顺带复用同一套 Range/鉴权/日志。

### 6.2 REST API

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | `/` | 单页 Web 管理界面 |
| GET | `/api/status` | 固件版本、IP、SD 剩余空间、RSSI、运行时长 |
| GET | `/api/plans?date=YYYY-MM-DD` | 某天要做的计划（含打卡状态） |
| POST | `/api/plans` | 新建 `{title, note, repeat, remind}` |
| PATCH | `/api/plans/{id}` | 编辑 |
| DELETE | `/api/plans/{id}` | 删除（软删，保留历史） |
| POST | `/api/plans/{id}/checkin` | 打卡 `{date}`，幂等 |
| DELETE | `/api/plans/{id}/checkin?date=` | 取消打卡 |
| GET | `/api/stats?from=&to=` | 连击、完成率、日历热力图数据 |
| GET | `/api/settings` | 读设置（**API Key 只返回掩码**） |
| POST | `/api/settings` | 写设置（WiFi/亮度/时区/AI 配置） |
| POST | `/api/ai/chat` | `{messages:[{role,content}]}` → `{reply}` |
| GET | `/api/files/list?path=/books` | 目录列表（大小、修改时间） |
| GET | `/files/{path}` | 下载，**支持 Range**（边下边听/看大书） |
| PUT | `/api/files/{path}` | 原始 body 上传（适合大文件，无 multipart 开销） |
| POST | `/api/files/upload` | multipart，供浏览器表单用 |
| POST | `/api/files/mkdir` `/rename` `/delete` | 目录与文件操作 |
| PROPFIND/GET/PUT/DELETE/MKCOL/MOVE/LOCK/UNLOCK | `/dav/{path}` | WebDAV |

鉴权：可选 HTTP Basic（设置里设密码）。默认关闭（局域网），开启后 `/dav` 和 `/api` 全部要求认证；`/` 静态页不拦，方便登录。

### 6.3 并发模型

```
core 0: LVGL task (5ms tick)          ← 所有 UI 操作必须持 lvgl lock
core 0: httpd task (esp_http_server)  ← 默认 4KB 栈不够，调到 12KB
core 1: app_task (业务事件队列)
core 1: audio_task (解码 + I2S 写入，优先级 6)
core 0: ai_task (HTTP + TLS，栈 8KB，PSRAM 缓冲)
```

跨任务只通过 **FreeRTOS 队列 + 事件位**，UI 更新一律 `lvgl_port_lock()` 包裹；网络任务绝不直接碰 LVGL。

### 6.4 大文件与内存

- 上传/下载一律 **64KB 流式拷贝**（`esp_http_client`/`httpd_resp_send_chunk`），不整文件进内存。
- 单次请求上限 32MB（防误传）。
- MIME 表覆盖 txt/md/epub/mp3/wav/flac/jpg/png/ogg。

---

## 7. 数据模型

### plans.json

```json
{
  "version": 1,
  "plans": [
    {
      "id": "p_1730000000",
      "title": "晨跑 3km",
      "note": "配速 6'30\"",
      "repeat": { "type": "daily" },
      "remind": "07:00",
      "created": "2026-10-20",
      "archived": false
    },
    {
      "id": "p_1730000001",
      "title": "背 20 个单词",
      "repeat": { "type": "weekly", "weekdays": [1,3,5] },
      "created": "2026-10-21",
      "archived": false
    }
  ],
  "checkins": {
    "p_1730000000": { "2026-10-22": 1761187200, "2026-10-23": 1761273600 },
    "p_1730000001": { "2026-10-23": 1761273600 }
  }
}
```

- `repeat.type`：`daily` / `weekly`(weekdays) / `once`(date) / `interval`(每 N 天)
- 连击（streak）与完成率**运行时计算**，不落盘（避免不一致）
- 打卡记录用 `YYYY-MM-DD` 作 key，天然幂等、跨时区安全

### config.json

```json
{
  "version": 1,
  "display": { "brightness": 80, "theme": "light", "dim_after_s": 60, "font_size": 20 },
  "clock": { "tz": "CST-8", "ntp": "ntp.aliyun.com" },
  "net": { "http_port": 80, "auth": { "enabled": false, "user": "admin", "pass": "" } },
  "ai": {
    "enabled": true,
    "base_url": "https://api.deepseek.com/v1",
    "model": "deepseek-chat",
    "api_key": "sk-****",
    "system_prompt": "你是我的日程助手，回答简短。",
    "temp": 0.7
  },
  "audio": { "volume": 70, "output": "i2s" }
}
```

---

## 8. UI 设计（480×640 竖屏）

```
┌────────────────────────────┐
│ 09:41   WiFi  🔋82%        │ 40px 状态栏
├────────────────────────────┤
│  10月23日 周四              │
│  ● 晨跑 3km           ✓    │ 72px 行，右侧勾选圆点
│  ○ 背 20 个单词            │ 点击=打卡，长按=编辑
│  ○ 读书 20 页              │
│  ────────────────────      │
│  今日 1/3   连击 4 天       │
├────────────────────────────┤
│  计划   阅读   音乐   设置   │ 48px 底部导航
└────────────────────────────┘
```

- **阅读器**：全屏文字，点左 1/4 上一页、右 1/4 下一页、中间呼出菜单（字号 / 目录 / 书签 / 进度条）。
- **音乐**：封面区 + 标题 + 进度条 + 上一首/播放/下一首；列表从 `/sdcard/music` 递归扫描。
- **设置**：WiFi（重新配网）、亮度、时区、AI（只显示 Key 掩码 +「在网页配置」提示）、存储占用、关于。
- **软键盘**：`lv_keyboard`，中文暂不支持输入（只读展示中文；输入用英文/数字/符号，够填密码和 API Key）。
- 无操作 60 秒降亮度、120 秒关背光（触摸唤醒）。

---

## 9. 音频方案（需要加硬件）

### 推荐 BOM（决策 A）

| 件 | 型号 | 价格 | 接线 |
|---|---|---|---|
| I2S DAC | **PCM5102A** 模块（或 ES9023） | ~15 元 | BCLK=GPIO43, LRCK=GPIO44, DIN=GPIO4, VIN=3.3V, GND |
| 耳机座 | 3.5mm 立体声 | ~1 元 | DAC L/R out（板上有裁剪好的焊盘更好） |
| 可选：功放 | MAX98357A + 3W 喇叭 | ~15 元 | 同一条 I2S 三线，直接外放 |
| 暂缓：蓝牙 | 3.5mm 蓝牙**发射**器 | ~40 元 | 插在耳机座上（以后想用无线耳机再加，软件零改动） |

> 只接 DAC 模块时，电源从板上 3.3V 取，注意 DAC 的模拟地与功放区分开；PCM5102A 模块自带 LDO，直供 3.3V 即可。

### 解码链

```
TF 卡 mp3 → esp_audio_codec（MP3/AAC/FLAC/WAV 解码）
          → 重采样到 44.1k/16bit 立体声
          → i2s_channel_write（I2S0, 主模式, 3 线）
          → DAC → 耳机 / 功放 / 蓝牙发射器
```

- 用 Espressif 官方组件 `espressif/esp_audio_codec`（components.espressif.com 直连可达）。
- 播放缓冲 2×8KB 在 PSRAM，解码任务独立，卡顿不影响 UI。
- 暂停/切歌时 `i2s_channel_disable` 关时钟，防底噪。
- 板载**蜂鸣器（EXIO8）**可以留给「打卡成功」「整点提醒」的提示音，不参与音乐。

### 如果要走「C 方案」（零硬件）

设备只提供 `/files/music/xxx.mp3` 的 Range 下载，Web 页内置一个 `<audio>` 播放器，手机连上后直接听 —— 不用改任何硬件，第 3 条需求顺手就覆盖了。

---

## 10. 阅读器设计

| 格式 | 做法 | 优先级 |
|---|---|---|
| `.txt` | 编码探测（UTF-8 / GBK，带 GBK→Unicode 表约 40KB）→ 按字节切行 → 分页 | **P0（必做）** |
| `.md` | 轻量解析（标题/列表/代码块/粗体）→ LVGL label + 不同样式 | **P1（必做）** |
| `.epub` | `zlib`/`miniz` 解压 → OPF → spine → XHTML 去标签 | 暂缓（接口预留，不做实现） |

**分页算法**：给定字号与可用宽高，逐行测量 `lv_txt_get_size()`，累计到超出高度即翻页；页边界缓存到 `book.pos[]`，翻页 O(1)。大文件按需读取（每页只读 4KB 窗口），不整本入内存。

**中文字体**（关键）：LVGL 内置字体没有 CJK。方案：

1. 用 `lv_font_conv` 生成 **GB2312 一级字库 3755 字 + 标点** 的 20px/4bpp 字体，约 1.4MB，烧进 `assets` 分区；
2. 再加一个 28px 版本供阅读器放大用（约 2.4MB），合计 ~3.8MB，16MB Flash 放得下；
3. 生僻字缺字时降级显示 `□`（可接受，网文/公版书基本覆盖）。
4. 备选：`freetype` + SD 卡里的 TTF（覆盖全，但每页渲染慢 3~10 倍，不推荐做默认）。

---

## 11. AI 功能设计

- **协议**：OpenAI 兼容 `POST {base_url}/chat/completions`，默认 DeepSeek（也可填任何兼容网关：OpenAI、Moonshot、硅基流动、本地 Ollama）。
- **配置**：Web 端 `/api/settings` 写入 `base_url / model / api_key / system_prompt`，界面只回显掩码 `sk-****abcd`。
- **三个用法**：
  1. **日程助手**：把今日计划 + 打卡情况塞进 system 上下文，问「我今天还剩什么」「帮我把明天的计划排一下」；
  2. **书摘问答**：把当前页文本作为上下文，「这段什么意思」「总结这一章」；
  3. **一句话总结打卡**：每晚汇总当天完成情况生成一句话。
- **实现要点**：
  - `esp_http_client` + `esp_crt_bundle`（已在本板验证 TLS 可用），**必须设 `timeout_ms`**（我们之前被无超时坑过）；
  - 请求体/响应体都走 PSRAM，超时 30s，失败重试 1 次；
  - 支持 SSE 流式（边收边刷 label），无流式则整段返回；
  - **API Key 落在 NVS + `config.json`，明文**（无 Flash 加密的话防不住拆机读 flash）—— 文档里明确写这一点，别存高价值 Key。

---

## 12. 配网流程（第 2 条需求）

复用 `esp-wifi-connect` 的行为，与 xiaozhi 一致：

1. 上电 → NVS 无凭据（或连续 3 次连不上）→ 进配网模式；
2. 板子开 AP：`PocketPlan-XXXX`（开放网络），显示屏给出提示 + 二维码（二维码内容是 `http://192.168.4.1`）；
3. 手机连 AP → 自动弹门户页（`esp_wifi_set_dns` 做 captive portal 劫持）→ 选 SSID、填密码 → 提交；
4. 板子保存凭据 → 重启 STA → 连上后屏幕提示「已连接：192.168.0.x」并显示 WebDAV 地址；
5. 长按 BOOT（GPIO0）3 秒 → 重新进入配网模式（换网络用）。

配网页同时提供「设备名」「HTTP 密码」两个可选字段，一次配完。

---

## 13. 开发阶段（每阶段都可烧进板子验证）

| 阶段 | 内容 | 验收 | 预估 |
|---|---|---|---|
| **M1** | 新工程骨架 + 板级搬运（LCD/触摸/背光/LVGL） | 屏幕显示自定义首页骨架 | 1 天 |
| **M2** | SDMMC 挂载 + LittleFS + 原子写 | 串口能列出 SD 文件，掉电不丢配置 | 0.5 天 |
| **M3** | HTTP 服务 + 文件 API + WebDAV | 电脑映射网络驱动器，拖文件进去 | 1.5 天 |
| **M4** | Web 管理页（文件管理 + 计划编辑） | 浏览器里建计划、传书 | 1.5 天 |
| **M5** | 计划数据层 + 主页列表 + 触摸打卡 + 连击统计 | 屏上打卡，网页同步看到 | 1.5 天 |
| **M6** | 配网（复用组件）+ 设置页 + 软键盘 | 换 WiFi 不用改代码 | 1 天 |
| **M7** | TXT/MD 阅读器 + CJK 字体 | 打开 SD 上的中文小说翻页流畅 | 2 天 |
| **M8** | AI 对话（文字） | 网页配 Key，屏上问一句有回答 | 1 天 |
| **M9** | 音乐播放（等硬件到位） | 插耳机出声、暂停切歌 | 1.5 天 |
| **M10** | OTA、功耗优化、异常恢复、打磨 | 完整可用 | 2 天 |

合计约 **11~12 个工作日**（已扣除 EPUB 与蓝牙相关工作；按每天有效编码 6 小时算）。
硬件未到位时可以先做 M1~M8，音乐（M9）等 PCM5102A 到了再补，C 方案（手机播放）在此期间已可用。

---

## 14. 风险清单

| 风险 | 影响 | 对策 |
|---|---|---|
| SD 与 LCD 共用 GPIO1/2 | 挂载失败 / 花屏 | 严格按 §4 顺序：先 LCD 后 SD，且 LCD 用 `auto_del_panel_io` 释放总线 |
| SD CS 在扩展器上 | 无法用 SPI 模式 | 用 SDMMC 1-bit（CS 不需要 GPIO），EXIO4 常高 |
| CJK 字体体积 | 撑爆 Flash | 子集化 + 只做 20/28 两档 + 生僻字降级 |
| `esp_http_server` 单线程 | 大文件传输时网页卡 | 上传下载流式、限速、放独立任务 |
| WebDAV 兼容性 | Windows 挂载失败 | 处理 PROPFIND depth、LOCK 假锁、`Content-Length`、路径编码 |
| esp_tls 无超时 | 卡死（已踩过） | 所有 TLS 客户端显式 `timeout_ms` |
| 计划文件写坏 | 数据丢失 | 原子写 + `.bak` + 启动校验，坏则回滚 |
| 无音频硬件 | 音乐功能不可用 | 先做 C 方案（手机播放），硬件到位再开 M9 |
| 没有麦克风 | AI 只能打字 | 明确产品定位，或后续加 I2S 麦克风 |

---

## 15. 下一步

三个决策已锁定（§0.1），可以直接开工。建议从 **M1 + M2** 开始：先把新工程骨架和板级/LVGL/SD 挂载跑通，屏幕上有我们自己的首页，SD 卡能列出文件——这是后面所有功能的地基，也是一次性风险最高的部分（GPIO1/2 复用、SDMMC 1-bit）。

每一阶段我都会编译烧录并让你在屏幕上看结果，验收通过再进下一阶段。

---

## 附：与现有代码的关系

- `xz/src/xiaozhi-esp32-main/` —— 完整的 xiaozhi 移植，**保留**，作为板级代码与配网组件的参考源（也是现在板子里在跑的固件，随时可以刷回去）。
- `demo/`、`i2cscan/`、`lcdtest/` —— 早期验证固件，保留作硬件回归测试。
- 新工程 `pocket/` —— 本项目。
- 工具：`tools/idfenv.ps1`（IDF 环境）、`tools/run.ps1`（Arduino 编译烧录）、`PORT-REPORT.md`（移植记录）。
