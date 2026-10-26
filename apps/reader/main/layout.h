// apps/reader/main/layout.h — 排版引擎：窗口 · 分页 · 绘制（阅读页正文 + 页脚）
//
// ★ 为什么要「窗口」：三本真书 2.4 / 9.9 / 17.7 MB，PSRAM 只有 8MB。
//   旧实现把整本读进 512KB 缓冲就完事 —— 大书读到 512KB 就"没有下一页"了，
//   目录跳转/进度也都没法做。现在一次只把**当前位置起 512KB** 读进 PSRAM 解析（窗口），
//   页起点用**文件字节偏移**当身份（`PageStart{off, li}`），窗口前后滑动、页表只增不减，
//   页码 / 进度 / 目录跳 / 历史续读都稳。这是 docs/reader/READER_TXT_RULES.md §0「B 流式档」的落地。
//
// ★ 分页与绘制是**同一个函数**（walk_page，draw 开关）：两边各写一份必然对不上
//   （旧代码就是这样，靠注释勉强对齐）。页起点 = walk_page 停下的位置，画出来的一定是同一页。
//
// ★ 排版游标只用「块文件偏移 + 块内行号」：块的 wrap 只跟宽度/字号有关，与窗口无关，
//   所以从任意页起点重新开窗都能还原同一页（跨窗口不漏行、不重行）。
#pragma once
#include <cstdint>
#include <cstring>
#include "ui.h"
#include "cfg.h"
#include "md_parse.h"
#include "txt_parse.h"
#include "chapter.h"
#include "../common/so_api.h"

namespace app::rd {

using ui::R;

inline void slog(const char* m) { so_log(m); so_log_file(m); }

// ══════════════════════════════════════════════════════════════
//  版面（540 × 960）
// ══════════════════════════════════════════════════════════════
inline constexpr int32_t CY = 64;         // 正文上边距（状态栏 60 + 4）
inline constexpr int32_t CH = 846;        // 正文高 → 正文区 64..910（页脚压成一行后由 800 涨到 846）
inline constexpr int32_t CX = 24;
inline constexpr int32_t CW = 492;        // 540 - 2*24

// 页脚：**只有一行**（章名 + 百分比）+ 底部一条细进度条，总高 42px < 状态栏 60px
//   914 ────────── 点线分隔
//   918  > 第二卷 初到大明 ········· 23%     （SZ_VAL 22px）
//   948  ▮▮▮▮▯▯▯▯▯▯▯▯▯▯▯▯▯▯▯▯▯▯▯▯▯▯        （8px，26 格）
inline constexpr int32_t SEP_Y   = 914;
inline constexpr int32_t INFO_Y  = 918;
inline constexpr int32_t BAR_Y   = 948, BAR_H = 8, BAR_CELLS = 26, BAR_CW = 17, BAR_GAP = 2;

// 模式
inline constexpr int MODE_LIST = 0, MODE_READ = 1, MODE_TOC = 2, MODE_CFG = 3, MODE_CHAP = 4;
inline int g_mode = MODE_LIST;

// ══════════════════════════════════════════════════════════════
//  窗口与文档
// ══════════════════════════════════════════════════════════════
inline constexpr uint32_t WIN_BYTES = 256 * 1024;   // 一次读进来的字节（省内存：SD 读的 DMA 缓冲在内部 RAM）
inline constexpr int      WIN_BLKS  = 6000;         // 块表容量（12B/项 = 72KB；256KB 窗够用）

inline char     g_name[256] = {0};      // 当前书文件名
inline uint32_t g_fsize = 0;            // 文件总字节（进度百分比的分母）
inline bool     g_md = false;           // .md：整本进窗口，不分章、不做窗口滑动

inline char*    g_win = nullptr;        // 窗口缓冲（PSRAM）
inline uint32_t g_win_off = 0;          // 窗口起始文件偏移（一定是行首）
inline int      g_win_len = 0;          // 窗口内有效字节（到最后一个完整行）
inline bool     g_win_eof = true;       // 窗口末尾就是文件尾
inline md::Doc  g_doc;
inline bool     g_doc_md_own = false;   // 块表是 md::parse 自己分配的（换窗口要 free）

// ── 行的内存表示：纯文本 + 每字节风格 ──
inline char    g_plain[4096];
inline uint8_t g_st[4096];
inline int32_t g_plain_n = 0;
inline int32_t g_b0[512], g_b1[512];    // 折行结果（字节下标）

// 预取期间允许被手指打断（放弃的半页不入缓存）
inline bool g_draw_may_abort = false;
inline bool g_draw_aborted   = false;
// 重画整页前的通知（PC 端回归测试把收集器清一下；设备上恒为 null，零成本）
inline void (*g_repaint_hook)() = nullptr;

// ★ 必须带上「不许越过字符串结尾」的检查：真书里有**行尾被截断的多字节序列**
//   （xuanhuan 就有 1 处 0xE9 收尾的行）。朴素的 u8_len 会把 0xE9 当 3 字节序列，
//   跳过一个字节的 NUL 继续读 —— 读到的正是窗口缓冲里**下一行的字节/上一窗的残留**，
//   后果是折行算错、页面上画出别处的文字（实测：整段重复 93 字节，查了很久）。
inline int u8_len(const char* p) {
    const uint8_t c = (uint8_t)p[0];
    int l;
    if (c < 0x80) l = 1;
    else if ((c & 0xE0) == 0xC0) l = 2;
    else if ((c & 0xF0) == 0xE0) l = 3;
    else if ((c & 0xF8) == 0xF0) l = 4;
    else l = 1;
    for (int i = 1; i < l; i++) if (p[i] == 0) return 1;   // 截断 → 只吃这一个字节
    return l;
}
inline int u8_count(const char* s) { int n = 0; for (const char* p = s; *p; p += u8_len(p)) n++; return n; }
inline int u8_offset(const char* s, int char_idx) {
    int off = 0;
    for (int i = 0; i < char_idx && s[off]; i++) off += u8_len(&s[off]);
    return off;
}

// 摊平：带标记正文 → 纯文本 + 风格表
inline void flatten(const char* text) {
    // ★ txt 是**纯文本**，不能按 markdown 行内规则解析：正文里真有 `**********`（密码遮罩）、
    //   反引号、方括号 — md 的行内解析会把它们当标记吃掉（实测：整本的 `**` 全被删，文字对不上）。
    if (!g_md) {
        int n = 0;
        for (; text[n] && n < (int)sizeof(g_plain) - 1; n++) {
            g_plain[n] = text[n];
            g_st[n]    = md::IN_NORMAL;
        }
        g_plain[n] = 0;
        g_plain_n = n;
        return;
    }
    md::Span sp[96];
    const int ns = md::inline_spans(text, sp, 96);
    g_plain[0] = 0;
    int n = 0;
    for (int i = 0; i < ns; i++) {
        for (uint16_t k = 0; k < sp[i].len && n < (int)sizeof(g_plain) - 1; k++) {
            g_plain[n] = sp[i].p[k];
            g_st[n]    = sp[i].st;
            n++;
        }
        if (sp[i].st == md::IN_LINK && n < (int)sizeof(g_plain) - 1) {
            g_plain[n] = ' '; g_st[n] = md::IN_NORMAL; n++;
        }
    }
    g_plain[n] = 0;
    g_plain_n = n;
}

// 折行：二分找放得下的最长前缀 → 字节下标写进 g_b0/g_b1
inline int wrap(int32_t width) {
    int n = 0;
    const char* s = g_plain;
    const char* p = s;
    while (*p && n < 512) {
        const int total = u8_count(p);
        if (total <= 0) break;
        int lo = 1, hi = total, best = 0;
        while (lo <= hi) {
            const int mid = (lo + hi) / 2;
            if (so_font_measure_prefix(ui::g_font, p, mid) <= width) { best = mid; lo = mid + 1; }
            else hi = mid - 1;
        }
        if (best <= 0) best = 1;
        int cut = u8_offset(p, best);
        if (best < total) {                       // 行尾退到空格（英文按词折）
            int sp_cut = -1;
            for (int i = cut - 1; i > 0; i--) if (p[i] == ' ') { sp_cut = i; break; }
            if (sp_cut > 0) cut = sp_cut;
        }
        g_b0[n] = (int32_t)(p - s);
        g_b1[n] = (int32_t)(p - s) + cut;
        n++;
        p += cut;
        while (*p == ' ') p++;
    }
    return n;
}

// ── 块样式：相对正文字号的比例（整数算，不用 double —— .so 不链 libgcc）──
struct BStyle { int32_t px; int32_t indent; int32_t before; int32_t after; };
inline BStyle style_of(uint8_t kind) {
    const int b = g_size;
    switch (kind) {
        case md::H1: return {b * 154 / 100, 0, 20, 12};
        case md::H2: return {b * 131 / 100, 0, 16, 10};
        case md::H3: return {b * 115 / 100, 0, 12, 8};
        case md::H4: case md::H5: case md::H6: return {b * 108 / 100, 0, 10, 6};
        case md::CODE: return {b * 92 / 100, 24, 0, 0};
        case md::QUOTE: return {b, 24, 6, 8};
        case md::BULLET: case md::ORDER: return {b, 16, 2, 2};
        default: return {b, 0, 6, 6};
    }
}
// 正文墨色：跟设置里的灰度走（0 浓 / 55 中 / 100 淡）；链接再浅一档
inline uint8_t body_ink()       { return (uint8_t)g_ink; }
inline uint8_t link_ink()       { return (uint8_t)(g_ink < 100 ? g_ink + 100 : 160); }

// 画一段文字（按行内风格切段）
inline void draw_run(int32_t& x, int32_t y, const char* buf, int len, uint8_t st, int32_t px) {
    char tmp[300];
    if (len > 280) len = 280;
    memcpy(tmp, buf, (size_t)len);
    tmp[len] = 0;
    const int32_t w = so_font_measure_prefix(ui::g_font, tmp, u8_count(tmp));
    if (w <= 0) return;
    switch (st) {
        case md::IN_CODE:
            ui::box(x - 2, y + 2, w + 4, px + 2, 0xEEEEEE);
            so_font_draw(ui::g_font, x, y, tmp, body_ink(), 0);
            break;
        case md::IN_BOLD:                          // 伪粗：错开 1px 描两遍
            so_font_draw(ui::g_font, x, y, tmp, body_ink(), 0);
            so_font_draw(ui::g_font, x + 1, y, tmp, body_ink(), 0);
            break;
        case md::IN_LINK:
            so_font_draw(ui::g_font, x, y, tmp, link_ink(), 0);
            break;
        default:
            so_font_draw(ui::g_font, x, y, tmp, body_ink(), 0);
    }
    x += w;
}

// 画一行（walk_page 的行循环调）
inline void draw_line(const md::Block& b, const BStyle& s, int32_t l, int32_t y, int32_t hang) {
    const int32_t x0 = CX + s.indent;
    int32_t x = x0 + (l > 0 ? hang : 0);
    if (b.kind == md::BULLET && l == 0) {
        so_font_set_size(ui::g_font, (float)s.px);
        so_font_draw(ui::g_font, x - 14, y, "·", body_ink(), 0);
    }
    if (b.kind == md::QUOTE) ui::box(CX - 2, y, 3, s.px + 6, 0x999999);
    int i = g_b0[l];
    while (i < g_b1[l]) {
        const uint8_t st = g_st[i];
        int j = i;
        while (j < g_b1[l] && g_st[j] == st) j++;
        draw_run(x, y, g_plain + i, j - i, st, s.px);
        i = j;
    }
}

// ══════════════════════════════════════════════════════════════
//  窗口装载
// ══════════════════════════════════════════════════════════════
inline bool win_has(uint32_t off) {
    return off >= g_win_off && off < g_win_off + (uint32_t)g_win_len;
}
inline uint32_t blk_abs(int i) { return g_win_off + g_doc.blk[i].off; }

// 章节行标记：txt 的章标题在源文件里就是普通一行，解析后按分章规则认出来
// （这样标题才有大字号 + 章前换页 + 页脚显示当前章名；规则与 tools/scan_chapters.py 同一套）
inline void classify_blocks() {
    if (g_md) return;
    for (int i = 0; i < g_doc.n; i++) {
        if (g_doc.blk[i].kind != md::PARA) continue;
        const char* t = nullptr;
        int tl = 0;
        const int lv = chap::classify(g_doc.blk[i].text, (int)strlen(g_doc.blk[i].text),
                                      chap::g_rules, t, tl);
        if (lv <= 0 || !t) continue;
        g_doc.blk[i].text = t;                       // 去掉首尾空白（标题别再吃缩进）
        g_doc.blk[i].kind = (lv == 1 || lv == 4) ? md::H1 : (lv == 2 ? md::H2 : md::H3);
    }
}

inline bool win_alloc() {
    if (!g_win) {
        g_win = (char*)so_psram_alloc((size_t)WIN_BYTES + 1);
        if (!g_win) { slog("窗口：PSRAM 分配失败"); return false; }
    }
    return true;
}

// 块表真正覆盖到的位置（撞 WIN_BLKS 上界时 < to）—— 「窗口读了 512KB」与
// 「块表里有 512KB 的块」是两回事，win_has 只能按后者算（否则会画出半页，实测丢过整段文字）
inline int doc_cov_end(const char* text, int to) {
    if (g_doc.n <= 0) return 0;
    const md::Block& last = g_doc.blk[g_doc.n - 1];
    if (!last.text) return 0;
    int e = (int)(last.text - text) + (int)strlen(last.text) + 1;
    if (e > to) e = to;
    return e;
}

// 开窗到 off（**必须是行首**：页起点 / 章标题偏移 / 0）。窗口内整表重解析。
inline bool win_load_at(uint32_t off) {
    if (!win_alloc()) return false;
    {
        static bool once = false;
        if (!once) {
            once = true;
            char b[128];
            so_snprintf(b, sizeof(b), "内存：内部余 %dKB / PSRAM 余 %dKB（开窗 256KB + 块表 72KB 后）",
                        so_mem_free_kb(0), so_mem_free_kb(1));
            slog(b);
        }
    }
    if (off > g_fsize) off = 0;

    if (g_md) {                                       // md：整本进窗口（小文件）
        const int got = so_read_book(g_name, g_win, (int)WIN_BYTES);
        if (got <= 0) return false;
        g_win[got] = 0;
        if (g_doc_md_own && g_doc.mem) md::free_doc(g_doc);
        const int n = md::parse(g_win, (size_t)got + 1, g_doc, 1500);
        if (n < 0) return false;
        g_doc_md_own = true;
        g_win_off = 0;
        g_win_len = doc_cov_end(g_win, got);
        g_win_eof = true;
        return true;
    }

    if (g_doc_md_own && g_doc.mem) { md::free_doc(g_doc); g_doc_md_own = false; }
    if (!g_doc.blk && !txt::parse_begin(WIN_BLKS, g_doc)) { slog("窗口：块表分配失败"); return false; }

    uint32_t want = WIN_BYTES;
    if (g_fsize - off < want) want = g_fsize - off;
    if (want == 0) return false;
    const int got = so_read_book_at(g_name, g_win, (int)off, (int)want);
    if (got <= 0) return false;

    const bool at_eof = (off + (uint32_t)got >= g_fsize);
    int to = got;
    if (!at_eof) {                                    // 后面还有字节 → 只解析到最后一个完整行
        while (to > 0 && g_win[to - 1] != '\n') to--;
        if (to <= 0) { to = got + 1; g_win[got] = '\n'; }   // 512KB 里没换行：病态文件，保底
    } else if (got < (int)WIN_BYTES && g_win[got - 1] != '\n') {
        // ★ 文件最后一行的结尾常常**没有换行**。parse_more 只认"以 \n 收尾的完整行"，
        //   所以这里补一个 —— 否则整本的**最后一行永远读不到**（实测：尾行少 46 字）。
        g_win[got] = '\n';
        to = got + 1;
    }
    g_doc.n = 0;
    int parsed_end = 0;
    txt::parse_window(g_win, (size_t)to, g_doc, &parsed_end);
    g_win_off = off;
    g_win_len = parsed_end;                           // ★ 块表覆盖范围（不是读进来的字节数）
    g_win_eof = at_eof;
    classify_blocks();
    return g_doc.n > 0;
}

// 二分：找到偏移正好等于 abs_off 的块；不在块首就退到它后面那块、li 归 0
inline int find_blk_exact(uint32_t abs_off, int32_t* li) {
    if (abs_off < g_win_off) return -1;
    const uint32_t t = abs_off - g_win_off;
    int lo = 0, hi = g_doc.n - 1;
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        const uint32_t o = g_doc.blk[mid].off;
        if (o == t) return mid;
        if (o < t) lo = mid + 1; else hi = mid - 1;
    }
    if (lo < g_doc.n) { if (li) *li = 0; return lo; }
    return -1;
}

// ══════════════════════════════════════════════════════════════
//  分页：页表（只增） + 走一页
// ══════════════════════════════════════════════════════════════
// off = 绝对文件偏移（块首），li = 块内第几行开始
struct PageStart { uint32_t off; int32_t li; };
// ★ 页表放 **PSRAM 且按需翻倍**：一本 18MB 的书 ≈ 26000 页，4096 项的老静态数组会**写越界**
//   （实测：大书翻到中途页表溢出 → 页起点乱序、文字对不上）。.so 的静态数据也在很紧的内部 RAM 里，
//   所以这里走 so_psram_alloc。
inline PageStart* g_ps     = nullptr;
inline int32_t    g_ps_cap = 0;
inline int32_t    g_ps_n   = 0;        // 已算出的页数
inline bool       g_ps_end = false;    // 最后一页已确定（后面没有了）
inline int32_t    g_page = 0;          // 当前页（g_ps 下标）

inline bool ps_reserve(int32_t need) {
    if (g_ps && need <= g_ps_cap) return true;
    int32_t cap = g_ps_cap ? g_ps_cap : 2048;
    while (cap < need) {
        cap += cap;
        if (cap > 300000) break;       // 兜底上限（18MB 书约 2.6 万页，够用）
    }
    PageStart* p = (PageStart*)so_psram_alloc((size_t)cap * sizeof(PageStart));
    if (!p) { slog("页表：PSRAM 分配失败"); return false; }
    for (int32_t i = 0; i < g_ps_n; i++) p[i] = g_ps[i];
    if (g_ps) so_psram_free(g_ps);
    g_ps = p; g_ps_cap = cap;
    return true;
}

inline bool ps_reserve(int32_t need);   // 前向声明（pages_reset 要用）
inline void next_set(PageStart* p, uint32_t off, int32_t li) { if (p) { p->off = off; p->li = li; } }

// 从 (off, li0) 起排一页。draw=false 只算「下一页从哪开始」。
// 返回 true = 有下一页（*next 已填）；false = 这页就是最后一页（*end）或窗口用完了（需要重新开窗）
inline bool walk_page(uint32_t off, int32_t li0, bool draw, PageStart* next, bool* end) {
    *end = false;
    if (!g_doc.blk || g_doc.n <= 0) { *end = true; return false; }
    if (!win_has(off)) return false;
    int32_t li = li0;
    const int bi0 = find_blk_exact(off, &li);
    if (bi0 < 0) {
        if (!g_win_eof) return false;                   // 块表没覆盖到页首 → 请调用方重开窗
        *end = true;
        return false;
    }

    int32_t y = CY;
    bool have_next = false;
    const float keep_px = ui::g_font ? so_font_size(ui::g_font) : (float)g_size;

    for (int bi = bi0; bi < g_doc.n; bi++) {
        const md::Block& b = g_doc.blk[bi];
        const uint32_t blk_off = blk_abs(bi);
        // ★ 预取期间每两块摸一次手指：有触摸立刻放弃这一页（真正的翻页绝不开这个开关）
        if (draw && g_draw_may_abort && ((bi - bi0) & 1) == 0 && so_touch_down()) {
            g_draw_aborted = true;
            so_font_set_size(ui::g_font, keep_px);
            return false;
        }
        if (!b.text || !b.text[0]) { li = 0; continue; }
        const BStyle s = style_of(b.kind);
        so_font_set_size(ui::g_font, (float)s.px);
        const int32_t lh = s.px + 8;

        // ★ 章前强制换页（READER_TXT_RULES §5）：H1/H2 与上一行同页会出现"上章尾巴 + 新章标题"，
        //   实测相邻章中位 64 行 ≈ 2–3 页，必须断。页面已空（y == CY）时不触发 —— 否则死循环。
        if (li == 0 && (b.kind == md::H1 || b.kind == md::H2) && y > CY) {
            next_set(next, blk_off, 0); have_next = true; break;
        }

        if (b.kind == md::HR) {
            if (li == 0) {
                if (y + s.before + lh > CY + CH) { next_set(next, blk_off, 0); have_next = true; break; }
                y += s.before;
            }
            if (draw) ui::box(CX, y + s.px / 2, CW, 2, ui::INK);
            y += s.px + s.after;
            li = 0;
            continue;
        }

        flatten(b.text);
        const int32_t hang = (b.kind == md::CODE) ? 16 : 0;
        const int32_t w = CW - s.indent - hang;
        const int32_t nl = wrap(w);
        if (nl <= 0) { li = 0; continue; }
        if (li == 0) {                                  // 块首：before + 第一行都放不下 → 整块推下页
            if (y + s.before + lh > CY + CH) { next_set(next, blk_off, 0); have_next = true; break; }
            y += s.before;
        }
        if (li >= nl) { y += s.after; li = 0; continue; }

        if (draw && b.kind == md::CODE) {               // 代码块灰底整块一次（逐行画会成横条）
            int32_t rows = 0, yt = y;
            while (li + rows < nl && yt + lh <= CY + CH) { yt += lh; rows++; }
            if (rows > 0) ui::box(CX + s.indent, y, CW - s.indent, rows * lh - 2, 0xF6F6F6);
        }
        for (int32_t l = li; l < nl; l++) {
            if (y + lh > CY + CH) { next_set(next, blk_off, l); have_next = true; break; }
            if (draw) draw_line(b, s, l, y, hang);
            y += lh;
        }
        if (have_next) break;
        y += s.after;
        li = 0;
    }

    so_font_set_size(ui::g_font, keep_px);
    if (have_next) return true;
    if (g_win_eof) { *end = true; return false; }
    return false;                                       // 窗口用完 → 调用方重新开窗再来
}

// 保证页表里已有第 k 页（g_ps_n > k）
inline void ensure_page(int32_t k) {
    int guard = 0;
    while (g_ps_n <= k && !g_ps_end && guard++ < 64) {
        const PageStart cur = g_ps[g_ps_n - 1];
        PageStart nx = {0, 0};
        bool end = false;
        if (!win_has(cur.off) && !win_load_at(cur.off)) { g_ps_end = true; break; }
        const bool ok = walk_page(cur.off, cur.li, false, &nx, &end);
        if (end) { g_ps_end = true; break; }
        if (!ok) {                                      // 这一页没排完（窗口到尾了）→ 在页首重开窗
            const uint32_t w0 = g_win_off, l0 = (uint32_t)g_win_len;
            if (!win_load_at(cur.off)) { g_ps_end = true; break; }
            // ★ 防呆：重开窗后覆盖范围一模一样、页首也一样 → 这一页永远排不完（病态超长行）。
            //   再循环就是每次 512KB 读+解析的 64 连击（秒级卡顿）→ 直接当成到底。
            if (g_win_off == w0 && (uint32_t)g_win_len == l0) {
                slog("本页排不完（超长行）→ 当作全书结束");
                g_ps_end = true;
                break;
            }
            continue;
        }
        if (!ps_reserve(g_ps_n + 1)) { g_ps_end = true; break; }
        g_ps[g_ps_n++] = nx;
    }
}

// 清页表：从 off 起重新算页（换书 / 跳章 / 改字号 / 续读都走它）
inline bool g_ps_ready_at = false;      // 页表首项是否已建（page_init 里第一次 reset 之前不能访问）
inline void pages_reset(uint32_t off, int32_t li = 0);
inline void pc_clear();

inline void pages_reset(uint32_t off, int32_t li) {
    if (!ps_reserve(4)) return;
    g_ps[0].off = off; g_ps[0].li = li;
    g_ps_n = 1; g_ps_end = false; g_page = 0;
    pc_clear();
}

inline int pct_of(uint32_t off) {
    if (g_fsize == 0) return 0;
    if (off > g_fsize) off = g_fsize;
    return (int)(off * 100u / g_fsize);
}
inline int page_pct() { return pct_of(g_ps[g_page].off); }

// ══════════════════════════════════════════════════════════════
//  当前章名（页脚显示用）—— 分章索引里最后一个 off ≤ 当前页偏移的条目
// ══════════════════════════════════════════════════════════════
inline char     g_cn_buf[96] = {0};
inline uint32_t g_cn_off = 0xFFFFFFFFu;
inline int      g_cn_lv = 0;

inline bool chapter_name_at(uint32_t off, char* out, int cap, int* lv) {
    if (g_md || chap::count() <= 0) return false;
    if (off != g_cn_off) {
        const chap::Chap* l = chap::list();
        int lo = 0, hi = chap::count() - 1, best = -1;
        while (lo <= hi) {
            const int mid = (lo + hi) / 2;
            if (l[mid].off <= off) { best = mid; lo = mid + 1; } else hi = mid - 1;
        }
        g_cn_off = off;
        g_cn_buf[0] = 0;
        g_cn_lv = 0;
        if (best >= 0 && chap::read_title(best, g_cn_buf, sizeof(g_cn_buf) - 1)) {
            g_cn_lv = chap::title_level(best);
        }
    }
    if (!g_cn_buf[0]) return false;
    ui::scopy(out, g_cn_buf, cap);
    if (lv) *lv = g_cn_lv;
    return true;
}

// ══════════════════════════════════════════════════════════════
//  页脚：细线 + 26 格进度条 + 点线信息行（章名 … 百分比）
// ══════════════════════════════════════════════════════════════
inline char g_fl[128] = {0};
inline const char* footer_left() {
    g_fl[0] = 0;
    if (g_md) { ui::scopy(g_fl, "> 正文", sizeof(g_fl)); return g_fl; }
    char t[96];
    if (chapter_name_at(g_ps[g_page].off, t, sizeof(t), nullptr)) {
        so_snprintf(g_fl, sizeof(g_fl), "> %s", t);
    } else if (chap::running()) {
        so_snprintf(g_fl, sizeof(g_fl), "// 索引生成中 %d%%", chap::pct());
    } else if (chap::count() > 0) {
        ui::scopy(g_fl, "// 章首", sizeof(g_fl));
    } else {
        ui::scopy(g_fl, "// 本书不分章", sizeof(g_fl));
    }
    return g_fl;
}

inline void footer_draw() {
    const int pct = page_pct();
    ui::dashed_h(CX, SEP_Y, CW, ui::GLINE);

    const int filled = (pct * BAR_CELLS + 50) / 100;     // 26 格
    for (int i = 0; i < BAR_CELLS; i++) {
        if (i >= filled) break;
        ui::box(CX + i * (BAR_CW + BAR_GAP), BAR_Y, BAR_CW, BAR_H, ui::INK);
    }

    char pb[8];
    so_snprintf(pb, sizeof(pb), "%d%%", pct);
    const char* left = footer_left();
    so_font_set_size(ui::g_font, ui::SZ_VAL);
    const int32_t ly  = INFO_Y;
    const int32_t rw  = ui::text_w(pb, ui::SZ_VAL, 1);
    const int32_t rx  = ui::SCR_W - ui::PADX - rw;
    const int32_t lim = rx - 40;                       // 左文字最多到这儿（留点线位置）
    int lpx = ui::SZ_VAL;
    while (lpx > 13 && ui::PADX + ui::text_w(left, lpx, 1) > lim) lpx -= 2;
    const int32_t lw = ui::text_w(left, lpx, 1);
    so_font_set_size(ui::g_font, (float)lpx);
    ui::text(ui::PADX, ly, left, ui::INK, 1);
    so_font_set_size(ui::g_font, ui::SZ_VAL);
    ui::text(rx, ly, pb, ui::INK, 1);
    so_font_set_size(ui::g_font, ui::SZ_KEY);
    ui::dot_leader(ui::PADX + lw + 10, rx - 10, ly + 3, ui::SZ_KEY);
}

// 只重画页脚那一条并推它（索引建好 / 章名变了的时候）
inline void footer_refresh() {
    so_set_epd_mode(ui::MF);
    ui::box(0, SEP_Y - 4, ui::SCR_W, ui::SCR_H - SEP_Y + 4, ui::L14);
    so_set_epd_mode(ui::MT);
    footer_draw();
    ui::push({0, SEP_Y - 4, ui::SCR_W, ui::SCR_H - SEP_Y + 4}, "页脚");
}

// ══════════════════════════════════════════════════════════════
//  页缓存（4bpp 整屏快照，逐 nibble 精确）+ 空闲预取
// ══════════════════════════════════════════════════════════════
inline constexpr int32_t PC_W = 540, PC_H = 960;
inline constexpr int     PC_SLOTS = 3;   // 3 页缓存 = 0.78MB PSRAM（原来是 5 = 1.26MB，内部 RAM 告急）
struct PageSlot { int32_t page = -1; uint8_t* buf = nullptr; };
inline PageSlot g_pc[PC_SLOTS];
inline int  g_pc_bytes = 0;
inline bool g_pc_ready = false;

inline void pc_init() {
    if (g_pc_ready) return;
    g_pc_bytes = so_fb_bytes(PC_W, PC_H);
    if (g_pc_bytes <= 0) { slog("页缓存：so_fb_bytes 失败 → 关闭"); return; }
    for (int i = 0; i < PC_SLOTS; i++) {
        g_pc[i].buf  = (uint8_t*)so_psram_alloc((size_t)g_pc_bytes);
        g_pc[i].page = -1;
    }
    g_pc_ready = true;
    char s[128];
    so_snprintf(s, sizeof(s), "页缓存：%d 页 × %d 字节 = %d KB（PSRAM）",
                PC_SLOTS, g_pc_bytes, PC_SLOTS * g_pc_bytes / 1024);
    slog(s);
}
inline void pc_clear() { pc_init(); for (int i = 0; i < PC_SLOTS; i++) g_pc[i].page = -1; }
inline int  pc_find(int32_t page) {
    if (!g_pc_ready) return -1;
    for (int i = 0; i < PC_SLOTS; i++) if (g_pc[i].buf && g_pc[i].page == page) return i;
    return -1;
}
inline int pc_victim(int32_t cur) {
    for (int i = 0; i < PC_SLOTS; i++) if (g_pc[i].page < 0) return i;
    int best = 0, bd = -1;
    for (int i = 0; i < PC_SLOTS; i++) {
        const int d = g_pc[i].page > cur ? g_pc[i].page - cur : cur - g_pc[i].page;
        if (d > bd) { bd = d; best = i; }
    }
    return best;
}
inline bool pc_store(int32_t page) {
    pc_init();
    if (!g_pc_ready) return false;
    int s = pc_find(page);
    if (s < 0) s = pc_victim(g_page);
    if (!g_pc[s].buf) return false;
    if (so_fb_save4(g_pc[s].buf, 0, 0, PC_W, PC_H) != 0) return false;
    g_pc[s].page = page;
    return true;
}
// 命中整屏快照：不重画，直接还原 + 推屏
inline bool pc_show(int32_t page) {
    const int s = pc_find(page);
    if (s < 0) return false;
    const int64_t a = so_now_ms();
    so_set_epd_mode(ui::MT);
    so_fb_restore4(g_pc[s].buf, 0, 0, PC_W, PC_H);
    so_display(0, 0, PC_W, PC_H);
    so_wait_display();
    g_page = page;
    char s2[128];
    so_snprintf(s2, sizeof(s2), "翻页 第%d页: 缓存命中 %lldms", page + 1, (long long)(so_now_ms() - a));
    slog(s2);
    return true;
}

// 预取开关：绘制中允许被手指打断（放弃的半页不入缓存）—— 定义在文件上方（walk_page 要用）

// 画第 k 页正文到帧缓冲（不推屏，不碰状态栏/页脚）
inline void paint_page(int32_t k) {
    if (k < 0) k = 0;
    ensure_page(k);
    if (k >= g_ps_n) k = g_ps_n - 1;
    if (!win_has(g_ps[k].off)) win_load_at(g_ps[k].off);

    PageStart nx = {0, 0};
    bool end = false;
    // ★ 预检（只在本页离窗口覆盖末端很近时做）：这一屏在**当前窗口里排不完**就先重开窗，
    //   免得先画半页再擦掉重画 —— 半页既费一次整屏推屏，也容易在别处留下"半个页面"的残影。
    //   正常一页正文不超过几十 KB，阈值 32KB 足够；真超了还有下面的兜底重画。
    const uint32_t left_cov = (g_win_off + (uint32_t)g_win_len > g_ps[k].off)
                            ? (g_win_off + (uint32_t)g_win_len - g_ps[k].off) : 0;
    if (left_cov < 32 * 1024) {
        if (!walk_page(g_ps[k].off, g_ps[k].li, false, &nx, &end) && !end
            && win_load_at(g_ps[k].off)) {
            walk_page(g_ps[k].off, g_ps[k].li, false, &nx, &end);
        }
    }

    g_draw_aborted = false;
    so_font_set_size(ui::g_font, (float)g_size);
    end = false;
    const bool ok = walk_page(g_ps[k].off, g_ps[k].li, true, &nx, &end);
    if (!ok && !end && !g_draw_aborted && win_load_at(g_ps[k].off)) {
        // 兜底（病态长行才会走到）：擦掉半页重画
        if (g_repaint_hook) g_repaint_hook();
        so_set_epd_mode(ui::MF);
        ui::box(0, CY, ui::SCR_W, CH, ui::L14);
        so_set_epd_mode(ui::MT);
        so_font_set_size(ui::g_font, (float)g_size);
        walk_page(g_ps[k].off, g_ps[k].li, true, &nx, &end);
        slog("本页重画（块表覆盖不够 → 页首重开窗）");
    }
    g_page = k;
}

// ══════════════════════════════════════════════════════════════
//  阅读页：进屏 / 翻页
// ══════════════════════════════════════════════════════════════
// 进度落盘（节流：翻页只更新内存，≥20s 或强制才写 SD）
inline int64_t g_prog_t = 0;
inline int64_t g_prog_saved = -100000;
inline void prog_touch(bool force) {
    if (!g_name[0] || g_ps_n <= 0) return;
    prog_set(g_name, g_ps[g_page].off, g_fsize);
    const int64_t now = so_now_ms();
    if (force || now - g_prog_saved > 20000) {
        prog_flush();
        g_prog_saved = now;
    }
}

// 状态栏时钟：整屏快照恢复的是旧时钟 → 变了就补一块
inline void bar_touch_clock() {
    char old[8];
    ui::scopy(old, ui::g_clock, sizeof(old));
    ui::bar_refresh();
    if (ui::streq(old, ui::g_clock)) return;      // 分钟没跳 → 一次都不推
    so_font_set_size(ui::g_font, ui::SZ_BAR);
    const int32_t lh = so_font_line_height(ui::g_font);
    const int32_t y  = (ui::BAR_H - lh) / 2;

    // ★ 擦/推 = 「旧 ∪ 新」的真实墨迹 ±3px。
    //   原来是固定 SCR_W/2-90、180px 宽的一大块 —— 每分钟都在屏中间闪一块方块。
    const int32_t xo = ui::clock_x(old), xn = ui::clock_x(ui::g_clock);
    int l1 = 0, r1 = 0, l2 = 0, r2 = 0;
    if (!ui::text_span(old, ui::SZ_BAR, 2, &l1, &r1)) { l1 = 0; r1 = 0; }
    if (!ui::text_span(ui::g_clock, ui::SZ_BAR, 2, &l2, &r2)) { l2 = 0; r2 = 0; }
    int32_t x0 = (xo + l1 < xn + l2) ? (xo + l1) : (xn + l2);
    int32_t x1 = (xo + r1 > xn + r2) ? (xo + r1) : (xn + r2);
    x0 -= 3; x1 += 3;
    if (x0 < 0) x0 = 0;
    if (x1 > ui::SCR_W) x1 = ui::SCR_W;
    if (x1 <= x0) return;
    const R r = {x0, y - 2, x1 - x0, lh + 4};
    so_set_epd_mode(ui::MF); ui::box(r, ui::L14);   // 擦 = FAST 写点阵底
    so_set_epd_mode(ui::MT);
    ui::text(xn, y, ui::g_clock, ui::GRAY, 2);
    ui::push(r, "时钟");                             // 一次推屏
}

// 整屏重画一页（缓存没命中时）
inline void render_page(int32_t k) {
    if (k < 0) k = 0;
    ensure_page(k);
    if (k >= g_ps_n) k = g_ps_n - 1;

    if (pc_show(k)) { bar_touch_clock(); prog_touch(false); return; }

    const int64_t a = so_now_ms();
    so_set_epd_mode(ui::MF);                            // 擦 = FAST 写点阵底（与底同档）
    ui::box(0, ui::BAR_H, ui::SCR_W, ui::SCR_H - ui::BAR_H, ui::L14);
    so_set_epd_mode(ui::MT);                            // 写 = TEXT（灰阶/抗锯齿）
    paint_page(k);
    g_page = k;
    footer_draw();
    pc_store(k);                                        // ★ 整屏已画好 → 存快照

    ui::push({0, ui::BAR_H, ui::SCR_W, ui::SCR_H - ui::BAR_H}, "翻页");
    char s[128];
    so_snprintf(s, sizeof(s), "翻页 第%d页: 绘制+推屏 %lldms | 偏移 %u/%u",
                k + 1, (long long)(so_now_ms() - a),
                (unsigned)g_ps[k].off, (unsigned)g_fsize);
    slog(s);
    prog_touch(false);
}

// 进阅读页（整屏，含状态栏）
inline void show_read(int32_t k) {
    g_mode = MODE_READ;
    if (k < 0) k = 0;
    ensure_page(k);
    if (k >= g_ps_n) k = g_ps_n - 1;
    if (pc_show(k)) { bar_touch_clock(); prog_touch(false); return; }

    const int64_t a = so_now_ms();
    so_set_epd_mode(ui::MF);
    ui::box(0, 0, ui::SCR_W, ui::SCR_H, ui::L14);
    so_set_epd_mode(ui::MT);
    ui::bar_refresh();
    ui::bar_draw("< BACK");
    paint_page(k);
    footer_draw();
    pc_store(k);
    so_set_epd_mode(ui::MQ);                            // 切屏整屏 QUALITY（清残影）
    ui::push({0, 0, ui::SCR_W, ui::SCR_H}, "进阅读");
    char s[128];
    so_snprintf(s, sizeof(s), "进阅读 第%d页: 整屏 %lldms | 偏移 %u/%u",
                k + 1, (long long)(so_now_ms() - a), (unsigned)g_ps[k].off, (unsigned)g_fsize);
    slog(s);
    prog_touch(false);
}

// 空闲预取一页（当前页往后最近的未缓存页）
inline int pc_next_target() {
    for (int d = 1; d <= PC_SLOTS; d++) {
        const int32_t p = g_page + d;
        ensure_page(p);
        if (g_ps_n <= p) break;                          // 排不到第 p 页 → 到底了
        if (pc_find(p) < 0) return p;
    }
    return -1;
}
inline bool pc_prefetch_step() {
    pc_init();
    if (!g_pc_ready || g_mode != MODE_READ) return false;
    const int32_t target = pc_next_target();
    if (target < 0) return false;

    const int32_t keep_page = g_page;
    const PageStart keep_ps = g_ps[keep_page];
    so_set_epd_mode(ui::MF);
    ui::box(0, ui::BAR_H, ui::SCR_W, ui::SCR_H - ui::BAR_H, ui::L14);
    so_set_epd_mode(ui::MT);
    g_draw_may_abort = true;
    paint_page(target);
    g_draw_may_abort = false;
    footer_draw();                                       // ★ 快照必须带页脚，否则翻到这一页时页脚是空的
    const bool ok = !g_draw_aborted && pc_store(target);
    g_page = keep_page;
    g_ps[keep_page] = keep_ps;                           // paint_page 只改 g_page，页表不动

    // 把当前页还回帧缓冲（预取刚把它覆盖了），并把窗口放回当前页
    const int cur = pc_find(keep_page);
    if (!win_has(g_ps[keep_page].off)) win_load_at(g_ps[keep_page].off);
    if (cur >= 0) {
        so_set_epd_mode(ui::MT);
        so_fb_restore4(g_pc[cur].buf, 0, 0, PC_W, PC_H);
    }
    if (ok) {
        char s[96];
        so_snprintf(s, sizeof(s), "预取第%d页 → 缓存", target + 1);
        slog(s);
    } else if (g_draw_aborted) {
        slog("预取让位（有触摸）→ 下一 tick 重来");
    }
    return true;
}

// ══════════════════════════════════════════════════════════════
//  阅读页交互：整页三区（左=上页 中=菜单 右=下页）+ 中央弹窗
// ══════════════════════════════════════════════════════════════
inline int zone_of(int32_t x, int32_t y) {
    if (y < 0 || y >= ui::SCR_H) return -1;
    if (x < 180) return 0;
    if (x < 360) return 1;
    return 2;
}

inline constexpr int32_t PUP_W = 360, PUP_ITEM_H = 64, PUP_PAD = 10;
inline int32_t pup_h(int n) { return 12 + n * PUP_ITEM_H; }
inline int32_t pup_x() { return (ui::SCR_W - PUP_W) / 2; }
inline int32_t pup_y(int n) { return (ui::SCR_H - pup_h(n)) / 2; }

inline void draw_popup(const char* const* items, int n) {
    const int32_t px = pup_x(), py = pup_y(n), ph = pup_h(n);
    so_font_set_size(ui::g_font, 26.0f);
    ui::box(px - PUP_PAD, py - PUP_PAD, PUP_W + 2 * PUP_PAD, ph + 2 * PUP_PAD, ui::WHITE);
    ui::box(px, py, PUP_W, ph, ui::INK);
    ui::box(px + 3, py + 3, PUP_W - 6, ph - 6, ui::WHITE);
    const int32_t lh = so_font_line_height(ui::g_font);
    for (int i = 0; i < n; i++) {
        const int32_t iy = py + 6 + i * PUP_ITEM_H;
        if (i) ui::box(px + 8, iy - 3, PUP_W - 16, 2, ui::INK);
        const int32_t tw = ui::text_w(items[i], 26, 0);
        ui::text(px + (PUP_W - tw) / 2, iy + (PUP_ITEM_H - lh) / 2, items[i], ui::INK, 0);
    }
}
inline void show_popup(const char* const* items, int n) {
    const int32_t px = pup_x(), py = pup_y(n), ph = pup_h(n);
    so_set_epd_mode(ui::MF);                                  // 弹窗要带擦除段的档，否则关窗留印
    ui::box(px - PUP_PAD, py - PUP_PAD, PUP_W + 2 * PUP_PAD, ph + 2 * PUP_PAD, ui::L14);
    so_set_epd_mode(ui::MT);
    draw_popup(items, n);
    ui::push({px - PUP_PAD - 2, py - PUP_PAD - 2, PUP_W + 2 * PUP_PAD + 4, ph + 2 * PUP_PAD + 4}, "弹窗");
}
// 关弹窗：从页缓存还原被盖住的那一条，只推它
inline bool hide_popup(int n) {
    const int s = pc_find(g_page);
    if (s < 0 || !g_pc[s].buf) return false;
    const int32_t stride = PC_W / 2;
    int32_t y0 = pup_y(n) - PUP_PAD - 2;
    int32_t h  = pup_h(n) + 2 * PUP_PAD + 4;
    if (y0 < 0) { h += y0; y0 = 0; }
    if (y0 + h > PC_H) h = PC_H - y0;
    so_set_epd_mode(ui::MT);
    so_fb_restore4(g_pc[s].buf + (size_t)y0 * stride, 0, y0, PC_W, h);
    so_display(0, y0, PC_W, h);
    so_wait_display();
    return true;
}
inline int hit_popup(int32_t x, int32_t y, int n) {
    const int32_t px = pup_x(), py = pup_y(n);
    if (x < px || x >= px + PUP_W) return -1;
    for (int i = 0; i < n; i++) {
        const int32_t iy = py + 6 + i * PUP_ITEM_H;
        if (y >= iy - 4 && y < iy + PUP_ITEM_H - 6) return i;
    }
    return -1;
}

} // namespace app::rd
