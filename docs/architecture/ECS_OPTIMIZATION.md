# ECS 实体/组件优化手册（papers3）

> 编写 2026-10-08。**所有数字都是从目标 ELF 实测**（`nm --size-sort -C build/papers3.elf`），
> 不是在开发机上量 `sizeof` —— 两者会差一倍（ESP32 指针 4B，开发机 8B）。
> 适用范围：`~/Project/papers3/main/ecs/`。
> 配套文档：`../guide/DEVELOPER.md`（ECS 架构在其附录）、`../display/EPD_TEST_MANUAL.md`。

---

## 零、结论摘要

| 问题 | 答案 |
|---|---|
| 256 是什么 | `ecs/world.hpp:28` 的 `MAX_ENTITIES = 256`，**手选常数**，不是硬件限制 |
| 32 是什么 | `ComponentMask` 是 `uint32_t` 位图 → 组件类型上限 32（已用 21）。**这个才是真·架构限制** |
| 实体能"用到才进内存"吗 | **现在完全不能。** 成本 = `Σ(被引用的组件类型) × MAX_ENTITIES × (sizeof(T)+1)`，**开机就常驻**（见第三节） |
| 溢出会怎样 | **静默失败**：`spawn()` 返回 `INVALID_ENTITY`，`add<T>()` 空操作 → **控件不出现，无日志** |
| ECS 存储实际多大 | **46.0 KB**（内部 RAM / `.bss`），占 `libmain.a` 的 `.bss`（99,311 B）约 47% |
| 最大单项 | **flex 四件套 = 16.0 KB = ECS 存储的 35%**，而全项目只有**主页两行按钮**在用（见 §1.5） |
| 最能省的几刀 | ① 处理 flex（−16.0KB 或 −8.6KB）② `Style` 字段收窄 −4.0KB ③ `MAX_ENTITIES 256→224` −5.8KB → **合计 ≈ −26 / −18 KB** |

---

## 一、实测数据

### 1.1 存储布局（`ecs/world.hpp`）

```cpp
Entity spawn() {                                  // :37  单调递增，只 _next++
    if (_next >= MAX_ENTITIES) return INVALID_ENTITY;
    Entity e = _next++;
    ...
}
void destroy(Entity e) { _alive[e] = false; }     // :45  只标死，槽位不回收

template <typename T>
static std::array<T, MAX_ENTITIES>& _store() {    // :128 ★ 每类型一个 256 槽数组
    static std::array<T, MAX_ENTITIES> arr{};     //    函数局部 static → .bss → 开机常驻
    return arr;
}
template <typename T>
static std::array<bool, MAX_ENTITIES>& _has() { ... }   // :134 每类型再加 256 B
```

- `_store<T>()` 是**函数局部 static**，零初始化 → 放 `.bss` → **加载时就分配好，不是第一次调用时才分配**。
- 所以"惰性"只体现在**有没有被实例化**（代码里有没有引用 `add<T>/get<T>/has<T>`），不体现在运行时。
- 落点是**内部 RAM**：`CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY` **未开**。

### 1.2 实测：ECS 组件存储合计 = **46.0 KB**

`nm --size-sort -C build/papers3.elf | grep -E '_store<|_has<'`（**注意 `--size-sort` 输出是十六进制**）：

| 类型 | 字节/实体 | × 256 |
|---|---|---|
| `Children`（`flex_detail`） | 34 | **8,704** |
| `Style` | 28 | **7,168** |
| `FlexLayout` | 24 | **6,144** |
| `Rect` | 16 | **4,096** |
| `TextBox` | 12 | 3,072 |
| `Cursor` | 12 | 3,072 |
| `OnDoubleClick` | 8 | 2,048 |
| `OnClick` | 8 | 2,048 |
| `Touchable` / `TextDirtyRange` / `Text` / `FlexItem` / `CustomDraw` | 4 | 各 1,024 |
| `Parent`（`flex_detail`） | 2 | 512 |
| `Pressed` / `EPDMode` | 1 | 各 256 |
| **`_store` 小计（16 个类型）** | | **42,496** |
| **`_has` 小计（18 个类型 × 256 B）** | | **4,608** |
| **★ 合计** | **184 B/实体** | **47,104 B = 46.0 KB** |

对照：`libmain.a` 的 `.bss` = **99,311 B**（≈97KB）→ **ECS 存储占了其中约 47%**。

> **而且 `libmain.a` 几乎就是 papers3 的全部静态内部 RAM** —— 同一次 `idf.py size-components` 里
> `libm5stack__m5gfx.a` 的 `.bss` 只有 **432 B**，其余组件都是 0。
> 也就是说：**papers3 的静态内部 RAM ≈ 97KB，其中 ECS 占 46KB（47%）。**
> 想省内部 RAM，ECS 是唯一值得动的地方。

### 1.3 两个反直觉的实测结论

**① 定义了但没人引用的组件类型，成本是 0 —— 所以"删掉没用的类型省内存"是错的。**

`OnLongPress / Battery / Progress / Interval / Scroll` 在 papers3 里 **0 处引用**，
它们的 `_store<T>` **根本没被实例化**，不进 `.bss`。**别按"删类型"去优化。**

**② 标记型组件（空结构）只有 `_has` 的 256 B，没有 `_store`。**

`Visible` / `Dirty` / `LeftAlign` 的 `_store<T>::arr` 符号**在 ELF 里不存在** ——
因为它们的值从不被读取（渲染只用 `has<T>()`，读的是 `_masks`），
写进去的值是死代码，被编译器/链接器 **DCE** 掉了。所以每个只花 **256 B**。

### 1.4 各部件实际挂的组件

| 工厂 | 组件 | 种数 |
|---|---|---|
| `rect()` | Visible, Rect, Style, EPDMode, Dirty | 5 |
| `label()` | 上面 + Text, TextBox | 7 |
| `button()` | 上面 + Touchable, OnClick | 8 |

**利用率**：18 个被实例化的类型 × 256 槽 = 4,608 槽；实体平均挂 ~6.5 种 × 256 = ~1,664 槽 →
**约 36%**。**矩阵 64% 是洞** —— 这是浪费的真正来源，不是"尾部没用满"。

### 1.5 ⚠️ flex 的成本审计：16.0 KB（ECS 存储的 35%）为一页服务

```
FlexLayout 6,144 + Children 8,704 + FlexItem 1,024 + Parent 512 = 16,384 B = 16.0 KB
```

实测它的**全部使用者**：

| 项 | 事实 |
|---|---|
| `widgets.h` 的 `flex()` 工厂（:288） | **0 个外部调用者**（只有定义，没人用） |
| `Children` 的实际最大孩子数 | **3**（全项目 `add_child` 只有 5 处，都在 `pages/demo.h`） |
| `flex_layout()` | `app.h:53` **每帧**调一次，扫全部实体 |
| 用它排版的地方 | **只有 `pages/demo.h`**（主页）两行按钮，每行 2~3 个 |
| settings / wifi / reader / ime | **全是手写 x/y 坐标，不用 flex** |

**也就是说：16 KB 是为「主页两行共 5 个按钮的排列」付的。**
`Children::entities[16]` 里 16 个槽是给"最多 3 个孩子"预留的 —— 浪费 81%。

---

## 二、三个上限

| 上限 | 位置 | 性质 | 改动代价 |
|---|---|---|---|
| **256 实体** | `world.hpp:28` | 手选常数；`destroy()` 不回收槽位 → 是**累计**额度 | 改常数，成本 = 类型数 × ΔN × (sizeof+1) |
| **32 组件类型** | `world.h:17` + `ComponentMask{uint32_t}` | **真架构限制**（位图位宽） | 要改 `ComponentMask` 位宽，波及 `set/unset/has` |
| **槽位不回收** | `world.hpp:37-50` | 设计选择（papers3 从不 `destroy()`，切页只切 `Visible`） | ~10 行加空闲栈 |

### 溢出的静默失败（最坑）

```cpp
Entity spawn() {
    if (_next >= MAX_ENTITIES) return INVALID_ENTITY;   // 0xFFFF
    ...
}
template <typename T> void add(Entity e, T c) {
    if (!is_alive(e)) return;      // is_alive(0xFFFF) == false → 静默什么都不做
    ...
}
```

**结果：控件根本不出现，没有报错、没有日志。** 表现是"界面少了一块"，几乎不可能联想到额度用完。

---

## 三、核心问题：实体/组件能不能"用到才进内存"？

**结论：现在完全不能。** 成本是 `Σ(被引用的类型) × MAX_ENTITIES × (sizeof+1)`，
在**加载时**就全部常驻，**跟运行时建了多少实体、有多少实体挂了该组件，一律无关**。
（实测：同一份代码，只引用 2 种组件类型 → `.bss` 小 44KB。）

"惰性"分四层，成本和收益差一个数量级：

| 层 | 现状 | 能懒吗 | 能省 | 代价 |
|---|---|---|---|---|
| ① **组件类型存储** | 引用即分配 256 槽 | **不能**靠"运行时用到才给"来省 | — | — |
| ② **实体槽位** | `_next` 单调不回收 | **能**（空闲栈） | 不改内存，改**语义**（累计→同时存活） | ~10 行；配合"页面按需建/销毁"会**牺牲切页速度** |
| ③ **槽位字段宽度** | 大量 `int32`/`uint32` 存小值 | **能** | **~5 KB**（见 P2） | 改结构体字段类型，改调用点 |
| ④ **组件数据矩阵** | 18 类型 × 256 槽，**64% 是洞** | **能，但要换存储模型** | **~30 KB**（46 → ~16KB） | 需要显式组件类型清单，属重构 |

### 为什么第 ④ 层省得最多，也最难

因为浪费不在"尾部没用满"，而在**矩阵太稀疏**：实体平均只挂 6.5 / 18 种类型。
把 256 降到 224 只省 12%；**只有换成"每个实体只占它真正挂的那些组件"的布局，才能省掉那 64% 的洞。**

卡点：

```cpp
// world.h:21-31  —— 类型 ID 是【运行时】惰性分配的
inline ComponentType _next_type_id() { static ComponentType id = 0; return id++; }
template <typename T> inline ComponentType type_id() { static const ComponentType id = _next_type_id(); return id; }
```

没有**编译期类型清单**，就算不出"第 k 种组件在一个实体里的字节偏移"，
存储只能退化成 type-major 稀疏矩阵。要真做第 ④ 层，先补一个显式 typelist：

```cpp
using Components = std::tuple<Rect, Style, Text, TextBox, Cursor, Touchable, OnClick,
                             EPDMode, Dirty, Visible, CustomDraw, FlexLayout, FlexItem, ...>;
constexpr size_t COMPONENT_TYPE_COUNT = std::tuple_size_v<Components>;
// type_id<T>() 改成 "T 在 tuple 里的下标"（编译期常量，不再运行时 ++）
```

有了编译期下标，才能在同一实体里紧凑排布它的组件（`base_offset[e]` + 偏移表），`get<T>()` 仍是 O(1)。

---

## 四、优化清单（按 收益 ÷ 成本 排序）

### P0 「键盘去实体化」—— ✅ 已做（2026-10-09）

屏幕键盘从 **35 个 ECS 实体（每键一个）** 收敛为 **1 个自持容器实体**：键面数据 `slot[35]` + 几何
`geo[35]` 由 `Keyboard` 结构体自己持有，容器只挂 `Rect + Style（白底无框无影）+ CustomDraw + EPDMode{2}`，
35 个键面由 `draw_all()` 一次画进帧缓冲、整块推送交给渲染器；触摸**不挂 `Touchable`**，改由 `app.h` 每帧
`ui::kb::tick_g()` 自轮询 `M5.Touch` 对 `geo[]` 算命中。输入法页 **53→19** 实体，全项目 **230→174**（省 56 实体）。
这是「去实体化」路线的样板 —— 自绘 + 自持，比按控件堆 ECS 实体省得多。

### P1 「让溢出可见」——**先做，零成本**

```cpp
// ecs/world.hpp
Entity spawn() {
    if (_next >= MAX_ENTITIES) {
        static bool warned = false;                 // 只吼一次，别刷屏
        if (!warned) { warned = true;
            std::printf("ECS: entity budget exhausted (%u) — widgets silently vanish\n",
                        (unsigned)MAX_ENTITIES);
        }
        return INVALID_ENTITY;
    }
    ...
}
```

顺手在建页处打每页增量，**才知道真实用量**：

```cpp
const size_t before = world.count();
build(w);
ESP_LOGI("ECS", "page %-10s +%u  (累计 %u / %u)", name,
         (unsigned)(world.count() - before), (unsigned)world.count(),
         (unsigned)ecs::World::MAX_ENTITIES);
```

### P2 「先问 flex 那 16.0 KB 值不值」——最大单项，属功能取舍

| 选择 | 省 | 代价 |
|---|---|---|
| **A. 主页那两行也改手写坐标**，然后删掉 flex 四件套 + `flex()` 工厂 + `app.h:53` 的 `flex_layout()` 调用 | **−16.0 KB** | 删一个功能。但 settings/wifi/reader 已证明手写坐标够用 |
| **B. 保留 flex，只收窄** | **−8.6 KB** | 无功能损失 |

B 的具体数字（当前最大孩子数 = **3**）：

| 改动 | 现在 | 改成 | 省 |
|---|---|---|---|
| `flex_detail::Children::entities[16]` | 34 B | `entities[4]` → 10 B | **6,144 B** |
| `FlexLayout` 的 `gap` + 4×`pad_*` | 5×int32 = 20 B | int16 → 10 B | **2,560 B** |

> ⚠️ `entities[4]` 是按"当前最大 3 个"定的。**先加一句运行期断言再定这个数**：
> `if (c.count >= 4) printf("flex: children overflow\n");`

### P3 「收窄其余字段宽度」——省 **~4.0 KB**，改结构体即可

| 改动 | 现在 | 改成 | 省 |
|---|---|---|---|
| `Style` 的 `radius`/`border_w`/`shadow` | 3×int32 = 12 B | uint8 → 3 B | **2,048 B** |
| `Style` 的 4 个颜色 `fg/bg/border/accent` | 4×uint32 = 16 B | uint16（RGB565）→ 8 B | **2,048 B** |

**两项合计 = −4,096 B = −4.0 KB**（逐项已按 ESP32 的对齐/padding 重算，
`Style` 两项有 padding 交互，**改一条量一条**，别一把改完才发现没省）。

前三条**只改结构体**（调用点的值本来就在小范围内）。**第四条要改调用点**：
现在传的是 `0xFFFFFF` 这种 24 位字面量，改 uint16 后必须传 RGB565（`TFT_WHITE`=0xFFFF 这类）。
**改前先确认 `[16]→[8]` 够用**（`Children` 是 flex 容器装孩子的地方，键盘一行 10 个，
所以 `[8]` 可能不够——量一下最大孩子数再定）。

### P4 「MAX_ENTITIES 按实测下调」——省 ~5.8 KB

1. 用 P1 的日志量出**峰值实体数**（不是估算）。
2. 设成 `峰值 + 余量`。从 256 → 224 省 **5.8 KB**；→ 192 省 **11.5 KB**。

**没有性能代价**（数组仍是 O(1) 下标），只是往上涨的余量变小。

### P5 「槽位回收」——只在你要改页面生命周期时才做

```cpp
Entity spawn() {
    if (_recycled > 0) {                        // 先吃回收来的槽
        Entity e = _free[--_recycled];
        _alive[e] = true; _masks[e] = {};
        return e;
    }
    if (_next >= MAX_ENTITIES) { /* P1 的告警 */ return INVALID_ENTITY; }
    Entity e = _next++;
    _alive[e] = true; _masks[e] = {};
    return e;
}
void destroy(Entity e) {
    if (e >= MAX_ENTITIES || !_alive[e]) return;
    _alive[e] = false; _masks[e] = {};
    if (_recycled < MAX_ENTITIES) _free[_recycled++] = e;   // 固定数组，不碰 heap
}
size_t alive_count() const {                                 // 新增：真实存活
    size_t n = 0; for (Entity e = 0; e < _next; e++) if (_alive[e]) ++n; return n;
}
private:
    std::array<Entity, MAX_ENTITIES> _free{};   // 空闲栈
    size_t _recycled = 0;
```

**收益**：把 256 从"累计额度"变成"同时存活额度"。
**但**：只有配合"页面按需建 / 离开销毁"才省内存，而 papers3 现在是**开机全建、切页只切 `Visible`**
（就是为了切页快）。改成按需建会**每次切页都要重建控件**，代价远大于省的几 KB。
**所以现阶段不建议做。**

⚠️ `count()` 现在返回 `_next`（累计），设置页的"实体 N / 256"用的就是它。
**一旦做了 P5，那个文案就会变成误导** —— 必须改用 `alive_count()`。

### P6 「紧凑存储」——真·懒分配，省 ~30 KB，属重构

按第三节的 typelist 方案改。**先量 P1 的真实用量再决定值不值** ——
46KB 占 `libmain.a` 的 .bss 约 47%，值不值得动 ECS 内核取决于你内部 RAM 的整体余量。

---

## 五、怎么量（每条都能复现）

```bash
cd ~/Project/papers3 && source ~/esp-env/env.sh

# ① 整体静态 RAM
idf.py build && idf.py size              # 看 Used static DRAM / .bss
idf.py size-components                   # 看是哪个组件吃掉的

# ② 精确量 ECS 存储（★ --size-sort 输出是【十六进制】）
nm --size-sort -C build/papers3.elf | grep -E '_store<|_has<'
#   期望看到每类型 256×(sizeof+1)；某类型的 _store 不存在 = 被 DCE 了（好事）

# ③ 运行期内部 RAM 余量
#    printf("internal free = %u KB\n",
#           (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
```

**别用开发机量 `sizeof` 代替**：ESP32 指针 4 B、开发机 8 B，
`Text`/`OnClick`/`CustomDraw` 这类带指针的会差一倍（我第一次就是这么量错的）。

## 六、验收判据

- [ ] 改了 `MAX_ENTITIES` 后，`.bss` 变化 ≈ `ΔN × 184 B`（对不上说明有类型被 DCE 或优化掉了）
- [ ] P1 的每页增量日志加起来 == 开机实体总数
- [ ] 开机 `grep -cE 'panic|Guru'` = 0
- [ ] 设置页"实体 N / 256"与 P1 日志一致（注意它显示的是 `_next` **累计值**）
- [ ] 改了任何结构体字段宽度后，`.bss` 的实际减小与表格预期一致（逐条对，别一把改完才发现没省）
- [ ] 若做了 P5，UI 文案必须改用 `alive_count()`

---

## 七、变更记录

| 日期 | 版本 | 变更 |
|---|---|---|
| 2026-10-08 | v1.0 | 首版。从目标 ELF 实测 ECS 存储 46.0KB、逐类型明细、四层"惰性"研究、P1~P5 清单 |
| 2026-10-08 | v1.0 | （同版修正）作废"删掉没用的组件类型省内存"这条结论——实测未引用的类型成本为 0 |
| 2026-10-08 | v1.1 | 补 flex 成本审计（§1.5）：四件套 16.0KB=35%，`flex()` 工厂 0 调用者，最大孩子数仅 3，全项目只有主页在用；优化清单据此重排为 P1~P6 |
| 2026-10-09 | v1.2 | 键盘做 P0 级优化：35 键实体 → 1 自持容器实体（`CustomDraw` 自绘 35 键面 + 自轮询触摸，不挂 `Touchable`）；输入法页 53→19，全项目 230→174 |

---

## 附：一页速查

```
成本公式   Σ(被引用的组件类型) × MAX_ENTITIES × (sizeof(T)+1)   → 内部 RAM，开机常驻
现在       46.0 KB / 184 B 每实体 / 占 libmain.a 的 .bss 约 47%
单项最大   Children 8.7KB · Style 7.2KB · FlexLayout 6.1KB · Rect 4.1KB
           → flex 四件套合计 16.0KB = 35%，而只有主页两行按钮在用
省内存     ① 溢出打日志(P1，零成本先做) ② flex 那 16KB 值不值(P2，最大单项)
           ③ Style 字段收窄(P3 ~4KB) ④ 降 MAX_ENTITIES(P4 ~5.8KB)
           ⑤ 紧凑存储(P6 ~30KB，重构)  ⑥ 槽位回收(P5，不改内存、现阶段别做)
不要做     删掉"没引用的组件类型" —— 它们已经是 0 成本
失败模式   额度用完 = 控件静默消失，无日志（先做 P1 让它可见）
```
