#pragma once

// systems/power.h — 熄屏（屏保） / 触摸唤醒 / 断网省电
//
// 三个设置项怎么分工（设置 → 电源与休眠）：
//   ss  屏保     = 到时间把**屏幕**熄掉（画选中的图或黑屏）
//   dp  深度休眠 = 到时间把 **WiFi 断掉**（开关注释里写的"息屏断 WiFi 省电"就是它）
//   idle 熄屏超时 = 多少分钟没摸就算"到时间"（上面两个各管各的，都由它触发）
//
// ★ 屏幕熄掉用的是 `M5.Display.sleep()`：EPD 面板进睡眠但**画面物理保持**（墨水屏
//   天生不断电也留影），触摸一下 wakeup() 恢复驱动。**不是 ESP32 deep sleep** ——
//   真深度睡眠下触摸 INT（GPIO48）不是 RTC 脚、醒不了，而且 POWER_HOLD 要专门保电，
//   做错就是"怎么按都不醒"。那一步等真有需求再说（旧工程 button_wake_test.txt 有底）。
//
// 唤醒后的恢复：睡眠前屏幕上是一整张图，UI 的增量刷新没法收拾 —— 所以醒来先
// QUALITY 整屏铺白，再把所有可见实体挂 Dirty 让渲染器重画（§5.2 方案 A 的极端版）。
//
// 触摸唤醒的"那一摸"不算点击：睡着时 app::update() 在这里就 return，
// handle_touch 看不到按下沿，自然不会误触"恢复出厂"这种玩意儿。

#include <cstdint>
#include <cstdio>
#include <esp_timer.h>
#include <esp_heap_caps.h>
#include <M5Unified.h>
#include "ecs/world.hpp"
#include "platform/nvs.h"
#include "platform/wifi_ap.h"
#include "platform/wifi_sta.h"
#include "plugin.h"

namespace papers3::systems::power {

using ecs::Entity;

inline bool    g_asleep  = false;   // 屏幕当前熄着
inline int64_t g_last_ms = 0;       // 最后一次触摸的时刻

inline int64_t now_ms() { return esp_timer_get_time() / 1000LL; }

// ── 熄屏：画屏保图（或黑屏）→ 面板睡眠；dp 开着就顺带断 WiFi ──
inline void sleep_now() {
    if (g_asleep) return;

    M5.Display.setEpdMode(epd_mode_t::epd_quality);   // 屏保是整屏灰阶图 → QUALITY
    M5.Display.startWrite();
    const char* rel = cfg::g_cfg.scr_img;
    bool drew = false;
    if (rel[0]) {
        char path[128];
        snprintf(path, sizeof(path), "/sdcard/screensaver/%s", rel);   // NVS 只存相对路径
        FILE* f = fopen(path, "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            const long n = ftell(f);
            fseek(f, 0, SEEK_SET);
            // ★ 缓冲走 PSRAM，并且设上限：内部 RAM 只剩十几 KB，一张几十 KB 的 PNG
            //   用 malloc 必然失败 → 屏保永远黑屏（用户报过"屏保图不显示"）。
            //   上限防一张超大图把 8MB PSRAM 吃光（解码器自己还要临时空间）。
            uint8_t* buf = nullptr;
            const long CAP = 4L * 1024 * 1024;
            if (n > 0 && n <= CAP) buf = (uint8_t*)heap_caps_malloc((size_t)n, MALLOC_CAP_SPIRAM);
            else if (n > CAP)      ESP_LOGW("PWR", "屏保图太大（%ld 字节 > %ld），改黑屏", n, CAP);
            if (buf && fread(buf, 1, (size_t)n, f) == (size_t)n) {
                // ★ 三种都试：PNG → BMP → JPG。
                //   原来只试 drawPng → 放 bmp/jpg 一律"解码失败，改黑屏"（用户报过屏保图不显示）。
                //   顺序按解码开销排：PNG 最省，BMP 次之（JPG 要跑 jpeg 解码）。
                drew = M5.Display.drawPng(buf, (uint32_t)n, 0, 0);
                if (!drew) drew = M5.Display.drawBmp(buf, (uint32_t)n, 0, 0);
                if (!drew) drew = M5.Display.drawJpg(buf, (uint32_t)n, 0, 0);
            }
            if (buf) heap_caps_free(buf);
            fclose(f);
            if (!drew) ESP_LOGI("PWR", "屏保图解码失败，改黑屏");
        } else {
            ESP_LOGI("PWR", "屏保图不在了（%s），改黑屏", path);
        }
    }
    if (!drew) M5.Display.fillScreen(TFT_BLACK);
    M5.Display.endWrite();
    M5.Display.display(0, 0, M5.Display.width(), M5.Display.height());
    M5.Display.waitDisplay();

    if (cfg::g_cfg.dp) {                          // 深度休眠：断 WiFi（省电大头）
        wifi_sta::g_state = 0;                    // 别让状态机把这次停机当"连接失败"
        wifi_sta::g_ip[0] = 0;
        wifi_ap::pause();
        ESP_LOGI("PWR", "深度休眠：WiFi 已断");
    }
    M5.Display.sleep();                           // 面板睡眠 —— 墨水屏画面保持
    g_asleep = true;
    ESP_LOGI("PWR", "熄屏（%s）—— 摸一下屏幕唤醒",
             cfg::g_cfg.scr_img[0] ? cfg::g_cfg.scr_img : "黑屏");
}

// ── 唤醒：面板出睡 → 整屏铺白 → 所有可见实体挂 Dirty（下一帧渲染器重画）──
inline void wake_now(ecs::World& w) {
    M5.Display.wakeup();
    M5.Display.setEpdMode(epd_mode_t::epd_quality);
    M5.Display.startWrite();
    M5.Display.fillScreen(TFT_WHITE);
    M5.Display.endWrite();
    M5.Display.display(0, 0, M5.Display.width(), M5.Display.height());
    M5.Display.waitDisplay();

    for (auto e : w.query<ecs::components::Visible>())
        w.add(e, ecs::components::Dirty{});

    if (cfg::g_cfg.dp && cfg::g_cfg.wifi_ssid[0]) {   // 断过的网，摸醒了就连回去
        wifi_ap::resume();
        wifi_sta::connect(cfg::g_cfg.wifi_ssid, cfg::g_cfg.wifi_pass);
    }
    g_asleep = false;
    g_last_ms = now_ms();
    ESP_LOGI("PWR", "唤醒 —— 重画 UI");
}

// ── 每帧调（app::update 最前面）。返回 false = 睡着（这一帧到此为止）──
inline bool tick(ecs::World& w) {
    const bool touched = (M5.Touch.getCount() > 0);

    if (g_asleep) {
        if (touched) wake_now(w);
        return false;                             // 唤醒这一帧不给 touch/render（见文件头）
    }

    if (touched) { g_last_ms = now_ms(); return true; }

    // .so app 独占时别熄屏：唤醒只重画 ECS 可见实体，而 app 的像素不是实体，
    // 醒了就是白屏（见 pages/launcher.h on_open_app）。等有 so_on_wake 回调再放开。
    if (plugin::active()) { g_last_ms = now_ms(); return true; }

    const uint16_t idle_min = cfg::g_cfg.idle;
    if (idle_min == 0) return true;
    if (!cfg::g_cfg.ss && !cfg::g_cfg.dp) return true;   // 两个都没开 → 不熄
    if (now_ms() - g_last_ms < (int64_t)idle_min * 60000LL) return true;

    if (cfg::g_cfg.ss) sleep_now();               // 屏保
    else if (cfg::g_cfg.dp) {                     // 只断网、不熄屏
        wifi_sta::g_state = 0;
        wifi_sta::g_ip[0] = 0;
        wifi_ap::pause();
        g_last_ms = now_ms();                     // 别每帧都重复断
        ESP_LOGI("PWR", "深度休眠：WiFi 已断（屏未熄）");
    }
    return !g_asleep;
}

} // namespace papers3::systems::power
