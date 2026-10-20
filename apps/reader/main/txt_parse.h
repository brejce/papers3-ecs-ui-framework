// apps/reader/main/txt_parse.h — 纯文本解析（照搬固件 text/txt.h）
//
// 每行一个块（kind 恒 PARA），不合并、不认标记、空行跳过。
// 复用 md_parse.h 的 Doc/Block 结构，释放也用 md::free_doc。
// 适配同 md_parse：heap_caps → so_psram_alloc/free。

#pragma once
#include <cstring>
#include <cstdlib>
#include "md_parse.h"

namespace app::txt {

inline int parse(char* text, size_t cap, app::md::Doc& d, int max_blocks = 8000) {
    d.mem = (char*)so_psram_alloc((size_t)max_blocks * sizeof(app::md::Block));
    if (!d.mem) return -1;
    d.blk = (app::md::Block*)d.mem;
    d.cap = max_blocks;
    d.n   = 0;

    char* p   = text;
    char* end = text + cap;

    while (p < end && *p && d.n < max_blocks) {
        char* eol = p;
        while (eol < end && *eol && *eol != '\n') eol++;
        const bool has_nl = (eol < end && *eol == '\n');
        if (has_nl) *eol = 0;
        if (eol > p && eol[-1] == '\r') eol[-1] = 0;

        if (*p) {
            d.blk[d.n].text  = p;
            d.blk[d.n].kind  = app::md::PARA;
            d.blk[d.n].level = 0;
            d.blk[d.n].off   = (uint32_t)(p - text);
            d.n++;
        }
        p = has_nl ? eol + 1 : eol;
    }
    return d.n;
}

// ── 增量续解析（开书"首读一小段 + 后台续读"用）──────────────────────
// ★ 前提：**只对从未解析过的区域调用**，游标只往前走。
//   本解析器就地改文本（把行尾换行写成 NUL），所以重解析已处理过的前缀会在第一个
//   NUL 处被外层循环判 false 停下 —— 不是死循环，但更阴：**静默只解析出第一块**。
//   调用方必须自己记"已解析到哪"（g_parse_pos），只往后接。
// 返回当前总块数。
inline int parse_more(char* text, size_t from, size_t to, app::md::Doc& d) {
    char* p   = text + from;
    char* end = text + to;

    while (p < end && *p && d.n < d.cap) {
        char* eol = p;
        while (eol < end && *eol && *eol != '\n') eol++;
        if (!(eol < end && *eol == '\n')) break;   // 半行 → 不解析，留给下一次
        *eol = 0;
        if (eol > p && eol[-1] == '\r') eol[-1] = 0;
        if (*p) {
            d.blk[d.n].text  = p;
            d.blk[d.n].kind  = app::md::PARA;
            d.blk[d.n].level = 0;
            d.blk[d.n].off   = (uint32_t)(p - text);
            d.n++;
        }
        p = eol + 1;
    }
    return d.n;
}

// ── 窗口模式（layout.h 用）────────────────────────────────────────
// 把窗口 [0, to) 里的完整行全部解析进**已分配好的**块表（d.cap 是上界）。
// ★ 每换一个窗口就整表重解析（d.n 归零）：块下标是窗口内的、会变，身份用 Block::off。
// ★ out_end：块表**真正覆盖到的字节位置**（最后一条完整行的行尾之后）。
//   块表有容量上界（WIN_BLKS），撞上界就只解析了一部分 —— 调用方必须用这个值当覆盖范围，
//   否则「窗口里还有字节」但「块表里没有块」，分页会画出半页（实测踩过：整段文字丢失）。
inline int parse_window(char* text, size_t to, app::md::Doc& d, int* out_end = nullptr) {
    d.n = 0;
    const int n = parse_more(text, 0, to, d);
    if (out_end) {
        if (d.n > 0) {
            const app::md::Block& last = d.blk[d.n - 1];
            const char* e = last.text + strlen(last.text);      // 这一行结尾的 NUL
            *out_end = (int)(e - text) + 1;                     // 跨过 '\n'
            if (*out_end > (int)to) *out_end = (int)to;
        } else {
            *out_end = 0;
        }
    }
    return n;
}

// 分配块表（进增量模式前调一次；之后一直用这块表追加）
inline bool parse_begin(int max_blocks, app::md::Doc& d) {
    d.mem = (char*)so_psram_alloc((size_t)max_blocks * sizeof(app::md::Block));
    if (!d.mem) return false;
    d.blk = (app::md::Block*)d.mem;
    d.cap = max_blocks;
    d.n   = 0;
    return true;
}

}   // namespace app::txt
