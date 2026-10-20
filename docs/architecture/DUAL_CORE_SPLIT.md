# 双核分工:UI 独占 core1

> **状态：✅ 已落地（2026-10-19）**，与本文 §3–§4 的设计**有两处不同**，以本框为准：
>
> | 项 | 设计稿（§3–§4） | 实际落地 |
> |---|---|---|
> | 输入 | 单独 `input_task` 在 core0 + SPSC 队列过核 | **没做**。触摸仍是**单一读者**，就在 ui 任务里（`app.update()` → `M5.update()`，加 `.so` 的 `so_touch`/`so_wait_display`）→ **跨核零数据**，连队列都不需要 |
> | core1 跑什么 | UI + 渲染 + 推屏 + 插件 | **UI + 渲染 + 插件**（`ui_task`，prio 5，栈 12288）；**EPD 推屏留在 core0** —— `Panel_EPD.cpp` 改成「绑创建核」而不是「创建核 +1」 |
> | core0 跑什么 | 采集 + WiFi | **WiFi(23) + sys_evt(20) + esp_timer(22) + lwIP + httpd + epd 推屏** |
>
> 实测负载（`/api/stat`，见 §5）：**core0 ≈ 11~13%，core1 ≈ 1%**（偶发 7~15%），堆 6.0MB / PSRAM 5.96MB。
> 任务表（`/api/tasks`）：`ui core=1 prio=5` / `wifi core=0` / `sys_evt core=0` / `httpd core=0`。
>
> 落地用的关键三行：① `main.cpp` 加 `xTaskCreatePinnedToCore(ui_task, "ui", 12288, &app, 5, nullptr, 1)`；
> ② `sdkconfig` 的 lwIP 改 `CPU0`（只挪 wifi 不够，真正的 TCP/HTTP 重活在 `tiT`）；
> ③ `web_files.h` 的 `httpd` 设 `core_id = 0`。
> 另外 `sys_evt` 是 IDF 硬编码 core0，**挪不走**。
>
> 下面 §0–§4 保留为**设计过程记录**（为什么这么分、踩过哪些坑），其中的 `input_task` 方案未实施。

---

## 0. 一句话

**把"输入"和"显示"彻底分开**:core0 只做 GT911 采集 + 按键 + WiFi,其余全部搬到 core1。
跨核只过一条事件队列,World / Display / 插件状态全部只被 core1 碰 → **不需要任何锁**。

---

## 1. 现状(2026-10 取证)

| 核 | 跑什么 | 证据 |
|---|---|---|
| **core0** | 主循环 `M5.update → flex → touch → kb/cb → render`、`flush_band`、`launcher::tick`、`plugin::tick`、**WiFi 任务**、LWIP、HTTP | `CONFIG_ESP_MAIN_TASK_AFFINITY_CPU0=y`<br>`CONFIG_ESP_WIFI_TASK_PINNED_TO_CORE_0=y`<br>`CONFIG_LWIP_TCPIP_TASK_AFFINITY_NO_AFFINITY=y` |
| **core1** | **空转,零任务** | `plugin::touch_task_start()` 被 `#if 0`(`main.cpp:180`、`plugin.cpp:898`) |

### 1.1 一次翻页的耗时构成(代码注释实测)

| 阶段 | 耗时 | CPU 实际状态 |
|---|---|---|
| 排版 + 光栅化 + 逐字形落图 | 283–383 ms | 真算 |
| `display()` + 等波形 | ~900 ms | **空转轮询**(`while(p->isDrawing())` + `delay(1)`) |
| 页快照 存 / 恢复 | 74 / 105 ms | 主要是 memcpy |
| 整页预取 | ~600 ms | 真算 |

**结论:瓶颈不是算力,是"波形等待"期间 core0 空转,而 core1 全程闲着。**

### 1.2 已经踩过的坑(必须遵守)

1. **GT911 触摸只能单读者** —— 点数据读一次就被消费。两核各读一次会互抢:
   主页按不动 + 阅读页三分区收不到触摸(`plugin.cpp:898-912` 的 `#if 0` 就是撤掉的证据)。
2. **`M5.Display` 非线程安全** —— 单实例、共享 SPI/并行总线,两核同时 draw/display 会撕裂。
3. **`M5.Display.waitDisplay()` 是忙等** —— 所以才有 `so_wait_display()` 自己轮询 `isDrawing()` + 采样触摸的补丁。

---

## 2. 为什么不能"两核各画一半"

帧缓冲只有一块,M5GFX 的 `startWrite/endWrite` 事务、`pushImage` 都写同一块缓冲。
两核分别画上半/下半屏会互相踩,并且 `display()` 推屏本身就是全局事务。
**结论:绘制要么单核独占,要么不做。**

---

## 3. 两个候选方案

### 方案 Y:逻辑留 core0,渲染上 core1

- core0:触摸 + 逻辑(改 World)
- core1:遍历 Dirty → 画帧缓冲 → 推屏
- **问题**:World 跨核共享 → 必须加锁;而渲染一次持锁 283–383ms,会把 core0 的触摸堵住,**等于没解决**。
- 若改用"快照/指令列表"避免锁,又要深拷贝 Text 字符串,并且**插件的 `CustomDraw` 会跑在 core1、而插件 `tick` 在 core0** → 插件内部状态跨核竞争。
- **结论:不推荐。插件架构下竞争面太大。**

### 方案 X:输入下沉,逻辑 + 渲染全在 core1(推荐)

- core0:GT911 采集(唯一读者)+ 按键 + WiFi
- core1:**ECS 逻辑 + 命中检测 + render + 推屏 + plugin::tick + CustomDraw**
- 跨核只有一条 **SPSC 事件队列**
- **World / Display / 插件状态都只被 core1 碰 → 零锁**

**核心优势:插件天然合规** —— `tick` 和 `CustomDraw` 同核,读同一份插件全局状态,不存在竞争。

---

## 4. 方案 X 详细设计

### 4.1 核职责

| 核 | 任务 | 优先级 | 栈 | 备注 |
|---|---|---|---|---|
| core0 | `input_task`(GT911 采集 + 按键) | 6 | 3072 | 每 4–8ms 读一次,沿检测后投递 |
| core0 | WiFi / LWIP / HTTP(IDF 自带) | 23 / 18 | — | 不动 |
| core1 | `ui_task`(原主循环) | 5 | 8192 | 逻辑 + 渲染 + 推屏 + 插件 |
| core1 | (后期)书籍加载分片 / 页缓存预取 | — | — | 跑在 ui_task 的空闲段 |

> 优先级说明:WiFi 23 仍高于所有人,空闲时不会被饿死;input 6 略高于 ui 5,保证按键采样优先。

### 4.2 输入事件队列(唯一跨核数据)

```c
// 位置:main/platform/input_queue.h
enum InEvType : uint8_t { INEV_DOWN = 1, INEV_UP, INEV_MOVE, INEV_KEY };

struct InEvent {
    uint8_t  type;
    int16_t  x, y;      // 触点坐标(INEV_KEY 时 x=键码)
    uint32_t t_ms;      // 本地时刻,调试用
};

// 64 项环形队列,SPSC(core0 写、core1 读),无锁
```

**规则:**
- core0 **只在状态变化时投递**(按下沿 / 抬起沿 / 移动超过阈值),不是每 4ms 一投
- core1 每帧把队列 **全部消费完**,按顺序重放成 `touch_begin / touch_move / touch_end`
- 队列满 → 丢最旧的 MOVE,保留 DOWN/UP(绝不能丢沿)

### 4.3 core0:input_task

```c
for (;;) {
    M5.Touch.update(m5gfx::millis());   // 唯一读者;4ms 内部节流
    int n = M5.Touch.getCount();
    // 沿检测:上帧 n>0 且本帧 n==0 → UP;反之 → DOWN
    // 投递 InEvent 到队列
    // 按键:M5.BtnA.wasPressed() → INEV_KEY
    vTaskDelay(pdMS_TO_TICKS(4));
}
```

**硬约束:**
- **绝不调任何 `M5.Display.*`**
- **绝不改 World / 插件状态**
- 保留"推屏期间漏触摸"的锁存逻辑(`g_tap_latch`),挪到 core0 —— 因为 900ms 的洞现在在 core1,core0 一直在采样,**这个补丁可以删掉**

### 4.4 core1:ui_task

内容 = 现在的 `while(true)` 循环,只需要把输入来源换掉:

```c
for (;;) {
    drain_input_queue();          // 队列事件 → systems::touch 的 down/move/up
    rtctime::tick();
    if (power::tick(world)) {
        flex_layout(world);
        handle_touch(world);      // 只处理本帧事件(不再读 GT911)
        ui::kb::tick_g();
        ui::cb::tick_g();
        render(world);            // 画帧缓冲 + epd_push
    }
    pages::flush_band();
    launcher::tick();
    plugin::tick();               // 插件逻辑与绘制同核
    idle_hooks();                 // 阶段 3/4 挂这里
    vTaskDelay(1);
}
```

**注意:`systems::handle_touch()` 与 `ui::kb::tick_g()` / `ui::cb::tick_g()` 现在都要从"读 GT911"改成"读事件队列"。**
键盘/候选栏是自持轮询的(`keyboard.h` / `candidate_bar.h`),要改成消费同一条队列(见 4.6)。

### 4.5 M5 拆分

| 子对象 | 核 | 说明 |
|---|---|---|
| `M5.Touch` | core0 | 唯一读者 |
| `M5.BtnA` / 电源键 | core0 | 按键事件投递 |
| `M5.Display` / `M5.Display.panel()` | **core1** | 全部绘制、`setEpdMode`、`display`、`waitDisplay`/`isDrawing` |
| `M5.Imu` | core0(可选) | 抬腕唤醒,事件投递 |
| `M5.Power` | core0 | 电源管理 |

**⚠️ 最大不确定点:`M5.update()` 是聚合调用**(Touch + Btn + Imu + Power)。
- 方案:`M5.update()` 留在 core0,core1 **只调 `M5.Display.*`**
- 需要核对 `M5Unified` 内部是否有跨子对象的共享状态(尤其 `M5.Display` 的 `startWrite` 与 `M5.update()` 是否共用总线锁)
- **阶段 1 第一天就要验证这一条**,不通过则回退方案 Y

### 4.6 键盘 / 候选栏的改造

现在 `kb::tick_g()` / `cb::tick_g()` 自己轮询 `M5.Touch`。
改造后它们**不再读硬件**,改为:
- `ui_task` 把本帧事件整理成 `TouchFrame{down, x, y}`(按下沿/当前点/抬起沿)
- `kb::tick_g(frame)` / `cb::tick_g(frame)` 用这份帧数据做命中

**好处:全项目只有 input_task 一个硬件读者**,再也不会出现"两个读者抢点"。

### 4.7 启动顺序

```
app_main(core0)
  ├─ app.begin()            初始化 M5(Display 只初始化,不画)
  ├─ 建 World / 页面 / 字体 / 拼音
  ├─ 起 ui_task(core1, 8192, 5)
  ├─ 起 input_task(core0, 3072, 6)
  └─ core0 只剩:WiFi 开关编排 + 空闲
```

**首屏白场(`app.begin()` 里那次 QUALITY 整屏)必须在 core1 起来之后画**,否则 core0 会碰 Display。

### 4.8 同步点清单

| 数据 | 谁写 | 谁读 | 同步方式 |
|---|---|---|---|
| 输入事件 | core0 | core1 | SPSC 环形队列 |
| World / Dirty | core1 | core1 | 无 |
| Display 帧缓冲 | core1 | core1 | 无 |
| 插件状态 | core1 | core1 | 无 |
| NVS 设置 | core0(设置页在 core1,但要写时投递) | — | 见下 |

**NVS:设置页在 core1,直接写 NVS 也没问题(Flash 操作有 IDF 锁)。不需要跨核。**

---

## 5. 分阶段计划

### 阶段 1:骨架(先证明可行)

1. 新增 `platform/input_queue.h`(SPSC 队列)
2. 新增 `input_task`(core0):GT911 沿检测 → 队列
3. 新增 `ui_task`(core1):迁主循环;`handle_touch` / `kb` / `cb` 改吃队列
4. `M5.Display` 全部收进 core1
5. **验证 `M5.update()` 与 `M5.Display` 跨核是否安全**

**验收:**
- 连点 20 下,零丢失
- 手动在 core1 里 `delay(1500)` 模拟卡顿,期间 core0 仍能继续投递 → core1 恢复后按顺序重放,点击不丢
- 推屏 900ms 期间手指抬起-再按下,能识别成两下

### 阶段 2:插件同核

- `plugin::tick()` 已在 core1(随主循环)
- `so_touch()` 改为读 core1 的"本帧事件",不再读 GT911
- `so_wait_display()` 里的触摸补报逻辑**删除**(core0 一直在采,不需要补)

**验收:** reader 翻页、三分区、弹窗全部正常;连续快速翻 10 页不丢点击。

### 阶段 3:书籍加载上 core1

现在 `open_book()` 在 core1 里同步读 512KB(~200ms)+ parse(~50ms)。
改成**分片加载**:

```c
struct LoadJob { char path[320]; size_t off, total; ... };
// ui_task 每帧只读 32–64KB,读满再 parse
```

**验收:** 17MB 的书,打开过程中 UI 不卡,能显示进度条;取消能中断。

### 阶段 4:页缓存预取(空闲段)

`pc_prefetch_step()` 已经在 core1 主循环里,只需加门控:

```c
void idle_hooks() {
    if (g_ui_busy) return;              // 有 Dirty / 有事件 → 不预取
    if (events_pending()) return;       // 有输入优先
    pc_prefetch_step();                 // 一次一页(内部可被触摸打断)
}
```

**验收:** 预取不拖慢翻页;翻页 P50 不劣化。

---

## 6. 风险

| 风险 | 影响 | 应对 |
|---|---|---|
| **M5Unified 跨核不安全**(Display 与 update 共享状态) | 阶段 1 直接卡住 | 第一天验证;不行退方案 Y(渲染上 core1,World 快照) |
| 插件 `.so` 里有静态全局,跨核? | 无(方案 X 下全部 core1) | — |
| WiFi 优先级 23 抢占 core0 的 input | 按键采样抖动 | input 优先级 6,WiFi 空转时不占;实测再看 |
| 队列满丢事件 | 点击丢失 | 丢 MOVE 保 DOWN/UP |
| core0 只有 input 任务,算力浪费 | 无功能影响 | 后期可把 WiFi 处理/OTA 也放 core0 |

**回滚:** 保留 `#define M5UI_DUAL_CORE 1` 开关,关掉 = 原单核主循环,零风险。

---

## 7. 验收总表

| 项 | 标准 |
|---|---|
| 触摸响应 | 推屏 900ms 期间点击 P99 < 100ms |
| 连点 | 20 连点零丢失 |
| 翻页 | 与当前一致(不劣化),点击不再"要按好几下" |
| 开大书 | 17MB 不白屏、可中断、有进度 |
| 预取 | 翻页 P50 不劣化 |
| 回滚 | 宏关掉即恢复旧行为 |

---

## 8. 可借鉴（InkBoy）：槽调度 + planner 分工

> 驱动模型部分见 `../display/EPD_LUT_MANUAL.md` §11；本节只管**调度与核分工**这两件与本文同域的事。
> **状态：未落地**，是候选方案，不是现状。

### 8.1 固定槽调度 + 三缓冲最新帧

InkBoy 的调度（实测 30 次/秒）：

- 槽固定 **33.334ms**，**锚在固定时刻表上，不锚在上一次扫描** —— 迟到的扫描不会把后面所有槽往后推，错过的槽**跳过不补**。
- 三缓冲交接：每个槽只取"最新一张画完的图"去规划，**旧帧永不排队**。
- 新图优先于"塞不进这个槽的收尾扫描"。

对照我们的现状：`systems/render.h` 的 `epd_push()` 是**事务式**（脏了就推、推完等波形），
`ink_tale` 的节拍也是手写的（`EPD_LUT_MANUAL.md` §10.5 坑 1：时刻必须记在 `do_step()` 之前）。
**"刷新周期 = 波形 + 节拍"就是没有槽调度的症状**（实测 2.6fps，本该 5fps）。

改成槽调度后：动画帧率**可预测**（槽宽由该档的走完行数算出），
"这一拍来不及"表现为**丢一帧**，而不是拖着整条链一起变慢。

---

### 8.2 planner 按行分核 + 等价性测试

InkBoy 灰度的 planner 要逐像素走 4 万像素，一个核塞不进一个槽，于是：

- 逐像素状态放**外部 RAM**（读数慢）→ 规划时先把工作集拷进**内部 RAM**；
- 屏幕**按行切上下两半**：一半 display 任务、一半另一核的 worker，结束再合并；
- **内部 RAM 不够就退回单核，结果必须完全相同**（它有一份 `..._parallel_equivalence.py` 逐字节比两路输出）。

对本项目：本文 §2 已判定"两核各画一半"**不成立**（帧缓冲只有一块、`display()` 是全局事务）——
但那条结论针对的是**绘制**，**不是规划**。planner（脏矩形合并、逐像素掩膜、成本估算）是纯函数式的，
可以照它那样按行分核，**前提是配一份等价性测试：两核结果与单核逐字节一致，否则不采用**。
这类"并行改动不改变输出"的判据是本文验收表（§7）现在缺的一类。

### 8.3 顺序与取舍

| 项 | 现在 | 建议 |
|---|---|---|
| 推屏 | core0（落地口径，见文首框） | **不动**：`display()` 是全局事务，挪回 core1 反而和 UI 抢总线 |
| planner | 没有独立 planner（触发即画即推） | 先有 planner（`EPD_LUT_MANUAL.md` §11 第 1 条），**再**谈分核 |
| 槽调度 | 无 | 只在动画场景（`ink_tale`）试点，翻页/菜单不引入 |

**顺序：先逐像素预算（决定"这一拍要驱动谁"）→ 再槽调度（决定"这一拍什么时候发"）→ 最后才是 planner 分核。**
跳步会得到"用两个核加速了一个错的模型"。

---

## 9. 待确认

1. `M5.update()` 里除 Touch 之外是否碰 Display(决定 4.5 是否成立)
2. `ui::kb::tick_g()` / `ui::cb::tick_g()` 的签名改动范围(全项目调用点)
3. `systems::handle_touch()` 里是否还有其他硬件直读(如 `M5.Touch.getDetail`)
4. 插件 `so_touch()` 的调用点(reader / settings / terminal 三处)
