# TTF 字体渲染（鸿蒙 HarmonyOS Sans SC）

给 M5Paper 墨水屏跑**真正的 TrueType 字体**：任意字号、任意常用汉字，实时轮廓→位图。

**状态：已验证可用**（2026-09-29 实测，屏幕确认中文清晰、中英混排基线对齐、触摸正常）

---

## 为什么要做这个

M5GFX 自带的 `loadFont()` 吃的是 **VLW**——预烘好的位图点阵，**固定字号 + 固定字符集**。SD 卡里 UIFlow 固件的 `FONTS/HARMON*.VLW` 就是这类。

要「任意字号的 TTF」只有一条路：`stb_truetype` 软件光栅化。

---

## 三个硬约束（不踩不知道）

### 1. stb_truetype 只认 glyf 轮廓，CFF/OTF 直接不认

| 字体 | 格式 | 能用 stb？ |
|---|---|---|
| **鸿蒙 HarmonyOS_SansSC** | glyf | ✅ |
| 霞鹜文楷 LXGW WenKai | glyf | ✅ |
| 文泉驿微米黑 wqy-microhei | glyf（但是 .ttc） | ⚠️ 需先抽单字体 |
| **思源黑体 SourceHanSans** | **CFF** | ❌ |
| **Noto Sans CJK** | **CFF** | ❌ |

本机 `fc-list` 里能用的中文字体看着一大堆，但**思源和 Noto CJK 全是 CFF/OTF，stb 直接拒绝**。选字体前先查轮廓表。

**查法：**
```python
from fontTools.ttLib import TTFont
t = TTFont("xxx.ttf", lazy=True)
print('glyf' in t, 'CFF ' in t)   # 需要 (True, False)
```

### 2. TTC（字体集合）不支持

`wqy-microhei.ttc` 有 2 个字体，stb 不认集合。必须先抽出单个字体再裁剪。

### 3. **8.48MB 字体 > 8MB PSRAM —— 这是最关键的一条**

ESP32-S3 只有 8MB PSRAM。字体**整份 `malloc` 进内存必然失败**。

唯一解：`esp_partition_mmap` 把 flash 分区映射成可随机访问的指针，stb_truetype 直接在映射区上解析——它只读自己需要的那些字节（当前字的轮廓、loca 表项），**零拷贝**。

```cpp
const void* ptr = nullptr;
esp_partition_mmap(part, 0, part->size, ESP_PARTITION_MMAP_DATA, &ptr, &handle);
// ptr 就是 TTF 指针，直接喂给 stbtt_InitFont
```

> 字体文件本体不占 RAM。字形位图（8bit 灰度 + 1bpp 阈值化后的）才走 PSRAM，单个 32px 汉字约 1.4KB。

---

## 分区表：为什么必须改

默认 `partitions_singleapp.csv` 的 `factory` 只有 **1MB**，**现在连固件本身都装不下了**（加了 WiFi + HTTP 文件管理后 1.16MB）—— 所以现在这张表是必需的，不只是为了字体。

`partitions.csv`（已提交）：

| 分区 | 偏移 | 大小 | 用途 |
|---|---|---|---|
| nvs | 0x9000 | 24KB | WiFi 的 PHY 校准数据也存这儿 |
| phy_init | 0xF000 | 4KB | |
| factory | 0x10000 | **3MB** | 固件（实测 1.16MB，余 61%） |
| **fonts** | **0x310000** | **3MB** | 默认字体：HarmonyOS_SansSC 裁切版（1.93MB / 6899 字符） |

合计 6.06MB，余 9.94MB。

> **偏移变过，记住了**：factory 从 1MB 涨到 3MB，fonts 就从 `0x110000` 挪到 **`0x310000`**。
> 改大小的同时**偏移也变了** → 不能只烧分区表，字体必须重烧到新地址（见文末烧录命令）。
> 判断依据还是那条：新表的每个分区 offset 和旧表一致才只烧 0x8000 那张表。
>
> fonts 分区按 **3MB** 留（裁切字体 1.93MB + ~1MB 余量），不再是按全量字体算的 9MB。
> 默认字体文件随项目保存为 **`ttf——file/HarmonyOS_SansSC-subset.ttf`**（2.14MB，glyf、7435 码位、GB2312 全集 6763 汉字）。**必须留着**。
> 源字体现在本机有副本（`~/下载/HarmonyOS+Sans/…/HarmonyOS_Sans_SC.ttf`，19.7MB），所以随时能重切；但只要源字体不在手边，分区一擦就恢复不了默认字体。

**坑：分区表占 0x8000 整个扇区，第一个分区必须从 0x9000 起。** 写 0x7000 会报：

```
CSV Error at line 9: Partitions overlap. Partition sets offset 0x7000.
But partition table occupies the whole sector 0x8000.
```

**改分区表后 `sdkconfig` 里的 `CONFIG_PARTITION_TABLE_SINGLE_APP=y` 必须改成 `CONFIG_PARTITION_TABLE_CUSTOM=y`**——只改 `sdkconfig.defaults` 不生效（`sdkconfig` 里已有值，优先级更高）。

---

## 字体裁剪

**源字体（本机就有，别再以为丢了）**：`~/下载/HarmonyOS+Sans/HarmonyOS Sans/HarmonyOS_Sans_SC.ttf`
= **19.7MB / 29824 字形 / cmap 29508**，是**可变字体**（含 `fvar`/`gvar`/`STAT`）。

裁剪后 **2.14MB / 7435 字符 / 29818 字形**，烧一次 11 秒（分区 3MB，余 ~876KB）。
脚本：`tools/make_font_subset.py`。

```bash
uv run --quiet --with fonttools python3 tools/make_font_subset.py \
  "$HOME/下载/HarmonyOS+Sans/HarmonyOS Sans/HarmonyOS_Sans_SC.ttf" \
  "ttf——file/HarmonyOS_SansSC-subset.ttf"
```

保留的字符集（**按 Unicode 区块整片加**，源字体没有的码位 fontTools 自动跳过）：

| 类别 | 内容 |
|---|---|
| ASCII | `0x20`–`0x7E` 全套 |
| Latin-1 补充 | `× ÷ ° ± § ¶ µ « » ® ©` |
| 常用标点 | `– — ⋯ · • † ‡ ‹ › ※` |
| 字母式符号 | `℃ № ™ Ω` |
| 箭头 | `← → ↑ ↓ ⇧ ⇒` |
| 数学运算符 | `− ≈ ≠ ≤ ≥ ∞ √ ∫` |
| 杂项技术 / 带圈数字 | `⌫ ⏎` / `① ② ③` |
| 制表符 / 方块 / 几何 | `─ │ ┌ ┐` / `★ ☆ □ △ ○ ● ■ ◆ ◇` |
| 杂项符号 / 装饰 | `0x2600`–`0x27BF` |
| CJK 标点 | `。，、「」『』〈〉` |
| 全角形式 | `－ ＋ ＝ ／ ＼ ｜` |
| 中文 | GB2312 **全部** 6763 个汉字 |

> 用 GB2312 全集（6763）而不是「常用字 3500」：**不裁生僻字**是故意的 —— 人名地名里的字不能显示方块。

### ★ 两个必须丢的表（否则装不进分区）

| 表 | 大小 | 为什么丢 |
|---|---|---|
| `gvar`（+`fvar`/`STAT`） | **2.44MB** | 源字体是**可变字体**。**stb_truetype 不支持可变字体**，只读 `glyf` 的默认轮廓，gvar 一个字节都不会读。留着裁完 4.43MB，直接超 3MB 分区 |
| hinting 字节码 | — | stb_truetype **没有 TrueType 字节码解释器**，hinting 指令它也不执行 |

脚本里已写死 `opts.drop_tables.extend(["gvar","fvar","avar","STAT","cvar","HVAR","VVAR","MVAR"])`。
**画质零损失** —— 丢的都是 stb 本来不读的东西。

> 排查心得：裁完体积不对时，**别猜选项，直接比对新旧字体每张表的字节数**
> （`font.reader.tables[tag].length`）—— 一眼就看出是 gvar 多出来的。

**`retain_gids=True` 要保留**（字形索引不变，stb 读到的 gid 才不位移）。
代价是保留全部 29818 个字形槽（含空槽），`loca`+`hmtx` 各 119KB —— 可以接受。

### 字符集审计（改 UI 文案后跑一遍）

```bash
# 把 main/ 里所有字符串字面量抽出来，逐个对 cmap 查缺
uv run --quiet --with fonttools python3 -c "
from fontTools.ttLib import TTFont; import re, os
f=TTFont('ttf——file/HarmonyOS_SansSC-subset.ttf', lazy=True).getBestCmap()
chars=set()
for dp,_,fs in os.walk('main'):
    if 'third_party' in dp: continue
    for fn in fs:
        if not fn.endswith(('.h','.hpp','.cpp')): continue
        s=open(os.path.join(dp,fn),encoding='utf-8',errors='replace').read()
        s=re.sub(r'//[^\n]*','',s)
        for m in re.finditer(r'\"((?:[^\"\\\\]|\\\\.)*)\"',s):
            for c in m.group(1):
                if c!='\\\\': chars.add(c)
print('缺:', [c for c in sorted(chars) if ord(c) not in f and ord(c)>=0x20])
"
```

当前结果：只剩 `⬆ 📁 📄`（都在 `platform/web_files.h` 的 HTML 里，手机浏览器渲染，不进墨水屏字体）。

---

## 烧录

字体和固件是两个独立的 flash 区域，**换字体不用重烧固件**：

```bash
# 烧字体（分区偏移固定 0x310000）
python -m esptool --chip esp32s3 -b 460800 --before default_reset --after hard_reset \
  write_flash --flash_mode dio --flash_size 16MB --flash_freq 80m \
  0x310000 HarmonyOS_SansSC-subset.ttf

# 烧固件
idf.py -p /dev/ttyACM0 flash
```

首次改分区表时必须**全量烧**（含 0x8000 的分区表本身）。

**但只改分区「大小」、不动偏移时不必全量擦**：nvs/factory/fonts 的偏移都没变，只烧 0x8000 那张表就行，字体和应用原地不动（9MB→3MB 就是这么改的）。

```bash
# 只烧分区表（改了大小、偏移没动）
python -m esptool --chip esp32s3 -p /dev/ttyACM0 -b 921600 \
  write_flash --flash_mode dio --flash_size 16MB --flash_freq 80m \
  0x8000 build/partition_table/partition-table.bin
```

改完**回读校验**（别信「Hash of data verified」就完事）：

```bash
python -m esptool --chip esp32s3 -p /dev/ttyACM0 -b 921600 read_flash 0x8000 0x1000 /tmp/pt.bin
python $IDF_PATH/components/partition_table/gen_esp32part.py /tmp/pt.bin
```

字体是否完好，看启动日志 `TTF: loaded 'fonts' 3145728 bytes` + `line_height=27`（3MB = 0x300000 = 3145728）。

---

## 渲染管线

```
flash fonts 分区
    │ esp_partition_mmap（零拷贝，不占 RAM）
    ▼
stbtt_InitFont → fontinfo
    │
    ├─ measure()  量宽度：逐字 stbtt_GetCodepointHMetrics → × scale
    │
    └─ rasterize() 每字：
         stbtt_GetCodepointBitmapBox  → 位图尺寸 + 相对基线偏移
         stbtt_MakeCodepointBitmap    → 8bit 灰度（PSRAM）
         阈值化 >128                   → 1bpp（PSRAM）
         M5.Display.pushImage(palette_1bit)
```

`render.h` 里按 `ttf::loaded()` 分支：字体加载成功走 TTF，失败自动回退 M5GFX 内置点阵字体——**不影响原有功能**。

---

## 踩过的四个坑

### 坑 1：`stbtt_GetFontVMetrics` 返回**设计单位**，不是像素

```cpp
// ❌ 错：把 928（设计单位）当像素 → line_height = 1172px，屏幕才 960 高，全乱
g_ascent = asc;
// ❌ 也错：这个函数的参数是设计单位，返回 928/1000 ≈ 0.9，不是像素
g_ascent = stbtt_ScaleForMappingEmToPixels(&g_font, asc);

// ✅ 对：scale 是「像素/设计单位」，乘一下就得到像素
g_ascent  = (int)(asc  * g_scale);   // 928 × 0.0273 = 25px
g_descent = -(int)(desc * g_scale);   // desc 是负值
```

`stbtt_ScaleForPixelHeight(font, 32)` = `32 / (hhea_ascent - hhea_descent)` = `32 / 1172` = 0.0273。

**同一批错还出现在 `measure()` 和 `rasterize()` 的 advance 计算里**——改一处要全局搜 `ScaleForMappingEmToPixels`。

### 坑 2：`pushImage` 调色板必须是 lgfx 包装类型

```cpp
uint16_t pal[2] = {...};   // ❌ 编译报 "request for member 'get' in ... non-class type"
lgfx::rgb565_t pal[2] = { lgfx::rgb565_t(fg), lgfx::rgb565_t(bg) };   // ✅
```

内部按 `pal[i].get()` 取值，裸 `uint16_t` 没有 `.get()`。

### 坑 3：`int32_t*` vs `int*` 严格类型检查

ESP-IDF 用 `-std=gnu++2b`，`int32_t` 就是 `long int`，和 `int` 不是同一类型。`measure()` 的出参要写 `int32_t*` 才对得上 render.h 里的 `int32_t tw`。

### 坑 4：`'中'` 是多字符常量

```cpp
has_codepoint('中')   // ❌ -Wmultichar 警告，值是几个字节拼的整数
has_codepoint(0x4E2D) // ✅ Unicode 码点
```

---

## 屏幕实测结论

| 字号 | 效果 | 建议 |
|---|---|---|
| 32px | 笔画完整清晰，中英混排基线对齐 | ✅ **最佳区间** |
| 28px | 清晰 | ✅ |
| 20px | 复杂汉字笔画发虚、粘连 | ⚠️ 下限，别低于 24px |

**这台屏写缓冲是 4bit/16 级灰，中文建议 28–32px。** 小字号的「断裂」不是代码 bug，是墨水屏物理限制——笔画密度超过像素密度。

---

## API

```cpp
namespace m5ui::ttf {

bool     load(const char* label, float pixel_size);  // 映射分区并初始化
bool     loaded();                                     // 渲染系统靠它决定走哪条路
void     set_size(float pixel_size);                  // 换字号（不重新解析字体）
void     set_threshold(uint8_t t);                     // 1bit 阈值，默认 128

void     measure(const char* utf8, int32_t* w, int32_t* h);
int32_t  draw_text(int32_t x, int32_t y, const char* utf8, uint32_t fg, uint32_t bg);
int      line_height();
int      has_codepoint(int32_t cp);                    // 字符在不在字体里

}
```

**阈值影响笔画粗细观感**：阈值高 → 细笔画容易断（字显瘦）；低 → 抗锯齿灰边全变黑（字显粗）。32px 中文建议 110–140。

---

## 字体授权

鸿蒙 HarmonyOS Sans 由华为免费商用（**不可修改字库本身、不可单独售卖**）。子集是裁剪产物——如果要做正式商用产品，注意「禁止修改字库」这条，裁剪版严格说算衍生作品，自用/内部测试没问题，对外发布前找官方确认。

官方来源：`https://developer.huawei.com/consumer/cn/design/resource/`
