#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>

namespace m5ui::ecs {

// ──── Entity ────
// 就是一个 uint16_t 编号。没有任何"类型"。
using Entity = uint16_t;
constexpr Entity INVALID_ENTITY = 0xFFFF;

// ──── Component Type ID ────
// 每种组件类型有一个唯一编号（编译期自动分配）
using ComponentType = uint8_t;
constexpr size_t MAX_COMPONENT_TYPES = 32;

// ──── Component ID 分配器 ────
// 每次 T::type() 被首次调用时分配一个递增 ID
inline ComponentType _next_type_id() {
    static ComponentType id = 0;
    return id++;
}

// ──── TypeTrait：获取 T 的唯一 ComponentType ────
template <typename T>
inline ComponentType type_id() {
    static const ComponentType id = _next_type_id();
    return id;
}

// ──── Bitset：记录一个 Entity 有哪些组件 ────
struct ComponentMask {
    uint32_t bits = 0;
    inline void set(ComponentType t) { bits |= (1u << t); }
    inline void unset(ComponentType t) { bits &= ~(1u << t); }
    inline bool has(ComponentType t) const { return bits & (1u << t); }
    inline bool has_all(ComponentMask other) const {
        return (bits & other.bits) == other.bits;
    }
};

} // namespace m5ui::ecs