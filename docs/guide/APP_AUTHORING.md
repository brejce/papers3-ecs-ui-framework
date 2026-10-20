# `.so` 应用开发手册（建 · 编 · 传 · 验）

> **这份管什么**：怎么给 m5ui 写一个新的原生应用（`.so`）、怎么编、怎么传上设备、怎么验。
> **不在这份里**：界面长什么样 → `../design/UI_DESIGN.md`；插件加载器的符号解析机制 → `../architecture/ELF_PLUGIN_ARCHITECTURE.md`。
> 整理：2026-10-19（吸收了原 `design/terminal-style/STYLE.md` §11 部署须知）。

---

## 0. 一句话

应用就是一份**独立的 `.so`**：固件 `dlopen` 它，按约定调 `page_init()`，之后每帧调 `page_tick()`（可选）。
它通过 `so_*` 白名单符号调固件能力（画图、字体、文件、WiFi、输入法），**不 include 任何 IDF 头**。

现有三个：`apps/settings`（设置六屏）、`apps/terminal`（终端三屏）、`apps/reader`（阅读器）。

---

## 1. 目录结构

```
apps/
  common/
    so_api.h        ← 宿主导出接口的声明（所有 app 共用）
    so_meta.h       ← .m5meta 元信息宏（图标/名称/描述）
  settings/
    build.sh        ← 编译脚本（含符号白名单闸门）
    main/settings.cpp
    build/settings.so
  terminal/
    build.sh
    main/terminal.cpp
    push-terminal.sh  ← 带产物闸门的推送脚本（源码比 .so 新就拒传）
  reader/
    build.sh
    main/{reader.cpp, layout.h, txt_parse.h, md_parse.h, chapter.h}
    build/reader.so
  main/             ← 宿主编译用的另一套工程（reader_test.c），不是 app
```

**每个 app 的界面代码是自持的** —— 风格 helper（`draw_hero`/`draw_seg`/`draw_li`…）在各自 `.cpp` 里实现一份，`apps/common/` 只有 `so_api.h` 和 `so_meta.h`（见 `UI_DESIGN.md` §0 的警告）。

---

## 2. 一个 app 必须/可选导出的符号

| 符号 | 必需 | 说明 |
|---|---|---|
| `so_app_api_version()` | ✅ | 返回 `SO_API_VERSION`。固件在 `dlopen` 之后、`page_init` **之前**核对 |
| `page_init()` | ✅ | 入口：建界面、开文件、画首屏 |
| `page_tick()` | 🔲 | 每帧回调（自持交互）；没有就是一次性页面 |
| `so_app_name()` | 🔲 | 显示名（没有用文件名）。现在没用——名字走 `.m5meta` |
| `page_ctor_check()` | 🔲 | 可选的加载自检 |

```cpp
#include "so_api.h"

extern "C" __attribute__((visibility("default")))
uint32_t so_app_api_version(void) { return SO_API_VERSION; }

extern "C" __attribute__((visibility("default")))
void page_init(void) { /* 画首屏 + 挂触摸轮询 */ }

extern "C" __attribute__((visibility("default")))
void page_tick(void) { /* 每帧：读触摸、推变化的那几块 */ }
```

### API 版本握手（有意设计的硬失败）

不导出版本 / **主版本不一致** / **`.so` 比固件新** → 固件**明确拒绝加载**，原因写到**串口和屏幕上**（launcher 显示一行错误），而不是跑到一半闪退白屏。

- 改 `apps/common/so_api.h` 的版本号时，**同步改固件 `main/plugin.h` 的 `API_VERSION_MAJOR/MINOR`**，并把 `apps/` 下的 `.so` 全部重编。
- **MINOR 只对「app 比固件新」生效** → 老 `.so`（minor 更小）依然能加载。

当前：`MAJOR = 1`，`MINOR = 13`。

---

## 3. 自带图标与元信息

见 `../design/UI_DESIGN.md` 附录 B（格式 + 视觉规范 + 三个坑）。写一行即可：

```c
#include "so_meta.h"
SO_APP_META("系统设置", "网络 · 显示 · 字体 · 电源", 32, 32, 1, 0x00, 0xFF, ...);
```

固件**不加载 `.so`**，只读 ELF 节表里的 `.m5meta` 节，所以元信息必须**定长、自包含、与地址无关**。

---

## 4. 编译（`build.sh`）

### 4.1 关键编译/链接参数

```bash
CPPFLAGS="-fPIC -fno-exceptions -fno-rtti -std=gnu++2b -fvisibility=hidden \
          -ffunction-sections -fdata-sections -Os -Wall -Wno-unused-parameter"
LDFLAGS="-shared -fPIC -static-libgcc -nostdlib -nostartfiles \
         -fdata-sections -ffunction-sections -Wl,--gc-sections -fvisibility=hidden \
         -Wl,--strip-all -Wl,--strip-debug -Wl,--strip-discarded \
         -Wl,--allow-shlib-undefined"
```

**软浮点这个坑要记住**：`-nostdlib` 会把 `libgcc` 一起排除，此时 `-static-libgcc` 只是「若链则静态」，并**不会**把它拉进来。用了 `double`/`float` 就会出现 `__muldf3`/`__divdf3` 这类未定义符号 → `dlopen` 报 `Can't find symbol __muldf3`。

- **默认不链 `-lgcc`**：改数值逻辑尽量全用 32 位整数；
- 真要用 `double`：必须显式 `-lgcc` 且写在 `.o` 之后，并接受链接器收尾时 `double free or corruption` 崩掉（信号 6，`.so` 其实已写出）。运行时 float 对 EPD UI 没有意义，**能不碰就不碰**。

### 4.2 符号白名单闸门（编译通过 ≠ 能加载）

`elf_loader` 的 libc 是**逐符号白名单**，漏一个只在 `dlopen` 那一刻炸。所以 `build.sh` 末尾会列出所有未定义符号、核对白名单，**不在白名单里就直接失败**：

```
✗ 白名单外符号: __muldf3  → dlopen 会失败
```

加新符号时改 `build.sh` 的 `ALLOW` 变量。**绝不能把 `__*` 一律跳过** —— 软浮点 helper 就是这么漏过去的（伪符号只跳 `_GLOBAL_OFFSET_TABLE_`、`__bss_start`、`__init_array*` 那几个）。

---

## 5. 上传 · 复位 · 验证

### 5.1 宿主**不重复加载**同名模块（所以必须复位）

固件 `dlopen` 的查重逻辑按**去扩展名模块名**查已加载列表，命中就直接返回 RAM 里那个旧模块，**不重新加载**；`plugin::close()` 也不 `dlclose`。

> **上传新 `.so` 之后必须复位设备**，新代码才生效。

### 5.2 命令

```bash
# 上传（IP 手给 —— 自动扫描会误中同网段电脑上的 Memos 服务）
curl -X POST "http://192.168.3.57/api/ul?path=/apps&name=settings.so" \
     --data-binary @apps/settings/build/settings.so

# 回读校验（必须做）
curl "http://192.168.3.57/api/dl?path=/apps/settings.so" -o /tmp/_v.so && cmp build/settings.so /tmp/_v.so

# 复位（串口一打开设备就重启一次，复位靠的就是这个）
python -m esptool --chip esp32s3 -p /dev/ttyACM0 --after hard_reset flash_id
```

### 5.3 验证一个 `.so` 的四件事

1. **回读 `cmp` 一致** —— 传输没坏；
2. **复位后串口日志**出现 `APPS: /sdcard/apps: N 个 .so`，且没有 `页面初始化失败` / `API 版本` 报错；
3. **屏幕**上 launcher 那一行有图标 + 名称 + 描述（元信息被读到了）；
4. **点进去**、退出，看有没有 `dlopen` 阶段的报错。

> 画质/残留**只能肉眼看屏**，串口只证明「没崩」。

### 5.4 验证 `.so` 里的中文字符串

用 `grep -a`（二进制模式），**不要用 `strings`** —— strings 按 ASCII 可打印段切，UTF-8 中文会被切断漏掉。

### 5.5 产物闸门

`apps/terminal/push-terminal.sh` 会检查 `terminal.cpp` 是否比 `terminal.so` 新（源码改了没编就拒传），防把旧产物传上去。

---

## 6. 宿主与 app 的交互约定

### 6.1 触摸是**单一读者**

触摸屏（GT911）全项目只有一处读：固件主循环的 `app.update()` → `M5.update()`，加上 `.so` 自己的 `so_touch()` / `so_wait_display()`。

- `so_touch()` 带**「补报」语义**：调一次就消耗一格。**只有 `page_tick` 该调它**；
- 别的地方（比如预取绘制里「摸到手指就收手」）用**不带补报**的那个读法，否则会把给 `page_tick` 准备的按下沿吃掉。

### 6.2 推屏

- `so_set_epd_mode(mode)` 决定**写入时怎么量化**：`1=QUALITY / 2=TEXT / 3=FAST / 4=FASTEST`；
- `so_fill_rect` / `so_font_draw` 写像素 → `so_display()` **按足迹推送**；
- 画与推**必须同档**；切屏才整屏 QUALITY（见 `UI_DESIGN.md` §9）；
- 每次推送**带原因**（`so_log_file` 落 SD）—— 面板会把「上次推屏之后写过的像素」并成一个大矩形，只能靠这条日志定位。

### 6.3 页面与覆盖层

- 键盘/输入法是**覆盖层**（`so_ime_open`），不是切页；
- `.so` 独占屏时固件的 touch 系统**直接 return** —— 所以覆盖层里的 `×` 关闭按钮也不能挂 `Touchable`，靠自持轮询。

### 6.4 内存

- `.so` 里的 `malloc` 只能退**内部 RAM**（很紧）；
- 大块（页缓存、书、位图）用 **`so_psram_alloc`**（8MB PSRAM）；
- 字符串格式化用 `so_snprintf`（白名单里没有 `snprintf`）。

---

## 7. 硬约束（做 app 前先看）

1. **插件不能给 ECS World 添加新的组件类型** —— 组件类型表是固件编译期定的。app 要自绘就整页 `CustomDraw`（阅读器就是这么干的）。
2. **模块只跟宿主说话**，别让 `.so` 互相调私有函数（宿主符号会遮蔽模块符号，顺序决定结果）→ 见 `../architecture/ELF_PLUGIN_ARCHITECTURE.md`。
3. **别在运行时写 flash 分区**：字体分区只放默认字体，运行时用 SD → PSRAM。
4. **USB-OTG 与 USB-Serial-JTAG 共用一个 PHY**：开了 USB 设备，`/dev/ttyACM0` 就没了（烧写要手动进下载模式）。
5. **上传后必须复位**（§5.1）。

---

## 8. 新建一个 app 的清单

1. `cp -r apps/settings apps/<name>`（或从空目录起）→ 改 `build.sh` 里的文件名与 `ALLOW`（先按需要补符号，闸门会告诉你缺哪个）；
2. 写 `main/<name>.cpp`：`so_app_api_version` + `page_init`（+ `page_tick`）；
3. 加 `SO_APP_META(...)`（名称/描述/32×32 图标，见 `UI_DESIGN.md` 附录 B）；
4. 界面按 `../design/UI_DESIGN.md` 写（令牌、部件、刷新纪律）；
5. `./build.sh` —— 过了白名单闸门才算编好；
6. 上传 + 回读 `cmp` + 复位 + 看串口和屏幕（§5.3）。
