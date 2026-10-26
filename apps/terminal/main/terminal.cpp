// apps/terminal/main/terminal.cpp — 设备上的真终端（v2：键盘常驻）
//
// 分工（见 docs/terminal/TERMINAL_SHELL.md）：
//   · 命令执行在固件（papers3::sh），.so 只调 so_sh_exec 拿回一段纯文本；
//   · 键盘用**宿主的输入法组件**，但用 so_ime_set_sticky(1) 让它常驻：
//     回车只把这一行交回 app，**不收起覆盖层**；只有退出 app 才消失。
//
//   v1 的坑（别再踩）：OK/回车在固件里本来就是 hide_overlay()，键盘一收
//   就得重画它占的那条带，怎么补都追不上。正确做法是根本不让它收。
//
// 版式（540×960）：
//    0 ┌ < EXIT        12:31        BAT 87% ┐  60
//   64 │ 文本区（提示符跟着输出往下走）      │ 覆盖层顶边−4
//  636 │ 输入法覆盖层：编辑条 54 + 候选 + 键面 │ 960   ← 常驻，app 不碰
//
// 刷新纪律：进屏整屏 GC13；之后每条命令只推文本区（64~覆盖层顶边−4）这一块 ——
//   每块都"画完再推"，且推两遍：先 FAST（立刻看到），再 TEXT（残影收干净）。
#include "../common/so_api.h"
#include <string.h>
#include "../common/so_meta.h"

namespace {

constexpr int SCR_W = 540, SCR_H = 960, PADX = 24, BAR_H = 60;
constexpr uint32_t L14 = 0xE8E8E8, INK = 0x000000;
// ★ 字号（2026-10-22 用户要求）：正文对齐键盘字形 34px（不再跟全局 size 走、也不给用户调）。
//   34px 的行高 = 33（实测 ascent 26 + descent 7），行距给到 36 留一点呼吸。
//   阅读 / 监控两个 app 有自己的字号设置，不受这里影响。
constexpr int SZ_BAR = 22, SZ_TXT = 22, LH = 26;

constexpr int AREA_Y = 64;
// ★ 文本区下边界**启动时问固件**（so_ime_band_y，API v1.18），不再手抄常量：
//   以前这里写死 616/638，固件改键盘几何（IM_CAND_Y 668→690、编辑条 48→54）时 app 毫无感知，
//   屏幕上就多出一条平灰色带。现在 = 覆盖层顶边 − 4px 余量；问不到老固件就退回 638。
int g_area_bottom = 638;                    // 键盘覆盖层顶边 − 4（page_init 里问固件）
constexpr int TXT_W = SCR_W - 2 * PADX;     // 492

constexpr int OUT_CAP = 3072;
constexpr int SCROLL_CAP = 8192;
constexpr int HIST_MAX = 20, HIST_LEN = 160;

void* g_font = nullptr;
char* g_buf = nullptr;
int   g_len = 0;
char  g_input[600] = {0};
int   g_push_n = 0;
int32_t g_prompt_y = 0;
bool  g_quit = false;       // 已请求退出（exit/quit）：之后不要再画，屏幕交给启动器
bool  g_kb_need = true;     // 本次启动还没拉过键盘（page_init 里置真）

// ★ 大缓冲一律 PSRAM（.so 的 .bss 落在内部 RAM，而内部 RAM 只剩十几 KB ——
//   "内部 RAM 里不许放大静态数组"是 2026-10-20 记下的第一条铁律）。
constexpr int LAY_MAX = 256;            // 折行行表容量
char* g_out = nullptr;                  // so_sh_exec 的输出缓冲（OUT_CAP）
int*  g_st  = nullptr;                  // 折行行起点
int*  g_en  = nullptr;                  // 折行行终点

char  g_hist[HIST_MAX][HIST_LEN];
int   g_nhist = 0;

// ── 滚动缓冲 ────────────────────────────────────────────────────
void scroll_drop(int need) {
    if (!g_buf) return;
    if (g_len + need <= SCROLL_CAP) return;
    int drop = 0;
    while (g_len + need > SCROLL_CAP && drop < g_len) {
        while (drop < g_len && g_buf[drop] != '\n') drop++;
        if (drop < g_len) drop++;
    }
    if (drop <= 0) { g_len = 0; g_buf[0] = 0; return; }
    for (int i = 0; i < g_len - drop; i++) g_buf[i] = g_buf[i + drop];   // memmove 不在白名单里
    g_len -= drop;
    g_buf[g_len] = 0;
}

void append(const char* s, int n) {
    if (!g_buf || !s || n <= 0) return;
    if (n > SCROLL_CAP) { s += n - SCROLL_CAP; n = SCROLL_CAP; }
    scroll_drop(n);
    memcpy(g_buf + g_len, s, (size_t)n);
    g_len += n;
    g_buf[g_len] = 0;
}
void append_s(const char* s) { if (s) append(s, (int)strlen(s)); }
void append_line(const char* s) { append_s(s); append("\n", 1); }

// ── 折行：按 492px 宽度切 ──────────────────────────────────────
int u8_len(unsigned char c) {
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

int fit_bytes(const char* s, int maxw) {
    const int n = (int)strlen(s);
    int i = 0, chars = 0;
    while (i < n) {
        const int step = u8_len((unsigned char)s[i]);
        chars++;
        const int w = (int)so_font_measure_prefix(g_font, s, chars);
        if (w > maxw) { chars--; break; }
        i += step;
    }
    if (chars <= 0) return u8_len((unsigned char)s[0]);
    return i;
}

int layout_lines(int* st, int* en, int cap) {
    int n = 0;
    if (!g_buf) return 0;
    int i = 0;
    while (i < g_len && n < cap) {
        int j = i;
        while (j < g_len && g_buf[j] != '\n') j++;
        if (j == i) { st[n] = i; en[n] = i; n++; i = j + 1; continue; }
        int p = i;
        while (p < j && n < cap) {
            char tmp[512];
            int tl = j - p;
            if (tl > (int)sizeof(tmp) - 1) tl = (int)sizeof(tmp) - 1;
            memcpy(tmp, g_buf + p, (size_t)tl);
            tmp[tl] = 0;
            int take = fit_bytes(tmp, TXT_W);
            if (take <= 0) take = u8_len((unsigned char)g_buf[p]);
            if (p + take > j) take = j - p;
            st[n] = p;
            en[n] = p + take;
            n++;
            p += take;
        }
        i = j + 1;
    }
    return n;
}

// ── 文本区：输出行 + 提示符行（提示符跟着输出往下走）───────────────
// 只画最后一屏（total - lines 之后的行）—— 没有滚动：更早的输出看不到（v2 用手势滚动的
// 那条路已经撤了，触摸全归固件键盘；要看更早的内容用 `clear` 重开一屏）。
void draw_text(int32_t bottom, int32_t from_y = 0) {
    if (!g_st || !g_en) return;
    const int n = layout_lines(g_st, g_en, LAY_MAX);
    const int lines = (bottom - AREA_Y) / LH;
    const int total = n + 1;                                   // +1 = 提示符那一行

    const int first = (total > lines) ? total - lines : 0;
    int y = AREA_Y;
    for (int i = first; i < n && y + LH <= bottom; i++, y += LH) {
        if (y + LH <= from_y) continue;               // 只画改动区，上面的不动
        char tmp[512];
        int len = g_en[i] - g_st[i];
        if (len > (int)sizeof(tmp) - 1) len = (int)sizeof(tmp) - 1;
        if (len > 0) {
            memcpy(tmp, g_buf + g_st[i], (size_t)len);
            tmp[len] = 0;
            so_font_draw(g_font, PADX, y, tmp, INK, 0);
        }
    }

    // 提示符行（输入内容由上面的编辑条显示，这里只画 ">"）
    if (y + LH <= bottom && y >= from_y) {
        g_prompt_y = y;
        so_font_draw(g_font, PADX, y, ">", 25, 1);
    }
}

// 一次重绘：写底（FAST → 点阵）→ 写字（GC13 → 抗锯齿）→ **先 FAST 推一次（快）→
// 再用 TEXT 档推同一块（收干净）**。
//
// ★ 顺序必须是"画完再推屏"。`ttf::push_glyph()` 只写帧缓冲
//   （Panel_EPD::writeImage 只把区域累进 _range_mod），它自己**不会**推屏；
//   原来是先 so_display() 再 draw_text()，于是这一笔字要等**下一条命令**的
//   so_display() 才被带上去，而那一次 so_fill_rect() 已经把最下面几行擦成空白 ——
//   症状就是"输出慢一条命令、屏幕最底下那一两行看不见"。
//
// ★ 两段推（2026-10-22 用户定）：新内容先用 FAST 立刻上屏（230ms 级，有残影），
//   紧接着用 TEXT 档把同一块再推一遍 —— 残影收干净，又不用等一次 QUALITY 那么久。
//   两段都只在"画完之后"发生，帧缓冲内容不变，只是波形不同。
//
// ★ 底一定要在 mode 3（FAST）下**写**：16 级灰经 FAST 的 Bayer 量化才会变成
//   4×4 点阵；写完之后推的时候用哪个档不影响像素值，只影响波形质量。
void paint_area(int32_t y0, int32_t from_y) {
    if (y0 < AREA_Y - 4) y0 = AREA_Y - 4;
    if (y0 >= g_area_bottom) return;
    const int32_t h = g_area_bottom - y0;
    so_font_set_size(g_font, (float)SZ_TXT);
    so_set_epd_mode(3);                          // 写底：FAST → 点阵
    so_fill_rect(0, y0, SCR_W, h, L14);
    so_set_epd_mode(1);                          // 写字：GC13 → 灰阶（AA 才平滑）
    draw_text(g_area_bottom, from_y);
    so_set_epd_mode(3);                          // ① 快：新内容立刻上屏
    so_display(0, y0, SCR_W, h);                 //    入队即返回（等屏会卡宿主，tap 就"愣"）
    so_set_epd_mode(2);                          // ② 收：TEXT 档再刷一遍，残影收干净
    so_display(0, y0, SCR_W, h);                 //    同样只入队；波形档随每次更新一起入队
}

// 整块重绘
void paint_text_mode() { paint_area(AREA_Y - 4, 0); }

// 局部重绘：只有从 y0 往下的这一段（命令只追加了几行、没有溢出时用）
void paint_text_from(int32_t y0) { paint_area(y0, y0); }

void paint_text(const char* why) {
    paint_text_mode();
    char b[64];
    so_snprintf(b, sizeof(b), "推%02d %s", ++g_push_n, why);
    so_log(b);
}

// ── 历史 ────────────────────────────────────────────────────────
void hist_add(const char* cmd) {
    if (!cmd[0]) return;
    if (g_nhist > 0 && strcmp(g_hist[g_nhist - 1], cmd) == 0) return;
    if (g_nhist == HIST_MAX) {
        for (int i = 0; i < (HIST_MAX - 1) * HIST_LEN; i++)
            (&g_hist[0][0])[i] = (&g_hist[1][0])[i];
        g_nhist--;
    }
    so_snprintf(g_hist[g_nhist], HIST_LEN, "%s", cmd);
    g_nhist++;
}

// ── 执行 ────────────────────────────────────────────────────────
void run(const char* raw) {
    char cmd[512];                    // ★ 原来是 HIST_LEN(160)：超过就被静默截断后半截再执行
    so_snprintf(cmd, sizeof(cmd), "%s", raw ? raw : "");
    char* s = cmd;
    while (*s == ' ') s++;
    int L = (int)strlen(s);
    while (L > 0 && (s[L - 1] == ' ' || s[L - 1] == '\n')) s[--L] = 0;
    if (!L) return;

    append_s("> ");
    append_line(s);
    hist_add(s);

    if (strcmp(s, "clear") == 0) { g_len = 0; g_buf[0] = 0; return; }
    if (strcmp(s, "exit") == 0 || strcmp(s, "quit") == 0) { g_quit = true; so_exit(); return; }
    if (strcmp(s, "history") == 0) {
        for (int i = 0; i < g_nhist; i++) {
            char t[HIST_LEN + 10];
            so_snprintf(t, sizeof(t), "  %2d  %s", i + 1, g_hist[i]);
            append_line(t);
        }
        return;
    }
    if (strcmp(s, "help") == 0) {
        append_line("文件   pwd cd ls cat head tail wc df mkdir rm mv cp write append");
        append_line("系统   free ps uptime date ver log");
        append_line("网络   wifi [scan|on|off|conn]  ip");
        append_line("配置   cfg [get <k>|set <k> <v>|save|reset]");
        append_line("电源   reboot     界面   help clear exit history");
        append_line("/apps 自动补成 /sdcard/apps；只能动 /sdcard，apps/*.so 只读");
        return;
    }

    if (!g_out) { append_line("sh: 输出缓冲没分配（PSRAM 不足）"); return; }
    int code = 0, flags = 0;
    const int n = so_sh_exec(s, g_out, OUT_CAP, &code, &flags);
    if (n < 0)      append_line("sh: 固件不支持 so_sh_exec（需要 API v1.14，刷机）");
    else if (n > 0) append(g_out, n);
    if (code == 1)      append_line("（命令不存在，敲 help）");
    else if (code == 2) append_line("（用法不对，敲 help）");
    else if (code == 3) append_line("（路径被拒绝：只能动 /sdcard）");
    else if (code == 4) append_line("（执行失败，看上一行）");
    if (flags & 1) append_line("设备正在重启…");
}

// ── 输入法回调（常驻：回车后键盘仍在）───────────────────────────
void on_ime_done(const char* text) {
    if (!text || !text[0]) return;
    so_snprintf(g_input, sizeof(g_input), "%s", text);
    const int32_t prev_prompt_y = g_prompt_y;         // 记下旧提示符位置（局部刷新的起点）
    run(g_input);
    g_input[0] = 0;

    // ★ 只有"内容真的超出一屏、需要滚动"时才做动画；没超就直接重画一次，别乱滚。
    if (!g_st || !g_en) return;
    if (g_quit) return;                       // 已经退出：别再往屏上画（会盖掉启动器）

    // ★ clear：缓冲空了就是整块重画，否则上面几十行旧字会留在屏上
    //   （原来走"局部重绘"分支，只擦掉最后两行）。
    if (g_len == 0) { paint_text("清屏"); return; }

    const int n = layout_lines(g_st, g_en, LAY_MAX);
    const int lines = (g_area_bottom - AREA_Y) / LH;
    if (n + 1 > lines) {
        paint_text("命令");                  // 超屏：整块重画（先 FAST 上屏，再 TEXT 收）
    } else {
        // 没超屏：只刷"旧提示符那行到下面"，上面的历史一个字都不碰
        paint_text_from(prev_prompt_y > AREA_Y ? prev_prompt_y - LH : AREA_Y);
    }
}

void on_ime_cancel() { /* 常驻键盘没有"取消"：退出用 exit 命令 */ }

void boot_text() {
    g_len = 0;
    if (g_buf) g_buf[0] = 0;
    append_line("papers3 terminal · v2");
    append_line("");
    append_line("root@papers3:/sdcard #");
}

}  // namespace

extern "C" __attribute__((visibility("default")))
void page_init(void) {
    if (!g_buf) g_buf = (char*)so_psram_alloc(SCROLL_CAP + 1);   // +1：始终留得下结尾的 NUL
    if (!g_out) g_out = (char*)so_psram_alloc(OUT_CAP);
    if (!g_st)  g_st  = (int*)so_psram_alloc(sizeof(int) * LAY_MAX);
    if (!g_en)  g_en  = (int*)so_psram_alloc(sizeof(int) * LAY_MAX);
    if (!g_font) g_font = so_font_builtin((float)SZ_TXT);

    // ★ 问固件要「键盘覆盖层顶边」：编辑条高度、候选栏位置都可能变，抄常量必然漂。
    {
        const int by = so_ime_band_y();
        if (by > 100 && by <= SCR_H) g_area_bottom = by - 4;
    }
    g_kb_need = true;      // 每次进 app 都要重新拉键盘（.so 不 dlclose，static 会残留）
    g_quit    = false;
    boot_text();

    // ★ 底必须在 mode 3 下**写**：经 FAST 的 Bayer 量化才是 4×4 点阵。
    //   原来这里先用 mode 1 写 → 进屏那几行是**平灰**，而之后局部重绘过的区域是点阵，
    //   两者背景差一个色（用户一眼看出来的那条分界）。整块铺到 960，键盘带也一致。
    so_set_epd_mode(3);
    so_fill_rect(0, BAR_H, SCR_W, SCR_H - BAR_H, L14);
    so_set_epd_mode(1);                       // 写字：GC13（抗锯齿灰）
    so_font_set_size(g_font, (float)SZ_TXT);
    so_bar_set("< EXIT", nullptr);            // 全局状态栏：左键 = 退出本 app（固件画、固件点）
    draw_text(g_area_bottom);
    // ★ 只推 BAR_H 以下：状态栏由 sbar 自己推（它在 so_bar_set 里已经推过整条）。
    so_display(0, BAR_H, SCR_W, SCR_H - BAR_H);   // 入队即返回，别等屏（否则进 app 也卡一下）
    so_log("终端：进屏（常驻键盘）");

    // 键盘在**第一帧 tick** 里开（page_init 阶段页面还没真正上屏，覆盖层会被丢掉）
}

extern "C" __attribute__((visibility("default")))
void page_tick(void) {
    // 第一帧：拉起常驻键盘（page_init 里开无效——那时页面还没真正上屏，覆盖层会被丢掉）
    // ★ 不能用函数内 static：.so 退出时**不 dlclose**，static 跨启动保留 → 第二次进终端
    //   就再也不开键盘（用户 2026-10-22 报的"有时候键盘不出来"就是这个）。
    //   改成文件级标志，在 page_init 里复位。
    if (g_kb_need) {
        g_kb_need = false;
        so_ime_set_sticky(1);
        if (so_ime_open("", on_ime_done, on_ime_cancel) != 0) {
            append_line("输入法打不开（固件太旧，需要 API 1.16）");
            paint_text("无键盘");
        }
        return;
    }
    // 之后：什么都不做（键盘常驻、触摸归固件、状态栏归固件组件）
}

extern "C" __attribute__((visibility("default")))
uint32_t so_app_api_version(void) { return SO_API_VERSION; }

extern "C" __attribute__((visibility("default")))
const char* so_app_name(void) { return "终端"; }

SO_APP_META("终端", "真 shell · 文件/内存/任务/配置/重启", 32, 32, 1,
                 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1F, 0xFF, 0xFF, 0xF8, 0x1F, 0xFF, 0xFF, 0xF8,
                 0x18, 0x00, 0x00, 0x18, 0x18, 0x00, 0x00, 0x18, 0x18, 0x00, 0x00, 0x18, 0x18, 0x00, 0x00, 0x18,
                 0x18, 0x60, 0x00, 0x18, 0x18, 0x61, 0x80, 0x18, 0x18, 0x19, 0x80, 0x18, 0x18, 0x1E, 0x00, 0x18,
                 0x18, 0x06, 0x00, 0x18, 0x18, 0x1E, 0x00, 0x18, 0x18, 0x19, 0x80, 0x18, 0x18, 0x61, 0xBF, 0x98,
                 0x18, 0x60, 0x3F, 0x98, 0x18, 0x00, 0x3F, 0x98, 0x18, 0x00, 0x00, 0x18, 0x18, 0x00, 0x00, 0x18,
                 0x1F, 0xFF, 0xFF, 0xF8, 0x1F, 0xFF, 0xFF, 0xF8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00);
