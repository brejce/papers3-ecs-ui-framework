# 《墨森物语》(Ink Crossing) 前置研究 — 距离实现差什么

> 编写 2026-10-10。方法：对来稿手册的硬件/架构声明逐条核对 M5PaperS3 官方规格 + papers3 现有源码，
> 再列「距离实现还缺什么」的缺口清单。**暂不实现**，本文件是前置研究。
> 游戏是长期目标，先搞清路怎么走、坑在哪，不写一行游戏代码。

---

## 零、结论摘要

手册**质量很高**：硬件映射表几乎全对，架构思路（分层 / 动静分离 / 熔断清理）方向正确。
但有 **1 处关键技术错误**、**1 处致命遗漏**，以及若干需修正的数字。

- ❌ **8080 并口直驱** → 错。实际是 **LCD_CAM 直驱 EPD**（M5GFX `Panel_EPD`），不是 I80/8080 总线。
- ❌ **FreeType 字符光栅化** → 错。papers3 用 **stb_truetype**（`third_party/stb_truetype.h`，只认 glyf 不认 CFF）。
- ⚠️ **"m5game 给每个游戏对象挂 ECS 实体"** → 撞 papers3 的 **256 实体累计硬上限**，手册完全没提，见 §三。
- ⚠️ 帧率数字（30Hz 逻辑 / 15Hz 渲染）→ 与墨水屏"每 10 秒刷一次"的烧屏警告冲突，见 §四。

**手册做对的部分**（会拿去做，不是只挑错）：I2C 互斥锁、陀螺仪滤波+死区、熔断清理、屏幕寿命保护、
多缓冲显存、SD 流式加载、存档 JSON —— 这些都对，且方向就是 papers3 已验证过的思路。

---

## 一、硬件映射核对（对 M5PaperS3 官方规格）

| 手册声明 | 核对 | 证据 |
|---|---|---|
| GT911 电容触摸（两点 + 手势）| ✅ 对 | 官方：SDA=G41, SCL=G42, INT=G48 |
| BM8563 RTC @ 0x51 | ✅ 对 | 官方规格表 |
| BMI270 陀螺仪 @ 0x68 | ✅ 对 | 官方规格表 |
| 无源蜂鸣器 | ✅ 对 | 官方：BUZ_PWM（G21 附近）|
| 960×540 16 级灰度 EPD | ✅ 对 | EPD_ED047TC1，全屏结构 |
| 8MB PSRAM / 16MB Flash / ESP32-S3R8 | ✅ 对 | 官方规格表 |
| 没有 IT8951 中间层、S3 直驱 | ✅ 对 | 官方：「E-ink Screen Control: ESP32S3R8 direct drive」；IT8951 是老款 M5Paper 的 |
| **8080 并口直驱** | ❌ **错** | 见下 |

### 1.1 屏幕驱动：不是 8080，是 LCD_CAM

手册 §4.2 说「8080 并口直驱」「ESP32-S3 的 LCD_CAM 直接驱动 EPD」——**前半句错，后半句对**。

- M5PaperS3 的 EPD 面板（EPD_ED047TC1）由 ESP32-S3 的 **LCD_CAM 外设直接驱动**，
  走的是 EPD 自己的并行数据时序，**不是** ESP-IDF 里的 I80/8080 总线（`esp_lcd_new_i80_bus`）。
- papers3 现状走的是 **M5GFX 的 `Panel_EPD`**（`main/systems/render.h:21` 注释明确写了「M5PaperS3 / Panel_EPD 实测耗时」）。
- 手册把「并行数据线」和「8080 协议」混为一谈。8080 是 Intel 总线的握手协议；EPD 直驱是另一套波形时序。

**为什么这条重要**：手册 §3.3 的「DMA 双缓冲混合刷新」能不能做，取决于 LCD_CAM + DMA 这条链。
核对结果是**能做，且已有先例**：

- 开源项目 **EPD_Painter**（github.com/tonywestonuk/EPD_Painter）专为 M5PaperS3 写了
  「绕过 `esp_lcd_panel_io_i80`、直接驱动 LCD_CAM」的高性能 EPD 驱动，配合
  ESP32-S3 SIMD 汇编 + delta 更新（只推变化像素）+ 双核流水线，做到 **~20fps 等效更新率**（FAST 模式，2bpp 4 级灰）。
- 这**恰好验证了手册 §3.3 混合刷新管线的可行性**——但它是一个独立 Arduino 库，要移植到 ESP-IDF + papers3，是笔工程账，不是"接一下"。

---

## 二、字体栈：FreeType → stb_truetype

手册 §5.1「Glyph Cache 1.5MB，FreeType 字符光栅化 LRU 缓存」——**papers3 不用 FreeType**。

- papers3 用 **stb_truetype**（`third_party/stb_truetype.h`），单文件纯 C，**只认 glyf 轮廓，不认 CFF/TTC**。
- 现成的字形缓存模型：flash mmap 零拷贝加载字体（≈0 RAM），SD 字体读 PSRAM，字形栅格化在内部 RAM 临时分配。
- 手册说的「8.48MB TTF 字体 mmap」**待核实**：papers3 现在用的鸿蒙裁切版是 **1.93MB**。8.48MB 若是完整中文字库，
  得确认 flash 分区（`0x310000 fonts 3MB`）装不装得下、要不要扩分区 + 重烧。

**结论**：游戏若需要字体渲染，直接用 papers3 的 ttf.h 栈，不需要引入 FreeType。Glyph LRU 缓存可以后加，但
用 stb_truetype 的 rasterize 包装，不是 FreeType 生态的东西。

---

## 三、致命遗漏：papers3 的 ECS 实体/组件上限，手册没提

手册 §3.2 要「m5game 给游戏对象挂 ECS 实体」（Transform / Sprite / GridPos…）。**这在 papers3 里会直接撞墙**：

| 约束 | 现状 | 对游戏的含义 |
|---|---|---|
| **实体数硬上限 256（累计）** | `ecs/world.hpp` `spawn()` 里 `_next++` 满 256 静默返 `INVALID_ENTITY` | 庭院场景家具+植物+NPC+UI 轻松上百个；**destroy 不回收槽位**，累计满 256 后彻底不能再建 |
| 组件类型上限 32 | 已用 21，`ComponentMask` uint32 | 加 5 个游戏组件（Transform/Kinematic/GridPos/Sprite/StateMachine）→ 26，还剩 6 个 |
| 组件静态 array 常驻 .bss | ECS 占 46KB 内部 RAM，184B/实体 | 256 实体全开 ≈ 47KB 内部 RAM，游戏再堆场景数据会紧张 |

**这个约束决定游戏引擎的架构，不是"注意一下"级别的细节。**

**解法（papers3 里已有先例）**：键盘重构已经把「海量内容」从 ECS 摘出去了——35 键 → **1 个容器实体自持**，
键数据全在 `Keyboard` 结构里，绘制走 CustomDraw 自绘、触摸走 `tick()` 自轮询。

游戏应该照抄这个模式：

- **ECS 实体只承担 UI 层**（按钮、菜单、HUD、对话框）。
- **游戏世界（家具/NPC/植物/物理体）不走 ECS**，是 `m5game` 自己的一层对象系统，自己管理、
  自己 tick、自己绘制到显存。
- 场景渲染 = **1 个 CustomDraw 实体**，`draw_scene()` 里合成整帧推屏——跟阅读器正文（1 个 CustomDraw 画整页）同构。

这样 ECS 的 256 上限对游戏无感，游戏世界的规模上限由 PSRAM 决定（8MB，够放几百个对象 + 显存），
而不是被 256 卡死。**手册 §3.2「给游戏对象挂 ECS 实体」这条要推翻重写。**

---

## 四、帧率 vs 墨水屏烧屏，手册自相矛盾

手册 §3.3 说「渲染最高 15Hz」，§4.3 又引用官方「建议每 10 秒刷新一次，太快易烧坏」——
**这两条打架**：15Hz = 每秒 15 次，和"每 10 秒 1 次"差 150 倍。

实际情况：

- EPD 全屏刷新（GC16/QUALITY）一次 **780ms**（papers3 实测，`render.h:21`），最快也只能 ~1.3Hz 全屏。
- FAST 局部刷新（DU）234ms、FASTEST（DU4）171ms —— 局部小块可以到几 Hz，但连刷会撕裂（papers3 `render.h:25` 注释）。
- EPD_Painter 的 ~20fps 是 **delta 极小区域**（只推几个变化像素），不是全屏刷新，且是 2bpp 4 级灰牺牲了 16 级灰。

**游戏设计要反过来迁就硬件**（这其实正是手册 §1.1 自己说的"慢生活"定位）：

- 场景**静止时完全不刷新**（手册 §4.3 的"停止操作 3 秒停刷"是对的）。
- 交互瞬间（拖拽家具、墨滴滚动）用 **FAST 局部 + delta**，靠熔断机制定期 GC16 清残影（papers3 已有 `FULL_EVERY=8` 同款思路）。
- **"30Hz 逻辑 tick"没问题**（算，不推屏）；**"15Hz 渲染"是错的**，改成"按需渲染，最高局部几 Hz，全屏 ≤1.3Hz"。

---

## 五、缺口清单：距离实现，按依赖顺序

### Phase 0 — 先解决硬约束（不解决后面全白搭）

- [ ] **游戏世界架构定型**：确认「游戏对象不走 ECS、场景 1 个 CustomDraw 实体」这个方向
  （见 §三），否则 256 实体上限会卡死一切。
- [ ] 确认 flash 分区是否要扩（8.48MB 字体若要用，`fonts` 3MB 分区装不下，改分区 = 全量擦 flash + 重烧）。

### Phase 1 — 基础设施（硬件能力接入）

- [ ] **I2C 互斥锁**：GT911 + BM8563 + BMI270 三个设备共享 I2C 总线，但**访问频率完全不同，别当成三个平等争抢**：
  - **触摸（GT911）**：主循环每帧轮询，**持续占用**，是必须保证的那一个。
  - **RTC（BM8563）**：**按需读**——进游戏时读一次算离线收益、存档时读一次打时间戳、需要时才碰，
    不需要专门轮询。读一次几字节，take 锁 → 读 → give 锁，微秒级，不影响触摸。
  - **陀螺仪（BMI270）**：**按需开启**——只有重力沙盘/摇晃体感这些玩法里才开，平时关掉省电。
    开着的那段时间才是真正要和触摸抢 I2C 的窗口（那才需要仔细设计采样调度）。
  → 手册 §4.1 的"高频轮询触摸时读 RTC 会死锁"**言过其实**：RTC 是低频插空读，不是高频轮询。
  真正的调度难点在「陀螺仪开启期间」和触摸的分时，不是三个设备三路并发。
- [ ] **BMI270 陀螺仪接入**：M5Unified 有 `M5.Imu` 接口，但要验证 + 低通滤波 + 死区（手册 §4.4 对）。
- [ ] **BM8563 RTC 接入**：`M5.Rtc`，验证互斥锁下的并发读。
- [ ] **蜂鸣器**：LEDC PWM 驱动（手册说 LEDC/RMT，无源蜂鸣器用 LEDC 出 PWM 即可）。

### Phase 2 — 渲染管线（最大工程，也是游戏引擎的核心）

- [ ] 现状是「**实体级刷新**」——每个脏实体独立 startWrite/endWrite + display 推一块
  （`render.h:53` render 循环）。游戏要的是「**脏矩形 diff + 多缓冲合成**」。
- [ ] **多缓冲显存**（手册 §3.3 的 vfb_background / vfb_dynamic / vfb_composite，每个 259KB，
  数学对：960×540×4bit÷8 = 259200B）：papers3 现在没有这层，是直接画 M5GFX 的帧缓冲。
- [ ] **LCD_CAM DMA 直驱 + delta 更新**：参考 EPD_Painter 移植，或**先降级**——慢节奏游戏用现有
  "实体级刷新 + FAST 局部"足够跑通原型，20fps 那套留到玩法验证后再上。
- [ ] 熔断清理（GC16 全刷）——papers3 已有 `FULL_EVERY=8` + `quality_wash()`，直接复用。

### Phase 3 — 游戏引擎（m5game）

- [ ] 对象系统（不走 ECS）：Transform/Kinematic/GridPos/Sprite/StateMachine 作为**普通结构体数组**，
  自己 tick。
- [ ] 物理：重力沙盘解谜（墨滴/果实滚动）**自写**即可——墨水屏场景的物理体数量级（几十个）
  不需要 Box2D，简化的圆-矩形碰撞 + 重力积分够用。
- [ ] 存档：JSON 读写 SD（`platform/sd.h` 已有挂载/读文件，加写 + JSON 解析即可）。

### Phase 4 — 美术 + 功耗

- [ ] 16 级灰度线稿美术资源 + 抖动算法（dithering）适配快速刷新模式。
- [ ] 动态刷新率（静止停刷）+ 深度休眠唤醒后时间跳跃的平滑过渡。

---

## 六、最大的三个风险（按优先级）

1. **256 实体上限** —— 架构定型前必须先定"游戏对象不走 ECS"，否则返工最大。
2. **渲染管线工程量大** —— 从"实体级刷新"到"脏矩形 diff + 多缓冲 + LCD_CAM DMA"是重写 render 层，
   不是加功能。建议先降级（现有刷新跑通玩法），再按需升级。
3. **I2C 分时调度** —— 三个设备共享一根 I2C，锁是必须的；但难点只在**陀螺仪开启期间**和触摸的分时
   （RTC 是低频插空读，触摸是持续轮询）。做错的表现是"重力沙盘里陀螺仪卡顿"或"读 RTC 时触摸丢帧"，
   间歇性、难定位。Phase 1 里 RTC 和陀螺仪**都按需接**，别一上来三路并发。

---

## 附：核对来源

| 事实 | 来源 |
|---|---|
| GT911/BMI270/BM8563/蜂鸣器/EPD_ED047TC1/8MB/16MB/直驱 | M5Stack PaperS3 官方规格表（docs.m5stack.com / switch-science 镜像 / shop 页）|
| 屏幕直驱（非 IT8951）| 官方对比表：「E-ink Screen Control: ESP32S3R8 direct drive（PaperS3）vs IT8951（M5Paper）」|
| LCD_CAM DMA 直驱可行性 | EPD_Painter（github.com/tonywestonuk/EPD_Painter）README：绕 i80、SIMD delta、~20fps |
| papers3 字体栈 = stb_truetype | `main/text/ttf.h`、`third_party/stb_truetype.h` |
| papers3 EPD 波形耗时 | `main/systems/render.h:21-25`（QUALITY 780ms / TEXT 780ms / FAST 234ms / FASTEST 171ms）|
| ECS 256 上限 / 组件 32 上限 | `main/ecs/world.hpp`、`main/ecs/components.h`（此前已核对）|
| 实体级刷新（非脏矩形）| `main/systems/render.h:53` render 循环 |
