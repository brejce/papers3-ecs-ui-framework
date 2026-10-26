# 原生 .so 插件架构（`esp_elf_loader`）—— 符号解析与模块间依赖

> 编写 2026-10-08。**所有结论都对过源码**，不是按通用 ELF 知识推的。
> 源码基准：`espressif/esp-iot-solution` → `components/elf_loader/`（registry 版 `espressif/elf_loader` v1.3.3）。
> 配套：`../research/WASM_FEASIBILITY.md`（四条路线对照）、`ECS_OPTIMIZATION.md`（同目录）、原 `ARCHITECTURE.md` 已并入 `../guide/DEVELOPER.md` 附录。

---

## 一、回答两个问题

### Q1：.so 要调固件能力，需要 API symbol 吗？→ **需要**

符号解析函数 `elf_find_sym_default()`（`src/esp_elf_symbol.c`）按**固定 5 步**查：

```c
uintptr_t elf_find_sym_default(const char *sym_name)
{
    // 1. libc 表        (CONFIG_ELF_LOADER_LIBC_SYMBOLS,  默认 y)
    // 2. ESP-IDF 表     (CONFIG_ELF_LOADER_ESPIDF_SYMBOLS, 默认 y)
    // 3. ★ 你的表        g_customer_elfsyms[]  (CONFIG_ELF_LOADER_CUSTOMER_SYMBOLS)
    extern const struct esp_elfsym g_customer_elfsyms[];
    // 4. ★ 运行期注册的表 esp_elf_find_symbol()   ← 反向注册序，最后注册的赢
    // 5. ★ 已加载的其他 .so  dlmod_getaddr()
}
```

**第 4 步是真接口面**，它比第 5 步更好用（见 §三）：

```c
int  esp_elf_register_symbol(esp_elf_symbol_table_t *table);    // 注册一张符号表
int  esp_elf_unregister_symbol(esp_elf_symbol_table_t *table);  // 注销
uintptr_t esp_elf_find_symbol(const char *name);                // 查找
// 注释原文：Search order is reverse registration order (latest registered first).
```

→ **加载一个模块时注册它的接口表、卸载时注销** = 你自己实现的"服务注册表"，
   而且**"后注册的赢"天然支持热替换**（新版本先注册，解析就指向新的）。

**所以固件必须显式声明"哪些函数/变量能被 .so 调用"** —— 就是第 3 步那张
`g_customer_elfsyms[]`，用官方 **`symbol.py`** 生成（可基于 ELF 文件或静态库）。

**注意可见性是白名单制**：没进表的固件函数，.so 里引用它会**在加载/重定位时报未解析符号**
（或按 `RTLD_LAZY` 拖到调用时才炸）。所以要按需把 `rect / label / button / …` 逐个加进表。

> 因为第 1、2 步默认开着，.so 里**直接就能** `printf` / `malloc` / `strcmp` / lwIP 那套，
> 不用你自己包一层。这省掉一大块工作量。

### Q2：一个 .so 要调用**另一个 .so** 的能力呢？→ **支持**，但它在解析链的**最后一步**

`dlmod_getaddr()`（`src/dlso/dlmod.c`）的注释写得很直白：

> **"Find a symbol address by name across all loaded modules."**

实现是遍历一个全局已加载模块链表（`g_dlmod_slist_head` + `SLIST_FOREACH`），
逐个查各模块的 `symtab`：

```c
SLIST_FOREACH(current, &g_dlmod_slist_head, next) {
    if (current->elf && current->elf->symtab && current->elf->num) {
        for (uint16_t i = 0; i < current->elf->num; i++) { /* 比名字 → 返回 addr */ }
    }
}
```

**所以 so→so 是能做的。** 但下面三个后果决定了架构该怎么做。

---

## 二、必须知道的三个后果

### 后果 1：**宿主符号会"遮蔽"模块符号**（顺序决定的）

解析链里第 3 步（你的表）**排在**第 5 步（其他 .so）**前面**。所以：

> 如果固件导出了 `books_progress`，而某个 .so 也导出了同名符号，
> **任何模块引用 `books_progress` 拿到的都是固件那一份。**

→ **每个模块的导出符号名必须全局唯一**（这层命名空间是扁平的）。
→ 实践规则：**给导出符号加模块前缀**（`books_*`、`render_*`、`page_*`）。

### 后果 2：**没有自动依赖加载**（没有 ELF 的 `DT_NEEDED`）

加载器不认识"我需要 B.so"这种声明。**必须按拓扑序自己 `dlopen`**：

```c
dlopen("/sdcard/libbooks.so",  RTLD_NOW);   // 被依赖的先加载
dlopen("/sdcard/librender.so", RTLD_NOW);   // 依赖者后加载（它才能解析到 libbooks 的符号）
```

若顺序反了 → 后加载者重定位时找不到符号 → 加载失败（`RTLD_NOW`）。

### 后果 3：**同名模块不会重复加载**（天然去重）

`dlmod.c` 里加载前会查链表：

```
"%s.so has been dynamically loaded"  → 直接返回已有句柄
```

→ 重复 `dlopen("libbooks")` 是安全的、幂等的。
→ 配合 `RTLD_NOLOAD`（"do not load the object"）可以先问"这个模块在不在"，
   这就是你自己实现"按需加载 / 依赖检查"的工具。

### 附带：调试利器

- `dllist(LIST_MODULE)` —— 列出已加载模块
- `dllist(LIST_SYMBOL)` —— **打印所有已加载模块的全部符号和地址**（排查"为什么解析不到"）
- `dlerror()` —— 每个 API 失败都设了可读错误串

---

## 三、⚠️ 架构规则：**模块只跟宿主说话，别让 .so 互相调私有函数**

你举的例子——"一个 .so 管书籍（分类、字数、进度），一个 .so 管书籍内容绘制"——
**技术上做得成**（加载顺序对 + 符号名唯一即可）。**但架构上不建议。**

**理由：直接互调会毁掉你本来想要的那个东西。**

A 调 B 的 `books_get_page()`，那就意味着：
- B 改函数签名 / 改结构体布局 → **A 崩**
- 于是你要维护 **A×B 的兼容矩阵**（每加一个模块，组合数平方增长）
- 而你拆成两个模块的**初衷**是"**各自独立更新**"—— 更新 B 不该牵连 A

**推荐做法（VS Code 扩展、浏览器扩展都是这个模型：扩展之间不能互相调用，只能通过宿主 API）：**

```
            ┌──────────── 宿主固件（唯一稳定接口面）────────────┐
            │  books_*（打开/读页/进度读写）  ui_*（rect/label…） │
            └──────▲──────────────────────────────▲────────────┘
                   │ 只调宿主                       │ 只调宿主
            ┌──────┴───────┐              ┌────────┴─────────┐
            │ 书籍逻辑 .so  │              │  绘制 .so        │
            └──────────────┘              └──────────────────┘
                    （互不认识、互不依赖、可各自单独更新）
```

- 共享状态放**宿主**（NVS / SD / World 里已有的组件），不放某个 .so 的 `.bss`
- 需要跨模块通知 → **宿主中介**：宿主 `dlsym` 拿到各模块的导出入口，按需回调
- 每个模块只依赖**一行**：宿主的 API 表；而不是 N×N 的模块间契约

### 3.1 首选做法：用 `esp_elf_register_symbol()` 做**宿主中介的注册表**

别依赖第 5 步的隐式 `dlmod_getaddr` + 加载顺序（那是不确定的、靠顺序侥幸成立的）。
**用第 4 步做显式注册**：

```c
// 模块加载成功后 —— 由宿主执行，不是模块自己偷偷干
esp_elf_register_symbol(&books_module.public_api);   // 这张表里是 books_* 的入口
// 模块卸载前
esp_elf_unregister_symbol(&books_module.public_api);
```

这样得到三件好处：
- **解析是确定的**，不依赖 `dlopen` 的先后侥幸
- **卸载语义干净**：注销后没人再能解析到已释放的代码（否则就是野指针）
- **中介权在宿主**：注册什么、给谁看，由固件决定 —— 正是 §三 想要的模型

### 3.2 如果确实必须 A 直接调 B（性能 / B 导出一大堆内部函数）

把这三件事一起做：
1. 导出符号**加模块前缀**（`books_*`）—— 避免被宿主静态表遮蔽（§后果 1 是扁平命名空间）
2. **加载顺序显式化**（拓扑）：被依赖的先 `dlopen`
3. 加**版本号**并在 `dlopen` 时校验，防"旧的 A 配上新的 B"

---

## 四、⚠️ 两条 papers3 特有的硬约束（直接影响架构设计）

### 约束 1：**插件不能给 ECS World 添加新的组件类型**

`ecs/world.hpp` 的 `_store<T>()` 是**按被引用的类型实例化**的（编译期）。
固件里没引用过的组件类型，在 ELF 里根本不存在对应存储。

→ "书籍进度""阅读位置"这类**模块私有状态，不能作为新组件塞进 World**。
→ 只能：(a) 放模块自己的 `.data` / `.bss`（就是普通全局变量）；
   或 (b) 放固件**预留的通用槽**（例如加一个 `void* user_data` 型组件，或按类型放定长 blob）。

### 约束 2：**`.so` 一 `dlclose()`，它自己的静态状态就没了**

你特意提到"进度"——这条决定**更新一个模块能不能保住用户数据**：

| 状态放哪 | 更新模块后 |
|---|---|
| 模块自己的 `.bss` | **丢**（卸载即释放） |
| 宿主 / NVS / SD | **保住** |

→ 要么**驻留不卸载**（更新即"重启后生效"），要么**状态外置到宿主/存储**。
→ 推荐后者：它是"热更新"和"数据不丢"能同时成立的前提。

---

## 五、要开的 Kconfig（真名 + 默认值，已对源码）

| 配置项 | 默认 | 为什么 |
|---|---|---|
| `CONFIG_ELF_LOADER` | **y** | 可见范围：ESP32 / S2 / **S3** / S3x1 / C6 / P4 / C61 |
| `CONFIG_ELF_DYNAMIC_LOAD_SHARED_OBJECT` | **n** ← **要手动开** | 不开就没有 `dlopen` 那套 |
| `CONFIG_ELF_FILE_SYSTEM_BASE_PATH` | `"/storage"` | `.so` 的根目录（接 SD 时改它） |
| `CONFIG_ELF_LOADER_LOAD_PSRAM` | **y**（S2/S3/S3x1/P4/C61 且 SPIRAM 时） | **把 ELF 放 PSRAM**，不占内部 RAM |
| `CONFIG_ELF_LOADER_CACHE_OFFSET` | n（**S3 上被自动 select**） | D-cache / I-cache 偏移不同时的必要条件 |
| `CONFIG_ELF_LOADER_LIBC_SYMBOLS` | y | .so 里能直接用 libc |
| `CONFIG_ELF_LOADER_ESPIDF_SYMBOLS` | y | .so 里能直接用 IDF API |
| `CONFIG_ELF_LOADER_CUSTOMER_SYMBOLS` | — | **你的 API 表**（`symbol.py` 生成） |

> 顺带确认了官方 README 的说法："ESP32-S3, support running ELF in PSRAM" —— 对应
> `ELF_LOADER_LOAD_PSRAM` 默认 y。**这条路不跟你那 512KB 内部 RAM 抢**（对比：WASM 的 AOT 要 `MALLOC_CAP_EXEC`，只能用内部 RAM，所以会失败）。

---

## 六、最小验证（建议下一步）

目标：**证明"一个 .so 能从 SD 加载、在屏上建出实体、并且能更新"**。

1. `symbol.py` 导出 3 个符号：`ui_rect` / `ui_label` / `ui_button`（包一层再导出，别直接导内部实现）
2. 写 `page_home.c`（或 .cpp），编成 `xtensa/libpage_home.so`，放进 SD
3. 宿主 `dlopen("libpage_home")` → `dlsym("page_init")` → 调用
4. **看屏：实体出现了吗** ← 这一步是全部价值所在
5. 改 `page_home` 重新编 → 覆盖 SD 上的 .so → 重新加载 → **看屏变了没有，固件一个字节没动**

风险点（要一起验）：
- `.so` 里的 **C++ 静态构造**会不会跑（官方未承诺）
- **字符串常量**在 .so 里的地址（PSRAM 执行 + cache 偏移）有没有问题

---

## 七、变更记录

| 日期 | 版本 | 变更 |
|---|---|---|
| 2026-10-08 | v1.1 | 补第 4 步真身（`esp_elf_register_symbol/unregister/find_symbol`，**反向注册序**）→ 新增 §3.1「用注册表做宿主中介」为首选做法，§3.2 降为次选 |
| 2026-10-08 | v1.0 | 首版。符号解析链 5 步（含 `g_customer_elfsyms` 与 `dlmod_getaddr`）、宿主遮蔽/无自动依赖/同名去重三个后果、模块只跟宿主说话的架构规则、papers3 两条硬约束、Kconfig 真名与默认值 |

## 附：核实用到的源码位置

| 结论 | 文件 |
|---|---|
| 解析链 5 步顺序 | `components/elf_loader/src/esp_elf_symbol.c` → `elf_find_sym_default()`（含 `g_customer_elfsyms` + `dlmod_getaddr`） |
| `elf_set_symbol_resolver()` 可整体替换解析器 | `src/esp_elf.c`（`current_resolver` / `atomic_store`） |
| 运行期注册接口表（第 4 步）+ **反向注册序** | `include/esp_elf.h`（`esp_elf_register_symbol` / `unregister` / `find_symbol` 及其注释） |
| `dlsym` 只在**本模块**符号表里找（无 RTLD_GLOBAL） | `src/dlso/dlfcn.c` |
| `dlmod_getaddr()` = "across all loaded modules" | `src/dlso/dlmod.c` |
| 同名模块不重复加载 | `src/dlso/dlmod.c`（`"%s.so has been dynamically loaded"`） |
| dlopen 模式只有 LAZY/NOW/NOLOAD/DEEPBIND（**无 GLOBAL**） | `include/esp_dlfcn.h` |
| Kconfig 真名与默认值 | `components/elf_loader/Kconfig` |
