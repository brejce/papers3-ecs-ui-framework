// apps/reader/main/md_parse.h — Markdown 解析（照搬固件 text/markdown.h）
//
// 只做三处适配：
//   1. 去掉 `#include "esp_heap_caps.h"`，改 include 公共的 so_api.h
//   2. `heap_caps_malloc(MALLOC_CAP_SPIRAM/8BIT)` → `so_psram_alloc`
//   3. `heap_caps_free` → `so_psram_free`
// 解析逻辑一字未改（块级：标题/段落/列表/引用/代码块/分隔线；行内：粗体/代码/链接）。

#pragma once
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include "../common/so_api.h"
#include "str_util.h"   // my_strncmp/my_strstr（白名单没有 strncmp/strstr）

namespace app::md {

enum Kind : uint8_t {
    PARA = 0, H1, H2, H3, H4, H5, H6, BULLET, ORDER, QUOTE, CODE, HR
};

struct Block {
    const char* text = nullptr;   // 块级标记去掉后的正文
    uint8_t kind  = PARA;
    uint8_t level = 0;            // 缩进层数（前导空格 / 2，0 起）
    // ★ 块文本在**缓冲区**里的字节偏移（窗口模型下 + 窗口起始偏移 = 文件偏移）。
    //   窗口可以滑动（见 layout.h），页起点用它当身份：块下标会随窗口变，字节偏移不会。
    //   加上这个字段后 Block 仍是 12 字节（4/1/1/2 填充/4），没有变大。
    uint32_t off  = 0;
};

struct Doc {
    Block* blk = nullptr;
    int    n   = 0;
    int    cap = 0;
    char*  mem = nullptr;         // 块数组的原始内存（PSRAM）
};

// 行内片段
enum Inline : uint8_t { IN_NORMAL = 0, IN_BOLD, IN_CODE, IN_LINK, IN_CODEBLOCK };

struct Span {
    const char* p  = nullptr;
    uint16_t    len = 0;
    uint8_t     st  = IN_NORMAL;
};

// 返回块数；-1 = 内存不够
inline int parse(char* text, size_t cap, Doc& d, int max_blocks = 1500) {
    d.mem = (char*)so_psram_alloc((size_t)max_blocks * sizeof(Block));
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
        d.blk[d.n].off   = (uint32_t)(s - text);
        d.n++;
        return true;
    };

    while (p < end && *p) {
        char* eol = p;
        while (eol < end && *eol && *eol != '\n') eol++;
        const bool has_nl = (eol < end && *eol == '\n');
        if (has_nl) *eol = 0;
        if (eol > p && eol[-1] == '\r') eol[-1] = 0;      // CRLF

        char* line = p;
        int   indent = 0;
        while (*line == ' ') { line++; indent++; }
        indent /= 2;

        if (my_strncmp(line, "```", 3) == 0) {
            in_code = !in_code;
            p = has_nl ? eol + 1 : eol;
            continue;
        }
        if (in_code) {
            // ★ 代码块保留原始行（含前导空格）：缩进是代码/树状图的内容，
            //   不能当排版边距吃掉 —— 否则 README 那种 tree 图会被拍平。
            if (*p) push(p, CODE, 0);
            p = has_nl ? eol + 1 : eol;
            continue;
        }

        if (!*line) { p = has_nl ? eol + 1 : eol; continue; }

        {
            char c = line[0];
            if ((c == '-' || c == '*' || c == '_') && line[1] == c && line[2] == c) {
                int k = 0; while (line[k] == c) k++;
                if (line[k] == 0 || line[k] == ' ') {
                    push(line, HR, 0);
                    p = has_nl ? eol + 1 : eol;
                    continue;
                }
            }
        }

        if (line[0] == '#') {
            int k = 0; while (line[k] == '#' && k < 6) k++;
            if (line[k] == ' ' || line[k] == 0) {
                char* t = line + k;
                while (*t == ' ') t++;
                char* te = t + strlen(t);
                while (te > t && (te[-1] == '#' || te[-1] == ' ')) *--te = 0;
                push(t, (Kind)(H1 + (k - 1)), 0);
                p = has_nl ? eol + 1 : eol;
                continue;
            }
        }

        if (line[0] == '>') {
            char* t = line + 1;
            while (*t == ' ') t++;
            push(t, QUOTE, indent);
            p = has_nl ? eol + 1 : eol;
            continue;
        }

        if ((line[0] == '-' || line[0] == '*' || line[0] == '+') && line[1] == ' ') {
            char* t = line + 2;
            while (*t == ' ') t++;
            push(t, BULLET, indent);
            p = has_nl ? eol + 1 : eol;
            continue;
        }

        {
            char* q = line;
            while (*q >= '0' && *q <= '9') q++;
            if (q > line && (*q == '.' || *q == ')') && q[1] == ' ') {
                push(line, ORDER, indent);
                p = has_nl ? eol + 1 : eol;
                continue;
            }
        }

        {
            char* start = line;
            char* tail  = eol;
            char* scan  = has_nl ? eol + 1 : eol;
            while (scan < end && *scan) {
                char* s_eol = scan;
                while (s_eol < end && *s_eol && *s_eol != '\n') s_eol++;
                if (s_eol == scan) break;
                if (my_strncmp(scan, "```", 3) == 0 || scan[0] == '#' || scan[0] == '>' ||
                    ((scan[0] == '-' || scan[0] == '*' || scan[0] == '+') && scan[1] == ' ') ||
                    (scan[0] == '-' && scan[1] == '-' && scan[2] == '-'))
                    break;
                if (s_eol > scan && s_eol[-1] == '\r') s_eol[-1] = 0;
                *tail = ' ';
                tail  = s_eol;
                const bool nl2 = (s_eol < end && *s_eol == '\n');
                if (nl2) *s_eol = 0;
                scan = nl2 ? s_eol + 1 : s_eol;
            }
            push(start, PARA, 0);
            p = tail;
            if (p < end && *p == 0) p++;
            while (p < end && *p == '\n') p++;
        }
    }
    return d.n;
}

inline void free_doc(Doc& d) {
    if (d.mem) so_psram_free(d.mem);
    d.mem = nullptr; d.blk = nullptr; d.n = 0; d.cap = 0;
}

// 行内标记 → 片段
inline int inline_spans(const char* text, Span* out, int max_n) {
    int n = 0;
    const char* p = text;
    const char* run = nullptr;
    auto flush = [&](const char* upto) {
        if (run && upto > run && n < max_n) {
            out[n].p = run; out[n].len = (uint16_t)(upto - run); out[n].st = IN_NORMAL;
            n++;
        }
        run = nullptr;
    };
    while (*p && n < max_n) {
        if (p[0] == '*' && p[1] == '*') {
            flush(p);
            const char* e = my_strstr(p + 2, "**");
            if (!e) { if (!run) run = p; p++; continue; }
            if (n < max_n) { out[n].p = p + 2; out[n].len = (uint16_t)(e - (p + 2)); out[n].st = IN_BOLD; n++; }
            p = e + 2;
            continue;
        }
        if (p[0] == '`') {
            flush(p);
            const char* e = strchr(p + 1, '`');
            if (!e) { if (!run) run = p; p++; continue; }
            if (n < max_n) { out[n].p = p + 1; out[n].len = (uint16_t)(e - (p + 1)); out[n].st = IN_CODE; n++; }
            p = e + 1;
            continue;
        }
        if (p[0] == '[') {
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

} // namespace app::md
