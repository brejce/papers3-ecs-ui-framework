# 墨水屏刷新测试手册（供 m5ui 优化使用）

> 编写 2026-10-08。来源：实验工程 `~/Project/epd_lut_lab`（真机实测，非推算）。
> 适用范围：M5PaperS3（`lgfx::Panel_EPD` 裸面板直驱 + M5GFX）。
> 配套文件：`../guide/DEVELOPER.md`（含 ECS 架构附录）。
> 实验台：`~/Project/epd_lut_lab`（独立工程，不污染 m5ui）。

**重要：本文所有耗时都是实测值。** §〇（机制速览）里的 780/1209/234/171ms 是
另一套口径（单行文字）得的，**全屏下复现不出来**，不要拿它做设计估算（实测全屏 QUALITY 是 1423ms，不是 1209ms）。

---

## 零、先记住四条纪律

1. **墨水屏的视觉问题（哪档灰还在、按下去看不看得见、一次点击刷多大一块）日志一个字都不说**，
   只能编译 + 烧写 + **肉眼验收**；串口只证明「没崩」。
   （旧版这条曾要求「先出 HTML 模拟稿」——**已作废**，UI 改动直接改代码，见 `../design/UI_DESIGN.md` §顶与 `../guide/DEVELOPER.md` §0.1。）
2. **一次只排除一个变量。** 拖影/残留类问题尤其如此——参数一动两个，结论就废了。
3. **诊断代码不进产物。** 探针、自检、临时日志验完立刻删，删完再 build 一次确认。
4. **改完别默认烧写。** 烧写时机由你定。

---

## 一、环境与流程

```bash
# 构建 / 烧写（实验台）
cd ~/Project/epd_lut_lab
source ~/esp-env/env.sh
idf.py build
idf.py -p /dev/ttyACM0 flash        # 设备 /dev/ttyACM0，你在 uucp 组

# 串口：只读、不复位（读启动日志）
~/esp-env/tools/python_env/idf5.5_py3.13_env/bin/python tools/read_serial.py 15

# 串口：后台持续记录，等你按键（跨对话回合也不丢）
~/esp-env/tools/python_env/idf5.5_py3.13_env/bin/python tools/serial_log.py ./serial.log 1200
```

**两条现场规矩**：

- **要抓交互就别复位**。`read_serial.py` 会硬复位芯片（DTR/RTS 一抖就是一次复位）；
  `serial_log.py` 走 `setDTR(False)/setRTS(False)` 不复位，**抓用户操作必须用后者**。
- **烧写前先杀掉记录进程**，否则串口被占。注意 `pkill -f serial_log.py` 会匹配到执行
  这条命令的 shell 自己（命令行里含同一串），要写 `pkill -f 'serial[_]log'`。

---

## 二、机制速查（先记死，否则改不动）

| 项 | 值 / 行为 |
|---|---|
| 逻辑坐标 | **540 × 960**（面板物理 960×540，`offset_rotation = 3`） |
| 灰度 | 16 级（帧缓冲恒 4bit/像素打包） |
| 触摸 | GT911，逻辑坐标 x 0~539 / y 0~959 |
| 绘制 | 只写 PSRAM 帧缓冲，**不碰屏幕** |
| 刷新 | `M5.Display.display(x, y, w, h)` 才真正刷；`display(0,0,0,0)` = 全屏 |
| 自动刷新 | `M5GFX` 默认开（`endWrite()` 里 `if (_auto_display) display(0,0,0,0)`）。要只刷指定区域就 `setAutoDisplay(false)` 自己调 |
| 刷新范围 | `_range_mod` **累积**「这一批改了哪块」 |

### 2.1 `_range_mod` 会累积 —— 这条会骗人

`_range_mod` 记录「自上次成功下发以来画过哪些区域」，**只在 `xQueueSend` 成功后才复位**。
所以下面这种写法**推的是整屏**，不是你以为的那一小块：

```cpp
redraw_all();                              // 顶部状态行 y0..36 和底部按钮 y764..960 都画了
M5.Display.display(0, kBtnY, 540, 196);    // 看起来只推底栏
// 实测日志：请求(0,764,540,196) -> 实际发送(x=0 y=0 w=960 h=540) full=1
```

**规矩：要「画一块、推一块」**，不要一次画完整屏再推一次。

### 2.2 逻辑坐标 ≠ 面板坐标

`display()` 会把传入矩形按 `_internal_rotation` 旋转后才并入刷新范围。实测对照：

| 逻辑请求 | 实际发送 |
|---|---|
| `(0, 0, 540, 960)` | `(0, 0, 960, 540)` 全屏 |
| `(20, 44, 500, 330)` | `(44, 20, 330, 500)` |
| `(0, 764, 540, 196)` | `(0, 0, 36, 540)`（顶/底横条 → 面板的竖条） |

**推屏区域要按面板坐标判断有没有重叠**，不要按逻辑坐标想当然。

### 2.3 档位是双重作用，同一事务不能混档

| 阶段 | 受 `_epd_mode` 影响 |
|---|---|
| 写像素（fillRect/drawString/writeImage） | 决定**量化**：FAST 系列 = 1bit，QUALITY/TEXT = 4bit |
| `display()` 推屏 | 决定**波形**：DU/DU4 = 2 级，GC16/GL16 = 16 级 |

```
✓ QUALITY → 画 → 推    (4bit + GC16)
✗ QUALITY → 画 → 换 FAST → 推   (4bit 数据 + 2 级波形 → 灰度丢失)
✗ 写入 1bit → 换 QUALITY 推     (灰度救不回来)
```

---

## 三、波形（LUT）机制

### 3.1 表结构

波形表是 `uint32_t[]`，**每行 = 一帧，16 列 = 16 级目标灰**，每列 2bit 动作：

| 值 | 含义 |
|---|---|
| 0 | end of data（该像素到此结束） |
| 1 | to_black |
| 2 | to_white |
| 3 | no-op（空转，不加电压） |

面板有五块，按顺序拼在扁平表里：`eraser(4行) → quality(32) → text(32) → fast(10) → fastest(7)`。
`_lut_offset_table[mode]` 记录每块起始行。

### 3.2 **擦除段是擦除段，不是可选项**

`step_text()` / `step_quality()` 会**先插 4 行 `lut_eraser`** 再走 mode 块；`step_fast()` /
`step_fastest()` **不插**。`lut_eraser` 的注释是「現在の階調から中間階調付近にシフトさせる」
——它是整条链里**唯一知道当前状态**的环节（mode 表是按**目标灰**索引的**开环**波形）。

> **实测结论：把已验证干净的表放进 fast 槽（去掉擦除段），照样残留。**
> 所以**自定义波形只能放 quality/text 槽**，fast/fastest 槽别承载自定义波形。

### 3.3 有些表末尾有**空转行**（沉降帧）

`eraser = 2动作+1空转+END`、`fast = 8+1+1`、`fastest = 5+1+1`、`quality = 15+16+1`。
每个原厂表 END 之前都至少有一行 no-op。它给粒子留沉降时间；**但补空转帧并不能治好 text 族的残留**
（试过 1/2/3 行，都留）。

### 3.4 耗时模型（实测拟合，偏差 2~4%）

```
全屏刷新   ≈ 37.3ms × 运行时行数 + 80ms
区域刷新   ≈ 29.5ms × 运行时行数 + 36ms     (区域 500×330 实测)
运行时行数 = 表的行数(含 END 行) + (该槽是否插擦除段 ? 4 : 0)
```

**地板价**：每帧固定扫描 ≈ **28ms**，与脏区面积无关。

> **任何刷新的耗时下限 = 28ms × 运行行数，把区域缩到多小都降不下来。**

**另一个有用的基准：完全没变化的一次刷新 = 66ms**（画面上没有任何像素需要驱动，一帧都不跑，
只剩固定开销）。**成本由「刷新范围的行数」决定，跟「实际变化了多少像素」几乎无关** ——
实测 540×960 全屏范围下，只有几十根细线变了也是 358ms。要省时间**要缩范围，不是缩变化量**（见 §5 之二）。

### 3.5 全屏实测（这些都是真跑出来的）

| 表 | 行数 | 全屏 | 区域 500×330 | 说明 |
|---|---|---|---|---|
| `lut_fastest` (DU4) | 7 | 340ms | 242ms | 双向残留 |
| `lut_fast` (DU) | 10 | 451ms | — | 有痕 |
| `lut_text8` | 13 | 598ms | — | 未在弹窗上测过 |
| `lut_text12` | 17 | 746ms | 532ms | 弹窗关掉后留 popup 印记 |
| `lut_quality13` | 20 | 832ms | **633ms** | ✅ 弹窗唯一干净的一档 |
| `lut_text` (GL16) | 36 | 1448ms | — | 原厂 |
| `lut_quality` (GC16) | 36 | 1423ms | 1097ms | 原厂 |

---

## 四、运行时换表（两个 hook + 一个会伪装成「死机」的陷阱）

### 4.1 注入点

面板在 `init_intenal()` 里把表烘成 `_lut_2pixel`，`config_detail()` **只在那里被读一次**。
所以：

- init 之后改 `config_detail()` 无效
- **不能重跑 `init()`**（重新分配帧缓冲 → 泄漏 ~2.5MB PSRAM + 多开一个 `task_update`）
- **不能靠 `setSleep(false)` 重载**（只切电源）

实验台已给面板加好 hook：

```cpp
panel->config_detail(d);      // 只改表指针和行数
panel->reload_lut_table();    // 只重烘 _lut_2pixel，带回退检查
panel->lut_alloc_steps();     // 查询分配容量（见 4.2）
M5.Display.panel()            // public，拿得到 Panel_EPD*
```

### 4.2 ⚠️ 容量陷阱：换表被拒 → **白屏 + 点哪都没反应**

`init_intenal()` 按**出厂行数**算容量并 malloc：

```
_lut_alloc_steps = 4 + 32 + 32 + 10 + 7 = 85 行
```

`reload_lut_table()` 拒绝任何 `need > 85` 的换表。而 `need = 4 + quality + text + fast + fastest`。
**所以 fast 槽最多只能装 10 行**——装一张 11 行的表就 `need=86`，被拒。

失败现象极具误导性：如果调用方**把清屏放在换表之前**，被拒后屏上只剩刚才那片全白、
**而且没有按钮**，看起来就是「白屏、点不动、只能重启」，跟波形问题完全混淆。
日志里的指纹是 **`touch #N -> miss` 连发**（屏上已经没按钮了）。

**两条规矩**：

1. **给分配留余量**（实验台用 `LUT_STEP_SLACK = 16` → `alloc=101`），并把 `lut_alloc_steps()`
   暴露出来，开机打一行 `alloc=NN rows, worst-case need=MM rows` 自检。
   注意 `need` 要**按槽取最大候选**，不是把注册表所有表加起来（每槽同时只装一张）。
2. **一定要先换表、后清屏。** 换表失败时立刻把界面/按钮画出来并显示 `INSTALL REFUSED`。

### 4.3 vendoring（改组件源码的必经步骤）

放在 `managed_components/` 下的 m5gfx 副本会在下次 `idf.py build` 时被组件管理器拉回上游，
**改动静默丢失**（编译报「has no member named ...」而旧代码里明明有）。要把组件移到项目自己的
`components/` 目录（本地组件优先级更高），并从 `main/idf_component.yml` 去掉对应依赖。

---

## 五、区域画面管理：弹窗（存/还原）与画布（虚拟视口）

### 5.1 先排除一个错误做法

**`readRect()` + `pushImage()` 往返是有损的，不能用来「存背景」。**

写路径是 RGB565 → BT.601 → 4bit + **4×4 有序抖动**，且抖动结果**依赖像素位置**。
实测 16 级灰里**只有 0 / 8 / 15 能原样往返**，1–7 与 9–14 各自裂成相邻两级 →
还原后原地留一片**麻点矩形**（正是弹窗最怕的痕迹）。

### 5.2 方案 A（**推荐**）：用 ECS 重绘代替帧缓冲拷贝

m5ui 是**保留式 + 脏标记**渲染器 —— **实体树本身就是那份「背景」**，不需要另存帧缓冲。

```
弹窗打开:  画 popup（压在该区域上） → setEpdMode(QUALITY) → display(该区域)
弹窗关闭:  把被覆盖的实体挂 Dirty   → 渲染器从模型重绘该区域 → setEpdMode(QUALITY) → display(该区域)
```

**优点**：不需要 vendor m5gfx、不需要新 hook、不占 PSRAM。
**代价**：还是那一次区域刷新的时间（见 §6）。

注意**重绘那一整块要用同一个档位**（`display()` 只用最后一次 set 的 `_epd_mode` 推屏）。
弹窗还原属于「清累积重影」，用 **QUALITY/GC16**。

### 5.3 方案 B（需要 vendor）：帧缓冲区域精确拷贝

适合被覆盖的内容**不由 ECS 管理**的场景（例如整页一个 `CustomDraw` 位图）。

实验台已在 vendored 面板上加好一对 hook：

```cpp
// 4bpp 打包，行距 (w+1)/2 字节，高半字节 = 偶数列
// 坐标映射与 _draw_pixels() 完全一致（含旋转），save/restore 成对调用自动抵消旋转
static size_t Panel_EPD::region_bytes(int32_t w, int32_t h);   // = (w+1)/2 * h
bool Panel_EPD::copy_region_out(int32_t x, int32_t y, int32_t w, int32_t h, uint8_t* dst);
bool Panel_EPD::copy_region_in (int32_t x, int32_t y, int32_t w, int32_t h, const uint8_t* src);
```

- 直接读写 `_buf` 的 nibble，**不经过 RGB / BT.601 / Bayer 抖动** → **精确往返**
  （实测带灰阶内容 5 次往返全 EXACT）
- 缓冲大小：500×330 只要 **82.5KB**（PSRAM 随便放）
- **`copy_region_in()` 之后仍要 `display(x,y,w,h)` 才会推屏**

```cpp
// 用法
if (panel->copy_region_out(AX, AY, AW, AH, saved)) {   // 弹窗出现前
  draw_popup(); flush_rect(AX, AY, AW, AH);
}
// 关闭时
panel->copy_region_in(AX, AY, AW, AH, saved);
flush_rect(AX, AY, AW, AH);
```

---

## 五之二、画布（虚拟视口）：画面比屏幕大、要平移时才用

**先纠正一个常见误解**：直接画到屏幕坐标之外的内容**不会被保存，会被裁掉丢弃**。
面板帧缓冲是按屏幕大小精确分配的（`_buf = heap_caps_malloc(panel_w * panel_h / 2)`），
没有任何屏外区域。（`offset_x/offset_y` 是给「显存大于屏幕」的滚动用的，但 `_buf` 没留这部分。）

要做到「内存里的大画面 + 移动视口」，机制是 **`M5Canvas`（= `LGFX_Sprite`）**：

```cpp
M5Canvas canvas(&M5.Display);          // 构造时 _psram = true，缓冲进 PSRAM
canvas.setColorDepth(4);               // 4bpp = 16 级，正好与面板同深度
canvas.createSprite(1080, 960);        // 任意尺寸（实测 506KB PSRAM，开机即分配成功）
drawScene(canvas);                     // 随便画，包括屏幕外的部分（裁到画布、不是屏幕）
...
canvas.pushSprite(-ox, 0);             // 负的目标坐标 = 显示画布内部 (ox,0) 起的窗口
```

关键在 `push_sprite` 的实现（`LGFX_Sprite.hpp:421`）：它把**整张画布**推到目标 `(x,y)`，
**由目标侧裁剪** —— 所以传**负坐标**就等价于「看画布内部的某个窗口」，这就是平移。

相关 API（vendored 源码位置）：

| 接口 | 位置 |
|---|---|
| `class M5Canvas : public lgfx::LGFX_Sprite`，构造时 `_psram = true` | `M5GFX.h:277-281` |
| `createSprite(w,h)` / `setColorDepth(bpp)` | `LGFX_Sprite.hpp:172, 308` |
| `pushSprite(x,y)` / `pushSprite(dst,x,y)` / 带透明色重载 | `LGFX_Sprite.hpp:334-338` |
| `getBuffer()` 取裸缓冲（做「只推变化条」要用） | `LGFX_Sprite.hpp:118` |
| `copyRect(dst_x,dst_y,w,h,src_x,src_y)` 子矩形拷贝 | `LGFX_Sprite.hpp:88` |

### 实测（1080×960 画布，窗口 540×960，每步平移 90px）

| 情形 | 耗时 |
|---|---|
| 平移一步（`pushSprite` + `display()`，FAST 档） | **358ms** |
| 内容大改后的第一次刷新 | 451ms |
| **完全没变化的一次刷新** | **66ms** |
| 放手后 QUALITY 清残影（全屏） | 1424ms |

> **那个 66ms 是个有用的基准**：画面没变时一帧都不驱动，只剩固定扫描开销。
> 它从另一头证实了差分确实在工作。

### ⚠️ 反直觉的坑：平移一步的成本跟「变化了多少像素」几乎无关

`pushSprite` 推整张画布 → `_range_mod` 被并成**整屏** → 而成本大头是
**「范围决定的行扫描」，不是变化量**。

**所以想让平移变便宜，要缩的是「范围」不是「变化量」**：不能推整张画布，
要从 `getBuffer()` 里把**新露出来那条带**抠出来单独 `pushImage()`，
再只 `display()` 那一条。否则永远付全屏的价。

### 另外三条

- `pushSprite` 走的是和 `fillRect` **同一条量化路径**（RGB565 → BT.601 → 4bit + Bayer 抖动），
  所以**别指望「画布→屏幕→读回」精确**（这正是弹窗不能用 `readRect()` 的原因，见 §5.1）。
- **别让画布和 ECS 变成两份「真相」**。m5ui 是保留式渲染器，实体树本身就是那份画面；
  再上一张画布就有两个模型，两边一旦不一致就是新 bug 源。
- **画布真正值钱的场景只有一个：画面比屏幕大、要平移/缩放**（省下重绘整场的 CPU，只做一次 blit）。
  **弹窗不值得为它上画布** —— m5ui 用实体树重绘就够（方案 A）。

交互节奏（墨水屏标准做法）：**拖动过程用 FAST/DU**（358ms/步，有残影但可接受）→
**放手后用一次 QUALITY 收尾**（1424ms 全屏）把累积残影清掉。

### 验证记录

实验台已实机验证：`createSprite(1080,960) depth=4` 分配成功（PSRAM 剩 6246 KiB），
左右各平移 6 步视觉行为符合预期，0 panic。代码见 `~/Project/epd_lut_lab/main/main.cpp`
的 `canvas_*` 函数与 `PAN CANVAS` 按钮。

---

## 六、弹窗波形成本（实测，8 轮累积 + 装置已验公平）

| 档 | 表 | 行数 | 区域耗时 | 残留 |
|---|---|---|---|---|
| EPD_FASTEST | `lut_fastest` | 7 | 242ms | ❌ 双向残留（底下内容透出 + popup 印子） |
| EPD_TEXT | `lut_text12` | 17 | 532ms | ❌ 关窗后 popup 印记 |
| **EPD_QUALITY** | **`lut_quality13`** | 20 | **633ms** | **✅ 干净** |
| EPD_QUALITY | 原厂 `lut_quality` | 36 | 1097ms | ✅（没必要，慢 464ms） |

**所以弹窗开/关各用一次 `epd_quality` + `lut_quality13`，各 633ms。**

### 为什么只有 quality 族行（这是原理，不是巧合）

- `lut_quality` 的前 5 行是**整行推白 → 整行推黑**（把整屏状态**均一化**）
- `lut_text` 的前 5 行是**逐列反向驱动**（等价于原地反转，**不归一化**状态）

所以 text 族不管怎么补沉降帧都治不好「关窗后 popup 印记」。**这是选族，不是调参。**

### 能省的只有一处（零风险）

**关窗时如果那块区域马上要被重绘，就跳过还原** → 直接省掉 633ms。

反例：~~关窗用一次全屏刷新代替区域还原~~ → 全屏 quality13 是 **832ms**，比区域的
**633ms 更慢**，这条路是死的。

---

## 七、怎么测（正确做法 + 三个会给你**假证据**的陷阱）

### 7.1 拖影（ghosting）测试

- 每档 N 轮：`[画高对比图案 → 本档刷屏] → [清白 → 本档刷屏]`，**停在清白那一帧**，残留即该档的清场能力
- 图案用**纯黑白**（各档写入量化不同，只有纯黑白能保证各档画出的图案一致，横向比较才公平）
- **档间必须先用最强清屏统一清干净**，否则上一档的残影叠加到下一档
- 一次跑完立即看，中途别插别的

### 7.2 弹窗残留测试：用**对照法**，别靠记忆

同一屏上放两块**内容完全相同**的区域：

- **A 区**：被弹窗盖住过、然后还原
- **B 区**：全程没人碰

**A 与 B 的差异就是残留**，不需要你记住"之前长什么样"。
另外在**帧缓冲**层面做 `memcmp`：相等 = 拷贝机制精确，剩下的差异只能是物理层的。

### 7.3 ⚠️ 三个陷阱（都真的骗过我）

**① 末尾全屏刷新 → 假「干净」**

测试收尾时如果「先把整介面画完（顶栏+底栏），再推一次」，`_range_mod` 会把上下并成
跨满屏的矩形 → **那一次实际推的是整屏**，用的是**该档自己的波形**：

- 强波形（GC16）把自己的残留擦掉了 → 看起来"干净"是**假象**
- 弱波形擦不掉 → 看起来"脏"

**规矩**：要测残留的区域，测试结束后的任何重绘都不得与它并成同一个矩形。**画一块、推一块**。

**② 每次运行的启动清屏会擦掉上一次的证据**

每档为了从干净状态起跑，开头都会「整屏清白 + 全屏刷新」。所以你**点一次、看一次**；
连点两下同档，第二次的启动清屏会把第一次的残留擦掉，你会误判成「这一档干净」。

**③ 必须有阳性对照**

每轮都要带一个**已知会脏**的档（如 `lut_fastest`）。如果它都看不出脏，说明**测法本身失效了**，
这一轮的"干净"结论一律作废。

### 7.4 怀疑「干净」时上仪器

给 `Panel_EPD::display()` 加一条 printf，打**合成后的刷新矩形**和是否全屏：

```cpp
printf("EPD upd req(%u,%u,%u,%u) -> send(x=%u y=%u w=%u h=%u) mode=%d full=%d\n",
       req_x, req_y, req_w, req_h, xs, ys, upd.w, upd.h, (int)upd.mode,
       (upd.x == 0 && upd.y == 0 && upd.w >= _cfg.panel_width && upd.h >= _cfg.panel_height));
```

这一条 printf 就是抓到陷阱①的手段。**比肉眼看可靠得多，验完删掉。**

### 7.5 光靠日志不行的部分

**EPD 没有光学回读**：`readRect()` / `_read_pixel()` 读的是帧缓冲，不是屏上实际状态。
**画质/拖影结论一律只能靠肉眼**，日志只能证明「交互发生了」「耗时符合模型」「没崩」。

日志只能验「没崩」：

```bash
grep -c 'rst:0x'                    # 应为 1（只有连接串口那次自己触发的复位）
grep -cE 'panic|Guru Meditation'    # 应为 0
```

**连串口这个动作本身就会复位芯片**，所以"刚才是不是崩了"看串口是看不到的 ——
要么在固件里用 `RTC_NOINIT_ATTR` 记一行跨复位的复位原因，要么不复位、被动监听抓现场。

---

## 八、已证伪的路（**别重走**，这些是真机烧了七八轮试出来的）

1. **手工设计 LUT 波形 —— 全部失败。** 试过 7 个变体：单调定向写 / 加闪白闪黑复位 /
   写段加长到 10 帧 / 加末尾沉降帧 1-3-6 行。**全部双向残留。**
   决定性对照：把**已验证干净的 quality13 载荷逐行原样**放进 fast 槽（只去掉擦除段）→ 照样残留。
2. **擦除段不是冗余**，它是必需条件（见 §3.2）。
3. **text 族不够**：`lut_text12` 补 1/2/3 行沉降帧都治不好关窗后的 popup 印记（见 §6）。
4. **补沉降帧不是万能药**：text 族补了也没用。
5. **关窗用全屏刷新代替区域还原更慢**（832ms vs 633ms）。
6. **`readRect()`+`pushImage()` 往返有损**（见 §5.1）。

> 剩下的唯一安全方向：**只裁原厂表**（砍空转行 / 砍尾部动作行），**绝不自己拼动作序列**。

---

## 九、m5ui 集成步骤

### 第一步（零改动）：确认基线

先在固件里把弹窗的几何和内容写出来 → 用 §7.2 的对照法量一次，
记录 A/B 差异，作为后续对比的基线。

### 第二步：接入弹窗

**优先走方案 A（§5.2）**——m5ui 是保留式渲染器，实体树就是背景，不需要 vendor。

需要改的地方：

1. **`main/ui/widgets.h`** — 加一个弹窗部件（`Rect + Style + Text + Touchable + OnClick` + 一个
   `Popup{covering_rect}` 组件记录它盖住了哪块）。**零新增组件类型优先**（组件类型上限 32）。
2. **`main/systems/render.h`** — 弹窗关闭时，把与 `covering_rect` 相交的实体挂 `Dirty`
   并重绘那一整块；推屏用 `setEpdMode(epd_quality)` + **一次** `display(该区域)`。
3. **档位**：弹窗开/关都用 **QUALITY/GC16**（这是清重影的场景，也是实测唯一干净的档）。
4. **推屏纪律**：`display()` 之前 set 档位；**一个部件一个事务**；要「画一块、推一块」（§2.1）。

### 第三步（可选，要省 400ms/次才做）：装 `lut_quality13`

`lut_quality13` = 原厂 `lut_quality` **砍掉 16 行空转帧** → 20 行 633ms（原厂 36 行 1097ms）。

需要 vendor m5gfx（§4.3）+ `reload_lut_table()` hook + 容量余量（§4.2）。
表可以直接用 `~/Project/epd_lut_lab/main/luts.h` 里的 `lut_quality13` 数组。

### 第四步（只有内容不是 ECS 管的才做）：方案 B

vendor 后加 `copy_region_out/in`（§5.3）。

---

## 十、验收清单

- [ ] `grep -c 'rst:0x'` = 1，`grep -cE 'panic|Guru'` = 0
- [ ] 换表失败时不会留下**没有按钮的白屏**（先换表后清屏 + 失败可见提示）
- [ ] 开机日志有 `LUT capacity: alloc=NN rows, worst-case need=MM rows`，且 `NN >= MM`
- [ ] 弹窗残留测试：同一轮里**阳性对照（FASTEST）能看出脏**，否则整轮结论作废
- [ ] 弹窗残留测试：日志里循环期间的推屏**没有 `full=1`**（除开头刻意的全屏清屏）
- [ ] A 区与 B 区肉眼无差异
- [ ] 末尾不再有跨满屏的重绘（画一块推一块）
- [ ] 诊断/探针代码已删干净，删后 build 通过

---

## 十一、变更记录

| 日期 | 版本 | 变更 |
|---|---|---|
| 2026-10-08 | v1.0 | 首版。7 轮真机实验汇总：波形成本模型、擦除段必需、容量陷阱、假证据三陷阱、已证伪清单、m5ui 集成步骤 |
| 2026-10-08 | v1.1 | 补 §5 之二：画布（`M5Canvas`）做可平移虚拟视口——屏外内容不会保存、API 位置、实机数据（平移 358ms/步、无变化 66ms、收尾 1424ms）、"缩范围不是缩变化量"这条反直觉结论 |

---

## 附：实验台文件索引

```
~/Project/epd_lut_lab/
├── components/m5gfx/src/lgfx/v1/platforms/esp32/
│   ├── Panel_EPD.hpp    ← reload_lut_table() / lut_alloc_steps() / copy_region_out|in / region_bytes
│   └── Panel_EPD.cpp    ← build_lut_table() / LUT_STEP_SLACK / copy_region_* 实现
├── main/main.cpp        ← 测试台：DU4(阳性对照) / Q15(干净) / T12(反例)、对照法 A-B 区、
│                           canvas_* 画布平移（PAN CANVAS 按钮）
├── main/luts.h          ← 全部波形表（程序化生成，含 lut_quality13）
├── tools/gen_luts.py    ← 从 m5gfx 源码程序化提取 + 派生（含失败的 flash_bw 反面记录）
├── tools/read_serial.py ← 串口只读（会复位）
├── tools/serial_log.py  ← 串口后台记录（不复位，抓交互用）
└── tools/check_s.py     ← 校验生成表是不是原厂表的逐行前缀
```

**表必须是程序化提取的。** 手抄必错：一行 `~0u, ~0u, ~0u` 有多项，逐行解析会漏
（早期手抄 `lut_text` 第 3、4 行写成 17 个值、丢了 4 行 hold）。一律 `tools/gen_luts.py`
提取 + `sizeof` 核行数 + 与上游源码逐行比对。

---

# 〇、机制速览（原「绘制约定」，从 sketch_jun4a / frame 总结）

> 合并自 EPD_DRAWING_NOTES.md（2026-09-29）。以下为原文：

> 整理时间 2026-09-29。来源：`~/Project/sketch_jun4a`、`~/Project/frame`
> （含 `M5PaperS3_Firmware_API.md`、`EPD_MODE_TEST.md`、`CHANGES.md`、`CORE_NEEDS.md` 与源码）
> 用途：m5ui 的 render/flex/widgets 应当照这套来。

---

## 一、机制硬事实（板子/驱动层面，别记错）

| 项 | 值 |
|---|---|
| 逻辑坐标 | **540 × 960**（面板物理 960×540，`offset_rotation = 3`） |
| 灰度 | 16 级 |
| 触摸 | GT911，坐标空间 x 0~539 / y 0~959 |
| 缓冲 | 绘制只写 PSRAM 帧缓冲（4bit/像素打包），**不碰屏幕** |
| 刷新触发 | **`endWrite()` 自动触发**（M5GFX `Panel.hpp`：`if (_auto_display) display(0,0,0,0)`）。UIFlow 固件 API 里 `M5.Display.display()` 根本不存在 |
| 刷新范围 | `_range_mod` 累积「这一批画改了哪块」，只刷那块 |
| 想指定区域 | `M5.Display.display(x, y, w, h)` —— 会把该矩形并入刷新范围 |

### 四档波形实测（M5PaperS3 的 Panel_EPD，单行文字 / 全屏图片）

| 档位 | 波形 | 文字 | 全屏图片 | 灰度级 | 适用 |
|---|---|---|---|---|---|
| 1 EPD_QUALITY | GC16 | 780ms | 1209ms | 16 | 首屏、图片、防重影全刷 |
| 2 EPD_TEXT | GL16 | 780ms | 1214ms | 16 | 文字（笔画细，双稳态最清晰） |
| 3 EPD_FAST | DU | 234ms | 377ms | 2 | 色块/按钮/矩形，日常默认 |
| 4 EPD_FASTEST | DU4 | 171ms | 281ms | 2 | 极简反馈；跳过重叠检查，连刷会撕裂 |

### 档位是「双重作用」——这是最大的坑

| 阶段 | 受 `_epd_mode` 影响 |
|---|---|
| `writeImage` / fillRect 写像素 | 决定**像素量化**：FAST = 1bit，QUALITY = 4bit |
| `display()` 刷屏 | 决定**刷新波形**：DU/DU4 = 2 级，GC16/GL16 = 16 级 |

**同一事务里不能混档**：写入时是 1bit，后面换 QUALITY 也救不回灰度；写入是 4bit 却用 DU 刷，灰色会被跳过（画面缺浅色）。

```
✓ QUALITY → 画 → 推     (4bit + GC16)
✓ FAST    → 画 → 推     (1bit + DU)
✗ QUALITY → 画 → 换 FAST → 推   (4bit 数据 + 2 级波形 → 灰度丢失)
```

---

## 二、sketch_jun4a（v3：DOM 树 + Flex + TabBar）

- **节点树 + 脏标记**：`Node` 有 `_dirty` / `_any_dirty`，`markDirty()` / `markSubtreeDirty()`
- **绘制循环（核心）**：

  ```cpp
  void Node::update() {
      if (!isActive()) return;
      if (_dirty) {
          M5.Display.setEpdMode(epd_mode);   // 组件自报档位
          draw();                            // 写帧缓冲，endWrite 自动刷
          _dirty = false;                    // 画完清脏
      }
      for (auto c : _children) if (c->isVisible()) c->update();
  }
  ```

  → **没有「每帧重绘」**，脏了才画。
- **组件自己声明三件事**：`epd_mode`、`touch_mask`、`grow`
- 绘制 API 就是 `M5.Display.fillRect / fillRoundRect / drawString / drawCenterString`；
  需要成批时用 `startWrite()` … `endWrite()` 包起来（ListView 就这么干），一次刷新。
- 组件↔档位分配：Label = TEXT，Button/Rect/ListView/BatteryIcon/ProgressBar = FAST（继承默认），Image = FAST（逐块推像素，GC16 全帧会打架）

---

## 三、frame（jun4a 的成熟版，M5PaperS3）

沿用同一套「脏标记 + 组件自报档位」，另外加了这些实机经验：

1. **文字先量后画**（`Label`）
   ```cpp
   config(): _tw = textWidth(text); _text_h = fontHeight(); _clear_w = _tw + 10;
   draw():   fillRect(x, y, _clear_w, _text_h, _bg);   // 先清自己的框，防旧字残留
             setTextColor(_fg); drawString(text, x, y);
   ```
   `setText` 时 `_clear_w` 取「旧宽/新宽」的较大值，否则长文本变短文本会留尾巴。
2. **防重影**：`TextView` 每 N 页强制一次 `EPD_QUALITY` 全刷（`_full_refresh_interval`），日常翻页用 `EPD_TEXT`。
3. **等波形跑完**：`for (int i = 0; i < 60 && M5.Display.displayBusy(); i++) delay(50);`
4. **批量绘制**：`TextView::_renderPage()` = `startWrite()` + `setEpdMode(mode)` + 背景 fillRect + 逐字 drawString + `endWrite()` → **整页只刷一次**。TODO 里还挂着一项「合并 endWrite 省 ~500ms」。
5. **省电**：空闲 → 先画屏保（QUALITY）→ `powerSaveOn()` → Light Sleep；唤醒后 `markSubtreeDirty()` 全量重绘。
6. 组件↔档位：Label = TEXT；Image = QUALITY；Button/Rect 等 = FAST。

### 他们的开发习惯（`CORE_NEEDS.md` 三圈，写代码时要守）
- 先写小测试验证猜想，跑通再整合（`_build1` 模式）
- 极简优先；`.` 点号风格优于 `::` 静态调用
- 接受不了看不懂的代码 → 注释、直观命名、写文档
- 对 e-ink 画质敏感，会反复对比档位；一次只排除一个变量
- EPD 刷新 120~450ms/次，是全系统最慢环节 —— 触摸命中、布局计算都不是瓶颈

---

## 四、m5ui 现在的差距（屏幕那些毛病的根因）

| m5ui 现状 | 参考项目做法 | 呈现的问题 |
|---|---|---|
| render 里从不 `setEpdMode` | 每个部件声明自己的档位 | 一直跑默认档，画质/耗时不可控 |
| `flex_layout()` 每帧给所有子节点挂 `Dirty` | 只有真变了才 `markDirty()` | 三个按钮**每帧重画 → 墨水屏不停刷** |
| 文字用默认 6×8 字体、坐标 +4 硬写 | 先 `textWidth/fontHeight` 量好再画（可居中） | **字太小、位置随意** |
| 没有「先清自己的框」 | Label/Button 都先 fillRect 自己的区域 | 残影、旧内容盖不掉 |
| `EPDMode` 组件定义了但没人读 | 组件声明 epd_mode，渲染时 set | **4 个档位形同虚设** |
| `App::begin()` 里 `clear_display = false` 且从不整屏刷一次 | frame 首次/清屏用 QUALITY 全刷 | 开机残留的深色块（未刷到的区域） |

## 五、照这套改 m5ui，只需动三个 system（不动 ECS 架构）

1. **flex.h** — 子节点 Rect 真的变了才 `add(Dirty{})`（现在是无条件挂）。
2. **render.h** — 遍历 `Dirty` 实体：`setEpdMode(实体 EPDMode 或默认档)` → 画 → `remove<Dirty>`；文字用 `textWidth/fontHeight` 量尺寸并居中；先填自己底色防残影。
3. **widgets.h** — `label()` 记下真实文字尺寸；`button()` 给 `EPDMode{3}(FAST)`、`label()` 给 `EPDMode{2}(TEXT)`；需要整屏重画的场景（首屏）用 `EPDMode{1}(QUALITY)` + `display(0,0,540,960)` + `waitDisplay()`。

> 一句话原则：**脏了才画、组件自报档位、先量后画、先清后画、成批一次刷**。

