#pragma once

// pages/launcher.h — 主页（app 启动器）
//
// 终端风 v2（2026-10-18 用户「有点单调」后重排）：
//   左侧竖墨条 + 序号列 + 表格式分隔线 + 页脚状态行，把 960px 的纵向空间用起来。
//   扫 /sdcard/apps/*.so，点一行 dlopen 打开（app 全屏独占）。
//   设置入口就是列表里的 settings.so。
//
//   版面（540×960）：
//     顶栏时钟  y=18
//     HERO      y=54   46px「APPS」
//     分隔粗线  y=124  2px 黑
//     副标      y=138
//     列表      y=190 起，行高 64；左侧 3px 墨条 + 序号列 + 1px 竖分隔 + 点线引到箭头
//     页脚细线  y=820
//     状态行    y=836  应用数 · 剩余堆 · 电量
//     AWAIT     y=892

#include <cstdio>
#include <cstring>
#include "esp_timer.h"
#include "esp_system.h"        // 剩余堆（页脚状态）
#include "pages/manager.h"
#include "ui/widgets.h"
#include "platform/apps.h"
#include "platform/rtc_time.h"   // 顶栏时钟：时间源＝板载 RTC（网络只做校准）
#include "text/ttf.h"            // 行高 / 量宽（垂直居中 + 点线起点）
#include "plugin.h"

namespace papers3::pages::launcher {

using namespace papers3::ui;
using namespace papers3::ecs::components;
using namespace papers3::ui::widgets;

inline apps::AppFile g_apps[16];
inline int           g_app_count = 0;
inline char          g_app_no[16][4];       // 序号 "01".."16"（Text 只存指针，缓冲必须先活到渲染）
inline char          g_sub[96];             // 副标（同上）
inline char          g_status[96];          // 页脚状态行（同上）
inline char          g_more[96];            // 「还有 N 个」（同上）
inline char          g_app_err[180] = "";
inline Entity        e_app_err;

inline void set_app_err() {
    auto& w = pages::g_ctx.app->world;
    if (auto* t = w.get_mut<Text>(e_app_err)) t->str = g_app_err;
    w.add(e_app_err, Visible{});
    w.add(e_app_err, Dirty{});
}

inline void on_open_app(void* ctx) {
    if (!ctx) return;                        // 翻页后的空行：靶子还在，但没有 app
    const char* name = (const char*)ctx;     // 完整文件名（含 .so）
    auto& w = pages::g_ctx.app->world;
    for (int i = 0; i < pages::g_launcher.n; i++) w.remove<Visible>(pages::g_launcher.e[i]);
    // elf_loader 的 base path 是 /sdcard，文件在 /sdcard/apps/ 下 → 名字带 "apps/" 前缀
    char path[80];
    snprintf(path, sizeof(path), "apps/%s", name);
    if (papers3::plugin::load_app(path)) return;    // 成功：app 接管屏幕
    // 失败：放回 launcher + 把原因摆在屏上
    for (int i = 0; i < pages::g_launcher.n; i++) {
        w.add(pages::g_launcher.e[i], Visible{});
        w.add(pages::g_launcher.e[i], Dirty{});
    }
    snprintf(g_app_err, sizeof(g_app_err), "%s：%s", name, papers3::plugin::last_error());
    set_app_err();
}

// ── 顶栏时钟 ──
inline Entity  e_clock;
inline Entity  e_clock_bg;      // 时钟背后那块 FAST 点阵（时间变了先重抖，擦掉旧数字）
inline char    g_clock[16] = "--:--";
inline int     g_clock_val = -2;
inline int64_t g_clock_us  = 0;

inline void refresh_clock() {
    const int t = papers3::rtctime::hhmm();
    if (t == g_clock_val) return;
    g_clock_val = t;
    if (t < 0) snprintf(g_clock, sizeof(g_clock), "--:--");
    else       snprintf(g_clock, sizeof(g_clock), "%02d:%02d", t / 100, t % 100);
    auto& w = pages::g_ctx.app->world;
    if (auto* x = w.get_mut<Text>(e_clock)) x->str = g_clock;
    // ★ 时钟字挂的是 TextNoBg（不铺底），旧数字没人擦 —— 先用 FAST 把这块重抖一遍再画。
    w.add(e_clock_bg, Dirty{});
    w.add(e_clock, Dirty{});
}

inline void tick() {
    if (pages::g_app_front) return;          // 前台是 .so app：整页在画 app，别往上面刷启动器的时钟
    const int64_t now = esp_timer_get_time();
    if (now - g_clock_us < 2000000) return;
    g_clock_us = now;
    refresh_clock();
}

// ── 布局 ──
inline constexpr int32_t LX       = 20;    // 左距
inline constexpr int32_t LBAR_W   = 3;     // 左侧竖墨条宽
inline constexpr int32_t LNO_X    = 34;    // 序号列起点
inline constexpr int32_t LDIV_X   = 78;    // 序号列右侧 1px 竖线
inline constexpr int32_t LICON_X  = 86;    // 图标左缘（源 32×32，按 2× 画成 64×64）
inline constexpr int32_t LNAME_X  = 158;   // 应用名起点（图标 2× = 64 宽，压在 86+64+8 之后）
inline constexpr int32_t LARROW_X = 488;   // 箭头">"起点
inline constexpr int32_t LROW_Y0  = 190;   // 首行
inline constexpr int32_t LROW_H   = 72;    // 行高（图标 32 + 名称/描述两行）
inline constexpr int32_t LSHOW    = 8;     // 一屏最多几行

// 点阵底 / 细线（终端风的底不是纯白，是浅灰走 FAST 量化出来的 Bayer 抖动）
inline constexpr uint32_t LBG   = 0xE8E8E8;   // 文字实体的 bg 也用这个值，才不会在抖动底上抠出白块
inline constexpr uint32_t LRULE = 0x888888;   // 1px 细线（FAST 下 50% 抖动，肉眼看得见）

// 指定字号下的行高（建页时量一次；渲染器每实体自己 set_size，不会受这里影响）
inline int32_t lh_at(int32_t px) {
    papers3::ttf::set_size((float)px);
    return papers3::ttf::line_height();
}
// 行内文字垂直居中
inline int32_t lty(int32_t y, int32_t px) {
    return y + (LROW_H - lh_at(px)) / 2;
}

// 可点行 = **纯触摸靶子**，不挂 Style/Text。
// ★ 原来挂了 Style：按下时渲染器会按「2px 黑边 + bg 填充」整行重画，
//   把那行上面的序号/名字/点线全盖成空白，而且按下+松开各推一次 TEXT（2×780ms）
//   → 用户看到「一整行变黑闪烁」。不挂 Style 就不进 render 的 Rect+Style 查询，
//   只留 Rect+Touchable+Visible 给 touch 系统命中。
inline Entity app_row(ecs::World& w, int32_t x, int32_t y, int32_t cw, int32_t ch,
                      void (*fn)(void*), void* ctx) {
    auto e = w.spawn();
    w.add(e, Visible{});
    w.add(e, Rect{x, y, cw, ch});
    w.add(e, Touchable{0xFF});
    w.add(e, OnClick{fn, ctx});
    return e;
}

// ── 翻页（一屏 8 行）────────────────────────────────────────────────
// ★ 为什么不"重建页面"：实体编号**不复用**（World::destroy 只清 alive，不回收到池子），
//   重建一次列表就永久吃掉 60+ 个编号 → 切两页就把 256 的池子耗光。
//   所以行实体只建一次，翻页只换它们指向的 app：
//   Text 存的是指针，直接指向 g_apps[idx] 里的稳定缓冲，不用再拷一份。
inline constexpr int LROWS = 8;                       // 行实体数量（= 一屏行数）
inline int g_page  = 0;
inline int g_pages = 1;
inline Entity e_row[LROWS], e_no[LROWS], e_icon[LROWS], e_name[LROWS], e_desc[LROWS], e_dot[LROWS];
inline Entity e_arrow[LROWS];
inline Entity e_listbg = ecs::INVALID_ENTITY;   // 列表区底：翻页时重抖一遍，擦掉上一页的字/图标
inline Entity e_barbar = ecs::INVALID_ENTITY;   // 左侧墨条
inline Entity e_div    = ecs::INVALID_ENTITY;   // 序号列竖线
inline Entity e_more   = ecs::INVALID_ENTITY;   // 页脚（够多页时 = 翻页靶子）

// 把第 i 行摆成 g_apps[idx]（idx 越界 → 空行）
inline void fill_row(ecs::World& w, int i, int idx) {
    const int y = LROW_Y0 + i * LROW_H;
    const bool has = (idx >= 0 && idx < g_app_count && idx < 16);

    if (auto* oc = w.get_mut<OnClick>(e_row[i])) oc->ctx = has ? (void*)g_apps[idx].name : nullptr;

    // & 0x7F 只是把写入长度夹在 "%02d" 的 3 字节以内（缓冲 4 字节）——
    // 编译器要能证明不会截断。app 最多 16 个，实际值永远 ≤ 16。
    if (has) snprintf(g_app_no[i], sizeof(g_app_no[i]), "%02d", (idx + 1) & 0x7F);
    else     g_app_no[i][0] = 0;
    if (auto* t = w.get_mut<Text>(e_no[i]))   t->str = g_app_no[i];
    if (auto* t = w.get_mut<Text>(e_name[i])) t->str = has ? g_apps[idx].label : "";
    if (auto* t = w.get_mut<Text>(e_desc[i])) t->str = (has && g_apps[idx].desc[0]) ? g_apps[idx].desc : "";

    const bool ic = has && g_apps[idx].icon_w > 0 && g_apps[idx].icon_h > 0;
    if (auto* bm = w.get_mut<Bitmap>(e_icon[i])) {
        bm->data = ic ? g_apps[idx].icon : nullptr;
        bm->w    = ic ? g_apps[idx].icon_w : 0;
        bm->h    = ic ? g_apps[idx].icon_h : 0;
        bm->bpp  = ic ? g_apps[idx].icon_bpp : 1;
        bm->scale = 2;      // ★ 一行有两行字（名称+简介 ≈54px），32×32 的图标太小 → 2× = 64×64
    }
    if (auto* r = w.get_mut<Rect>(e_icon[i])) {
        const int32_t iw = ic ? (int32_t)g_apps[idx].icon_w * 2 : 0;   // 2× 放大后的实际尺寸
        const int32_t ih = ic ? (int32_t)g_apps[idx].icon_h * 2 : 0;
        r->x = LICON_X;
        r->y = y + (LROW_H - ih) / 2;
        r->w = iw;
        r->h = ih;
    }

    // 点线：按这一行名字**实测**宽度重算（每页名字都不一样）
    papers3::ttf::set_size(26.0f);
    int32_t nw = 0, nh = 0;
    if (has) papers3::ttf::measure(g_apps[idx].label, &nw, &nh);
    if (auto* r = w.get_mut<Rect>(e_dot[i])) {
        const int32_t lx0 = LNAME_X + nw + 12;
        const int32_t lx1 = LARROW_X - 16;
        r->x = lx0;
        r->y = y + 6 + lh_at(26) / 2;
        r->w = (lx1 > lx0) ? (lx1 - lx0) : 0;
        r->h = 1;
    }
    // ★ 点线是「纯 rect」，它的填充色就是 Style.bg —— 宽度为 0 时渲染会按
    //   「文字尺寸兜底」补一个 8×8 的小方块，所以在空行上把底色改成列表底，
    //   看起来才是"什么都没有"。
    if (auto* st = w.get_mut<Style>(e_dot[i]))
        st->bg = (w.get<Rect>(e_dot[i]) && w.get<Rect>(e_dot[i])->w > 0) ? LRULE : LBG;

    // 空行不画箭头（> 是静态字，不清就会在每个空行后面挂一个孤零零的箭头）
    if (auto* t = w.get_mut<Text>(e_arrow[i])) t->str = has ? ">" : "";
}

// 换页：只改行内数据 + 整块重画，不新建实体
inline void show_page(int p) {
    if (g_pages <= 1) return;
    auto& w = pages::g_ctx.app->world;
    p %= g_pages;
    if (p < 0) p += g_pages;
    g_page = p;

    const int rows_n = (g_app_count < LSHOW) ? g_app_count : LSHOW;
    const int rows_bottom = LROW_Y0 + rows_n * LROW_H - 8;

    // ★ 整块交给「带」：带内实体各自不推屏，由 ui_task 的 pages::flush_band()
    //   一次 QUALITY 收尾。逐个实体推 = 每块几百 ms，翻一页要十几秒。
    systems::band_begin(LROW_Y0 - 6, rows_bottom + 44 - (LROW_Y0 - 6));

    w.add(e_listbg, Dirty{});      // 先把整块刷回点阵（擦掉上一页的字和图标）
    w.add(e_barbar, Dirty{});
    w.add(e_div, Dirty{});
    for (int i = 0; i < LROWS; i++) {
        fill_row(w, i, p * LROWS + i);
        w.add(e_no[i], Dirty{});   w.add(e_name[i], Dirty{});
        w.add(e_desc[i], Dirty{}); w.add(e_icon[i], Dirty{}); w.add(e_dot[i], Dirty{});
        w.add(e_arrow[i], Dirty{});      // 箭头也被列表底盖住了，必须跟着重画
    }
    snprintf(g_more, sizeof(g_more), "// 第 %d/%d 页 · 点这里翻页", g_page + 1, g_pages);
    if (auto* t = w.get_mut<Text>(e_more)) t->str = g_more;
    w.add(e_more, Dirty{});
}
inline void on_flip_page(void*) { show_page(g_page + 1); }

inline void build(ecs::World& w) {
    auto& P = pages::track;
    // 所有静态文字统一：不铺底（坐在点阵上）+ 指定字号
    auto T = [&](ecs::Entity e, int px) { w.add(e, TextNoBg{}); w.add(e, FontSize{(int16_t)px}); return e; };

    P(rect(w, 0, 0, 540, 960, LBG));      // 整屏点阵底（FAST → Bayer 抖动，同时盖住上一页）

    // 顶栏时钟 + 它背后那块 FAST 点阵（时间变了先重抖这块，擦掉旧数字）
    e_clock_bg = P(rect(w, 424, 12, 100, 36, LBG));
    e_clock    = T(P(setting_text(w, 430, 18, g_clock, theme::FG_BLACK, LBG)), 20);

    // HERO 大字（46px，比原来 38 更压得住版面）
    T(P(setting_text(w, LX, 54, "APPS", theme::FG_BLACK, LBG)), 46);

    // 标题下的粗分隔线（2px 黑，整条）
    P(rect(w, LX, 124, 500, 2, theme::FG_BLACK));

    g_app_count = apps::scan(g_apps, 16);

    if (g_app_count == 0) {
        snprintf(g_sub, sizeof(g_sub), "// SD /sdcard/apps/ 里没有 .so");
        T(P(setting_text(w, LX, 138, g_sub, theme::G55, LBG)), 20);
    } else {
        snprintf(g_sub, sizeof(g_sub), "// %d 个应用 · 点一行打开", g_app_count);
        T(P(setting_text(w, LX, 138, g_sub, theme::G55, LBG)), 20);

        const int show = (g_app_count < LSHOW) ? g_app_count : LSHOW;
        const int rows_bottom = LROW_Y0 + show * LROW_H - 8;

        // 列表底（翻页时靠它一次把整块刷回点阵：这些字都挂 TextNoBg，自己不擦旧字形，
        // 图标实体更是从不擦底）。必须建在墨条/竖线/行文字**之前**（渲染顺序 = 创建顺序）。
        e_listbg = P(rect(w, LX, LROW_Y0 - 6, 500, rows_bottom - LROW_Y0 + 12, LBG));
        // 左侧竖墨条（列表区左边一道实心黑，把列表钉在版面上）
        e_barbar = P(rect(w, LX, LROW_Y0, LBAR_W, rows_bottom - LROW_Y0, theme::FG_BLACK));
        // 序号列右侧 1px 竖分隔
        e_div = P(rect(w, LDIV_X, LROW_Y0, 1, rows_bottom - LROW_Y0, LRULE));

        g_pages = (g_app_count + LROWS - 1) / LROWS;

        for (int i = 0; i < show; i++) {
            const int y = LROW_Y0 + i * LROW_H;

            // 触摸靶子（不绘制，只接点击；ctx 由 fill_row 填）
            e_row[i] = P(app_row(w, LX, y, 500, LROW_H, on_open_app, nullptr));

            // 序号（两位，弱灰）
            g_app_no[i][0] = 0;
            e_no[i] = T(P(setting_text(w, LNO_X, lty(y, 20), g_app_no[i], theme::G55, LBG)), 20);

            // 图标（内容由 fill_row 填：空位先建 0×0，翻页时换数据 + 换 Rect）
            e_icon[i] = P(bitmap(w, LICON_X, y, nullptr, 0, 0, 1, LBG));

            // 名称（.so 自带，没有就用文件名）
            e_name[i] = T(P(setting_text(w, LNAME_X, y + 4, "", theme::FG_BLACK, LBG)), 26);

            // 描述（名称下一行）
            e_desc[i] = T(P(setting_text(w, LNAME_X, y + 40, "", theme::G55, LBG)), 19);

            // 点线引（只画在名称那条中线上；宽度由 fill_row 按名字实测重算）
            e_dot[i] = P(rect(w, LNAME_X, y + 6 + lh_at(26) / 2, 0, 1, LRULE));

            // 箭头（名称同一行）
            e_arrow[i] = T(P(setting_text(w, LARROW_X, y + 6, ">", theme::FG_BLACK, LBG)), 26);

            fill_row(w, i, i);          // 第 0 页
        }

        // 页脚那行：够两页 → 变成翻页靶子（点一下换页并显示页码）；只有一页 → 空着
        g_more[0] = 0;
        e_more = T(P(setting_text(w, LX, rows_bottom + 10, g_more, theme::G55, LBG)), 19);
        if (g_pages > 1) {
            P(app_row(w, LX, rows_bottom + 4, 500, 36, on_flip_page, nullptr));
            snprintf(g_more, sizeof(g_more), "// 第 1/%d 页 · 点这里翻页", g_pages);
        }
    }

    // ── 页脚 ──
    P(rect(w, LX, 820, 500, 1, LRULE));
    {
        const int bat = (int)M5.Power.getBatteryLevel();
        const int batp = (bat < 0) ? 0 : (bat > 100 ? 100 : bat);
        snprintf(g_status, sizeof(g_status), "%d APPS · %dKB FREE · BAT %d%%",
                 g_app_count, (int)(esp_get_free_heap_size() / 1024), batp);
        T(P(setting_text(w, LX, 836, g_status, theme::G55, LBG)), 18);
    }

    // 打开失败原因行（默认隐藏）
    e_app_err = T(P(setting_text(w, LX, 782, g_app_err, theme::FG_BLACK, LBG)), 19);
    w.remove<Visible>(e_app_err);

    // 底部终端提示
    T(P(setting_text(w, LX, 892, "AWAITING COMMAND", theme::G99, LBG)), 18);
}

} // namespace papers3::pages::launcher
