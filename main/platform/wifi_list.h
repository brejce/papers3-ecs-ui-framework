#pragma once

// platform/wifi_list.h — 已存 WiFi 热点列表（多热点记忆）
//
// 存 NVS 命名空间 "m5ui" 的 blob 键 "wlist"：MAX_ENTRIES 个 Entry 的连续字节。
// 空槽 = ssid[0]=='\0'。开机不必主动 load —— so_wifi_* 首次调用时懒加载。
// 密码只在这里落盘，不进日志。

#include <cstdio>
#include <cstring>
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_log.h"

namespace m5ui::wifi_list {

inline constexpr int MAX_ENTRIES = 8;
struct Entry { char ssid[33]; char pass[65]; };   // 98 字节，无 padding，可 blob 直存

inline Entry g_entries[MAX_ENTRIES];
inline bool  g_loaded = false;

inline int count() {
    int n = 0;
    for (int i = 0; i < MAX_ENTRIES; ++i) if (g_entries[i].ssid[0]) ++n;
    return n;
}

inline void load() {
    if (g_loaded) return;
    g_loaded = true;
    memset(g_entries, 0, sizeof(g_entries));
    nvs_handle_t h;
    if (nvs_open("m5ui", NVS_READONLY, &h) != ESP_OK) return;
    size_t len = sizeof(g_entries);
    if (nvs_get_blob(h, "wlist", g_entries, &len) != ESP_OK)
        memset(g_entries, 0, sizeof(g_entries));
    nvs_close(h);
}

inline void save() {
    nvs_handle_t h;
    if (nvs_open("m5ui", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, "wlist", g_entries, sizeof(g_entries));
    nvs_commit(h);
    nvs_close(h);
}

inline int find(const char* ssid) {
    if (!ssid) return -1;
    for (int i = 0; i < MAX_ENTRIES; ++i)
        if (g_entries[i].ssid[0] && strcmp(g_entries[i].ssid, ssid) == 0) return i;
    return -1;
}

// 加或更新。返回 0=ok -1=满了
inline int add(const char* ssid, const char* pass) {
    if (!ssid || !ssid[0]) return -1;
    int idx = find(ssid);
    if (idx < 0) {
        for (int i = 0; i < MAX_ENTRIES; ++i)
            if (!g_entries[i].ssid[0]) { idx = i; break; }
        if (idx < 0) return -1;
        snprintf(g_entries[idx].ssid, sizeof(g_entries[idx].ssid), "%s", ssid);
    }
    snprintf(g_entries[idx].pass, sizeof(g_entries[idx].pass), "%s", pass ? pass : "");
    return 0;
}

// 忘掉一个（后面往前挪，保持连续）
inline void remove(const char* ssid) {
    const int idx = find(ssid);
    if (idx < 0) return;
    for (int i = idx; i < MAX_ENTRIES - 1; ++i) g_entries[i] = g_entries[i + 1];
    memset(&g_entries[MAX_ENTRIES - 1], 0, sizeof(Entry));
}

} // namespace m5ui::wifi_list
