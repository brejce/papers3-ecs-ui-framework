#pragma once

// ──────────────────────────────────────────────
// ime.h — 中文拼音输入法「逻辑层」
//
// 只做：状态机、拼音缓冲、候选生成、UTF-8 编辑。
// 不做：绘制 / 刷新（systems/render.h）、键盘布局（ui/keyboard.h）。
//
// 它只是键盘组件的一个**输出端**：keyboard.h 把「按了什么键 + 修饰键状态」丢过来，
// 这里决定是进输入框还是进拼音缓冲。以后接 USB HID 输出端时，本文件完全不参与。
//
// 候选字符集的两道闸（关键）：
//   ① 离线：main/text/pinyin_index.h 由 tools/make_pinyin_index.py 从**字体 cmap** 生成。
//      换字体 → 重跑脚本 → 索引自动跟着字体变。
//   ② 在线：candidates() 里用 ttf::has_codepoint() 过滤。
//      就算索引没重跑、字体变小了，也只是候选变少 —— **绝不会画出方块**。
// ──────────────────────────────────────────────

#include <cstdint>
#include <cstdio>
#include <cstring>
#include "text/ttf.h"
#include "ui/keyboard.h"
#include "text/pinyin_store.h"   // 拼音索引的存储：内置默认 / SD 文件可升级 / 运行时按用量调序

namespace m5ui::ime {

namespace kb = m5ui::ui::kb;   // 键盘组件：本文件只是它的一个输出端

// ── 模式 ──
// 键盘面板（字母/符号）归 keyboard.h 管；这里只管「输入语义」是英文还是中文拼音。
enum Mode : uint8_t { MODE_EN = 0, MODE_CN = 1, MODE_SYMBOL = 2 /* 保留，未用 */ };

// ── 输入上下文 ──
static constexpr int MAX_CAND = 8;    // 候选栏一页显示几个（= 候选格数量）
static constexpr int LIST_MAX = 32;   // 单次匹配最多收集这么多候选（翻页看剩下的）

struct Context {
    Mode mode      = MODE_EN;
    Mode prev_mode = MODE_EN;

    char text[64]  = {0};   // 已上屏文字（UTF-8）
    int  caret     = 0;     // 光标位置（**字符**下标，不是字节）
    char py[24]    = {0};   // 拼音缓冲（小写 ASCII）
    int  py_len    = 0;

    uint16_t list[LIST_MAX] = {0};   // 候选全集（按字频降序），翻页从这儿取
    int      list_n = 0;
    int      page   = 0;             // 当前页号（0 起）
    uint16_t cand[MAX_CAND] = {0};   // 当前页 = list[page*8 .. +8]
    int      cand_n = 0;
};

// 本次按键影响了什么 —— UI 层据此决定重绘哪块（EPD 上少刷一块是一块）
struct Change {
    bool text  = false;   // 正文变了
    bool cand  = false;   // 候选栏变了
    bool panel = false;   // 键盘面板/键面要换

    // ★ **只有插入点动了**：正文没变、正在拼的拼音也没变（纯左右移光标）。
    //   UI 只挪光标那一小块，不碰文字。
    //   注意「正在拼的拼音变了」不算这一类 —— 拼音是**画在文字那一行里面**的
    //   （见 pages/ime.h 的 g_line），它一变整行显示串就变了 → 走 text。
    bool strip = false;

    // ★ 正文变了时，**从第几个字符开始变**（字符下标，不是字节）。
    //   插入 = 插入处；退格 = 被删字符处。UI 据此只擦/只画从这儿到末尾，
    //   前面的字纹丝不动（render.h 左对齐分支按 TextDirtyRange 算）。
    //   原来没有这个位置信息，页面只能整段重画 —— 这就是输入法页比演示页浪费的根因。
    //   text 为假时这个值无意义。
    int16_t from = 0;
};

// ── UTF-8 工具 ──
inline int u8_len(const char* p) {
    uint8_t c = (uint8_t)*p;
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}
inline int u8_count(const char* s) {
    int n = 0;
    for (const char* p = s; *p; p += u8_len(p)) n++;
    return n;
}
inline int u8_offset(const char* s, int char_idx) {
    int off = 0;
    for (int i = 0; i < char_idx && s[off]; i++) off += u8_len(&s[off]);
    return off;
}
inline int cp_to_utf8(uint32_t cp, char* out) {
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp < 0x800) { out[0] = (char)(0xC0 | (cp >> 6)); out[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}
inline void u8_insert(char* buf, size_t cap, int at_char, uint32_t cp) {
    char enc[4];
    int n = cp_to_utf8(cp, enc);
    int off = u8_offset(buf, at_char);
    size_t len = strlen(buf);
    if (len + (size_t)n + 1 > cap) return;
    memmove(buf + off + n, buf + off, len - off + 1);
    memcpy(buf + off, enc, n);
}
inline void u8_delete_before(char* buf, int at_char) {
    if (at_char <= 0) return;
    int off = u8_offset(buf, at_char - 1);
    int n = u8_len(buf + off);
    memmove(buf + off, buf + off + n, strlen(buf + off + n) + 1);
}

// ── 候选：全集 + 分页 ──
inline void page_sync(Context& c) {
    c.cand_n = 0;
    int start = c.page * MAX_CAND;
    for (int i = 0; i < MAX_CAND; i++) {
        int k = start + i;
        if (k >= c.list_n) break;
        c.cand[c.cand_n++] = c.list[k];
    }
}

// 翻页 dir = -1 上一页 / +1 下一页（越界就停在边界）
inline int page_move(Context& c, int dir) {
    int pages = (c.list_n + MAX_CAND - 1) / MAX_CAND;
    if (pages <= 0) return 0;
    int np = c.page + dir;
    if (np < 0) np = 0;
    if (np >= pages) np = pages - 1;
    if (np == c.page) return 0;
    c.page = np;
    page_sync(c);
    return 1;
}

inline int page_count(const Context& c) {
    return (c.list_n + MAX_CAND - 1) / MAX_CAND;
}

// 收集候选全集：精确匹配优先；否则前缀匹配（轮流取各音节首选，等价于跨音节按字频排）
// 只过滤「字体里没有的码位」，不做别的裁剪 —— 每音节最多 LIST_MAX 个，靠翻页看全
inline void candidates(Context& c) {
    c.list_n = 0;
    c.page   = 0;
    if (c.py_len == 0) { page_sync(c); return; }

    auto push = [&](uint16_t cp) {
        if (c.list_n >= LIST_MAX) return;
        if (!m5ui::ttf::has_codepoint((int32_t)cp)) return;   // ★ 字体没有 → 不要（否则画方块）
        for (int i = 0; i < c.list_n; i++) if (c.list[i] == cp) return;
        c.list[c.list_n++] = cp;
    };

    // 精确匹配：二分（表在 table_load 里已按拼音排序）
    Entry* e = table_find(g_table, c.py);
    if (e) {
        for (int i = 0; i < e->n; i++) push(e->cand[i]);
        page_sync(c);
        return;
    }

    // 前缀匹配：定位第一项，再轮流取
    int start = table_prefix_first(g_table, c.py, c.py_len);
    if (start >= 0) {
        for (int round = 0; round < MAX_CAND && c.list_n < LIST_MAX; round++) {
            for (int i = start; i < g_table.count && c.list_n < LIST_MAX; i++) {
                const Entry& p = g_table.ent[i];
                if (strncmp(p.py, c.py, (size_t)c.py_len) != 0) break;
                if (round < p.n) push(p.cand[round]);
            }
        }
    }
    page_sync(c);
}

// ── 输入框显示串 = 前半文字 + 拼音（正在拼的部分）+ 后半文字 ──
inline void build_display(const Context& c, char* out, size_t cap) {
    int off = u8_offset(c.text, c.caret);
    snprintf(out, cap, "%.*s%s%s", off, c.text, c.py, c.text + off);
}

// 提交第 idx 个候选
//
// ★ 只清**拼音缓冲**，候选列表**原地保留**。三条都有用，别改回去：
//   ① 清拼音（py_len=0）：屏幕上那条拼音要消失、下一个字母开始新音节；
//   ② **不调 candidates()** —— 一调就把 list_n 归零、候选列表清空。
//      保留它才能「连点同一个候选字反复上屏」：想要两个/多个相同的字时不用重新拼写。
//      （用户明确要求：「我需要还能点击，免得我想要两个/多个一样的字符却不能点击」）
//   ③ 返回的 ch.cand 保持 false = **不请求重画候选栏**。中文连打时
//      每选一个字就省掉一次 780ms 的 TEXT 刷新；候选栏只在「又拼出新音节」时才刷。
inline Change commit(Context& c, int idx) {
    Change ch;
    if (idx >= 0 && idx < c.cand_n) {
        uint16_t cp = c.cand[idx];
        // 只有「正在拼」的时候才谈「拼音→字」的调序。空拼音点候选（连点同一个字）
        // 没有这个映射，table_bump 传空串会把词频表搞乱。
        if (c.py_len > 0) table_bump(g_table, c.py, cp);
        ch.from = (int16_t)c.caret;              // 变化起点 = 插入处（插入前的光标位）
        u8_insert(c.text, sizeof(c.text), c.caret, cp);
        c.caret++;
        ch.text = true;
    }
    c.py_len = 0; c.py[0] = 0;   // 只清拼音；候选列表留着（能连点）
    return ch;
}

// ── 按键分发（键盘组件的本地输出端）──
// mods 见 kb::Mod：中文拼音不受大小写影响，英文模式用它定大小写
inline Change press(Context& c, const kb::Key& k, uint8_t mods) {
    Change ch;
    switch (k.kind) {
        case kb::K_CHAR: {
            // 中文模式 + ASCII 字母 → 进拼音缓冲，不动正文
            if (c.mode == MODE_CN && k.ch >= 'a' && k.ch <= 'z') {
                if (c.py_len < (int)sizeof(c.py) - 1) { c.py[c.py_len++] = (char)k.ch; c.py[c.py_len] = 0; }
                candidates(c);
                // ★ ch.text 必须有：拼音画在**文字那一行里面**，它变了整行显示串就变了。
                //   原先只置 ch.cand，页面就只刷候选栏，那一行从来没被标脏 ——
                //   结果是"打字时看不到自己拼的拼音"（原有 bug）。
                ch.cand = true; ch.text = true; ch.from = (int16_t)c.caret;
                break;
            }
            uint32_t cp = k.ch;
            if (c.mode == MODE_EN && (mods & kb::MOD_LSHIFT) && cp >= 'a' && cp <= 'z') cp -= 32;
            ch.from = (int16_t)c.caret;              // 变化起点 = 插入处
            u8_insert(c.text, sizeof(c.text), c.caret, cp);
            c.caret++;
            ch.text = true;
            break;
        }

        case kb::K_BACKSPACE:
            if (c.py_len > 0) {                     // 先删拼音，正文不动
                c.py[--c.py_len] = 0;
                candidates(c);
                ch.cand = true; ch.text = true; ch.from = (int16_t)c.caret;   // 那一行变了（拼音变短）
            } else if (c.caret > 0) {
                ch.from = (int16_t)(c.caret - 1);   // 变化起点 = 被删掉的那个字符
                u8_delete_before(c.text, c.caret);
                c.caret--;
                ch.text = true;
            }
            break;

        case kb::K_SPACE:
            if (c.mode == MODE_CN && c.py_len > 0) {   // 中文有拼音 → 空格 = 上屏首选
                // ★ 必须走同一个 commit()：这里再抄一份逻辑的话，必然漏掉
                //   「保留候选列表 / 不重画候选栏 / 空拼音不调 table_bump」那几条 ——
                //   抄一份的结果就是空格上屏后候选栏又白刷一次 780ms。
                ch = commit(c, 0);
            } else {
                ch.from = (int16_t)c.caret;
                u8_insert(c.text, sizeof(c.text), c.caret, ' ');
                c.caret++;
                ch.text = true;
            }
            break;

        case kb::K_MODE: {
            const bool had_py = (c.py_len > 0);
            c.mode = (c.mode == MODE_CN) ? MODE_EN : MODE_CN;
            c.py_len = 0; c.py[0] = 0;
            candidates(c);
            // 清拼音：只有**原来真在拼**的时候那一行才会变（否则纯白刷一次）。
            // 原来无条件置 ch.text=true，切一次中英就整段重画一遍（780ms 白给）。
            if (had_py) { ch.text = true; ch.from = (int16_t)c.caret; }
            ch.cand = true; ch.panel = true;
            break;
        }

        case kb::K_PREV:
        case kb::K_NEXT:
            if (page_move(c, k.kind == kb::K_NEXT ? 1 : -1)) ch.cand = true;
            break;

        // 光标左右移：只动 c.caret，正文一个字节不碰，所以不置 ch.text。
        // 插入本来就发生在 caret 处（u8_insert 用 c.caret），所以挪完光标再打字
        // 就是「就地插入」—— 这正是「在任意地方插入」需要的全部机制。
        // 正在拼拼音时不动拼音缓冲：拼音是跟着光标走的（build_display 把它插在
        // caret 处），挪完光标它自然跟过去，下次上屏仍插在光标处，不丢字也不误上屏。
        case kb::K_LEFT:
        case kb::K_RIGHT: {
            const int before = c.caret;
            if (k.kind == kb::K_LEFT) { if (c.caret > 0) c.caret--; }
            else                      { if (c.caret < u8_count(c.text)) c.caret++; }
            ch.strip = (c.caret != before);   // 已经在两端就别报变化，省一次刷新
            break;
        }

        case kb::K_ENTER:
            ch.panel = true;   // UI 层处理（把文字带回主页面）
            break;

        default:               // K_SHIFT（键盘自己消化）/ K_SYMBOL（键盘换面板）
            break;
    }
    return ch;
}

} // namespace m5ui::ime
