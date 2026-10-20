#pragma once
// apps/reader/main/chapter.h — txt 分章：规则 + 流式扫描（**第一步：只建索引**）
//
// 本次范围：流式扫全文 → 记下每章的 (文件偏移, 级别, 标题) → 存 SD。
// **不动 UI**（目录页 MODE_TOC、章前断页是第二步）。
//
// 规则与 PC 侧参考实现 `tools/scan_chapters.py` **同一套语义** —— 那份是三本书
// （2.4/9.9/17.7MB）3132 章逐条核对过的回归基准，内置这份默认值必须与
// `tools/reader_rules.txt` 一致。改规则 → 把 RULES_VER +1（旧索引自动失效）。
//
// ★ 为什么必须流式：三本书最小 2.4MB，而 `so_read_book` 整本读上限是 512KB
//   （PSRAM 总共才 8MB），**整本常驻此路不通**。索引本身很小：
//   1834 章 × 12B ≈ 21KB，装得下也存得起。
//
// ★ .so 里没有 regex，也不需要：章节标记的 UTF-8 字节序列是固定的，逐字节比即可；
//   汉字数字同样按字节比，不必解码（`str_util.h` 里有 my_strncmp/my_strstr）。

#include <cstdint>
#include <cstddef>
#include <cstring>
#include "../common/so_api.h"
#include "str_util.h"

namespace app::chap {

// ══════════════════════════════════════════════════════════════
//  一、索引项与规则
// ══════════════════════════════════════════════════════════════

// 索引项（12 字节）
// ★ off 是**文件字节偏移**，不是块下标 —— 书装不进内存，也就没有全量块数组。
//   第二版（目录页）就是拿这个偏移去 so_read_book_at 读那一章附近 64KB 窗口。
struct Chap {
    uint32_t off;        // 标题行的文件字节偏移
    uint32_t title_off;  // 标题文本偏移（行首去空白之后）
    uint16_t title_len;  // 标题文本字节数（不含行尾 \r\n）
    uint8_t  level;      // 1=卷 2=章 3=节 4=独立关键字行（前传/番外…）
    uint8_t  _pad;
};

struct RejPair { const char* a; const char* b; };

struct Rules {
    const char* v_mark = "卷部篇集";
    const char* c_mark = "章回";
    const char* s_mark = "节";
    const char* hanzi  = "零〇一二三四五六七八九十百千两";
    const char* kw[12] = { "前传","序章","序言","楔子","引子","尾声",
                           "终章","后记","番外","外传","完本感言","上架感言" };
    int kw_n      = 12;
    int max_title = 30;    // 整行 ≤30 字（≈90 字节）
    int min_hits  = 2;     // 卷 + 章 < 2 → 判定不可靠，整本不分章
    RejPair rej[2] = { {"回","合"}, {"集","团"} };
    int rej_n      = 2;
};

// 关键字后面允许跟的分隔符（U+3000 全角空格 / U+FF1A ： / U+FF08 （ / U+3001 、 / U+00B7 ·）
static const char SEP_AFTER_KW[] =
    " \t" "\xE3\x80\x80" ":" "\xEF\xBC\x9A" "\xEF\xBC\x88" "(["
    "\xE3\x80\x81" "." "\xC2\xB7" "-0123456789";

static Rules g_rules;      // 内置一份（将来可改成从 reader_rules.txt 读）

// ── UTF-8 小工具（有长度上界，不越读行尾）──
inline int u8l(const char* p) {
    const uint8_t c = (uint8_t)p[0];
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}
// 字节区间 [s, s+len) 里的字符数
inline int u8n(const char* s, int len) {
    int n = 0;
    for (int i = 0; i < len; ) { int l = u8l(s + i); if (i + l > len) l = len - i; i += l; n++; }
    return n;
}
// p 处的 UTF-8 字符是否出现在 set 里（avail = p 之后还有多少字节可用）
inline bool chr_in(const char* set, const char* p, int avail) {
    const int pl = u8l(p);
    if (pl > avail) return false;
    for (const char* q = set; *q; ) {
        const int ql = u8l(q);
        if (ql == pl && app::my_strncmp(q, p, (unsigned)pl) == 0) return true;
        q += ql;
    }
    return false;
}
inline bool eqc(const char* p, int avail, const char* c) {   // p 处是不是字符 c
    const int cl = (int)strlen(c);
    return avail >= cl && app::my_strncmp(p, c, (unsigned)cl) == 0;
}

// 修剪行首尾空白：空格 / 制表 / CR / LF / U+3000 全角空格
inline void trim(const char*& s, int& len) {
    for (;;) {
        if (len <= 0) return;
        if (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') { s++; len--; continue; }
        if (eqc(s, len, "\xE3\x80\x80")) { s += 3; len -= 3; continue; }
        break;
    }
    for (;;) {
        if (len <= 0) return;
        const char* t = s + len - 1;
        if (*t == ' ' || *t == '\t' || *t == '\r' || *t == '\n') { len--; continue; }
        if (len >= 3 && eqc(s + len - 3, len, "\xE3\x80\x80")) { len -= 3; continue; }
        break;
    }
}

// ── 判定：返回级别（0 = 不是章节行）；title/tlen 回传去空白后的标题 ──
inline int classify(const char* raw, int raw_len, const Rules& R,
                    const char*& title, int& tlen)
{
    const char* s = raw;
    int len = raw_len;
    trim(s, len);
    if (len <= 0) return 0;

    // ① 独立关键字行（前传/序章/番外…）—— 后面必须是行尾或分隔符。
    //    ⚠️ 少了这个门，"结局全部一样。"这类正文句子会被误收（实测）。
    for (int i = 0; i < R.kw_n; ++i) {
        const char* k  = R.kw[i];
        const int   kl = (int)strlen(k);
        if (len < kl || app::my_strncmp(s, k, (unsigned)kl) != 0) continue;
        const int rest = len - kl;
        const bool ok_tail = (rest == 0) || chr_in(SEP_AFTER_KW, s + kl, rest);
        if (ok_tail && u8n(s, len) <= u8n(k, kl) + 20) { title = s; tlen = len; return 4; }
    }

    // ② 前缀「第」+ 数字(≤7) + 标记字
    if (!eqc(s, len, "\xE7\xAC\xAC")) return 0;          // 「第」= E7 AC AC
    int pos = 3, n = 0;
    while (pos < len) {
        const char* c  = s + pos;
        const int   cl = u8l(c);
        if (cl > len - pos) break;                        // 行尾截断的坏字节
        bool is_num = (cl == 1 && c[0] >= '0' && c[0] <= '9');
        if (!is_num) is_num = chr_in(R.hanzi, c, len - pos);
        if (!is_num) break;
        pos += cl; n++;
        if (n > 7) return 0;                              // 编号太长 → 不是章节号
    }
    if (n == 0 || pos >= len) return 0;

    const char* mk  = s + pos;
    const int   ml  = u8l(mk);
    if (ml > len - pos) return 0;
    int lv = 0;
    if      (chr_in(R.v_mark, mk, len - pos)) lv = 1;
    else if (chr_in(R.c_mark, mk, len - pos)) lv = 2;
    else if (chr_in(R.s_mark, mk, len - pos)) lv = 3;
    else return 0;

    // ③ 长度门槛（按**字符**数）。正文里「第一回合，自己秘密抢先出击……」（53 字）靠它挡。
    //    ⚠️ 不能拿标点当判据：真章名本来就带 ，？（）—— 实测误杀过 7 条真章节。
    if (u8n(s, len) > R.max_title) return 0;

    // ④ 拒绝对：回+合（第一回合）/ 集+团（第16集团军）
    for (int i = 0; i < R.rej_n; ++i) {
        if (eqc(mk, len - pos, R.rej[i].a) &&
            eqc(mk + ml, len - pos - ml, R.rej[i].b)) return 0;
    }

    title = s; tlen = len;
    return lv;
}

// ══════════════════════════════════════════════════════════════
//  二、流式扫描
// ══════════════════════════════════════════════════════════════
inline void slog(const char* m) { so_log(m); so_log_file(m); }

static const int    CHUNK   = 32 * 1024;   // 每 tick 读这么多（40MHz 下 ≈ 35ms）
static const int    CARRY   =  8 * 1024;   // 跨块半行尾（实测约 152 字节/行，8KB 极宽裕）
static const int    IDX_CAP = 4096;        // 索引容量（最大的书实测 1834 章）
static const size_t HDR     = 16;          // 索引文件头

struct IdxHdr { uint32_t magic, rules_ver, n, fsize; };
static const uint32_t IDX_MAGIC = 0x58444943u;   // "CIDX"
static const uint32_t RULES_VER = 1;             // ★ 改规则就 +1

static char*    g_buf     = nullptr;   // [CARRY + CHUNK]
static char*    g_idxmem  = nullptr;   // [HDR][IDX_CAP × Chap]
static Chap*    g_idx     = nullptr;
static int      g_n       = 0;
static int      g_carry   = 0;
static uint32_t g_fpos    = 0;         // g_buf[0] 对应的文件偏移
static uint32_t g_fsize   = 0;
static bool     g_run     = false;
static int64_t  g_t0      = 0;
static int      g_lv[5]   = {0,0,0,0,0};
static uint32_t g_nextlog = 0;
static char     g_book[256]    = {0};
static char     g_idxname[300] = {0};

inline bool ensure_bufs() {
    if (!g_buf)    g_buf    = (char*)so_psram_alloc((size_t)CARRY + (size_t)CHUNK);
    if (!g_idxmem) g_idxmem = (char*)so_psram_alloc(HDR + (size_t)IDX_CAP * sizeof(Chap));
    if (g_idxmem)  g_idx = (Chap*)(g_idxmem + HDR);
    return g_buf != nullptr && g_idxmem != nullptr;
}

inline void reset() {
    g_run = false; g_carry = 0; g_fpos = 0; g_n = 0;
    g_book[0] = 0; g_idxname[0] = 0;
}

inline void add_line(const char* p, int len, uint32_t off) {
    const char* title = nullptr; int tlen = 0;
    const int lv = classify(p, len, g_rules, title, tlen);
    if (lv <= 0 || g_n >= IDX_CAP) return;
    Chap& c = g_idx[g_n++];
    c.off       = off;
    c.title_off = off + (uint32_t)(title - p);
    c.title_len = (uint16_t)(tlen > 65535 ? 65535 : tlen);
    c.level     = (uint8_t)lv;
    c._pad      = 0;
    if (lv <= 4) g_lv[lv]++;
}

// 存索引到 SD（失败只是"下次还得再扫"，不是错误）
inline bool save() {
    if (!g_idxmem) return false;
    IdxHdr* h = (IdxHdr*)g_idxmem;
    h->magic = IDX_MAGIC; h->rules_ver = RULES_VER;
    h->n = (uint32_t)g_n; h->fsize = g_fsize;
    const int len = (int)(HDR + (size_t)g_n * sizeof(Chap));
    return so_blob_save(g_idxname, g_idxmem, len) == len;
}

// 试读 SD 上的索引；命中（同书 + 同规则版本 + 同文件大小）就免扫
inline bool try_load() {
    if (!g_idxmem) return false;
    const int cap = (int)(HDR + (size_t)IDX_CAP * sizeof(Chap));
    const int got = so_blob_load(g_idxname, g_idxmem, cap);
    if (got < (int)HDR) return false;
    const IdxHdr* h = (const IdxHdr*)g_idxmem;
    if (h->magic != IDX_MAGIC || h->rules_ver != RULES_VER) return false;
    if (h->fsize != g_fsize) return false;                  // 书换了 / 改过 → 重扫
    if (h->n > (uint32_t)IDX_CAP) return false;
    if (got < (int)(HDR + (size_t)h->n * sizeof(Chap))) return false;
    g_n = (int)h->n;
    for (int i = 0; i < 5; i++) g_lv[i] = 0;
    for (int i = 0; i < g_n; i++) if (g_idx[i].level <= 4) g_lv[g_idx[i].level]++;
    return true;
}

inline void finish() {
    if (g_carry > 0) {                       // 最后一行可能没有换行结尾
        add_line(g_buf, g_carry, g_fpos);
        g_fpos += (uint32_t)g_carry;
        g_carry = 0;
    }
    g_run = false;
    const int64_t ms = so_now_ms() - g_t0;
    char b[240];

    // 闸门：判定不可靠时宁可不给目录，也别给错的
    if (g_lv[1] + g_lv[2] < g_rules.min_hits) {
        so_snprintf(b, sizeof(b),
                    "分章：候选不足（卷%d 章%d < %d）→ 整本不分章；仍写空索引以免每次重扫"
                    " | 扫描%lldms",
                    g_lv[1], g_lv[2], g_rules.min_hits, (long long)ms);
        slog(b);
        g_n = 0;
        save();
        return;
    }
    so_snprintf(b, sizeof(b),
                "分章索引：卷%d 章%d 节%d 特殊%d | %d 项 / %d KB | 全书 %lldms（%d KB/s）| 已存 %s",
                g_lv[1], g_lv[2], g_lv[3], g_lv[4], g_n,
                (int)(HDR + (size_t)g_n * sizeof(Chap)) / 1024,
                (long long)ms,
                ms > 0 ? (int)((int)(g_fsize / 1024) * 1000 / (int)ms) : 0,
                g_idxname);
    slog(b);
    save();
}

// 开书时调：先试 SD 缓存，没命中才起后台扫描
inline void start(const char* book, int fsize) {
    reset();
    if (!book || !book[0] || fsize <= 0) return;
    if (!ensure_bufs()) { slog("分章：PSRAM 不够，跳过"); return; }
    so_snprintf(g_book,    sizeof(g_book),    "%s", book);
    so_snprintf(g_idxname, sizeof(g_idxname), "%s.idx", book);
    g_fsize = (uint32_t)fsize;

    if (try_load()) {
        char b[240];
        if (g_n == 0)
            so_snprintf(b, sizeof(b), "分章索引：读 SD 缓存 → 这本判定为**不分章**，免重扫");
        else
            so_snprintf(b, sizeof(b),
                        "分章索引：读 SD 缓存 → %d 项（卷%d 章%d 节%d 特殊%d），免重扫",
                        g_n, g_lv[1], g_lv[2], g_lv[3], g_lv[4]);
        slog(b);
        return;
    }

    g_carry = 0; g_fpos = 0; g_n = 0;
    for (int i = 0; i < 5; i++) g_lv[i] = 0;
    g_nextlog = 2u * 1024u * 1024u;
    g_t0  = so_now_ms();
    g_run = true;

    char b[240];
    // ★ 全整数：这行以前用 %.1f / double，把 libgcc 的 __divdf3/__muldf3/__floatsidf/__udivdi3
    //   拖进了 .so 的未定义符号表 —— 那些不在 elf_loader 白名单里，dlopen 会挂。
    so_snprintf(b, sizeof(b),
                "分章：起后台扫描 %s（%dMB ≈ %d 秒；一个空闲 tick 扫一块，不抢输入）",
                book, fsize / 1048576, fsize / (900 * 1024));   // 40MHz 下实测约 0.9MB/s
    slog(b);
}

// 一个空闲 tick 推一块。返回 true = 还在扫（调用方知道这帧干了活）
inline bool step() {
    if (!g_run) return false;
    if (g_fpos >= g_fsize) { finish(); return false; }

    // 超长行保护：carry 满了就当成整行处理（正常书不会走到这里）
    if (g_carry >= CARRY) {
        add_line(g_buf, g_carry, g_fpos);
        g_fpos += (uint32_t)g_carry;
        g_carry = 0;
    }

    uint32_t want = g_fsize - (g_fpos + (uint32_t)g_carry);
    if (want > (uint32_t)CHUNK) want = CHUNK;
    if (want == 0) { finish(); return false; }

    const int got = so_read_book_at(g_book, g_buf + g_carry,
                                    (int)(g_fpos + (uint32_t)g_carry), (int)want);
    if (got <= 0) { finish(); return false; }

    const int total = g_carry + got;
    int p = 0;
    while (p < total) {
        int e = p;
        while (e < total && g_buf[e] != '\n') e++;
        if (e >= total) break;                       // 半行 → 留给下一次（carry）
        add_line(g_buf + p, e - p, g_fpos + (uint32_t)p);
        p = e + 1;
    }
    const int tail = total - p;
    if (tail > 0) memcpy(g_buf, g_buf + p, (size_t)tail);
    g_carry = tail;
    g_fpos += (uint32_t)p;

    if (g_fpos >= g_nextlog) {
        g_nextlog += 2u * 1024u * 1024u;
        char b[160];
        so_snprintf(b, sizeof(b), "分章扫描：%d%%（%d/%d MB）已收 %d 项",
                    (int)(g_fpos / (g_fsize / 100u + 1u)),
                    (int)(g_fpos / 1048576u), (int)(g_fsize / 1048576u), g_n);
        slog(b);
    }
    return true;
}

// ── 给 UI 看的访问器（分章分析页面用）──
inline bool running()   { return g_run; }
inline bool active()    { return g_run; }                 // 还在扫
inline int  n_chaps()   { return g_n; }
inline int  lv(int i)   { return (i >= 0 && i < 5) ? g_lv[i] : 0; }
inline int  pct()       { return g_fsize ? (int)(g_fpos / (g_fsize / 100u + 1u)) : 0; }
inline int  mb_done()   { return (int)(g_fpos / 1048576u); }
inline int  mb_all()    { return (int)(g_fsize / 1048576u); }
inline int  idx_kb()    { return (int)(HDR + (size_t)g_n * sizeof(Chap)) / 1024; }
inline int64_t elapsed(){ return g_t0 ? (so_now_ms() - g_t0) : 0; }
inline int  ready()     { return g_idxmem != nullptr; }   // 缓冲区已分配

// 第 i 项的级别 / 标题（标题字节在**文件**里，按 title_off 现场读出来）
inline int  title_level(int i) { return (i >= 0 && i < g_n) ? g_idx[i].level : 0; }
inline bool read_title(int i, char* out, int cap) {
    if (i < 0 || i >= g_n || !out || cap < 2) return false;
    const Chap& c = g_idx[i];
    int want = (int)c.title_len;
    if (want > cap - 1) want = cap - 1;
    const int got = so_read_book_at(g_book, out, (int)c.title_off, want);
    if (got <= 0) { out[0] = 0; return false; }
    out[got] = 0;
    return true;
}

// 已建好的索引（第二步做目录页时用）
inline int  count()      { return g_n; }
inline const Chap* list(){ return g_idx; }

}   // namespace app::chap
