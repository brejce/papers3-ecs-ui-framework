#pragma once

// platform/wifi_sta.h — 连外部热点（手机热点）
//
// 和 wifi_ap.h 共用同一个 WiFi 驱动：设备自己的热点**照旧开着**（手机还能连设备），
// 这里另外去连一个外部热点。连上之后，同一个热点下的手机浏览器打开
// http://<设备IP>/ 就能传/删 SD 卡上的文件（见 platform/web_files.h）。
//
// 状态全在事件回调里推进，UI 每帧读 g_state 显示：
//   0 = 没连    1 = 正在连    2 = 已连上（g_ip 是设备拿到的地址）    3 = 失败（g_reason 是原因码）
//
// 模式是开机就设成 APSTA 的（见 wifi_ap.h），所以这里**不需要**中途改模式 ——
// 运行中改 WiFi 模式要重启驱动，风险大，不做。

#include <cstdio>
#include <cstring>
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"   // vTaskDelay / pdMS_TO_TICKS（切热点先断开用）
#include "freertos/task.h"
#include "platform/wifi_ap.h"
#include "platform/nvs.h"      // 读「自动开热点」开关：决定起驱动时带不带 AP
#include "platform/rtc_time.h"  // 联网成功 → 触发一次时间校准

namespace m5ui::wifi_sta {

inline constexpr int RETRY_MAX = 3;     // 失败自动重试几次

inline char g_ssid[33] = {0};           // 正在连（或连过）的热点名
inline char g_pass[65] = {0};
inline char g_ip[16]   = {0};           // 连上后拿到的 IP
inline int  g_state    = 0;
inline int  g_reason   = 0;             // wifi_err_reason_t
inline int  g_retry    = 0;
inline bool g_hooked   = false;         // 事件回调注册过没

// ── 事件：连接状态全靠这里推进 ──
inline void on_event(void*, esp_event_base_t base, int32_t id, void* data) {
    if (base == WIFI_EVENT) {
        if (id == WIFI_EVENT_STA_CONNECTED) {
            ESP_LOGI("STA", "已连上热点 '%s'，等分配 IP…", g_ssid);

        } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
            if (g_state == 0) return;                       // 是我们自己断的，别当失败
            auto* ev = (wifi_event_sta_disconnected_t*)data;
            g_reason = ev ? ev->reason : 0;
            if (g_state == 1 && g_retry < RETRY_MAX) {      // 还在连 → 再试
                g_retry++;
                ESP_LOGW("STA", "第 %d 次没连上（reason=%d），重试…", g_retry, g_reason);
                esp_wifi_connect();
            } else {
                g_state = 3;                                // 最终失败（或连上之后又掉了）
                ESP_LOGW("STA", "连接失败：reason=%d", g_reason);
            }
        }

    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        auto* ev = (ip_event_got_ip_t*)data;
        if (ev) snprintf(g_ip, sizeof(g_ip), IPSTR, IP2STR(&ev->ip_info.ip));
        g_state = 2;
        ESP_LOGI("STA", "拿到地址 %s —— 手机浏览器打开 http://%s/", g_ip, g_ip);
        m5ui::rtctime::kick_ntp("STA 拿到地址");   // 联上网就校准一次时间（RTC 是时间源，见 rtc_time.h）
    }
}

// 保底：WiFi 驱动本身得是开着的（热点页可能被暂停过）
inline bool ensure() {
    // ★ 要连 STA 就顺手把**设备热点**也开起来 —— 那会让「自动开热点」开关形同虚设
    //   （开关关了、重启后照样搜得到 M5PaperS3）。所以按 NVS 里的开关决定模式：
    //   开 = APSTA；关 = 只起 STA。
    if (!wifi_ap::g_up)           { if (!wifi_ap::start(m5ui::cfg::g_cfg.ap != 0)) return false; }
    else if (!wifi_ap::running()) { if (!wifi_ap::resume()) return false; }
    if (g_hooked) return true;
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, nullptr, nullptr);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, nullptr, nullptr);
    g_hooked = true;
    return true;
}

// ── 连一个热点 ──
inline bool connect(const char* ssid, const char* pass) {
    if (!ssid || !ssid[0]) { g_state = 3; g_reason = 0; return false; }
    if (!ensure()) { g_state = 3; return false; }

    // ★ 已连着别的热点时先断开：esp_wifi_connect 在已连接状态不会切换、
    //   也不会触发任何事件，状态会卡死在「连接中」。断开是异步的，等一小会儿再连。
    if (g_state == 2 || g_state == 1) {
        g_state = 0;                 // 先标「自己断的」，免得回调当失败
        esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    snprintf(g_ssid, sizeof(g_ssid), "%s", ssid);
    snprintf(g_pass, sizeof(g_pass), "%s", pass ? pass : "");

    wifi_config_t wc = {};
    strncpy((char*)wc.sta.ssid,     g_ssid, sizeof(wc.sta.ssid) - 1);
    strncpy((char*)wc.sta.password, g_pass, sizeof(wc.sta.password) - 1);
    wc.sta.scan_method        = WIFI_ALL_CHANNEL_SCAN;
    wc.sta.sort_method        = WIFI_CONNECT_AP_BY_SIGNAL;
    wc.sta.failure_retry_cnt  = 2;
    wc.sta.threshold.authmode = WIFI_AUTH_OPEN;     // 开放/WPA/WPA2 都让连（不卡认证方式）
    wc.sta.sae_pwe_h2e        = WPA3_SAE_PWE_BOTH;  // 手机热点若是 WPA3 也能连

    g_ip[0] = 0; g_reason = 0; g_retry = 0; g_state = 1;

    esp_err_t r = esp_wifi_set_config(WIFI_IF_STA, &wc);
    if (r != ESP_OK) { ESP_LOGE("STA", "set_config: %s", esp_err_to_name(r)); g_state = 3; return false; }
    r = esp_wifi_connect();
    if (r != ESP_OK) { ESP_LOGE("STA", "connect: %s", esp_err_to_name(r));   g_state = 3; return false; }

    ESP_LOGI("STA", "正在连 '%s'…", g_ssid);
    return true;
}

inline void disconnect() {
    if (g_state == 0) return;
    g_state = 0;                    // 先标「自己断的」，免得回调把它当失败
    g_ip[0] = 0;
    esp_wifi_disconnect();
    ESP_LOGI("STA", "已断开");
}

// ── 扫附近热点（阻塞到扫完）──
// 结果按信号强度顺序填进 names（每项 33 字节 SSID + '\0'），最多 max 个，返回个数。
inline int scan(char (*names)[33], int8_t* rssi, int max) {
    if (max <= 0) return 0;
    if (!ensure()) return 0;
    wifi_scan_config_t sc = {};
    sc.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    sc.scan_time.active.min = 100;
    sc.scan_time.active.max = 300;
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) return 0;
    uint16_t total = 0;
    if (esp_wifi_scan_get_ap_num(&total) != ESP_OK || total == 0) return 0;
    if (total > 16) total = 16;
    static wifi_ap_record_t s_recs[16];
    uint16_t n = total;
    if (esp_wifi_scan_get_ap_records(&n, s_recs) != ESP_OK) return 0;
    int out = 0;
    for (uint16_t i = 0; i < n && out < max; ++i) {
        if (s_recs[i].ssid[0] == 0) continue;   // 隐藏网络（空 SSID）跳过
        snprintf(names[out], 33, "%s", (const char*)s_recs[i].ssid);
        if (rssi) rssi[out] = s_recs[i].rssi;   // 信号强度 (dBm)
        ++out;
    }
    return out;
}

// 原因码 → 人看得懂的话（EPD 上直接显示这个）
inline const char* reason_text(int r) {
    switch (r) {
        case WIFI_REASON_NO_AP_FOUND:            return "没找到这个热点（名字对吗）";
        case WIFI_REASON_AUTH_FAIL:              return "密码不对";
        case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT: return "密码不对（握手超时）";
        case WIFI_REASON_HANDSHAKE_TIMEOUT:      return "密码不对（握手超时）";
        case WIFI_REASON_ASSOC_FAIL:             return "热点拒绝了（连的人满？）";
        case WIFI_REASON_CONNECTION_FAIL:        return "连不上这个热点";
        case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
        case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
        case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD: return "热点不兼容（安全方式）";
    }
    return "连接失败（看串口日志）";
}

} // namespace m5ui::wifi_sta
