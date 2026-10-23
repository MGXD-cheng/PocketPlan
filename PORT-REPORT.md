# ESP32-S3-Touch-LCD-2.8B 移植 xiaozhi 记录

## 结论

这块板子现在跑的是自己编译的 xiaozhi 固件，已激活、已联网、屏幕和触摸正常：

```
Board: UUID=e46396b1-3c6d-4993-b830-6062a804e4da SKU=esp32-s3-touch-lcd-2.8b
App version: 2.5.3        ESP-IDF: v6.1.0       PSRAM 8MB @80MHz
GT911 I2C address: 0x5D   TouchPad_ID:0x39,0x31,0x31
netcheck: gateway 192.168.0.1 reachable, connectivity OK (attempt 1)
MQTT: Connected to endpoint
Application: Activation done
StateMachine: State: activating -> idle
```

**没有麦克风和喇叭**（这块板子本身没装音频编解码芯片），所以只能看界面、触摸、联网，不能语音对话。

---

## 1. 硬件实测

### I2C 扫描（SDA=GPIO15 / SCL=GPIO7，100kHz）

| 地址 | 器件 | 备注 |
|------|------|------|
| 0x14 | GT911 触摸 | 复位后 INT 拉高时落到 0x14 |
| 0x20 | TCA9554PWR | 8 位 IO 扩展，屏/触摸/SD 复位与片选都挂在上面 |
| 0x51 | PCF85063 | RTC |
| 0x6B | QMI8658 | 六轴 IMU（WHO_AM_I = 0x7C） |
| 0x7E | 未识别 | 无应答风险，未使用 |

**关键**：0x24（CH32V003）无应答。板子出厂刷的 `esp32-s3-cam-2.8` 固件按 CH32V003 去找扩展器，取到空句柄后去写背光 PWM，解引用 NULL+0x38 → `LoadProhibited` 无限重启。这是固件与板型不匹配，不是硬件坏。

### 引脚

```
I2C      SDA=15  SCL=7          I2C 触摸 INT=16
LCD      ST7701S, 480x640 RGB565
PCLK=41  DE=40   VSYNC=39  HSYNC=38
DATA0..15 = 5,45,48,47,21,14,13,12,11,10,9,46,3,8,18,17   (B1..B5,G0..G5,R1..R5)
SPI(3线) CS=EXIO3  SCL=2  SDA=1    背光=GPIO6 (20kHz PWM)
EXIO  P0=LCD_RST P1=TP_RST P2=LCD_CS P3=SD_CS P4=IMU_INT1 P5=IMU_INT2 P6=RTC_INT P7=BUZZER
BOOT=0   SD: D0=42 CMD=1 SCK=2 CS=EXIO4
```

---

## 2. 板型文件

新增了 `xz\src\xiaozhi-esp32-main\main\boards\waveshare\esp32-s3-touch-lcd-2.8b\`：

- `config.h` —— 全部引脚宏
- `esp32-s3-touch-lcd-2.8b.cc` —— `WaveshareEsp32s3TouchLCD28B : public WifiBoard`
- `config.json` / `README.md`

注册链路（三处缺一不可）：

1. `main\Kconfig.projbuild` → `config BOARD_TYPE_WAVESHARE_ESP32_S3_TOUCH_LCD_2_8B`
2. `main\CMakeLists.txt` → `set(BOARD_DIR "waveshare/esp32-s3-touch-lcd-2.8b")` + 字体内置
3. 板子目录本身 `config.json`（`scripts/build.py` 用它生成 build 条目）

---

## 3. 编译与烧录

```powershell
# 编译（约 25 分钟全量 / 1.5 分钟增量）
powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
  "D:\Users\PC\Desktop\esp32-mgxd\tools\idfenv.ps1" `
  -Command "python scripts/build.py waveshare/esp32-s3-touch-lcd-2.8b --name esp32-s3-touch-lcd-2.8b --wake-word disabled"

# 只烧 app 分区（保留 WiFi 配置与激活状态，首选）
C:\Users\PC\.espressif\python_env\idf6.1_py3.13_env\Scripts\python.exe -m esptool `
  --chip esp32s3 -p COM5 -b 921600 --before default-reset --after hard-reset `
  write-flash 0x20000 <repo>\build\xiaozhi.bin
```

注意事项：

- `scripts/build.py` 结束固定返回 exit code 1（构建成功后多跑了一次 configure 被截断），日志里有 `Build completed successfully!` 就是成功。
- **整包 `merged-binary.bin` 会连 NVS 一起覆盖**（0x0–0xA91E78 含 0x9000 的 nvs），烧完 WiFi 配置和激活状态全丢，要重新配网 + 重新绑定。日常改动只用 `0x20000`。
- 串口是 USB-Serial-JTAG：**DTR 接 GPIO0、RTS 接 EN**，所以读串口必须 `DtrEnable = $false`，否则芯片被按在下载模式、一个字都不出。复位用 `RtsEnable` 脉冲 350ms。

---

## 4. 踩过的坑（都已修）

| 现象 | 根因 | 修法 |
|------|------|------|
| 开机 `io_expander: Pin[0] can't set level in input mode` → 无限重启 | TCA9554 驱动禁止对仍配置为输入的脚写电平 | 先 `esp_io_expander_set_dir()` 设方向，再逐个 `set_level` |
| 触摸初始化失败 | GT911 复位时 INT 电平决定地址是 0x5D 还是 0x14 | 两个地址都 `i2c_master_probe`，用命中的那个 |
| 一直黑屏 + 控制台最后一行是 `Ota: Current version` 再无输出 | `esp_ssl.cc` 的 `esp_tls_conn_new_sync` 没设 `timeout_ms`，握手卡死就永久阻塞 | 修网络（见下条），并在板级加联网看门狗 |
| HTTPS 时通时不通 | 家里有两个同名 SSID `601` 的 AP，其中一个 DHCP 能过但数据转发死了 | 板级 `netcheck` 任务：5 秒后探网关/公网 DNS，不通就把当前 BSSID 拉黑并重新扫描连另一个 AP |
| 即使连对 AP 也偶尔 TLS 超时 | Wi-Fi 省电模式 + HT40 | `esp_wifi_set_ps(WIFI_PS_NONE)` + `esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW20)` |
| 云端不下发 6 位验证码，只回 202 | 板子 eFuse 里是文本序列号 → 请求头带 `Activation-Version: 2`，走 challenge 流程 | 本板强制清空 serial_number_，退回 v1 |
| 纯色测试时屏幕全黑 | 背光在构造函数最后才点亮，测试跑在它前面 | 测试前先 `GetBacklight()->SetBrightness()`（已删，仅教训） |

---

## 5. 已验证 / 已知限制

已验证：

- 屏幕：RGB 纯色测试 白/红/绿/蓝/黑 全部正确 —— 面板、16 根数据线、ST7701S 初始化序列、RGB565 格式都没问题。
  （之前照片里偏蓝是手机相机白平衡；小智主题的浅色背景本身带一点冷调。）
- 触摸、RTC、IMU、SD 卡、蜂鸣器（低电平关闭）都正常。
- 配网 → OTA → MQTT → `Activation done` → `idle` 全链路打通。

限制：

- **无音频**：原理图、wiki 引脚表、I2C 实扫三重确认板上没有 ES8311/ES7210，也没有 I2S 走线。固件用 `DummyAudioCodec`。
- 开机到 idle 要 25 秒左右，几乎全花在 OTA 请求上（`cost=24940`），属网络问题不是固件问题。

---

## 6. 如果想继续

1. **加语音**：需要外接 I2S 麦克风 + I2S 功放喇叭（占了 6 个 GPIO），或换一块自带 ES8311 的板子。
2. **关掉家里那个坏的中继 AP**，比在固件里绕更彻底（现在的看门狗已经能自动躲开）。
3. 把 `main/ota.cc` 里那段强制 v1 的 `#if defined(CONFIG_BOARD_TYPE_WAVESHARE_ESP32_S3_TOUCH_LCD_2_8B)` 换成本板专用的选项开关，免得以后影响别的板子。

---

## 7. PocketPlan 复用这套板级驱动（2026-10-23）

PocketPlan（`D:\Users\PC\Desktop\esp32-mgxd\pocketplan\`）把本报告的移植成果整体复用，没有重写板级代码：

| 能力 | 复用方式 | 与 xiaozhi 的差异 |
|---|---|---|
| 背光 / I2C / TCA9554 / ST7701 RGB / GT911 | 直接照搬初始化序列与引脚 | 无音频编解码器、无 ES8311/ES7210 初始化 |
| LVGL | `esp_lvgl_port 2.9` + LVGL 9.5 | **任务栈 4KB → 8KB**：阅读器排版在小栈上会栈溢出 |
| SD 卡 | 同一套 SPI 模式挂载（20MHz 失败退 400kHz） | 只读为主，写走 WebDAV |
| 蜂鸣器 | 保留（P7 低电平关） | **M12 起用于闹钟响铃**：`pp_board_buzzer(bool)` 直接写 TCA9554 P7 |
| 触摸 | GT911 @ 0x5D | 同一驱动，无手势逻辑 |

**新的硬件需求（PocketPlan M4 才有）**：板子没有 I2S 走线，要出声必须外接 PCM5102A 模块 —— GPIO43 (BCLK) / GPIO44 (LRCK) / GPIO4 (DIN) + 3.3V/GND，占 3 个空闲 GPIO。焊上之前固件里 `music_ready=false`，音乐页只显示橙色提示。

**两条与板子强相关的固件级教训**（都写进了进度文档 §七）：

1. **Wi-Fi 必须比 UI 先初始化**：`esp_wifi_init` 需要一整块连续内部 DMA RAM，UI 建完只剩 46KB 时直接 `ESP_ERR_NO_MEM`。
2. **`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL` 必须是 0**：设成 16384 会让所有 <16KB 的 malloc 落内部 RAM，列表类 UI 几十秒就把内部堆吃光，随后 SD 读失败（`allocate_dma_buf: not enough mem, err=0x101`）。

### 7.1 显示链路补充：换字体与取屏（2026-10-23 23:5x）

| 事项 | 与板级驱动的关系 |
|---|---|
| 自定义 1bpp 中文字体 | 面板仍是 `LCD_COLOR_FMT_RGB565` / `LV_COLOR_FORMAT_RGB565` / `CONFIG_LV_COLOR_DEPTH=16`，**没动色彩链路**；中文缺字是 LVGL 字体问题，不是面板问题 |
| 全局字体注入 | 在 `pp_board_display()` 拿到 `lv_display_t*` 之后 `lv_theme_default_init(disp, 黑, 黑, false, &pp_font_16)` + `lv_display_set_theme()`，一次生效于所有控件 |
| 取屏（截图） | 面板是 RGB direct-mode **双缓冲**，`lv_display_get_buf_active()` 在大重绘后可能仍指上一帧 → 截图改用 `lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_RGB565)`，并且取像素必须全程在 `lvgl_port_lock()` 内（要开 `CONFIG_LV_USE_SNAPSHOT=y`） |
| 传图 | `CONFIG_LWIP_TCP_SND_BUF_DEFAULT` / `TCP_WND_DEFAULT` = 5760，往返 200ms 时 TCP 只有 ~28KB/s；截图因此做成 1bpp PBM（38KB），更大的图必须走串口 |
| LVGL 任务栈 | 8KB 仍够用（截图整屏渲染、阅读器排版都在这个任务里跑通）；若以后加大字号或整屏刷新频率，建议再涨到 12KB |

### 7.2 横屏（专注模式）为什么不能用 display rotation（2026-10-24 00:2x）

1. `lv_display_set_rotation(disp, LV_DISPLAY_ROTATION_90)`（`lv_display.c:996`）只是改 `disp->rotation` 再 `update_resolution()`；渲染模式为 **direct / full refresh** 时（本板 RGB 面板就是 direct mode），旋转必须再叠一个 `lv_display_set_matrix_rotation()`（`lv_display.c:1012`），而它要求 `LV_DRAW_TRANSFORM_USE_MATRIX`。
2. 本工程 `CONFIG_LV_USE_MATRIX` **未开**（`sdkconfig:4109`），开了还要过 `LV_DRAW_TRANSFORM_USE_MATRIX` 的渲染路径，且矩阵变换要一整屏 640×480×2 ≈ **600KB** 的转换层（进 PSRAM 也不便宜），并触发 13 分钟全量重编。
3. `esp_lvgl_port` 的 `lvgl_port_rotation_cfg_t {swap_xy, mirror_x, mirror_y}` 注释写明 **Only HW state. Not supported for default SW rotation** —— 对 RGB 面板改这些只会错位，不会得到横屏。

采用的替代方案：**只对专注页那几个 label 做 90° `lv_obj_set_style_transform_rotation(obj, 900, 0)`**（pivot 50%/50%），坐标按竖屏帧缓冲手工映射 `ox=-(v-240), oy=(u-320)`。代价是每个标签一个独立变换（标签多了吃 CPU），好处是不碰 sdkconfig、不占整屏内存、1bpp 大字旋转后依然锐利。实测持握方向为**设备顶边朝左**（逆时针转设备）。

> 若将来要做「整页横屏」（比如横屏阅读、横屏表格），才值得开 `LV_DRAW_TRANSFORM_USE_MATRIX` 走整屏旋转，那时记得同步改 `lv_conf` 并预留 PSRAM。

### 7.3 蜂鸣器终于用上了：闹钟响铃（2026-10-24 01:3x）

M1–M11 期间蜂鸣器只做「上电确保不响」，M12 的闹钟功能把它变成了唯一的声学输出：

| 事项 | 结论 |
|---|---|
| 硬件能力 | 蜂鸣器在 **TCA9554 P7**，只能写 0/1。没有 PWM、没有频率寄存器 → **做不出音调**，只有「嘀」与「静音」两种状态 |
| 驱动 | `board.h` 新增 `void pp_board_buzzer(bool on)`（内部 `esp_io_expander_set_level(s_expander, PP_EXIO_BUZZER, on ? 1 : 0)`）与 `bool pp_board_buzzer_ready(void)`；`board.c` 初始化时仍先拉低，避免上电长鸣 |
| 节奏 | 独立任务 `ring_task`（栈 2048）350ms 开 / 300ms 关，即每秒约 1.5 次「嘀」，比常响省电也不刺耳；响满 60s 自动停 |
| 与 I2C 的关系 | P7 是 I2C 扩展脚，每次开关是一次 I2C 写（100kHz 下几十分之一毫秒），不会干扰同总线的 GT911/PCF85063/QMI8658 —— 实测响铃期间触摸与时钟读取都正常 |
| 副作用 | 响铃时全屏浮层占住 LVGL，触摸只响应「停止」按钮；停止后原页面原样恢复 |

### 7.4 显示链路补一个字体区间：键盘符号（同轮）

上一轮把全局字体换成 1bpp SimHei 时，覆盖区间是 `0x20-0x7E / 0xA0-0xFF / 0x2010-0x2027 / 0x3000-0x303F / 0x4E00-0x9FA5 / 0xFF00-0xFF5E` + 部分 FontAwesome。M12 发现 LVGL 键盘上的回车、退格、地球、左右箭头全是空心方框 —— 它们用的是 **FontAwesome 私有区**（`LV_SYMBOL_NEW_LINE` = U+F8A2、`LV_SYMBOL_BACKSPACE` = U+F55A 等），不在上面任何区间里。

修法：`.fontgen/gen2.sh` 的 `lv_font_conv` 参数追加 `-r 0xF8A0-0xF8FF` 重新生成 `pp_font_16` / `pp_font_20`（21,318 字形不变，只多几十个图标），重烧后键盘符号全部可见。**与面板、色彩、LVGL 版本都无关**，纯粹是字体覆盖问题。
### 7.5 显示链路的另一个坑：浅灰在纯黑白 UI 上等于没有（2026-10-24 02:5x）

M13 里用户报「计划勾选完成的圈圈有白边」，根因不在面板也不在驱动，而在**配色对比度**：

| 事项 | 结论 |
|---|---|
| 现象 | 打卡圈原来是「白底 + 2px `0xC8C8C8` 边框」，完成时把边框刷黑；1bpp 点阵字体 + 纯黑白配色下，未完成的浅灰环在白底上几乎看不见，完成的黑环里又是白心 → 观感像「白边」 |
| 面板侧无关 | ST7701 RGB565 本身能显示灰阶，问题是**浅灰 200 级在白底 255 级上对比度太低**，再加上没有反锯齿，边界完全糊掉 |
| 截图侧放大问题 | `shot.c` 取屏是 **1bpp PBM**（fold to 1bpp，按中灰阈值判定「墨」）：浅灰被算成白，所以截图里 chip/边框直接消失 —— 看截图时比真机更明显 |
| 结论 | 这种单色风格里不要用灰做「次要层次」，一律纯黑描边 / 黑底白字反白；设置页 chip（`C_CHIP 0xE4E4E4`、`C_EDGE 0xC8C8C8`）已全部改 `0x000000` |
| 附带 | 1bpp 截图仍是排查配色的好工具：凡是截图里「看不出形状」的元素，真机上也一定是弱对比 |

### 7.6 横屏方案复用到了「时钟模式」（2026-10-24 03:0x）

用户要把时钟模式也改成横屏。§7.2 的结论没有变（RGB direct mode 下不做整屏旋转），于是把专注页那套「label 级 90° transform + 手工坐标映射」抽成同一个模式，在 `pocketplan/main/clockmode.c` 里复用：

| 事项 | 做法 |
|---|---|
| 坐标系 | 先按 640×480 的横屏坐标排版，再换算成竖屏帧缓冲里的偏移：`ox = -(v - 480/2)`、`oy = (u - 640/2)`，`lv_obj_align(obj, LV_ALIGN_CENTER, ox, oy)` |
| 旋转 | 每个 label 单独 `lv_obj_set_style_transform_rotation(obj, 900, 0)` + `transform_pivot_x/y = LV_PCT(50)`；900 是顺时针 90°，1bpp 点阵字旋转后依然锐利 |
| 布局 | **不用 flex 容器**：旋转是绘制期变换，布局期仍按未旋转包围盒测量，`LV_SIZE_CONTENT` + flex 在横屏下容易错位；倒数日改成 4 个固定坐标 label（`v = 292 + i*32`），空行 `LV_OBJ_FLAG_HIDDEN` |
| 代价 | 每个带 transform 的对象都是一层独立渲染，标签一多就吃 CPU；时钟页总共 7 个 label（时/日/闹钟/4 条倒数），实测每秒刷新无压力 |
| 截图方向 | 串口/HTTP 取回的是竖屏 480×640 帧缓冲，PC 端要 `PIL.Image.rotate(90, expand=True)` 才是人眼看到的横屏；`docs/screenshots/18-clock-mode-landscape-dark.png`、`19-clock-mode-landscape-light.png` 已经转好 |
| 验证 | `---TOURCLOCK---` / `---TOURCLOCKLIGHT---` 两块截图内容逐字一致，只是黑白相反；`---TOURTAPEXIT active=0---` 证明「轻触 3 下退出」用真实 `LV_EVENT_CLICKED` 也能关掉 overlay |

> 结论：**要横屏而没有整屏旋转能力时，label 级 transform 是够用的**——前提是页面元素少（十几个以内）、排版不用 flex。真要整页横屏（阅读、表格）还是得回到 §7.2 的整屏矩阵方案。

### 7.7 背光就是「自动休眠」的全部（2026-10-24 03:3x）

用户要自动休眠。这块板子的背光在 **GPIO6**，走 LEDC（20 kHz，10 bit，`ledc_channel 0`），也就是说「休眠」在本项目里的物理含义只有一件事：**把 duty 写成 0**。

| 事项 | 做法 / 事实 |
|---|---|
| 熄灭 | `pp_board_backlight(0)` → `ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0)` + `ledc_update_duty`。面板、RGB 时序、LVGL 全都不停，只是没有背光 |
| 唤醒 | `pp_board_backlight(cfg->brightness)`，亮度 10..100 % 映射到 0..1023 |
| 怎么判「没人操作」 | 用 LVGL 的 inactivity clock（`lv_display_get_inactive_time(NULL)`），GT911 触摸驱动每次 read 都会给它打戳——不需要在触摸中断里另外记时间 |
| 怎么验「真的黑了」 | 读回寄存器而不是看状态变量：`ledc_get_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0)`，串口打印 `---TOURSLEEP asleep=1 duty=0---` / `---TOURWAKE asleep=0 duty=603---`（603 ≈ 59% × 1023） |
| 硬件注意 | 背光**没有**接在 TCA9554 上，关背光不需要 I2C 往返；反过来 duty=0 时 GT911 仍能中断，触摸唤醒是可靠的 |
| 没做的部分 | 没进 light sleep、没关 RGB 面板电源、没降频——那些会把 WiFi 和闹钟一起拖下水；代价是待机电流仍是几十 mA 量级 |

> 结论：对这块板子来说「自动休眠」= 背光控制 + 一个空闲计时器，**不需要动电源管理**；真要省电再谈 light sleep。

### 7.8 截图里的颜色是「假的」：滑块紫色其实是浏览器（2026-10-24 03:4x）

用户说「设置页的滑块条是紫色的」。查下来这块屏**没有问题**，是两条「看不见的颜色」叠在一起造成的误判：

| 环节 | 事实 |
|---|---|
| 设备端到底什么颜色 | 在 `pocketplan/main/page_settings.c` 的 `style_slider()` 里，覆盖前先读 LVGL 解析后的样式值并打印：`I (47619) settings: slider theme colour before override: ind=#ff000000 knob=#ff000000`（两个滑块各一条）。**本来就是纯黑**，和 `lv_theme_default_init(disp, 0x000000, 0x000000, ...)` 的预期一致 |
| 截图为什么不可信 | `shot.c` 取屏是 **1bpp PBM**：读 RGB565 后逐像素和阈值比大小，只留「墨/纸」两种结果。**颜色在截图里根本不存在**，紫色也会变成黑，黑色也可能因为亮度略高变成白——所以「截图里是黑的」证明不了屏上是黑的 |
| 真正的紫色 | **网页端** `input[type=range]`：浏览器对没有 `accent-color` 的 range 使用自己的默认强调色（Chrome / Material 是靛紫 `#6200EE` 一类），于是同一个设置页在手机上就是紫色滑块 |
| 修法 | 设备端仍显式钉死 MAIN / INDICATOR / KNOB 三个 part（白底 + 2px 黑描边、黑填充、黑圆钮），不再依赖主题；网页端一度加 `input[type=range]{accent-color:#000}`，**M14.4 按用户要求又撤掉、恢复浏览器默认**（见 §7.9 末尾）。改完 `05-settings.png` 已更新 |
| 教训 | **不要用 1bpp 截图判断颜色**。要判断颜色只有两条路：读 LVGL 的 `lv_obj_get_style_*`（软件侧），或者读面板/背光寄存器（硬件侧）。真要看颜色，用手机拍屏 |

### 7.9 同一类问题的后一半：可交互控件必须自己画出轮廓（2026-10-24 04:1x）

§7.5 讲的是「浅灰等于没有」。这条毛病对**控件**比对文字更致命：用户看不出哪里能点、哪里能拖。设备端两处都中招，两处都不是硬件问题。

| 控件 | 症状 | 原因 | 做法 |
|---|---|---|---|
| 设置页两个 `lv_slider` | 滑块只显示成一根实心黑条，看不出钮在哪 | M14.2 把 MAIN/INDICATOR/KNOB 全钉成纯黑后，**黑钮压在黑填充上**，两者同色；主题默认的钮本来也是 primary 色（黑），以前只是靠 INDICATOR 不是纯黑才勉强分得开 | KNOB 改白底 + 3px 黑描边 + `pad_all 11`：黑轨道上看到白圆，白轨道上看到黑环 |
| AI 页 `lv_keyboard` | 按键只剩字母，符号键（`1#`/`ABC`/`⌫`/`⏎`/`←`/`→`/`✓`）像浮在页面上的散点 | 主题给 buttonmatrix 的按键是白底 + **浅灰**边框，1bpp 屏上边框直接消失 | `LV_PART_ITEMS` 统一白底 + 1px 黑描边 + radius 6 + `&pp_font_20`，`LV_STATE_PRESSED`/`CHECKED` 反色 |

一个硬限制：`lv_buttonmatrix` 的键**不是对象**，v9 里 `LV_BUTTONMATRIX_CTRL_CUSTOM_1` 没有映射到 `LV_STATE_USER_x`，**没法给单个键加样式**，只能整块上样式再靠按下/选中状态反色。想做「回车键更宽更黑」这类细节，得换成每个键一个 `lv_button` 自己排。

> 补充（同日）：网页端那条 `accent-color:#000` 随后按用户要求撤回——「网页端的滑块改回原样」。所以现在**设备端是纯黑 + 白钮黑环，网页端是浏览器默认（部分安卓/Chrome 为靛紫）**，两边不一致是用户选择的，不是漏改。

### 7.10 截图撕裂的真正原因：跨任务调用快照（2026-10-24 04:5x）

`05-settings.png` 曾经左右两半错位、同一行控件出现两次。上一版只加了「重试 3 次 + 失败不回退」，那是在**压症状**；根因在抓图这条链路的三个设计决定上：

| 层次 | 原来的做法 | 为什么坏 |
|---|---|---|
| 线程 | tour / console **任务**里直接 `lv_snapshot_take()`，只靠 `lvgl_port_lock` 互斥 | LVGL 9 的 snapshot 走渲染管线，要求在同一次 LVGL 任务上下文里。从别的任务挤进去时，LVGL 任务可能正停在一帧 flush 中间，display 的 draw buffer 正被改写 |
| 兜底 | 快照失败就 `lv_display_get_buf_active()` 读**面板帧缓冲** | RGB 面板双缓冲 + 直接模式：那块缓冲正在被扫描输出，天生可能半新半旧，一张图里两个 x 偏移 |
| 分配 | 每次快照现场 malloc 480×640×2（约 600KB） | PSRAM 碎片化时成功率不稳，失败越多越常走到上一条 |

现在（`pocketplan/main/shot.c`）：

- `pp_shot_pbm()` 只负责**发请求**：置标志 + `xSemaphoreTake()`，抓图期间不再持有 `lvgl_port_lock`；
- 真正的渲染由跑在 LVGL 任务里的 `lv_timer`（20ms 周期）完成：`lv_obj_invalidate()` + `lv_refr_now()` 把待刷新内容推完，再快照；
- 缓冲第一次 `lv_snapshot_create_draw_buf()` 建 480×640 RGB565，之后 `lv_snapshot_reshape_draw_buf()` + `lv_snapshot_take_to_draw_buf()` **复用同一块**，不再逐次 malloc；
- **任何情况下都不读面板帧缓冲**；失败就返回错误，串口打 `---PBMFAIL ...---`；
- 同一时刻只允许一张抓图（`s_snap_state`），8 秒超时后排空信号量，避免上一次的完成串到下一次请求。

判据也换成可验证的：tour 里加了 `---TOURSETSETTINGS2---`，对同一静止页面**连拍两张**。修复后两张 PNG 的 sha256 完全相同（各 6,368 字节），19 块截图里 `snapshot failed` / `PBMFAIL` 计数为 0。以后改 UI，这条自检仍然有效：连拍不一致就说明又在读半帧。

> 顺带修掉一个会误导人的现象：阅读器 16px / 20px 两张曾经一模一样——不是阅读器不认字号，而是开机 config 里 `font_size` 本来就是 20（设备上手动点过 20 档），第一次打开就已经是 20px。tour 现在在第一次打开前显式把字号钉成 16。
