// apps/reader/main/pages.h — 阅读器的每一屏（终端风 v3）
//
// 屏：书目列表 / 阅读 / 目录 / 阅读设置 / 分章分析（选书·进度·结果）+ 阅读页中央弹窗
// 每屏三件事分开：`show_xxx()` 画并推整屏、`draw_xxx()` 只画、`tick_xxx(x,y)` 只处理一次按下。
// 局部刷新一律走 ui::push(rect, why)（擦 = MF 写点阵底，写 = MT）。
#pragma once
#include "layout.h"
#include "cfg.h"

namespace app::rd {

// ── 跨屏共享状态 ──
inline char g_books[8][256];
inline int  g_nbooks = 0;
inline int  g_back_to = MODE_LIST;        // 目录/设置从哪进来的（返回用）
inline char g_list_log[160] = {0};        // 列表页底部那行 `> ...`

// reader.cpp 提供
bool open_book(int i);
bool open_book_by_name(const char* name);
bool reload_font();
void chap_begin(const char* book);

// 本文件里的屏（先声明，tick_* 里要互相跳）
inline void show_list();
inline void show_read(int32_t k);
inline void show_toc();
inline void show_cfg();
inline void show_chap_pick();
inline void show_chap_progress();
inline void show_chap_result();

// ══════════════════════════════════════════════════════════════
//  一、书目列表
// ══════════════════════════════════════════════════════════════
inline constexpr int32_t L_ROW_Y0 = 296, L_ROW_H = 52, L_ROW_PITCH = 56;
inline constexpr int32_t L_RES_Y  = 200;   // 「继续阅读」行
inline constexpr int32_t L_NAV_Y0 = 768, L_NAV_PITCH = 56, L_NAV_H = 52;

inline void book_pct_str(const char* name, char* out, int cap) {
    const int p = prog_pct(name);
    if (p < 0) ui::scopy(out, "NEW", cap);
    else       so_snprintf(out, cap, "%d%%", p);
}

inline void draw_list() {
    ui::draw_hero(72, "READER", "// 点一行开始读 · /sdcard/books");
    // 最近阅读（历史）：单独一行，点了直接续读
    const char* res = (g_nprog > 0) ? g_prog[0].name : nullptr;
    if (res && res[0]) {
        char v[24];
        book_pct_str(res, v, sizeof(v));
        char r[40];
        so_snprintf(r, sizeof(r), "%s  %s", v, "继续");
        const R rr = {ui::PADX, L_RES_Y, ui::SCR_W - 2 * ui::PADX, L_ROW_H};
        ui::draw_li(rr, res, r, 1, 0);                 // 选中态：2px 框 + 行首 "> "
        ui::draw_h(L_RES_Y - 26, "// 上次读到");
    }
    ui::draw_h(res && res[0] ? L_ROW_Y0 - 26 : L_RES_Y - 26, "// 全部书籍");

    char cnt[48];
    so_snprintf(cnt, sizeof(cnt), "// %d 本书", g_nbooks);
    if (g_nbooks <= 0) {
        ui::draw_warn(L_ROW_Y0, "[ /sdcard/books 里没有 .txt / .md ]", ui::SZ_VAL);
    } else {
        for (int i = 0; i < g_nbooks && i < 8; i++) {
            char v[24];
            book_pct_str(g_books[i], v, sizeof(v));
            const R rr = {ui::PADX, L_ROW_Y0 + i * L_ROW_PITCH, ui::SCR_W - 2 * ui::PADX, L_ROW_H};
            ui::draw_li(rr, g_books[i], v, 0, 0);
        }
    }
    ui::draw_h(L_NAV_Y0 - 26, cnt);
    const char* nav1 = "分章分析";
    const char* nav2 = "阅读设置";
    ui::draw_li({ui::PADX, L_NAV_Y0, ui::SCR_W - 2 * ui::PADX, L_NAV_H}, nav1, ">", 0, 0);
    ui::draw_li({ui::PADX, L_NAV_Y0 + L_NAV_PITCH, ui::SCR_W - 2 * ui::PADX, L_NAV_H}, nav2, ">", 0, 0);
    ui::draw_log(886, g_list_log[0] ? g_list_log : "点一行开始读", ui::GRAY);
}

inline void show_list() {
    g_mode = MODE_LIST;
    const int64_t a = so_now_ms();
    so_set_epd_mode(ui::MF);
    ui::box(0, 0, ui::SCR_W, ui::SCR_H, ui::L14);
    so_set_epd_mode(ui::MT);
    ui::bar_refresh();
    ui::bar_draw("< EXIT");
    draw_list();
    so_set_epd_mode(ui::MQ);
    ui::push({0, 0, ui::SCR_W, ui::SCR_H}, "书目列表");
    char s[96];
    so_snprintf(s, sizeof(s), "书目列表: 整屏 %lldms（%d 本）", (long long)(so_now_ms() - a), g_nbooks);
    slog(s);
}

inline void tick_list(int32_t x, int32_t y) {
    if (ui::hit_bar_left(x, y)) { prog_flush(); so_exit(); return; }
    // 最近阅读
    const char* res = (g_nprog > 0) ? g_prog[0].name : nullptr;
    if (res && res[0] && y >= L_RES_Y - 4 && y < L_RES_Y + L_ROW_H + 4) {
        if (!open_book_by_name(res)) { ui::scopy(g_list_log, "这本书已经不在了", sizeof(g_list_log)); show_list(); }
        return;
    }
    const int32_t y0 = (res && res[0]) ? L_ROW_Y0 : L_ROW_Y0;
    for (int i = 0; i < g_nbooks && i < 8; i++) {
        const int32_t ry = y0 + i * L_ROW_PITCH;
        if (y >= ry - 4 && y < ry + L_ROW_PITCH - 4 && x >= ui::PADX - 12 && x < ui::SCR_W - ui::PADX + 12) {
            if (open_book(i)) return;
            ui::scopy(g_list_log, "这本打不开（看看文件）", sizeof(g_list_log));
            show_list();
            return;
        }
    }
    if (y >= L_NAV_Y0 - 6 && y < L_NAV_Y0 + L_NAV_H + 6) { g_back_to = MODE_LIST; show_chap_pick(); return; }
    if (y >= L_NAV_Y0 + L_NAV_PITCH - 6 && y < L_NAV_Y0 + L_NAV_PITCH + L_NAV_H + 6) {
        g_back_to = MODE_LIST;
        show_cfg();
        return;
    }
}

// ══════════════════════════════════════════════════════════════
//  二、阅读页交互（整页三区 + 中央弹窗）
// ══════════════════════════════════════════════════════════════
inline bool g_popup = false;
inline int  g_popup_n = 0;
inline const char* const POPUP_ITEMS[4] = { "目录", "阅读设置", "返回书目", "关闭" };

inline void go_prev() {
    if (g_page > 0) render_page(g_page - 1);
    else            slog("（已是第一页）");
}
inline void go_next() {
    ensure_page(g_page + 1);
    if (g_ps_n > g_page + 1) render_page(g_page + 1);
    else                     slog("（已是最后一页）");
}

inline void tick_read(int32_t x, int32_t y) {
    if (g_popup) {
        const int it = hit_popup(x, y, g_popup_n);
        if (it < 0) { hide_popup(g_popup_n); g_popup = false; }
        else if (it == 0) { g_popup = false; hide_popup(g_popup_n); g_back_to = MODE_READ; show_toc(); }
        else if (it == 1) { g_popup = false; hide_popup(g_popup_n); g_back_to = MODE_READ; show_cfg(); }
        else if (it == 2) { g_popup = false; prog_touch(true); show_list(); }
        else              { hide_popup(g_popup_n); g_popup = false; }
        return;
    }
    if (ui::hit_bar_left(x, y)) { prog_touch(true); show_list(); return; }
    const int z = zone_of(x, y);
    if (z == 0)      go_prev();
    else if (z == 2) go_next();
    else if (z == 1) { g_popup_n = 4; g_popup = true; show_popup(POPUP_ITEMS, g_popup_n); }
}

// ══════════════════════════════════════════════════════════════
//  三、目录（分章索引 → 逐条列出，点一条跳过去）
// ══════════════════════════════════════════════════════════════
inline constexpr int32_t T_Y0 = 170, T_PITCH = 44, T_ROWS = 14;
inline int  g_toc_top = 0;
inline int  g_toc_cur = -1;         // 当前章（最后一个 off ≤ 当前页偏移）

inline int toc_count() { return chap::count(); }
inline int toc_pages() {
    const int n = toc_count();
    return n <= 0 ? 1 : (n + T_ROWS - 1) / T_ROWS;
}
// 当前章下标（-1 = 还在正文前）
inline int toc_current() {
    if (toc_count() <= 0) return -1;
    const chap::Chap* l = chap::list();
    const uint32_t off = (g_ps_n > 0) ? g_ps[g_page].off : 0;
    int best = -1;
    for (int i = 0; i < toc_count(); i++) { if (l[i].off <= off) best = i; else break; }
    return best;
}
inline int32_t toc_row_px(int lv) { return lv <= 1 ? 28 : (lv == 2 ? 24 : 22); }
inline int32_t toc_row_indent(int lv) { return lv <= 1 ? 0 : (lv == 2 ? 24 : 48); }

inline void draw_toc() {
    char sub[160];
    so_snprintf(sub, sizeof(sub), "// %s · 卷%d 章%d 节%d", g_name,
                chap::lv(1), chap::lv(2), chap::lv(3));
    ui::draw_hero(72, "目录", sub);

    if (chap::running()) {
        ui::draw_h(T_Y0, "// 索引生成中（本书第一次打开要扫一遍，存 SD 下次直接用）");
        char b[64];
        so_snprintf(b, sizeof(b), "%d%%   %d / %d MB", chap::pct(), chap::mb_done(), chap::mb_all());
        so_font_set_size(ui::g_font, 30.0f);
        ui::text(ui::PADX, 260, b, ui::INK, 1);
        const R px = {ui::PADX, 320, ui::SCR_W - 2 * ui::PADX, 20};
        ui::frame(px, 2, ui::INK);
        const int32_t fw = (px.w - 6) * (chap::pct() < 0 ? 0 : chap::pct()) / 100;
        if (fw > 0) ui::box(px.x + 3, px.y + 3, fw, px.h - 6, ui::INK);
        ui::draw_log(390, "扫完自动出目录 · 期间可以按 < BACK 去读书", ui::GRAY);
        return;
    }
    if (toc_count() <= 0) {
        ui::draw_warn(T_Y0, "[ 这本书没有可用的章节 ]", ui::SZ_VAL);
        ui::draw_cmt(T_Y0 + 80, "· 分章规则见 /sdcard/m5ui/reader_rules.txt");
        ui::draw_cmt(T_Y0 + 108, "· 也可以去「分章分析」看扫描结果");
        return;
    }

    g_toc_cur = toc_current();
    const int n = toc_count();
    for (int r = 0; r < T_ROWS; r++) {
        const int i = g_toc_top + r;
        if (i >= n) break;
        const int lv = chap::title_level(i);
        char t[128];
        if (!chap::read_title(i, t, sizeof(t) - 1)) ui::scopy(t, "(读不到标题)", sizeof(t));
        const int32_t y = T_Y0 + r * T_PITCH;
        const int32_t ind = toc_row_indent(lv);
        const int32_t px = toc_row_px(lv);
        so_font_set_size(ui::g_font, (float)px);
        const int32_t lh = so_font_line_height(ui::g_font);
        const int32_t ty = y + (T_PITCH - lh) / 2;
        const char* mark = (i == g_toc_cur) ? "> " : "  ";
        ui::text(ui::PADX + ind, ty, mark, ui::INK, 1);
        ui::text_fit(ui::PADX + ind + 30, ty, t,
                     lv >= 3 ? ui::GRAYD : ui::INK, 0, px, ui::SCR_W - ui::PADX);
    }
}

inline void show_toc() {
    g_mode = MODE_TOC;
    const int64_t a = so_now_ms();
    so_set_epd_mode(ui::MF);
    ui::box(0, 0, ui::SCR_W, ui::SCR_H, ui::L14);
    so_set_epd_mode(ui::MT);
    ui::bar_refresh();
    ui::bar_draw("< BACK");
    draw_toc();
    // 底部：上一页 / 下一页 + x/y
    if (!chap::running() && toc_count() > 0) {
        const int pg = g_toc_top / T_ROWS + 1;
        char pb[24];
        so_snprintf(pb, sizeof(pb), "%d/%d", pg, toc_pages());
        const R s1 = {ui::PADX, 880, 150, 52};
        const R s2 = {ui::SCR_W - ui::PADX - 150, 880, 150, 52};
        const int off = g_toc_top;
        ui::draw_seg(s1, "上一页", off > 0);
        ui::draw_seg(s2, "下一页", off + T_ROWS < toc_count());
        so_font_set_size(ui::g_font, ui::SZ_VAL);
        ui::text((ui::SCR_W - ui::text_w(pb, ui::SZ_VAL, 1)) / 2, 894, pb, ui::INK, 1);
    }
    so_set_epd_mode(ui::MQ);
    ui::push({0, 0, ui::SCR_W, ui::SCR_H}, "目录");
    char s[96];
    so_snprintf(s, sizeof(s), "目录: 整屏 %lldms（%d 条 / 第%d屏）",
                (long long)(so_now_ms() - a), toc_count(), g_toc_top / T_ROWS + 1);
    slog(s);
}

// 只翻目录页：底部按钮 + 行区
inline void toc_turn(int delta) {
    const int pages = toc_pages();
    int pg = g_toc_top / T_ROWS + delta;
    if (pg < 0) pg = 0;
    if (pg > pages - 1) pg = pages - 1;
    if (pg == g_toc_top / T_ROWS) return;
    g_toc_top = pg * T_ROWS;
    so_set_epd_mode(ui::MF);
    ui::box(0, T_Y0 - 10, ui::SCR_W, 960 - T_Y0 + 10, ui::L14);
    so_set_epd_mode(ui::MT);
    draw_toc();
    const R s1 = {ui::PADX, 880, 150, 52};
    const R s2 = {ui::SCR_W - ui::PADX - 150, 880, 150, 52};
    char pb[24];
    so_snprintf(pb, sizeof(pb), "%d/%d", pg + 1, pages);
    ui::draw_seg(s1, "上一页", g_toc_top > 0);
    ui::draw_seg(s2, "下一页", g_toc_top + T_ROWS < toc_count());
    so_font_set_size(ui::g_font, ui::SZ_VAL);
    ui::text((ui::SCR_W - ui::text_w(pb, ui::SZ_VAL, 1)) / 2, 894, pb, ui::INK, 1);
    ui::push({0, T_Y0 - 10, ui::SCR_W, 960 - T_Y0 + 10}, "目录翻页");
}

inline void tick_toc(int32_t x, int32_t y) {
    if (ui::hit_bar_left(x, y)) { show_read(g_page); return; }
    if (chap::running() || toc_count() <= 0) return;
    const R s1 = {ui::PADX, 880, 150, 52};
    if (y >= s1.y - 10 && y < s1.y + s1.h + 10) {
        if (x < ui::SCR_W / 2) toc_turn(-1); else toc_turn(1);
        return;
    }
    // 点一条 → 跳到那一章
    if (y >= T_Y0 - 6 && y < T_Y0 + T_ROWS * T_PITCH) {
        const int r = (y - (T_Y0 - 6)) / T_PITCH;
        const int i = g_toc_top + r;
        if (i < 0 || i >= toc_count()) return;
        const chap::Chap* l = chap::list();
        char t[128] = {0};
        chap::read_title(i, t, sizeof(t) - 1);
        char msg[192];
        so_snprintf(msg, sizeof(msg), "跳章 → [%d] %s @%u", i + 1, t, (unsigned)l[i].off);
        slog(msg);
        pages_reset(l[i].off, 0);            // 页表从这一章重新起（页起点 = 标题行偏移）
        // 现场取证：跳完先确认"第一页的第一块"就是刚才点的这一章（偏移/类型/文字）
        if (win_load_at(l[i].off) && g_doc.n > 0) {
            char d[200];
            so_snprintf(d, sizeof(d), "  跳章落点：页首 %u · 块0 abs=%u kind=%d 文字[%.40s] · 字号%d",
                        (unsigned)g_ps[0].off, (unsigned)blk_abs(0), g_doc.blk[0].kind,
                        g_doc.blk[0].text ? g_doc.blk[0].text : "(null)", g_size);
            slog(d);
        } else {
            slog("  跳章落点：窗口装载失败");
        }
        show_read(0);
        return;
    }
}

// ══════════════════════════════════════════════════════════════
//  四、阅读设置（字号 / 灰度 / 字体）
// ══════════════════════════════════════════════════════════════
inline constexpr int32_t C_SEG_Y = 200, C_SEG_H = 52, C_SEG_W = 100, C_SEG_GAP = 6;
inline constexpr int32_t C_PRV_Y = 266, C_PRV_H = 56;      // 预览框（高度随字号，最多 56）
inline constexpr int32_t C_INK_Y = 366;
inline constexpr int32_t C_FACE_Y = 462, C_FACE_H = 52, C_FACE_PITCH = 56;
// 版面清单（自上而下，全部从状态栏 60 以下起，留 8px 呼吸）：
//   hero 76 / sub 132 · SIZE 170 → 段式 200..252 → 预览 266..322
//   INK 336 → 段式 366..418 · FACE 432 → 字体行 462..742
//   kv 770 / 802 / 834 · log 880
inline const int SIZES[4] = {22, 26, 30, 34};
inline const char* const INK_NAMES[3] = { "浓", "中", "淡" };

inline int ink_sel(void) {
    for (int i = 0; i < 3; i++) if (INK_LEVELS[i] == g_ink) return i;
    return -1;
}
inline int size_sel(void) {
    for (int i = 0; i < 4; i++) if (SIZES[i] == g_size) return i;
    return -1;
}

inline void draw_cfg() {
    ui::draw_hero(76, "CONFIG / READER", "// 改动即生效 · 存 /sdcard/apps/reader.cfg");

    ui::draw_h(170, "// SIZE  正文字号");
    for (int i = 0; i < 4; i++) {
        char b[8];
        so_snprintf(b, sizeof(b), "%d", SIZES[i]);
        const R r = {ui::PADX + i * (C_SEG_W + C_SEG_GAP), C_SEG_Y, C_SEG_W, C_SEG_H};
        ui::draw_seg(r, b, SIZES[i] == g_size);
    }
    // 预览：拿当前字号与灰度画一行真字
    {
        const R pr = {ui::PADX, C_PRV_Y, ui::SCR_W - 2 * ui::PADX, g_size + 22};
        ui::dashed_frame(pr, 1, ui::GLINE);
        so_font_set_size(ui::g_font, (float)g_size);
        ui::text(ui::PADX + 14, C_PRV_Y + 11, "山海之间，墨痕如初。", body_ink(), 1);
    }

    ui::draw_h(336, "// INK  正文灰度");
    for (int i = 0; i < 3; i++) {
        const R r = {ui::PADX + i * (C_SEG_W + C_SEG_GAP), C_INK_Y, C_SEG_W, C_SEG_H};
        ui::draw_seg(r, INK_NAMES[i], INK_LEVELS[i] == g_ink);
    }

    ui::draw_h(432, "// FACE  字体");
    for (int i = 0; i <= g_nfonts && i < 5; i++) {
        const char* nm = (i == 0) ? "内置字体" : g_fonts[i - 1];
        const int sel = (i == 0) ? (g_face[0] == 0) : ui::streq(g_face, g_fonts[i - 1]);
        const R r = {ui::PADX, C_FACE_Y + i * C_FACE_PITCH, ui::SCR_W - 2 * ui::PADX, C_FACE_H};
        ui::draw_li(r, nm, i == 0 ? "BUILTIN" : "SD", sel, 0);
    }

    char v1[32], v2[32], v3[48];
    so_snprintf(v1, sizeof(v1), "%d PX", g_size);
    so_snprintf(v2, sizeof(v2), "%d（%s）", g_ink, ink_sel() >= 0 ? INK_NAMES[ink_sel()] : "自定义");
    ui::scopy(v3, g_face[0] ? g_face : "内置", sizeof(v3));
    ui::draw_kv(772, "SIZE", v1);
    ui::draw_kv(804, "INK", v2);
    ui::draw_kv(836, "FACE", v3);
    ui::draw_log(884, g_nfonts > 0 ? "字体来自 /sdcard/fonts/*.ttf" : "没有 SD 字体，只有内置", ui::GRAY);
}

inline void show_cfg() {
    g_mode = MODE_CFG;
    const int64_t a = so_now_ms();
    so_set_epd_mode(ui::MF);
    ui::box(0, 0, ui::SCR_W, ui::SCR_H, ui::L14);
    so_set_epd_mode(ui::MT);
    ui::bar_refresh();
    ui::bar_draw("< BACK");
    draw_cfg();
    so_set_epd_mode(ui::MQ);
    ui::push({0, 0, ui::SCR_W, ui::SCR_H}, "阅读设置");
    char s[96];
    so_snprintf(s, sizeof(s), "阅读设置: 整屏 %lldms（%dpx ink%d %s）",
                (long long)(so_now_ms() - a), g_size, g_ink, g_face[0] ? g_face : "builtin");
    slog(s);
}

// 改设置后：页表（字号/字体变了要重排）与页缓存作废，并把位置钉在当前页起点
inline void cfg_applied(bool repaginate) {
    const PageStart cur = g_ps[g_page];
    if (repaginate) pages_reset(cur.off, cur.li);
    else            pc_clear();
    cfg_save();
}

inline void cfg_set_size(int v) {
    if (v == g_size) return;
    g_size = v;
    cfg_applied(true);                     // 字号变了 → 一页装的行数变了，页表必须重排
    // 只重画跟字号有关的那两块：段式那一组 + 预览（**别碰 INK / FACE**，它们的 y 在 336 以下）
    const int32_t ry = C_SEG_Y - 6;
    const int32_t rh = (C_PRV_Y + C_PRV_H + 8) - ry;          // 194 .. 330
    so_set_epd_mode(ui::MF);
    ui::box(ui::PADX - 2, ry, ui::SCR_W - 2 * ui::PADX + 4, rh, ui::L14);
    so_set_epd_mode(ui::MT);
    for (int i = 0; i < 4; i++) {
        char b[8];
        so_snprintf(b, sizeof(b), "%d", SIZES[i]);
        const R r = {ui::PADX + i * (C_SEG_W + C_SEG_GAP), C_SEG_Y, C_SEG_W, C_SEG_H};
        ui::draw_seg(r, b, SIZES[i] == g_size);
    }
    const R pr = {ui::PADX, C_PRV_Y, ui::SCR_W - 2 * ui::PADX, g_size + 22};
    ui::dashed_frame(pr, 1, ui::GLINE);
    so_font_set_size(ui::g_font, (float)g_size);
    ui::text(ui::PADX + 14, C_PRV_Y + 11, "山海之间，墨痕如初。", body_ink(), 1);
    ui::push({ui::PADX - 2, ry, ui::SCR_W - 2 * ui::PADX + 4, rh}, "字号");
    char m[96];
    so_snprintf(m, sizeof(m), "字号 → %dpx（已存 reader.cfg）", g_size);
    slog(m);
}

inline void cfg_set_ink(int v) {
    if (v == g_ink) return;
    g_ink = v;
    cfg_applied(false);                    // 只换墨色 → 页表不用动，清缓存重画即可
    so_set_epd_mode(ui::MF);
    ui::box(ui::PADX - 2, C_INK_Y - 2, ui::SCR_W - 2 * ui::PADX + 4, C_SEG_H + 4, ui::L14);
    ui::box(ui::PADX - 2, 276, ui::SCR_W - 2 * ui::PADX + 4, 84, ui::L14);
    so_set_epd_mode(ui::MT);
    for (int i = 0; i < 3; i++) {
        const R r = {ui::PADX + i * (C_SEG_W + C_SEG_GAP), C_INK_Y, C_SEG_W, C_SEG_H};
        ui::draw_seg(r, INK_NAMES[i], INK_LEVELS[i] == g_ink);
    }
    const R pr = {ui::PADX, 284, ui::SCR_W - 2 * ui::PADX, g_size + 22};
    ui::dashed_frame(pr, 1, ui::GLINE);
    so_font_set_size(ui::g_font, (float)g_size);
    ui::text(ui::PADX + 14, 284 + 11, "山海之间，墨痕如初。", body_ink(), 1);
    ui::push({ui::PADX - 2, C_INK_Y - 2, ui::SCR_W - 2 * ui::PADX + 4, C_SEG_H + 4}, "灰度");
    char m[96];
    so_snprintf(m, sizeof(m), "灰度 → %d（已存 reader.cfg）", g_ink);
    slog(m);
}

inline void cfg_set_face(const char* name) {
    ui::scopy(g_face, name ? name : "", sizeof(g_face));
    if (!reload_font()) {                    // 加载失败 → 回退内置并清掉记录（别每次开机白磨一次）
        slog("字体加载失败 → 回退内置字体，并清掉记录");
        g_face[0] = 0;
        reload_font();
    }
    cfg_applied(true);                       // 字体换了一页装的行数也会变
    so_set_epd_mode(ui::MF);
    ui::box(ui::PADX - 2, C_FACE_Y - 2, ui::SCR_W - 2 * ui::PADX + 4, 5 * C_FACE_PITCH + 4, ui::L14);
    so_set_epd_mode(ui::MT);
    for (int i = 0; i <= g_nfonts && i < 5; i++) {
        const char* nm = (i == 0) ? "内置字体" : g_fonts[i - 1];
        const int sel = (i == 0) ? (g_face[0] == 0) : ui::streq(g_face, g_fonts[i - 1]);
        const R r = {ui::PADX, C_FACE_Y + i * C_FACE_PITCH, ui::SCR_W - 2 * ui::PADX, C_FACE_H};
        ui::draw_li(r, nm, i == 0 ? "BUILTIN" : "SD", sel, 0);
    }
    ui::push({ui::PADX - 2, C_FACE_Y - 2, ui::SCR_W - 2 * ui::PADX + 4, 5 * C_FACE_PITCH + 4}, "字体");
    char m[128];
    so_snprintf(m, sizeof(m), "字体 → %s（已存 reader.cfg）", g_face[0] ? g_face : "内置");
    slog(m);
}

inline void tick_cfg(int32_t x, int32_t y) {
    if (ui::hit_bar_left(x, y)) {
        if (g_back_to == MODE_READ) show_read(g_page);
        else                        show_list();
        return;
    }
    if (y >= C_SEG_Y - 8 && y < C_SEG_Y + C_SEG_H + 8) {
        for (int i = 0; i < 4; i++) {
            const int32_t sx = ui::PADX + i * (C_SEG_W + C_SEG_GAP);
            if (x >= sx - 4 && x < sx + C_SEG_W + 4) { cfg_set_size(SIZES[i]); return; }
        }
        return;
    }
    if (y >= C_INK_Y - 8 && y < C_INK_Y + C_SEG_H + 8) {
        for (int i = 0; i < 3; i++) {
            const int32_t sx = ui::PADX + i * (C_SEG_W + C_SEG_GAP);
            if (x >= sx - 4 && x < sx + C_SEG_W + 4) { cfg_set_ink(INK_LEVELS[i]); return; }
        }
        return;
    }
    for (int i = 0; i <= g_nfonts && i < 5; i++) {
        const int32_t ry = C_FACE_Y + i * C_FACE_PITCH;
        if (y >= ry - 4 && y < ry + C_FACE_PITCH - 4) {
            cfg_set_face(i == 0 ? "" : g_fonts[i - 1]);
            return;
        }
    }
}

// ══════════════════════════════════════════════════════════════
//  五、分章分析（工具页：选书 → 扫描进度 → 结果）
// ══════════════════════════════════════════════════════════════
inline char g_chap_book[256] = {0};
inline int  g_chap_sub = 0;                // 0 = 选书页 1 = 进度页 2 = 结果页
inline char g_chap_pool[20][128];
inline const char* g_chap_lines[20];
inline char g_chap_sum1[160];
inline char g_chap_sum2[200];

inline void draw_chap_pick() {
    ui::draw_hero(72, "分章分析", "// 点一本书：扫全文建目录索引（存 SD，只扫一次）");
    for (int i = 0; i < g_nbooks && i < 8; i++) {
        const R r = {ui::PADX, 190 + i * L_ROW_PITCH, ui::SCR_W - 2 * ui::PADX, L_ROW_H};
        char v[24];
        if (chap::count() > 0 && ui::streq(g_books[i], g_chap_book)) ui::scopy(v, "已扫", sizeof(v));
        else book_pct_str(g_books[i], v, sizeof(v));
        ui::draw_li(r, g_books[i], v, 0, 0);
    }
    ui::draw_log(886, "大书扫一遍要十几秒；结果存 /sdcard/apps/<书名>.idx", ui::GRAY);
}

inline void show_chap_pick() {
    g_mode = MODE_CHAP;
    so_set_epd_mode(ui::MF);
    ui::box(0, 0, ui::SCR_W, ui::SCR_H, ui::L14);
    so_set_epd_mode(ui::MT);
    ui::bar_refresh();
    ui::bar_draw("< BACK");
    draw_chap_pick();
    so_set_epd_mode(ui::MQ);
    ui::push({0, 0, ui::SCR_W, ui::SCR_H}, "分章选书");
}

inline void draw_chap_prog_body();          // 前向：进度页的正文块（不含 hero）
inline void draw_chap_progress() {
    ui::draw_hero(72, "分章分析", "// 正在扫全文（只看章节标题行，读到哪扫到哪）");
    draw_chap_prog_body();
}

inline void draw_chap_prog_body() {
    so_font_set_size(ui::g_font, 24.0f);
    ui::text(ui::PADX, 162, g_chap_book, ui::INK, 1);
    char b[64];
    so_snprintf(b, sizeof(b), "%d%%", chap::pct());
    so_font_set_size(ui::g_font, 34.0f);
    ui::text(ui::PADX, 208, b, ui::INK, 0);
    so_snprintf(b, sizeof(b), "%d / %d MB · 已收 %d 项", chap::mb_done(), chap::mb_all(), chap::n_chaps());
    so_font_set_size(ui::g_font, ui::SZ_VAL);
    ui::text(ui::PADX + 150, 224, b, ui::INK, 1);
    const R px = {ui::PADX, 276, ui::SCR_W - 2 * ui::PADX, 20};
    ui::frame(px, 2, ui::INK);
    const int fw = (px.w - 6) * (chap::pct() < 0 ? 0 : chap::pct()) / 100;
    if (fw > 0) ui::box(px.x + 3, px.y + 3, fw, px.h - 6, ui::INK);
    ui::draw_log(356, "扫描期间可以按 < BACK 取消", ui::GRAY);
    ui::draw_log(392, "一次只扫一本书，扫完结果常驻 SD", ui::GRAYD);
}

inline void show_chap_progress() {
    g_mode = MODE_CHAP;
    so_set_epd_mode(ui::MF);
    ui::box(0, 0, ui::SCR_W, ui::SCR_H, ui::L14);
    so_set_epd_mode(ui::MT);
    ui::bar_refresh();
    ui::bar_draw("< BACK");
    draw_chap_progress();
    so_set_epd_mode(ui::MQ);
    ui::push({0, 0, ui::SCR_W, ui::SCR_H}, "分章进度");
}

inline int g_chap_shown_pct = -1;
inline void update_chap_progress() {
    const int p = chap::pct();
    if (p < g_chap_shown_pct + 5 && p < 100) return;
    g_chap_shown_pct = p;
    so_set_epd_mode(ui::MF);
    ui::box(0, 152, ui::SCR_W, 268, ui::L14);          // 只擦正文块（hero 不动）
    so_set_epd_mode(ui::MT);
    draw_chap_prog_body();
    ui::push({0, 152, ui::SCR_W, 268}, "分章进度条");
}

inline void draw_chap_result() {
    ui::draw_hero(72, "分章分析 · 结果", "// 索引已存 SD：下次进这本书直接用，不再扫");
    so_font_set_size(ui::g_font, 24.0f);
    ui::text(ui::PADX, 162, g_chap_book, ui::INK, 1);
    ui::draw_kv(212, "LEVELS", g_chap_sum1);
    ui::draw_cmt(256, g_chap_sum2);
    ui::draw_h(302, "// 前 20 条");
    for (int i = 0; i < 20; i++) {
        if (!g_chap_lines[i]) break;
        ui::text_fit(ui::PADX, 336 + i * 28, g_chap_lines[i], ui::INK, 0, ui::SZ_KEY, ui::SCR_W - ui::PADX);
    }
}

inline void show_chap_result() {
    g_mode = MODE_CHAP;
    g_chap_sub = 2;
    so_set_epd_mode(ui::MF);
    ui::box(0, 0, ui::SCR_W, ui::SCR_H, ui::L14);
    so_set_epd_mode(ui::MT);
    ui::bar_refresh();
    ui::bar_draw("< BACK");
    draw_chap_result();
    so_set_epd_mode(ui::MQ);
    ui::push({0, 0, ui::SCR_W, ui::SCR_H}, "分章结果");
}

inline void tick_chap(int32_t x, int32_t y) {
    if (ui::hit_bar_left(x, y)) {
        if (g_back_to == MODE_READ && g_chap_sub != 0) { show_read(g_page); }
        else                                             show_list();
        return;
    }
    if (g_chap_sub != 0) return;              // 进度页 / 结果页：只认状态栏的 < BACK
    for (int i = 0; i < g_nbooks && i < 8; i++) {
        const int32_t ry = 190 + i * L_ROW_PITCH;
        if (y >= ry - 4 && y < ry + L_ROW_PITCH - 4) {
            chap_begin(g_books[i]);
            return;
        }
    }
}

} // namespace app::rd
