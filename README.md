# papers3-ecs-ui-framework

> **📖 文档总索引：[`docs/README.md`](docs/README.md)** —— 一个问题域一份权威，先在那找。
>
> 常用三份：**开发总册** [`docs/guide/DEVELOPER.md`](docs/guide/DEVELOPER.md)（环境/构建/烧写/硬约束）·
> **UI 设计手册** [`docs/design/UI_DESIGN.md`](docs/design/UI_DESIGN.md)（设计规则 + 风格，唯一权威）·
> **`.so` 应用开发** [`docs/guide/APP_AUTHORING.md`](docs/guide/APP_AUTHORING.md)。
>
> 本文只讲架构概览和文件结构。

面向 M5PaperS3（ESP32-S3 + 8 位并行直驱 16 灰墨水屏）的 **ECS 驱动 UI 框架 + 原生 `.so` 应用宿主**。

基于 ESP-IDF 构建（不是 Arduino）。M5GFX / M5Unified 以 ESP-IDF 组件方式引入。

---

## 三块拼图

| 块 | 是什么 |
|---|---|
| **ECS 框架** | 实体/组件/系统；固件页（启动器、输入法）由它驱动 |
| **`.so` 应用宿主** | 固件 `dlopen` 应用、按约定调 `page_init`/`page_tick`；应用自带图标与元信息（`.m5meta`） |
| **双核分工** | UI 独占 **core1**，WiFi / lwIP / httpd / EPD 推屏留 **core0**（见 `docs/architecture/DUAL_CORE_SPLIT.md`） |

## 架构：ECS

```
Entity（实体）    = uint16_t 编号，没有类继承
Component（组件） = 纯数据结构体（Rect、Text、Style、Touchable、OnClick …）
System（系统）    = 按组件组合查询实体、并执行一件事的函数
```

没有继承，没有虚函数，没有 DOM 树。

- **按钮** = 挂了 `Rect + Text + Style + Touchable + OnClick` 的 Entity
- **标签** = 挂了 `Rect + Text + Style` 的 Entity
- **Flex 容器** = 挂了 `Rect + FlexLayout + Children` 的 Entity

> 详细的 ECS 思路、按钮如何「组装」出来、以及和 OOP 继承写法的对比，见
> [`docs/guide/DEVELOPER.md`](docs/guide/DEVELOPER.md) 末尾的「ECS 架构说明」附录。

## 两种界面载体

| 载体 | 在哪 | 现状 |
|---|---|---|
| **固件页** | `main/pages/` | `launcher.h`（启动器，终端风）· `ime.h`（输入法）· `manager.h`（页面管理） |
| **`.so` 应用** | `apps/settings` `apps/terminal` `apps/reader` | 独立编译、上传到 SD 的 `/apps`，启动器里点进去 |

两者用**同一套视觉语言**（终端风）；设计规则只有一份：`docs/design/UI_DESIGN.md`。

## 中文字体

内置**鸿蒙 HarmonyOS Sans SC**（GB2312 全 6763 汉字 + ASCII 全套），任意字号实时栅格化。

字体存在 flash 的 **3MB 独立分区**里，用 `esp_partition_mmap` 零拷贝映射（裁切后 1.93MB，**运行时零 RAM、零拷贝**）；换字体只需换 SD 卡里的 ttf，或重烧该分区。

> 字体约束（stb_truetype 只认 glyf 不认 CFF）、分区表、裁剪脚本、踩过的坑，见
> [`docs/display/TTF_FONT.md`](docs/display/TTF_FONT.md)。

## 文件结构

```
papers3-ecs-ui-framework/
├── CMakeLists.txt              ← ESP-IDF 工程根
├── partitions.csv              ← 自定义分区表（含 3MB fonts 分区，给 TTF 用）
├── sdkconfig / sdkconfig.defaults
├── pinyin.idx                  ← 可升级的拼音词库（拷到 SD 卡 /papers3/pinyin.idx）
├── ttf——file/                   ← 字体源文件（HarmonyOS_SansSC-subset.ttf 等）
├── main/                       ← 固件（宿主）
│   ├── main.cpp                ← app_main()：初始化 + ui_task 钉 core1
│   ├── app.h                   ← App 类（每帧：M5.update → 反馈音 → 布局 → 触摸 → 渲染）
│   ├── plugin.cpp / plugin.h   ← `.so` 宿主：dlopen / 符号导出 / API 版本握手
│   ├── ecs/                    ← World / 组件（world.h、world.hpp、components.h）
│   ├── systems/                ← render / touch / flex / power / epd_lut
│   ├── ui/                     ← 固件 UI 层
│   │   ├── theme.h             ← 设计令牌（Neo-Brutalist 层）
│   │   ├── widgets.h           ← 工厂函数（rect/label/bitmap/button/flex/input_frame/cursor/toggle…）
│   │   ├── icons.h             ← 图元图标（箭头 / 退格 / A-a）
│   │   ├── keyboard.h          ← 屏幕键盘组件（输出端可插拔）
│   │   ├── candidate_bar.h     ← 候选栏组件
│   │   └── qr.h                ← 二维码绘制
│   ├── pages/                  ← 固件页面
│   │   ├── manager.h           ← 页面管理（多页共存，切页只切 Visible）
│   │   ├── launcher.h          ← 应用启动器（读 `.m5meta` 显示图标/名称/描述）
│   │   └── ime.h               ← 输入法页
│   ├── platform/               ← sd / nvs / wifi_ap / wifi_sta / wifi_list / web_files
│   │   ├── apps.h              ← 扫 /apps 下的 .so + 读 `.m5meta`
│   │   ├── sound.h             ← 触摸反馈音（内置短哔 / SD WAV）
│   │   └── rtc_time.h          ← RTC 供时 + 网络校准
│   ├── text/                   ← ttf / ime / pinyin_store / pinyin_index / markdown / txt
│   └── third_party/stb_truetype.h
├── apps/                       ← `.so` 应用（各自独立编译）
│   ├── common/                 ← so_api.h（宿主接口声明）+ so_meta.h（图标/名称/描述）
│   ├── settings/               ← 设置六屏 + 子页
│   ├── terminal/               ← 终端三屏
│   └── reader/                 ← 阅读器
├── components.lock             ← ★ 第三方组件锁定清单（版本号 + 补丁 sha256，**进 git**）
├── components/                 ← ★ 第三方组件（**不在 git 里**，跑 tools/fetch_components.sh 按 lock 还原）
│                                 m5gfx / m5unified / elf_loader / cmake_utilities
├── patches/                    ← ★ 对第三方库的本地改动（5 个文件的 diff，进 git）
├── tools/                      ← 字体裁剪 / 拼音索引 / 屏保出图 / 分章扫描 / 拉组件
└── docs/                       ← 全部文档（总索引见 docs/README.md）
    ├── guide/                  ← DEVELOPER.md（开发总册）+ APP_AUTHORING.md（.so 应用）
    ├── design/                 ← UI_DESIGN.md（设计唯一权威）+ 来稿 + 设计稿
    ├── display/                ← 刷新 / 波形 / 字体
    ├── architecture/           ← ECS 额度 / 插件架构 / 双核分工 / 阅读器 API 面
    ├── reader/                 ← TXT 切块分章规则
    └── research/               ← 已完结的调研归档
```

## 构建

```bash
# ★ 第一次 clone 后先还原第三方组件（m5gfx / m5unified / elf_loader / cmake_utilities）
#   它们不在 git 里 —— 仓库只留 components.lock（钉死版本）+ patches/（我们的改动）。
#   脚本按 lock 去上游拉对应版本，先拿官方 CHECKSUMS.json 逐文件核 sha256，再打本地补丁；
#   版本不对 / 内容被换 / 补丁打不上，都会当场报错，不会"跳过继续"。
./tools/fetch_components.sh

# 校验现在这套第三方代码就是 lock 里那一版（不下载、不改动）
./tools/fetch_components.sh --verify

# 指定芯片型号并编译烧录
idf.py set-target esp32s3
idf.py build flash monitor

# `.so` 应用单独编（各自目录下有 build.sh）
cd apps/settings && ./build.sh
```

## 用法（固件页）

```cpp
#include "app.h"
#include "ui/widgets.h"

using namespace papers3;
using namespace papers3::ui::widgets;

void on_click(void* ctx) {
    // 处理点击
}

extern "C" void app_main() {
    App app;
    app.begin();
    auto& w = app.world;

    rect(w, 0, 0, 540, 960, theme::BG_WHITE);           // 背景
    Entity lbl = label(w, 20, 40, "Hello ECS", theme::FG_BLACK);
    button(w, 20, 100, 200, 50, "Click me", on_click);  // 按钮 = 组件组合

    Entity row = flex(w, 20, 200, 500, 60, FlexLayout::ROW);
    add_child(w, row, button(w, 0, 0, 140, 50, "A", nullptr));
    add_child(w, row, button(w, 0, 0, 140, 50, "B", nullptr));

    while (true) {
        app.update();
        delay(1);
    }
}
```

## 加新功能 = 加新组件

想要「可拖拽的标签」？加一个 `Draggable` 组件 + 一个 `drag_system`。想要「动画」？加一个 `Tween` 组件 + `tween_system`。不用改已有代码，也不用写新的子类。

> **但 `.so` 应用不能给 World 加新组件类型**（组件表是固件编译期定的）—— 应用要整页自绘用 `CustomDraw`。
> 见 [`docs/guide/APP_AUTHORING.md`](docs/guide/APP_AUTHORING.md) 硬约束。

---

## 每帧流程

```
ui_task（core1，prio 5）—— 全项目唯一读触摸的地方
  app.update()
    ├─ M5.update()          ← 读 GT911（单一读者）
    ├─ sound::click()       ← 按下沿 → 反馈音（限流 60ms）
    ├─ power::tick()        ← 熄屏时这帧只等唤醒
    ├─ flex_layout(world)   ← 遍历 (FlexLayout + Children)，算子节点坐标
    ├─ handle_touch(world)  ← 遍历 (Rect + Touchable)，命中检测
    └─ render(world)        ← 遍历 (Rect + Style + Dirty)，画 + 按足迹推屏
  pages::flush_band()       ← 覆盖层（键盘）整条 QUALITY 推一次
  pages::launcher::tick()   ← 启动器交互
  plugin::tick()            ← 当前 `.so` 应用的 page_tick()

core0：WiFi(23) + sys_evt(20) + esp_timer(22) + lwIP + httpd + EPD 推屏
```
