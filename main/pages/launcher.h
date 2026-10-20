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

namespace m5ui::pages::launcher {

using namespace m5ui::ui;
using namespace m5ui::ecs::components;
using namespace m5ui::ui::widgets;

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
    const char* name = (const char*)ctx;     // 完整文件名（含 .so）
    auto& w = pages::g_ctx.app->world;
    for (int i = 0; i < pages::g_launcher.n; i++) w.remove<Visible>(pages::g_launcher.e[i]);
    // elf_loader 的 base path 是 /sdcard，文件在 /sdcard/apps/ 下 → 名字带 "apps/" 前缀
    char path[80];
    snprintf(path, sizeof(path), "apps/%s", name);
    if (m5ui::plugin::load_app(path)) return;    // 成功：app 接管屏幕
    // 失败：放回 launcher + 把原因摆在屏上
    for (int i = 0; i < pages::g_launcher.n; i++) {
        w.add(pages::g_launcher.e[i], Visible{});
        w.add(pages::g_launcher.e[i], Dirty{});
    }
    snprintf(g_app_err, sizeof(g_app_err), "%s：%s", name, m5ui::plugin::last_error());
    set_app_err();
}

// ── 顶栏时钟 ──
inline Entity  e_clock;
inline Entity  e_clock_bg;      // 时钟背后那块 FAST 点阵（时间变了先重抖，擦掉旧数字）
inline char    g_clock[16] = "--:--";
inline int     g_clock_val = -2;
inline int64_t g_clock_us  = 0;

inline void refresh_clock() {
    const int t = m5ui::rtctime::hhmm();
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
inline constexpr int32_t LICON_X  = 86;    // 图标左缘（32×32）
inline constexpr int32_t LNAME_X  = 128;   // 应用名起点（图标右侧）
inline constexpr int32_t LARROW_X = 488;   // 箭头">"起点
inline constexpr int32_t LROW_Y0  = 190;   // 首行
inline constexpr int32_t LROW_H   = 72;    // 行高（图标 32 + 名称/描述两行）
inline constexpr int32_t LSHOW    = 8;     // 一屏最多几行

// 点阵底 / 细线（终端风的底不是纯白，是浅灰走 FAST 量化出来的 Bayer 抖动）
inline constexpr uint32_t LBG   = 0xE8E8E8;   // 文字实体的 bg 也用这个值，才不会在抖动底上抠出白块
inline constexpr uint32_t LRULE = 0x888888;   // 1px 细线（FAST 下 50% 抖动，肉眼看得见）

// 指定字号下的行高（建页时量一次；渲染器每实体自己 set_size，不会受这里影响）
inline int32_t lh_at(int32_t px) {
    m5ui::ttf::set_size((float)px);
    return m5ui::ttf::line_height();
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

        // 左侧竖墨条（列表区左边一道实心黑，把列表钉在版面上）
        P(rect(w, LX, LROW_Y0, LBAR_W, rows_bottom - LROW_Y0, theme::FG_BLACK));
        // 序号列右侧 1px 竖分隔
        P(rect(w, LDIV_X, LROW_Y0, 1, rows_bottom - LROW_Y0, LRULE));

        for (int i = 0; i < show; i++) {
            const apps::AppFile& a = g_apps[i];
            const int y = LROW_Y0 + i * LROW_H;

            // 触摸靶子（不绘制，只接点击）
            P(app_row(w, LX, y, 500, LROW_H, on_open_app, (void*)a.name));

            // 序号（两位，弱灰）
            snprintf(g_app_no[i], sizeof(g_app_no[i]), "%02d", i + 1);
            T(P(setting_text(w, LNO_X, lty(y, 20), g_app_no[i], theme::G55, LBG)), 20);

            // 图标（.so 自带位图；没有元信息就空着）
            if (a.icon_w > 0 && a.icon_h > 0) {
                const int iy = y + (LROW_H - a.icon_h) / 2;
                P(bitmap(w, LICON_X, iy, a.icon, a.icon_w, a.icon_h, a.icon_bpp, LBG));
            }

            // 名称（.so 自带，没有就用文件名）
            T(P(setting_text(w, LNAME_X, y + 4, a.label, theme::FG_BLACK, LBG)), 26);

            // 描述（名称下一行）—— 原来是 16px + G99，电子墨水屏上又小又淡，看不清
            if (a.desc[0])
                T(P(setting_text(w, LNAME_X, y + 40, a.desc, theme::G55, LBG)), 19);

            // 点线引（只画在名称那条中线上）
            m5ui::ttf::set_size(26.0f);
            int32_t nw = 0, nh = 0;
            m5ui::ttf::measure(a.label, &nw, &nh);
            const int32_t lx0 = LNAME_X + nw + 12;
            const int32_t lx1 = LARROW_X - 16;
            const int lny = y + 6 + lh_at(26) / 2;
            if (lx1 > lx0) P(rect(w, lx0, lny, lx1 - lx0, 1, LRULE));

            // 箭头（名称同一行）
            T(P(setting_text(w, LARROW_X, y + 6, ">", theme::FG_BLACK, LBG)), 26);
        }

        // 多出来的应用：页脚提示（列表一屏只放 LSHOW 行）
        if (g_app_count > LSHOW) {
            snprintf(g_more, sizeof(g_more), "// 还有 %d 个 · 整理 /sdcard/apps 查看", (int)(g_app_count - LSHOW));
            T(P(setting_text(w, LX, rows_bottom + 10, g_more, theme::G55, LBG)), 19);
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

} // namespace m5ui::pages::launcher
