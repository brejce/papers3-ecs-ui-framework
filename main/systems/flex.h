#pragma once

#include "ecs/world.hpp"
#include "ecs/components.h"
#include <M5Unified.h>

namespace m5ui::systems {

// ──────────────────────────────────────────────
// flex_system — Flexbox 布局
//
// 遍历有 FlexLayout 的 Entity，根据其子节点（parent → children 关系）
// 的 FlexItem.grow 和 Rect 尺寸，重新计算子节点位置。
// ──────────────────────────────────────────────

// 需要 Parent/Child 组件来建立树关系
// 这里用 Entity 数组存 children

namespace flex_detail {

struct Children {
    ecs::Entity entities[16];
    uint8_t count = 0;
};

struct Parent {
    ecs::Entity parent;
};

} // namespace flex_detail

inline void flex_layout(ecs::World& w) {
    using namespace ecs::components;
    using namespace flex_detail;

    // 遍历所有有 FlexLayout 的 Entity
    for (auto e : w.query<FlexLayout, Rect>()) {
        auto* layout = w.get_mut<FlexLayout>(e);
        auto* container = w.get<Rect>(e);
        if (!layout || !container) continue;

        // 收集子节点
        auto* children = w.get_mut<Children>(e);
        if (!children || children->count == 0) continue;

        bool is_row = (layout->direction == FlexLayout::ROW);
        int32_t cx = container->x + layout->pad_left;
        int32_t cy = container->y + layout->pad_top;
        int32_t cw = container->w - layout->pad_left - layout->pad_right;
        int32_t ch = container->h - layout->pad_top - layout->pad_bottom;

        int32_t main_size  = is_row ? cw : ch;
        int32_t cross_size = is_row ? ch : cw;

        // 收集固定尺寸和 grow
        int32_t fixed_total = 0;
        int32_t grow_total  = 0;
        int32_t n = children->count;

        for (int i = 0; i < n; i++) {
            auto& child = children->entities[i];
            auto* item = w.get<FlexItem>(child);
            auto* rect = w.get<Rect>(child);
            if (!rect) continue;

            if (item && item->grow > 0) {
                grow_total += item->grow;
            } else {
                fixed_total += is_row ? rect->w : rect->h;
            }
        }

        // ★ gap 只在本次计算里用，不回写 layout->gap：
        // 回写会导致「gap 每帧变一点 → 子节点坐标每帧都变 → 每帧都刷屏」
        int32_t gap = layout->gap;
        int32_t gap_total = gap * (n > 0 ? n - 1 : 0);
        int32_t remaining = main_size - fixed_total - gap_total;
        if (remaining < 0) remaining = 0;

        // 计算每个子节点的主轴尺寸
        int32_t pos = cx;  // 将根据 justify 调整

        // 简化版 justify：START / CENTER / SPACE_BETWEEN
        int32_t offset = 0;
        switch (layout->justify) {
            case FlexLayout::JUSTIFY_START:   offset = 0; break;
            case FlexLayout::JUSTIFY_CENTER:  offset = remaining / 2; break;
            case FlexLayout::JUSTIFY_END:     offset = remaining; break;
            case FlexLayout::SPACE_BETWEEN:
                offset = 0;
                if (n > 1) {
                    gap = remaining / (n - 1);   // SPACE_BETWEEN 下 gap 由剩余空间决定
                }
                break;
            case FlexLayout::SPACE_AROUND: {
                gap = remaining / n;
                offset = gap / 2;
                break;
            }
            case FlexLayout::SPACE_EVENLY: {
                gap = remaining / (n + 1);
                offset = gap;
                break;
            }
        }

        if (is_row)
            pos = cx + offset;
        else
            pos = cy + offset;

        // 设置子节点位置 —— 只有坐标真的变了才标脏
        for (int i = 0; i < n; i++) {
            auto& child = children->entities[i];
            auto* rect = w.get_mut<Rect>(child);
            auto* item = w.get<FlexItem>(child);
            if (!rect) continue;

            int32_t m = is_row ? rect->w : rect->h;
            if (item && item->grow > 0 && grow_total > 0) {
                m = remaining * item->grow / grow_total;
            }

            int32_t c = is_row ? rect->h : rect->w;
            // 交叉轴对齐
            int32_t cross_off = 0;
            bool stretch = false;
            switch (layout->align) {
                case FlexLayout::FLEX_START:   cross_off = 0; break;
                case FlexLayout::FLEX_END:     cross_off = cross_size - c; break;
                case FlexLayout::ALIGN_CENTER: cross_off = (cross_size - c) / 2; break;
                case FlexLayout::STRETCH:      cross_off = 0; stretch = true; break;
            }

            int32_t nx = rect->x, ny = rect->y, nw = rect->w, nh = rect->h;
            if (is_row) {
                nx = pos;
                ny = cy + cross_off;
                if (item && item->grow > 0) nw = m;
                if (stretch) nh = cross_size;
            } else {
                nx = cx + cross_off;
                ny = pos;
                if (item && item->grow > 0) nh = m;
                if (stretch) nw = cross_size;
            }

            // ★ 只有真的变了才挂 Dirty —— 每帧无条件标脏会让墨水屏疯狂刷新
            if (nx != rect->x || ny != rect->y || nw != rect->w || nh != rect->h) {
                rect->x = nx; rect->y = ny; rect->w = nw; rect->h = nh;
                w.add(child, Dirty{});
            }

            pos += m + gap;
        }
    }
}

} // namespace m5ui::systems