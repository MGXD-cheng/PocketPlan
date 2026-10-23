# PocketPlan 项目进度报告

> 生成时间：2026-10-24 03:4x（本地，M14.2 滑块纯黑 + README 图库；此前 M14 自动休眠 + 开源发布，M13.1 把时钟模式改成横屏）
> 设备：Waveshare **ESP32-S3-Touch-LCD-2.8B**（2.8" 480×640 RGB 屏 + GT911 触摸 + 8MB PSRAM + 16MB Flash）
> 代码位置：`D:\Users\PC\Desktop\esp32-mgxd\pocketplan\`
> 设计文档：`D:\Users\PC\Desktop\esp32-mgxd\PLAN-BOARD-DESIGN.md`（v0.2）

---

## 一、任务目标

### 1.1 用户提出的原始需求（原话）

> 我想做一个可以从存储卡内读取文件的设备，功能包括：
> 1. 看电子书、听音乐（设置中连接蓝牙耳机）
> 2. 参考小智的初始联网方式（热点连接）进行 WiFi 配网
> 3. 连上 WiFi 之后，可以映射一个网络端口，对其内部文件进行上传和下载
> 4. 主页是一个计划列表，可以在 WiFi 网络端口上进行编辑。计划支持每日打卡
> 5. 还要有一个 AI 功能，API key 可以在网络端口上配置

### 1.2 需求落地时确认的三个现实边界（硬约束）

| # | 约束 | 结论 |
|---|---|---|
| 1 | ESP32-S3 **只有 BLE，没有经典蓝牙** | A2DP 蓝牙耳机**不可能**直接在板子上做；听音乐走 **3.5mm 耳机孔**，想要无线就外挂一个 3.5mm 蓝牙发射器 |
| 2 | 板子**没有麦克风、没有音频编解码芯片**（图纸 + 引脚表 + I2C 扫描三重确认，只有蜂鸣器） | AI 功能做成**文字对话**（打字提问、屏幕显示回答） |
| 3 | 空闲 GPIO 只剩 **43 / 44 / 4** 三个 | 音频 I2S 会全部占用；GPIO4 让出后电池 ADC 不可用 |

### 1.3 已拍板的技术选型

- **音频**：方案 A —— `PCM5102A` I2S DAC + 3.5mm 耳机孔（GPIO43/44 + GPIO4）
- **电子书**：先做 **TXT + Markdown**，EPUB 暂缓
- **AI**：默认 **DeepSeek 兼容接口**（`/v1/chat/completions`），base_url / model / api_key 都可在网页配置
- **网络端口**：选 **WebDAV**（不走 SMB：445 端口常被运营商/公司网封、SMB2 吃 RAM）
- **配网**：完全复刻小智那套 —— 开机即开热点 `PocketPlan-XXXX`，网页填 SSID/密码，凭据存 NVS

---

## 二、硬件事实（已实测确认）

| 项目 | 值 |
|---|---|
| 主控 | ESP32-S3 (QFN56) rev v0.2，双核 240MHz，16MB Flash，8MB PSRAM（octal 80MHz） |
| 屏幕 | **ST7701S**，480×640 RGB565 并口 + 三线 SPI 初始化 |
| 触摸 | **GT911**（I2C 地址 `0x14` 或 `0x5D`，由复位时 INT 电平决定，驱动会自动探测） |
| IO 扩展 | **TCA9554PWR @ 0x20**（8 个脚全部用满） |
| RTC | PCF85063 @ 0x51 |
| IMU | QMI8658 @ 0x6B |
| SD 卡 | SDMMC **1-bit**（D0=GPIO42, CMD=GPIO1, CLK=GPIO2, D3/CS 由 TCA9554 的 P3 拉高使能） |
| 蜂鸣器 | TCA9554 P7，**高电平响**（必须保持低） |

### 2.1 引脚全表

```
I2C(SD/TP/RTC/IMU 共用)  SDA=GPIO15   SCL=GPIO7
显示屏三线 SPI           SDA=GPIO1     SCL=GPIO2     CS=TCA9554 P2
显示屏 RGB 并口          PCLK=41  DE=40  VSYNC=39  HSYNC=38
  DATA0..15 = 5,45,48,47,21,14,13,12,11,10,9,46,3,8,18,17   (B1..B5,G0..G5,R1..R5)
背光                     GPIO6（LEDC 20kHz / 10bit）
触摸                     INT=GPIO16    RST=TCA9554 P1
屏复位                   TCA9554 P0
SD 卡座                  CLK=2  CMD=1  D0=42   D3/CS=TCA9554 P3
蜂鸣器                   TCA9554 P7（高电平响）
BOOT 按键                GPIO0（长按可清除 WiFi 凭据）
```
> TCA9554 的 `P0..P7` 对应代码里的位掩码 `1<<0 .. 1<<7`。

---

## 三、当前进度总览

```
[x] M0  板子点亮、硬件全验证（屏幕/触摸/RTC/IMU/SD 逐个跑通）
[x] M0.5 顺手把 xiaozhi 语音助手完整跑起来并激活（作为"这块板子确实能用"的证明）
[x] M1  PocketPlan 骨架：屏幕 UI + SD 挂载 + WiFi 配网 + WebDAV 文件服务
[x] M2  可导航文件列表（点文件夹进入 / 返回上级 / 分页 24 条）
[x] M3  电子书阅读器（TXT/MD，UTF-8/GBK/UTF-16 自动识别，流式分页，记忆位置）
[x] M4  音乐播放器"界面"（列出音乐、封面位；出声要等 PCM5102A 焊上）
[x] M5  计划列表 + 每日打卡（plans.json 存 SPIFFS，屏幕与网页双端一致）
[x] M6  网页端编辑界面（内置单页应用，计划 CRUD/打卡/文件管理/AI/设置）
[~] M7  WebDAV 打磨（PC 端 PUT/GET/MKCOL/DELETE 全通；Windows 映射与断线重连待实测）
[x] M8  AI 文字对话（DeepSeek 兼容 /v1/chat/completions，Key 网页配置）
[x] M9  设置页（亮度、字号、行距、时区、网络与存储状态、重启）
[~] M10 收尾（字体已自建 1bpp 全字库；剩 OTA、长时间稳定性、外壳）
[x] M11 专注模式（横屏全屏时钟）+ 倒数日（第 6 个 tab + 网页增删改）
[x] M12 体验补齐（闹钟蜂鸣器 / 专注暗色 + 倒数日联动 / 计划·倒数·闹钟·AI 历史可存 SD 卡 / 网页打卡乐观更新 / 键盘符号 / AI 气泡排版）
[x] M12.1 REST handler 槽位扩容（32 → 48，修 WebDAV HEAD/GET 被挤掉）
[x] M13 三项修正（打卡圈纯黑无白边 / 「时钟模式」只有时间+倒数日 / 音乐 tab 先显示「读取中」再异步分批建行，6.2s → 0.7–1.5s）
[x] M13.1 时钟模式改横屏（640×480）+ 显示下一个闹钟 + 去掉退出提示改为轻触 3 下退出；WiFi 凭据竞争修复（不再出现 sta is connecting / cannot set config）
[x] M14 自动休眠（无操作 N 分钟关背光，轻触唤醒；闹钟响铃与时钟模式期间不熄屏；设置页 chip + 网页下拉）
[x] M14.1 开源发布（README / MIT LICENSE / .gitignore；github.com/MGXD-cheng/PocketPlan）
[x] M14.2 滑块纯黑（设备端显式样式 + 网页 `accent-color:#000`）与 README 图库补齐
```

### 3.1 已实测通过的清单（M1 + M3–M9，2026-10-23 22:1x）

| 能力 | 证据 |
|---|---|
| 屏幕点亮、色彩正确 | 纯色测试白/红/绿/蓝/黑全部正确 |
| 触摸可用 | `GT911 at 0x5D`、`TouchPad_ID:0x39,0x31,0x31`、点击行有响应 |
| SD 卡挂载 | 7681MB（exFAT），根目录 120 项，中文名正常 |
| WiFi 热点配网 | 热点 `PocketPlan-2F79` @ `192.168.4.1`，网页填 SSID/密码 → 存 NVS |
| WiFi 联网 | `station got ip 192.168.0.106`（STA 与 AP 同时在线，约 4.8s 拿到 IP） |
| SNTP 对时 | `clock: RTC updated from SNTP`，`/api/status` 的 time 与 PC 一致，并回写 PCF85063 |
| WebDAV | `OPTIONS/PROPFIND/PUT/GET/MKCOL/DELETE` 全部正确（见 §6） |
| 内置网页 | `GET /` → 200 单页应用（M11 时 25,391 字节；M12 加到 **33,002 字节**，多了闹钟编辑、专注设置、数据存卡） |
| REST API | `/api/status`、`/api/plans`(GET/POST/DELETE)、`/api/checkin`、`/api/config`、`/api/files`、`/api/ai`、`/api/wifi` 全部按预期返回 |
| 计划与打卡 | 新建 → `{"ok":true,"id":"p4"}`；按 id 更新不改 id；打卡 → `{"ok":true,"done":true}`；删除 → `{"ok":true}` |
| 阅读器 | 14,147,359 字节的 `/sdcard/诡秘之主.txt` 流式打开，连续翻页 `off 0→469→1277→2385→3286`，全程内部 RAM 稳定在 ~119KB |
| 内部 RAM 余量 | 各页建完仍余 ~124KB（见 §7 第 9、10 条的两个关键修复） |
| 中文字体 | 自制 1bpp SimHei 全字库（16px UI / 20px 阅读器），21,318 字形，屏幕实测无缺字、无灰边（见 §3.3） |
| 文字观感 | 全界面纯黑 #000000 + 白底 + 浅灰描边；1bpp 点阵，无抗锯齿灰边 |
| 倒数日 | 第 2 个 tab「倒数」：种子「元旦 69 天 / 国庆节 342 天」，按剩余天数升序；网页端 /api/countdowns 增改删全通（中文往返一致、非法日期 400、列表满 507） |
| 专注模式 | 设置页「进入专注模式」→ 640×480 横屏全屏：96px 时钟 + 日期星期 + 已专注计时；点屏幕任意处退出，原页面（含 tab 状态）原样恢复 |
| 稳定性 | WiFi 断线指数退避 3→6→12→24→48→60s（拿到 IP 复位）；每 60s 打印内部堆/最大块/PSRAM/SPIFFS，低于 32KB 或最大块低于 16KB 告警 |

### 3.2 分页与性能实测

| 场景 | 耗时 |
|---|---|
| 开机到 `ready`（UI + WiFi + WebDAV 全就绪） | **3.5 s** |
| 根目录扫描 120 项（`opendir` + `stat`） | 130 ms |
| 文件页首次构建（**24 条/页**，分页后） | 190–330 ms |
| 文件页构建（分页前，一次建 120 行） | **8 000 ms**（LVGL 对象创建随兄弟节点数超线性增长，见 §7 第 11 条） |
| 音乐页扫 115 首（`opendir` + `stat`，冷 / 热） | 681 ms / 205 ms |
| 音乐页建 115 行（首版 4 对象/行） | **6 251 ms** |
| 音乐页建 115 行（终版：单 label 行 + 建行期间隐藏列表 + 每 20ms 建 40 行） | **675–1 454 ms**（见 §3.6） |
### 3.3 字体与显示（2026-10-23 23:5x，本轮重点）

| 项 | 结论 |
|---|---|
| 缺字的真因 | 所有页面**从未设置字体**，全局吃 LVGL 默认 Montserrat 14（**零 CJK 字形**）；`CONFIG_LV_FONT_SOURCE_HAN_SANS_SC_14/16_CJK` 是官方**子集**字体，只有一千来个随机常用字，而且只有阅读器在用 |
| 新字体 | `lv_font_conv` + `C:/Windows/Fonts/simhei.ttf` 生成 **1bpp**（无抗锯齿）点阵：`pp_font_16`（行高 18）给 UI、`pp_font_20`（行高 21）给阅读器，各 21,318 字形 |
| 覆盖区间 | `0x20-0x7E`、`0x00A0-0x00FF`（含中文间隔点 U+00B7）、`0x2010-0x2027`、`0x3000-0x303F`、`0x4E00-0x9FA5`（汉字基本区全集）、`0xFF00-0xFF5E` + FontAwesome 图标区间 |
| 体积 | 源文件 pp_font_16.c 6.4MB / pp_font_20.c 8.3MB；固件净增约 1.6MB，同时关掉两个 Source Han 子集（2.2MB），**净省约 600KB** |
| 全局应用 | `ui.c` 的 `pp_ui_init()` 里 `lv_theme_default_init(disp, 黑, 黑, false, &pp_font_16)` + `lv_display_set_theme()`，一次覆盖所有控件（含 LVGL 键盘、列表、滑块） |
| 阅读器字号 | 设置页「字号 16 / 20」→ `reader_font()` 返回 `pp_font_16` / `pp_font_20`；config 里旧的 14 一律按 16 载入 |
| 颜色 | 顶栏由蓝底白字改**白底黑字 + 1px 灰线**；正文/标题/状态/图标全 `#000000`；卡片与选中态用 `#E4E4E4` 底、`#C8C8C8`/`#D0D0D0` 描边；toast 浅灰底黑边黑字 |
| 验证方式 | 新增 `GET /api/screenshot`（480x640 1bpp PBM，38,411 字节）与开机自检 tour（串口 base64 输出 PBM），PC 端 `decodetour.py` 解成 PNG 逐页核对 |

阅读器 16px 与 20px 两档都实机截图确认：中文不缺字、笔画为纯黑点阵、没有抗锯齿灰边。

真机截图（1bpp PBM 转 PNG，存在 `pocketplan/docs/screenshots/`）：

| 文件 | 内容 |
|---|---|
| `01-plans.png` | 计划页（3 条计划、打卡圆圈、中文间隔点） |
| `02-files.png` | 文件页（实心文件夹/文档/音符图标、分页 1/5） |
| `03-music.png` | 音乐页（PCM5102A 未接线提示） |
| `04-ai.png` | AI 页（欢迎气泡整段换行、输入框、发送按钮） |
| `05-settings.png` | 设置页（字号 16/20、行距与亮度滑块、网络状态） |
| `06-reader-16.png` | 阅读器 16px（《诡秘之主》第一章） |
| `07-reader-20.png` | 阅读器 20px（同位置，字号切到大档） |
| `08-countdown.png` | 倒数页（元旦 69 天 / 国庆节 342 天，badge 大数字） |
| `09-focus-landscape.png` | 专注模式（640×480 横屏，96px 时钟 + 日期 + 已专注计时） |
| `10-settings-focus.png` | 设置页新增「专注」区块 |
| `11-countdown-alarms.png` | 倒数页新增「闹钟」区块（01:19 喝水 · 每天 + 「开」chip） |
| `12-ai-keyboard.png` | AI 页弹出 LVGL 键盘（Enter / Backspace / 地球 / 箭头 / 对勾全部可见） |
| `13-alarm-ring.png` | 闹钟响铃全屏浮层（闹钟 / 01:32 / 起床 / 停止） |
| `14-focus-dark.png` | 暗色专注模式（黑底白字 + 「元旦 · 还有 69 天」） |
| `15-plans-checked-circle.png` | 打卡圈纯黑：完成＝纯黑实心圆＋白勾，未完成＝黑空心圈（M13） |
| `16-music-library.png` | 音乐页 115 首（状态「共 115 首」，每行 ♪ 名字 + 文件大小两行）（M13） |
| `18-clock-mode-landscape-dark.png` | 时钟模式**横屏**暗色（640×480：时间 + 日期 + 下一个闹钟 + 3 条倒数日，无退出提示）（M13.1） |
| `19-clock-mode-landscape-light.png` | 时钟模式整屏亮色（同布局白底黑字）（M13.1） |
| `17-settings-modes.png` | 设置页「专注」区块：进入专注模式 / 时钟模式 两个黑框 chip + 暗色时钟（裁切放大）（M13） |

### 3.4 专注模式与倒数日（2026-10-24 00:2x，本轮新增）

| 项 | 结论 |
|---|---|
| 倒数日数据 | `/lfs/countdowns.json`（`{"version":1,"items":[{id,title,date,note,yearly}]}`，最多 24 条），仿 plans.c 的互斥锁 + 原子写；首次启动种两条（元旦、国庆节，`yearly:true`） |
| 剩余天数 | `days_left = date_days(date) - date_days(today)`；`yearly` 的条目自动滚到今年/明年，所以元旦永远显示「还有 N 天」而不是负数 |
| 倒数页 UI | 第 2 个 tab（铃铛图标 U+F0F3）：卡片左侧 badge 用 32px 数字（今天/明天/`N 天`/已过），右侧标题 + 日期；7 天内黑底反白，已过置灰；按剩余天数升序，已过排最后 |
| 设备端只读 | 屏幕没有文字输入（拼音键盘不划算），新增/修改/删除都在网页端 `/api/countdowns` 完成，设备页只负责展示与刷新 |
| 专注模式入口 | 设置页「专注」区块 → 「进入专注模式」按钮；也可 `POST /api/focus {"on":true}` 远程进入 |
| 横屏实现 | **没有**用 `lv_display_set_rotation()`（RGB direct-mode 需要 `LV_DRAW_TRANSFORM_USE_MATRIX` + 一整屏 640×480×2 的转换层，约 600KB，还要改 sdkconfig 全量重编）；改成**只对几个 label 做 90° `transform_rotation`**，坐标按 `ox=-(v-240), oy=(u-320)` 映射 |
| 持握方向 | `ROT=900`（顺时针）时画面需把设备**逆时针转 90°**（顶边朝左）才是正的 |
| 计时 | `lv_timer` 每秒刷新：`%H:%M` 大时钟、`%d月%d日 周X`、已专注（<1h 显示 `分:秒`，≥1h 显示 `时:分:秒`） |
| 退出 | 点屏幕任意处退出（不用右上角小叉，规避旋转后点击坐标映射的风险）；`lv_async_call` 延迟销毁，避免在自身事件里删对象 |

### 3.5 六项体验补齐（2026-10-24 01:5x，本轮新增）

用户这一轮给的是六条「用起来别扭」的具体痛点，逐条对应到实现：

| # | 用户原话 | 落地方案 | 关键实现 |
|---|---|---|---|
| 1 | 网页上计划勾选的反馈不及时 | **乐观更新**：点下去先切 UI，请求失败再回滚 | `index.html` 的 `checkin(id, done, date, el)` 先 `markCheck(el, done)` 并就地改「完成 N/M」计数，`fetch` 返回非 ok 或超时才反向回滚并 toast |
| 2 | AI 的对话排版有问题 | 气泡宽度**统一量宽 + 显式设宽** | `page_ai.c` 的 `fit_bubble(label, bubble, text)`：bubble = `w + 2*10 + 2*1`，用户/助手/占位气泡走同一套；`reply_done()` 回填后重算宽度；`plain_text()` 去掉 `*` 与反引号；`text_line_space 4` |
| 3 | 键盘上的符号不可见 | 字体补 **FontAwesome 段** | `LV_SYMBOL_NEW_LINE` = U+F8A2、`BACKSPACE` = U+F55A 等都在 `0xF8A0-0xF8FF`；`.fontgen/gen2.sh` 追加 `-r 0xF8A0-0xF8FF` 重生成 `pp_font_16`/`pp_font_20` |
| 4 | 可在网页端选择倒数日显示在专注模式 / 专注模式支持暗色 | config 加 `focus_cd` + `focus_dark` | `page_focus.c` 重写：按 `focus_cd` 找倒数日显示 `<title> · 还有 N 天/明天/就是今天/已过 N 天`（无选定时不占位）；`s_fg` 按 `focus_dark` 取白/黑，背景取反 |
| 5 | AI 对话、计划、倒数日等信息可以缓存进 TF 卡，设置里能开关 | `cache_sd` 开关 + 统一数据层 | `store.c` 新增 `pp_data_read/pp_data_write/pp_data_path/pp_data_on_sd`；plans/countdowns/alarms/ai_chat 四个文件全走这一层；开关切换后 `pp_store_migrate_data()` 迁移并重启 |
| 6 | 增加闹钟功能（可与倒数 tab 合并，用蜂鸣器提示） | 倒数 tab 内「闹钟」区块 + 全屏响铃浮层 | 新文件 `alarms.c`：`alarm_task` 每秒匹配 `%H:%M` + 周几位，`ring_task` 350ms 开 / 300ms 关驱动 TCA9554 P7 蜂鸣器，60s 自动停 |

**几个必须记住的取舍**

- 蜂鸣器在 **TCA9554 P7，只有开/关两位**，没有频率与音量 —— 所以响铃只能是「嘀嘀」节奏，不是铃声；想换音色得换硬件。
- 数据存 SD 卡是**整目录迁移**：`/lfs/{plans,countdowns,alarms,ai_chat}.json` → `/sdcard/.pocketplan/`，迁移完保留新位置、删掉旧文件；开关开着但卡不在时 `pp_data_write` 返回 `ESP_ERR_INVALID_STATE` 并让 UI 提示，**绝不静默写回 /lfs**（否则同一份数据会在两个位置分裂）。
- 闹钟去重键是 `"<日期> <HH:MM> <id>"`，所以同一分钟内不会重复响，改时间或改星期也不用清状态。
- 倒数 tab 现在是**两段式**：上半「闹钟」列表、下半「倒数日」列表；设备端闹钟只能开关，时间/星期/标签都在网页端改（屏幕没有中文输入）。

**容量**：REST handler 从 18 条涨到 **23 条**（`/api/alarms` GET/POST/DELETE、`/api/alarms/stop`、`/api/ai` GET 回放历史），加上 WebDAV 的 12 条共 35 条，因此 `webdav.c` 的 `cfg.max_uri_handlers` 从 32 提到 **48**（详见 §七 第 27 条）。

### 3.6 M13 三项修正（2026-10-24 02:5x，本轮新增）

用户这一轮提的是三个「用起来别扭」的点，都是纯观感/等待问题：

| # | 用户原话 | 落地方案 | 关键实现 |
|---|---|---|---|
| 1 | 计划中勾选完成的圈圈有白边 | 完成＝**纯黑实心圆**，不再有描边 | `page_home.c` 的 `build_card()`：done 时 `bg=0x000000` + `border_width 0` + 白勾；未完成改成 `border_color 0x000000` 黑空心圈（原来用 `0xC8C8C8`：白底上极浅，截图里直接消失，真机上也几乎分不开，反而像「没有圈」） |
| 2 | 加一个时钟模式（只有时间和倒数日） | 新文件 `clockmode.c`：全屏 overlay（M13 竖屏 → **M13.1 改横屏**，见 §3.6.1） | 时间 `pp_font_96` + 日期 + 下一个闹钟 + 最多 4 条倒数日（`%s · 还有 N 天` / `· 就是今天` / `· 已过 N 天`）；`lv_timer` 1s 刷新，跨日自动重算；颜色跟随 `focus_dark` |
| 3 | 音乐的 tab 每次读取时间长，改为先进入 tab 显示读取中，读取后显示所有文件 | 扫描与建行**全部异步 + 分批** | `page_music.c` 重写：`scan_task` 扫目录 → `lv_async_call(begin_render)` → `lv_timer` 每 20ms 建 40 行；状态行依次显示「读取中…」→「读取中 N/115」→「共 115 首」 |

**音乐页为什么从 6.2s 降到 0.7–1.5s（这一轮最有价值的实测）**

| 版本 | 115 行建完耗时 |
|---|---|
| ① container + icon + 名字 + 大小（4 对象/行，460 个对象），每批 8 行 | 6 251 ms |
| ② 改成单 label 行（`♪ 名字\n11.3 MB`，115 个对象） | 1 671 ms（重建 7 238 ms） |
| ③ 再改手工定位 `set_pos/set_size` + 列表 `LV_LAYOUT_NONE` | 2 023 ms（重建 6 967 ms） |
| ④ **建行期间把列表 `LV_OBJ_FLAG_HIDDEN`，建完再显示** | 1 356 / 1 941 / 1 999 ms |
| ⑤ ④ + 每批 40 行（每 20ms 一批）→ **最终版本** | **675 / 1 454 ms** |

- 真正的成本**不是** `opendir/stat`（115 个文件只要 681ms，热缓存 205ms），而是 **LVGL 每 `add_obj` 一行都会让整块列表区域失效并重绘**，重绘成本随已有行数线性增长 → 总成本 O(n²)。
- 隐藏列表后单行成本变成常数：插桩显示 `row 0: create 2214us / row 114: create 2037us`（隐藏前是 5406us → 随索引暴涨）。
- 行改单 label 后对象数 460 → 115；行高必须 `ROW_H 64` + `pad 7` + `line_space 2`，否则第二行（文件大小）会被 `LV_LABEL_LONG_DOT` 截成「…」。
- **浅灰在纯黑白 UI 里等于没有**：这一轮顺带把设置页所有 chip 的 `C_CHIP 0xE4E4E4` / `C_EDGE 0xC8C8C8` 全改成 `0x000000`（白底黑框，选中＝黑底白字），否则 chip 在白底上看起来只是文字，用户找不到「时钟模式」按钮。（截图本身是 1bpp：`shot.c` 用中灰阈值取墨，浅灰直接算白，所以截图能放大这个问题。）

**时钟模式与专注模式的区别（两个入口并排在设置页「专注」区块）**

| | 专注模式 | 时钟模式（M13.1） |
|---|---|---|
| 方向 | 横屏 640×480（每个 label 90° transform） | 横屏 640×480（同一套旋转技巧） |
| 内容 | 大时钟 + 日期 + **已专注计时** + 可选一条倒数日 | 大时钟 + 日期 + **下一个闹钟** + **前 4 条倒数日**（没有计时） |
| 用途 | 学习/工作时摆在旁边看时间与专注时长 | 立在桌上当台钟/倒数牌 |
| 退出 | 点屏幕任意处（屏幕上有「轻触屏幕退出」提示） | **屏幕不显示任何退出提示**，2.5s 内轻触 3 下退出 |

#### 3.6.1 M13.1：时钟模式改横屏 + 下一个闹钟 + 三击退出（2026-10-24 03:0x）

用户这一轮四句话拆成四个改动（原话：m03493「时钟模式支持要横屏，不显示退出方式，还要显示下一个闹钟时间，退出方式改为轻触屏幕3下」）：

| 用户要求 | 落地 |
|---|---|
| 时钟模式要横屏 | 面板是竖屏 480×640 且工作于 RGB 直连模式（不支持 `lv_display_set_rotation`），所以复用专注模式那套「按 640×480 排版、每个 label 用 `transform_rotation 900` 转过来」的做法：`place(obj, u, v)` 里 `ox = -(v - 480/2)`、`oy = (u - 640/2)`，再 `lv_obj_align(obj, LV_ALIGN_CENTER, ox, oy)` |
| 不显示退出方式 | 删掉原来的「轻触屏幕退出」label；屏幕只剩时间 / 日期 / 闹钟 / 倒数日 |
| 显示下一个闹钟时间 | `next_alarm_text()`：取本地 `HH:MM` 与「周几（周一=0）」，遍历启用中的闹钟，对每个闹钟向前找 0–7 天内第一个匹配 `days` 掩码的日子（当天必须还没到点），取全局最早的一个；文案分三档：`闹钟 07:30 · 起床` / `闹钟 明天 07:30 · 起床` / `闹钟 周一 07:30 · 起床`；没有启用的闹钟就整行隐藏 |
| 退出改为轻触 3 下 | `root_clicked()` 用 `lv_tick_get()` 计数：第一次点击或距上次超过 2.5s 就重置为 1，累计到 3 就 `lv_async_call(close_async, NULL)`（**不能在自己的事件回调里删自己**），由下一轮 LVGL 循环把 overlay 删掉 |

- 布局（横屏坐标）：时钟 `v=132`（`pp_font_96`）、日期 `v=204`、闹钟 `v=242`、倒数日 `v=292/324/356/388`（最多 4 行，行距 32，20px 字）。
- 倒数日改成 4 个**常驻 label**（`s_cd[MAX_LINES]`，空的加 `LV_OBJ_FLAG_HIDDEN`），不再用 flex 容器：横屏下旋转变换 + flex 的测量容易错位，手排更稳。
- 每秒 `update()` 顺带 `update_alarm()`，所以跨过闹钟时间点后这一行会自动跳到「明天」。
- 真机截图：`docs/screenshots/18-clock-mode-landscape-dark.png`、`19-clock-mode-landscape-light.png`（都是 640×480，已转正）；M13 的竖屏版两张已删除，截图编号因此连续到 19。
- 三击退出用**真实事件**验证过：tour 里 `lv_obj_send_event(root, LV_EVENT_CLICKED, NULL)` 连发 3 次，600ms 后 `pp_clockmode_active()` 返回 0，串口打出 `---TOURTAPEXIT active=0---`。这条很关键——时钟模式屏幕上没有任何按钮，三击失效等于把设备锁死在时钟页。


### 3.7 M14 自动休眠（2026-10-24 03:3x，本轮新增）

用户原话（m03777）：「加上自动休眠功能，可在设置中开关」。需求很小，真正要想清楚的是**「空闲」从哪里来**。

| 环节 | 落地方案 |
|---|---|
| 判空闲 | 直接用 LVGL 的 **inactivity clock**：每个 indev 的 read 回调都会给 `disp->last_activity_time` 打戳，`lv_display_get_inactive_time(NULL)` 就是「多久没人碰屏幕」。自己记 tick 会漏掉键盘/滚轮/未来新增的输入设备，也会漏掉「界面自己滚动」与「人真的在操作」的差别 |
| 熄屏 | `pp_board_backlight(0)`（LEDC duty=0），面板不停、LVGL 不停、WiFi 不停 |
| 唤醒 | 发现 inactivity < 700ms（`WAKE_WINDOW_MS`）就恢复 `cfg->brightness`。**唤醒不需要额外事件**：人一碰，LVGL 的时钟自己归零 |
| 轮询 | `sleep_task`（栈 3072 / 优先级 2）每 500ms 看一次，读时钟前 `lvgl_port_lock(100)` |
| 不熄屏的例外 | `pp_alarms_ringing()`（响铃必须看得见）与 `pp_clockmode_active()`（时钟模式是「常亮台钟」，熄屏就没意义） |
| 设置项 | `config.json` 新增 `auto_sleep`（bool，默认 true）与 `sleep_min`（1..60，默认 5）。设置页「屏幕」区块两行 chip：开/关 + 每 1/3/5/10/30 分；网页端「自动休眠」卡片是复选框 + 下拉 |
| REST | `/api/status` 多一个 `asleep`；`/api/config` GET/POST 收发 `auto_sleep`/`sleep_min` |

**三个必须记住的点**

- **熄屏 ≠ 省电模式**：只关背光（面板 0.2W 左右），CPU、PSRAM、WiFi、闹钟任务全都照常跑。要做真省电得进 light sleep，但那样 WiFi 掉线、闹钟要靠定时器唤醒，复杂度翻几倍 —— 这版选的是「一碰就亮」而不是「一躺十天」。
- **网页保存设置时可能正处在熄屏状态**：`h_config_post` 里保存完如果发现 `!pp_autosleep_active()` 就顺手把背光按新亮度点回去，否则用户在网页上改完亮度，设备仍是黑屏，会以为设置没生效。
- **tour 固件必须临时关掉自动休眠**：tour 全程不碰屏幕，5 分钟后屏幕自己就黑了，16 块截图会全是黑的。所以 `shot.c` 的 tour 在开头把 `auto_sleep` 置 false、结尾再存回用户的值（这一条踩过一次，见 §七 第 44 条）。

### 3.8 M14.1 开源发布（github.com/MGXD-cheng/PocketPlan）

| 项 | 内容 |
|---|---|
| 仓库 | <https://github.com/MGXD-cheng/PocketPlan>（public，MIT） |
| 入库内容 | 102 个文件：`pocketplan/` 全部源码 + 4 个字体 `.c` + `sdkconfig`/`sdkconfig.defaults`/`partitions.csv`/`dependencies.lock`、19 张真机截图、`tools/` 里的构建/烧录/串口脚本、`.fontgen/` 字体生成脚本、三份报告 + 设计文档、README 与 LICENSE |
| 刻意不入库 | `pocketplan/build`（约 234MB）、`pocketplan/managed_components`（184MB）、`*.bin/*.elf/*.map`、所有串口日志与临时截图、早期实验目录（`case/ demo/ idf/ i2cscan/ ref/ ref2/ sdtest/ xz/`）、早期下载的 `tools/arduino-cli.exe`/`.zip`（55MB，与本项目无关） |
| README | 中文：一句话简介 + 4 张主图 + 9 张图库表 + 功能表 12 行 + 硬件表（含引脚归属）+ 编译/烧录/首次使用 + 目录结构 + REST API 表 + 已知限制 + 许可与致谢 |
| 许可证 | MIT（`Copyright (c) 2026 PocketPlan contributors`），第三方组件（ESP-IDF / LVGL / esp_lvgl_port / cJSON / esp_lcd 驱动）各自遵循原许可 |

**一个环境事实（写下来免得下次再查）**：这台机器所在网络 **`git push` 到 `github.com:443` 直接连不上**（`github.com` 被 DNS 解析到 `20.205.243.166`，该 IP 无响应；而 `api.github.com`、`codeload.github.com` 以及 `github.com` 的 `140.82.112.3` 等 IP 都是通的）。所以首次发布是**走 GitHub REST 的 Git Data API** 完成的：`POST /git/blobs`（102 个，base64）→ `POST /git/trees` → `POST /git/commits` → `PATCH /git/refs/heads/main`（force）。本地 `git` 仓库与远端是同一份内容，但 commit sha 不同（作者时间戳不同）；下次有网时 `git push -f origin main` 或直接重新 clone 覆盖即可对齐。


### 3.9 M14.2 滑块纯黑 + README 图库（2026-10-24 03:4x，本轮新增）

用户这一轮的原话是「设置页主题色要与前几页一致，github开源记得放图片」，追问后确认要改的是**滑块**：「设置页的滑块条是紫色的，改为黑色的」。

| 位置 | 改动 |
|---|---|
| 设备端 | `pocketplan/main/page_settings.c` 新增 `style_slider()`：行距、亮度两个 `lv_slider` 的 MAIN / INDICATOR / KNOB 三部分全部显式钉成纯黑（白底 + 2px 黑描边轨道、黑色填充、黑色圆钮），不再依赖主题 |
| 设备端取证 | `style_slider()` 覆盖前先打印主题给的颜色：`settings: slider theme colour before override: ind=#ff000000 knob=#ff000000`（两次，两个滑块）。**说明设备端本来就是黑的**，紫色的来源不在这块屏上 |
| 网页端 | `pocketplan/main/www/index.html` 给 `input[type=range]` 加 `accent-color:#000`。浏览器（Chrome/Android/Material）对未指定 accent 的 range 用的就是默认靛紫，这才是「紫色滑块条」的真正出处 |
| README | 顶部 4 张主图之外新增 9 张一组的图库表（计划打卡 / 阅读器 / 倒数日+闹钟 / 音乐列表 / AI+键盘 / 设置 / 时钟模式 / 闹钟响铃 / 专注模式），指向 `pocketplan/docs/screenshots` |

**为什么先怀疑错对象**：设备截图是 `shot.c` 的 1bpp PBM（按中灰阈值判墨），**任何颜色在截图上都会变成黑或白**，所以「截图里滑块是黑的」证明不了屏上是黑的；反过来也证明不了是紫的。真正把这件事定死的是上面那行日志——它读的是 LVGL 解析后的样式值。

---

## 四、代码结构

```
D:\Users\PC\Desktop\esp32-mgxd\pocketplan\
├─ CMakeLists.txt                顶层工程
├─ partitions.csv                分区表：nvs/otadata/phy/ota_0/ota_1/storage(SPIFFS 1MB @0x800000)
├─ sdkconfig.defaults            默认配置（PSRAM / FATFS / LVGL / HTTPD / SPIFFS …）
├─ main\
│  ├─ CMakeLists.txt             SRCS = 26 个 .c + 4 个字体 .c，EMBED_FILES "www/index.html"
│  ├─ idf_component.yml          组件依赖（st7701 / gt911 / tca9554 / lvgl 9.5 / lvgl_port 2.9 / cjson）
│  ├─ app_main.c                 启动流程 + BOOT 长按清 WiFi 凭据
│  ├─ board.c                    背光 / I2C / TCA9554 / ST7701 RGB / GT911 / LVGL(8KB 任务栈) / SD
│  ├─ ui.c                       外壳：标题栏 + 6 个底部 tab + toast + 懒创建页面
│  ├─ page_home.c                计划卡片列表 + 圆形打卡按钮（完成＝纯黑实心圆+白勾，未完成＝黑空心圈）
│  ├─ page_countdown.c           倒数日卡片（badge 大数字、7 天内反白、已过置灰）+ 闹钟区块（开关 chip）
│  ├─ page_focus.c               专注模式：640×480 横屏全屏时钟（每个 label 90° transform）+ 可暗色 + 联动倒数日
│  ├─ clockmode.c               时钟模式：横屏全屏，只有时间 + 日期 + 下一个闹钟 + 前 4 条倒数日（颜色跟随 focus_dark，轻触 3 下退出）
│  ├─ page_files.c               SD 浏览器（分页 24 条/页、返回上级、文本文件进阅读器）
│  ├─ page_reader.c              全屏阅读器（点左侧 1/3 上一页，右侧下一页）
│  ├─ page_music.c               音乐列表（扫描与建行全异步分批：状态「读取中 N/115」→「共 N 首」；PCM5102A 未焊时给橙提示）
│  ├─ page_ai.c                  聊天界面（气泡 + LVGL 键盘 + 异步请求）
│  ├─ page_settings.c            亮度 / 字号 / 行距 / 专注与时钟模式入口（暗色 + 显示哪个倒数）/ 网络 / 数据存 SD 卡 / 重启
│  ├─ reader.c                   流式分页引擎（8KB 行缓冲 + 16KB 页缓冲，全走 PSRAM）
│  ├─ plans.c                    plans.json 读写、repeat 规则、打卡、streak（互斥锁保护）
│  ├─ countdowns.c               countdowns.json 读写、yearly 滚动、剩余天数排序（互斥锁保护）
│  ├─ alarms.c                   闹钟：alarms.json、每秒匹配、蜂鸣器节奏响铃、全屏停止浮层
│  ├─ config.c                   config.json（AI base/key/model/persona、时区、亮度、字号、focus_dark/focus_cd/cache_sd）
│  ├─ store.c                    SPIFFS 挂载在 /lfs + 原子写（tmp→fsync→rename）+ 可把数据文件迁到 /sdcard/.pocketplan
│  ├─ clock.c                    PCF85063 RTC + SNTP 对时回写 + pp_clock_today()
│  ├─ ai.c                       esp_http_client + crt_bundle 调 /chat/completions
│  ├─ webapi.c                   23 个 REST handler + 内置网页（含 /api/countdowns、/api/focus、/api/alarms*、/api/ai GET）
│  ├─ webdav.c                   WebDAV 通配 handler + /wifi 表单页（老的 HTML 文件页保留）
│  ├─ wifi.c                     APSTA 常开、热点配网、凭据 NVS、断线重连
│  ├─ gbk.c                      编码转换（FatFS 用 UTF-8 后已退化为直通）
│  ├─ fonts\pp_font_16.c         自制 1bpp SimHei 16px 全字库（21,318 字形，行高 18）
│  ├─ fonts\pp_font_20.c         同上 20px，阅读器大字号
│  ├─ fonts\pp_font_32.c         32px（倒数日 badge 的「N 天」「今天/明天/已过」）
│  ├─ fonts\pp_font_96.c         96px（专注模式大时钟，只有 0-9 与冒号 11 个字形）
│  ├─ shot.c                     屏幕快照：LVGL 全屏渲染 → 1bpp PBM（HTTP 或串口 base64 取出）
│  ├─ www\index.html             单页应用（33KB，计划/倒数+闹钟/文件/AI/设置 + 专注设置 + 数据存卡开关）
│  └─ include\                   board.h / ui.h / net.h / webdav.h / gbk.h / store.h / config.h / alarms.h / clockmode.h /
│                                shot.h / pp_fonts.h（LV_FONT_DECLARE 两个字号）/
│                                clock.h / plans.h / reader.h / pages.h / ai.h / webapi.h
└─ managed_components\           组件管理器自动拉取
```

### 4.1 启动顺序（app_main.c，顺序有讲究）

```
nvs_flash_init
  → pp_board_init()       背光→I2C→TCA9554→ST7701→LVGL→GT911（面板先起，它借用了 SD 的 GPIO1/2）
  → pp_board_sd_init()    挂载 /sdcard（20MHz 失败退 400kHz）
  → pp_store_init()       SPIFFS 挂到 /lfs（首次自动格式化）
  → pp_config_load() → pp_clock_init()（TZ + RTC）→ pp_plans_init() → pp_countdowns_init()
  → pp_net_start()        ★ 必须在 UI 之前：WiFi 要一大块连续内部 DMA RAM
  → pp_clock_start_sntp()
  → pp_ai_init()
  → pp_ui_init()          装全局主题（1bpp SimHei 16px）+ 外壳 + 只建当前 tab
  → pp_reader_ui_init()   阅读器 overlay（隐藏）
  → pp_webdav_start()     80 端口：先注册 /api 与 /，再注册 WebDAV 通配
  → status_task（每 60s 打印内部堆/最大块/PSRAM/SPIFFS，低于阈值告警）
  → button_task（BOOT 长按 3s 擦 WiFi 凭据后重启）
```

---

## 五、构建与烧录手册

### 5.1 环境（本机已装好，无需重装）

- ESP-IDF **v6.1.0** → `D:\esp\esp-idf-v6.1`
- 工具链与 python 虚拟环境 → `C:\Users\PC\.espressif`
- 封装脚本：`tools\pp_build.ps1`（编译）、`tools\pp_flash.ps1`（烧录）、`tools\pp_serial.ps1`（看日志）
- **旧的 `tools\idfenv.ps1` 已废弃**：它不处理 `MSYSTEM=UCRT64`，IDF 6.1 的 `activate.py` 会直接报 MSys/Mingw is not supported

### 5.2 编译（必须用 pp_build.ps1）

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
  "D:\Users\PC\Desktop\esp32-mgxd\tools\pp_build.ps1" `
  -Command "idf.py build" -Log build.log
```

脚本内部做的事（缺一步就编译失败）：`$env:MSYSTEM=$null` → 设 `IDF_PATH` / `IDF_TOOLS_PATH` / `PYTHONUTF8=1` → `. activate.py --export` → 执行传入的 `-Command`。

- 纯增量 **40–90 秒**；改了 `sdkconfig` 会触发全量重编，**约 15 分钟**
- 成功标志：`Project build complete.` + `pocketplan.bin binary size 0x379220 bytes. 0x76de0 bytes (12%) free.`（3,641,888 字节，app 分区 0x3f0000 = 4,128,768 字节，剩 486,880 字节）
- 编译**必须后台跑并轮询日志**（前台会超时）；日志是 UTF-16，用 `Select-String` 读，别用 `cat`

### 5.3 烧录

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
  "D:\Users\PC\Desktop\esp32-mgxd\tools\pp_flash.ps1"
```

等价于：

```powershell
& "C:\Users\PC\.espressif\python_env\idf6.1_py3.13_env\Scripts\python.exe" -m esptool `
  --chip esp32s3 -p COM5 -b 921600 --before default-reset --after hard-reset `
  write-flash 0x8000  "pocketplan\build\partition-table.bin" `
  write-flash 0x20000 "pocketplan\build\pocketplan.bin"
```

**分区表也要烧**（`0x8000`）：storage 分区已经从 256KB 改成 1MB，只烧 app 会导致 SPIFFS 挂载失败。不烧 NVS，WiFi 凭据与阅读进度都保留。

### 5.4 看串口日志（有坑，务必照做）

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
  "D:\Users\PC\Desktop\esp32-mgxd\tools\pp_serial.ps1" -Port COM5 -Seconds 30
```

- COM5 是 **USB-Serial-JTAG**：`DtrEnable=true` 会把芯片按进下载模式 → **必须 `DtrEnable=$false`**
- 打开串口本身会复位芯片；要完整启动日志就 `Open()` 后 `RtsEnable=$true` 350ms 再 `$false`
- 只想看实时输出不打断运行：加 `-NoReset`
- 供电/下载模式卡死时：**按住 BOOT → 拔插 Type-C → 松开 BOOT**
- 复位后 **等 4–6 秒** WiFi 才会重新拿到 IP，这期间 ping 不通是正常现象

### 5.5 纯色测试（怀疑屏幕/颜色时用）

`board.c` 里 `pp_board_init()` 末尾临时把面板刷成纯红即可判断背光与面板是否工作；测完记得删掉。

### 5.6 屏幕截图（改字体/配色后必用）

不用凑到屏幕跟前，直接把整屏取回来在 PC 上看：

- **HTTP 方式**：`curl -s -m 45 -o shot.pbm http://192.168.0.106/api/screenshot` → 480x640 1bpp PBM（38,411 字节）。PC 端 `python` + Pillow 读 PBM 即可转 PNG。
  - 注意 `CONFIG_LWIP_TCP_SND_BUF_DEFAULT/TCP_WND_DEFAULT = 5760`，往返 200ms 时吞吐只有 ~28KB/s；几百 KB 的大图会 `httpd_sock_err: error in send: 104`，所以截图刻意做成 1bpp。
- **串口方式（更稳）**：把 `pocketplan/main/shot.c` 里的 `#define PP_SCREEN_TOUR` 改回 `1` 重新编译烧录，开机 9 秒后自动逐页 tour，**共 16 段 dump**（6 个 tab + 音乐读取中 + 音乐完成 + 打卡完成态 + 键盘 + 闹钟响铃 + SD 缓存自检 + 专注 + 时钟模式暗/亮 + 阅读器 16px + 阅读器 20px），每页一个 PBM，**每个约 7 秒**（38KB → 51KB base64 @115200），**要 `-Seconds 165` 才走完全程**，每页把 PBM 以 base64 打到串口（分隔标记 `---TOURTAB n---` / `---TOURMUSICLOAD---` / `---TOURCLOCK---` / `---TOURCLOCKLIGHT---` / `---TOURREADER---` / `---TOURREADER20---` / `---TOUREND---`；旁证打印 `---TOURTAPEXIT active=0---`、`---TOURCACHE …---`、`---TOURALARM---`）。抓取与解码：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File 'tools/pp_serial.ps1' -Port COM5 -Seconds 100 > tour.log 2>&1
```

```bash
cd pocketplan && python decodetour.py ../tour32.log ../n32_   # 生成 ../n32_0.png … ../n32_15.png（16 块）
```

- `decodetour.py` 现在对丢字符的坏块是容错的：base64 自动补 `=` 到 4 的倍数、`ImageFile.LOAD_TRUNCATED_IMAGES = True`、逐块 try/except。串口 115200 下偶尔会截断一块（见 §七 第 42 条），跳过即可，不影响其余块。
- 抓完图记得把 `PP_SCREEN_TOUR` 改回 0 再编一次——**交付版必须是 `PP_SCREEN_TOUR 0`**（当前已是 0），否则每次开机都被自检占用几十秒屏幕。

---

## 六、验证记录（2026-10-23 22:1x 实机）

### 6.1 启动日志（节选）

```
I (2089) board: display ready
I (2210) board: SD card mounted
I (2240) wifi: AP 'PocketPlan-2F79' at http://192.168.4.1/
I (3099) ui: shell ready
I (3120) webapi: web app and API registered (13 handlers)
I (3140) webdav: WebDAV listening on port 80
I (3549) pocketplan: ready
I (4849) wifi: sta ip 192.168.0.106
I (4900) clock: RTC updated from SNTP
```

### 6.2 REST / 网页 API（PC curl → 192.168.0.106）

| 请求 | 结果 |
|---|---|
| `GET /` | 200，21,158 字节单页应用 |
| `GET /api/status` | sd / lfs / wifi / time 全正常，time 与 PC 一致（SNTP 生效） |
| `GET /api/plans` | 3 条种子计划 |
| `POST /api/plans` | `{"ok":true,"id":"p4"}`（新建）；带 id 再 POST 则原地更新，id 不变 |
| `POST /api/checkin` | `{"ok":true,"done":true}`（再打一次变 false，可撤销） |
| `DELETE /api/plans?id=p4` | `{"ok":true}` |
| `GET/POST /api/config` | 正常；`key_set` 是布尔，`ai_key` 永不回显 |
| `GET /api/files?path=/` | 120 项；`path=%2F`、`path=` 同样落到 /sdcard 根 |
| `POST /api/ai`（未配 Key） | `{"ok":false,"reply":"还没有配置 API Key。…"}` |
| `POST /api/wifi`（空 ssid） | `{"ok":false,"error":"ssid required"}` |

### 6.3 WebDAV 端到端

```
OPTIONS           -> 200   (DAV: 1,2,3 / MS-Author-Via: DAV)
PROPFIND Depth:1  -> 207   (返回 XML multistatus)
PUT pc-test.txt   -> 201
GET  pc-test.txt  -> 200   body = [hello webdav from pc]
MKCOL pc-dir      -> 201
DELETE pc-test.txt-> 204
DELETE pc-dir     -> 204
GET /             -> 200   21,158 字节 HTML 页面
```

中文文件名（修复编码后）：

```
/%E5%91%A8%E6%9D%B0%E4%BC%A6-%E4%B8%83%E9%87%8C%E9%A6%99.mp3   →  周杰伦-七里香.mp3
周杰伦-等你下课(with 杨瑞代).mp3
周杰伦-青花瓷.mp3
```

Windows 映射方式：资源管理器 →「映射网络驱动器」→ `http://192.168.0.106/`（需 WebClient 服务开启；若系统不允许 http 映射，用 `net use Z: http://192.168.0.106/` 或在浏览器里用内置网页操作）。

### 6.4 字体与 AI 页轮复测（2026-10-23 23:5x）

| 项 | 结果 |
|---|---|
| 启动 | `ui: shell ready` @3.26s、`pocketplan: ready` @3.70s、`net: station got ip 192.168.0.106` @4.70s，无 error |
| `GET /` | 200，21,158 字节 |
| `GET /api/screenshot` | 200，38,411 字节，PBM 头 `P4 / 480 640`，PC 端可直接转 PNG |
| `GET /api/plans` | 3 条（p1 Drink water / p2 Read 30 minutes / p3 Weekly review） |
| `POST /api/checkin` | `{id:p1,done:true}` → `{ok:true,done:true}`，plans 里 p1 `done_today=true, streak=1`；再发 `done:false` 复位 |
| `POST /api/ai`（未配 Key） | `{ok:false, reply:还没有配置 API Key…}`（中文提示，链路通） |
| WebDAV | `PUT /dv.txt` → 201、`GET` → 200 内容一致、`DELETE` → 204（`WWW_ROOT` 即根；`/dav/` 这种不存在的子目录返回 500/404 属预期） |
| 屏幕 | tour 截图逐页核对：5 个 tab + 阅读器 16px + 阅读器 20px，中文全部齐全、纯黑无灰边，AI 气泡整段可读、输入框可见 |

### 6.5 本轮复测（2026-10-24 00:2x，专注模式与倒数日）

| 项 | 结果 |
|---|---|
| 启动 | `display ready` @2.09s、`ui: shell ready` @3.26s、`pocketplan: ready` @3.76s、`net: station got ip 192.168.0.106` @4.78s，无 error，开机**不再**跑自检 tour |
| `GET /api/status` | 新增 `mem{internal_free:119879, internal_largest:31744, psram_free:7013836}` 与 `focus:false` |
| `GET /api/countdowns` | `{today:"2026-10-24", items:[c1 元旦 69 天、c2 国庆节 342 天]}` |
| `POST /api/countdowns` | `{"ok":true,"id":"c3"}`；中文标题/备注往返一致（`title == "体检"`、`note == "空腹抽血"` 断言通过） |
| `POST /api/countdowns`（带 id） | 原地更新，返回 `{"ok":true,"id":"c3"}`（**修了更新时 id 回空串的 bug**） |
| `POST /api/countdowns`（`date:2026-13-40`） | 400 Bad Request |
| `DELETE /api/countdowns?id=c3` | `{"ok":true}`，列表回到 2 条 |
| `POST /api/focus {on:true}` | `{"ok":true,"focus":true}`；`/api/status` 的 focus 同步为 true；截图确认 640×480 横屏时钟（`00:22`、`10月24日 周六`、`已专注 00:01`） |
| `POST /api/focus {on:false}` | `{"ok":true,"focus":false}`；截图确认回到计划页，tab 与内容原样 |
| 倒数页截图 | `08-countdown.png`：元旦 69 天、国庆节 342 天，badge 32px 数字 + 「天」 |
| 启动内存 | 6 个 tab 全建完后内部 RAM ~119.9KB（最大连续块 31.7KB），与 5 tab 版本基本持平（新页面只有 2 个卡片） |

### 6.6 板端 UI

- 文件页：`page_files: /sdcard: 120 entries, page 0, scan 128 ms, rows 331 ms`（分页后；分页前同一目录 rows 8000ms）
- 阅读器：`/sdcard/诡秘之主.txt` 14,147,359 字节，翻页 off 0 → 469 → 1277 → 2385 → 3286，内部 RAM 稳定 ~119KB
- 6 个 tab 全部建完后内部 RAM ~120KB，无 allocate_dma_buf 报错

### 6.7 六项需求真机复测（2026-10-24 01:3x，串口 tour 固件）

这一轮 PC 与板子不在同一网段（板子拿到 `192.168.10.104`，PC 在 `192.168.0.105`），HTTP 验证走不通，改用**串口 tour 截图 + 串口日志**逐项取证：

| 项 | 结果 |
|---|---|
| 启动 | `display ready` @2.09s、`plans: loaded 3 plans`、`alarms: loaded 1 alarms`、`ui: shell ready` @3.79s、`ready` @3.92s、`station got ip 192.168.10.104` @4.54s，无 error、无 `no slots left for registering handler` |
| 闹钟响铃 | `alarms: ringing for 01:32 起床`；截图 `13-alarm-ring.png` 全屏浮层「闹钟 / 01:32 / 起床 / 停止」 |
| 键盘符号 | 截图 `12-ai-keyboard.png`：Enter(↵)、Backspace(⌫)、键盘图标、左右箭头、确定勾全部可见（字体里 F8A2/F55A/F11C/F00C/F00D/F053 均在） |
| 暗色专注 | 截图 `14-focus-dark.png`：像素统计 98% 为黑（白字），内容「元旦 · 还有 69 天 / 01:32 / 10月24日 周六 / 已专注 00:02 / 轻触屏幕退出 / 专注」 |
| 数据缓存 SD | tour 自检打印 `---TOURCACHE on_sd=1 mig=ESP_OK file=1 plans=3/3 back=ESP_OK lfs=1---`，日志 `store: moved 3 data file(s) to /sdcard/.pocketplan` 与 `moved 3 data file(s) to /lfs`（迁过去再迁回来，plans 3/3 完好） |
| 倒数页 | 截图 `11-countdown-alarms.png`：闹钟行「01:19 喝水 · 每天」+ 黑底「开」chip；元旦 69 天 / 高考 226 天 / 国庆节 342 天 |
| 内存 | 无回退：各页建完内部 RAM 仍 ~124KB，响铃浮层与键盘同时显示期间无 `allocate_dma_buf` 告警 |

**没验到的部分**：网页端那六项（打卡乐观更新、闹钟编辑器、专注设置下拉、数据存卡开关）需要**与设备同网段**的浏览器；这一轮 PC 路由不通，只做了代码级检查，列为 §九 第 1 条。

### 6.8 M13 三项真机复测（2026-10-24 02:1x–02:5x）

设备已回到 601-5G 节点（`192.168.0.106`），所以这一轮既能串口抓图、也能从 PC 打 REST。

| 项 | 结果 |
|---|---|
| 打卡圈纯黑 | 截图 `15-plans-checked-circle.png`：完成项＝纯黑实心圆＋白勾（无描边、无白边），未完成＝黑空心圈；页头「完成 1/2」与 `/api/plans` 的 `done` 一致 |
| 时钟模式（暗） | M13 当时是**竖屏**：黑底白字 02:29 / 10月24日 周六 / 元旦·还有 69 天 / 高考·还有 226 天 / 国庆节·还有 342 天（那两张竖屏截图已被 M13.1 的横屏版 `18-…`/`19-…` 取代并删除，见 §6.9） |
| 时钟模式（亮） | 同上，白底黑字（`focus_dark` 切换即时生效） |
| 时钟模式入口 | 截图 `17-settings-modes.png`：设置页「专注」区块并排两个黑框 chip「进入专注模式」「时钟模式」，下方「暗色时钟」chip 为黑底白字 |
| 音乐页异步 | 日志 `music: scanned 115 tracks in 681 ms`（冷）/ `205 ms`（热）、`music: 115 rows built in 675–1454 ms`；截图 `16-music-library.png` 显示状态行「共 115 首」+ 每行两行文本（♪ 周杰伦-七里香.mp3 / 11.3 MB） |
| 最终固件启动 | `boot14.log`：`display ready`@2.10s、`plans: loaded 3 plans, today 2026-10-24`、`alarms: loaded 1 alarms`、`ready`@4.02s、`station got ip 192.168.0.106`@5.04s，**无 E 行**、无 `no slots`、无 `sta is connecting` |
| 二进制 | `0x378060`（3,637,344 字节），app 分区剩 `0x77fa0`（491,424 字节，12%） |
| 内存 | `/api/status`：`internal_free 114195`、`internal_largest 31744`、`psram_free 7012336` |
| REST（PC curl / Python，UTF-8） | `GET /api/alarms` → `{"now":"02:35","items":[{id:a1,time:07:30,label:起床,days:127,enabled:true,active_today:true}],"ringing":false}`；`POST /api/alarms` 新建 `a2`、`DELETE ?id=a2` → `{"ok":true}`；`POST /api/checkin` 开关往返 → `{"ok":true,"done":true/false}`；`GET /api/files?path=/music` → 115 项；`GET /api/ai` → `{"ok":true,"configured":true,"items":[]}` |

**这一轮踩到的两个新坑**（已记入 §七 33、36 条）：浅灰在纯黑白 UI 里几乎不可见（打卡圈灰边、chip 灰底都看不出形状，截图里直接变白）；LVGL 往**可见**列表里逐行 add 是 O(n²)，建行期间必须先隐藏列表。

### 6.9 M13.1 时钟模式横屏 / 下一闹钟 / 三击退出（2026-10-24 03:0x，串口 tour 固件）

tour 固件（`shot.c` 临时 `PP_SCREEN_TOUR=1`）抓了 16 块 1bpp 截图，其中相关的几行：

| 项 | 结果 |
|---|---|
| 时钟模式横屏（暗） | 截图 `18-clock-mode-landscape-dark.png`（640×480 已转正）：黑底白字 02:54 / 10月24日 周六 / **闹钟 07:30 · 起床** / 元旦·还有 69 天 / 高考·还有 226 天 / 国庆节·还有 342 天 |
| 时钟模式横屏（亮） | 截图 `19-clock-mode-landscape-light.png`：同布局白底黑字，四行内容与暗色逐字一致 |
| 退出提示已去掉 | 两块截图里都**没有任何**「轻触屏幕退出」文字，底部留白 |
| 三击退出 | 串口 `---TOURTAPEXIT active=0---`：tour 向 overlay 连发 3 次真实 `LV_EVENT_CLICKED`，600ms 后 `pp_clockmode_active()` 已是 0（overlay 被 `lv_async_call` 删掉） |
| 下一个闹钟算法 | 设备上只有一个启用闹钟 `a1 07:30 起床（days=127 每天）`，凌晨 02:54 抓到的是**当天** 07:30；同一个算法在「今天的点已过」时会输出 `闹钟 明天 07:30 · 起床`，没有启用闹钟时整行隐藏（`next_alarm_text()` 返回 false） |
| 最终固件启动 | `boot17.log`：`display ready`@2.09s、`plans: loaded 3 plans, today 2026-10-24`、`alarms: loaded 1 alarms`、`ready`@4.15s、`station got ip 192.168.10.104`@4.45s、`clock: RTC updated from SNTP`@5.01s，**无 E 行**（连 `wifi:sta is connecting` 也没有了） |
| 二进制 | `0x3782f0`（3,638,000 字节），app 分区剩 `0x77d10`（490,768 字节，12%）；M14 终版 `0x378f10`，见 §6.10 |

**顺手修掉的 WiFi 竞争**（§七 第 42 条）：这一轮之前在**每次开机**都能看到 `E wifi: sta is connecting, cannot set config` ×6 + `E net: set STA config failed: ESP_ERR_WIFI_STATE`。真因是 `WIFI_EVENT_STA_START` 里已经用 NVS 里的同一份凭据调了 `esp_wifi_connect()`，而 `apply_saved_credentials()` 又去 `disconnect + set_config`，`disconnect` 弹不掉正在进行的连接。现在先用 `esp_wifi_get_config()` 比对驱动里的 ssid/password：一样就什么都不做；真换了凭据才 `esp_wifi_stop() → set_config() → esp_wifi_start()`。修完 `boot17.log` 里一条 E 行都没有，STA 照样在 4.45s 拿到 IP。

### 6.10 M14 自动休眠真机复测（2026-10-24 03:2x，串口 tour 固件）

`shot.c` 临时 `PP_SCREEN_TOUR=1`，串口抓 17 块截图 + 4 个状态 marker，全部命中：

| 项 | 结果 |
|---|---|
| 熄屏 | `---TOURSLEEP asleep=1 duty=0---`：把 `sleep_min` 设成 1、等 2.5s 后 `pp_autosleep_active()` 为真，且 `ledc_get_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0)` 读回 **0** —— 不是「以为关了」，是背光寄存器真的为 0 |
| 唤醒 | tour 用 `lv_display_trigger_activity(NULL)` 模拟一次触摸，1.5s 后 `---TOURWAKE asleep=0 duty=603---`：状态回 false、duty 恢复 603（亮度 59% × 1023 ≈ 603） |
| 设置页开关 | 截图 `05-settings.png`（已换成这一版）：额外加了一段 tour，把 `auto_sleep` 真的打开再进设置页 → 「自动休眠 **开**」「休眠时间 **每 5 分**」，下面一行提示「熄屏后轻触屏幕唤醒；闹钟响铃和时钟模式不会熄屏」 |
| 字号 chip 反白 | 同一张截图顺带确认了 M13 那个没抓到的修复：选中「16」是**黑底白字**，未选中「20」是白底黑框 |
| 其余 16 块 | 计划/倒数+闹钟/文件/音乐/AI/键盘/响铃浮层/专注/时钟暗/时钟亮/三击退出/阅读器 16/阅读器 20 全部与 M13.1 一致（这次没动它们，属于回归） |
| 最终固件启动 | `boot18.txt`：`display ready`@2.09s、`plans: loaded 3 plans, today 2026-10-24`、`alarms: loaded 1 alarms`、`ready`@4.11s、`station got ip 192.168.10.104`@4.55s、`clock: RTC updated from SNTP`@5.98s；`E (` 计数 **0**，只有三条无害 `W`（3wire_spi 删 CS、GPIO16 冲突、SD 输入延迟） |
| 二进制 | `0x378f10`（3,641,104 字节），app 分区剩 `0x770f0`（487,664 字节，12%）；M14.2 终版 `0x379220`（3,641,888 字节，剩 `0x76de0` = 486,880 字节），见 §6.11 |
| 未做到 | 设备这次落在 mesh 的另一个节点上（`192.168.10.104`），PC 在 `192.168.0.x` 不路由，**`/api/config` 里 `auto_sleep`/`sleep_min` 两个字段没能从 PC 用 curl 验证**；字段收发是照着既有 `focus_dark`/`cache_sd` 的同一套代码写的，属于代码级确认，未做端到端 |


### 6.11 M14.2 滑块纯黑真机复测（2026-10-24 03:41，串口 tour 固件）

`shot.c` 临时 `PP_SCREEN_TOUR=1`，`pp_serial.ps1 -Seconds 190` 抓到 907,540 字节日志、17 块截图、20 个 marker，无一条 `E (`：

| 项 | 结果 |
|---|---|
| 主题原色取证 | `I (47619) settings: slider theme colour before override: ind=#ff000000 knob=#ff000000`，第二个滑块 `I (47629) settings: ...` 同值 → **设备端滑块本来就是纯黑**，紫色不在设备上 |
| 覆盖后外观 | `05-settings.png` 已换成这一版：两条滑块都是「白底 + 黑描边轨道 + 黑色填充 + 黑色圆钮」，行距滑块钮在中点、亮度滑块钮约 58%，与其它控件的黑框风格一致 |
| 网页端 | `input[type=range]{accent-color:#000}`；这段是纯 CSS，**没有浏览器实测**（PC 与设备这次不在同一网段，且没有无头浏览器可用） |
| 回归 | 计划/倒数+闹钟/文件/音乐/AI/键盘/响铃浮层/专注/时钟暗/时钟亮/三击退出(`---TOURTAPEXIT active=0---`)/阅读器 16/阅读器 20/自动休眠(`---TOURSLEEP asleep=1 duty=0---`)/唤醒(`---TOURWAKE asleep=0 duty=603---`) 全部与 M14 一致 |
| 二进制 | tour 版 `0x3799b0`；恢复 `PP_SCREEN_TOUR=0` 后正式版 `0x379220`（3,641,888 字节，剩 `0x76de0`），已烧录，`Hash of data verified.` |
| 正式版启动 | `boot20.txt`：`display ready`@2.10s、`plans: loaded 3 plans, today 2026-10-24`、`alarms: loaded 1 alarms`、`ui: shell ready`@3.64s、`ready`@4.17s、`station got ip 192.168.10.104`@4.57s、`clock: RTC updated from SNTP`@7.12s；`E (` 计数 **0**，仍是那三条无害 `W`（3wire_spi 删 CS、GPIO16 冲突、SD 输入延迟） |

---

## 七、踩过的坑与修法（都已在代码里修好，留作备忘）

| # | 现象 | 根因 | 修法 |
|---|---|---|---|
| 1 | 开机黑屏 + 蜂鸣器长鸣 | `esp_io_expander_set_dir/set_level` 第二个参数是**位掩码**不是引脚号，`PP_EXIO_*` 被写成 0..7 → 蜂鸣器 P7 从未拉低，3-wire SPI 的 CS 指错脚 → 面板从未初始化 | `board.h` 里改成 `(1u << n)`，`board.c` 里按掩码组合 |
| 2 | 主任务死循环、看门狗 20s 一次 | LVGL 用内置 64KB 静态池（`LV_MEM_SIZE_KILOBYTES=64`）+ `LV_ASSERT_NULL` 的 handler 是 `while(1)`；100+ 文件 × 每行 3 个 label 瞬间耗尽 | sdkconfig 改 `LV_USE_CLIB_MALLOC/CLIB_STRING/CLIB_SPRINTF`，关掉 `LV_USE_ASSERT_NULL` 与 `ASSERT_MALLOC` |
| 3 | `ESP_ERR_WIFI_STATE (0x3006)` 无限重启 | `esp_wifi_start()` 之前设了只填 ssid 的 config；start 后立即开始连接，再 `set_config` 必然报 sta is connecting | 删掉预热，改成 start 之后统一 `apply_saved_credentials()`；`pp_net_set_credentials()` 先 `disconnect` + 50ms 再 `set_config` |
| 4 | 中文文件名变 ??? / %EF%BF%BD | FatFS `API_ENCODING_ANSI_OEM` + CODEPAGE 936 给的是 GBK 字节 | 改 `CONFIG_FATFS_API_ENCODING_UTF_8=y`，`gbk.c` 退化为 UTF-8 直通（省掉 196KB 查表） |
| 5 | 纯色测试屏幕全黑但日志正常 | 背光初始 duty=0，`SetBrightness(80)` 在构造函数末尾才调用 | 测试前先 `SetBrightness(80)`；背光初始化要早于任何画面 |
| 6 | 照片看起来偏蓝 | 手机相机白平衡 + xiaozhi 浅色主题自带冷调 | 纯色测试确认面板/数据线/时序全对，无需改代码 |
| 7 | build.py / esptool 各种"假失败" | xiaozhi 的 `build.py` 结束返回 1；esptool 新版子命令是连字符 | 只看 `Build completed successfully!` / `Hash of data verified.` |
| 8 | 编译报 MSys/Mingw is not supported | 全局环境变量 `MSYSTEM=UCRT64`（Git Bash 装的），IDF 6.1 的 `activate.py` 见到就拒绝 | `pp_build.ps1` 里先 `$env:MSYSTEM=$null` 再激活 |
| 9 | 启动即 ESP_ERR_NO_MEM，日志指 wifi.c:183 esp_wifi_init | Wi-Fi 要**一整块连续**内部 DMA RAM；UI 先建完只剩 46KB 碎片 | `app_main.c` 里把 `pp_net_start()` 提到 `pp_ui_init()` 之前 |
| 10 | 切最后一个 tab 内部 RAM 只剩 23 字节，随后 `sdmmc: allocate_dma_buf not enough mem (0x101)`、读 14MB 书 Double exception | `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384`：**所有 <16KB 的 malloc 都落内部 RAM**，120 行列表的小对象（obj/style/label 文本）合计 90KB 全进内部 | 改成 `=0`（`sdkconfig` 与 `sdkconfig.defaults` 两处），内部 RAM 稳定 ~124KB |
| 11 | 进入 120 项目录要 8 秒 | 不是扫描慢（130ms），是 **LVGL 创建 120 行兄弟节点 O(n²)**：每加一行，后面的 create/styles/label 耗时都涨 | `page_files.c` 改为**分页 24 条/页** + 行绝对定位，仅建当前页 |
| 12 | 首次切 tab 卡 6–11 秒 | `pp_ui_show_tab()` 第一次被调就把 5 个 tab 全建 | 只建被点的那一页，其余循环里 `if (NULL) continue` |
| 13 | 网页端 `/api/files?path=/` 空列表 | 路径前缀 `/` 与根 `/sdcard` 拼接重复，且 `..` 检查误伤 | 先剥掉全部前导 `/` 再做 `..` 检查；query 值加 `uri_unquote()`（%XX 与 +） |
| 14 | 中文大面积显示成空心方框 | 页面从没设过字体 → 全局 Montserrat 14 无 CJK；内置 Source Han 是**子集**字体（千余字），且只有阅读器在用 | 自建 1bpp SimHei 全字库 `pp_font_16/20`（21,318 字形），`lv_theme_default_init(..., &pp_font_16)` 设为全局默认 |
| 15 | 「每天 · 连续 0 天」的间隔点仍是方框 | 源码里是 **U+00B7**，生成字体时漏了 `0x00A0-0x00FF` 区间 | 重新生成时加该区间；`chk_chars.py` 扫全部源码的非 ASCII 码点比对覆盖表 |
| 16 | 文字发虚、有灰边 | 与面板/色彩链路无关（RGB565 + 未开 subpixel），是 4bpp 抗锯齿在小字号上的灰阶边缘 | 直接生成 **1bpp** 点阵（`--bpp 1`），纯黑无过渡色 |
| 17 | AI 页气泡被裁成很窄一列，每行只剩几个字 | 用 `lv_text_get_size(..., BUBBLE_MAX, ...)` 先量宽再 `set_width`，但气泡按内容推导出的宽度只有 130px（label 却是 320） | 用 `LV_COORD_MAX` 量单行宽，并**同时显式给 label 和 bubble 设宽**（`w` / `w + 2*10 + 2*1`） |
| 18 | AI 输入框在屏幕上几乎看不见 | textarea 用主题默认样式：白底 + 浅灰边 + 浅色占位符 | 显式设白底/黑边/黑字；占位符用 `lv_obj_set_style_text_color(input, 0x707070, LV_PART_TEXTAREA_PLACEHOLDER)`（LVGL 9 没有 `text_placeholder_color`） |
| 19 | 截图里 AI tab 与音乐 tab 逐字节相同 | RGB 面板 direct-mode 双缓冲，`lv_display_get_buf_active()` 在大重绘后可能仍指向上一帧 | 改用 `lv_snapshot_take(lv_screen_active(), RGB565)` 渲染整屏，且取像素全程在 `lvgl_port_lock()` 内 |
| 20 | 截图整屏全黑 | PBM 阈值写成 `luma < 31*0xFFFF/2`（1,015,792），白色实际 luma 上限只有 538,864 | 改成 `r*2126 + g*7152 + b*722 < 269000`（白色 31/63/31 的一半） |
| 21 | `curl` 取大图必 ECONNRESET / 超时 | LWIP `TCP_SND_BUF_DEFAULT`、`TCP_WND_DEFAULT` 只有 5760，往返 200ms → 吞吐 ~28KB/s | 截图改成 1bpp PBM（38KB）；更大的传输走串口 base64 或 WebDAV 分块 |
| 22 | 想用 `lv_display_set_rotation(disp, 90)` 直接横屏，改了没效果 | RGB **direct mode** 的显示旋转必须走 `lv_display_set_matrix_rotation()`，要开 `LV_DRAW_TRANSFORM_USE_MATRIX`（本工程 `CONFIG_LV_USE_MATRIX` 未开），而且转换层要一整屏 640×480×2 ≈ 600KB，还得改 sdkconfig 全量重编 13 分钟 | 放弃整屏旋转，改成**只对几个 label 设 `lv_obj_set_style_transform_rotation(obj, 900, 0)`**（pivot 50%/50%），坐标手工映射 `ox=-(v-240), oy=(u-320)`；1bpp 大字旋转后依然锐利 |
| 23 | 专注模式旋转后方向不确定 | `transform_rotation` 正角度是顺时针，屏幕坐标系与物理持握差 90° | 实测 `ROT=900` 时设备**顶边朝左**（逆时针转设备）为正；截图需 `PIL.rotate(90, expand=True)` 才正，这个换算已写进报告 |
| 24 | 专注页「已专注」一直是 00:00 | 格式化写成了 `%02ld:%02ld` + `secs/3600, (secs/60)%60`（时:分） | 改成 <1h 用 `分:秒`，≥1h 才 `时:分:秒`；用 `strcmp` 判等避免每秒重设文本 |
| 25 | `POST /api/countdowns` 带 id 更新时返回 `{"ok":true,"id":""}` | `pp_countdowns_upsert()` 只在**新建**分支回填 `new_id` | 改成始终从 item 里读回 id 回填（新建与更新都返回 `c<N>`） |
| 26 | 用 bash 的 `curl -d '{"title":"体检"}'` 建出来的标题是乱码，一度以为固件 UTF-8 处理有问题 | Windows 下把命令行参数转成进程参数时会按本地代码页（936）转换，中文在**发送前**就已经不是 UTF-8 了 | 改用 Python `json.dumps(...).encode("utf-8")` 发送并**断言** `title == "体检"` → 设备端往返完全正确；以后测中文一律走 Python/文件，不用 shell 内联中文 |
| 27 | 加了闹钟/历史/存卡几组接口后，WebDAV 的 `HEAD`/`GET` 全部失效（资源管理器看不到文件） | REST 23 条 + WebDAV 12 条 = **35 > `cfg.max_uri_handlers = 32`**，`httpd_register_uri_handler` 静默失败，日志只有 `W httpd_uri: ... no slots left for registering handler` 与 `W webdav: register /* failed: ESP_ERR_HTTPD_HANDLERS_FULL` | `webdav.c` 的 `cfg.max_uri_handlers` 从 32 提到 **48**；以后每加 handler 都要数一遍总数 |
| 28 | LVGL 键盘上的回车/退格/地球/箭头显示成空心方框 | 字体覆盖只到 `0xFF5E`，而 LVGL 的符号常量全在 **FontAwesome 段**（`LV_SYMBOL_NEW_LINE` = U+F8A2） | `.fontgen/gen2.sh` 追加 `-r 0xF8A0-0xF8FF` 重生成两个字号，重新烧录后符号齐全 |
| 29 | 网页端点完打卡要等一整轮刷新才有反馈 | 每次 `checkin` 后都全量 `loadPlans()` 重渲染，网络往返 200ms+ 才看到变化 | `index.html` 改**乐观更新**：先切圆圈与「完成 N/M」计数，`fetch` 失败或超时才回滚 + toast |
| 30 | AI 回复被裁成很窄一列，比提问气泡窄一半 | 占位气泡按「正在思考」四个字量宽，回复回填后没重算 | `reply_done()` 里用 `fit_bubble()` 按真实回复重新量宽并同时设 label 与 bubble 宽度 |
| 31 | 把数据切到 SD 卡后拔卡开机，读不到计划 | 迁移是「复制→删源」，卡不在时 `fopen` 直接失败 | `pp_data_write` 在开关开着但卡未挂载时返回 `ESP_ERR_INVALID_STATE`，**不静默写回 /lfs**；UI 提示「SD 卡未挂载」，插回卡立即恢复 |
| 32 | 闹钟同一分钟里响两次（改配置后更明显） | 每秒轮询只比 `HH:MM`，处理函数返回后下一轮又命中 | 去重键改成 `"<日期> <HH:MM> <id>"`，响过就记下；改时间/改星期自然换键 |
| 33 | 打卡圈改纯黑后看起来「没有圈」/ 设置页 chip 看起来只是文字 | **浅灰描边在纯黑白 UI 上几乎不可见**：`border_color 0xC8C8C8`、`bg 0xE4E4E4` 白底上对比度极低；截图又是 1bpp 中灰阈值，直接当白丢掉，所以截图里完全看不到形状 | 所有描边/选中态一律纯黑：打卡圈未完成 `border 0x000000`、完成 `bg 0x000000` + 白勾；设置页 `C_CHIP/C_EDGE` 全改 `0x000000`，选中＝黑底白字 |
| 34 | 打卡完成态有「白边」 | 原来是白底 + 2px 黑环 + 黑勾，1bpp 下环内外都是白，视觉上像是白边套黑圈 | done 时 `border_width 0`、`bg 0x000000`、勾改白色，即纯黑实心圆 |
| 35 | 音乐 tab 每次进入都要等 6 秒 | 115 行 × 4 个对象（container+icon+名字+大小 ≈ 460 个对象）一次性建完 | 行改**单 label**（`♪ 名字\n11.3 MB`，460 → 115 个对象）+ 建行期间列表 `LV_OBJ_FLAG_HIDDEN` + 每 20ms 建 40 行 → 675–1454ms |
| 36 | 逐行 `lv_obj_create` 越来越慢（插桩：row 0 5.4ms → row 114 64ms） | 每加一行都让**整块列表区域失效重绘**，成本随已有行数线性增长 → O(n²)；改成绝对定位 / 关滚动 / `LV_LAYOUT_NONE` 都无效 | **建行期间把列表 HIDDEN**，建完 `clear_flag`：单行成本立刻变成常数（row 0 2214us、row 114 2037us） |
| 37 | 音乐行第二行（文件大小）被截成「...」 | `ROW_H 52` + `pad 9` 放不下两行 16px 文本，`LV_LABEL_LONG_DOT` 直接把第二行吃掉 | `ROW_H 64` + `pad_top/bottom 7` + `text_line_space 2` |
| 38 | 每次开机必现 `E wifi: sta is connecting, cannot set config` ×6 + `E net: set STA config failed: ESP_ERR_WIFI_STATE` | 第一次以为是「disconnect 后等太短」，改成 100ms×6 重试**照样全失败**：真因是 `WIFI_EVENT_STA_START` 里已经用 NVS 里的**同一份**凭据调了 `esp_wifi_connect()`，而连接中调用 `esp_wifi_disconnect()` 也返回 `ESP_ERR_WIFI_STATE`、弹不掉握手，于是配置永远设不进去 | 先 `esp_wifi_get_config()` 比对驱动里已有的 ssid/password：**一样就什么都不做**；确实换了凭据才 `esp_wifi_stop() → esp_wifi_set_config() → esp_wifi_start()`（只有 stop 能保证回到 idle）。`boot17.log` 已无任何 E 行 |
| 39 | SD 缓存迁移把闹钟弄丢过一次（`alarms: loaded 0 alarms`） | 迁移是「复制 → 删源」，写入磁盘的瞬时失败没有被发现就把源删了，根因未完全复现 | 迁移写完**读回校验长度**，不一致就 `ESP_LOGE` 并**两份都保留**、不删源；数据丢失事件已如实记录（见 SD-REPORT.md） |
| 40 | 时钟模式屏幕上一个按钮都没有，三击万一失效设备就锁死在时钟页 | 在对象自己的事件回调里 `lv_obj_delete(self)` 属于 LVGL 明令禁止的用法（删完回调还在栈上跑） | 点击计数到 3 时只调 `lv_async_call(close_async, NULL)`，下一轮 LVGL 循环再删；tour 里用 `lv_obj_send_event(root, LV_EVENT_CLICKED, NULL)` 连发 3 次真实事件验证，串口 `---TOURTAPEXIT active=0---` |
| 41 | 时钟模式横屏排版：flex 容器 + 90° 旋转子对象的测量/换行不可控 | 旋转是绘制期变换，布局期仍按未旋转的包围盒算，`LV_SIZE_CONTENT` 与 `flex` 组合容易错位 | 不赌布局：倒数日改成 4 个**固定坐标** label（`CD_TOP + i*32`），空行加 `LV_OBJ_FLAG_HIDDEN`，页面只在 `landscape` 坐标里手排 |
| 42 | 串口 tour 抓图偶发一块解不出来：`binascii.Error: Incorrect padding` / `image file is truncated` | 115200 串口高速输出偶发丢字符，base64 被截断 | `decodetour.py` 补 `=` 到 4 的倍数 + `ImageFile.LOAD_TRUNCATED_IMAGES = True` + 逐块 try/except，坏块跳过不影响其余 15 块 |
| 43 | 自动休眠的「多久没人操作」自己算不准 | 想用 `lv_tick_get()` 减「最后一次事件」时间，但输入来自多个 indev（触摸、键盘），还会被程序自己刷新误导 | 直接用 LVGL 的 `lv_display_get_inactive_time(NULL)`：每个 indev read 都会给 `disp->last_activity_time` 打戳，天然覆盖所有输入；模拟一次触摸也用 `lv_display_trigger_activity(NULL)`（tour 就是这么验的） |
| 44 | tour 固件跑完 16 张截图，屏幕在中途黑了、后半段全黑 | tour 全程不碰屏幕，自动休眠（默认 5 分钟）到点就关背光——截图截的是背光关掉后的画面 | tour 开头把 `auto_sleep` 置 false，结尾再读用户原值写回 `config.json`（tour 中途多次 `pp_config_save()`，不写回就把 false 永久留在配置里） |
| 45 | 网页上改亮度/主题，设备端看起来「没反应」 | 设备正处在熄屏状态，背光还是 0 | `h_config_post` 保存完检查 `pp_autosleep_active()`，若已熄屏就按新亮度把背光点回去；切「数据存 SD 卡」时同理 |
| 46 | `git push` 到 github.com 一直 `Failed to connect ... after 21132 ms`，但 `curl https://api.github.com` 正常 | 这个网络到 `github.com` 的 A 记录（`20.205.243.166`）被丢包，而 `20.205.243.168`（api）、`20.205.243.165`（codeload）、`140.82.112.3` 都通 —— 换 IP 用 `curl --resolve` 立刻 200 | 用 GitHub **Git Data API** 发布：blobs → tree → commit → `PATCH /git/refs/heads/main`（force），102 个文件 15.6MB，一次成功；`git` 本身仍保留本地提交，待网络正常时再对齐 |
| 47 | 本地 `git add -A` 把 55MB 的 `tools/arduino-cli.exe/.zip`、16 张临时截图、`_check.js` 也加进了索引 | 工作区是「项目 + 一堆早期实验」混住的，`.gitignore` 只写了实验目录名 | `git rm --cached` 剔除 + `.gitignore` 补 `tools/arduino-cli.*`、`pocketplan/n[2-4]*.png`、`pocketplan/main/www/_check.js`，再 `commit --amend`；入库前一定先 `git ls-files | wc -l` 和 `git ls-tree -r -l | sort -k4 -nr | head` 看一眼 |
| 48 | 用户说「设置页的滑块条是紫色的」，但设备截图里滑块明明是黑的 | 两个原因叠在一起：① 设备截图是 `shot.c` 的 **1bpp PBM**，只按灰度阈值判墨，**颜色在截图里不存在**，所以截图既证明不了黑也证明不了紫；② 真正紫的是**网页端** `input[type=range]`，浏览器对未指定 `accent-color` 的 range 用的是 Material 默认靛紫 | 先用 `lv_obj_get_style_bg_color(LV_PART_INDICATOR/KNOB)` 把 LVGL 解析后的值打出来（`ind=#ff000000 knob=#ff000000` → 设备端本来就是黑），再改网页 `input[type=range]{accent-color:#000}`；设备端仍显式钉死三个 part 作为保险。**教训：截图不能用来判断颜色，要读样式值或背景色寄存器** |

---

## 八、已知限制

- **无麦克风、无喇叭**，音频只能走 I2S → PCM5102A（GPIO43/44/4，**模块尚未焊上**）：M4 目前只有界面，出声要等硬件
- 阅读器只做**纯文本流式排版**：Markdown 按原文显示，没有加粗/标题样式，也没有图片
- 音乐页只扫描 `/sdcard/music` 一层，不递归
- 网页端是**单文件 SPA**（33KB 内嵌在固件里）：改页面要重新编译烧录
- 文件列表分页固定 24 条，没有做「滚动到底自动加载」
- `W (2048) gpio: conflict found for GPIO[16]` —— TP_INT 被复位代码与触摸驱动各配置一次，无害，待清理
- **字体只有 16px / 20px 两档点阵**，没有中间字号：15px、18px 这类需求要么放大要么缩小；1bpp 也没有反锯齿，斜笔画在极小字号下会比矢量字体硬
- 字体源文件 `.c` 合计 ~14.7MB，全量重编时编译器要多花几十秒；增量构建不受影响
- 固件已到 **3.64MB / app 分区 3.94MB（0x379220 = 3,641,888 字节，剩 12%，0x76de0 = 486,880 字节）**，再加功能（OTA、更多字体档位、MP3 解码）前必须先瘦身（例如裁掉 0x4E00-0x9FA5 里用不到的字，或把 20px 改成按需生成）
- Windows 映射网络驱动器连 HTTP WebDAV 需要开 WebClient 服务；**尚未在真资源管理器里实测**（curl 层面全通）
- 倒数日在**设备端只读**：屏幕没有中文输入法，增删改只能在网页端做（这是故意的取舍，不是没做完）
- 专注模式目前只是「横屏时钟 + 已专注计时」：没有番茄钟倒计时/提醒、没有白噪音、不统计历史专注时长，退出即清零
- 专注模式的横屏靠**每个 label 单独 90° transform**：标签数量再翻几倍会明显吃 CPU（每个变换都要独立渲染层），要做复杂横屏页面还是得开 `LV_DRAW_TRANSFORM_USE_MATRIX` 走整屏旋转
- 底部 6 个 tab 后 nav 按钮宽度从 88 压到 74：文字两字还好，将来加第 7 个 tab 就得换成图标-only 或滚动 nav
- **闹钟只能「嘀嘀」**：蜂鸣器挂在 TCA9554 P7，只有开/关两位，没有频率、没有音量，所以做不了铃声/渐强；上限 16 个，重复只能按周几（没有「每隔 N 天」、没有贪睡）
- **闹钟响铃时全屏浮层会盖住当前页面**（这是有意的），响满 60 秒自动停；如果期间正在看阅读器，退出后回到原页
- **切「数据存 SD 卡」会重启**（约 1.2s）：迁移完成才重启，所以一定切得干净；但迁移途中拔卡会留下「一半在 /lfs、一半在卡上」的状态，插回卡再切一次即可自愈（不会丢数据，只会复制不完整）
- **UI 是纯黑白风格（1bpp 点阵字体，无抗锯齿）**：层次只能靠黑框、黑底、反白表达；浅灰（`0xC8C8C8`/`0xE4E4E4`）在白底上几乎看不见，截图里更是直接变白。想弱化次要信息只能用 16px 小字，不能用灰
- **音乐页 115 首仍要 0.7–1.5s 才建完**：期间可以点、可以滚动，但一直显示「读取中 N/115」；超过 200 首会被 `MAX_TRACKS` 截断（列表上限），真正上万首要换成虚拟列表/分页
- **时钟模式最多显示 4 条倒数日**：多了不显示（暂时不做翻页），要看得在网页端删掉或等后续加分页
- **时钟模式屏幕上看不到任何退出提示**（用户要求「不显示退出方式」），只能 2.5s 内轻触 3 下退出；不知道这个手势的人会以为卡住了——真要做成品，建议在设置页保留这句说明（当前就在「专注」区块的提示文案里）
- **时钟模式的「下一个闹钟」只显示最近一个启用的闹钟**：不显示再往后的，也不显示「今天已响过」；全部关掉时整行隐藏
- **网页端 M12 那六项功能仍只做了代码级检查**：M13 时设备已回到 `192.168.0.106`（PC 同网段、REST 全通），但**网页 UI 的交互观感**（乐观更新是否顺滑、chip 点击手感）需要人拿浏览器点一遍，机器验不了
- SD 卡上的数据目录是 `/sdcard/.pocketplan/`（隐藏目录）：网页端「文件」列表会显示它，删掉等于清空计划/倒数/闹钟/AI 历史
- **自动休眠只关背光，不进 light sleep**：WiFi、CPU、闹钟任务照常跑，所以待机功耗仍在几十 mA 量级；换来的是「一碰就亮」且闹钟不会漏。要真省电得进 light sleep + 定时器唤醒，那是另一档复杂度
- **熄屏没有锁**：一碰就亮，没有 PIN/图案锁。设备和「关机」的区别就是屏幕黑不黑
- **响铃与时钟模式不熄屏**是有意的例外：闹钟响时必须看得见，时钟模式本来就是当台钟用；代价是响铃 60 秒内屏幕一直亮着
- 休眠时长只有 1/3/5/10/30 五档（屏幕 chip 循环切换，网页是下拉），没有「不熄屏」以外的自定义值；30 分钟以上只能再进网页设置（上限 60）
- **GitHub 仓库的首个 commit 是走 API 建的**（见 §3.8）：本地 `git` 的 commit sha 与远端不同，在这台机器上 `git push` 也不通（`github.com:443` 被丢包），要同步得先换网络或继续用 API

---

## 九、下一步（建议顺序）

1. **网页端人工验收**（最优先，属上一轮的未闭环）：手机或电脑打开 `http://192.168.0.106/` → 打卡看是否即时反色、「倒数」tab 的闹钟编辑器能增删改、「设置」→「专注」能选倒数日与暗色、「设置」→「数据存储」能切 SD 卡并在状态里看到 `data_on_sd:true`（REST 层已全通，缺的只是 UI 手感）
   - **设备端手感验收**：点打卡圈（黑圆/空心圈切换）、设置里两个 chip 进出「专注/时钟模式」、音乐 tab 进入时是否先看到「读取中 N/115」
   - **滑块颜色确认**（M14.2）：先在**手机网页**上确认「设置」里的亮度/行距滑块不再是紫色（`accent-color:#000` 是纯 CSS，没做过浏览器实测）；再在**设备屏**上看一眼两条滑块——设备端日志已证明是纯黑（`ind=#ff000000 knob=#ff000000`），显式样式只是保险
2. **焊 PCM5102A**（GPIO43 BCLK / GPIO44 LRCK / GPIO4 DIN，3.3V + GND）→ M4 接 `esp_audio_codec` 解 MP3 → I2S 出声
3. **触摸交互实测**：打卡按钮、阅读器翻页、设置滑块、AI 键盘（AI 要先在网页端填 DeepSeek Key）、闹钟「开/关」chip
4. **Windows 映射实测**：资源管理器映射 `http://<设备IP>/`，把「拖拽拷书」这条最爽的路径走通
5. **M10 收尾**：OTA（浏览器上传固件，注意 app 分区只剩 12%）、长时间挂机稳定性、外壳
6. **稳定性长跑**：让板子连续挂机 24h+，看 `status_task` 打出的内部堆是否缓慢下降（当前 ~124KB 稳定），以及 WiFi 多次断线后的指数退避是否真的收敛到 60s
7. 可选：闹钟贪睡/自定义时长、专注模式番茄钟倒计时与白噪音、专注时长历史统计、倒数日支持农历节日、阅读器书签/目录、音乐页递归扫描
8. 可选（M13 遗留）：音乐列表超过 200 首改虚拟列表；时钟模式超过 4 条倒数日改翻页/滚动
9. **开源后续**（M14.1 刚起步）：给仓库加 Issue 模板 / Release（附 `pocketplan.bin` 与烧录说明）/ 一张实物照片进 README；`tools/pp_build.ps1` 里写死的 `IDF_PATH=D:\esp\esp-idf-v6.1` 改成参数或环境变量
10. 可选（M14 遗留）：自动休眠加「只在电池供电时休眠」与 light sleep 档位；时钟模式加「熄屏但保留画面」（面板 standby）

---

## 十、相关文件索引

| 文件 | 说明 |
|---|---|
| `D:\Users\PC\Desktop\esp32-mgxd\PLAN-BOARD-DESIGN.md` | 产品设计文档 v0.2（架构、分区、数据模型、UI、阶段计划） |
| `D:\Users\PC\Desktop\esp32-mgxd\PocketPlan-项目进度.md` | 本文档 |
| `D:\Users\PC\Desktop\esp32-mgxd\SD-REPORT.md` | SD 卡诊断报告 + §8 PocketPlan 读卡实测 |
| `D:\Users\PC\Desktop\esp32-mgxd\PORT-REPORT.md` | 板子硬件移植报告 + §7 PocketPlan 板级复用 |
| `D:\Users\PC\Desktop\esp32-mgxd\pocketplan\` | PocketPlan 固件工程（26 个 .c + 4 个字体 .c + 内嵌网页） |
| `D:\Users\PC\Desktop\esp32-mgxd\pocketplan\main\alarms.c` | 闹钟：alarms.json 读写 + 每秒匹配 + 蜂鸣器节奏 + 全屏浮层（M12 新增） |
| `D:\Users\PC\Desktop\esp32-mgxd\pocketplan\main\autosleep.c` | 自动休眠：轮询 LVGL inactivity clock，到点关背光、一碰恢复；响铃与时钟模式豁免（M14 新增） |
| `D:\Users\PC\Desktop\esp32-mgxd\README.md` | 开源首页（功能表 / 硬件表 / 编译烧录 / REST API / 已知限制） |
| `D:\Users\PC\Desktop\esp32-mgxd\LICENSE` | MIT 许可证 |
| `D:\Users\PC\Desktop\esp32-mgxd\.gitignore` | 排除 build/、managed_components/、二进制、日志、早期实验目录 |
| <https://github.com/MGXD-cheng/PocketPlan> | 开源仓库（M14.1 发布，102 个文件） |
| `D:\Users\PC\Desktop\esp32-mgxd\pocketplan\main\clockmode.c` | 时钟模式：横屏全屏，只有时间 + 日期 + 下一个闹钟 + 前 4 条倒数日；轻触 3 下退出（M13 新增，M13.1 改横屏） |
| `D:\Users\PC\Desktop\esp32-mgxd\pocketplan\docs\screenshots\` | 19 张真机截图（01–19；M14 用带「自动休眠 开/每 5 分」的新图替换了 05-settings.png） |
| `D:\Users\PC\Desktop\esp32-mgxd\tools\pp_build.ps1` | 编译封装（处理 MSYSTEM、激活 IDF、跑 idf.py） |
| `D:\Users\PC\Desktop\esp32-mgxd\tools\pp_flash.ps1` | 烧录封装（分区表 0x8000 + app 0x20000） |
| `D:\Users\PC\Desktop\esp32-mgxd\tools\pp_serial.ps1` | 串口日志（`-NoReset` 只看输出） |
| `D:\Users\PC\Desktop\esp32-mgxd\pocketplan\decodetour.py` | 把串口 tour 日志里的 base64 PBM 解成 PNG（`python decodetour.py ../tour.log ../t_`） |
| `D:\Users\PC\Desktop\esp32-mgxd\pocketplan\chk_chars.py` | 扫描源码里所有非 ASCII 码点，比对字体覆盖区间，重新生成字体前先跑它 |
| `D:\Users\PC\Desktop\esp32-mgxd\.fontgen\gen2.sh` | 字体生成脚本（lv_font_conv 两个字号 + 自动修 include） |
| `D:\Users\PC\Desktop\esp32-mgxd\tools\idfenv.ps1` | **已废弃**（MSYSTEM 坑），仅 sdtest/xz 旧工程还在用 |
| `D:\Users\PC\Desktop\esp32-mgxd\sdtest\` | SD 卡验证固件（虚标卡检测、容量探针） |
| `D:\Users\PC\Desktop\esp32-mgxd\xz\src\xiaozhi-esp32-main\` | xiaozhi 源码（ST7701 初始化序列、RGB 配置的原件） |
