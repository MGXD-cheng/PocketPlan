# TF 卡验证报告（ESP32-S3-Touch-LCD-2.8B / idf 5.6 测试固件 sdtest）

时间：2026-10-23
结论先说：**开发板、接线、SDMMC 1-bit 驱动、FatFS 全部正常；问题在卡本身 —— 这是一张"虚标容量"的卡（报 30000MB，实际可读写只有约 16MB）。**

---

## 1. 硬件与固件侧全部通过

| 项目 | 结果 |
| --- | --- |
| TCA9554 扩展器 | `output set to 0x0F (read back 0x0F)` → SD_D3/CS 拉高，正常 |
| SDMMC 总线 | slot 1 / 1-bit / CLK=GPIO2 / CMD=GPIO1 / D0=GPIO42 / 20 MHz |
| 挂载 | `attempt 1` 即成功，`SD MOUNT OK` |
| 卡身份 | `Name: LYXSD  Type: SDHC  Speed: 20.00 MHz`，`capacity=61440000` 扇区（≈30000 MB），CSD ver=2 |
| 容量报告 | `29968.02 MB total, 29882.44 MB free` |
| 目录遍历 | 正确列出 `Music/ Books/ Pictures/ Videos/ Wallpapers/ System Volume Information/`，文件大小与 PC 一致（如 `R-C.jpg` 923675 字节） |
| 原生块读 | `read sectors 0..1 -> ESP_OK`，引导扇区 `EB 58 90 4D 53 44 4F 53 35 2E 30`（FAT32 "MSDOS5.0"），FSInfo `52 52 61 41`（"RRaA"） |

字节级验证用的是一条**完全绕过 FatFS 的 FAT32 解析路径**（自己读引导扇区算 LBA，再 `sdmmc_read_sectors` 直读），排除 FatFS 干扰：

```
fatprobe: bps=512 spc=32 rsvd=32 nfat=2 fatsz=14985 root_clus=2 data_start=30002
fatprobe: dir cluster 2 first bytes 42 20 00 49 00 6E 00 66      <- 真实长文件名项
fatprobe: found 'PICTURES' -> cluster 9 size 0
fatprobe: dir cluster 9 first bytes 2E 20 20 20 20 20 20 20      <- 真实 "." 项
fatprobe: found 'R-C.JPG' -> cluster 5281 size 923675
fatprobe: file data starts at LBA 198930
FILEDATA [32 bytes]: 00 00 00 ...                                <- 全零！
```

**元数据（引导扇区 / FAT / 目录项，位于 LBA 0~约 15MB）读出来完全正确；文件内容（LBA 198930，约 97MB 偏移）读出来是全零。**

## 2. 排除 FatFS 与写路径

- 所有 FatFS 读接口（`fread` / `read` / 不同 sector 配置 `FATFS_SECTOR_512` vs 4096 / 卸载后重新挂载）都返回**正确字节数**，内容全零 → 不是读接口 bug，也不是缓存问题。
- 写接口自报成功：`fprintf=28 fclose=0 stat size=28`、`posix write=13 fsync=0 close=0 stat size=13` —— 而且**我们新建的测试文件的目录项确实写成功了**（卸载重挂后仍在，size 也对）。

## 3. 决定性实验：容量探针（写→立刻回读）

在卡报告容量范围内挑四个远离已用区的偏移，写 512 字节特征值再回读：

```
cap: LBA  262144 (  128 MB) write=0 read=0 match=2/512 first=00000000
cap: LBA 1048576 (  512 MB) write=0 read=0 match=2/512 first=00000000
cap: LBA 2097152 ( 1024 MB) write=0 read=0 match=2/512 first=00000000
cap: LBA 4194304 ( 2048 MB) write=0 read=0 match=2/512 first=00000000
```

`write=0` 是 `ESP_OK`——**卡对写操作回了"成功"，但数据根本没落盘，回读全是零**。这是虚标容量（counterfeit / shrunken）卡的教科书特征。

再叠加一个事实：我们自己通过 FatFS 新建文件后，**目录项（低 LBA）保存成功，文件数据（高 LBA 分配到的簇）回读全零**。说明真实可读写区域只覆盖前面一小段（FAT + 根目录所在，即 **约 16MB 以内**），超出部分写被静默丢弃、读返回零。

## 4. 结论

- 开发板 TF 卡槽、GPIO1/2/42、TCA9554 控制的 SD_D3/CS、SDMMC 1-bit 驱动、FAT32 解析全部**已验证可用**。
- 这张 `LYXSD 30000MB` 卡的**真实可用容量约 16MB**，其余 29GB 是伪造的：能挂载、能列目录、能显示文件大小，但文件内容是零、新数据写不进去。这也解释了为什么"写成功却读回全零"。

## 5. 建议

1. **换一张正品卡**（8~32GB、FAT32 格式化）再跑同一个测试固件即可立刻确认 —— 预期 `FILEDATA` 出现 `FF D8 FF` 的 JPEG 头，写读测试 `match` 全中。
2. 想继续用这张卡的话，在 PC 上用 `h2testw` 或 Windows 的"完整格式化（不要快速格式化）"可以量出真实容量；结果大概率是十几 MB 或直接报错。
3. 换卡验证通过后，把 xiaozhi 固件刷回：
   ```
   write-flash 0x20000 D:\Users\PC\Desktop\esp32-mgxd\xz\src\xiaozhi-esp32-main\build\xiaozhi.bin
   ```

## 6. 复测：换卡后全部通过（2026-10-23 19:0x）

用户换上一张 8GB 卡，同一份测试固件、未做任何代码改动，直接复位重跑：

```
I (362) sdtest: === SD MOUNT OK ===
I (992) sdtest: card: 7678.09 MB total, 6912.28 MB free
I (1052) sdtest:   <约 100 个 .MP3>   3458737 bytes ...
I (1112) sdtest:   西游记.TXT   14147359 bytes      <- 名字 GBK 显示为乱码，实为中文名
I (412) sdtest: fatprobe: bps=0 spc=0 ... unexpected BPB   <- 这张卡是 exFAT，不是 FAT32
cap: LBA  262144 (  128 MB) write=0 read=0 match=512/512 first=5A5B5859
cap: LBA 1048576 (  512 MB) write=0 read=0 match=512/512 first=5A5B5859
cap: LBA 2097152 ( 1024 MB) write=0 read=0 match=512/512 first=5A5B5859
cap: LBA 4194304 ( 2048 MB) write=0 read=0 match=512/512 first=5A5B5859
map: LBA   38194 (   4 MB) err=0 nonzero=508 first=160C0940
map: LBA  324914 ( 144 MB) err=0 nonzero=506 first=C97D6927
readback fread -> 28 bytes
READBACK [28 bytes]: 70 6F 63 6B 65 74 70 6C 61 6E 20 73 64 20 77 72 69 74 65 20 74 65 73 74 20 6F 6B 0A
```

- **写→回读 `match=512/512`，128MB / 512MB / 1GB / 2GB 四个偏移全部命中** → 容量真实。
- 数据区每 4MB 抽样 `nonzero≈500` → 盘上有真实数据。
- `READBACK` 内容 = `pocketplan sd write test ok\n` → **文件级读写在 FatFS 上完全正常**。
- 结论：开发板 TF 卡槽整条链路（GPIO1/2/42 + TCA9554 控制 SD_D3/CS + SDMMC 1-bit + FatFS）**全部验证通过**，之前那张 30000MB 卡确认为虚标卡。

两点产品级注意：
1. 这张卡是 **exFAT**（引导扇区 OEM 名 `android `），不是 FAT32；IDF 的 FatFS 已能读写，但分区/格式化策略要明确支持 exFAT 或要求 FAT32。
2. 中文名显示为乱码（`CONFIG_FATFS_CODEPAGE=437`）→ 产品里必须改 `CONFIG_FATFS_CODEPAGE_936` 并做 GBK→UTF-8 转换。

---

## 7. 复现命令


```powershell
# 编译
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "D:\Users\PC\Desktop\esp32-mgxd\tools\idfenv.ps1" `
  -WorkDir "D:\Users\PC\Desktop\esp32-mgxd\sdtest" -Command "idf.py build"

# 烧 app（保留 NVS / WiFi 配置 / xiaozhi 激活状态）
C:\Users\PC\.espressif\python_env\idf6.1_py3.13_env\Scripts\python.exe -m esptool --chip esp32s3 -p COM5 `
  -b 921600 --before default-reset --after hard-reset write-flash 0x20000 `
  "D:\Users\PC\Desktop\esp32-mgxd\sdtest\build\sdtest.bin"
```

---

## 8. PocketPlan 里的读卡实测（2026-10-23，同一张卡）

PocketPlan 固件（ESP-IDF v6.1 + LVGL 9.5）在同一张卡上的实测结果：

| 项目 | 结果 |
|---|---|
| 挂载 | `SD card mounted`，容量 **7,681 MB**，exFAT 读写正常 |
| 根目录扫描 120 项 | `opendir` + `stat` + 排序 **128 ms** |
| 中文长文件名 | 正常显示（`周杰伦-等你下课(with 杨瑞代).mp3`） |
| 14 MB 文本流式读取 | `/sdcard/诡秘之主.txt`（14,147,359 字节）逐页读取，翻页 off 0 → 469 → 1277 → 2385 → 3286，无卡顿 |
| 写路径 | WebDAV `PUT` 建 1KB 文件、`MKCOL` 建目录、`DELETE` 删除全部成功 |

**一处对 §6 结论的修正**：当时建议「`CONFIG_FATFS_CODEPAGE_936` + GBK→UTF-8 转换」，实际更简单的做法是直接设 `CONFIG_FATFS_API_ENCODING_UTF_8=y`（CODEPAGE 仍留 936）—— FatFS 直接返回 UTF-8 文件名，`gbk.c` 退化为直通，省掉 196KB 的 GBK 查表。

**踩到的坑**：内部 RAM 被 LVGL 对象吃光后，SD 读会报 `E diskio_sdmmc: sdmmc_read_blocks failed (0x101)`，看起来像卡的问题，实际是 `allocate_dma_buf` 拿不到连续内存。判断方法：看日志里紧跟的 `E sdmmc_cmd: allocate_dma_buf: not enough mem`。修法是 `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=0`。

**再补两条（2026-10-23 23:5x，字体换成 1bpp 后复测）**

- 换字体不改变读卡表现：根目录仍是 120 项 / 128 ms 级的扫描，14 MB 文本翻页照旧。但**固件体积涨到 3.43 MB**、app 分区只剩 13%，所以「把整本书打进固件」这条路不可行，书必须留在 SD 卡上走流式读取。
- 读 14 MB 书时内部 RAM 仍要留足（≥100KB）：1bpp 字体的 LVGL 字形缓存比 4bpp 小得多，本轮各页建完反而更宽裕（~124KB），`sdmmc allocate_dma_buf` 的假故障没有再出现。
**再补三条（2026-10-24 00:2x，倒数日 + 专注模式上线后复测）**

- **SPIFFS 里新增 `countdowns.json`**（`/lfs/countdowns.json`，与 `config.json` / `plans.json` 同级，storage 分区 1MB）：倒数日数据不落 SD 卡，原因是它要「断电也不丢、开机立刻可算剩余天数」，而 SD 卡可能被拔走；SPIFFS 当前剩余 931KB，够写几百条。
- SD 卡仍是**唯一**的大文件仓：音乐（`/sdcard/music`）、书（`/sdcard/*.txt`）、WebDAV 上传的任意文件都在这张卡上；固件 3.61MB（app 分区剩 12%）已不允许再往 flash 里塞内容。
- 长时间运行观感：6 个 tab 全建完内部 RAM ~119.9KB（最大连续块 31.7KB），比 5 tab 版本只少 4KB；`status_task` 每 60s 打印一次堆快照，连续读卡 + 翻页场景下没有出现 `allocate_dma_buf` 报错。

**再补四条（2026-10-24 01:3x，数据文件可迁到 SD 卡之后）**

- **SD 卡上多了一个隐藏数据目录**：`/sdcard/.pocketplan/`，里面是 `plans.json` / `countdowns.json` / `alarms.json` / `ai_chat.json`（只在设置里打开「数据存 SD 卡」后才用）。目录第一次写入时自动 `mkdir`，之后按「开关 + 卡已挂载」两个条件决定读哪个位置。
- **迁移实测（真机）**：串口 tour 自检打印 `---TOURCACHE on_sd=1 mig=ESP_OK file=1 plans=3/3 back=ESP_OK lfs=1---`，对应日志 `store: moved 3 data file(s) to /sdcard/.pocketplan` 与 `moved 3 data file(s) to /lfs` —— 3 条计划迁到卡上再迁回来，内容完整（`plans=3/3`），旧文件在新文件落盘之后才删，中途断掉最多是多一份副本，不会丢数据。
- **卡不在时的行为**：开关开着但卡没挂载（拔卡/接触不良）→ `pp_data_write` 返回 `ESP_ERR_INVALID_STATE`，UI 提示「SD 卡未挂载」，**不会静默回退写 /lfs**。这是刻意的：否则同一份计划会在两个位置各有一个版本，插回卡就不知道该信谁。
- **对读卡性能没有影响**：数据文件都在 1–4KB 量级，一次写入几毫秒；14MB 书与音乐仍然走纯流式读取，与数据存哪儿无关。唯一新增的开销是切换开关时的一次目录迁移 + 1.2s 重启。
**再补三条（2026-10-24 02:5x，M13 音乐列表异步化 + 迁移加固）**

- **115 个 mp3 的目录扫描很快，慢的是 UI**：`music: scanned 115 tracks in 681 ms`（冷启动；第二次 205 ms，exFAT 目录项已在缓存里），而同样 115 项建 LVGL 行要 6.2s —— 换成单 label 行 + 建行期间隐藏列表 + 每 20ms 建 40 行后降到 **675–1454 ms**。结论：卡不是瓶颈（`/api/files?path=/music` 也是立刻返回 115 项），不要在读卡侧做优化。
- **数据迁移加了读回校验**：M12 末期出现过一次`alarms: loaded 0 alarms`（闹钟丢失），当时的迁移流程是「写目标 → 删源」；现在写完会**读回比对长度**，不一致就 `ESP_LOGE(TAG, "%s read back short after migration, keeping both", dst)` 并保留两份、不删源。数据丢失的具体触发条件没有复现出来（同一流程后来又跑通多次），所以结论只能是「加固 + 如实记录」，不能说已根因修复。
- **隐藏目录的两面性**：数据放 `/sdcard/.pocketplan/` 后，网页端「文件」页和 WebDAV 都能看到这个目录（隐藏属性在 FAT/exFAT 上不等于不可见）。误删它等于清空计划/倒数日/闹钟/AI 历史 —— 建议用户不要动；要备份就整目录拷走。



