#pragma once

// ui/candidate_bar.h — 候选栏组件（终端风：无边框，点阵底 + 透明中文）
//
// 照 keyboard.h 的架构：一个容器实体 + CustomDraw 自绘全部 + 自持触摸轮询。
// 候选字直接画在点阵底上：ttf::draw_text 本来就是「读底合成」，字形没墨处透出点阵，
// 所以这里**不铺白底、不画框**，整条候选栏就是点阵 + 中文字形 + 两个翻页三角。
//
// 刷新纪律（2026-10-17 用户「候选区还是一整个刷新的，要按字形绘制」）：
//   - 覆盖层开着、带已推完（g_band_y < 0）→ refresh() 逐格对比，只重画**变了的那一格**
//     （擦成点阵 + 画新字形 + 只推这一格），不再整条带重推。
//   - 开机 / 覆盖层正在开（带内）→ 只 mark_dirty，整块交给渲染器一次画完（省得画两遍）。

#include <cstdint>
#include <cstring>
#include "ecs/world.hpp"
#include "ecs/components.h"
#include "ui/icons.h"
#include "text/ime.h"          // cp_to_utf8
#include "text/ttf.h"
#include "systems/render.h"    // apply_epd_mode / epd_push / g_band_y

namespace papers3::ui::cb {

using ecs::Entity;
using namespace papers3::ecs::components;
using namespace papers3::ui::widgets;

inline constexpr int MAX_CELLS = 8;
inline constexpr uint32_t CB_DOT = 0xE8E8E8;   // 点阵底色（FAST 档 → Bayer 点阵）

struct Bar;

inline Bar* g_inst = nullptr;

inline void cb_draw_thunk(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t fg, uint32_t bg);

struct Bar {
    char   buf[MAX_CELLS][8];       // 每格 UTF-8 串
    int    n = 0;                   // 当前候选数
    int    aw = 30, cw = 50, gap = 5;
    int32_t x0 = 10, y = 0, h = 56, w_avail = 520;
    int32_t prev_x = 0, next_x = 0, cell_x[MAX_CELLS] = {0};

    Entity (*track)(Entity) = nullptr;
    void (*on_pick)(void*) = nullptr;   // 点了第 i 格，ctx = i
    void (*on_page)(void*) = nullptr;   // 点了箭头，ctx = -1 / +1
    ecs::World* world = nullptr;
    Entity self = ecs::INVALID_ENTITY;
    int8_t  pressed_i = -1;            // -2 上一页 / -3 下一页 / 0..7 格 / -1 无

    // 建：在 [x0, x0+w_avail] 里横向居中排一条（几何与旧版一致）
    void build(ecs::World& w, int32_t y0, int32_t h0, int32_t x0v, int32_t w_avail_v,
               int32_t awv = 30, int32_t cwv = 50, int32_t gapv = 5) {
        world = &w; y = y0; h = h0; x0 = x0v; w_avail = w_avail_v;
        aw = awv; cw = cwv; gap = gapv;
        int total = aw * 2 + MAX_CELLS * cw + (MAX_CELLS + 1) * gap;
        int x = x0 + (w_avail - total) / 2;
        prev_x = x; x += aw + gap;
        for (int i = 0; i < MAX_CELLS; i++) { cell_x[i] = x; x += cw + gap; }
        next_x = x;

        self = w.spawn();
        w.add(self, Visible{});
        w.add(self, Rect{x0, y, w_avail, h});
        w.add(self, Style{0, CB_DOT, 0, 0, 0, 0, 0});   // 无框无影；底 = 点阵（渲染器擦成点阵）
        w.add(self, CustomDraw{cb_draw_thunk});
        w.add(self, EPDMode{3});                        // 点阵底 FAST（与终端整屏底同档；FASTEST 实测出黑）
        w.add(self, Dirty{});
        if (track) track(self);
        g_inst = this;
    }

    void mark_dirty() {
        if (world && self != ecs::INVALID_ENTITY) world->add(self, Dirty{});
    }

    bool shown() const {
        return world && self != ecs::INVALID_ENTITY && world->has<Visible>(self);
    }

    // 摆内容：逐格对比，只重画变了的那一格（局部刷新，见文件头注释）
    void refresh(ecs::World& w, const uint16_t* cps, int n0) {
        (void)w;
        n = n0 < MAX_CELLS ? n0 : MAX_CELLS;
        const bool partial = shown() && systems::g_band_y < 0;
        for (int i = 0; i < MAX_CELLS; ++i) {
            char s[8] = {0};
            if (i < n) { int k = papers3::ime::cp_to_utf8(cps[i], s); s[k] = 0; }
            if (std::strcmp(buf[i], s) == 0) continue;   // 没变 → 不刷
            std::strcpy(buf[i], s);
            if (partial) redraw_cell(i);                 // 打字中：只推这一格
        }
        if (!partial) mark_dirty();                      // 开机/带内：整块交给渲染一次画完
    }

    // 只画某一格的字形进帧缓冲（不推屏）。带内由渲染器整块推，这里不推。
    void draw_cell_glyph(int i) {
        if (i < 0 || i >= MAX_CELLS || !buf[i][0]) return;
        systems::apply_epd_mode(2);                  // 文字：quality（中文抗锯齿，读底合成透点阵）
        const float keep = papers3::ttf::size_px();
        papers3::ttf::set_size((float)KB_FONT_PX);
        int32_t tw = 0, th = 0;
        papers3::ttf::measure(buf[i], &tw, &th);
        M5.Display.startWrite();
        papers3::ttf::draw_text(cell_x[i] + (cw - tw) / 2, y + (h - th) / 2,
                             buf[i], 0x000000, 0);
        M5.Display.endWrite();
        papers3::ttf::set_size(keep);
    }

    // 局部重画一格：擦成点阵 + 画新字形 + 只推这一格（打字中候选变了的走这里）
    void redraw_cell(int i) {
        if (!world || self == ecs::INVALID_ENTITY) return;
        systems::apply_epd_mode(3);                  // FAST：擦成点阵（与容器底同档）
        M5.Display.startWrite();
        M5.Display.fillRect(cell_x[i], y, cw, h, CB_DOT);
        M5.Display.endWrite();
        draw_cell_glyph(i);
        systems::epd_push(cell_x[i], y, cw, h);      // 只推这一个格
    }

    // 命中：-2 上一页 / -3 下一页 / 0..7 格 / -1 无
    int hit(int32_t px, int32_t py) const {
        if (py < y || py >= y + h) return -1;
        if (px >= prev_x && px < prev_x + aw) return -2;
        if (px >= next_x && px < next_x + aw) return -3;
        for (int i = 0; i < MAX_CELLS; ++i)
            if (px >= cell_x[i] && px < cell_x[i] + cw) return i;
        return -1;
    }

    void fire(int i) {
        if (i == -2) { if (on_page) on_page((void*)(intptr_t)-1); }
        else if (i == -3) { if (on_page) on_page((void*)(intptr_t)+1); }
        else if (i >= 0 && i < MAX_CELLS) { if (on_pick) on_pick((void*)(intptr_t)i); }
    }

    // 自持触摸轮询（与键盘同一套路，各查各的几何，不抢）
    void tick() {
        if (!world || self == ecs::INVALID_ENTITY) { pressed_i = -1; return; }
        if (!world->has<Visible>(self)) { pressed_i = -1; return; }
        if (M5.Touch.getCount() == 0) {
            if (pressed_i != -1) { fire(pressed_i); pressed_i = -1; }
            return;
        }
        auto& d = M5.Touch.getDetail(0);
        if (d.wasPressed()) {
            const int i = hit(d.x, d.y);
            if (i != -1) pressed_i = (int8_t)i;
        } else if (d.wasReleased() && pressed_i != -1) {
            fire(pressed_i);
            pressed_i = -1;
        }
    }
};

inline void cb_draw_thunk(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t fg, uint32_t bg) {
    (void)x; (void)w; (void)fg; (void)bg;
    Bar* b = g_inst;
    if (!b) return;
    // 点阵底已由渲染器擦好（Style.bg=CB_DOT + EPDMode{3}）—— 这里只画字和箭头
    for (int i = 0; i < MAX_CELLS; ++i) b->draw_cell_glyph(i);
    systems::apply_epd_mode(2);                  // 翻页三角（无框，画在点阵上）
    icons::arrow_left(b->prev_x, b->y, b->aw, b->h, 0x000000, 0);
    icons::arrow_right(b->next_x, b->y, b->aw, b->h, 0x000000, 0);
}

inline void tick_g() { if (g_inst) g_inst->tick(); }

} // namespace papers3::ui::cb
