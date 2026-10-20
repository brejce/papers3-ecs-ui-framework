# 阅读器依赖 API 面清单

> 编写 2026-10-10。目的：把「阅读器」（`main/pages/reader.h`）到底依赖固件的哪些能力，逐条列出来。
> 这是之前多轮「阅读器要不要做成 .so」讨论的收口——无论阅读器留固件还是进 .so，
> **这五项能力都是它逃不掉的依赖**；差别只在"直接调用"还是"宿主翻译成 C 接口"。

---

## 结论一句话

阅读器分两层，边界按**「决策 vs 能力」**切：

- **业务逻辑（.so 自己做）**：md/txt 解析、分段、**分章分卷、章节识别**、决定每段/每字用什么样式渲染、翻页状态机、字体/字号/间距设置。这些是"阅读器自己的智慧"（用户 micropython 时代就实现过），不依赖外设，纯逻辑。
- **基础能力（固件导出给 .so）**：字体栅格化、排版测量、绘制推屏、文件读写、回 launcher。

**为什么这样切是对的**：`.so` 里跑"解析 → 分章 → 定样式 → 排版（调固件 measure）→ 逐字调固件 draw"，决策在 .so、能力调用在固件，是正常分层，不是绕圈。`.so` 需要固件提供的是**原子能力**（量一个前缀多宽、画一段字、读一个文件），而不是固件替它做排版。

---

## 一、文件读写（platform/sd.h + 标准 libc）

| 能力 | 现状出处 | 阅读器怎么用 |
|---|---|---|
| SD 挂载 | `sd::mount()` | 扫书目、读正文前挂载 |
| 扫目录 | `opendir/readdir/closedir` | `scan_books()` 扫 `/sdcard/books/*.md/.txt` |
| 读文件 | `fopen/fseek/ftell/fread` | `open_book()` 读正文进 PSRAM（上限 512KB）|
| 找字体 | `sd::find_ttf` / `scan_fonts` | 设置页字体列表 |

**关键约束（做 .so 时已踩过的坑）**：elf_loader 的 libc 白名单里**没有** `opendir/readdir/fopen/fread/fseek/ftell/snprintf/strcat/strcpy/strcasecmp`。所以 .so 里不能自己扫目录、读文件、拼路径、格式化——这些全要宿主导出 `so_list_books` / `so_read_book` / `so_list_fonts` / `so_snprintf`。

---

## 二、字体栅格化（text/ttf.h）

| 能力 | 函数 | 阅读器怎么用 |
|---|---|---|
| 加载字体（flash mmap）| `ttf::load` | 兜底字体 |
| 加载字体（SD→PSRAM）| `ttf::load_from_sd` | 用户选的字体 |
| 换字号 | `ttf::set_size` | 标题 H1~H6 字号递减、正文 26px、代码 24px |
| 量文字宽度 | `ttf::measure` / `ttf::measure_prefix` | 折行二分：找放得下的最长前缀 |
| 画文字 | `ttf::draw_text` / `ttf::draw_text_from` | 正文、粗体（描两遍）、链接（灰）|
| 行高 | `ttf::line_height` | 排版行距 |
| 灰度字形合成 | `ttf::rasterize` + `push_glyph` | 16 级灰抗锯齿上屏 |

**字体栈 = stb_truetype**（不是 FreeType，见 ../research/INK_CROSSING_FEASIBILITY.md §二）。`.so` 要换字体得用多实例 `FontCtx` 句柄（已实现），不动固件全局字体。

---

## 三、排版 / 文本处理（.so 自己做，纯逻辑）

| 能力 | 出处 | 阅读器怎么用 |
|---|---|---|
| UTF-8 字符计数 | `ime::u8_count` | 折行时算"几个字" |
| UTF-8 字节偏移 | `ime::u8_offset` | 折行时按字数定位字节 |
| Markdown 块解析 | `md::parse` / `md::inline_spans` | 标题/代码/引用/列表/粗体/链接 |
| 纯文本解析 | `txt::parse` | .txt 每行一块 |
| **分章分卷 / 章节识别** | **.so 自己写** | 识别章节名、按卷/章分页（用户 micropython 版已有）|

**这部分进 .so**：解析逻辑本身是纯 C（`strncmp/strchr/strstr/strlen` 都在 elf_loader 白名单里，能编进 .so）。
但有一个坑：`md::parse`/`txt::parse` 用 `heap_caps_malloc(MALLOC_CAP_SPIRAM)` 分配块数组，**这个符号不在白名单**
（白名单只有 `malloc/calloc/realloc/free`）。所以 .so 里跑解析，要么宿主导出 `so_psram_alloc/free`，
要么 .so 用 `malloc` 退到内部 RAM（512KB，装不下大书）。

**排版测量**（`ttf::measure_prefix` 二分折行）是固件能力，.so 通过 `so_font_measure` 调用——
决策（折行到哪、分页到哪）在 .so，量宽度这个原子动作在固件。这不叫耦合，叫分层。

**实测定位（2026-10-10，见 §七 实测数据）**：耗时大头是这套排版测量（`measure_prefix` 二分折行），
不是 stbtt 纯栅格化——`paginate` 全本 396 页的 8 秒全由逐行二分累出来。

---

## 四、绘制推屏（M5GFX / M5Unified）

| 能力 | 调用 | 阅读器怎么用 |
|---|---|---|
| 设波形档 | `M5.Display.setEpdMode` | 文字 TEXT/GL16（`EPDMode{2}`）|
| 色块 | `M5.Display.fillRect` | 代码块灰底、引用灰杠、分隔线 |
| 推屏 | `M5.Display.display` / `epd_push` | 整页一次性推 |
| 等波形 | `M5.Display.waitDisplay` | 翻页后等跑完 |

**整页 = 1 个 CustomDraw 实体**（`reader.h:399`），`draw_page()` 里自绘整页。这是"海量内容不自持实体"的正确姿势，游戏引擎要照抄这个模式（见 ../research/INK_CROSSING_FEASIBILITY.md §三）。

---

## 五、交互 / 页面管理（ECS + pages）

| 能力 | 出处 | 阅读器怎么用 |
|---|---|---|
| 页面切换 | `pages::push` / `pages::back` / `show` | 从 launcher 进阅读器、返回 |
| 实体/组件 | ECS `World` + `Rect/Text/Style/CustomDraw/EPDMode/Touchable/OnClick/Visible/Dirty` | 书目行（按钮）、正文（CustomDraw）、翻页按钮 |
| 切页重扫 | `on_enter` 钩子 | 每次进阅读器重扫书目（SD 上可能刚加了文件）|

---

## 六、如果做成 .so，需要导出的接口面（已实现状态）

| so_* 接口 | 对应依赖 | 状态 |
|---|---|---|
| `so_list_books` / `so_read_book` | §一 文件 | ✅ 已导出 |
| `so_list_fonts` / `so_font_load(name,px)` | §一+§二 字体 | ✅ 已导出（多实例句柄）|
| `so_font_measure_prefix` | §三 折行二分（性能关键）| ✅ 已导出 |
| `so_psram_alloc` / `so_psram_free` | §三 解析要 PSRAM 块数组 | ✅ 已导出 |
| `so_font_draw/measure/line_height/set_size/has` | §二 栅格化 | ✅ 已导出 |
| `so_snprintf` | 格式化（.so 白名单没有）| ✅ 已导出 |
| `so_fill_rect/set_epd_mode/display/wait` | §四 绘制 | ✅ 已导出 |
| `so_exit` | §五 回 launcher | ✅ 已导出 |

**接口面缺口（要做 .so 时补）**：
- `so_psram_alloc` / `so_psram_free`——md/txt 解析要 PSRAM 块数组，`heap_caps_malloc(MALLOC_CAP_SPIRAM)` 不在白名单。✅ 已补。
- `so_font_measure_prefix`——折行二分需要"量 N 个字符前缀的宽度"。✅ 已补。
- 分章分卷、章节识别 = .so 自己的逻辑，不需要宿主。

---

## 七、性能优化考量

`.so` 与固件之间的每次调用都是一次**跨边界跳转**（ELF 重定位后直接函数调用，比 WASM 便宜，但仍有开销）。
排版是阅读器最热的内层循环，跨边界次数直接决定翻页速度。关键决策：

| 场景 | 低效做法 | 高效做法 | 依据 |
|---|---|---|---|
| 折行：找一行放几个字 | 逐字 `so_font_measure` 累加 → **O(字数)** 次跨边界 | `so_font_measure_prefix` 二分 → **O(log 字数)** 次 | 一行 20 字：逐字 20 次 vs 二分 5 次；整页 15 行 = 300 次 vs 75 次 |
| 画一行文字 | 逐字 `so_font_draw`（每字一次跨边界 + 每字推屏）| 整行一次 `so_font_draw`（内部循环画完）| `so_font_draw` 已是"整段画"语义，别在 .so 侧再拆 |
| 读正文 | 分段 `so_read_book` 多次读 | 一次 `so_read_book` 读进 PSRAM 缓冲 | 文件读一次 I/O，别拆 |
| 块数组分配 | `malloc`（退内部 RAM，512KB 装不下大书）| `so_psram_alloc`（PSRAM，8MB）| 大书块数组可到几百 KB |

**排版测量为什么不进 .so**：`measure_prefix` 内部是 `stbtt_GetCodepointHMetrics` 逐字累加，
它和字体数据（`FontCtx`）绑定。放固件，.so 用一次跨边界换"量 N 字宽"，是最省的切法；
放进 .so 就要连字体数据一起搬进去（回到 .so 不能直接碰 ttf 的问题）。

**一个待定优化**（等 .so 阅读器真做起来再决定）：折行二分每次跨边界仍要 ~5 次，翻 500 页书
会到几千次。若实测慢，可以把 `so_font_measure_prefix` 升级成 `so_font_measure_run`——
一次跨边界量**整行所有前缀宽**（返回一个数组），.so 侧纯内存二分，跨边界降到每行 1 次。
现在不急着做，先让最小版跑起来量真实开销。

### 实测数据（M5PaperS3，书 daming1973.txt 396 页）

| 动作 | 内容 | 实测 |
|---|---|---|
| `open_book` | 读 512KB + parse + paginate 全本 396 页 | 8008 ms |
| `draw_page` 单页 | 栅格化 + 写帧缓冲（不含推屏）| 456 ms（重复测稳定 456 ms）|
| EPD 推屏波形 | 全屏 QUALITY/TEXT | 780 ms（已知常量）|

### 三个结论（按优先级）

- **★★★ 懒分页**：`open_book` 现在一次性 `paginate` 全本 396 页花了 8 秒，打开一本书要干等 8 秒。改成「打开只排第 1 页，用户翻到哪页排到哪页」，打开从 8 秒降到几百 ms。这是最大痛点。
- **★★ 多页缓存**：单页栅格化 456 ms + 推屏 780 ms = 翻页 1236 ms；缓存能砍掉 456 ms，翻页降到 780 ms，快 37%。所以多页缓存值得做。但更优的是「后台预渲染下一页」：翻页后后台 task 立刻栅格化下一页，用户下次翻页时下一页已就绪，效果等同缓存但只用 1 份页缓冲（253KB）而非 N 份。
- **推屏 780 ms 是物理下限**，任何缓存省不掉。

### 内存账（4bit 页缓冲）

4bit 页缓冲（1 字节 2 像素，与 M5GFX `_buf` 同格式）：一页 540×960÷2 = 253KB；3 页 759KB；PSRAM 8MB 余约 4MB，放得下。

### 一个关键认识

这套「排版测量（`measure_prefix` 二分折行）」才是耗时大头，不是 stbtt 纯栅格化——`paginate` 的 8 秒全是逐行二分 `measure_prefix` 累出来的。而「何时分页、怎么分页」正是 .so 阅读器的业务逻辑，固件只提供 `measure_prefix` 这个原子能力。这与本文「决策 vs 能力」的分层结论一致（呼应 §三、§七）。

---

## 八、给游戏项目的迁移价值

这份清单不只是给阅读器的。游戏（《墨森物语》）要用到的能力**和阅读器高度重叠**：
文件（SD 存档/tile 流式加载）、字体（图鉴文字排版）、绘制推屏（场景渲染）、交互（触摸拖拽）。
差别只在游戏多了**物理 + 陀螺仪 + RTC + 蜂鸣器 + 脏矩形渲染管线**（见 ../research/INK_CROSSING_FEASIBILITY.md §五）。

所以这份 API 面清单，本质上是 **m5ui 作为宿主给"上层应用"提供的能力面**——阅读器是第一个
（已经吃透的）案例，游戏是下一个（还没动工的）案例。两者共享同一套核心服务层。

---

## 九、.so 编写硬约束：libc 白名单（踩过坑）

`.so` 里调 libc **不是随便调**——`esp_elf_symbol.c` 的 `g_esp_libc_elfsyms[]` 是**白名单**。
符号找不到 → `E: Can't find symbol xxx` + `DLMOD: Failed to relocate ELF file errno=-88`
→ dlopen 失败、`page_init` 根本不会跑 → **页面直接卡死**（看着像死机，其实设备还活着、Web 服务正常）。

白名单实际内容（`managed_components/espressif__elf_loader/src/esp_elf_symbol.c`）:

| 头 | 可用符号 |
| :--- | :--- |
| string.h | `strerror memset memcpy strlen strtod strrchr strchr strcmp strtol strcspn strncat` |
| stdio.h  | `puts putchar fputc fputs printf vfprintf fprintf fwrite` |
| unistd.h | `usleep sleep exit close` |
| stdlib.h | `malloc calloc realloc free` |
| time.h   | `clock_gettime strftime` |
| pthread  | `pthread_create pthread_attr_init pthread_attr_setstacksize` |

★ **没有的（易踩）**：`strncmp` `strstr` `memcmp` `snprintf` `sprintf` `strcpy` `strcat`
`strcasecmp` `atoi` `heap_caps_malloc` …… 以及**任何文件系统 API**（`fopen/fread/opendir/…`）。

铁律：
1. 缺的字符串/内存函数**自己在 .so 里实现**（纯逻辑、零外部符号）——见 `apps/reader/main/str_util.h`
   （`my_strncmp`/`my_strstr`）。别指望编译器替你生成 memcmp 调用——写完用 `nm -u` 核。
2. 文件/字体/绘制等**系统能力一律走宿主 `so_*` 导出**，`.so` 不碰 IDF 头。
3. **每次编完 `.so` 必做**：`xtensa-esp32s3-elf-nm -u build/xxx.so | awk '{print $NF}' | sort -u`，
   逐个核对白名单；出现名单外符号 = 下次 dlopen 必失败。
