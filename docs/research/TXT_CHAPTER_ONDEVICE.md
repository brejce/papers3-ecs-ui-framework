# papers3_uiframe/old 阅读器盘点 + 「txt 分章」核查结论

> 核查方式：`old/` 全部 14 个 `.py` 逐文件读完（`file` 确认都是 UTF-8，不是编码问题），
> 再对整个 `~/Project` 做 `章|chapter|目录|TOC|第X章` 全量 grep（排除第三方库）。
> 复现命令见文末。

---

## 0. 先回答「分章分类在哪」

**`old/` 里没有分章分类**，整个 `papers3_uiframe` 也没有（一个「章」字都没有）。

分章这件事在你所有工程里只出现在两处，**都在 papers3 侧**：

| 位置 | 原文 |
|---|---|
| `papers3/apps/reader/main/reader.cpp:4` | 懒分页、位图缓存（so_fb_save/restore）、**分章分卷 —— 后续加**。 |
| `papers3/docs/architecture/READER_API_SURFACE.md:57` | **分章分卷 / 章节识别** → **.so 自己写**（识别章节名、按卷/章分页，**用户 micropython 版已有**） |

文档说"micropython 版已有"，但那份实现**不在** `papers3_uiframe`、`papers3`、`frame`、`sketch_jun4a` 里。
下落排查建议：① 设备 SD 卡（`http://<设备IP>/?path=/` 可直接浏览，
MicroPython 时代留在卡里的 `apps/*.py`）；② 备份目录
（`~/Project/m5ui_backup_20261009_140547`、`m5ui-code-backup-20261009`、`m5ui-doc-backup-20261009`、
`~/Project/old/`）；③ 已丢 —— 那就照 §3 重写，零件都还在。

---

## 1. `old/` 里到底是什么（逐文件）

| 文件 | 行数 | 职责 |
|---|---|---|
| `core.py` | 262 | SD 挂载（`PIN 47/39/38/40`、20MHz、slot 3）、16 级灰度调色板 `_GRAY[0..15]`、`Theme/ThemeManager`（vlw 字体加载 + `textWidth/fontHeight` 度量） |
| **`BookReader.py`** | **190** | **字节偏移分页**（详见 §2），书架之上的"翻页"层 |
| `BooksShelf.py` | 141 | 书架列表：`boomanager.list_books()` → 按钮列表 |
| **`gridview.py`** | **316** | **GridView**：网格参数计算 + 3 页缓存 + canvas 离屏绘制（纯渲染，数据由 BookReader 供） |
| `GridSystem.py` | 157 | 早期网格坐标预计算（已被 gridview 取代，历史遗留） |
| `ListSystem.py` / `ListView.py` | 103 / 326 | 列表控件（滚动条、内联批量绘制） |
| `Framework.py` / `AppBase.py` / `UIElement.py` | 253 / 323 / 218 | 应用框架：页面栈、脏标记、元素树 |
| `Button.py` / `TouchZone.py` / `BatteryIcon.py` / `Color.py` | 114 / 26 / 211 / 55 | 部件层 |

---

## 2. `BookReader.py` 的分页机制（最值得抄的部分）

一页的数据形状：

```python
PageContent(chars_with_coords=[(char, x, y), ...],
            byte_offset_start, byte_offset_end, raw_text_preview)
```

- **`_read_raw_page(max_cells)`**：**逐字节读文件**（`self.book.read(1)`）——
  遇 `\n`/`\r` 换行、列满换列，直到行满或字符数满；返回这一页的起止**字节偏移**。
  坐标来自 `grid.get_all_coordinates()` 预计算表，绘制只是照表落字。
- **定位第 N 页**（`_load_page`）：从**最近的已缓存页**的 `byte_offset_end` 往后顺读；
  一页都没缓存就 `seek(0)` 从头发。**不做全书索引。**
- **缓存策略**：最多 3 页；超了删**离当前页最远**的那页（`_trim_cache`）。
- **预读**：`preload_next(count)`；进度靠 `_save_progress()` 存"页号 + 字节偏移"。
- ⚠️ **`ReadProgressManager` 这个模块不在工程里**（`BookReader.py:2` import 它，
  但全工程找不到该文件）→ 这份代码**直接跑会 ImportError**，要用得自己补。

**它的设计取向一句话**：分页单位 =「**字节偏移 + 固定网格容量**」，
不解析内容结构。好处是**任意大的书都能读**（不需要整本进内存）、翻页定位是 O(偏移差)；
代价是**天然不认章节、不认段落**（一个字符就是一个格子），所以"分章"在这套里根本无从谈起。

---

## 3. 与 papers3 阅读器（`apps/reader`，当前在用的那份）的差异

| | `old/`（MicroPython） | `papers3/apps/reader`（`.so`） |
|---|---|---|
| 书进内存 | **不用**：按需 seek 读窗口 | 整本进 PSRAM，**512KB 上限**（`reader.cpp:21 MAXF`） |
| 分页 | 字节偏移 + 固定网格容量 | 先解析成 **Block 数组**再排版分页（`paginate_until`） |
| 内容结构 | 不解析（逐字） | **解析**：md → H1..H6/列表/引用/代码；txt → 每行一个 PARA |
| 块数上限 | 无 | md **1500** / txt **8000**，超了**静默截断** |
| 进度续读 | 有（页号 + 字节偏移） | 无 |
| 分章 | 无 | 无（TODO） |

---

## 4. 分章怎么落地（补齐 papers3 缺的那块）

### 4.1 索引结构
书整本在 PSRAM、排版单位是**块**，所以索引存块下标最自洽：

```cpp
struct Chap {
    uint32_t blk;        // 章节标题所在的块下标
    uint16_t level;      // 1=卷 2=章 3=节（目录树缩进用）
    uint32_t title_off;  // 标题在 g_buf 里的字节偏移
    uint16_t title_len;
};
```
每章 ≈ 14 字节 × 几百章 ≈ 几 KB（PSRAM）。

### 4.2 判定规则（txt：**纯字节扫描，不用正则**）
`.so` 里没有 regex，也不需要 —— 章节标记的 UTF-8 字节序列固定，直接比字节；
汉字数字同样按字节比，不必解码。

1. 行首修剪空白后匹配：`第 + [0-9０-９一二三四五六七八九十百千零〇两]{1,7} + [章回节卷部篇集]`；
   `Chapter/CHAPTER + 数字`；`卷 + 数字`；以及 `序章/楔子/尾声/后记/番外/前言`。
2. **整行独立**（这行除标题外没别的）—— 防止正文里"他说第三章……"被误判。
3. **长度门槛** ≤ 30 字（≈90 字节）。
4. **同书候选 < 2 就整本不分章**（判定不可靠时宁可不给目录，也别给错的）。
5. 卷 > 章 > 节 归级；首行标题 / 前后空行加权。

### 4.3 接进现有管线（借 H1/H2，零新增样式）
`md::Kind` 已有 `H1..H6`，样式表也是现成的（`layout.h` 与固件页
`main/pages/reader.h:122 style_of(kind)` 同构）→ **卷→H1、章→H2、节→H3**，
字号、段前段后间距、目录缩进全部白拿。要新增的只有两件：

- `MODE_TOC` 目录页：列出 `Chap[]`，点一条 → 跳该章首页；
- **章首页断页**：章节块前强制换页。

### 4.4 成本与分层
一遍线性扫描，O(字节)，512KB 上限 → 毫秒级，纯逻辑跑在 `.so` 里；
**不需要任何新宿主导出** —— 与 `../architecture/READER_API_SURFACE.md` 的分层结论一致
（`.so` 不能自己开文件，书由 `so_read_book` 整本读进 PSRAM）。

---

## 5. 分章之前必须先修的三个坑

1. **块数静默截断**：`md::parse(...,max_blocks=1500)`、`txt::parse(...,max_blocks=8000)`，
   而 `reader.cpp:39/40` 调用时**都没传上限** → 超了就在 `while (d.n < max_blocks)` 直接停，
   **页数算少且无任何提示**。要么抬上限（`Block` 12 字节，8 万块 ≈ 1MB PSRAM），
   要么改成"只索引章 + 懒解析正文"。
2. **整本 512KB 上限**：长篇小说常 1–5MB。要读大书得走 §2 那套**字节偏移窗口**
   （`old/BookReader.py` 的 `seek(byte_offset)` 思路），而不是整本常驻。
3. **`ReadProgressManager` 缺失**：想抄 old/ 的续读能力，这个模块要自己补。

---

## 附：复现用的命令

```bash
# ① old/ 里有没有「章」
grep -rn '章' ~/Project/papers3_uiframe/papers3_uiframe/old

# ② 整个 ~/Project 找分章实现（排除第三方库）
grep -rlE '章节|分章|chapter|CHAP_|is_chapter|第[0-9零一二三四五六七八九十百千]{1,6}[章回节卷]' \
  ~/Project --include=*.py --include=*.c --include=*.cpp --include=*.h --include=*.ino \
  | grep -vE 'M5GFX|M5Unified|node_modules|\.venv|__pycache__|third_party|/build/'

# ③ papers3 阅读器现状
sed -n '1,5p' ~/Project/papers3/apps/reader/main/reader.cpp
grep -n '分章\|章节识别' ~/Project/papers3/docs/architecture/READER_API_SURFACE.md

# ④ 编码核查（怕 GBK 才漏搜）
file ~/Project/papers3_uiframe/papers3_uiframe/old/*.py
```
