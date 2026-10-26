#pragma once

#include "world.h"
#include <vector>
#include <array>
#include <algorithm>
#include <cstring>

namespace papers3::ecs {

// ──────────────────────────────────────────────
// World — 实体管理器
//
// Entity  = uint16_t 编号
// Component = 任意 POD / struct，按类型存连续数组
//
// 核心操作：
//   spawn()                   → 新建 Entity
//   add<T>(entity, T{})       → 给 Entity 挂一个 T 组件
//   get<T>(entity)            → 读 T
//   get_mut<T>(entity)        → 写 T
//   remove<T>(entity)          → 摘掉 T
//   query<T1, T2, ...>()      → 返回所有同时有这些组件的 Entity
// ──────────────────────────────────────────────

class World {
public:
    static constexpr size_t MAX_ENTITIES = 256;

    World() {
        _masks.fill({});
        _alive.fill(false);
    }

    // ── Entity 生命周期 ──

    Entity spawn() {
        if (_next >= MAX_ENTITIES) {
            // ★ 实体编号**不复用**（destroy 只清 alive，不回收到池子里）→ 一旦越界，
            //   之后所有新控件都会静默拿不到实体（add() 对非法实体是 no-op），
            //   表现是"新加的控件就是不出现"，而且串口一行日志都没有。
            static bool warned = false;
            if (!warned) {
                warned = true;
                ESP_LOGE("ECS", "实体表满（%d）—— 之后所有 spawn() 都会失败", (int)MAX_ENTITIES);
            }
            return INVALID_ENTITY;
        }
        Entity e = _next++;
        _alive[e] = true;
        _masks[e] = {};
        return e;
    }

    void destroy(Entity e) {
        if (e >= MAX_ENTITIES || !_alive[e]) return;
        _alive[e] = false;
        _masks[e] = {};
        // 组件数据不单独清——下次复用同类型时自然覆盖
    }

    bool is_alive(Entity e) const {
        return e < MAX_ENTITIES && _alive[e];
    }

    // ── 组件操作 ──

    template <typename T>
    void add(Entity e, T component) {
        if (!is_alive(e)) return;
        ComponentType tid = type_id<T>();
        _store<T>()[e] = std::move(component);
        _has<T>()[e] = true;
        _masks[e].set(tid);
    }

    template <typename T>
    void remove(Entity e) {
        if (!is_alive(e)) return;
        ComponentType tid = type_id<T>();
        _has<T>()[e] = false;
        _masks[e].unset(tid);
    }

    template <typename T>
    bool has(Entity e) const {
        if (!is_alive(e)) return false;
        return _masks[e].has(type_id<T>());
    }

    template <typename T>
    const T* get(Entity e) const {
        if (!has<T>(e)) return nullptr;
        return &_store<T>()[e];
    }

    template <typename T>
    T* get_mut(Entity e) {
        if (!has<T>(e)) return nullptr;
        return &_store<T>()[e];
    }

    // ── 查询：返回所有有指定组件集的 Entity ──

    template <typename T>
    std::vector<Entity> query() {
        std::vector<Entity> result;
        ComponentMask need;
        need.set(type_id<T>());
        for (Entity e = 0; e < _next; e++) {
            if (_alive[e] && _masks[e].has_all(need))
                result.push_back(e);
        }
        return result;
    }

    template <typename T1, typename T2, typename... Rest>
    std::vector<Entity> query() {
        std::vector<Entity> result;
        ComponentMask need;
        need.set(type_id<T1>());
        need.set(type_id<T2>());
        (need.set(type_id<Rest>()), ...);
        for (Entity e = 0; e < _next; e++) {
            if (_alive[e] && _masks[e].has_all(need))
                result.push_back(e);
        }
        return result;
    }

    size_t count() const { return _next; }

private:
    // ── 类型擦除的组件存储 ──
    // 每种组件类型有独立数组和 has-flag 数组

    template <typename T>
    static std::array<T, MAX_ENTITIES>& _store() {
        static std::array<T, MAX_ENTITIES> arr{};
        return arr;
    }

    template <typename T>
    static std::array<bool, MAX_ENTITIES>& _has() {
        static std::array<bool, MAX_ENTITIES> arr{};
        return arr;
    }

    Entity _next = 0;
    std::array<bool, MAX_ENTITIES> _alive;
    std::array<ComponentMask, MAX_ENTITIES> _masks;
};

} // namespace papers3::ecs