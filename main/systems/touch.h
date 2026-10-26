#pragma once

#include "ecs/world.hpp"
#include "ecs/components.h"
#include "esp_timer.h"
#include "plugin.h"      // plugin::active() —— .so app 独占时固件不参与命中检测
#include <M5Unified.h>

namespace papers3::systems {

// ──────────────────────────────────────────────
// touch_system — 触摸事件分发
//
// 1. 从 M5.Touch 读触摸状态
// 2. 对 (Rect + Touchable + Visible) 的 Entity 做命中检测
// 3. 松手触发 OnClick；挂了 OnDoubleClick 的实体走双击判定
//
// 注意：M5Unified 的 touch_state_t 是位标志：
//   touch=0b0001 mask_touch | touch_end=0b0010 mask_change | touch_begin=0b0011
//   hold=0b0101 | hold_end=0b0110 | hold_begin=0b0111 | flick_end=0b1010 ...
// 长按（>500ms）松手时状态是 hold_end(0b0110)，**不等于** touch_end(0b0010)。
// 所以绝不能用 `state == touch_end` 判松手，否则长按后 Pressed 永远不清、按钮卡在按下态。
// 用库自带的 wasPressed()/wasReleased()：wasReleased 覆盖 touch_end/hold_end/flick_end/drag_end。
//
// 双击（OnDoubleClick）：同一实体在 DOUBLE_MS 内被点第二下算双击。
// 挂了双回的实体，第一下**不立刻**触发 OnClick，而是欠着；等窗口过期（每帧检查）才补发单次。
// 这样双击不会顺带点出一次单击 —— 代价是单击晚 DOUBLE_MS 生效。
// ──────────────────────────────────────────────

inline uint32_t now_ms() { return (uint32_t)(esp_timer_get_time() / 1000); }

inline void handle_touch(ecs::World& w) {
    using namespace ecs::components;

    // ★ .so app 独占屏幕时，固件这一侧不参与命中检测。
    //   理由：两边各有一套"按下沿"判定（固件靠 wasPressed()，.so 靠 so_touch），
    //   而且两边看的是**同一只手指** —— 不挡的话，app 里点一下，固件这边
    //   也会在同一帧按坐标找到某个仍在 Visible 的实体触发一次（实测"乱跑"）。
    //   app 的交互全权交给 .so 的 page_tick；固件页面的实体这时候也都摘了 Visible。
    if (plugin::active()) return;

    static constexpr uint32_t DOUBLE_MS = 400;

    static ecs::Entity _touch_target = ecs::INVALID_ENTITY;   // 这次按下命中的实体
    static ecs::Entity _pending      = ecs::INVALID_ENTITY;   // 欠着的那次单击
    static uint32_t    _pending_ms   = 0;

    // 松开：清按下态 + 标脏 + 派发回调（幂等，靠 _touch_target 保证一次按下只派发一次）
    auto release = [&]() {
        ecs::Entity e = _touch_target;
        if (e == ecs::INVALID_ENTITY) return;
        _touch_target = ecs::INVALID_ENTITY;
        w.remove<Pressed>(e);
        w.add(e, Dirty{});

        if (auto* d2 = w.get<OnDoubleClick>(e); d2 && d2->fn) {
            if (_pending == e && (now_ms() - _pending_ms) <= DOUBLE_MS) {
                _pending = ecs::INVALID_ENTITY;       // 第二下到了 → 双击
                d2->fn(d2->ctx);
            } else {
                _pending    = e;                      // 第一下 → 先欠着
                _pending_ms = now_ms();
            }
            return;
        }
        if (auto* cb = w.get<OnClick>(e); cb && cb->fn) cb->fn(cb->ctx);
    };

    // 双击窗口过期 → 把欠的那次单击补上（每帧检查）
    if (_pending != ecs::INVALID_ENTITY && (now_ms() - _pending_ms) > DOUBLE_MS) {
        ecs::Entity e = _pending;
        _pending = ecs::INVALID_ENTITY;
        if (w.has<Visible>(e)) {                      // 页已经切走了就别补发
            if (auto* cb = w.get<OnClick>(e); cb && cb->fn) cb->fn(cb->ctx);
        }
    }

    // 兜底：没有触摸点却还留着按下目标 → 清掉（防止残留卡住）
    if (M5.Touch.getCount() == 0) { release(); return; }

    auto& d = M5.Touch.getDetail(0);

    // 按下沿：命中检测（wasPressed == state == touch_begin）
    if (d.wasPressed()) {
        _touch_target = ecs::INVALID_ENTITY;
        for (auto e : w.query<Rect, Touchable, Visible>()) {
            auto* r = w.get<Rect>(e);
            if (d.x >= r->x && d.x <= r->x + r->w &&
                d.y >= r->y && d.y <= r->y + r->h) {
                _touch_target = e;
                w.add(e, Pressed{true});
                w.add(e, Dirty{});
                break;  // 只触发最上层一个
            }
        }
    }

    // 松开沿：wasReleased() 同覆盖 touch_end / hold_end / flick_end / drag_end
    if (d.wasReleased()) release();
}

} // namespace papers3::systems
