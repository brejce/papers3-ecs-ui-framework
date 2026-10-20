// apps/reader/main/ui.h — 阅读器自己的风格 helper（终端风 v3）
//
// ★ 与 apps/settings/main/settings.cpp、apps/terminal/main/terminal.cpp 是**同一套约定**，
//   不是共享头文件（见 docs/design/UI_DESIGN.md §0）：改风格要三处一起改。
//   本文件只放「画」与「几何」，不放业务。
//
// 规则速查（UI_DESIGN.md §2/§4/§6）：
//   点阵底 = MF 档写 0xE8E8E8（FAST 档 Bayer 量化 → 4×4 一格黑点）
//   文字 = MT 档；灰 = ink 亮度档 25 / 90（不是颜色）
//   黑块只给唯一主操作；选中 = 2px 框 + 方括号 / 行首 `> `；未选 = 1px 灰框 + 灰字
#pragma once
#include <cstdint>
#include <cstring>
#include "../common/so_api.h"

namespace app::ui {

struct R { int32_t x, y, w, h; };

inline constexpr int32_t SCR_W = 540, SCR_H = 960, PADX = 24, BAR_H = 60;

inline constexpr uint32_t L14   = 0xE8E8E8;   // 点阵底（FAST 档下）
inline constexpr uint32_t INK   = 0x000000;
inline constexpr uint32_t WHITE = 0xFFFFFF;
inline constexpr uint32_t GLINE = 0x888888;
inline constexpr uint8_t  GRAY  = 25;         // 键名 / 小标题 / 副提示 / 旧日志
inline constexpr uint8_t  GRAYD = 90;         // 禁用 / 说明小字

inline constexpr int MQ = 1, MT = 2, MF = 3, MX = 4;   // QUALITY/TEXT/FAST/FASTEST

// 字号档位（§5.1，禁止新增档位）
inline constexpr int SZ_BAR = 20, SZ_H = 20, SZ_KEY = 19, SZ_VAL = 22, SZ_HERO = 38,
                     SZ_SEG = 21, SZ_CMT = 19, SZ_LOG = 20, SZ_BTN = 24, SZ_SUB_HINT = 20;

inline void* g_font = nullptr;    // 全局唯一字体句柄（reader.cpp 初始化 / 换字体重载）
inline int   g_push_n = 0;        // push 序号（日志里看得见第几次）

// ── 字符串（白名单里没有 snprintf 之外的格式化，也没有 strncmp/strstr）──
inline int slen(const char* s) { int n = 0; while (s && s[n]) ++n; return n; }
inline void scopy(char* d, const char* s, int cap) {
    if (!d || cap <= 0) return;
    int i = 0;
    if (s) for (; s[i] && i < cap - 1; ++i) d[i] = s[i];
    d[i] = 0;
}
inline bool streq(const char* a, const char* b) {
    if (!a || !b) return a == b;
    int i = 0;
    while (a[i] && b[i]) { if (a[i] != b[i]) return false; ++i; }
    return a[i] == b[i];
}

// ── 推屏：**每次带原因**（串口 + SD），否则「哪块在刷」只能靠猜 ──
inline void push(const R& r, const char* why) {
    so_display(r.x, r.y, r.w, r.h);
    so_wait_display();
    char b[96];
    so_snprintf(b, sizeof(b), "推%03d %-10s (%d,%d %dx%d)", ++g_push_n, why, r.x, r.y, r.w, r.h);
    so_log(b);
    so_log_file(b);
}

// ── 底与线 ──
inline void box(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t c) { so_fill_rect(x, y, w, h, c); }
inline void box(const R& r, uint32_t c) { so_fill_rect(r.x, r.y, r.w, r.h, c); }
inline void frame(const R& r, int32_t t, uint32_t c) {
    box(r.x, r.y, r.w, t, c);
    box(r.x, r.y + r.h - t, r.w, t, c);
    box(r.x, r.y, t, r.h, c);
    box(r.x + r.w - t, r.y, t, r.h, c);
}
inline void dashed_h(int32_t x, int32_t y, int32_t w, uint32_t c) {
    for (int32_t i = 0; i < w; i += 8) box(x + i, y, (w - i) < 5 ? (w - i) : 5, 1, c);
}
inline void dashed_frame(const R& r, int32_t t, uint32_t c) {
    for (int32_t x = r.x; x < r.x + r.w; x += 8) {
        const int32_t w = (r.x + r.w - x) < 5 ? (r.x + r.w - x) : 5;
        box(x, r.y, w, t, c); box(x, r.y + r.h - t, w, t, c);
    }
    for (int32_t y = r.y; y < r.y + r.h; y += 8) {
        const int32_t h = (r.y + r.h - y) < 5 ? (r.y + r.h - y) : 5;
        box(r.x, y, t, h, c); box(r.x + r.w - t, y, t, h, c);
    }
}

// ── 文字 ──
inline void text(int32_t x, int32_t y, const char* s, uint8_t ink, int32_t ls) {
    so_font_draw(g_font, x, y, s, ink, ls);
}
inline int m_lh(int px) { so_font_set_size(g_font, (float)px); return so_font_line_height(g_font); }
inline int text_w(const char* s, int px, int ls) {
    so_font_set_size(g_font, (float)px);
    int32_t w = 0, h = 0;
    so_font_measure(g_font, s, &w, &h);
    const int n = slen(s);
    return w + (n > 0 ? (n - 1) * ls : 0);
}
// 整串真墨迹框（左右）；0 = 量不到
inline int text_span(const char* s, int px, int ls, int* l, int* r) {
    so_font_set_size(g_font, (float)px);
    int32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    if (so_font_text_ink(g_font, s, ls, &x0, &y0, &x1, &y1) != 0) return 0;
    if (x1 <= x0) return 0;
    *l = x0; *r = x1;
    return 1;
}
// 墨迹盒（擦旧值用，比行盒小）
inline R ink_rect(int32_t x, int32_t y, const char* s, int px, int ls) {
    int l = 0, r = 0;
    if (!text_span(s, px, ls, &l, &r)) return (R){0, 0, 0, 0};
    return (R){x + l - 1, y, r - l + 2, m_lh(px)};
}
// 放不下就压字间距、再缩字号（标题/书名用）
inline void text_fit(int32_t x, int32_t y, const char* s, uint8_t ink, int ls, int px, int32_t right) {
    so_font_set_size(g_font, (float)px);
    if (x + text_w(s, px, ls) > right) {
        ls = 0;
        while (px > 11 && x + text_w(s, px, 0) > right) { px -= 2; so_font_set_size(g_font, (float)px); }
    }
    so_font_draw(g_font, x, y, s, ink, ls);
}
// 点线引导（KEY ........ VALUE）：点距 6，量到刚好不越右界
inline void dot_leader(int32_t x0, int32_t x1, int32_t y, int px) {
    if (x1 - x0 < 24) return;
    char d[80];
    int n = 0;
    for (; n < 72; ++n) {
        d[n] = '.'; d[n + 1] = 0;
        int l = 0, r = 0;
        if (!text_span(d, px, 3, &l, &r) || x0 + r > x1) break;
    }
    if (n < 3) return;
    d[n] = 0;
    so_font_set_size(g_font, (float)px);
    text(x0, y, d, GRAY, 3);
}

// ── 通用部件 ──
inline void draw_hero(int32_t y, const char* title, const char* sub) {
    so_font_set_size(g_font, SZ_HERO);
    text(PADX, y, title, INK, 0);
    so_font_set_size(g_font, SZ_SUB_HINT);
    text(PADX, y + 56, sub, GRAY, 1);
}
inline constexpr int SZ_SUB_HINT_DUMMY_UNUSED = 0;
inline void draw_h(int32_t y, const char* t) {           // // 小标题
    so_font_set_size(g_font, SZ_H);
    text(PADX, y, t, GRAY, 2);
}
inline void draw_cmt(int32_t y, const char* t) {
    so_font_set_size(g_font, SZ_CMT);
    text(PADX, y, t, GRAYD, 1);
}
inline void draw_log(int32_t y, const char* t, uint8_t ink) {   // > 日志行
    char b[128];
    so_snprintf(b, sizeof(b), "> %s", t);
    so_font_set_size(g_font, SZ_LOG);
    text(PADX, y, b, ink, 1);
}
inline void draw_kv(int32_t y, const char* key, const char* val) {
    so_font_set_size(g_font, SZ_KEY);
    const int ky = y + (m_lh(SZ_VAL) - m_lh(SZ_KEY)) / 2;
    text(PADX, ky, key, GRAY, 1);
    const int32_t kx1 = PADX + text_w(key, SZ_KEY, 1);
    so_font_set_size(g_font, SZ_VAL);
    const int32_t vx0 = SCR_W - PADX - text_w(val, SZ_VAL, 1);
    text(vx0, y, val, INK, 1);
    dot_leader(kx1 + 10, vx0 - 10, ky, SZ_KEY);
}
inline void draw_seg(const R& r, const char* label, int sel) {
    char b[48];
    if (sel) so_snprintf(b, sizeof(b), "[ %s ]", label);
    else     so_snprintf(b, sizeof(b), "%s", label);
    frame(r, sel ? 2 : 1, sel ? INK : GLINE);
    so_font_set_size(g_font, SZ_SEG);
    text(r.x + (r.w - text_w(b, SZ_SEG, 1)) / 2,
         r.y + (r.h - m_lh(SZ_SEG)) / 2, b, sel ? INK : GRAYD, 1);
}
// 列表项：选中 = 2px 框 + 行首 "> "；未选 = 1px 框；空槽 = 灰虚线框 (empty)
inline void draw_li(const R& r, const char* t, const char* right, int sel, int empty) {
    if (empty) {
        dashed_frame(r, 1, GLINE);
        so_font_set_size(g_font, SZ_VAL);
        text(r.x + 14, r.y + (r.h - m_lh(SZ_VAL)) / 2, "(empty)", GRAYD, 1);
        return;
    }
    frame(r, sel ? 2 : 1, INK);
    so_font_set_size(g_font, SZ_VAL);
    const int32_t ty = r.y + (r.h - m_lh(SZ_VAL)) / 2;
    if (sel) text(r.x + 12, ty, ">", INK, 1);
    text_fit(r.x + (sel ? 34 : 14), ty, t, INK, 1, SZ_VAL, r.x + r.w - 90);
    if (right && right[0]) {
        so_font_set_size(g_font, SZ_CMT);
        text(r.x + r.w - 14 - text_w(right, SZ_CMT, 1), r.y + (r.h - m_lh(SZ_CMT)) / 2, right, GRAYD, 1);
    }
}
// 警告框（全屏唯一加粗框）
inline void draw_warn(int32_t y, const char* t, int px) {
    const R r = {PADX, y, SCR_W - 2 * PADX, 56};
    frame(r, 2, INK);
    so_font_set_size(g_font, px);
    text(r.x + (r.w - text_w(t, px, 2)) / 2, r.y + (r.h - m_lh(px)) / 2, t, INK, 2);
}

// ── 状态栏（左 `< EXIT` / `< BACK`，中时钟真墨迹居中，右 BAT n%）──
inline char g_clock[8] = "--:--";
inline int  g_bat = -1;

inline void bar_refresh(void) {
    const int t = so_clock_hhmm();
    if (t < 0) scopy(g_clock, "--:--", sizeof(g_clock));
    else       so_snprintf(g_clock, sizeof(g_clock), "%02d:%02d", t / 100, t % 100);
    g_bat = so_battery_level();
}
inline int32_t clock_x(const char* s) {
    so_font_set_size(g_font, SZ_BAR);
    int l = 0, r = 0;
    if (text_span(s, SZ_BAR, 2, &l, &r)) return (SCR_W - (r - l)) / 2 - l;
    return (SCR_W - text_w(s, SZ_BAR, 2)) / 2;
}
inline void bar_draw(const char* left) {
    so_font_set_size(g_font, SZ_BAR);
    text(PADX, (BAR_H - m_lh(SZ_BAR)) / 2, left, INK, 2);
    text(clock_x(g_clock), (BAR_H - m_lh(SZ_BAR)) / 2, g_clock, GRAY, 2);
    text(SCR_W - PADX - 3 * so_font_measure_prefix(g_font, "0", 1) - 12 - 44,
         (BAR_H - m_lh(SZ_BAR)) / 2, "BAT", GRAY, 2);
    char b[8];
    if (g_bat < 0) scopy(b, "--", sizeof(b));
    else           so_snprintf(b, sizeof(b), "%d", g_bat);
    so_font_set_size(g_font, SZ_BAR);
    const int32_t cw = so_font_measure_prefix(g_font, "0", 1);
    text(SCR_W - PADX - slen(b) * cw, (BAR_H - m_lh(SZ_BAR)) / 2, b, INK, 0);
    text(SCR_W - PADX + 3, (BAR_H - m_lh(SZ_BAR)) / 2, "%", INK, 0);
    box(0, BAR_H - 1, SCR_W, 1, INK);
}
// 状态栏左键热区（点它 = 退出/返回）
inline bool hit_bar_left(int32_t x, int32_t y) { return y < BAR_H + 14 && x < 170; }

} // namespace app::ui
