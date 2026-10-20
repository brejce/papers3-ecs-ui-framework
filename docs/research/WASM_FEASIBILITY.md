# m5ui → WASM 可行性研究（对来稿手册的事实核查）

> 编写 2026-10-08。**方法**：不装依赖、不编译，先把来稿手册的每条硬事实回到权威源核一遍
> （WAMR 官方仓库/文档、ESP Component Registry、独立实践者记录），再评估架构。
> 核查对象：来稿《m5ui → WASM 架构改造手册》。
> 配套：`../guide/DEVELOPER.md`（原 `ARCHITECTURE.md` 已并入其附录）、`../architecture/ECS_OPTIMIZATION.md`、`../display/EPD_TEST_MANUAL.md`。

---

## 零、结论摘要

**你的目标**（原话）："每个页面可以为一个 app，按需加载，**可以不重刷固件更新页面**。"
拆成必要条件：① 页面是独立分发单元 ② 按需加载 ③ 更新页面不需重烧固件。
**注意：WASM 只是达成③的候选手段之一，它本身不是目标。**

| 判定 | 内容 |
|---|---|
| ✅ **对的** | Registry 组件 `espressif/wasm-micro-runtime v2.4.0~1` 真实存在；`wamrc --target=xtensa` 与 `wamr-compiler/build_llvm_xtensa.sh` **真实存在**；**`LIB_PTHREAD` 默认开启会让 `create_exec_env` 失败** 的坑**真实**（有独立实践者复现） |
| ❌ **核心前提错** | "关闭解释器、只用 AOT" 这条在 ESP32 上**默认加载就失败**：`AOT module load failed: mmap memory failed`。**必须先走 XIP 模式**（`wamrc --xip`）—— 手册**完全没提 XIP**，所以 §11 Phase 4"切换到 .aot"照做会失败 |
| ⚠️ **前提要改** | 手册的分区表与 m5ui 现状不符（m5ui 现在**没有 `otadata`**；`fonts` 是 **subtype 0x40 自定义**）；改分区表意味着**必须全量 `erase-flash`** → 清掉 NVS 设置、字体分区要重烧 |

### ⚠️ 修订（2026-10-08 晚）：这一节的结论被推翻，见 §八

初版结论是"用页面描述格式（数据驱动）替代 WASM，成本 1/10"。**这个结论答错了问题**：

> 用户的批评（原话）："**但是固件本身没提供的能力，新的页面就无法工作了……这跟我手写一个操作系统
> 有啥区别……**"

**成立。** 数据格式只能改**布局**（换皮），**不能加能力** —— 固件没有的控件/逻辑，页面写不出来。
它答的是"不刷固件改布局"，而目标是"不刷固件**加能力**"。**两件事。**

> **判断依据一句话：能"加能力"的插件机制，必然要求你定义一层 API 面（syscall 表）。
> 差别不在"要不要写这层"，而在"这层要写多厚"。**
> → 三条路的"API 面厚度"与能力上限，见 **§七**。

---

## 一、来稿手册逐条核查

| # | 手册的声明 | 核查结果 | 依据 |
|---|---|---|---|
| 1 | `espressif/wasm-micro-runtime: "==2.4.0~1"` | **✅ 真实** | ESP Component Registry 有该版本 |
| 2 | `CONFIG_WAMR_ENABLE_LIB_PTHREAD=n` 必需，否则 `create_exec_env` 失败 | **✅ 真实**，且是新手第一大坑 | 独立实践者（Zenn）复现：registry 版 Kconfig 默认 AOT/pthread/WASI/AppFramework **全开**，`LIB_PTHREAD` 开着时 `wasm_runtime_create_exec_env` 失败 |
| 3 | `wamrc --target=xtensa --cpu=esp32s3` + `build_llvm_xtensa.sh` | **✅ 真实**（`xtensa` 在 wamrc 的 target 列表里；该脚本确实存在，内容就是 `build_llvm.sh --platform xtensa`） | `wamr-compiler/main.c`、`wamr-compiler/build_llvm_xtensa.sh` |
| 4 | `CONFIG_WAMR_ENABLE_AOT=y` + `CONFIG_WAMR_ENABLE_INTERP=n`（只用 AOT） | **❌ 默认会失败** | 见 §二 |
| 5 | "AOT 比解释器快 10–50 倍，**必须使用 AOT**" | **⚠️ 倍率同量级但偏高**：WAMR 官方性能表是 **8.8×~22×**（matrix 22.02 / gimli 9.94 / CoreMark 8.79） | WAMR wiki: Performance |
| 6 | native 签名规则 `i/I/f/F/$/*/~` | ✅ 与 WAMR 的 `doc/export_native_api.md` 一致 | 官方文档 |
| 7 | Rust wasm 默认 1MB 栈要压到 8KB | ✅ 合理（wasm32-unknown-unknown 的默认栈确实过大） | 通用已知问题 |
| 8 | 分区表（含 `otadata`、`fonts` 用 spiffs、`wasm_apps` 6M） | **⚠️ 与现状不符**，见 §六 | m5ui 的 `partitions.csv` |
| 9 | "WASM 创建的 Entity 进同一个 World，`flex_layout/handle_touch/render` **完全不需要修改**" | **❌ 不成立**，见 §五 | m5ui 的组件定义 |
| 10 | 边界调用 ~15.8µs / 2.5~3.7µs | 量级合理（**解释器模式**） | 与 WAMR 社区的实测数量级一致 |
| 11 | 示例 `widgets::rect(*g_world, x, y, w, h, **(uint16_t)color**)` | **❌ 颜色会错**：m5ui 的 `ui/theme.h` 用 **24bit RGB888**（`0xFFFFFF/0x000000/0x555555`），`render.h` 直接喂 LGFX 的 `uint32_t`。截成 16bit → **白色不是白** | `ui/theme.h`、`systems/render.h` |
| 12 | §3.3「边界检查是宿主责任……长度不会自动校验」 | **❌ 反了**：`$`/`*`/`~` 由**运行时自动做地址转换 + 边界检查**。只有改用 `'i'` 自己传指针时才需手动 `wasm_runtime_validate_app_addr` + `wasm_runtime_addr_app_to_native` | WAMR `doc/export_native_api.md` |
| 13 | 「`idf: ">=5.1"`」 | ✅ 与组件 `idf_component.yml` 一致 | 同上 |
| 14 | 默认 target | ⚠️ 官方 IDF 示例默认 target 是 **esp32c3**，不是 S3（要自己 `set-target`） | 示例 README |

---

## 二、AOT 这条线的真实状态（最关键的一条）

### 2.1 默认路径：加载就失败

社区记录（WAMR discussion #1093，"How to compile from .wasm to aot for esp32 family"）：

```
I (336) SPIFFS: File open "/spiffs/hello.aot". File size: 1648 Bytes
E (356) wamr: Error in wasm_runtime_load: AOT module load failed: mmap memory failed
```

`.wasm` 能跑，`.aot` 挂。

**机理（已对源码核实）**：WAMR 申请 AOT 代码内存时用 `MMAP_PROT_EXEC`，
落到 ESP-IDF 平台层就是 `heap_caps_malloc(MALLOC_CAP_EXEC)`
—— **只能从内部 RAM 分配，不能用 PSRAM**。而 ESP32-S3 的**内部 RAM 只有 512KB**，
m5ui 自己已经占掉 ~97KB 静态 + 运行时堆栈帧缓冲。
（PSRAM 镜像有开关 `WASM_MEM_DUAL_BUS_MIRROR`，但它由 `CONFIG_ESP32S3_SPIRAM_SUPPORT`
这个 **IDF 4 时代的旧名**控制，在 IDF 5.x 上是否生效需要实测。）

**所以 AOT 在 ESP32-S3 上是"内部 RAM 够不够"的问题，不是"能不能"的问题。**
→ 这直接推出下面的 XIP：**把 AOT 代码留在 flash 里，根本不申请可执行内存。**

### 2.2 官方解法是 **XIP（Execution In Place）** —— 手册遗漏的关键

WAMR 有专门的文档 `doc/xip.md`，开篇即写明它存在的理由：

> "Some IoT devices may require to run the AOT file from flash or ROM which is read-only,
> so as to reduce the memory consumption, **or resolve the issue that there is no executable
> memory available to run AOT code**."

```bash
# XIP 的正式用法
wamrc --xip -o page.aot page.wasm
# --xip 是下面两个选项的简写：
wamrc --enable-indirect-mode --disable-llvm-intrinsics -o page.aot page.wasm
```

**32 位 MCU 还要额外处理 64 位整型/浮点内置函数**（XIP 不能直接用 LLVM intrinsic）：

```bash
# 无 FPU 的 MCU（如 Cortex-M3）要三个
wamrc --target=thumbv7m --cpu=cortex-m3 --xip \
      --enable-builtin-intrinsics=i64.common,fp.common,fpxint -o hello.aot hello.wasm
# ESP32-S3 有 FPU，f32/f64 走原生指令；但 i64 运算仍需 builtin 兜底
```

### 2.3 XIP 是"照做会失败"和"能做"之间的那一环

**手册 §11 Phase 4「切换到 .aot，从 SPIFFS 动态加载」按字面做 = 失败。**
正确顺序是：

```bash
# 1. 编 wasm（照手册）
clang --target=wasm32 -nostdlib -Wl,--no-entry -Wl,--export-all -o page.wasm page.c
# 2. 关键：加 --xip（手册漏了这一步）
wamrc --target=xtensa --cpu=esp32s3 --xip \
      --enable-builtin-intrinsics=i64.common -o page.aot page.wasm
```

ESP32-S3 **有 FPU**，所以 `fp.common`/`fpxint` 不必加；但 **i64 运算**要 builtin 兜底
（XIP 模式不能直接用 LLVM intrinsic）。

### 2.4 但 XIP **本身也没完全成熟**

同一份文档承认：

> "There may be some relocations to the '.rodata' like sections which require to patch the
> AOT code. **More work will be done to resolve it in the future.**"

**结论**：AOT 不是不能做，是**必须在手册的基础上加 `--xip` + builtin-intrinsics 调优**，
且要接受它仍有未完成的重定位问题。**而"退回到解释器"是必然能跑通的路径。**

性能上（WAMR 官方表）解释器 vs AOT 差 8.8~22×。
**但对你的场景这个差距基本无感** —— 页面初始化只是几十次 `rect/label/button` 的 native 调用
（每次 2.5~3.7µs，总共不到 1ms），而墨水屏一次刷新就是 **350~1400ms**。
**计算在解释器里慢 20 倍，也仍比一次刷屏快三个数量级。**

---

## 三、两条路线的成本对照（决定性）

| | **WASM（WAMR）** | **页面描述格式（数据驱动）** |
|---|---|---|
| 额外 RAM | 运行时池（手册建议 ≥256KB PSRAM）+ 每实例线性内存（8~64KB） | **≈ 0**（实体本来就要建） |
| 额外 flash | 运行时代码 **29.4~58.9KB**（官方数字）+ WASM 应用分区 | **≈ 2KB**（加载器） |
| 新工具链 | LLVM + Xtensa 后端 + 自建 `wamrc` + WASI SDK | **无** |
| 桥接层工作量 | **手册自认"最大的工作量"**（每个 API 一个 wrapper + 签名字符串） | **小**（一个格式解析器，~200 行） |
| 每次控件创建的边界调用 | 1 次（2.5~3.7µs × 几十个控件） | **0** |
| 能做任意逻辑 | ✅ 是（这才是它唯一不可替代的价值） | ❌ 只能"声明 + 命名动作" |
| 调试 | 跨语言边界，必须理解 WAMR 内存模型 | 单语言，普通日志 |
| 成熟度 | AOT 在 Xtensa 上仍有未完成项；解释器可用 | 无外部依赖 |

### WASM 唯一不可替代的能力

**让"页面"里跑任意代码 —— 尤其是别人写的代码。** 这正是 AkiraOS 那类项目的定位
（"每个 app 是沙箱化的 .wasm 模块，OTA 部署，无需重烧固件"，含**权限/沙箱**诉求）。

**所以先回答一个问题：你要的是"我自己能改页面"，还是"别人能给我装页面"？**

- 前者 → 数据驱动格式（成本 1/10）
- 后者 → WASM 才是必需的（沙箱、隔离、语言无关）

---

## 四、⚠️ 架构冲突：m5ui 现在的三个设计与 WASM 直接打架

手册说"现有的 flex_layout / handle_touch / render 系统**完全不需要修改**"——
**这句话在 m5ui 里不成立**，因为组件里存的是**裸 C++ 指针**：

```cpp
struct Text        { const char* str = ""; };                    // ← 裸指针
struct OnClick     { void (*fn)(void* ctx); void* ctx; };        // ← 裸函数指针
struct CustomDraw  { void (*fn)(int32_t,int32_t,int32_t,int32_t,
                                uint32_t,uint32_t); };           // ← 裸函数指针
```

### 冲突 1：`Text` 存的是指针 → **WASM 卸载后悬空**（手册没提）

WASM 传字符串时那个 `char*` 指向 **WASM 线性内存**。若宿主直接 `w.add(e, Text{text})` 存指针：

- 页面加载期间：渲染正常
- **页面按需卸载（你的需求就是"按需加载"）后：World 里的 Entity 还活着，指针已悬空**
  → 渲染时读野内存 → **乱码或崩**

**必须**：wrapper 里把字符串**拷进原生内存**（并管理生命周期；ECS 没有析构，
要么用固定大小的字符串池，要么让 `Text` 改成定长数组）。这是设计级改动，不是"wrapper 里注意一下"。
（手册 §3.3 只提了"边界检查、防越界读"，没提**生命周期**。）

### 冲突 2：`CustomDraw` 是**渲染期回调** → 跨边界调用频率被打破

`render.h` 每个脏实体渲染时调 `cd->fn(...)`。而 m5ui 的关键页面**重度依赖它**：

- 屏上键盘的图标（Backspace / Enter / Shift 箭头）
- 设置页大标题（临时改字号）
- **阅读器整页正文**（一个 `CustomDraw` 画一整页）

WASM 页面不可能提供一个原生函数指针。于是只有两条出路，**都很难看**：
- 让宿主在渲染循环里**回调进 WASM**（每个实体每帧一次跨边界调用）
- 或者给 WASM 提供一组**命名绘图原语**（draw_line / draw_glyph…）—— 那 WASM 就要管像素级绘制

**而这两条都正好撞在手册自己划的红线上**："不要在 WASM 中逐像素操作"、
"边界调用频率控制在每帧几次以内"。**手册的这条约束与 m5ui 的渲染模型是矛盾的。**

### 冲突 3：`on_enter` 是 C++ lambda，跨不了边界

`pages/ime.h`、`connect.h` 里的 `on_enter = [] { refresh(); }` —— lambda 捕获的
是原生上下文。WASM 侧只能导出**具名函数**，所以所有页面钩子都要重写成
"导出函数 + 宿主 dispatch"，并且要定义清楚哪些原生状态可被触达。

---

## 五、分区表：要按 m5ui 现状重写（并认清代价）

m5ui 现状（`partitions.csv`）：

```
nvs,      data, nvs,     0x9000,   0x6000
phy_init, data, phy,     0xF000,   0x1000
factory,  app,  factory, 0x10000,  0x300000     # 固件 1.2MB / 3MB
fonts,    data, 0x40,    0x310000, 0x300000     # ← 自定义 subtype 0x40
# 合计 ~6.06MB，余 ~9.94MB
```

与手册方案的差异：

| 手册 | 现状 | 影响 |
|---|---|---|
| 有 `otadata` | **没有** | 加了才能做 app 级 OTA；不加也不影响 wasm_apps |
| `fonts` 写成 `spiffs` | **`data, 0x40`** | 若代码按 subtype 0x40 找字体分区，**改了会找不到字体** |
| `factory` 从 0x20000 起 | 0x10000 起 | 无所谓，但会浪费一段空洞 |
| `wasm_apps` 6MB | — | ✅ 有空间（余 9.94MB 够） |

⚠️ **改分区表 = 必须全量 `erase-flash`** → **NVS 设置全清、`fonts` 分区要重烧**。
这不是"改个文件"的代价，要提前认。

---

## 六、建议的推进路线

### 路线 A（先做，建议）：页面描述格式 + 原生加载器

**为什么先做这个**：它用最小的代价验证你真正的目标（热更新 + 按需加载），
而且**万一不够用，它的产出（页面数据模型、部署通道）在 WASM 方案里仍然要用**。

最小验证（半天，零新依赖）：

1. 定一个紧凑的页面格式（二进制最省，JSON 最省事）
   ```
   {"w":540,"h":960,"items":[
     {"t":"rect","x":0,"y":0,"w":540,"h":960,"bg":"#fff"},
     {"t":"label","x":20,"y":40,"text":"Hello","fg":"#000"},
     {"t":"button","x":20,"y":100,"w":200,"h":50,"text":"Click","act":"goto:settings"}
   ]}
   ```
2. 写原生加载器：解析 → 调**现成的** `rect()/label()/button()` 工厂 → 进同一个 World。
   `act` 用**命名动作表**（`goto:` / `set:` / `svc:`）映射到原生函数，**不跨边界、不存指针**。
3. **通道现成（已核实）**：`platform/web_files.h` 就是**网页版的 SD 卡文件管理**
   （`<title>SD</title>` / 「SD 卡」目录列表 / 支持删除）→ 往 SD 卡丢一个 `.page` 文件即可。
   配合 SD 上的**断线重载**或热重载，**不碰固件、不碰分区表**。
   （注：m5ui **没有** SPIFFS/LittleFS 分区，`fonts` 是独立只读分区、`web_files` 走 SD。）
4. 验收：改一个 `.page` 文件 → 页面变化，固件与分区表**一个字节没动**。

**局限要认**：页面里不能有任意逻辑（循环、算法）。对 UI 页面通常够；
阅读器那种"解析 Markdown 并分页"的属于**逻辑**，继续留在原生侧（它本来就在 `text/markdown.h`）。

### 路线 B（确认要"任意/第三方代码"时才做）：WASM，且按这个顺序

**Phase 0 只验三件事，别按 AOT 设计**：

1. `CONFIG_WAMR_ENABLE_LIB_PTHREAD=n` 后，`create_exec_env` 能成功
2. 从 **SPIFFS 加载 `.wasm`**（不是 .aot）能跑起来
3. 一个 native 回调（如 `ui_log`）能从 WASM 调通

**Phase 1** 才考虑 AOT，且必须用 `wamrc --xip` + `--enable-builtin-intrinsics=i64.common`
（ESP32-S3 有 FPU，fp 类不必），并预期 `.rodata` 重定位仍有坑。

**Phase 2 之前先解决 §四 的三个冲突**（字符串生命周期、CustomDraw、页面钩子），
否则会做出一个"能跑但不稳定"的东西。

---

## 七、三条路的真实对照（正面回答"跟我手写 OS 有啥区别"）

**能"加能力"的插件机制，都躲不掉"定义 API 面"这一层。** 关键差别是这层多厚：

| | 页面数据格式 | **原生 .so（elf_loader）** | WASM（WAMR） | OTA |
|---|---|---|---|---|
| **页面能加固件没有的能力?** | ❌ **不能**（换皮） | ✅ 能（任意原生代码） | ✅ 能 | ✅ 能（就是固件） |
| **要写 API 面?** | 要（动作表，薄） | 要，**但极薄**：`symbol.py` **自动生成**导出符号表 | 要，**且厚**：每个 API 手写 wrapper + 签名字符串 | **不用** |
| 工具链新增 | 无 | **无**（现有 xtensa 工具链 + `symbol.py`） | LLVM+Xtensa + `wamrc` + wasi-sdk | 无 |
| 额外 RAM | 0 | **PSRAM 执行（S3 官方支持 ✓）** | ≥256KB 池；**AOT 还要内部 RAM** | 0 |
| `CustomDraw`/字符串/原生回调 | 不适用 | **全部原生可用** → §四 三个冲突**全部消失** | 三个冲突全在 | — |
| 隔离 / 沙箱 | — | ❌ 无（自己的代码通常无所谓） | ✅ 有（**第三方代码才需要**） | — |
| 更新粒度 | 一个 `.page` | 一个 `.so`（几 KB） | 一个 `.wasm` | 整个固件 1.2MB |
| 要重启? | 不用 | **不用**（`dlopen` 热加载） | 不用 | **要**（2~3s） |

### 7.1 `espressif/elf_loader` 已核对事实

| 项 | 事实 |
|---|---|
| 组件与版本 | `espressif/elf_loader` **v1.3.3**（注册表有） |
| 支持 SoC | ESP32-S2 / **S3** / S3x1 / P4 / C6 / C61，**"support running ELF in PSRAM"** |
| API | `dlopen / dlsym / dllist / dlclose` + `esp_elf_relocate()`；v1.3.1 起有 `elf_set_symbol_resolver()` 可自定义解析 |
| 定位（官方原话） | "the application **does not need compile into the whole firmware in advance**……like running `./main.o` on Ubuntu" |
| **固件侧要做什么** | 用官方 **`symbol.py` 生成导出符号表** —— 声明"哪些函数/变量可被 ELF app 调用" |
| 示例 | `elf_loader_example`、`elf_console_example`（控制台 `mod_load xtensa/lib.so`，目录名直接是 `xtensa`，印证 Xtensa 支持） |
| 所在仓库 | `espressif/esp-iot-solution`（registry 发布）；另有较旧的 `esp_elf`，**用带 `dlopen` 的新版** |

### 7.2 这条路要认的代价

1. **无沙箱**：一个写坏的 `.so` 能崩整个系统。自己代码通常接受；要跑别人的代码就不行。
2. **ABI 耦合**：固件里结构体的布局/语义一改，旧 `.so` 会崩 → 要么冻结 ABI，要么加版本号并在
   `dlopen` 时校验拒绝。
3. **符号表随固件走**：新增一个可导出 API 要重新生成符号表（但**不用改桥接代码**）。
4. **C++ 特性的支持边界要实测**：静态构造、异常、TLS 在 .so 里的支持程度官方没承诺到细节。

### 7.3 什么情况下选哪条

| 你的真实诉求 | 选 |
|---|---|
| 只要"不用插线就能更新" | **OTA**（零成本；代价=整包 1.2MB + 重启） |
| "页面要能加新能力 + 不刷固件 + 不写 OS" | **`elf_loader` 原生 .so**（§7.1，最贴合） |
| "要跑**别人的**、不信任的代码"（真·app 平台） | **WASM**（桥接层的代价此时是必要的，因为它就是安全边界） |
| "只想改改布局/文字" | 页面数据格式（但这不是你的目标） |

---

## 八、变更记录

| 日期 | 版本 | 变更 |
|---|---|---|
| 2026-10-08 | v1.0 | 首版。对来稿 WASM 手册的事实核查、AOT/XIP 真实状态、两路线成本对照、m5ui 的三个架构冲突、分区表差异 |
| 2026-10-08 | v2.0 | **推翻初版的核心建议**。用户指出"数据格式不能加能力"，成立。新增 §八：三条路（数据格式 / **elf_loader 原生 .so** / WASM / OTA）的能力上限与"API 面厚度"对照；核实 `espressif/elf_loader` v1.3.3 支持 S3 + PSRAM + `symbol.py` 生成符号表。**新的推荐 = 原生 .so** |
| 2026-10-08 | v1.1 | 补三条已核对事实：① 示例 `(uint16_t)color` 与 m5ui 的 **24bit** 配色冲突 ② `$` 签名**自带**边界检查（手册说反） ③ 官方示例默认 target 是 c3。补 AOT 失败机理（`MALLOC_CAP_EXEC` 只能内部 RAM）+ XIP 恢复步骤（§2.3） |

---

## 附：核实用到的一手来源

| 事实 | 来源 |
|---|---|
| 组件与版本存在 | ESP Component Registry `espressif/wasm-micro-runtime` v2.4.0~1 |
| `xtensa` 是 wamrc 的合法 target | `bytecodealliance/wasm-micro-runtime` → `wamr-compiler/main.c`（`--target=` 帮助文本列出 xtensa） |
| Xtensa 构建脚本存在 | 同 repo → `wamr-compiler/build_llvm_xtensa.sh`（内容：`build_llvm.sh --platform xtensa`） |
| `LIB_PTHREAD` 默认开会导致 `create_exec_env` 失败 | 独立实践者记录（Zenn「ESP32でWASMアプリを動かす」）；registry 版 Kconfig 默认 AOT/pthread/WASI/AppFramework 全开 |
| AOT 在 ESP32 上 `mmap memory failed` | WAMR discussion #1093 |
| XIP 存在且正是为此设计 | WAMR `doc/xip.md`（含 `wamrc --xip` 与 `--enable-builtin-intrinsics` 用法、以及 `.rodata` 重定位尚未完成的说明） |
| AOT vs 解释器倍率 8.8~22× | WAMR wiki: Performance（matrix / gimli / CoreMark） |
| 运行时体积 29.4K(aot) / 56.3K(classic) / 58.9K(fast interp) | 同 repo 与 registry README |
| AOT 只从内部 RAM 申请可执行内存 | `core/shared/platform/esp-idf/espidf_memmap.c`（`MALLOC_CAP_EXEC`） |
| Kconfig 默认值（AOT/INTERP/LIB_PTHREAD/WASI/APP_FRAMEWORK 全 y；无 BOUND_CHECK） | `build-scripts/esp-idf/wamr/Kconfig` |
| `aot_reloc_xtensa.c` 存在 | `core/iwasm/aot/arch/` |
| m5ui 配色是 24bit RGB888 | `main/ui/theme.h`、`main/systems/render.h` |
