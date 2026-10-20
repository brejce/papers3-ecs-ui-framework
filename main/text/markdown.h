#pragma once

// text/markdown.h — 极简 Markdown 解析（就为了在墨水屏上读 README 这种文档）
//
// 块级：`#`~`######` 标题 / 普通段落 / `-`·`*` 无序列表 / `1.` 有序列表 /
//       `>` 引用 / ``` 代码块 / `---` 分隔线
// 行内：**粗体** / `行内代码` / [文字](链接)  —— 解析只负责**去掉标记**并标出范围，
//       怎么画（描两遍=伪粗、加底色=代码、灰色=链接）由 UI 层决定
// 不支持：表格、图片（只显示 alt 文字）、HTML、复杂嵌套。够读 README 就行。
//
// 用法：buf = 把文件读进可写缓冲（PSRAM）；parse() 会**就地**改写它
//       （把换行换成分隔、每块以 NUL 结尾），所以传进来的必须可写、且末尾要有空间。
//
//   md::Doc d;
//   if (md::parse(buf, len, d)) { for (int i = 0; i < d.n; i++) ... d.blk[i].text ... }

#include <cstdint>
#include <cstring>
#include <cstdlib>
#include "esp_heap_caps.h"

namespace m5ui::md {

enum Kind : uint8_t {
    PARA = 0, H1, H2, H3, H4, H5, H6, BULLET, ORDER, QUOTE, CODE, HR
};

struct Block {
    const char* text = nullptr;   // 块级标记去掉后的正文
    uint8_t kind  = PARA;
    uint8_t level = 0;            // 缩进层数（前导空格 / 2，0 起）
};

struct Doc {
    Block* blk = nullptr;
    int    n   = 0;
    int    cap = 0;
    char*  mem = nullptr;         // 块数组的原始内存（PSRAM）
};

// 行内片段：p 指向 text 的子串，len 字节，st 见下
enum Inline : uint8_t { IN_NORMAL = 0, IN_BOLD, IN_CODE, IN_LINK, IN_CODEBLOCK };

struct Span {
    const char* p  = nullptr;
    uint16_t    len = 0;
    uint8_t     st  = IN_NORMAL;
};

// ── 解析 ──
// 返回块数；-1 = 内存不够
inline int parse(char* text, size_t cap, Doc& d, int max_blocks = 1500) {
    // 块数组（PSRAM 优先，退到内部 RAM）
    d.mem = (char*)heap_caps_malloc((size_t)max_blocks * sizeof(Block), MALLOC_CAP_SPIRAM);
    if (!d.mem) d.mem = (char*)heap_caps_malloc((size_t)max_blocks * sizeof(Block), MALLOC_CAP_8BIT);
    if (!d.mem) return -1;
    d.blk = (Block*)d.mem;
    d.cap = max_blocks;
    d.n   = 0;

    char* p = text;
    char* end = text + cap;
    bool in_code = false;

    auto push = [&](char* s, Kind k, int lvl) -> bool {
        if (d.n >= d.cap) return false;
        d.blk[d.n].text  = s;
        d.blk[d.n].kind  = (uint8_t)k;
        d.blk[d.n].level = (uint8_t)lvl;
        d.n++;
        return true;
    };

    while (p < end && *p) {
        // 一行到 '\n'（不含）
        char* eol = p;
        while (eol < end && *eol && *eol != '\n') eol++;
        const bool has_nl = (eol < end && *eol == '\n');
        if (has_nl) *eol = 0;
        if (eol > p && eol[-1] == '\r') eol[-1] = 0;      // CRLF

        char* line = p;
        int   indent = 0;
        while (*line == ' ') { line++; indent++; }
        indent /= 2;

        // ``` 代码块开关
        if (strncmp(line, "```", 3) == 0) {
            in_code = !in_code;
            p = has_nl ? eol + 1 : eol;
            continue;
        }
        if (in_code) {
            if (*line) push(line, CODE, indent);
            p = has_nl ? eol + 1 : eol;
            continue;
        }

        if (!*line) {                                     // 空行：只当分隔
            p = has_nl ? eol + 1 : eol;
            continue;
        }

        // 分隔线：--- / *** / ___（≥3 个）
        {
            char c = line[0];
            if ((c == '-' || c == '*' || c == '_') &&
                line[1] == c && line[2] == c) {
                int k = 0; while (line[k] == c) k++;
                if (line[k] == 0 || line[k] == ' ') {
                    push(line, HR, 0);
                    p = has_nl ? eol + 1 : eol;
                    continue;
                }
            }
        }

        // 标题 #~######
        if (line[0] == '#') {
            int k = 0; while (line[k] == '#' && k < 6) k++;
            if (line[k] == ' ' || line[k] == 0) {
                char* t = line + k;
                while (*t == ' ') t++;
                // 去掉结尾的 #（ATX 闭合写法）
                char* te = t + strlen(t);
                while (te > t && (te[-1] == '#' || te[-1] == ' ')) *--te = 0;
                push(t, (Kind)(H1 + (k - 1)), 0);
                p = has_nl ? eol + 1 : eol;
                continue;
            }
        }

        // 引用 >
        if (line[0] == '>') {
            char* t = line + 1;
            while (*t == ' ') t++;
            push(t, QUOTE, indent);
            p = has_nl ? eol + 1 : eol;
            continue;
        }

        // 无序列表 - * +
        if ((line[0] == '-' || line[0] == '*' || line[0] == '+') && line[1] == ' ') {
            char* t = line + 2;
            while (*t == ' ') t++;
            push(t, BULLET, indent);
            p = has_nl ? eol + 1 : eol;
            continue;
        }

        // 有序列表 1. / 2)  —— **保留序号**，让 UI 直接画（省得再存一份编号）
        {
            char* q = line;
            while (*q >= '0' && *q <= '9') q++;
            if (q > line && (*q == '.' || *q == ')') && q[1] == ' ') {
                push(line, ORDER, indent);
                p = has_nl ? eol + 1 : eol;
                continue;
            }
        }

        // 普通段落：把接下来的连续非空行**合并**（把行尾的 NUL 换成空格，就地合并）
        {
            char* start = line;
            char* tail  = eol;                 // 段落末尾
            char* scan  = has_nl ? eol + 1 : eol;
            while (scan < end && *scan) {
                char* s_eol = scan;
                while (s_eol < end && *s_eol && *s_eol != '\n') s_eol++;
                if (s_eol == scan) break;                       // 空行 → 段落结束
                // 下一个块级标记也结束段落
                if (strncmp(scan, "```", 3) == 0 || scan[0] == '#' || scan[0] == '>' ||
                    ((scan[0] == '-' || scan[0] == '*' || scan[0] == '+') && scan[1] == ' ') ||
                    (scan[0] == '-' && scan[1] == '-' && scan[2] == '-'))
                    break;
                if (s_eol > scan && s_eol[-1] == '\r') s_eol[-1] = 0;
                *tail = ' ';                                    // 行尾 NUL → 空格，接上下一行
                tail  = s_eol;
                const bool nl2 = (s_eol < end && *s_eol == '\n');
                if (nl2) *s_eol = 0;
                scan = nl2 ? s_eol + 1 : s_eol;
            }
            push(start, PARA, 0);
            p = tail;                // tail 处已是 NUL（段落末尾）
            if (p < end && *p == 0) p++;   // 跳过段落结尾那个 NUL（原换行位置）
            while (p < end && *p == '\n') p++;
        }
    }
    return d.n;
}

inline void free_doc(Doc& d) {
    if (d.mem) heap_caps_free(d.mem);
    d.mem = nullptr; d.blk = nullptr; d.n = 0; d.cap = 0;
}

// ── 行内标记 → 片段 ──
// 返回片段数。** 粗体、` 代码、[](链接)：链接的文字按普通画、URL 单独给一段（灰色）。
inline int inline_spans(const char* text, Span* out, int max_n) {
    int n = 0;
    const char* p = text;
    const char* run = nullptr;      // 当前普通段的起点
    auto flush = [&](const char* upto) {
        if (run && upto > run && n < max_n) {
            out[n].p = run; out[n].len = (uint16_t)(upto - run); out[n].st = IN_NORMAL;
            n++;
        }
        run = nullptr;
    };
    while (*p && n < max_n) {
        if (p[0] == '*' && p[1] == '*') {                  // **粗体**
            flush(p);
            const char* e = strstr(p + 2, "**");
            if (!e) { if (!run) run = p; p++; continue; }
            if (n < max_n) { out[n].p = p + 2; out[n].len = (uint16_t)(e - (p + 2)); out[n].st = IN_BOLD; n++; }
            p = e + 2;
            continue;
        }
        if (p[0] == '`') {                                  // `行内代码`
            flush(p);
            const char* e = strchr(p + 1, '`');
            if (!e) { if (!run) run = p; p++; continue; }
            if (n < max_n) { out[n].p = p + 1; out[n].len = (uint16_t)(e - (p + 1)); out[n].st = IN_CODE; n++; }
            p = e + 1;
            continue;
        }
        if (p[0] == '[') {                                  // [文字](链接)
            const char* mid = strchr(p + 1, ']');
            if (mid && mid[1] == '(') {
                const char* e = strchr(mid + 2, ')');
                if (e) {
                    flush(p);
                    if (n < max_n) { out[n].p = p + 1; out[n].len = (uint16_t)(mid - (p + 1)); out[n].st = IN_NORMAL; n++; }
                    if (n < max_n) { out[n].p = mid + 2; out[n].len = (uint16_t)(e - (mid + 2)); out[n].st = IN_LINK; n++; }
                    p = e + 1;
                    continue;
                }
            }
        }
        if (!run) run = p;
        p++;
    }
    flush(p);
    return n;
}

} // namespace m5ui::md
