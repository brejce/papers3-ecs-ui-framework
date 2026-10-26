# papers3 开发者手册

> **版本** 1.0 ｜ **更新** 2026-10-04 ｜ **适用** `~/Project/papers3`（M5PaperS3 / ESP32-S3）
> **读者** 下一个接手的人（或几个月后的自己）。先读这份，再动代码。
>
> **本文管什么**：环境 / 构建 / 烧写 / 分区表 / 目录归属 / ECS 心智模型 / 硬约束（踩过的坑）/ 固件侧管线。
> **本文不重复什么**（去对应权威文档）：
> 界面设计 → `../design/UI_DESIGN.md` ｜ `.so` app 开发 → `APP_AUTHORING.md` ｜ 刷新机制与波形 → `../display/EPD_TEST_MANUAL.md`、`../display/EPD_LUT_MANUAL.md` ｜ 字体裁剪与字符集 → `../display/TTF_FONT.md`
> 全文索引：`../README.md`

---

## 0. 一句话

在 **M5PaperS3**（ESP32-S3R8 / 16MB flash / 8MB Octal PSRAM / 8 位并行直驱 16 灰墨水屏）上，
用自研 ECS UI 框架做的多页应用。字体是运行时栅格化的 TTF（鸿蒙裁切版），固件 1.18MB，实机 0 panic。

**阅读顺序**：本册「附录：ECS 架构说明」（为什么是 ECS）→ 本册正文（怎么用）→ `../display/EPD_TEST_MANUAL.md` §〇（墨水屏机制）。
`HANDOFF.md` 已删（被本册取代）。

---

## 0.1 铁律：**UI 改动直接改代码，不出 HTML 模拟稿**

涉及 UI（新页面、改版面、改配色、改交互、加控件）的改动，流程是：

1. **直接改 `main/` 或对应 app 的 C++**，编译、烧写/上传，**他自己看设备**；
2. 改完**别默认烧写** —— 他说「烧」再烧。

**不要出 HTML 模拟稿。**（用户明确叫停过：「别再出 HTML 模拟稿 —— 模拟稿都货不对版」。）
历史备注：2026-10-04 那版手册里写的是「先生 HTML 模拟，他看过才写固件」，那条**已经作废**，
`design/*.html` 只作为**定稿的参考稿**留着，不作为流程的一部分。

设计规则本身看 `../design/UI_DESIGN.md`（唯一权威），本文不重复。

---

## 1. 环境 / 构建 / 烧写

```bash
source ~/esp-env/env.sh          # ESP-IDF v5.5.5（用 bash 的脚本；fish 用户见下）
cd ~/Project/papers3

./tools/fetch_components.sh      # ★ 第一次 clone 后必须先跑：拉第三方组件（30 秒）
idf.py build
idf.py -p /dev/ttyACM0 flash
```

> ★ **`components/` 不在 git 里** —— 那四个是第三方库（m5gfx / m5unified /
> elf_loader / cmake_utilities，约 165MB），仓库只留我们的本地改动
> （`patches/`，5 个文件）。`./tools/fetch_components.sh` 从 ESP registry 拉官方
> zip + 打补丁还原。改动内容和升级步骤见 [`../../patches/README.md`](../../patches/README.md)。

| 坑 | 说明 |
|---|---|
| python 版本 / IDF_TOOLS_PATH / 换行符 | 见技能 `esp-idf-m5stack-dev`（记了 10 条） |
| fish 空通配符 | `/dev/ttyACM*` 会让**整条命令不执行**，用 `ls /dev \| grep` |
| 看日志别用 monitor | 交互式，直接读串口（脚本见下） |
| **没跑 fetch_components.sh 就 build** | 报 `m5gfx`/`elf_loader` 找不到。先跑那个脚本 |
| **`dependencies.lock` 是死路径** | 它记的是绝对路径，所以进了 `.gitignore`；`fetch_components.sh` 会删掉它，build 时自动重生成 |
| 组件管理器会覆盖补丁 | 这几个组件**不能**在 `main/idf_component.yml` 里声明成 registry 依赖，见该文件顶部注释 |

读串口日志（不经 monitor）：

```python
import time, serial
s = serial.Serial('/dev/ttyACM0', 115200, timeout=0.2)
s.setDTR(False); s.setRTS(True); time.sleep(0.15); s.setRTS(False)   # 硬复位
buf = b''; t0 = time.time()
while time.time() - t0 < 15: buf += s.read(8192)
print(buf.decode('utf-8', 'replace'))
```

### 1.1 判断「刚才是不是崩了重启」—— 有个坑

**读串口本身就会复位芯片**（DTR/RTS 一抖就是一次 USB 复位），所以「连上去看日志」这个动作
会把**上一次的复位原因冲掉**：你看到永远是 `rst:0x15 (USB_UART_CHIP_RESET)`，也就是你自己那次。

固件为此记了一行跨复位的复位原因（`RTC_NOINIT` 内存，不写 flash、不磨 NVS）：

```
I (947) MAIN: 复位: 本次=11(USB（连串口触发的，不是故障）)  上次=11(USB（连串口触发的，不是故障）)
```

`上次` 是**上一次开机是谁触发的** —— 这才是有用的那个。对照表：

| esp_reset_reason | ROM banner | 含义 |
|---|---|---|
| 1 POWERON | `rst:0x1` | 正常上电 |
| 4 **PANIC** | `rst:0xc (RTC_SW_CPU_RST)` | **崩溃**（异常/assert/abort），会带 Backtrace |
| 6 TASK_WDT | `rst:0x7` 等 | 任务看门狗 |
| 9 **BROWNOUT** | — | **掉压**（供电不足，墨水屏全刷瞬间电流大时易发） |
| 11 USB | `rst:0x15` | 连串口触发的，不是故障 |
| 15 CPU_LOCKUP | — | CPU 锁死 |

实测验证过：故意 `abort()` 一次 → 本 boot 打 Backtrace，下一个 boot 报
`本次=4(*** 异常/panic（崩溃）)`，同时 ROM banner 是 `rst:0xc (RTC_SW_CPU_RST)`。

**要抓崩溃现场就别复位**：被动打开串口监听（`setDTR(False); setRTS(False)` 之后不再动），
让它自己崩、自己重启，两个 boot 的日志都留在缓冲区里。

**健康启动日志**（约 4.4 秒，`panic=0`）：

```
I (3487) TTF: loaded 'fonts' 3145728 bytes (flash mmap), line_height=27
I (3570) SD: card: SU01G, 20.0 MHz, 942 MB
I (3714) PYIDX: SD 索引: 400 音节 (43.0 KB)
I (3728) READ: 书目 5 本
I (3730) MAIN: entities: main=15 ime=50 connect=11 reader=16 total=92
I (4321) WIFI: 热点 'M5PaperS3' 已开，浏览器打开 http://192.168.4.1/
I (4341) WEB: 文件管理页面已就绪
```

---

## 2. 分区表（**改了就出事，先看这里**）

```
nvs        0x9000    24K   ← 设置持久化 + WiFi PHY 校准
phy_init   0xF000     4K
factory    0x10000   3M    ← 固件（1.18MB，余 61%）
fonts      0x310000  3M    ← 默认字体（Maple Mono 子集 ≈2.23MB，余 ≈900KB）
```

- **偏移变过**：factory 从 1MB 涨到 3MB 时，fonts 从 `0x110000` 挪到 `0x310000`。
  **这种改动必须重烧字体**，只烧 0x8000 那张表会读不到。
- 烧字体：`esptool write_flash ... 0x310000 fonts/MapleMono-CN-Regular-L1-tree.ttf`
  （字体由 `tools/make_font_maple.py` 裁出来；改了字符集还要重跑 `tools/make_pinyin_index.py`）
- 详见 `../display/TTF_FONT.md`。

---

## 3. 目录归属

```
main/
├── main.cpp        入口：初始化 + 建页 + 主循环（只该做这些）
├── app.h           App：M5 初始化 / World / 主循环
├── ecs/            ECS 内核（world / components）—— 谁都不依赖它俩之外的东西
├── systems/        render / touch / flex（只有这三个）
├── ui/             UI 层
│   ├── theme.h     设计令牌（底/墨/描边/阴影）
│   ├── widgets.h   部件工厂（唯一创建实体的地方）
│   ├── icons.h     图元图标（字体里没有的 ⇧ ⌫ ←→ 一律自绘）
│   ├── keyboard.h  屏幕键盘（输出端 Sink 可插拔）
│   ├── candidate_bar.h  候选栏组件
│   └── qr.h        二维码绘制
├── pages/          页面（每页一个文件：只管本页存储 + 把组件接起来）
│   ├── manager.h   页面管理：多页共存，切页只切 Visible（有 on_enter/on_leave）
│   ├── demo.h      1 演示页        19 实体（含设置入口 + 调试块）
│   ├── ime.h       2 输入法页      19 实体（键盘 1 容器实体）
│   ├── connect.h   3 热点/二维码页 11 实体
│   ├── reader.h    4 Markdown 阅读器 16 实体
│   ├── wifi.h      5 连 WiFi（手机热点）
│   └── settings.h  6 设置（5 屏，含字体选择/字号/NVS）
├── text/           文本与输入法
│   ├── ttf.h       TTF 栅格化（stb_truetype + flash mmap / SD→PSRAM）
│   ├── ime.h       拼音状态机（键盘的一个输出端）
│   ├── pinyin_store.h  索引存储：内置 / SD 文件 / 运行时词频
│   ├── pinyin_index.h  内置索引（生成物，别手改）
│   └── markdown.h  极简 Markdown 解析
├── platform/
│   ├── sd.h        SD 挂载 + 找文件 + 扫 /sdcard/fonts
│   ├── nvs.h       设置持久化（cfg::Config）
│   ├── wifi_ap.h   热点
│   └── web_files.h 网页文件管理
└── third_party/    stb_truetype.h
```

`tools/`：`make_font_subset.py`（裁字体）、`make_pinyin_index.py`、`junda_freq.txt`（字频表）。
`design/settings-neo.html` —— 设置页设计稿（含三档灰度对照、EPD 延迟模拟），**改设置页先改它**。

---

## 4. ECS 心智模型（30 秒版）

- **Entity** = `uint16_t` 编号，**不含任何数据和行为**。
- **Component** = 纯数据 struct，一个组件只管一件事。
- **System** = 普通函数，遍历「有特定组件组合」的实体，干一件事。

```cpp
// 一个按钮 = 5 个组件的组合，没有 Button 类
auto btn = widgets::button(w, 20, 100, 200, 50, "删", on_single);
w.add(btn, OnDoubleClick{on_double, nullptr});   // 双击：就这一行
```

每帧流程（`app.h`）：

```
app.update()
  ├─ flex_layout(world)   遍历 (FlexLayout + Children) 算子节点坐标
  ├─ handle_touch(world)  遍历 (Rect + Touchable + Visible) 命中检测
  └─ render(world)        遍历 (Rect + Style + Dirty) 画脏了的实体
```

**新功能 = 新 Component + 新 System 查询条件。没有新类，没有虚函数。**

---

## 5. 硬约束（**踩过的坑，别再踩**）

1. **档位是双重作用**，同一事务里不能混档：
   - 写像素时决定**量化**（FAST=1bit，QUALITY=4bit）
   - 刷屏时决定**波形**（DU/DU4=2 级，GC16/GL16=16 级）
   - 写入 1bit 后换 QUALITY 刷不出灰；写入 4bit 却用 DU 刷，浅色被跳过。
2. **全屏 ≠ QUALITY。这两个是独立决定**：
   - **全屏**是必需的（切页时旧像素不会自己消失）
   - **档位由「要不要出灰」决定**，不由「推多大」决定。有灰用 TEXT/GL16，比 QUALITY 省 430ms
   - GC16 的真正用途是**清累积重影**（先全黑全白清场再落墨），只在连刷多次 / 首屏 / 手动去残影时升级
3. **按下反馈只改描边+阴影**（阴影收缩 0、描边 +2px），元素和文字**原地不动**。`systems/render.h` 已实现。
4. **擦除按"足迹"**：元素尺寸 **+ 阴影偏移**。擦除和 `display()` 都按足迹算，否则切换时阴影区留黑边。
5. **空文字的左对齐实体不许凭空擦框**。`render.h` 的 `LeftAlign` 分支对空串会算出
   `x0 = Rect.x`、`x1 = x0 + 8`，即**每次标脏都刷一条 8px 宽 × 行高 的白杠**。
   左对齐实体常常和正文**共用同一个 y**（输入法页的拼音条就贴在同一条基线上），
   那条白杠会切掉紧挨光标那个字符靠光标侧的约 1/4（8/34px ≈ 24%，现象就是"字符只剩 3/4"）。
   空文字本来就没东西可画 —— 只擦**自己上次真画过的那块**（`TextBox` 的 `px`/`w`）就够了。
6. **可见性用 `Visible`，没有 `Hidden`**：工厂自动挂上，切页 = 摘掉/挂上。
7. **FATFS 必须开长文件名 + UTF-8**（`CONFIG_FATFS_LFN_HEAP` + `CONFIG_FATFS_API_ENCODING_UTF_8`），
   否则中文名在读目录那层就没了。改它要同时改 `sdkconfig` 和 `sdkconfig.defaults`（前者优先）。
8. **运行时不写 flash 分区**：fonts 分区只放默认字体，换字体 = 读 SD 进 PSRAM。
8. **触摸判据用 `wasPressed()`/`wasReleased()`**，绝不用 `state == touch_end`（长按松手是 `hold_end`）。
9. **双击只给危险操作**：`OnDoubleClick` 的语义是「双击不触发单击，窗口过期才补发单击」，
   所以挂了它的按钮**单击要等 `DOUBLE_MS`(400ms)** 才生效。墨水屏上普通按钮绝不能挂。
10. **热点启动慢**：首次（PHY 校准）~8.8 秒，之后 ~0.2 秒。所以放在**首屏画完之后**开，别挡启动。
11. **字形要按底色合成，`bg` 不能扔**。字形位图是整块包围盒 `pushImage` 上去的，含大量无墨像素。
    若重映射时把无墨处恒定写成白（旧写法 `out = 255 - (255-ink)*cov/255`），
    **黑字白底看不出来，白字黑底就是「白字带一整块白矩形」**。
    正确：`out = bg + (ink - bg) * cov / 255`（cov=0→bg，cov=255→ink）。
    **两条绘制路径都要改**：`draw_text` 和**局部重绘的 `draw_text_from`**（后者原先连 tint 都不调、
    恒画黑墨，而 `LeftAlign` 走的正是它 —— 反白行会变成「黑字 + 白盒子」）。
12. **切页不需要每次整屏 QUALITY 清场**。每个页面建的第一件事就是 `rect(0,0,540,960, 白)` ——
    它自己就整屏盖掉了旧页，而且走 FAST/DU 推一块白底矩形（约 377ms），比 QUALITY 清场的 1209ms 快三倍。
    所以 `widgets::rect()` 用 `EPDMode{3}`（纯色块用不到灰度）。累积重影每 8 次切页用
    `pages::quality_wash()` 清一次（对照 `frame/text_view.cpp:162` 的 `_full_refresh_interval`）。
13. **临时改全局字号必须还原成「原来的」字号，不是硬编码默认值**。
    `ttf::set_size()` 是全局 scale，自绘部件（大标题、开关的 ON/OFF 字样）会在回调里临时改字号。
    还原时用 `ttf::size_px()` 存下的旧值 —— 早先阅读器 `layout()` 结尾还原成硬编码的 `28`，
    结果**读完一本书，全局字号就变成 28**，其它页面的字全跟着变（字号可配之后这就是明确的 bug）。
14. **USB-OTG 与 USB-Serial-JTAG 共用一个 PHY，同时只能用一个** —— 开 USB 键盘后 `/dev/ttyACM0` 就没了。

---

## 6. 四档波形（实测值）

**这一节只留一句结论，完整实测表在 `../display/EPD_TEST_MANUAL.md` §一「机制硬事实」：**

| 档位 | 波形 | 灰阶 | 单行文字 | 全屏图片 |
|---|---|---|---|---|
| 1 QUALITY | GC16 | 16 | 780ms | 1209ms |
| 2 TEXT | GL16 | 16 | 780ms | 1214ms |
| 3 FAST | DU | 2 | 234ms | 377ms |
| 4 FASTEST | DU4 | 2 | 171ms | 281ms |

> ★ **含文字的部件一律 `EPDMode{2}`（TEXT/GL16），不许用 FAST/FASTEST。** 档位是**双重作用**：
> `setEpdMode()` 既决定刷屏波形，**也决定写像素时存几位** —— 2 级波形在**写帧缓冲那一刻**就把
> 抗锯齿的灰量化成非黑即白，之后拿 GL16 再刷一遍也恢复不了。
> 板子是什么（不是 IT8951）、档位双重作用的细节、`_range_mod` 会累积等机制，
> 都在 `../display/EPD_TEST_MANUAL.md` §〇/§二。

---

---

## 7. 设计系统 → EPD

**已并入 `../design/UI_DESIGN.md`**（§2 色板与灰阶 / §3 设计原则 / §4 状态语义）。
那里是唯一权威：彩色稿怎么砍、灰怎么算、强调只用哪三个手段、按下反馈改什么，都在那。

一句话版本：**直角、纯黑描边、无阴影（终端风）/ 硬阴影（固件 widgets）、无圆角无渐变无过渡、
强调只用「反白 / 描边加粗 / 阴影偏移」，颜色不承担语义。**

---

## 8. 墨水屏刷新成本（固件 widgets 侧）

**设计侧纪律在 `../design/UI_DESIGN.md` §9。** 这里是固件 widgets 的落法：

- **一个部件一个事务**：`startWrite()` → 画 → `endWrite()` → `display(x,y,w,h)`。
  别把不同档位的部件塞进同一个 `startWrite`；
- 静态部分只画一次（容器边框画完不再挂 `Dirty`）；
- 光标独立成实体 + FASTEST（只推旧∪新那一小条）；
- 文字只重绘变化段（`TextDirtyRange{from_char}`）。
- 批量重绘（一批脏实体画在一个事务里、只调一次 `display()`）是最值的一步优化。

---

## 8.5 固件 widgets 的风格（Neo-Brutalist 层）

固件自带的 widgets 页（`main/ui/widgets.h` + `theme.h`）仍是 **Neo-Brutalist（Memphis）** 译法：
直角、纯黑描边 2/3/4px、硬阴影 4~8px 零模糊、反白选中态、含灰部件 `EPDMode{2}`。

启动器与全部 `.so` app 用的是**终端风**（另一套：点阵底、无阴影、黑块只给 COMMIT）。
**两套的令牌、选中语言、部件形态都在 `../design/UI_DESIGN.md` §2.5 / §4 / §6。**

---

## 9. 多页面共存与切换

一个 World 里放多个页面，切页只切**可见性** —— 别反复 spawn/destroy。

- 页面显隐**只用 `Visible`**：`render` 和 `touch` 都 `if (!w.has<Visible>(e)) continue;`。
  ⚠️ **摘 `Visible` ≠ 擦除 —— 这是本框架的已知缺口，不是 bug。** 不可见实体在渲染里是
  **跳过**（不画、不擦），触摸也命中不到。所以这套只在「另有一块东西把那片区域重铺掉」时成立：
  跨页 `show()` 每页第一块整屏白底兜着，所以没问题；**同一页内切子视图就没有这个兜底** ——
  阅读页 书目↔正文 只摘 `Visible`，正文和翻页按钮的旧像素会留在屏上，
  而且那些实体此时触摸已失效（表现为"图纸还在但点不动"）。
  真实项目要做的是渲染层 **damage/erase**：实体丢掉 `Visible` 的那一帧，擦掉它上次的足迹
  （每实体记上次的框，先跑一遍擦除再画）。当前是「只画不擦」，够小 demo 用，
  做正经应用前必须先补这一层。临时绕过：子视图切换时显式整屏重铺白底。
- 开机把所有页**都建好**（各自记一份实体表），非首页整页摘掉 `Visible`。
- **切页靠页面自己的白底 `rect(0,0,540,960,白)` 盖住旧页**（FAST/DU，约 377ms），
  不再每次走 QUALITY 清场（1209ms）。每 8 次切页由 `pages::quality_wash()` 清一次累积重影。
  **这条成立的前提：每个页面的第一块实体必须是整屏白底** —— 新建页面时别漏。
- 切过去时给新页所有实体补 `Dirty`（隐藏期间 render 会把它们的脏标摘掉），
  并把左对齐文字的 `TextDirtyRange.from_char` 归 0 强制整段重画。
- **页面进出要有钩子**：`Page` 里有 `on_enter`/`on_leave`。顺序（`manager.h::show()`）：
  ① 摘旧页 `Visible` → ② `on_leave` → ③ `g_ctx.cur = &新页` →
  ④ **统一补 `Visible` + `Dirty`** → ⑤ `on_enter`。
  两个关键点，顺序反了都会出 bug：
  - **统一补 `Visible` 必须在 `on_enter` 之前**。否则本页在 `on_enter` 里**摘掉**的 `Visible`
    会被无条件挂回来，页面自己就管不住可见性了 —— 实测踩过两次：
    阅读页藏起来的正文重新露出来（书目列表下方浮出文章）、字体页摘掉的多余槽位变成空行。
  - **`g_ctx.cur` 要在 `on_enter` 之前就指向新页**，这样钩子里能问出「我是不是当前页」。
    `connect::refresh()` 靠这句决定要不要挂二维码 —— 少了它，开机自动开热点时
    （那时还停在主页）二维码会挂到主页上，就是"一启动二维码就出来了"。
  - `Dirty` 早挂晚挂无所谓：渲染是在 `show()` 返回之后才跑的。
  - **页面自己管可见性的模式**：`on_enter` 里调本页的 `refresh()`，在里面**先判断
    「本页是不是当前页」再决定挂不挂 `Visible`**（见 `connect.h::is_live()`）。
    凡是从页面外被调用的刷新函数（开机自动开热点、主循环 tick）都必须带这个判断，
    否则会在别的页面上把本页实体挂出来。
- **两个硬上限**：`MAX_ENTITIES=256` 且 `spawn()` 只 `_next++`、**从不回收**；
  组件类型上限 32（`ComponentMask` 是 `uint32`），**加新组件类型前先数**。

---

## 10. 屏上键盘与输入法页

> **版面规格与设计规则**在 `../design/UI_DESIGN.md` §8 + 附录 A；本节只记**实现层的坑**。

屏小的时候键盘的取舍（用户明确纠正过）：

- **键高要接近正方形**。540px 放 10 列键宽只有 ~49px，键高再拉到 128px 就成了「又高又瘦」竖条，必须压到 ~46px。
- 行数 4 行（qwerty / asdf / zxcv+功能 / 功能行），**数字行挪到符号页**：`ROW_KEYS = {10, 9, 9, 7}`，`KEY_TOTAL = 35`。
- **三个面板循环**（`PANEL_COUNT = 3`）：0 字母 / 1 数字+中文标点 / 2 英文符号（27 个）。
  面板键的 label = **按下去会去哪个面板**：`?123` → `#+=` → `ABC` → 回字母页。
  英文符号那页是「键盘没有减号」补出来的，顺序按实际会打的频率排（`. , - _` 在最前）。
- **键面标签用中英都能看懂的短 ASCII 码**：`A/a`、`en/cn`、`?123`/`#+=`/`ABC`、`space`、`OK`、`BACK`；光标键用 `←` `→`。
  两条理由：中英读者都认；ASCII 在**任何**子集字体里都有（`←` `→` 是已加进内置子集的 Arrows 区块）。
- **行4 的固定宽度按最大字号 34px 实测排**（`{52,84,50,50,134,50,64}`，合计 484 + 6×6 = 520）。
  标签像素宽 = `adv × size / (hhea.ascent - hhea.descent)`（按**像素高**缩放，不是 `unitsPerEm`）：
  `space` 76.8 / `?123` 62.1 / `OK` 41.8 / `←` `→` 29.0。改字号上限前先重算这张表。
- **`SLOT_MODE`（en/cn）的键面归输出端管**，布局表里那个 `en` 只是初值；`apply_panel()` 拷完布局必须把当前值盖回去，
  否则「中文模式 → 点一下 `?123`」会把键面冲回 `en` 而状态栏还写着 `cn`。
  另：`apply_panel()` 整体重写 `slot[]`，所以 `press()` 里对 `slot[i]` 的引用在调它之后失效 —— 面板键要读完就 return。
- **光标 ← →：插入一直是插在 `c.caret`，缺的只是「移动光标的键」。** 两个 kind（`K_LEFT`/`K_RIGHT`）+
  `press()` 里改 `c.caret` 就够，**不用动插入逻辑**。挪光标走**单独的 `Change.caret` 标志**（与 `text` 互斥），
  UI 层只推光标块 —— 复用「正文变了」会把整个输入框重排重刷。两端不报变化；正在拼拼音时不动拼音缓冲。
- **键盘键的硬阴影要缩小**（2px）：键距只有 6px，用元素那套 5px 会压到隔壁键。
- **候选必须过字符集**：生成时逐个 `ttf::has_codepoint(cp)` 过滤，字体没有的就不出。
- **每个按键的刷新代价 ≈ 1.1s**：键反馈 171ms + 拼音条 171ms + 候选栏 780ms。
  嫌慢就做**合并推送**。
- **实体模型（2026-10-09 重构）**：键盘从 **35 个键实体** 收敛为 **1 个自持容器实体**（`Rect + Style 白底无框无影 + CustomDraw + EPDMode{2}`）；键面 `slot[35]` + 几何 `geo[35]` 由 `Keyboard` 结构体自持，`draw_all()` 一次画完。触摸**不挂 `Touchable`**（挂了每点一键会在 520×208 容器上脏标两次、整块键盘重推两遍），改由 `app.h` 每帧 `ui::kb::tick_g()` 自轮询 `M5.Touch` 对 `geo[]` 算命中；按下只画+只推那一个键的 `(w+2, h+2)` 足迹。

**但真要接 USB HID 之前先认一条硬约束**：ESP32-S3 的 USB-OTG 与 USB-Serial-JTAG
**共用同一个内部 PHY（GPIO19/20），同时只能一个工作**。应用里一初始化 TinyUSB，
`/dev/ttyACM0` 就消失 —— 日志没了，而且烧写必须手动进下载模式。做成编译开关且默认关。

---

## 11. TTF 字体管线

> **裁剪 / 分区 / 字符集审计**在 `../display/TTF_FONT.md`；本节只讲**固件里的管线**（加载→测量→画）。

### 11.1 为什么不用 `M5GFX::loadFont()`

`loadFont()` 只吃 **VLW / BFF** 两种离线栅格化格式（固定字号 + 固定字符集）。
枚举里的 `ft_ttf` 是障眼法，真 TTF 解析被 `#ifdef LGFX_TTFFONT_HPP_` 门控、该宏未定义。
raw `.ttf` 会被当 VLW 解析直接失败。要真 TTF = 自己接 `stb_truetype`。

### 11.2 三个硬约束

1. **stb_truetype 只认 `glyf` 轮廓**（真 TrueType）。CFF/OTF 直接不认 ——
   思源黑体 SourceHanSans / Noto Sans CJK 全是 CFF → 用不了。
   可用：鸿蒙 HarmonyOS SansSC、文泉驿微米黑、霞鹜文楷。
2. **不支持 TTC**（字体集合）：必须先用 fontTools 抽单字体再另存。
3. **「SD + 不占 flash + 不占 PSRAM」三者不可兼得**。stb_truetype 要整份在连续内存，
   SD 不能 mmap，读进 PSRAM 则 8MB 上限。全量中文 8.5MB+ > 8MB → 现实三选一：
   flash mmap（0 RAM、占 flash）/ 子集化后 SD 读进 PSRAM（≤2MB）/ 换小字体。

**顺序：先验格式和容量，最后才写渲染代码。**

### 11.3 当前实现的两种来源

| 来源 | 函数 | 内存 | 换回代价 |
|---|---|---|---|
| flash `fonts` 分区 | `ttf::load("fonts", size)` | `esp_partition_mmap`，**零 PSRAM** | 无 |
| SD 卡 `.ttf` | `ttf::load_from_sd(path, size)` | 整份读进 **PSRAM**，常驻 | `heap_caps_free` |

`load_from_sd` 会先验「有没有 'A' 的字形」提前抓出 CFF 字体，并打印读/初始化耗时。

### 11.4 stb_truetype 的两个数值坑

- `stbtt_GetFontVMetrics` 返回的是**设计单位**（hhea 单位），必须乘 `g_scale` 换算成像素。
  误把 asc 当像素 → 928px，标签高度爆掉（屏幕才 960 高）。
- `set_size()` 改的是**全局 scale** —— 不只这一页，**所有页的文字尺寸都变**，必须整屏重排。

### 11.5 字形位图渲染

字形位图分配到**内部 RAM**（`heap_caps_malloc(..., MALLOC_CAP_INTERNAL)`，不占 PSRAM）；
8bit 灰度直接 `pushImage`（16 级灰抗锯齿，**别阈值成 1bpp** —— 那正是黑底无抗锯齿的根因）。
stbtt 覆盖值 0=背景(白) / 255=墨(黑)，LGFX grayscale 0=黑 / 255=白 → **取反**。

**墨色**：墨水屏没有颜色，「字体颜色」只能是**墨的灰阶档位**：fg 的亮度映射到 0(满墨)~255(看不见)，
逐像素按覆盖度重映射。两个条件要**同时**满足：① 墨色真的传下去；② 该部件走 16 级波形。

---

## 12. NVS 设置持久化

> 已实现，见 `platform/nvs.h`。写法与键表如下。

| 键 | 类型 | 默认 | 作用 |
|---|---|---|---|
| `size` | u16 | 28 | 默认字号 → `ttf::set_size()`，开机即生效 |
| `font` | str | "" | SD 字体**相对路径**（如 `fonts/霞鹜文楷.ttf`）；空 = 内置 flash |
| `wave` | u8 | 2 | 默认刷新档位 |
| `ink` | u8 | 0x66 | 默认墨色档 |
| `bw` | u8 | 3 | 描边粗细 |
| `idle` | u16 | 5 | 熄屏超时（分钟） |
| `ap` / `ss` / `dp` | u8 | 1/1/0 | 热点 / 屏保 / 深度休眠开关 |

**三条规矩**：

1. **只存相对路径**，不存绝对路径 —— SD 挂载点固定是 `/sdcard`，但万一以后改挂载点，存绝对路径就全失效。
   开机拼成 `/sdcard/<相对路径>`。
2. **只在用户点「保存」或切回内置字体时写**，不要每次改动都写 —— NVS 写会磨损 flash 分区。
3. **NVS 已在 `wifi_ap.h:44` 初始化过**（`nvs_flash_init` + 失败则 `erase` + 重试）。
   直接 `nvs_open("m5ui")` 即可，**别重复 init**。

**中文路径**：FATFS 已开 `LFN_HEAP` + `UTF-8`，中文文件名读得回来。
但 NVS 的 value 是裸字节串，**换字体时按字节比对路径**，别转 codepoint 再比。

---

## 13. 字体切换（NVS 驱动）

设计稿第 6 屏。**字体不自动挑 —— 必须用户在设置里选中、写入 NVS，开机才按 NVS 加载。**

| 项 | 值 |
|---|---|
| 扫描目录 | `/sdcard/fonts`（专目录，不全卡递归） |
| 字体来源 | **只有两条**：内置 flash 分区，或用户在设置里选中并写进 NVS 的那个 |
| 内置字体 | `ttf::load("fonts", size)` → flash mmap，**零 PSRAM**，出厂默认 |
| SD 字体 | `ttf::load_from_sd(path, size)` → 整份读进 PSRAM 常驻（**8MB 上限**） |
| 内存代价 | 换回内置立即 `heap_caps_free`；切 SD 字体要重新读 SD（几十~几百 ms） |
| 重排代价 | 字体切换 / 字号切换 → **整屏重排** TEXT/GL16 780ms（`set_size` 是全局 scale） |

扫目录照 `reader.h:236 scan_books()` 抄 —— 它是本项目「扫目录列文件」的直接先例。

### 13.1 开机加载流程

```
1. nvs_open("m5ui") 读 size / font
2. font 为空          → ttf::load("fonts", size)                    ← 出厂默认
3. font 非空          → 拼 /sdcard/<font> + ttf::load_from_sd()
4. 加载成功            → ttf::set_size(nvs_size)
```

**第 3 步失败必须回退内置，并顺手把 NVS 里的 `font` 清空。**
用户在设置里选了 `fonts/x.ttf`，之后把卡拔了或删了文件 —— 开机时那条路径不存在。
不能就这么放着：否则每次开机都失败、每次都走回退，还白磨一次 NVS。

```cpp
// main.cpp 开机处的形状（**已按此实现**，见 main.cpp 第 2~4 步）
float sz = 28.0f;
if (cfg_load(&c)) {
    sz = c.size;
    if (c.font[0]) {
        char path[320];
        snprintf(path, sizeof(path), "/sdcard/%s", c.font);
        if (!ttf::load_from_sd(path, sz)) {
            ESP_LOGW("MAIN", "NVS 里的字体 '%s' 加载失败 → 回退内置并清 NVS", c.font);
            cfg_clear_font();          // ★ 关键：不然每次开机都失败
            ttf::load("fonts", sz);    // 回退
        }
    } else {
        ttf::load("fonts", sz);
    }
}
```

**并且删掉现有 `main.cpp:61-70` 那段「递归找第一个 `.ttf`」** ——
那是「自动挑字体」，跟这个需求直接冲突。

### 13.2 ★ 固件侧的 bug：加载失败会把屏幕画坏（**已修，见 14 节**）

`text/ttf.h:109-163` 的 `load_from_sd()` **开头没有 `g_ready = false`**（`load()` 第 76 行有）。

```cpp
int off = stbtt_GetFontOffsetForIndex(buf, 0);
if (off < 0 || !stbtt_InitFont(&g_font, buf, off)) { ...free...; return false; }  // 140
...
g_font_buf = buf;          // 151  ← 新字体已顶掉旧的 g_font
g_mapped = nullptr;        // 152
if (!finish_init(pixel_size)) return false;   // 155  ← CFF 字体到这才被发现，已经晚了
```

选一个 CFF/OTF 字体时：`stbtt_InitFont` 会成功，但 `finish_init` 发现没有 'A' 字形返回 false ——
此时 **`g_ready` 仍然是 true**，而 `g_font` 已指向那份没有可用字形的字体。
之后所有 `draw_text` 都用它 → **字全画不出来，屏幕看着像坏了**。

**修法**（三件事一起做，缺一不可）：

1. `load_from_sd()` 开头补 `g_ready = false;`（对齐 `load()` 的写法）。
2. **先验后提交**：`stbtt_InitFont` 用一个**局部** `stbtt_fontinfo` 试，不碰全局 `g_font`；
   全部校验通过（InitFont + `finish_init` 的字形检查）才赋值给全局、并接管 `buf`。
3. 失败时**保留旧字体**：`g_font` / `g_font_buf` / `g_mapped` 一律不动，
   `buf` 释放掉，函数返回 false。调用方据此**不写 NVS**、屏幕不重绘。

设置页的交互也按这个口径：加载失败 → 保留原字体、列表里标个 ✕ 写明原因
（CFF/OTF 不支持 / 超过 PSRAM 8MB），**不写 NVS、不整屏重排**。

## 14. 🗄 历史：固件内置设置页（2026-10-04，**已作废**）

> ⚠️ **这一页的代码已经不存在了。** 设置现在是一个 `.so` 应用（`apps/settings`，六屏 + 子页），
> 固件 `main/pages/` 只剩 `manager.h` / `launcher.h` / `ime.h`。
> 本节的实体数、文件清单都是**重构前的历史快照**，只作记录；现行设置在
> `../design/UI_DESIGN.md` §7.2，实现要点在 `APP_AUTHORING.md`。

**已烧录实机验证**：`panic=0`，实体 207/256，启动约 4.5s。

| # | 文件 | 状态 | 说明 |
|---|---|---|---|
| 0 | `text/ttf.h` | ✅ 已修 | **先验后提交**（见 13.2）；`set_size()` 补上 ascent/descent 重算；`load()` 补 munmap 防漏 |
| 1 | `platform/nvs.h` | ✅ 新建 | `cfg::Config` + `load/save/set_font/clear_font/font_path`，命名空间 `"papers3"` |
| 2 | `ui/theme.h` | ✅ | 加 `G55/G99/GDD`，原有四个没动 |
| 3 | `ui/widgets.h` | ✅ | 加 `setting_row` / `setting_text` |
| 4 | `pages/settings.h` | ✅ 新建 | 五屏：设置主页 / 显示 / 字体 / 电源 / 关于 |
| 5 | `pages/manager.h` | ✅ | 加 `g_settings` + 四个子屏 Page |
| 6 | `pages/demo.h` | ✅ | 加「设置」按钮 + 调试块（黑底白字，显示实体数/字体来源/波形） |
| 7 | `main.cpp` | ✅ | 开机按 NVS 加载；**删掉了「递归找第一个 .ttf」** |

`systems/render.h` **一行没改**（按下反馈原本就对）。

### 14.1 实体分布（实测）

```
main=19  ime=19  connect=11  reader=16  wifi=21
settings=14  set_display=20  set_font=25  set_power=18  set_about=13
total=174 / 256
```

> 键盘重构（2026-10-09）：屏幕键盘 35 个键实体 → **1 个自持容器实体**（`CustomDraw` 自绘 35 键面 + 自轮询触摸、不挂 `Touchable`），ime 页 53→19，全项目 **230→174**（省 56 实体）。上表 main/connect/reader/wifi/settings 仍是重构前快照，重构后以开机日志 `entities:` 为准。

设置五屏合计 90 个实体（原估 87，差在字体页多了一个预览框）。

### 14.2 实现时对设计稿的偏离（都不是偷懒，是约束）

| 设计稿 | 实际做法 | 为什么 |
|---|---|---|
| 单独的 home 导航页（6 个入口方块） | **不加**，在现有 `demo.h` 上加一个「设置」按钮 | `demo.h` 本来就是首页、已经有去 ime/connect/reader/wifi 的按钮。再加一个导航页是两个首页 |
| 行 = 外框 + 标签 + 值 + 箭头（4 实体） | 行 = 外框 + 左对齐文字（**2 实体**），值直接接在标签后面 | 箭头要自绘、右对齐值要每次按字体实测宽度改 `Rect`。设置页几十行，一个实体×行数是决定性的 |
| 设计稿里 6 个字体槽 | **5 个** | 实体预算 |
| 大标题（38px）和正文（28px） | 用 `CustomDraw` 里临时 `set_size` 再**还回去** | **没有「每个实体自己的字号」** —— `ttf::set_size` 是全局 scale。回调里改完必须还原，否则同一渲染循环里后面的实体全被带跑 |
| 选中态用反白 | 一致（`Style.bg` 换黑 + 文字换白） | 设备上强调只有反白/加粗描边/阴影偏移 |

### 14.3 实机自检结果（临时代码，已删）

```
DIAG: scan_fonts 找到 1 个: SmileySans-Oblique.ttf  2629764 bytes
DIAG: 加载 -> 成功, line_height 27 -> 27, source=SD
DIAG: 拿目录当字体 -> 失败, line_height 27 -> 27（不变）, source=SD   ← 先验后提交生效
DIAG: 恢复内置: source=flash
```

第三条是关键：**加载一个不可用的东西，原字体一点没动**。修 bug 前这条会返回成功并把字体换坏。

### 14.4 还没验的：版面

**日志验不了刷屏效果**。需要人眼确认（见第 15 节）：
字号够不够大、选中反白看不看得出来、灰字（`G99`）在 16 级波形下清不清楚、
按下时是不是只动描边和阴影、字体页切换后有没有残影。

## 15. 验收

**日志只能证明"没崩"**：

```bash
# 先把日志存成文件，再验（grep 一个文件计数才有意义）
python3 - <<'PY' > /tmp/papers3.log
import time, serial
s = serial.Serial('/dev/ttyACM0', 115200, timeout=0.2)
s.setDTR(False); s.setRTS(True); time.sleep(0.15); s.setRTS(False)
buf = b''; t0 = time.time()
while time.time() - t0 < 15: buf += s.read(8192)
print(buf.decode('utf-8', 'replace'))
PY

grep -c 'rst:0x' /tmp/papers3.log        # 期望 1（只有自己那次复位）
grep -cE 'panic|Guru Meditation' /tmp/papers3.log   # 期望 0
grep -E 'entities:' /tmp/papers3.log      # 实体数要符合预期
```

- **刷屏类问题日志看不出来**，必须让用户肉眼看屏幕：字够不够大、有没有残影、
  按下去是否只变那一块。改完主动把这几条列出来让他对照。
- **改完别默认烧写**：用户说「烧」再烧。改完先交一份上屏对照清单。
- **纯结构重构的验收判据**（比读代码可靠）：实体是开机一次性建好的，
  所以**运行时日志里的实体计数必须逐字不变**。重构前后都该是同一串。
- **诊断/测试代码不进产物**：验证用的自检、临时日志，验完立刻删干净。
- **未经明确要求不要删代码**：`OnLongPress`、`Battery`、`Progress` 这类没人调用的
  **往往是他计划要做的功能**，不是死代码。只删他点名的那一个。

---

## 16. 给下一个接手的建议

1. **改 UI 只动 `pages/` 和组件**，不要动 `ecs/`、`systems/` —— 内核很稳，动它要连带动所有页面。
2. **加新页**：`pages/` 加一个文件 + `manager.h` 加一个 `Page` + `main.cpp` build + 隐藏。
   页面之间**只通过显式接口调**（比如输入法页调 `demo::set_input()`），别互相伸手拿变量。
3. **页面文件只准有「本页存储 + 接线」**：本页状态变量、实体句柄，加上把组件接起来的那几行。
   组件自己的逻辑一律不进页面 —— 一行候选格 + 翻页箭头这种就该是 `ui/candidate_bar.h` 里的一个 struct。
   判断标准一句话：**这段代码换个页面还用得上吗？用得上就是组件。**
4. **验证习惯**：改完先 `idf.py build`，再烧，再读串口日志。
5. **墨水屏视觉效果只能靠眼睛** —— 远端看不到屏幕时，**先说清"这需要你看一眼"，别猜**。


---

# 附录：ECS 架构说明（原《ARCHITECTURE.md》全文）

> 合并自 docs/ARCHITECTURE.md。只讲**为什么是 ECS**；怎么用看本册前文。

> 本文只讲**为什么是 ECS**。怎么用、墨水屏约束、设置页移植清单见 `DEVELOPER.md`。

## 核心问题

传统 OOP UI 框架（包括之前的 sketch_jun4a）用**继承树**组织代码：

```
Node（基类：位置、脏标记、draw、onTouch）
  ├── Rect
  ├── Label
  ├── Button（位置 + 文字 + 圆角 + 按下状态 + 回调 + …）
  └── ...
```

**问题：** 要给 Label 加"可点击"，需要新写 `ClickableLabel` 子类。要给 Rect 加"可拖拽"，需要新写 `DraggableRect` 子类。行为焊在类名上——每种组合都要一个新类。mirui 的核心创新在于：**不按类组织，按数据片组织**。

---

## ECS = Entity + Component + System

### Entity — 就是一个编号

```cpp
using Entity = uint16_t;   // 只是一个数字，没有"类型"

Entity btn = world.spawn();  // 拿到编号 7
Entity lbl = world.spawn();  // 拿到编号 9
```

Entity 本身**不包含任何数据、任何行为**。它只是 System 查找数据时的钥匙。

### Component — 纯数据 struct，没有方法

```cpp
struct Rect     { int32_t x, y, w, h; };
struct Text     { const char* str; };
struct Style    { uint32_t fg, bg, radius; };
struct OnClick  { void (*fn)(void*); void* ctx; };
struct Dirty    {};   // 标记——只存在或不存在，不用值
```

每种组件只表示**一个方面**：
- `Rect` = 位置 → 所有可见 Entity 都需要
- `Text` = 文字 → Label、Button 需要，Rect 不需要
- `OnClick` = 点击回调 → Button 需要，普通 Label 不需要
- `Dirty` = 需要重绘 → 只当数据变化才挂上

### System — 遍历"有特定组件组合"的 Entity，做一件事

```cpp
// 绘制系统：遍历所有同时有 Rect + Style + Dirty 的 Entity
void render(World& w) {
    for (auto e : w.query<Rect, Style, Dirty>()) {
        auto* r = w.get<Rect>(e);
        auto* s = w.get<Style>(e);
        M5.Display.fillRoundRect(r->x, r->y, r->w, r->h, s->radius, s->bg);
        w.remove<Dirty>(e);  // 画完消标记
    }
}
```

System 是**普通函数**，不是类的成员、不是虚函数。遍历 World，找到匹配的 Entity，干活。

---

## 按钮是怎么"做"出来的

没有 `Button` 类。一个按钮是**同时挂了以下组件的 Entity**：

```cpp
Entity btn = world.spawn();

// 位置
world.add(btn, Rect{20, 100, 200, 50});
// 文字
world.add(btn, Text{"点我"});
// 外观（背景、圆角）
world.add(btn, Style{0xFFFFFF, 0x000000, 25});
// 可触摸
world.add(btn, Touchable{0xFF});
// 点击回调
world.add(btn, OnClick{my_callback, nullptr});

world.add(btn, Dirty{});  // 初始需要画一次
```

这个 Entity 进到 `render` System：
- `query<Rect, Style, Dirty>()` → 匹配（有三个组件）
- `query<Rect, Touchable>()` → 匹配（touch System 来做命中检测）

**"按钮"不是一个类——它是 Rect + Text + Style + Touchable + OnClick 的组合。**

---

## 对比：加一个"可拖拽的 Label"

| | OOP（旧） | ECS（新） |
|---|---|---|
| 怎么做 | 写 `DraggableLabel : public Label` 新子类 | 给同一个 Entity 多挂一个 `Draggable` 组件 |
| 影响范围 | 继承链多了一个类 | 零改动——drag_system 本来就会遍历 `(Rect + Draggable)` |
| 已有的 Label | 不受影响 | 不受影响（没有 Draggable 组件） |

**新功能 = 新组件 + 新 System。不影响现有代码。** 这是 ECS 的核心价值：**组合优于继承**。

---

## 当前项目结构

```
main/
├── main.cpp            ← app_main() 入口：初始化 + 建两页 + 主循环，就这些
├── app.h               ← App 类（主循环：flex → touch → render）
├── ecs/
│   ├── world.h         ← Entity 类型、ComponentType 分配器、ComponentMask 位集
│   ├── world.hpp       ← World 类（spawn、add<T>、get<T>、remove<T>、query<T...>）
│   └── components.h    ← 组件定义（Rect、Text、Style、OnClick、Dirty...）
├── systems/
│   ├── render.h        ← render：遍历 (Rect+Style+Dirty) 画到屏幕
│   ├── touch.h         ← touch：遍历 (Rect+Touchable) 做命中检测+回调
│   └── flex.h          ← flex：遍历 (FlexLayout+Children) 计算子节点坐标
├── ui/                 ← UI 层（组件 + 设计令牌）
│   ├── theme.h         ← 设计令牌（底/墨/描边/阴影的取值）
│   ├── widgets.h       ← 工厂函数（rect/label/button/flex/input_frame/cursor/key...）
│   ├── icons.h         ← 图元图标（字体里没有的箭头 / 退格 / A-a）
│   ├── keyboard.h      ← 屏幕键盘组件（独立成型，输出端可插拔）
│   ├── candidate_bar.h ← 候选栏组件（一行候选格 + 翻页箭头）
│   └── qr.h            ← 二维码绘制（按模块画，version 从 1 往上试）
├── pages/              ← 页面（每页一个文件，只管本页存储 + 把组件接起来）
│   ├── manager.h       ← 页面管理：多页共存，切页只切 Visible（on_enter/on_leave 钩子）
│   ├── demo.h          ← 第 1 页：演示页
│   ├── ime.h           ← 第 2 页：输入法页
│   ├── connect.h       ← 第 3 页：手机连设备（热点二维码 + 网页文件管理）
│   └── reader.h        ← 第 4 页：Markdown 阅读器（书目 + 翻页排版）
├── text/               ← 文本与输入法
│   ├── ttf.h           ← TTF 光栅化（stb_truetype + mmap flash 分区）
│   ├── ime.h           ← 拼音输入法状态机（键盘的一个输出端）
│   ├── pinyin_store.h  ← 索引存储：内置默认 / SD 文件 / 运行时词频
│   ├── pinyin_index.h  ← 内置索引（tools/make_pinyin_index.py 从字体 cmap 生成）
│   └── markdown.h      ← 极简 Markdown 解析（块级 + 行内标记，够读 README）
├── platform/
│   ├── sd.h            ← SD 卡挂载 + 找文件
│   ├── wifi_ap.h       ← 热点（SoftAP）
│   └── web_files.h     ← 网页文件管理（HTTP + SD 增删改查）
└── third_party/
    └── stb_truetype.h
```

---

## 工作流程（每帧）

```
app.update()
  │
  ├─ flex_layout(world)    ← 遍历 (FlexLayout + Children)
  │   对每个 Row/Column 容器，计算子节点 Rect 坐标
  │
  ├─ handle_touch(world)   ← 从 M5.Touch 读状态
  │   遍历 (Rect + Touchable)，命中检测
  │   touch_begin → 挂 Pressed + Dirty
  │   touch_end   → 消 Pressed + 挂 Dirty + 调 OnClick
  │
  └─ render(world)         ← 遍历 (Rect + Style + Dirty)
      按 Rect 坐标、Style 颜色画到屏幕
      画完消 Dirty
```

---

## 怎么自己加功能

**双击：已经内置（`OnDoubleClick`）。**

给实体挂上就走双击判定，窗口 400ms（实现在 `systems/touch.h`）：

```cpp
auto e = button(w, 20, 100, 200, 50, "删", on_single);
w.add(e, OnDoubleClick{on_double, nullptr});   // 就这一行
```

挂了它的实体，第一下**不立刻**触发 `OnClick`，而是欠着；等窗口过期才补发单次 ——
所以双击不会顺带点出一次单击（代价：这个按钮的单击晚 400ms 生效）。

**这就是 ECS 的加法：新组件 + 系统里一段判定，其它按钮一行不动。**

---

**核心公式：**

```
新行为 = 新 Component 类型 + 新/修改 System 查询条件
           ↳ 没有新类            ↳ 没有虚函数
```
