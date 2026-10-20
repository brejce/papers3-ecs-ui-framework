// apps/settings/main/settings.cpp — 系统设置 (.so app) · 终端风格 v3
//
// 版面严格照 docs/design/UI_DESIGN.md §7.2（六屏 + 子页），老稿 design/terminal-style/settings-terminal.html 仅作参考，
// 选中态按收紧后的规则（见 docs/design/UI_DESIGN.md §4）：无整块反白，实心黑块只有底部 COMMIT。
//   段式按钮：选中 = 2px 黑框 + [ X ]；未选 = 1px 灰框 + 灰字
//   开关：当前态 2px 框 + 黑字，另一态灰小字无框
//   列表项：选中 = 2px 框 + 行首 "> "；空槽 = 灰虚线框 (empty)
//   返回 = 状态栏左 < BACK（菜单页 < EXIT = 退出 app）
//
// 刷新纪律（照 docs/design/UI_DESIGN.md §9）：静态只画一次；改设置**只擦写变的那一小块**——
//   开关 → 那一个开关框；段式 → 那一组；列表 → 新旧两行；顺带那一行日志。
//   只有切页才整屏 QUALITY。擦 = FAST 写 L14（与底同档），写 = TEXT。每次 push 带 why。
#include "../common/so_api.h"
#include "../common/so_meta.h"
#include <cstring>

// ══════════════════════════════════════════════════════════════
// 基础
// ══════════════════════════════════════════════════════════════
struct R { int x, y, w, h; };
static const int SCR_W = 540, SCR_H = 960;
static const int PADX  = 24;
static const int BAR_H = 60;

static const uint32_t L14   = 0xE8E8E8;      // FAST 档下的点阵底
static const uint32_t INK   = 0x000000;
static const uint32_t WHITE = 0xFFFFFF;
static const uint32_t GLINE = 0x888888;      // 未选中 1px 灰框
static const uint8_t  GRAY  = 25;            // 键名/标题/旧日志/副提示
static const uint8_t  GRAYD = 90;            // 禁用/说明的浅灰

static const int MQ = 1, MT = 2, MF = 3, MX = 4;   // QUALITY/TEXT/FAST/FASTEST

// 字号分类（默认 = 原写死值）。运行时可变，存 /sdcard/apps/settings_sz.cfg。
static int SZ_BAR = 20, SZ_HERO = 38, SZ_SUB = 20, SZ_H = 20,
           SZ_KEY = 22, SZ_VAL = 22, SZ_SEG = 21, SZ_BTN = 24,
           SZ_LOG = 22, SZ_CMT = 19;

struct SzCat { const char* cn; const char* hint; int* v; int lo; int hi; };
static SzCat g_szcat[10] = {
    {"状态栏",   "顶栏时钟", &SZ_BAR,  14, 32},
    {"大字标题", "CONFIG",  &SZ_HERO, 22, 56},
    {"副标题",   "//说明",   &SZ_SUB,  14, 32},
    {"小标题",   "//WIFI",   &SZ_H,    14, 32},
    {"键名",     "STATE键",  &SZ_KEY,  14, 40},
    {"正文",     "菜单列表", &SZ_VAL,  14, 40},
    {"开关",     "OFF/ON",   &SZ_SEG,  14, 40},
    {"按钮",     "COMMIT",   &SZ_BTN,  16, 44},
    {"日志",     ">提示",     &SZ_LOG,  14, 40},
    {"注释",     "说明小字", &SZ_CMT,  12, 36},
};
static const int SZ_N = 10;
static const int SZ_UI = 22;   // 微调页自身的固定字号（不跟被调字号跑）

static int atoi2(const char* s) {
    int v = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); ++s; }
    return v;
}
static void sz_save(void) {
    char buf[256]; int off = 0;
    for (int i = 0; i < SZ_N; ++i)
        off += so_snprintf(buf + off, sizeof(buf) - off, "%d ", *g_szcat[i].v);
    so_blob_save("settings_sz.cfg", buf, off);
}
static void sz_load(void) {
    char buf[256] = "";
    if (so_blob_load("settings_sz.cfg", buf, sizeof(buf)) <= 0) return;
    char* p = buf;
    for (int i = 0; i < SZ_N; ++i) {
        while (*p == ' ') ++p;
        if (*p < '0' || *p > '9') return;
        int v = atoi2(p);
        while (*p >= '0' && *p <= '9') ++p;
        if (v < g_szcat[i].lo) v = g_szcat[i].lo;
        if (v > g_szcat[i].hi) v = g_szcat[i].hi;
        *g_szcat[i].v = v;
    }
}

static void* g_font = nullptr;
static int   g_push_n = 0;

static int slen0(const char* s) { int n = 0; while (s[n]) ++n; return n; }
static void scopy(char* d, const char* s, int cap) {
    int i = 0; for (; s[i] && i < cap - 1; ++i) d[i] = s[i]; d[i] = 0;
}
static int streq(const char* a, const char* b) {
    int i = 0; while (a[i] && b[i]) { if (a[i] != b[i]) return 0; ++i; }
    return a[i] == b[i];
}

static void push(const R& r, const char* why) {
    so_display(r.x, r.y, r.w, r.h);
    so_wait_display();
    char b[96];
    so_snprintf(b, sizeof(b), "推%03d %-10s (%d,%d %dx%d)", ++g_push_n, why, r.x, r.y, r.w, r.h);
    so_log(b);
    so_log_file(b);
}
static void box(const R& r, uint32_t c) { so_fill_rect(r.x, r.y, r.w, r.h, c); }
static void box(int x, int y, int w, int h, uint32_t c) { so_fill_rect(x, y, w, h, c); }
static void frame(const R& r, int t, uint32_t c) {
    box(r.x, r.y, r.w, t, c);
    box(r.x, r.y + r.h - t, r.w, t, c);
    box(r.x, r.y, t, r.h, c);
    box(r.x + r.w - t, r.y, t, r.h, c);
}
static void dashed_h(int x, int y, int w, uint32_t c) {
    for (int i = 0; i < w; i += 8) box(x + i, y, (w - i) < 5 ? (w - i) : 5, 1, c);
}
static void dashed_frame(const R& r, int t, uint32_t c) {
    for (int x = r.x; x < r.x + r.w; x += 8) {
        const int w = (r.x + r.w - x) < 5 ? (r.x + r.w - x) : 5;
        box(x, r.y, w, t, c); box(x, r.y + r.h - t, w, t, c);
    }
    for (int y = r.y; y < r.y + r.h; y += 8) {
        const int h = (r.y + r.h - y) < 5 ? (r.y + r.h - y) : 5;
        box(r.x, y, t, h, c); box(r.x + r.w - t, y, t, h, c);
    }
}
static void text(int x, int y, const char* s, uint8_t ink, int ls) { so_font_draw(g_font, x, y, s, ink, ls); }
static int m_lh(int px) { so_font_set_size(g_font, (float)px); return so_font_line_height(g_font); }
static int text_w(const char* s, int px, int ls) {
    so_font_set_size(g_font, (float)px);
    int32_t w = 0, h = 0; so_font_measure(g_font, s, &w, &h);
    int n = slen0(s);
    return w + (n > 0 ? (n - 1) * ls : 0);
}
static int text_span(const char* s, int px, int ls, int* l, int* r) {
    so_font_set_size(g_font, (float)px);
    int32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    if (so_font_text_ink(g_font, s, ls, &x0, &y0, &x1, &y1) != 0) return 0;
    if (x1 <= x0) return 0;
    *l = x0; *r = x1; return 1;
}
static R ink_rect(int x, int y, const char* s, int px, int ls) {
    int l = 0, r = 0;
    if (!text_span(s, px, ls, &l, &r)) return (R){0, 0, 0, 0};
    return (R){x + l - 1, y, r - l + 2, m_lh(px)};
}
static void text_fit(int x, int y, const char* s, uint8_t ink, int ls, int px, int right) {
    so_font_set_size(g_font, (float)px);
    if (x + text_w(s, px, ls) > right) {
        ls = 0;
        while (px > 11 && x + text_w(s, px, 0) > right) { px -= 2; so_font_set_size(g_font, (float)px); }
    }
    so_font_draw(g_font, x, y, s, ink, ls);
}
static void dot_leader(int x0, int x1, int y, int px) {
    if (x1 - x0 < 24) return;
    char d[72]; int n = 0;
    for (; n < 70; ++n) {
        d[n] = '.'; d[n + 1] = 0;
        int l = 0, r = 0;
        if (!text_span(d, px, 3, &l, &r) || x0 + r > x1) break;
    }
    if (n < 3) return;
    d[n] = 0;
    so_font_set_size(g_font, (float)px);
    text(x0, y, d, GRAY, 3);
}

// ══════════════════════════════════════════════════════════════
// 通用部件
// ══════════════════════════════════════════════════════════════
static void draw_hero(const char* title, const char* sub) {
    so_font_set_size(g_font, SZ_HERO);
    text(PADX, 80, title, INK, 0);
    so_font_set_size(g_font, SZ_SUB);
    text(PADX, 136, sub, GRAY, 1);
}
static void draw_h(int y, const char* t) {
    so_font_set_size(g_font, SZ_H);
    text(PADX, y, t, GRAY, 2);
}
static void draw_cmt(int y, const char* t) {
    so_font_set_size(g_font, SZ_CMT);
    text(PADX, y, t, GRAYD, 1);
}
static void draw_log(int y, const char* t, uint8_t ink) {
    char b[96];
    so_snprintf(b, sizeof(b), "> %s", t);
    so_font_set_size(g_font, SZ_LOG);
    text(PADX, y, b, ink, 1);
}
static void draw_kv(int y, const char* key, const char* val) {
    so_font_set_size(g_font, SZ_KEY);
    const int ky = y + (m_lh(SZ_VAL) - m_lh(SZ_KEY)) / 2;
    text(PADX, ky, key, GRAY, 1);
    const int kx1 = PADX + text_w(key, SZ_KEY, 1);
    so_font_set_size(g_font, SZ_VAL);
    const int vx0 = SCR_W - PADX - text_w(val, SZ_VAL, 1);
    text(vx0, y, val, INK, 1);
    dot_leader(kx1 + 10, vx0 - 10, ky, SZ_KEY);
}
static void draw_seg(const R& r, const char* label, int sel) {
    char b[48];
    if (sel) so_snprintf(b, sizeof(b), "[ %s ]", label);
    else     so_snprintf(b, sizeof(b), "%s", label);
    frame(r, sel ? 2 : 1, sel ? INK : GLINE);
    so_font_set_size(g_font, SZ_SEG);
    text(r.x + (r.w - text_w(b, SZ_SEG, 1)) / 2,
         r.y + (r.h - m_lh(SZ_SEG)) / 2, b, sel ? INK : GRAYD, 1);
}
static void draw_toggle(const R& r, int on) {
    const int cellw = 46, gap = 12, h = 44;
    const int x0 = r.x + r.w - (cellw * 2 + gap);
    const int cy = r.y + (r.h - h) / 2;
    const R c1 = {x0, cy, cellw, h};
    const R c2 = {x0 + cellw + gap, cy, cellw, h};
    so_font_set_size(g_font, SZ_VAL);
    if (on) {
        text(c1.x, c1.y + (h - m_lh(SZ_VAL)) / 2, "OFF", GRAYD, 2);
        frame(c2, 2, INK);
        text(c2.x + (cellw - text_w("ON", SZ_VAL, 2)) / 2, c2.y + (h - m_lh(SZ_VAL)) / 2, "ON", INK, 2);
    } else {
        frame(c1, 2, INK);
        text(c1.x + (cellw - text_w("OFF", SZ_VAL, 2)) / 2, c1.y + (h - m_lh(SZ_VAL)) / 2, "OFF", INK, 2);
        text(c2.x, c2.y + (h - m_lh(SZ_VAL)) / 2, "ON", GRAYD, 2);
    }
}
static R draw_togglerow(int y, const char* key, int on) {
    so_font_set_size(g_font, SZ_KEY);
    const int ky = y + (44 - m_lh(SZ_KEY)) / 2;
    text(PADX, ky, key, GRAY, 1);
    const R t = {SCR_W - PADX - (46 * 2 + 12), y, 46 * 2 + 12, 44};
    dot_leader(PADX + text_w(key, SZ_KEY, 1) + 10, t.x - 10, ky, SZ_KEY);
    draw_toggle(t, on);
    return t;
}
static void draw_li(const R& r, const char* t, const char* right, int sel, int empty) {
    if (empty) {
        dashed_frame(r, 1, GLINE);
        so_font_set_size(g_font, SZ_VAL);
        text(r.x + 14, r.y + (r.h - m_lh(SZ_VAL)) / 2, "(empty)", GRAYD, 1);
        return;
    }
    frame(r, sel ? 2 : 1, INK);
    so_font_set_size(g_font, SZ_VAL);
    const int ty = r.y + (r.h - m_lh(SZ_VAL)) / 2;
    if (sel) text(r.x + 12, ty, ">", INK, 1);
    text_fit(r.x + (sel ? 34 : 14), ty, t, INK, 1, SZ_VAL, r.x + r.w - 80);
    if (right && right[0]) {
        so_font_set_size(g_font, SZ_CMT);
        text(r.x + r.w - 14 - text_w(right, SZ_CMT, 1), r.y + (r.h - m_lh(SZ_CMT)) / 2, right, GRAYD, 1);
    }
}
static void draw_warn(int y, const char* t, int px) {
    const R r = {PADX, y, SCR_W - 2 * PADX, 56};
    frame(r, 2, INK);
    so_font_set_size(g_font, px);
    text(r.x + (r.w - text_w(t, px, 2)) / 2, r.y + (r.h - m_lh(px)) / 2, t, INK, 2);
}
static void draw_commit(int pressed) {
    const R r = {PADX, 840, SCR_W - 2 * PADX, 80};
    box(r, INK);
    frame(r, 2, WHITE);
    so_font_set_size(g_font, SZ_BTN);
    text(r.x + (r.w - text_w("COMMIT", SZ_BTN, 12)) / 2,
         r.y + (r.h - m_lh(SZ_BTN)) / 2, "COMMIT", 255, 12);
    if (pressed) {
        so_set_epd_mode(MT);
        box(r.x, r.y, r.w, 4, WHITE); box(r.x, r.y, 4, r.h, WHITE);
        box(r.x, r.y + r.h - 6, r.w, 6, WHITE); box(r.x + r.w - 6, r.y, 6, r.h, WHITE);
    }
}

// ══════════════════════════════════════════════════════════════
// 状态栏
// ══════════════════════════════════════════════════════════════
static char g_clock[8] = "--:--";
static int  g_bat = -1;

static void clock_str(char* out, int cap) {
    const int t = so_clock_hhmm();
    if (t < 0) scopy(out, "--:--", cap);
    else so_snprintf(out, cap, "%02d:%02d", t / 100, t % 100);
}
static int clock_x(const char* s) {
    so_font_set_size(g_font, SZ_BAR);
    int l = 0, r = 0;
    if (text_span(s, SZ_BAR, 2, &l, &r)) return (SCR_W - (r - l)) / 2 - l;
    return (SCR_W - text_w(s, SZ_BAR, 2)) / 2;
}
static void bar_static(int is_home) {
    so_font_set_size(g_font, SZ_BAR);
    text(PADX, (BAR_H - m_lh(SZ_BAR)) / 2, is_home ? "< EXIT" : "< BACK", INK, 2);
    clock_str(g_clock, sizeof(g_clock));
    text(clock_x(g_clock), (BAR_H - m_lh(SZ_BAR)) / 2, g_clock, GRAY, 2);
    text(SCR_W - PADX - 3 * so_font_measure_prefix(g_font, "0", 1) - 12 - 44,
         (BAR_H - m_lh(SZ_BAR)) / 2, "BAT", GRAY, 2);
    char b[8];
    if (g_bat < 0) scopy(b, "--", 8);
    else so_snprintf(b, sizeof(b), "%d", g_bat);
    so_font_set_size(g_font, SZ_BAR);
    const int cw = so_font_measure_prefix(g_font, "0", 1);
    text(SCR_W - PADX - slen0(b) * cw, (BAR_H - m_lh(SZ_BAR)) / 2, b, INK, 0);
    text(SCR_W - PADX + 3, (BAR_H - m_lh(SZ_BAR)) / 2, "%", INK, 0);
    box(0, BAR_H - 1, SCR_W, 1, INK);
}
// 电量变了：只刷状态栏右侧那几位数字
static void bar_bat_push(void) {
    char b[8];
    if (g_bat < 0) scopy(b, "--", 8);
    else so_snprintf(b, sizeof(b), "%d", g_bat);
    so_font_set_size(g_font, SZ_BAR);
    const int cw = so_font_measure_prefix(g_font, "0", 1);
    const int y = (BAR_H - m_lh(SZ_BAR)) / 2;
    const R r = {SCR_W - PADX - 3 * cw - 2, y - 2, 3 * cw + 4 + text_w("%", SZ_BAR, 0) + 4, m_lh(SZ_BAR) + 4};
    so_set_epd_mode(MF); box(r, L14);
    so_set_epd_mode(MT);
    text(SCR_W - PADX - slen0(b) * cw, y, b, INK, 0);
    text(SCR_W - PADX + 3, y, "%", INK, 0);
    push(r, "电量");
}

// ══════════════════════════════════════════════════════════════
// 页 + 状态机
// ══════════════════════════════════════════════════════════════
enum { PG_MENU = 0, PG_NET = 1, PG_DISP = 2, PG_FONT = 3, PG_PWR = 5, PG_SAVER = 6, PG_ABOUT = 7, PG_WIFI = 8, PG_FACE = 9, PG_SND = 10, PG_SNDFILE = 11 };
static int g_pg = PG_MENU;
static int g_need_full = 1;
static int g_dirty = 0;         // 有改动还没 COMMIT
static int g_dbl_act = 0, g_dbl_t = 0;
static int g_clock_t = 0, g_slow_t = 0;
static int g_press_c = 0, g_press_f = 0;   // COMMIT / FACTORY 按下反馈的弹起时间戳
static char g_msg[96] = "";

// ── WiFi 页：已存热点 + 附近热点（带信号）+ 添加新热点 ──
static char   g_aps[8][33];         // 扫描到的 SSID
static int8_t g_aps_rssi[8];        // 信号强度 (dBm)
static int    g_nap = 0;
static char   g_saved[8][33];       // 已存热点 SSID
static int    g_nsaved = 0;
static char   g_conn_ssid[33] = ""; // 点中的新热点（等口令）
static char   g_add_ssid[33] = "";  // 添加新热点：第一步输的 SSID
static int    g_ime_field = -1;     // >=0 = 输入法开着（也判断键盘是否盖着）
static int    g_ime_mode = 0;       // 0=给新热点输口令 1=添加时输SSID 2=添加时输口令
static int    g_need_ime = 0;       // 1=done 里要连着再开一次输入法（下一帧开）
static int    g_wifi_scan = 0;      // 1=进 WiFi 页/连接后 待扫描（tick 里真扫）

static R   g_nap_r[8]; static int g_nap_id[8]; static int g_nnap = 0;   // 附近热点行（和 g_li 分开记）
static int g_nap_off = 0;      // 附近列表滚动偏移（第几行开始）
static const int NAP_SHOW = 5; // 每屏显示行数
static R g_up_r, g_dn_r;       // ▲▼ 滚动按钮

static int ap_saved(const char* ssid) {
    for (int i = 0; i < g_nsaved; ++i)
        if (streq(ssid, g_saved[i])) return 1;
    return 0;
}
static int nap_hit(int x, int y) {
    for (int i = 0; i < g_nnap; ++i)
        if (x >= g_nap_r[i].x && x < g_nap_r[i].x + g_nap_r[i].w &&
            y >= g_nap_r[i].y && y < g_nap_r[i].y + g_nap_r[i].h) return i;
    return -1;
}
static const char* rssi_txt(int8_t r) { return r >= -55 ? "强" : r >= -70 ? "中" : "弱"; }
// 输入法「完成」回调（固件缓冲，要立刻用掉）
static void on_ime_done(const char* text) {
    if (g_ime_mode == 1) {   // 添加：SSID 输完 → 接着输口令（可空 = 开放网络）
        scopy(g_add_ssid, text, sizeof(g_add_ssid));
        g_ime_mode = 2;
        g_need_ime = 1;
        g_ime_field = -1;
    } else {                 // 0=新热点口令 / 2=添加的口令 → 连接
        const char* ssid = (g_ime_mode == 0) ? g_conn_ssid : g_add_ssid;
        so_wifi_connect(ssid, text);
        g_ime_mode = 0;
        g_ime_field = -1;
        g_wifi_scan = 1;
        g_need_full = 1;
    }
}

// 输入法「×」取消回调：中断输入，回到正常 WiFi 页（弹窗消失）
static void on_ime_cancel(void) {
    g_ime_mode = 0;
    g_ime_field = -1;
    g_conn_ssid[0] = 0;
    g_add_ssid[0] = 0;
    g_need_full = 1;   // 重画 WiFi 页，去掉弹窗
}

// ── 输入弹窗 ──
// 弹键盘输 SSID/密码时，盖在 WiFi 页列表上（0~620 区域），醒目说明「现在在输什么」。
// 键盘覆盖层只盖 642 以下，弹窗画在上面这块不会被键盘擦掉。
static void draw_wifi_dialog(void) {
    const R dlg = {40, 140, 460, 480};
    box(dlg.x, dlg.y, dlg.w, dlg.h, WHITE);      // 白底盖住 SAVED/NEARBY 列表
    frame(dlg, 2, INK);

    so_font_set_size(g_font, SZ_HERO);
    text(dlg.x + 28, dlg.y + 30, g_ime_mode == 0 ? "连接 WiFi" : "添加 WiFi", INK, 1);

    so_font_set_size(g_font, SZ_SUB);
    text(dlg.x + 28, dlg.y + 86, "// 在下方键盘输入", GRAY, 1);

    char b[72];
    if (g_ime_mode == 1) {
        so_font_set_size(g_font, SZ_H + 6);
        text(dlg.x + 28, dlg.y + 150, "第 1 步 · 网络名称 (SSID)", INK, 1);
        so_font_set_size(g_font, SZ_CMT);
        text(dlg.x + 28, dlg.y + 330, "· 输入后自动进入第 2 步（密码）", GRAYD, 1);
    } else if (g_ime_mode == 2) {
        so_font_set_size(g_font, SZ_H + 6);
        text(dlg.x + 28, dlg.y + 150, "第 2 步 · 密码", INK, 1);
        so_snprintf(b, sizeof(b), "连接到：%s", g_add_ssid);
        so_font_set_size(g_font, SZ_VAL);
        text(dlg.x + 28, dlg.y + 210, b, INK, 1);
        so_font_set_size(g_font, SZ_CMT);
        text(dlg.x + 28, dlg.y + 330, "· 密码留空 = 开放网络（无密码）", GRAYD, 1);
    } else {   // g_ime_mode == 0：给已点的新热点输密码
        so_snprintf(b, sizeof(b), "连接：%s", g_conn_ssid);
        so_font_set_size(g_font, SZ_H + 6);
        text(dlg.x + 28, dlg.y + 150, b, INK, 1);
        so_font_set_size(g_font, SZ_VAL);
        text(dlg.x + 28, dlg.y + 210, "输入密码", INK, 1);
        so_font_set_size(g_font, SZ_CMT);
        text(dlg.x + 28, dlg.y + 330, "· 密码留空 = 开放网络（无密码）", GRAYD, 1);
    }

    so_font_set_size(g_font, SZ_CMT);
    text(dlg.x + 28, dlg.y + 372, "· 输入内容在下方编辑条，OK 键确认", GRAYD, 1);

    push(dlg, "wifi_dlg");
}

enum KEY { K_BW, K_SIZE, K_IDLE, K_WAVE, K_SNDVOL };
struct Seg { R r; int key; int val; const char* label; };
static Seg g_seg[8]; static int g_nseg = 0;
enum TKEY { T_AP, T_WEB, T_WIFI, T_SS, T_DP, T_WAVEDEF, T_SND };
struct Tog { R r; int key; };
static Tog g_tog[4]; static int g_ntog = 0;
struct Li  { R r; int id; };
static Li  g_li[8]; static int g_nli = 0;
enum ACT { ACT_NONE, ACT_ADDWIFI, ACT_UP, ACT_DN, ACT_IMAGE, ACT_FACE, ACT_SNDFILE, ACT_SNDTEST };
struct Act { R r; int act; };
static Act g_act[3]; static int g_nact = 0;
static R g_frz;
static R g_commit = {PADX, 840, SCR_W - 2 * PADX, 80};

static int cfg_i(const char* k, int def) { return so_fwcfg_get_int(k, def); }

static char g_fonts[8][80];   // 字体页列表（局部刷新帮手要用，提前声明）
static int  g_nfonts = 0;

// ── 局部刷新帮手（改设置后只擦写变的那小块）──
static int tog_state(int tkey) {
    switch (tkey) {
        case T_AP: return so_net_ap_on();
        case T_WEB: return so_net_web_on();
        case T_WIFI: return cfg_i("wifi", 0);
        case T_SS: return cfg_i("ss", 0);
        case T_DP: return cfg_i("dp", 0);
        case T_SND: return cfg_i("snd", 1);          // ★ 漏了这一条 → 重画时读成 wave_def，开关看着永远不动
        default: return cfg_i("wave_def", 0);
    }
}
static void redraw_tog(int idx) {
    const R& r = g_tog[idx].r;
    const R e = {r.x - 1, r.y - 1, r.w + 2, r.h + 2};
    so_set_epd_mode(MF); box(e, L14);
    so_set_epd_mode(MT); draw_toggle(r, tog_state(g_tog[idx].key));
    push(e, "开关");
}
static int seg_sel(const Seg& sg) {
    switch (sg.key) {
        case K_BW: return cfg_i("bw", 3) == sg.val;
        case K_SIZE: return cfg_i("size", 28) == sg.val;
        case K_SNDVOL: return cfg_i("snd_vol", 60) == sg.val;
        case K_IDLE: return cfg_i("idle", 5) == sg.val;
        default: return cfg_i("wave_def", 0) == 0 && cfg_i("wave_sel", 2) == sg.val;
    }
}
static void redraw_seg_group(int key) {
    int x0 = 9999, y0 = 9999, x1 = 0, y1 = 0, n = 0;
    for (int i = 0; i < g_nseg; ++i) if (g_seg[i].key == key) {
        const R& r = g_seg[i].r;
        if (r.x < x0) x0 = r.x;
        if (r.y < y0) y0 = r.y;
        if (r.x + r.w > x1) x1 = r.x + r.w;
        if (r.y + r.h > y1) y1 = r.y + r.h;
        ++n;
    }
    if (!n) return;
    const R e = {x0 - 2, y0 - 2, x1 - x0 + 4, y1 - y0 + 4};
    so_set_epd_mode(MF); box(e, L14);
    so_set_epd_mode(MT);
    for (int i = 0; i < g_nseg; ++i) if (g_seg[i].key == key)
        draw_seg(g_seg[i].r, g_seg[i].label, seg_sel(g_seg[i]));
    push(e, "段式组");
}
static void log_push(int y, const char* t, uint8_t ink) {
    char b[96];
    so_snprintf(b, sizeof(b), "> %s", t);
    so_font_set_size(g_font, SZ_LOG);
    const R r = ink_rect(PADX, y, b, SZ_LOG, 1);
    if (r.w <= 0) return;
    so_set_epd_mode(MF); box(r, L14);
    so_set_epd_mode(MT); text(PADX, y, b, ink, 1);
    push(r, "日志行");
}
static void redraw_frz() {
    so_set_epd_mode(MF); box(g_frz, L14);
    so_set_epd_mode(MT);
    frame(g_frz, 2, INK);
    so_font_set_size(g_font, SZ_VAL);
    text(g_frz.x + (g_frz.w - text_w("[ FACTORY RESET ]", SZ_VAL, 2)) / 2,
         g_frz.y + (g_frz.h - m_lh(SZ_VAL)) / 2, "[ FACTORY RESET ]", INK, 2);
    push(g_frz, "FACTORY框");
}

// ══════════════════════════════════════════════════════════════
// 菜单页
// ══════════════════════════════════════════════════════════════
static const char* MENU_KEY[6] = { "01 network", "02 display", "03 font", "04 sound", "05 power", "06 about" };

static void menu_values(char v[6][32]) {
    const int nst = so_net_state();
    if (nst == 2) scopy(v[0], "WIFI OK", 32);
    else if (so_net_ap_on()) scopy(v[0], "AP ON", 32);
    else scopy(v[0], "IDLE", 32);
    so_snprintf(v[1], 32, "border %dpx", cfg_i("bw", 3));
    char f[64] = ""; so_fwcfg_get_str("font", f, sizeof(f));
    so_snprintf(v[2], 32, "%dPX · %s", cfg_i("size", 28), f[0] ? "SD" : "BUILTIN");
    so_snprintf(v[3], 32, "%s · %d", cfg_i("snd", 1) ? "ON" : "OFF", cfg_i("snd_vol", 60));
    so_snprintf(v[4], 32, "%d MIN", cfg_i("idle", 5));
    so_snprintf(v[5], 32, "API %d.%d", SO_API_VERSION_MAJOR, SO_API_VERSION_MINOR);
}

static void draw_menu(void) {
    bar_static(1);
    draw_hero("CONFIG", "// 6 个模块 · 点一行进入");
    char v[6][32]; menu_values(v);
    for (int i = 0; i < 6; ++i) {
        const R r = {PADX, 190 + i * 56, SCR_W - 2 * PADX, 52};
        so_font_set_size(g_font, SZ_VAL);
        text(r.x, r.y + 14, MENU_KEY[i], INK, 1);
        char val[40];
        so_snprintf(val, sizeof(val), "%s  >", v[i]);
        so_font_set_size(g_font, SZ_VAL);
        const int vx0 = r.x + r.w - text_w(val, SZ_VAL, 1);
        text(vx0, r.y + 14, val, INK, 1);
        so_font_set_size(g_font, SZ_KEY);
        dot_leader(r.x + text_w(MENU_KEY[i], SZ_VAL, 1) + 10, vx0 - 10, r.y + 14, SZ_KEY);
        dashed_h(r.x, r.y + r.h - 1, r.w, GLINE);
    }
    g_frz = (R){PADX, 560, SCR_W - 2 * PADX, 56};
    frame(g_frz, 2, INK);
    so_font_set_size(g_font, SZ_VAL);
    text(g_frz.x + (g_frz.w - text_w("[ FACTORY RESET ]", SZ_VAL, 2)) / 2,
         g_frz.y + (g_frz.h - m_lh(SZ_VAL)) / 2, "[ FACTORY RESET ]", INK, 2);
    draw_cmt(640, "双击执行 · 清 NVS + 回内置字体");
    draw_log(700, g_dirty ? "未 COMMIT · 改动只在内存" : "改动只在内存，COMMIT 才写 NVS", g_dirty ? INK : GRAY);
    draw_log(732, "AWAITING COMMAND", GRAY);
    draw_commit(0);
}

// ══════════════════════════════════════════════════════════════
// 网络页
// ══════════════════════════════════════════════════════════════
// 已存热点行（无信号，点直接连）
static void draw_saved_row(const R& r, const char* ssid) {
    so_font_set_size(g_font, SZ_VAL);
    const int ty = r.y + (r.h - m_lh(SZ_VAL)) / 2;
    text_fit(r.x, ty, ssid, INK, 1, SZ_VAL, r.x + r.w - 26);
    text(r.x + r.w - 22, ty, ">", INK, 1);
    dashed_h(r.x, r.y + r.h - 1, r.w, GLINE);
}
// 附近热点行（带信号；已存标「已存」）
static void draw_nearby_row(const R& r, const char* ssid, int8_t rssi, int saved) {
    so_font_set_size(g_font, SZ_VAL);
    const int ty = r.y + (r.h - m_lh(SZ_VAL)) / 2;
    text_fit(r.x, ty, ssid, INK, 1, SZ_VAL, r.x + r.w - 96);
    const char* tag = saved ? "已存" : rssi_txt(rssi);
    so_font_set_size(g_font, SZ_CMT);
    text(r.x + r.w - 34 - text_w(tag, SZ_CMT, 1), r.y + (r.h - m_lh(SZ_CMT)) / 2, tag, GRAYD, 1);
    text(r.x + r.w - 22, ty, ">", INK, 1);
    dashed_h(r.x, r.y + r.h - 1, r.w, GLINE);
}
static void draw_nap_head(void) {
    so_font_set_size(g_font, SZ_H);
    text(PADX, 370, "// NEARBY", GRAY, 2);
    g_up_r = (R){SCR_W - PADX - 92, 366, 44, 28};
    g_dn_r = (R){SCR_W - PADX - 44, 366, 44, 28};
    frame(g_up_r, 1, INK); frame(g_dn_r, 1, INK);
    so_font_set_size(g_font, SZ_SEG);
    text(g_up_r.x + (g_up_r.w - text_w("▲", SZ_SEG, 1)) / 2, g_up_r.y + (g_up_r.h - m_lh(SZ_SEG)) / 2, "▲", INK, 1);
    text(g_dn_r.x + (g_dn_r.w - text_w("▼", SZ_SEG, 1)) / 2, g_dn_r.y + (g_dn_r.h - m_lh(SZ_SEG)) / 2, "▼", INK, 1);
    if (g_nap > NAP_SHOW) {
        char c[16];
        const int end = (g_nap_off + NAP_SHOW) < g_nap ? (g_nap_off + NAP_SHOW) : g_nap;
        so_snprintf(c, sizeof(c), "%d-%d/%d", g_nap_off + 1, end, g_nap);
        so_font_set_size(g_font, SZ_CMT);
        text(g_up_r.x - 8 - text_w(c, SZ_CMT, 1), g_up_r.y + (g_up_r.h - m_lh(SZ_CMT)) / 2, c, GRAYD, 1);
    }
}
static void draw_nap_list(void) {
    g_nnap = 0;
    const int end = (g_nap_off + NAP_SHOW) < g_nap ? (g_nap_off + NAP_SHOW) : g_nap;
    for (int i = g_nap_off; i < end; ++i) {
        const R r = {PADX, 394 + (i - g_nap_off) * 38, SCR_W - 2 * PADX, 34};
        draw_nearby_row(r, g_aps[i], g_aps_rssi[i], ap_saved(g_aps[i]));
        g_nap_r[g_nnap] = r;
        g_nap_id[g_nnap] = i;   // id = 全局索引（含偏移）
        ++g_nnap;
    }
    if (g_nap == 0) draw_cmt(394, "没扫到附近热点");
}
// 局部刷附近热点区（scanning=1 → 「扫描中…」；0 → 画列表）
static void redraw_nap_area(int scanning) {
    const R r = {PADX, 364, SCR_W - 2 * PADX, 220};
    so_set_epd_mode(MF); box(r, L14);
    so_set_epd_mode(MT);
    draw_nap_head();
    if (scanning) { draw_cmt(394, "扫描中…"); }
    else          { draw_nap_list(); }
    push(r, scanning ? "扫描中" : "扫描结果");
}

static void draw_net(void) {
    bar_static(0);
    draw_hero("CONFIG / NETWORK", "// 热点 · 文件服务 · 连接");
    g_ntog = 0;
    g_tog[g_ntog++] = (Tog){draw_togglerow(190, "DEVICE-AP", so_net_ap_on()), T_AP};
    draw_cmt(236, "开着手机才能连上来");
    g_tog[g_ntog++] = (Tog){draw_togglerow(278, "FILE-SERVER", so_net_web_on()), T_WEB};
    draw_cmt(324, "网页上传/下载 (http)");
    g_tog[g_ntog++] = (Tog){draw_togglerow(366, "AUTO-WIFI", cfg_i("wifi", 0)), T_WIFI};
    draw_cmt(412, "开机自动连上次连过的热点");

    draw_h(470, "// WIFI");
    g_nli = 0;
    const R rw = {PADX, 498, SCR_W - 2 * PADX, 48};
    draw_li(rw, "WIFI 列表", "→", 0, 0);
    g_li[g_nli++] = (Li){rw, 0};

    char sip[80], ip[40] = "-";
    so_net_ip(ip, sizeof(ip));
    if (!ip[0]) scopy(ip, "-", sizeof(ip));
    const int ns = so_net_state();
    const char* st = ns == 0 ? "IDLE" : ns == 1 ? "CONNECTING" : ns == 2 ? "CONNECTED" : "FAILED";
    so_snprintf(sip, sizeof(sip), "%s %s", st, ip);
    draw_kv(600, "STATE", sip);

    draw_log(680, "点 WIFI 列表 进扫描连接页", GRAY);
    draw_commit(0);
}

// ══════════════════════════════════════════════════════════════
// WiFi 页（独立：已存 + 附近带信号 + 添加新热点）
// ══════════════════════════════════════════════════════════════
static void draw_wifi(void) {
    bar_static(0);
    draw_hero("CONFIG / WIFI", "// 已存热点 · 附近热点 · 点名字连接");
    // 已存
    draw_h(184, "// SAVED");
    g_nsaved = so_wifi_saved((char*)g_saved, 33, 8);
    g_nli = 0;
    for (int i = 0; i < g_nsaved && i < 4; ++i) {
        const R r = {PADX, 208 + i * 38, SCR_W - 2 * PADX, 34};
        draw_saved_row(r, g_saved[i]);
        g_li[g_nli++] = (Li){r, i};   // id = 已存索引
    }
    if (g_nsaved == 0) draw_cmt(208, "(空 · 点下方添加)");
    // 附近（扫描，异步）
    draw_nap_head();
    if (g_wifi_scan) {
        draw_cmt(394, "扫描中…");
        g_nap = 0; g_nnap = 0;
    } else {
        draw_nap_list();
    }
    // 滚动按钮 + 添加按钮
    g_nact = 0;
    g_act[g_nact++] = (Act){g_up_r, ACT_UP};
    g_act[g_nact++] = (Act){g_dn_r, ACT_DN};
    const R btn = {PADX, 600, SCR_W - 2 * PADX, 52};
    g_act[g_nact++] = (Act){btn, ACT_ADDWIFI};
    frame(btn, 1, INK);
    so_font_set_size(g_font, SZ_VAL);
    text(btn.x + (btn.w - text_w("+ 添加新 WiFi", SZ_VAL, 2)) / 2,
         btn.y + (btn.h - m_lh(SZ_VAL)) / 2, "+ 添加新 WiFi", INK, 2);
    // 状态
    char sip[80], ip[40] = "-";
    so_net_ip(ip, sizeof(ip));
    if (!ip[0]) scopy(ip, "-", sizeof(ip));
    const int ns = so_net_state();
    const char* st = ns == 0 ? "IDLE" : ns == 1 ? "CONNECTING" : ns == 2 ? "CONNECTED" : "FAILED";
    so_snprintf(sip, sizeof(sip), "%s %s", st, ip);
    draw_kv(686, "STATE", sip);
}

// ══════════════════════════════════════════════════════════════
// 显示页（波形 · 描边）
// ══════════════════════════════════════════════════════════════
static const char* WN[5] = { "gc13", "text12", "fast", "fastest", "bw4" };
static const char* BWNAME[3] = { "2PX", "3PX", "4PX" };

static void draw_disp(void) {
    bar_static(0);
    draw_hero("CONFIG / DISPLAY", "// 波形 · 描边");
    g_nseg = 0; g_ntog = 0;

    draw_h(184, "// WAVEFORM");
    g_tog[g_ntog++] = (Tog){draw_togglerow(206, "AUTO", cfg_i("wave_def", 0) != 0), T_WAVEDEF};

    for (int i = 0; i < 5; ++i) {
        const int row = i / 3, col = i % 3;
        const int w = (row == 0) ? 160 : 243;
        const int x = PADX + col * (w + 6);
        const int y = (row == 0) ? 268 : 322;        // 上行 268..320，下行 322..374（别叠）
        const R r = {x, y, w, 52};
        draw_seg(r, WN[i], cfg_i("wave_def", 0) == 0 && cfg_i("wave_sel", 2) == i + 1);
        g_seg[g_nseg++] = (Seg){r, K_WAVE, i + 1, WN[i]};
    }
    draw_cmt(388, "ON = 自动选档 · OFF = 强制用下面选中的档");

    draw_h(430, "// BORDER");
    for (int i = 0; i < 3; ++i) {
        const R r = {PADX + i * (160 + 6), 458, 160, 52};
        draw_seg(r, BWNAME[i], cfg_i("bw", 3) == (i + 2));
        g_seg[g_nseg++] = (Seg){r, K_BW, i + 2, BWNAME[i]};
    }
    draw_cmt(640, "描边作用于所有带框部件");
    draw_cmt(668, "波形改档要重启才换表");
    char b[64];
    so_snprintf(b, sizeof(b), "border = %dpx", cfg_i("bw", 3));
    draw_log(760, b, INK);
    so_snprintf(b, sizeof(b), "wave_def = %d (%s)", cfg_i("wave_def", 0), cfg_i("wave_def", 0) ? "auto" : "forced");
    draw_log(792, b, GRAY);
    draw_commit(0);
}

// ══════════════════════════════════════════════════════════════
// 字体页（合并：全局基准字号 + 字体文件入口 + app 分类字号微调）
// ══════════════════════════════════════════════════════════════
static void szrow_draw(int idx);   // 前向声明：定义在下方「分类字号」段，本页要用

static void draw_font(void) {
    bar_static(0);
    draw_hero("CONFIG / FONT", "// 基准字号 · 字体文件 · 分类微调");
    g_nseg = 0; g_nli = 0; g_nact = 0;

    // ── 全局正文基准（写 NVS size）──
    draw_h(168, "// BASE (全局正文)");
    const char* SN[3] = { "24", "28", "34" };
    const int SZV[3] = { 24, 28, 34 };
    for (int i = 0; i < 3; ++i) {
        const R r = {PADX + i * (160 + 6), 194, 160, 44};
        draw_seg(r, SN[i], cfg_i("size", 28) == SZV[i]);
        g_seg[g_nseg++] = (Seg){r, K_SIZE, SZV[i], SN[i]};
    }

    // ── 字体文件（收起一行，点进选择页）──
    draw_h(250, "// FONT FACE");
    char cur[96] = "";
    so_fwcfg_get_str("font", cur, sizeof(cur));
    const R rf = {PADX, 276, SCR_W - 2 * PADX, 44};
    frame(rf, 1, INK);
    so_font_set_size(g_font, SZ_VAL);
    const int fty = rf.y + (rf.h - m_lh(SZ_VAL)) / 2;
    const char* fnm = "builtin";
    if (cur[0]) { fnm = cur; for (const char* p = cur; *p; ++p) if (*p == '/') fnm = p + 1; }
    text_fit(rf.x + 14, fty, fnm, INK, 1, SZ_VAL, rf.x + rf.w - 80);
    so_font_set_size(g_font, SZ_CMT);
    text(rf.x + rf.w - 56, fty + 4, cur[0] ? "SD" : "内置", GRAYD, 1);
    so_font_set_size(g_font, SZ_VAL);
    text(rf.x + rf.w - 24, fty, ">", INK, 1);
    g_act[g_nact++] = (Act){rf, ACT_FACE};

    // ── 设置 app 自身的分类字号（10 行 [-] [+]）──
    draw_h(332, "// APP 分类字号");
    for (int i = 0; i < SZ_N; ++i) szrow_draw(i);

    draw_cmt(748, "tap [-] / [+] 调字号 · 改动即存");
    draw_log(774, "各页即时生效 · FONT FACE 换字体文件", GRAY);
    draw_commit(0);
}

// 字体文件选择页（PG_FACE）
static void draw_face(void) {
    bar_static(0);
    draw_hero("CONFIG / FONT FACE", "// /sdcard/fonts · 选中即加载");
    g_nli = 0;
    g_nfonts = so_list_fonts(g_fonts, 8);
    if (g_nfonts < 0) g_nfonts = 0;
    char cur[96] = "";
    so_fwcfg_get_str("font", cur, sizeof(cur));

    const R r0 = {PADX, 190, SCR_W - 2 * PADX, 52};
    draw_li(r0, "builtin", "出厂默认", cur[0] == 0, 0);
    g_li[g_nli++] = (Li){r0, -1};
    for (int i = 0; i < g_nfonts && i < 8; ++i) {
        const R r = {PADX, 190 + (i + 1) * 58, SCR_W - 2 * PADX, 52};
        char rel[96];
        so_snprintf(rel, sizeof(rel), "fonts/%s", g_fonts[i]);
        const int sel = (cur[0] != 0 && streq(cur, rel));
        draw_li(r, g_fonts[i], nullptr, sel, 0);
        g_li[g_nli++] = (Li){r, i};
    }
    for (int i = g_nfonts + 1; i <= 8; ++i) {
        const R r = {PADX, 190 + i * 58, SCR_W - 2 * PADX, 52};
        draw_li(r, nullptr, nullptr, 0, 1);
    }
    draw_log(700, g_msg[0] ? g_msg : "选中即加载 · 失败保留原字体", g_msg[0] ? INK : GRAY);
    draw_log(732, "字体整份读进 PSRAM 常驻", GRAY);
    draw_commit(0);
}

// ══════════════════════════════════════════════════════════════
// 电源页
// ══════════════════════════════════════════════════════════════
static void draw_pwr(void) {
    bar_static(0);
    draw_hero("CONFIG / POWER", "// 屏保 · 休眠 · 熄屏");
    g_ntog = 0; g_nseg = 0; g_nact = 0;

    g_tog[g_ntog++] = (Tog){draw_togglerow(190, "SCREENSAVER", cfg_i("ss", 0)), T_SS};
    draw_cmt(236, "久不动熄屏");
    g_tog[g_ntog++] = (Tog){draw_togglerow(284, "DEEP-SLEEP", cfg_i("dp", 0)), T_DP};
    draw_cmt(330, "息屏断 WiFi 省电");

    draw_h(390, "// IDLE (MIN)");
    const char* IV[3] = { "1", "5", "10" };
    const int IVV[3] = { 1, 5, 10 };
    for (int i = 0; i < 3; ++i) {
        const R r = {PADX + i * (160 + 6), 418, 160, 56};
        draw_seg(r, IV[i], cfg_i("idle", 5) == IVV[i]);
        g_seg[g_nseg++] = (Seg){r, K_IDLE, IVV[i], IV[i]};
    }

    draw_h(504, "// IMAGE");
    const R img = {PADX, 532, SCR_W - 2 * PADX, 52};
    {
        char im[64] = "";
        so_fwcfg_get_str("scr_img", im, sizeof(im));
        if (!im[0]) scopy(im, "black (no image)", sizeof(im));
        draw_li(img, im, "→", 0, 0);
    }
    g_act[g_nact++] = (Act){img, ACT_IMAGE};

    draw_warn(628, "[ 恢复出厂：去主页双击 FACTORY RESET ]", 17);
    char b[64];
    so_snprintf(b, sizeof(b), "ss = %d  dp = %d  idle = %d", cfg_i("ss", 0), cfg_i("dp", 0), cfg_i("idle", 5));
    draw_log(720, b, INK);
    draw_commit(0);
}

// ══════════════════════════════════════════════════════════════
// 屏保页
// ══════════════════════════════════════════════════════════════
static char g_scrs[9][64];
static int  g_nscrs = 0;

static int is_img(const char* nm) {
    const char* d = nullptr;
    for (const char* p = nm; *p; ++p) if (*p == '.') d = p;
    if (!d) return 0;
    char ext[8]; int n = 0;
    for (const char* e = d + 1; *e && n < 7; ++e)
        ext[n++] = (*e >= 'A' && *e <= 'Z') ? (char)(*e + 32) : *e;
    ext[n] = 0;
    return streq(ext, "png") || streq(ext, "bmp") || streq(ext, "jpg") || streq(ext, "jpeg");
}

static void draw_saver(void) {
    bar_static(0);
    draw_hero("CONFIG / SAVER", "// 熄屏显示哪张图");
    g_nli = 0;

    char all[9][64];
    const int total = so_list_files("/sdcard/screensaver", nullptr, (char*)all, 64, 9);
    g_nscrs = 0;
    for (int i = 0; i < total && g_nscrs < 8; ++i)
        if (is_img(all[i])) scopy(g_scrs[g_nscrs++], all[i], 64);

    char cur[64] = "";
    so_fwcfg_get_str("scr_img", cur, sizeof(cur));

    const R r0 = {PADX, 184, SCR_W - 2 * PADX, 52};
    draw_li(r0, "black (no image)", nullptr, cur[0] == 0, 0);
    g_li[g_nli++] = (Li){r0, 0};
    for (int i = 0; i < g_nscrs && i < 6; ++i) {
        const R r = {PADX, 184 + (i + 1) * 60, SCR_W - 2 * PADX, 52};
        draw_li(r, g_scrs[i], nullptr, (cur[0] != 0 && streq(cur, g_scrs[i])), 0);
        g_li[g_nli++] = (Li){r, i + 1};
    }
    for (int i = g_nscrs + 1; i <= 6; ++i) {
        const R r = {PADX, 184 + i * 60, SCR_W - 2 * PADX, 52};
        draw_li(r, nullptr, nullptr, 0, 1);
    }
    draw_cmt(630, "16 级灰 PNG 放进 SD 的 screensaver/");
    draw_cmt(658, "tools/make_screensaver.py 可生成");
    draw_log(720, "点一下 = 选中并预览 2 秒", INK);
    draw_log(752, "改完回主页 COMMIT", GRAY);
    draw_commit(0);
}

// ══════════════════════════════════════════════════════════════
// 关于页
// ══════════════════════════════════════════════════════════════
static void draw_about(void) {
    bar_static(0);
    draw_hero("CONFIG / ABOUT", "// 固件 / 分区 / 实体");

    char sram[40], psram[40], cpu[16], font[40];
    {
        const int st = so_mem_size_kb(0), sf = so_mem_free_kb(0);
        const int pt = so_mem_size_kb(1), pf = so_mem_free_kb(1);
        so_snprintf(sram, sizeof(sram), st > 0 ? "%dK/%dK" : "--", st, st - sf);
        so_snprintf(psram, sizeof(psram), pt > 0 ? "%dK/%dK" : "--", pt, pt - pf);
    }
    const int c = so_cpu_load();
    so_snprintf(cpu, sizeof(cpu), c >= 0 ? "%d%%" : "--", c);
    char f[64] = ""; so_fwcfg_get_str("font", f, sizeof(f));
    so_snprintf(font, sizeof(font), "%s · %dPX", f[0] ? "SD" : "BUILTIN", cfg_i("size", 28));

    const char* KEYS[7] = { "FIRMWARE", "FONT", "RESOLUTION", "PANEL", "SRAM", "PSRAM", "CPU" };
    const char* VALS[7] = { "m5ui 1.0.0", font, "540 x 960", "8BIT EPD · 16 GRAY", sram, psram, cpu };
    for (int i = 0; i < 7; ++i) draw_kv(196 + i * 52, KEYS[i], VALS[i]);

    draw_h(590, "// BUILD");
    draw_log(618, "idf.py build", GRAY);
    draw_log(648, "idf.py -p /dev/ttyACM0 flash", GRAY);
    draw_log(710, "panic = 0", INK);
    draw_log(740, "墨水屏效果必须肉眼看", GRAY);
    draw_warn(820, "[ END OF CONFIG ]", 18);
}

// ══════════════════════════════════════════════════════════════
// 字号微调页（设置 app 自身的字号）
// ══════════════════════════════════════════════════════════════
struct SzRow { R minus, plus; };
static SzRow g_szrow[10];

static void szrow_draw(int idx) {
    const SzCat& c = g_szcat[idx];
    const int y = 358 + idx * 38;
    const int bw = 44, h = 36;
    const R rp = {SCR_W - PADX - bw * 2 - 10, y, bw, h};   // [-]
    const R rm = {SCR_W - PADX - bw, y, bw, h};            // [+]
    frame(rp, 1, INK);
    so_font_set_size(g_font, SZ_UI);
    text(rp.x + (rp.w - text_w("-", SZ_UI, 1)) / 2, rp.y + (h - m_lh(SZ_UI)) / 2, "-", INK, 1);
    frame(rm, 1, INK);
    text(rm.x + (rm.w - text_w("+", SZ_UI, 1)) / 2, rm.y + (h - m_lh(SZ_UI)) / 2, "+", INK, 1);
    char b[16];
    so_snprintf(b, sizeof(b), "%dpx", *c.v);
    so_font_set_size(g_font, SZ_UI);
    const int vx = rp.x - 12 - text_w(b, SZ_UI, 1);
    text(vx, y + (h - m_lh(SZ_UI)) / 2, b, INK, 1);
    // 分类名：用它**自己的字号**画 = 活样例（改了当场变大变小）
    // ✱ 但要夹在行高里：56px 的名字画进 36px 的行会压到上下两行（重叠）
    const int spx = (*c.v > h - 8) ? (h - 8) : (*c.v);
    so_font_set_size(g_font, (float)spx);
    text(PADX, y + (h - m_lh(spx)) / 2, c.cn, GRAY, 1);
    int lx = PADX + text_w(c.cn, spx, 1) + 8;
    // 用法提示（固定小字，说明它管的是哪块）
    if (c.hint[0]) {
        so_font_set_size(g_font, SZ_UI);
        text(lx, y + (h - m_lh(SZ_UI)) / 2, c.hint, GRAYD, 1);
        lx += text_w(c.hint, SZ_UI, 1) + 10;
    }
    dot_leader(lx, vx - 10, y + (h - m_lh(SZ_UI)) / 2, SZ_UI);
    g_szrow[idx].minus = rp;
    g_szrow[idx].plus = rm;
}
static void redraw_szrow(int idx) {
    const int y = 358 + idx * 38;
    so_set_epd_mode(MF); box(PADX, y, SCR_W - 2 * PADX, 36, L14);
    so_set_epd_mode(MT);
    szrow_draw(idx);
    push((R){PADX, y, SCR_W - 2 * PADX, 36}, "字号行");
}
static int sz_hit(int x, int y) {
    for (int i = 0; i < SZ_N; ++i) {
        const SzRow& r = g_szrow[i];
        if (x >= r.minus.x && x < r.minus.x + r.minus.w && y >= r.minus.y && y < r.minus.y + r.minus.h) return i * 2;
        if (x >= r.plus.x  && x < r.plus.x  + r.plus.w  && y >= r.plus.y  && y < r.plus.y  + r.plus.h)  return i * 2 + 1;
    }
    return -1;
}

// ══════════════════════════════════════════════════════════════
// 声音页（触摸反馈音）
// ══════════════════════════════════════════════════════════════
static char g_snd_line[96];
static char g_wavs[10][64];
static int  g_nwavs = 0;

static void draw_snd(void) {
    bar_static(0);
    draw_hero("CONFIG / SOUND", "// 触摸反馈音 · 音量 · 音效文件");
    g_ntog = 0; g_nseg = 0; g_nact = 0; g_nli = 0;

    draw_h(184, "// TOUCH");
    g_tog[g_ntog++] = (Tog){draw_togglerow(210, "FEEDBACK", cfg_i("snd", 1)), T_SND};
    draw_cmt(266, "按下屏幕时响一声（固件统一在触摸里触发）");

    draw_h(300, "// VOLUME");
    const char* VN[3] = { "30", "60", "90" };
    const int VV[3] = { 30, 60, 90 };
    for (int i = 0; i < 3; ++i) {
        const R r = {PADX + i * (160 + 6), 334, 160, 46};      // 标题 300..326，段式 334 起（别压标题）
        draw_seg(r, VN[i], cfg_i("snd_vol", 60) == VV[i]);
        g_seg[g_nseg++] = (Seg){r, K_SNDVOL, VV[i], VN[i]};
    }
    draw_cmt(392, "点一下 = 选中并试听新音量");

    draw_h(430, "// SOUND FILE");
    {
        char sf[64] = "";
        so_fwcfg_get_str("snd_file", sf, sizeof(sf));
        const char* nm = "builtin beep";
        if (sf[0]) nm = strrchr(sf, '/') ? strrchr(sf, '/') + 1 : sf;
        const R r = {PADX, 464, SCR_W - 2 * PADX, 48};
        draw_li(r, nm, sf[0] ? "WAV" : "默认", 0, 0);
        g_act[g_nact++] = (Act){r, ACT_SNDFILE};
    }
    draw_cmt(520, "点开选 /sdcard/sounds/*.wav · 空 = 内置短哔");

    {
        const R r = {PADX, 560, SCR_W - 2 * PADX, 52};
        frame(r, 2, INK);
        so_font_set_size(g_font, SZ_BTN);
        text(r.x + (r.w - text_w("[ TEST ]", SZ_BTN, 2)) / 2,
             r.y + (r.h - m_lh(SZ_BTN)) / 2, "[ TEST ]", INK, 2);
        g_act[g_nact++] = (Act){r, ACT_SNDTEST};
    }

    so_snprintf(g_snd_line, sizeof(g_snd_line), "snd = %d  vol = %d",
                cfg_i("snd", 1), cfg_i("snd_vol", 60));
    draw_log(650, g_snd_line, GRAY);
    draw_log(682, "音效文件整份读进 PSRAM，按下即播", GRAY);
    draw_commit(0);
}

// 音效文件选择页（PG_SNDFILE）
static void draw_sndfile(void) {
    bar_static(0);
    draw_hero("CONFIG / SOUND FILE", "// /sdcard/sounds/*.wav");
    g_nli = 0;
    g_nwavs = so_list_files("/sdcard/sounds", ".wav", (char*)g_wavs, 64, 8);
    if (g_nwavs < 0) g_nwavs = 0;
    char cur[64] = "";
    so_fwcfg_get_str("snd_file", cur, sizeof(cur));

    const R r0 = {PADX, 190, SCR_W - 2 * PADX, 52};
    draw_li(r0, "builtin beep", "默认", cur[0] == 0, 0);
    g_li[g_nli++] = (Li){r0, -1};
    for (int i = 0; i < g_nwavs && i < 7; ++i) {
        const R r = {PADX, 190 + (i + 1) * 58, SCR_W - 2 * PADX, 52};
        char rel[96];
        so_snprintf(rel, sizeof(rel), "sounds/%s", g_wavs[i]);
        draw_li(r, g_wavs[i], nullptr, cur[0] && streq(cur, rel), 0);
        g_li[g_nli++] = (Li){r, i};
    }
    for (int i = g_nwavs + 1; i <= 7; ++i) {
        const R r = {PADX, 190 + i * 58, SCR_W - 2 * PADX, 52};
        draw_li(r, nullptr, nullptr, 0, 1);
    }
    so_snprintf(g_snd_line, sizeof(g_snd_line), "扫描到 %d 个 .wav", g_nwavs);
    draw_log(680, g_snd_line, INK);
    draw_log(712, "选中即试听 · BACK 返回声音页", GRAY);
    draw_commit(0);
}

// ══════════════════════════════════════════════════════════════
// 整页
// ══════════════════════════════════════════════════════════════
static void page_draw(void) {
    so_set_epd_mode(MF);
    box((R){0, 0, SCR_W, SCR_H}, L14);
    so_set_epd_mode(MT);
    g_ntog = 0; g_nseg = 0; g_nli = 0; g_nact = 0; g_nnap = 0;   // ★ 清掉上一页的交互区，别让旧页的开关/按钮还吃点击
    switch (g_pg) {
        case PG_MENU:  draw_menu();  break;
        case PG_NET:   draw_net();   break;
        case PG_DISP:  draw_disp();  break;
        case PG_FONT:  draw_font();  break;
        case PG_FACE:  draw_face();  break;
        case PG_SND:   draw_snd();   break;
        case PG_SNDFILE: draw_sndfile(); break;
        case PG_PWR:   draw_pwr();   break;
        case PG_WIFI:  draw_wifi();  break;
        case PG_SAVER: draw_saver(); break;
        default:       draw_about(); break;
    }
    so_set_epd_mode(MQ);
    push((R){0, 0, SCR_W, SCR_H}, "切页整屏");
    g_need_full = 0;
}

extern "C" __attribute__((visibility("default")))
uint32_t so_app_api_version(void) { return SO_API_VERSION; }

extern "C" __attribute__((visibility("default")))
const char* so_app_name(void) { return "系统设置"; }

// ── launcher 元信息（.m5meta 节）：图标 + 名称 + 描述 ──
// 固件读 .so 文件时按 ELF 节表取这一段，**不加载、不执行**（见 apps/common/so_meta.h）。
SO_APP_META("系统设置", "网络 · 显示 · 字体 · 电源", 32, 32, 1,
                 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                 0x01, 0xFC, 0x00, 0x00, 0x01, 0xFC, 0x00, 0x00, 0x01, 0xFC, 0x00, 0x00, 0x01, 0xFC, 0x00, 0x00,
                 0x01, 0xFC, 0x00, 0x00, 0x1F, 0xFF, 0xFF, 0xF8, 0x1F, 0xFF, 0xFF, 0xF8, 0x01, 0xFC, 0x00, 0x00,
                 0x01, 0xFC, 0x3F, 0x80, 0x01, 0xFC, 0x3F, 0x80, 0x01, 0xFC, 0x3F, 0x80, 0x01, 0xFC, 0x3F, 0x80,
                 0x00, 0x00, 0x3F, 0x80, 0x1F, 0xFF, 0xFF, 0xF8, 0x1F, 0xFF, 0xFF, 0xF8, 0x00, 0x00, 0x3F, 0x80,
                 0x00, 0x1F, 0xFF, 0x80, 0x00, 0x1F, 0xFF, 0x80, 0x00, 0x1F, 0xFF, 0x80, 0x00, 0x1F, 0xFF, 0x80,
                 0x00, 0x1F, 0xC0, 0x00, 0x1F, 0xFF, 0xFF, 0xF8, 0x1F, 0xFF, 0xFF, 0xF8, 0x00, 0x1F, 0xC0, 0x00,
                 0x00, 0x1F, 0xC0, 0x00, 0x00, 0x1F, 0xC0, 0x00, 0x00, 0x1F, 0xC0, 0x00, 0x00, 0x1F, 0xC0, 0x00,
            );


extern "C" __attribute__((visibility("default")))
void page_init(void) {
    so_log("settings: 进入 app");
    so_log_file("");
    so_log_file("===== settings 新会话 =====");
    g_font = so_font_builtin(13.0f);
    if (!g_font) { so_log("settings: 内置字体加载失败"); return; }
    sz_load();
    g_bat = so_battery_level();
    g_pg = PG_MENU;
    g_need_full = 1;
    page_draw();
    so_log("settings: 首屏完成");
}

static void apply_if_needed(int key) {
    g_dirty = 1;
    // 只有「换字体 / 换波形表」才需要宿主动手；音量别走 apply（它会重载字体，白等 1~2 秒）
    if (key == K_SIZE || key == K_WAVE) so_fwcfg_apply();
    else if (key == K_SNDVOL) so_sound_test();      // 顺手试听一下新音量
}
static int seg_hit(int x, int y) {
    for (int i = 0; i < g_nseg; ++i)
        if (x >= g_seg[i].r.x && x < g_seg[i].r.x + g_seg[i].r.w &&
            y >= g_seg[i].r.y && y < g_seg[i].r.y + g_seg[i].r.h) return i;
    return -1;
}
static int tog_hit(int x, int y) {
    for (int i = 0; i < g_ntog; ++i)
        if (x >= g_tog[i].r.x && x < g_tog[i].r.x + g_tog[i].r.w &&
            y >= g_tog[i].r.y && y < g_tog[i].r.y + g_tog[i].r.h) return i;
    return -1;
}
static int li_hit(int x, int y) {
    for (int i = 0; i < g_nli; ++i)
        if (x >= g_li[i].r.x && x < g_li[i].r.x + g_li[i].r.w &&
            y >= g_li[i].r.y && y < g_li[i].r.y + g_li[i].r.h) return i;
    return -1;
}
static int act_hit(int x, int y) {
    for (int i = 0; i < g_nact; ++i)
        if (x >= g_act[i].r.x && x < g_act[i].r.x + g_act[i].r.w &&
            y >= g_act[i].r.y && y < g_act[i].r.y + g_act[i].r.h) return i;
    return -1;
}

extern "C" __attribute__((visibility("default")))
void page_tick(void) {
    const int now = (int)so_now_ms();

    // 输入法覆盖层开着（正在输 SSID/口令）：键盘触摸归固件，.so 自己的 UI/时钟都暂停
    if (g_ime_field >= 0) return;

    if (g_need_full) page_draw();

    // 连着再开一次输入法（添加新热点：输完 SSID → 输口令）
    if (g_need_ime) {
        g_need_ime = 0;
        if (g_pg == PG_WIFI) draw_wifi_dialog();   // 刷新弹窗：第 1 步 → 第 2 步
        g_ime_field = 0;
        if (so_ime_open("", on_ime_done, on_ime_cancel) != 0) g_ime_field = -1;
    }

    // WiFi 扫描（异步：进 WiFi 页/连接后 设 g_wifi_scan，这里真扫，扫完局部刷列表）
    if (g_pg == PG_WIFI && g_wifi_scan) {
        g_wifi_scan = 0;
        g_nap = so_wifi_scan((char*)g_aps, 33, 8, g_aps_rssi);
        g_nsaved = so_wifi_saved((char*)g_saved, 33, 8);
        g_nap_off = 0;   // 新扫描从头显示
        redraw_nap_area(0);
    }

    // 时钟 5s（局部：只刷那几个数字）
    if (now - g_clock_t >= 5000) {
        g_clock_t = now;
        char b[8]; clock_str(b, sizeof(b));
        if (!streq(b, g_clock)) {
            so_font_set_size(g_font, SZ_BAR);
            const int xo = clock_x(g_clock), xn = clock_x(b);
            R r = ink_rect(xo, (BAR_H - m_lh(SZ_BAR)) / 2, g_clock, SZ_BAR, 2);
            const R rn = ink_rect(xn, (BAR_H - m_lh(SZ_BAR)) / 2, b, SZ_BAR, 2);
            if (rn.w > 0) {
                if (r.w <= 0) r = rn;
                else {
                    const int x0 = r.x < rn.x ? r.x : rn.x;
                    const int x1 = (r.x + r.w) > (rn.x + rn.w) ? (r.x + r.w) : (rn.x + rn.w);
                    r = (R){x0, r.y, x1 - x0, r.h};
                }
            }
            scopy(g_clock, b, 8);
            if (r.w > 0) {
                so_set_epd_mode(MF); box(r, L14);
                so_set_epd_mode(MT);
                so_font_set_size(g_font, SZ_BAR);
                text(xn, (BAR_H - m_lh(SZ_BAR)) / 2, b, GRAY, 2);
                push(r, "时钟");
            }
        }
    }
    // 电量 30s（局部：只刷那几位数字）
    if (now - g_slow_t >= 30000) {
        g_slow_t = now;
        const int lv = so_battery_level();
        if (lv != g_bat) { g_bat = lv; bar_bat_push(); }
    }
    // 双击窗口超时：只清状态
    if (g_dbl_act && now - g_dbl_t > 6000) g_dbl_act = 0;
    // 按下反馈弹起（局部）
    if (g_press_c && now - g_press_c > 180) { draw_commit(0); push(g_commit, "弹起"); g_press_c = 0; }
    if (g_press_f && now - g_press_f > 180) { redraw_frz(); g_press_f = 0; }

    static int down = 0;
    int32_t x = 0, y = 0;
    const int t = so_touch(&x, &y);
    if (t && !down) {
        if (y < BAR_H && x < 200) {
            if (g_pg == PG_MENU) so_exit();
            else if (g_pg == PG_WIFI) { g_pg = PG_NET; g_need_full = 1; }   // WiFi 页返回网络页
            else if (g_pg == PG_SAVER) { g_pg = PG_PWR; g_need_full = 1; }   // 屏保页返回电源页
            else { g_pg = PG_MENU; g_need_full = 1; }
        }
        else if (g_pg == PG_MENU) {
            if (x >= g_commit.x && x < g_commit.x + g_commit.w && y >= g_commit.y && y < g_commit.y + g_commit.h) {
                draw_commit(1); push(g_commit, "按下"); g_press_c = now;
                const int ok = (so_fwcfg_save() == 0);
                g_dirty = ok ? 0 : 1;
                so_log(ok ? "settings: 已写入 NVS" : "settings: 写 NVS 失败");
                char b[64];
                scopy(b, g_dirty ? "未 COMMIT · 改动只在内存" : "改动只在内存，COMMIT 才写 NVS", sizeof(b));
                log_push(700, b, g_dirty ? INK : GRAY);   // 只刷那一行提示
            } else if (x >= g_frz.x && x < g_frz.x + g_frz.w && y >= g_frz.y && y < g_frz.y + g_frz.h) {
                so_set_epd_mode(MT); frame(g_frz, 4, INK);
                push(g_frz, "按下"); g_press_f = now;
                if (g_dbl_act == 1) {
                    g_dbl_act = 0;
                    so_fwcfg_reset();
                    so_log("settings: 已恢复出厂，请重启生效");
                    so_exit();
                } else {
                    g_dbl_act = 1; g_dbl_t = now;
                    so_log("settings: 再点一次 [ FACTORY RESET ] 确认");
                }
            } else {
                static const int MENU_PG[6] = { PG_NET, PG_DISP, PG_FONT, PG_SND, PG_PWR, PG_ABOUT };
                for (int i = 0; i < 6; ++i) {
                    const R r = {PADX, 190 + i * 56, SCR_W - 2 * PADX, 52};
                    if (x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h) {
                        g_pg = MENU_PG[i];
                        g_need_full = 1;
                        break;
                    }
                }
            }
        } else {
            // 合并后的 FONT 页：先处理 10 行分类字号的 [-] / [+]（没点中就走下面的段式/按钮/列表）
            if (g_pg == PG_FONT) {
                const int h2 = sz_hit(x, y);
                if (h2 >= 0) {
                    const int idx = h2 / 2, is_plus = h2 % 2;
                    int* pv = g_szcat[idx].v;
                    *pv += is_plus ? 1 : -1;
                    if (*pv < g_szcat[idx].lo) *pv = g_szcat[idx].lo;
                    if (*pv > g_szcat[idx].hi) *pv = g_szcat[idx].hi;
                    sz_save();
                    redraw_szrow(idx);
                    goto done;
                }
            }
            // 子页：COMMIT → 段式 → 开关 → 动作 → 列表，全部局部刷新
            if (x >= g_commit.x && x < g_commit.x + g_commit.w && y >= g_commit.y && y < g_commit.y + g_commit.h) {
                draw_commit(1); push(g_commit, "按下"); g_press_c = now;
                const int ok = (so_fwcfg_save() == 0);
                g_dirty = ok ? 0 : 1;
                so_log(ok ? "settings: 已写入 NVS" : "settings: 写 NVS 失败");
                goto done;
            }
            {
                const int s = seg_hit(x, y);
                if (s >= 0) {
                    const Seg& sg = g_seg[s];
                    switch (sg.key) {
                        case K_BW:   so_fwcfg_set_int("bw", sg.val); break;
                        case K_SIZE:  so_fwcfg_set_int("size", sg.val); break;
                        case K_IDLE:  so_fwcfg_set_int("idle", sg.val); break;
                        case K_WAVE:  so_fwcfg_set_int("wave_sel", sg.val); so_fwcfg_set_int("wave_def", 0); break;
                        case K_SNDVOL: so_fwcfg_set_int("snd_vol", sg.val); break;
                    }
                    apply_if_needed(sg.key);
                    redraw_seg_group(sg.key);
                    char b[64];
                    if (sg.key == K_BW) {
                        so_snprintf(b, sizeof(b), "border = %dpx", cfg_i("bw", 3));
                        log_push(760, b, INK);
                    } else if (sg.key == K_WAVE) {
                        so_snprintf(b, sizeof(b), "wave_def = %d (%s)", cfg_i("wave_def", 0), cfg_i("wave_def", 0) ? "auto" : "forced");
                        log_push(792, b, GRAY);
                    } else if (sg.key == K_IDLE) {
                        so_snprintf(b, sizeof(b), "ss = %d  dp = %d  idle = %d", cfg_i("ss", 0), cfg_i("dp", 0), cfg_i("idle", 5));
                        log_push(720, b, INK);
                    } else if (sg.key == K_SIZE) {
                        so_snprintf(b, sizeof(b), "base size = %dpx", cfg_i("size", 28));
                        log_push(748, b, INK);
                    }
                    goto done;
                }
            }
            {
                const int tg = tog_hit(x, y);
                if (tg >= 0) {
                    const Tog& t2 = g_tog[tg];
                    const char* key = nullptr;
                    switch (t2.key) {
                        case T_AP: key = "ap"; break;
                        case T_WEB: key = "web"; break;
                        case T_WIFI: key = "wifi"; break;
                        case T_SS: key = "ss"; break;
                        case T_DP: key = "dp"; break;
                        case T_WAVEDEF: key = "wave_def"; break;
                        case T_SND: key = "snd"; break;
                    }
                    if (key) {
                        const int nv = cfg_i(key, 0) ? 0 : 1;
                        so_fwcfg_set_int(key, nv);
                        if (t2.key == T_AP) so_net_set_ap(nv);
                        else if (t2.key == T_WEB) so_net_set_web(nv);
                        g_dirty = 1;
                        if (t2.key == T_WAVEDEF) so_fwcfg_apply();
                        if (t2.key == T_SND && nv) so_sound_test();   // 打开时试听一声
                        redraw_tog(tg);
                        char b[64];
                        if (t2.key == T_WAVEDEF) {
                            so_snprintf(b, sizeof(b), "wave_def = %d (%s)", nv, nv ? "auto" : "forced");
                            log_push(792, b, GRAY);
                        } else if (g_pg == PG_PWR) {
                            so_snprintf(b, sizeof(b), "ss = %d  dp = %d  idle = %d", cfg_i("ss", 0), cfg_i("dp", 0), cfg_i("idle", 5));
                            log_push(720, b, INK);
                        } else if (g_pg == PG_SND) {
                            so_snprintf(b, sizeof(b), "snd = %d  vol = %d", cfg_i("snd", 1), cfg_i("snd_vol", 60));
                            log_push(650, b, INK);
                        }
                    }
                    goto done;
                }
            }
            {
                const int a = act_hit(x, y);
                if (a >= 0) {
                    if (g_act[a].act == ACT_ADDWIFI) {
                        // 添加新 WiFi：先输 SSID，再输口令（空 = 开放网络）
                        g_add_ssid[0] = 0;
                        g_ime_mode = 1;
                        g_ime_field = 0;
                        draw_wifi_dialog();   // 弹窗说明「第 1 步 · SSID」
                        if (so_ime_open("", on_ime_done, on_ime_cancel) != 0) g_ime_field = -1;
                    } else if (g_act[a].act == ACT_UP) {
                        if (g_nap_off > 0) {
                            g_nap_off -= NAP_SHOW;
                            if (g_nap_off < 0) g_nap_off = 0;
                            redraw_nap_area(0);
                        }
                    } else if (g_act[a].act == ACT_DN) {
                        const int mx = g_nap - NAP_SHOW;
                        if (mx > 0 && g_nap_off < mx) {
                            g_nap_off += NAP_SHOW;
                            if (g_nap_off > mx) g_nap_off = mx;
                            redraw_nap_area(0);
                        }
                    } else if (g_act[a].act == ACT_FACE) {
                        g_pg = PG_FACE; g_need_full = 1;
                    } else if (g_act[a].act == ACT_SNDFILE) {
                        g_pg = PG_SNDFILE; g_need_full = 1;
                    } else if (g_act[a].act == ACT_SNDTEST) {
                        so_sound_test();                    // 试听当前反馈音
                    } else if (g_act[a].act == ACT_IMAGE) {
                        g_pg = PG_SAVER; g_need_full = 1;
                    }
                    goto done;
                }
            }
            {
                const int li = li_hit(x, y);
                if (li >= 0) {
                    const Li& l2 = g_li[li];
                    if (g_pg == PG_SNDFILE) {
                        if (l2.id < 0) so_fwcfg_set_str("snd_file", "");
                        else {
                            char rel[96];
                            so_snprintf(rel, sizeof(rel), "sounds/%s", g_wavs[l2.id]);
                            so_fwcfg_set_str("snd_file", rel);
                        }
                        so_fwcfg_save();
                        so_sound_test();                    // 选中即试听（顺带重新加载）
                        g_pg = PG_SND; g_need_full = 1;     // 选完回声音页
                    } else if (g_pg == PG_FACE) {
                        if (l2.id < 0) so_fwcfg_set_str("font", "");
                        else {
                            char rel[96];
                            so_snprintf(rel, sizeof(rel), "fonts/%s", g_fonts[l2.id]);
                            so_fwcfg_set_str("font", rel);
                        }
                        so_fwcfg_save();
                        so_fwcfg_apply();
                        so_snprintf(g_msg, sizeof(g_msg), "%s 已启用（已存）",
                                    l2.id < 0 ? "内置字体" : g_fonts[l2.id]);
                        so_log(g_msg);
                        g_pg = PG_FONT; g_need_full = 1;   // 选完回 FONT 页
                    } else if (g_pg == PG_SAVER) {
                        if (l2.id == 0) so_fwcfg_set_str("scr_img", "");
                        else so_fwcfg_set_str("scr_img", g_scrs[l2.id - 1]);
                        so_fwcfg_save();
                        g_pg = PG_PWR; g_need_full = 1;   // 选完回电源页
                    } else if (g_pg == PG_NET) {
                        // 点 WIFI 列表 → 进独立 WiFi 页
                        g_pg = PG_WIFI;
                        g_wifi_scan = 1;
                        g_need_full = 1;
                    } else if (g_pg == PG_WIFI) {
                        // 点已存热点 → 直接连
                        if (l2.id >= 0 && l2.id < g_nsaved) {
                            so_wifi_connect(g_saved[l2.id], "");
                            g_wifi_scan = 1;
                            g_need_full = 1;
                        }
                    }
                    goto done;
                }
            }
            {
                const int np = nap_hit(x, y);
                if (np >= 0) {
                    const int ai = g_nap_id[np];
                    if (ai >= 0 && ai < g_nap) {
                        if (ap_saved(g_aps[ai])) {
                            so_wifi_connect(g_aps[ai], "");
                            g_wifi_scan = 1;
                            g_need_full = 1;
                        } else {
                            scopy(g_conn_ssid, g_aps[ai], sizeof(g_conn_ssid));
                            g_ime_mode = 0;
                            g_ime_field = 0;
                            draw_wifi_dialog();   // 弹窗说明「输入密码」+ 目标 SSID
                            if (so_ime_open("", on_ime_done, on_ime_cancel) != 0) g_ime_field = -1;
                        }
                    }
                    goto done;
                }
            }
        }
    }
done:
    down = (t != 0);
}
