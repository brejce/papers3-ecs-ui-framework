#pragma once

// platform/wifi_ap.h — 开一个热点，让手机连进来
//
// 用途：手机扫码（或手动）连上这个热点 → 浏览器打开 http://192.168.4.1/ 
//       → 管 SD 卡里的文件（见 platform/web_files.h）。
//
// SSID / 密码在下面两行改。密码留空 = 开放热点（好连，但谁都能进）。
// 这是**配网用的临时热点**，不做路由、不通外网。

#include <cstdio>
#include <cstring>
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "nvs_flash.h"

namespace papers3::wifi_ap {

static constexpr const char* SSID    = "M5PaperS3";
static constexpr const char* PASS    = "12345678";   // ≥8 位（WPA2）
static constexpr int         CHANNEL = 1;
static constexpr int         MAX_STA = 4;

inline char g_ip[16] = "192.168.4.1";   // SoftAP 的默认网段
inline bool g_up      = false;          // 驱动初始化过（init 只能来一次）
inline bool g_running = false;          // 当前开着
// 这次起的驱动**要不要带设备热点**（= NVS 的「自动开热点」开关）。
// 关掉的场合：开机只想连手机热点 → WIFI_MODE_STA，别人搜不到 M5PaperS3。
inline bool g_want_ap = true;

inline void on_event(void*, esp_event_base_t base, int32_t id, void*) {
    if (base != WIFI_EVENT) return;
    if (id == WIFI_EVENT_AP_START)                ESP_LOGI("WIFI", "AP 已启动");
    else if (id == WIFI_EVENT_AP_STACONNECTED)    ESP_LOGI("WIFI", "手机连上了");
    else if (id == WIFI_EVENT_AP_STADISCONNECTED) ESP_LOGI("WIFI", "手机断开了");
}

inline bool resume();   // 先声明：start() 里要用（上次只是暂停过就直接恢复）

// AP 的配置（init 与「后来补开热点」两处共用）
inline void fill_ap(wifi_config_t& wc) {
    wc = {};
    strncpy((char*)wc.ap.ssid, SSID, sizeof(wc.ap.ssid) - 1);
    wc.ap.ssid_len       = (uint8_t)strlen(SSID);
    strncpy((char*)wc.ap.password, PASS, sizeof(wc.ap.password) - 1);
    wc.ap.channel        = CHANNEL;
    wc.ap.max_connection = MAX_STA;
    wc.ap.authmode       = (strlen(PASS) >= 8) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
}

// with_ap=false → 只起 STA（设备热点不广播）。给「自动开热点=关，但开机要连手机热点」用。
inline bool start(bool with_ap = true) {
    // 驱动已经在跑、但之前是「不带热点」起的，现在要热点 → 切回 APSTA 并把 AP 配上。
    // （运行中改模式会让 WiFi 断一下重来；墨水屏这几个页面没有实时流量，可接受。）
    if (g_up && with_ap && !g_want_ap) {
        wifi_config_t wc; fill_ap(wc);
        if (esp_wifi_set_mode(WIFI_MODE_APSTA) != ESP_OK ||
            esp_wifi_set_config(WIFI_IF_AP, &wc) != ESP_OK) {
            ESP_LOGE("WIFI", "补开热点失败"); return false;
        }
        g_want_ap = true;
        ESP_LOGI("WIFI", "热点已补开 http://%s/", g_ip);
        return true;
    }
    if (g_running) return true;
    if (g_up) return resume();              // 上次只是停了，直接恢复

    // WiFi 驱动要 NVS 存 PHY 校准数据；没初始化过或版本不合就重来
    esp_err_t r = nvs_flash_init();
    if (r == ESP_ERR_NVS_NO_FREE_PAGES || r == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        r = nvs_flash_init();
    }
    if (r != ESP_OK) ESP_LOGW("WIFI", "nvs: %s", esp_err_to_name(r));

    if (esp_netif_init() != ESP_OK) { ESP_LOGE("WIFI", "netif init 失败"); return false; }
    esp_event_loop_create_default();          // 已建过会返回错误，忽略即可
    esp_netif_t* ap = esp_netif_create_default_wifi_ap();
    if (!ap) { ESP_LOGE("WIFI", "创建 AP netif 失败"); return false; }
    // 顺手把 STA 接口也建好：模式从一开始就是 APSTA，这样「连手机热点」页不用在运行时
    // 改模式（改模式要重启 WiFi，风险大）。官方 softap_sta 示例就是这个顺序：
    //   两个 netif 都建好 → esp_wifi_init → set_mode(APSTA) → esp_wifi_start
    if (!esp_netif_create_default_wifi_sta())
        ESP_LOGW("WIFI", "创建 STA netif 失败（连手机热点将不可用）");

    wifi_init_config_t ic = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&ic) != ESP_OK) { ESP_LOGE("WIFI", "wifi init 失败"); return false; }
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, nullptr, nullptr);

    g_want_ap = with_ap;
    wifi_config_t wc; fill_ap(wc);

    // 不带热点时用纯 STA 模式：别人就搜不到设备的 M5PaperS3 了（「自动开热点」关掉的含义）
    if (esp_wifi_set_mode(with_ap ? WIFI_MODE_APSTA : WIFI_MODE_STA) != ESP_OK ||
        (with_ap && esp_wifi_set_config(WIFI_IF_AP, &wc) != ESP_OK) ||
        esp_wifi_start() != ESP_OK) {
        ESP_LOGE("WIFI", "启动失败（内存不够？）");
        return false;
    }

    esp_netif_ip_info_t ip{};
    if (esp_netif_get_ip_info(ap, &ip) == ESP_OK && ip.ip.addr)
        snprintf(g_ip, sizeof(g_ip), IPSTR, IP2STR(&ip.ip));

    g_up = true;
    g_running = true;
    if (with_ap) ESP_LOGI("WIFI", "热点 '%s' 已开，浏览器打开 http://%s/", SSID, g_ip);
    else         ESP_LOGI("WIFI", "只起 STA —— 没开设备热点（NVS 里「自动开热点」是关）");
    return true;
}

// 暂停 / 恢复：省电用。只停不 deinit —— 再开时不用重新初始化。
inline bool resume() {
    if (!g_up) return start();
    if (esp_wifi_start() != ESP_OK) { ESP_LOGE("WIFI", "恢复失败"); return false; }
    g_running = true;
    ESP_LOGI("WIFI", "热点已恢复 http://%s/", g_ip);
    return true;
}

inline void pause() {
    if (!g_running) return;
    esp_wifi_stop();
    g_running = false;
    ESP_LOGI("WIFI", "热点已停（省电；再点一下就恢复）");
}

inline bool running() { return g_running; }

// ── 独立开关：运行期把「要不要设备热点」落到驱动上（不动 STA 的连接）──
// 给连接页的「设备热点」开关用。STA 的连/断归 wifi_sta，这里只改 AP 那半边。
inline bool set_ap(bool on) {
    if (!g_up) return start(on);                     // 还没起过 → 按要的形态起
    if (g_running && g_want_ap == on) return true;   // 已经是这个形态
    if (!g_running) {                                // 驱动停着 → 先拉起来
        if (esp_wifi_start() != ESP_OK) return false;
        g_running = true;
    }
    // ⚠️ 运行中改模式会让 WiFi 断一下重来（墨水屏这几页没有实时流量，可接受）
    if (esp_wifi_set_mode(on ? WIFI_MODE_APSTA : WIFI_MODE_STA) != ESP_OK) {
        ESP_LOGE("WIFI", "切模式失败");
        return false;
    }
    if (on) {
        wifi_config_t wc; fill_ap(wc);
        if (esp_wifi_set_config(WIFI_IF_AP, &wc) != ESP_OK) return false;
    }
    g_want_ap = on;
    if (on) ESP_LOGI("WIFI", "设备热点已开 http://%s/", g_ip);
    else    ESP_LOGI("WIFI", "设备热点已关（STA 不受影响）");
    return true;
}

} // namespace papers3::wifi_ap
