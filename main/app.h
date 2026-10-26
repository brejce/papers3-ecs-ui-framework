#pragma once

#include "ecs/world.hpp"
#include "ecs/components.h"
#include "systems/render.h"
#include "systems/touch.h"
#include "systems/flex.h"
#include "systems/power.h"
#include "ui/keyboard.h"        // kb::tick_g（键盘自持触摸轮询）
#include "ui/candidate_bar.h"   // cb::tick_g（候选栏自持触摸轮询）
#include "platform/rtc_time.h"  // 时间：RTC 供时 + 网络校准
#include "platform/sound.h"     // 触摸反馈音
#include <M5Unified.h>

namespace papers3 {

// 输入法覆盖层的每帧自持轮询（编辑条右侧 × 关闭按钮），由 ime.h 的 build() 登记。
// 为什么走函数指针、不在 app.h 里直接 include ime.h：ime.h → manager.h → app.h，
// 反向 include 会撞上「class App 还没定义 manager.h 就用 App*」的编译顺序问题。
inline void (*g_ime_tick)() = nullptr;

// ──────────────────────────────────────────────
// App — 主循环
//
// 职责：
//   1. 初始化 M5Paper
//   2. 每帧跑 systems：flex → touch → render
//   3. 管理 World
// ──────────────────────────────────────────────

class App {
public:
    ecs::World world;

    void begin() {
        auto cfg = M5.config();
        // 注意：cfg.serial_baudrate 只在 Arduino 构建下存在
        //（M5Unified 里被 #if defined(ARDUINO) 包住），ESP-IDF 下没有这个字段
        cfg.clear_display = false;   // 首屏自己刷（下面 QUALITY 整屏推一次）
        cfg.output_power = true;
        M5.begin(cfg);
        M5.delay(500);

        // 字号：默认 6×8 在 540×960 上太小了
        M5.Display.setFont(&fonts::Font4);

        // 刷新完全自己控制：绘制只改帧缓冲，什么时候推哪块由 render 决定
        //（关掉自动刷新，否则每个绘制调用都会触发一次物理刷新）
        M5.Display.setAutoDisplay(false);

        // 首屏：QUALITY 整屏刷一次白，把开机残留的旧内容清掉
        M5.Display.setEpdMode(epd_mode_t::epd_quality);
        M5.Display.fillScreen(TFT_WHITE);
        M5.Display.display(0, 0, M5.Display.width(), M5.Display.height());
        M5.Display.waitDisplay();    // 首屏等波形跑完（约 1s），免得后面的 push 排队打架

        // 时间：开机先把板载 RTC 读进系统时钟（网络只做校准，见 platform/rtc_time.h）
        rtctime::begin();

        _running = true;
    }

    void update() {
        if (!_running) return;
        M5.update();
        // ★ 触摸反馈音：只认「0 → 1」这个物理按下沿。
        //   放在这里（而不是各页的命中代码里）→ 固件页、键盘、.so app 全都只响一次，
        //   不会因为同一次按下被多个消费者看到而连响。
        {
            const int n = M5.Touch.getCount();
            if (n > 0 && _prev_touch == 0) papers3::sound::click();
            _prev_touch = n;
        }
        rtctime::tick();                            // 网络校准到了 → 写回 RTC（I2C 只在主循环里做）
        if (!systems::power::tick(world)) return;   // 熄着屏：这帧只等唤醒（见 power.h）
        systems::flex_layout(world);
        systems::handle_touch(world);
        ui::kb::tick_g();                           // 键盘自持触摸（不挂 Touchable，见 keyboard.h）
        ui::cb::tick_g();                           // 候选栏自持触摸（同上）
        if (g_ime_tick) g_ime_tick();               // 覆盖层 × 关闭按钮（ime.h 登记）
        systems::render(world);
    }

private:
    bool _running = false;
    int  _prev_touch = 0;     // 上一帧触点数（0→1 = 按下沿 → 反馈音）
};

} // namespace papers3