#pragma once

// text/txt.h — 纯文本（.txt）独立解析器
//
// 为什么 txt 不能走 markdown 解析器（md::parse）：
//   1. md 会把「连续非空行」合并成一个段落 —— 一个 100KB 的 txt 会被并成一段，
//      而 reader 的 flatten 缓冲只有 4096 字节 → 整本书只剩第一页。
//   2. md 会误判标记：txt 里行首的 `#` 被当标题、`- ` 被当无序列表、`---` 被当分隔线，
//      纯文本的语义全被破坏。
//
// 本解析器：**每行一个块**（kind 恒为 PARA），不合并、不认任何标记、空行跳过。
// 复用 md::Doc / md::Block 结构（text/markdown.h），释放也用 md::free_doc。

#include <cstring>
#include <cstdlib>
#include "esp_heap_caps.h"
#include "text/markdown.h"

namespace m5ui::txt {

// 返回块数；-1 = 内存不够
inline int parse(char* text, size_t cap, md::Doc& d, int max_blocks = 8000) {
    d.mem = (char*)heap_caps_malloc((size_t)max_blocks * sizeof(md::Block), MALLOC_CAP_SPIRAM);
    if (!d.mem) d.mem = (char*)heap_caps_malloc((size_t)max_blocks * sizeof(md::Block), MALLOC_CAP_8BIT);
    if (!d.mem) return -1;
    d.blk = (md::Block*)d.mem;
    d.cap = max_blocks;
    d.n   = 0;

    char* p   = text;
    char* end = text + cap;

    while (p < end && *p && d.n < max_blocks) {
        char* eol = p;
        while (eol < end && *eol && *eol != '\n') eol++;
        const bool has_nl = (eol < end && *eol == '\n');
        if (has_nl) *eol = 0;
        if (eol > p && eol[-1] == '\r') eol[-1] = 0;      // CRLF → LF

        if (*p) {                                          // 非空行 → 一个块（空行跳过）
            d.blk[d.n].text  = p;
            d.blk[d.n].kind  = md::PARA;
            d.blk[d.n].level = 0;
            d.n++;
        }
        p = has_nl ? eol + 1 : eol;
    }
    return d.n;
}

} // namespace m5ui::txt
