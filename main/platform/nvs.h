#pragma once

// platform/nvs.h — 设置持久化
//
// 一份 Config 存进 NVS 命名空间 "m5ui"。**开机读、改动时写**（不要每帧写 —— NVS 写会磨损 flash 分区）。
//
// 三条规矩：
//   1. 字体只存**相对路径**（如 "fonts/霞鹜文楷.ttf"），不存绝对路径 ——
//      SD 挂载点固定是 /sdcard，但万一以后改了挂载点，存绝对路径就全失效。
//   2. 只在「加载成功」或用户点保存时写。
//   3. NVS 已在 wifi_ap.h 里 nvs_flash_init() 过，这里只 nvs_open，
//      但 init() 做成幂等的 —— 谁先跑都不怕。

#include <cstdio>
#include <cstring>
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_log.h"

namespace papers3::cfg {

static const char* TAG = "CFG";
static const char* NS  = "m5ui";

// ── 全部设置 ──
struct Config {
    uint16_t size = 28;            // 默认字号（px）
    // 注：全局波形档的**历史**在这里改过两次 —— 早先是全局 wave，
    //   2026-10 删掉改成"按内容由部件自己声明"（有灰→TEXT/GL16，纯黑白→FAST）。
    //   现在回来了，形态不同：见下面 wave_def / wave_sel。
    uint8_t  ink  = 0x66;          // 墨色档（0=满墨 … 255=看不见）
    uint8_t  bw   = 3;             // 描边粗细
    uint16_t idle = 5;             // 熄屏超时（分钟）
    uint8_t  ap   = 1;             // 开机自动开热点
    uint8_t  web  = 1;             // 开机自动开网页文件服务（与热点解耦）
    uint8_t  wifi = 1;             // 开机自动连已存热点（STA；与热点解耦）
    // ── 波形档（LUT）── 5 档：1=gc13 2=text12 3=fast 4=fastest 5=bw4（见 systems/epd_lut.h）
    // wave_def = 1「默认配置」：档位由部件按内容自选（render.h apply_epd_mode），用原厂表。
    // wave_def = 0：全局强制用 wave_sel 那一档，且装上对应的自定义表。
    uint8_t  wave_def = 1;         // 默认配置开关（1 = 部件自选）
    uint8_t  wave_sel = 2;         // 全局档（1=gc13 2=text12 3=fast 4=fastest 5=bw4；仅 wave_def=0 时生效）
    uint8_t  ss   = 1;             // 屏保
    uint8_t  dp   = 0;             // 深度休眠
    char     font[128] = {0};      // SD 字体相对路径；空 = 用内置 flash 字体
    // 上次连过的手机热点 —— 开机自动连（见 main.cpp）。空 = 没存过，不自动连。
    char     wifi_ssid[33] = {0};  // 802.11 SSID 上限 32 + '\0'
    char     wifi_pass[65] = {0};  // WPA2 口令上限 63 + '\0'
    // 熄屏时显示的屏保图（SD 卡 screensaver/ 下的**相对路径**，规矩同字体：不存绝对路径）
    char     scr_img[64] = {0};    // 空 = 纯黑屏
    // ── 触摸反馈音（2026-10-18）──
    uint8_t  snd = 1;              // 1 = 按下有反馈音
    uint8_t  snd_vol = 60;         // 音量 0~100
    char     snd_file[64] = {0};   // SD 音效相对路径（sounds/xxx.wav）；空 = 内置短哔
};

inline Config g_cfg;                       // 全局一份，开机读一次
inline bool   g_loaded = false;

// 幂等初始化（wifi_ap.h 里也调，谁先都行）
inline void init() {
    esp_err_t r = nvs_flash_init();
    if (r == ESP_ERR_NVS_NO_FREE_PAGES || r == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        r = nvs_flash_init();
    }
    if (r != ESP_OK && r != ESP_ERR_INVALID_STATE)
        ESP_LOGW(TAG, "nvs_flash_init: %s", esp_err_to_name(r));
}

// ── 读：任何一项缺失都保留 struct 里的默认值（所以先给默认值再覆盖） ──
inline bool load() {
    nvs_handle_t h;
    esp_err_t e = nvs_open(NS, NVS_READONLY, &h);
    if (e != ESP_OK) {
        ESP_LOGI(TAG, "还没存过设置（%s）—— 全用默认值", esp_err_to_name(e));
        g_loaded = true;
        return false;
    }

    uint16_t u16; uint8_t u8;

    if (nvs_get_u16(h, "size", &u16) == ESP_OK) g_cfg.size = u16;
    if (nvs_get_u8 (h, "ink",  &u8)  == ESP_OK) g_cfg.ink  = u8;
    if (nvs_get_u8 (h, "bw",   &u8)  == ESP_OK) g_cfg.bw   = u8;
    if (nvs_get_u16(h, "idle", &u16) == ESP_OK) g_cfg.idle = u16;
    if (nvs_get_u8 (h, "ap",   &u8)  == ESP_OK) g_cfg.ap   = u8;
    if (nvs_get_u8 (h, "web",  &u8)  == ESP_OK) g_cfg.web  = u8;
    if (nvs_get_u8 (h, "wifi", &u8)  == ESP_OK) g_cfg.wifi = u8;
    if (nvs_get_u8 (h, "wavdef", &u8) == ESP_OK) g_cfg.wave_def = u8;
    if (nvs_get_u8 (h, "wavsel", &u8) == ESP_OK) g_cfg.wave_sel = u8;
    if (nvs_get_u8 (h, "ss",   &u8)  == ESP_OK) g_cfg.ss   = u8;
    if (nvs_get_u8 (h, "dp",   &u8)  == ESP_OK) g_cfg.dp   = u8;
    if (nvs_get_u8 (h, "snd",  &u8)  == ESP_OK) g_cfg.snd  = u8;
    if (nvs_get_u8 (h, "sndvol", &u8) == ESP_OK) g_cfg.snd_vol = u8;

    size_t len = sizeof(g_cfg.font);
    esp_err_t fe = nvs_get_str(h, "font", g_cfg.font, &len);
    if (fe == ESP_ERR_NVS_NOT_FOUND) {
        g_cfg.font[0] = 0;                 // 没存过 → 用内置字体
    } else if (fe != ESP_OK) {
        ESP_LOGW(TAG, "读 font 失败: %s", esp_err_to_name(fe));
        g_cfg.font[0] = 0;
    }
    // WiFi：读不到就是「没存过」→ 保持空字符串（不自动连）
    size_t l1 = sizeof(g_cfg.wifi_ssid);
    if (nvs_get_str(h, "wssid", g_cfg.wifi_ssid, &l1) != ESP_OK) g_cfg.wifi_ssid[0] = 0;
    size_t l2 = sizeof(g_cfg.wifi_pass);
    if (nvs_get_str(h, "wpass", g_cfg.wifi_pass, &l2) != ESP_OK) g_cfg.wifi_pass[0] = 0;
    size_t l3 = sizeof(g_cfg.scr_img);
    if (nvs_get_str(h, "scrimg", g_cfg.scr_img, &l3) != ESP_OK) g_cfg.scr_img[0] = 0;
    size_t l4 = sizeof(g_cfg.snd_file);
    if (nvs_get_str(h, "sndfile", g_cfg.snd_file, &l4) != ESP_OK) g_cfg.snd_file[0] = 0;

    nvs_close(h);

    // 范围兜底：NVS 里的值可能来自旧固件或被人为改坏
    // ★ 2026-10-22：字号不再对外开放（设置 app 的字号项已删）。全局正文固定 22px，
    //   旧 NVS 里存的 34 一律覆盖掉 —— 否则启动器 / 输入法编辑条还是旧的大字号。
    g_cfg.size = 22;
    if (g_cfg.wave_def > 1) g_cfg.wave_def = 1;                 // 波形：默认配置开关
    if (g_cfg.wave_sel < 1 || g_cfg.wave_sel > 5) g_cfg.wave_sel = 2;  // 波形：可选档 1~5

    g_loaded = true;
    // ★ 密码不进日志（串口日志会被贴来贴去），只打热点名
    ESP_LOGI(TAG, "设置: size=%u ink=0x%02X bw=%u idle=%u ap=%u web=%u wifi=%u ss=%u dp=%u wavdef=%u wavsel=%u font='%s' wifi='%s'",
             g_cfg.size, g_cfg.ink, g_cfg.bw, g_cfg.idle,
             g_cfg.ap, g_cfg.web, g_cfg.wifi, g_cfg.ss, g_cfg.dp,
             g_cfg.wave_def, g_cfg.wave_sel,
             g_cfg.font[0] ? g_cfg.font : "(内置)",
             g_cfg.wifi_ssid[0] ? g_cfg.wifi_ssid : "(没存过)");
    return true;
}

// ── 写：一次 commit，别一项一个 commit ──
inline bool save() {
    nvs_handle_t h;
    esp_err_t e = nvs_open(NS, NVS_READWRITE, &h);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open 失败: %s", esp_err_to_name(e));
        return false;
    }

    nvs_set_u16(h, "size", g_cfg.size);
    nvs_set_u8 (h, "ink",  g_cfg.ink);
    nvs_set_u8 (h, "bw",   g_cfg.bw);
    nvs_set_u16(h, "idle", g_cfg.idle);
    nvs_set_u8 (h, "ap",   g_cfg.ap);
    nvs_set_u8 (h, "web",  g_cfg.web);
    nvs_set_u8 (h, "wifi", g_cfg.wifi);
    nvs_set_u8 (h, "wavdef", g_cfg.wave_def);
    nvs_set_u8 (h, "wavsel", g_cfg.wave_sel);
    nvs_set_u8 (h, "ss",   g_cfg.ss);
    nvs_set_u8 (h, "dp",   g_cfg.dp);
    // 空字符串也要写：那是「用内置字体」的有效状态，删掉键反而读不出来
    nvs_set_str(h, "font", g_cfg.font);
    nvs_set_str(h, "wssid", g_cfg.wifi_ssid);
    nvs_set_str(h, "wpass", g_cfg.wifi_pass);
    nvs_set_str(h, "scrimg", g_cfg.scr_img);
    nvs_set_u8 (h, "snd",    g_cfg.snd);
    nvs_set_u8 (h, "sndvol", g_cfg.snd_vol);
    nvs_set_str(h, "sndfile", g_cfg.snd_file);

    e = nvs_commit(h);
    nvs_close(h);
    if (e != ESP_OK) { ESP_LOGE(TAG, "nvs_commit: %s", esp_err_to_name(e)); return false; }
    ESP_LOGI(TAG, "已保存（font='%s'）", g_cfg.font[0] ? g_cfg.font : "(内置)");
    return true;
}

// ── 字体：相对路径（"fonts/x.ttf"），空 = 内置 ──
inline void set_font(const char* rel) {
    snprintf(g_cfg.font, sizeof(g_cfg.font), "%.*s",
             (int)sizeof(g_cfg.font) - 1, rel ? rel : "");
}
// 清掉字体选择 → 回内置。开机加载失败时也要调它，否则每次开机都失败
inline void clear_font() {
    g_cfg.font[0] = 0;
    save();
}
// 拼出绝对路径供 fopen 用
inline void font_path(char* out, size_t n) {
    snprintf(out, n, "/sdcard/%s", g_cfg.font);
}

} // namespace papers3::cfg
