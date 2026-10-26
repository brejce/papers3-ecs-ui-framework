#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>
#include "esp_log.h"

namespace papers3::ecs {

// ──── Entity ────
// 就是一个 uint16_t 编号。没有任何"类型"。
using Entity = uint16_t;
constexpr Entity INVALID_ENTITY = 0xFFFF;

// ──── Component Type ID ────
// 每种组件类型有一个唯一编号（编译期自动分配）
using ComponentType = uint8_t;
constexpr size_t MAX_COMPONENT_TYPES = 64;   // = ComponentMask 的位数（world.h）

// ──── Component ID 分配器 ────
// 每次 T::type() 被首次调用时分配一个递增 ID
inline ComponentType _next_type_id() {
    static ComponentType id = 0;
    const ComponentType cur = id++;
    // ★ 越界只能在这里抓到：掩码是定宽整数，第 (位数+1) 种类型会让位移变成 UB，
    //   而症状是"某个组件的 has()/query() 结果乱掉"，极难反查。
    if (cur >= MAX_COMPONENT_TYPES) {
        ESP_LOGE("ECS", "组件类型数超过 %d（正在分配第 %d 种）→ 掩码会溢出，见 world.h 的 ComponentMask",
                 (int)MAX_COMPONENT_TYPES, (int)cur + 1);
    }
    return cur;
}

// ──── TypeTrait：获取 T 的唯一 ComponentType ────
template <typename T>
inline ComponentType type_id() {
    static const ComponentType id = _next_type_id();
    return id;
}

// ──── Bitset：记录一个 Entity 有哪些组件 ────
// ★ 64 位而不是 32：项目在鼓励「加新功能 = 加新组件」，32 个的余量太薄，
//   第 33 种类型会静默变成 UB（1u << 32）。
struct ComponentMask {
    uint64_t bits = 0;
    inline void set(ComponentType t)   { bits |= (1ull << (t & 63)); }
    inline void unset(ComponentType t) { bits &= ~(1ull << (t & 63)); }
    inline bool has(ComponentType t) const { return (bits & (1ull << (t & 63))) != 0; }
    inline bool has_all(ComponentMask other) const {
        return (bits & other.bits) == other.bits;
    }
};

} // namespace papers3::ecs