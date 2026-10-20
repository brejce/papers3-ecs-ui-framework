#pragma once

// platform/sound.h — 触摸反馈音
//
// M5PaperS3 板载**无源蜂鸣器**（GPIO21，M5Unified 的 Speaker 走 LEDC/PWM，spk_cfg.buzzer=true）。
//
// 两种声源：
//   1. 内置短哔：M5.Speaker.tone(2000Hz, 30ms) —— 零依赖，默认
//   2. SD 音效：/sdcard/sounds/xxx.wav —— 配置里存**相对路径**（规矩同字体），
//      开机 / 改设置时整份读进 PSRAM，按下时 playWav()
//
// 纪律：
//   · 只在 ui 任务里调（和触摸同一个任务，不需要锁）
//   · 非阻塞：tone()/playWav() 只是把活交给 Speaker 内部任务，立即返回
//   · 限流 60ms：快速连点不会连成一串（丢的是声音，不是触摸事件）
//   · 播放失败/文件缺失 → 自动退回内置短哔，不会没声音

#include <cstdio>
#include <cstring>
#include "M5Unified.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "platform/nvs.h"

namespace m5ui::sound {

inline uint8_t* g_wav     = nullptr;   // PSRAM 里的 WAV 原始字节（含头）
inline int      g_wav_len = 0;
inline int64_t  g_last_us = 0;
inline bool     g_ready   = false;     // 是否已应用过一次配置

inline constexpr int64_t  MIN_GAP_US = 60000;      // 60ms 限流
inline constexpr float    BEEP_HZ    = 2000.0f;
inline constexpr uint32_t BEEP_MS    = 30;
inline constexpr int      WAV_MAX    = 128 * 1024; // 反馈音不该太长（PSRAM 有限）

inline void unload() {
    if (g_wav) { heap_caps_free(g_wav); g_wav = nullptr; }
    g_wav_len = 0;
}

// 按 cfg::g_cfg.snd_file 加载（"sounds/click.wav"）
inline bool load_file() {
    unload();
    const char* rel = cfg::g_cfg.snd_file;
    if (!rel[0]) return false;

    char p[128];
    snprintf(p, sizeof(p), "/sdcard/%.100s", rel);
    FILE* f = fopen(p, "rb");
    if (!f) { ESP_LOGW("SND", "音效打不开 %s（用内置短哔）", p); return false; }
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 44 || n > WAV_MAX) {
        fclose(f);
        ESP_LOGW("SND", "音效大小不合适: %ld 字节（用内置短哔）", n);
        return false;
    }
    uint8_t* buf = (uint8_t*)heap_caps_malloc((size_t)n, MALLOC_CAP_SPIRAM);
    if (!buf) { fclose(f); ESP_LOGW("SND", "PSRAM 分配失败"); return false; }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
        heap_caps_free(buf); fclose(f); return false;
    }
    fclose(f);
    g_wav = buf;
    g_wav_len = (int)n;
    ESP_LOGI("SND", "音效已加载 %s（%d 字节）", p, g_wav_len);
    return true;
}

// 应用配置：开机一次 + 设置里改完调一次
inline void reload() {
    if (cfg::g_cfg.snd_vol > 100) cfg::g_cfg.snd_vol = 100;
    M5.Speaker.setVolume((uint8_t)(cfg::g_cfg.snd_vol * 255 / 100));
    load_file();                       // 没有/打不开 → g_wav = null → 内置短哔
    g_ready = true;
}

// 按下反馈音（非阻塞，带限流）
inline void click() {
    if (!cfg::g_cfg.snd) return;
    if (!g_ready) reload();
    const int64_t now = esp_timer_get_time();
    if (now - g_last_us < MIN_GAP_US) return;
    g_last_us = now;
    if (g_wav) M5.Speaker.playWav(g_wav, (size_t)g_wav_len, 1, -1, true);
    else       M5.Speaker.tone(BEEP_HZ, BEEP_MS);
}

// 设置页「试听」（不跟 snd 开关，也不走 60ms 限流）
inline void test() {
    if (!g_ready) reload();
    if (g_wav) M5.Speaker.playWav(g_wav, (size_t)g_wav_len, 1, -1, true);
    else       M5.Speaker.tone(BEEP_HZ, BEEP_MS * 4);
}

} // namespace m5ui::sound
