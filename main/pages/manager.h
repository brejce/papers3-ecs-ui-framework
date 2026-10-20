#pragma once

// pages/manager.h — 页面管理
//
// 页面实体开机一次性建好，共存于同一个 World，切页只切 Visible 组件
// （渲染 / 触摸都只认挂了 Visible 的实体）。
//
// EPD 上换页必须整屏刷一次 QUALITY：旧页的像素不会自己消失。

#include <M5Unified.h>
#include "ecs/world.hpp"
#include "ecs/components.h"
#include "app.h"

namespace m5ui::pages {

using ecs::Entity;
using ecs::components::Dirty;
using ecs::components::Visible;

static constexpr int MAX_PAGE_ENTS = 160;

struct Page {
    Entity e[MAX_PAGE_ENTS];
    int    n = 0;
    void (*on_leave)() = nullptr;   // 切走这一页时调用（比如输入法固化词频）
    void (*on_enter)() = nullptr;   // 切进这一页时调用（比如阅读器重扫书目）
};

struct Ctx {
    App*  app = nullptr;
    Page* cur = nullptr;            // 当前正在建 / 正在显示的页
};

inline Ctx  g_ctx;
inline Page g_launcher;             // 首页：app 启动器

// ── 输入法的「输出目标」：谁打开输入法谁设置这两样 ──
// 输入法现在**不是页面**，是覆盖层能力（g_open_overlay 弹窗），不认识任何页面。
inline void (*g_ime_done)(const char*) = nullptr;   // 「完成」时把最终文字交给谁
inline const char* g_ime_seed = nullptr;            // 进覆盖层时预填的原文（可空 = 空着输）
inline void (*g_ime_cancel)() = nullptr;            // 覆盖层「×」取消时回调（可空）

// ── 建页：之后 track() 的实体都记到这一页 ──
inline void begin(Page& p) { g_ctx.cur = &p; p.n = 0; }

// ── 只换「当前页」指针，不动记账（建完页复位用）──
inline void focus(Page& p) { g_ctx.cur = &p; }

inline Entity track(Entity e) {
    Page* p = g_ctx.cur;
    if (p && p->n < MAX_PAGE_ENTS) p->e[p->n++] = e;
    return e;
}

// ══════════ 覆盖层（弹窗）══════════
//
// 键盘这类"盖在当前页上面"的东西走这里，**不占 Page**（不动当前页的 Visible）。
// 收起时的还原方式是文档 `docs/display/EPD_TEST_MANUAL.md` §5.2 方案 A：
//   m5ui 是保留式渲染器 —— **实体树本身就是那份"背景"**，不需要把帧缓冲存下来。
//   把被盖住的实体挂 Dirty，渲染器从模型重画一遍就还原了。
//
// 档位（§6 实测）：弹窗开/关**只有 quality 族干净**，FASTEST / TEXT 关窗后都留 popup 印记。
//   原厂 quality 一条带 ≈ 1097ms；换 lut_quality13 可到 633ms，但那要 vendor m5gfx —— 先不动。

static constexpr int32_t SCR_W = 540, SCR_H = 960;

inline Page g_overlay;                          // 覆盖层自己的实体表
// 谁提供覆盖层内容谁登记（输入法页在 build 里登记）。别的页只管调这个，
// 不必 include 输入法页。
inline void (*g_open_overlay)() = nullptr;
inline bool g_overlay_on = false;
inline int32_t g_overlay_y = 0;                 // 覆盖层顶边

inline void overlay_begin() { g_overlay.n = 0; }

// 建覆盖层内容时用这个记账（和 track() 并用：同一批实体既要属于"输入法页"，也要属于覆盖层）
inline Entity track_overlay(Entity e) {
    if (g_overlay.n < MAX_PAGE_ENTS) g_overlay.e[g_overlay.n++] = e;
    return e;
}

inline void show_overlay(int32_t y0) {
    auto& w = g_ctx.app->world;
    g_overlay_y = y0;
    for (int i = 0; i < g_overlay.n; i++) {
        w.add(g_overlay.e[i], Visible{});
        w.add(g_overlay.e[i], Dirty{});
    }
    systems::band_begin(y0, SCR_H - y0);
    g_overlay_on = true;
}

inline void hide_overlay() {
    using ecs::components::Rect;
    auto& w = g_ctx.app->world;
    for (int i = 0; i < g_overlay.n; i++) w.remove<Visible>(g_overlay.e[i]);

    // ★ 还原第一步：把那条带铺回画布色（白）。
    //   不铺这一步的话，弹窗的按键像素没人擦 —— 带内实体各自只擦自己的小框。
    //   写入也必须在 QUALITY 下（档位同时决定写入位深，§2.3）。
    M5.Display.setEpdMode(epd_mode_t::epd_quality);
    M5.Display.startWrite();
    M5.Display.fillRect(0, g_overlay_y, SCR_W, SCR_H - g_overlay_y, TFT_WHITE);
    M5.Display.endWrite();

    // ★ 还原第二步：把**被盖住**的实体挂 Dirty，渲染器从模型重画（§5.2 方案 A）。
    //   判相交用「底边」——文字类实体 h 可能是 0（尺寸由渲染实测），按行高兜底。
    //   ⚠️ **跳过整屏背景**（每页第一块 rect(0,0,540,960,白)）：它横跨屏幕，
    //      一重画就把整个帧缓冲刷白，而带外的实体没被标脏 → 上半屏会全空。
    //      带底已经由上面那句 fillRect 铺好了，不需要它。
    if (g_ctx.cur) {
        for (int i = 0; i < g_ctx.cur->n; i++) {
            auto* r = w.get<Rect>(g_ctx.cur->e[i]);
            if (!r) continue;
            if (r->w >= SCR_W && r->h >= SCR_H) continue;          // 整屏背景，跳过
            const int32_t h = (r->h > 0) ? r->h : (m5ui::ttf::line_height() + 8);
            if (r->y + h > g_overlay_y) w.add(g_ctx.cur->e[i], Dirty{});
        }
    }
    systems::band_begin(g_overlay_y, SCR_H - g_overlay_y);
    g_overlay_on = false;
}

// 由主循环在 app.update()（= render）之后调一次：
// 用 QUALITY 把那条带一次推完。**档位必须在 display() 之前 set**（§9 需求 4）。
inline void flush_band() {
    if (systems::g_band_y < 0) return;
    const int32_t y = systems::g_band_y, h = systems::g_band_h;
    systems::g_band_y = -1;
    M5.Display.setEpdMode(epd_mode_t::epd_quality);
    M5.Display.display(0, y, SCR_W, h);
}

// ── 切页 ──
//
// 不再每次切页都整屏 QUALITY 清场（那要 1209ms）。
// **每个页面建的第一件事就是 rect(0,0,540,960, 白)** —— 它自己就把旧页整屏盖掉了，
// 而且是一块白底矩形的推法（FAST/DU，约 377ms），比 QUALITY 波形「先全黑全白清场」快得多。
//
// 累积重影还是要管的：每 FULL_EVERY 次切页用 QUALITY 清一次场
// （对照 ~/Project/frame/text_view.cpp:162 的 _full_refresh_interval 思路）。
static constexpr int FULL_EVERY = 8;
inline int& _since_wash() { static int n = 0; return n; }

// QUALITY 整屏清场：先全黑全白再落墨，专门用来消累积重影
inline void quality_wash() {
    M5.Display.setEpdMode(epd_mode_t::epd_quality);
    M5.Display.startWrite();
    M5.Display.fillScreen(TFT_WHITE);
    M5.Display.endWrite();
    M5.Display.display(0, 0, 540, 960);
    M5.Display.waitDisplay();          // 等波形跑完，免得和紧接着的局部刷新打架
}

inline void show(Page& on) {
    Page* off = g_ctx.cur;
    auto& w = g_ctx.app->world;

    if (g_overlay_on) {                       // 切页 = 弹窗自动收起
        for (int i = 0; i < g_overlay.n; i++) w.remove<Visible>(g_overlay.e[i]);
        g_overlay_on = false;
        systems::g_band_y = -1;               // 整页都要换，不需要再单独还原那条带
    }

    if (off && off != &on) {
        for (int i = 0; i < off->n; i++) w.remove<Visible>(off->e[i]);   // 藏起来 = 摘掉 Visible
        if (off->on_leave) off->on_leave();
    }
    // ★ 顺序：先「当前页」认到新页、再统一挂 Visible/Dirty、**最后**才让本页收尾。
    //
    //   反过来（先 on_enter 再统一挂）有两个连锁毛病：
    //   ① 本页在 on_enter 里**摘掉**的 Visible 会被下面这句无条件挂回来 ——
    //      页面自己管可见性就管不住了。
    //   ② on_enter 里问「我是不是当前页」也问不出来（那时 g_ctx.cur 还是旧页）。
    //   Dirty 早挂晚挂不影响：渲染是在 show() 返回之后才跑的。
    g_ctx.cur = &on;
    for (int i = 0; i < on.n; i++) { w.add(on.e[i], Visible{}); w.add(on.e[i], Dirty{}); }
    if (on.on_enter) on.on_enter();          // 本页收尾（自己标脏、按自己状态摆可见性）

    if (++_since_wash() >= FULL_EVERY) {     // 每 N 次切页清一次重影
        _since_wash() = 0;
        quality_wash();
    }
}

// ── 返回栈：从哪开返哪（不硬编码回主页）──
static constexpr int MAX_BACK = 8;
inline Page* g_back[MAX_BACK];
inline int   g_back_n = 0;

inline void push(Page& on) {                     // 跳转：当前页入栈，再切页
    Page* cur = g_ctx.cur;
    if (cur && cur != &on && g_back_n < MAX_BACK) g_back[g_back_n++] = cur;
    show(on);
}
inline void back() {                             // 返回：弹栈回上一页（栈空回 applist）
    if (g_back_n > 0) show(*g_back[--g_back_n]);
    else              show(g_launcher);
}
inline void home() {                             // 回首页：清栈 + 回 applist（单独按钮）
    g_back_n = 0;
    show(g_launcher);
}

} // namespace m5ui::pages
