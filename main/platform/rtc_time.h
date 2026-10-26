#pragma once
// ──────────────────────────────────────────────────────────────
// 系统时间：**板载 RTC 供时间，网络（SNTP）只做校准**（用户口径 2026-10-15）
//
//   开机      ：读 RTC → 写进系统时钟（掉电后 RTC 靠纽扣电池/超级电容走时）
//   联网成功  ：SNTP 同步一次 → 校准系统时钟 **并写回 RTC**
//   设置页连接：再校准一次（用户明确要求：连接 wifi 后做一次校准）
//
// 为什么这么分：墨水屏待机久了要能直接显示时间，而网络不能一直有
//（酒店/热点网、离线场景），所以“时间源”必须是 RTC；网络只负责纠偏。
//
// 三个接口：begin() 开机读、kick_ntp(why) 触发校准、tick() 主循环里写回 RTC。
// ──────────────────────────────────────────────────────────────
#include <time.h>
#include <sys/time.h>
#include <esp_netif_sntp.h>
#include <esp_sntp.h>
#include <M5Unified.h>

namespace papers3 { namespace rtctime {

inline bool g_valid        = false;   // 系统时间可信（RTC 读到过 或 网络同步过）
inline bool g_ntp_inited   = false;
inline bool g_rtc_ok       = false;   // RTC 芯片可用且值合理
inline volatile bool g_synced   = false;   // 网络时间到了 → 待写回 RTC（标志只在主循环消费）
inline volatile int  g_sync_cnt = 0;

// 只用一台 NTP（servers[] 长度受 CONFIG_LWIP_SNTP_MAX_SERVERS=1 限制）。
// 想加备用服务器要先把 sdkconfig 那个值改成 2。
inline const char* NTP1 = "ntp.aliyun.com";   // 国内可达；这次拿不到下个周期（1h）再试

// 时区：中国标准时间（无夏令时）。setenv 只需一次。
inline void apply_tz() {
    setenv("TZ", "CST-8", 1);
    tzset();
}

// SNTP 回调：**只置标志**。写 RTC 是 I2C 操作，放主循环里做，
// 不在 SNTP 任务上下文里碰总线（避免和其他 I2C 访问撞车）。
inline void on_sync(struct timeval* tv) {
    if (tv) settimeofday(tv, nullptr);          // 系统时钟对齐
    g_valid  = true;
    g_synced = true;
    g_sync_cnt++;
}

// 开机：RTC → 系统时钟。返回是否拿到了可信时间。
inline bool begin() {
    apply_tz();
    if (!M5.Rtc.isEnabled() && !M5.Rtc.begin()) {
        ESP_LOGW("RTC", "RTC 起不来（板子没有 / I2C 没通）→ 时间不可信，等联网校准");
        return false;
    }
    // 低压标志 = 掉过电，走时可能已经不准 → 当作不可信，等网络纠偏
    if (M5.Rtc.getVoltLow()) {
        ESP_LOGW("RTC", "RTC 电压低标志置位（掉过电）→ 值不可信，等联网校准");
        return false;
    }
    // ★ 必须写 m5:: —— rtc_datetime_t 在 m5 命名空间里，本文件在 papers3::rtctime 里，
    //   非限定查找找不到（编译报的就是 'rtc_datetime_t' was not declared）。
    m5::rtc_datetime_t dt;
    if (!M5.Rtc.getDateTime(&dt)) {
        ESP_LOGW("RTC", "读 RTC 失败 → 等联网校准");
        return false;
    }
    // 合理性检查：这几个数是 RTC 掉电后的典型垃圾值
    if (dt.date.year < 2024 || dt.date.year > 2099 ||
        dt.date.month < 1 || dt.date.month > 12 ||
        dt.date.date < 1 || dt.date.date > 31 ||
        dt.time.hours > 23 || dt.time.minutes > 59 || dt.time.seconds > 59) {
        ESP_LOGW("RTC", "RTC 值不合理（%04d-%02d-%02d %02d:%02d:%02d）→ 等联网校准",
                 dt.date.year, dt.date.month, dt.date.date,
                 dt.time.hours, dt.time.minutes, dt.time.seconds);
        return false;
    }
    struct tm tmv = {};
    tmv.tm_year = dt.date.year - 1900;
    tmv.tm_mon  = dt.date.month - 1;
    tmv.tm_mday = dt.date.date;
    tmv.tm_hour = dt.time.hours;
    tmv.tm_min  = dt.time.minutes;
    tmv.tm_sec  = dt.time.seconds;
    const time_t t = mktime(&tmv);               // 按 TZ 解释
    if (t <= 0) { ESP_LOGW("RTC", "mktime 失败 → 等联网校准"); return false; }
    struct timeval tv = { t, 0 };
    settimeofday(&tv, nullptr);
    g_valid = g_rtc_ok = true;
    ESP_LOGI("RTC", "开机读 RTC → 系统时钟 %04d-%02d-%02d %02d:%02d:%02d",
             dt.date.year, dt.date.month, dt.date.date,
             dt.time.hours, dt.time.minutes, dt.time.seconds);
    return true;
}

// 触发一次网络校准（幂等）。第一次会起 SNTP 服务；之后只是要求立刻重查。
inline void kick_ntp(const char* why) {
    if (!g_ntp_inited) {
        // ⚠️ servers[] 的长度 = CONFIG_LWIP_SNTP_MAX_SERVERS，本项目是 **1**
        //    （sdkconfig：CONFIG_LWIP_SNTP_MAX_SERVERS=1）→ 只能给一个服务器。
        //    别写 cfg.servers[1] = ... —— 那是越界写（数组只有 1 个元素）。
        esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(NTP1);
        cfg.sync_cb = on_sync;
        const esp_err_t e = esp_netif_sntp_init(&cfg);
        if (e != ESP_OK) {
            ESP_LOGW("RTC", "SNTP 起不来（%s）：%s", why, esp_err_to_name(e));
            return;
        }
        g_ntp_inited = true;
        ESP_LOGI("RTC", "SNTP 已启动（%s）→ 同步到就校准系统时钟并写回 RTC", why);
    } else {
        esp_sntp_restart();                     // 立刻重查一次，不等下一个周期
        ESP_LOGI("RTC", "要求重新校准（%s）", why);
    }
}

// 主循环每帧调：把网络时间写回 RTC（I2C 只在这里发生）
inline void tick() {
    if (!g_synced) return;
    g_synced = false;
    const time_t now = time(nullptr);
    struct tm tmv;
    localtime_r(&now, &tmv);
    if (M5.Rtc.isEnabled()) {
        M5.Rtc.setDateTime(&tmv);
        g_rtc_ok = true;
        ESP_LOGI("RTC", "已校准并写回 RTC：%04d-%02d-%02d %02d:%02d:%02d（第 %d 次同步）",
                 tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                 tmv.tm_hour, tmv.tm_min, tmv.tm_sec, (int)g_sync_cnt);
    } else {
        ESP_LOGI("RTC", "网络时间已到（RTC 不可写）：%02d:%02d", tmv.tm_hour, tmv.tm_min);
    }
}

inline bool valid() { return g_valid; }
inline bool rtc_ok() { return g_rtc_ok; }

// -1 = 时间不可信（还没 RTC 值、也没联网校准）；否则 HH*100+MM
inline int hhmm() {
    if (!g_valid) return -1;
    const time_t now = time(nullptr);
    struct tm tmv;
    localtime_r(&now, &tmv);
    return tmv.tm_hour * 100 + tmv.tm_min;
}

}} // namespace papers3::rtctime
