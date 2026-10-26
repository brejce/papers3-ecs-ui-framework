#pragma once

#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdspi_host.h"
#include "driver/spi_master.h"
#include <dirent.h>
#include <sys/stat.h>
#include <cstdio>
#include <cstring>
#include "esp_log.h"

// platform/sd.h — SD 卡
//
// 挂载（SPI2）+ 找文件。以后的手机扫码文件管理（增删改查 SD）也从这里长。

namespace papers3::sd {

// M5PaperS3 的 SD 卡是 SPI 模式（不是 SDMMC）：
//   CLK=39, MOSI(COPI)=38, MISO(CIPO)=40, CS=47
// 显示器是 8 位并行 EPD 总线（不占 SPI），触摸是 I2C → SPI2 完全空闲，给 SD 用。

static sdmmc_card_t* g_card = nullptr;   // 挂载成功后常驻，供后续读文件用

// 挂载 SD（SPI2）。已挂载则直接返回 true。
inline bool mount() {
    if (g_card) return true;

    static constexpr int SD_CLK  = 39;
    static constexpr int SD_MOSI = 38;
    static constexpr int SD_MISO = 40;
    static constexpr int SD_CS   = 47;

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();   // slot = SPI2_HOST
    // ★ 曾经试过 host.max_freq_khz = 40000（想提速）：**实测零收益** —— 卡自己降档回 20MHz，
    //   却让 sdmmc_card_init 更容易 CRC 失败（实测出现过 mount failed ESP_ERR_INVALID_CRC，
    //   整台设备"没有 SD"、launcher 里一个 app 都看不到）。已撤回，保持默认 20MHz。

    spi_bus_config_t bus_cfg = {};
    bus_cfg.mosi_io_num = SD_MOSI;
    bus_cfg.miso_io_num = SD_MISO;
    bus_cfg.sclk_io_num = SD_CLK;
    bus_cfg.quadwp_io_num = -1;
    bus_cfg.quadhd_io_num = -1;
    bus_cfg.max_transfer_sz = 16384;   // 大块 DMA 读 2MB 字体更快

    esp_err_t ret = spi_bus_initialize((spi_host_device_t)host.slot, &bus_cfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE("SD", "spi_bus_initialize: %s", esp_err_to_name(ret));
        return false;
    }

    sdspi_device_config_t slot_cfg = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_cfg.gpio_cs = (gpio_num_t)SD_CS;
    slot_cfg.host_id = (spi_host_device_t)host.slot;

    esp_vfs_fat_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
    };

    sdmmc_card_t* card = nullptr;
    // ★ 挂载要重试：SD 在 SPI 模式下**偶发** CRC 错（实测开机 mount failed
    //   ESP_ERR_INVALID_CRC）。一次失败就放弃的代价极大 —— 整个设备"没有 SD"，
    //   launcher 里一个 app 都看不到、书也读不了，而现象完全不像"卡接触不良"。
    esp_err_t ret2 = ESP_FAIL;
    for (int attempt = 0; attempt < 3; attempt++) {
        ret2 = esp_vfs_fat_sdspi_mount("/sdcard", &host, &slot_cfg, &mount_cfg, &card);
        if (ret2 == ESP_OK) break;
        ESP_LOGW("SD", "mount 第 %d 次失败：%s —— 重试", attempt + 1, esp_err_to_name(ret2));
        vTaskDelay(pdMS_TO_TICKS(150));
    }
    if (ret2 != ESP_OK) {
        ESP_LOGE("SD", "mount failed: %s", esp_err_to_name(ret2));
        spi_bus_free((spi_host_device_t)host.slot);
        return false;
    }
    g_card = card;
    ESP_LOGI("SD", "card: %.8s, %.1f MHz, %llu MB",
             card->cid.name,
             (double)card->real_freq_khz / 1000.0,
             (unsigned long long)((uint64_t)card->csd.capacity * card->csd.sector_size / 1024 / 1024));
    return true;
}

// 递归找第一个 .ttf（大小写不敏感），把完整路径写进 out。
static bool _find_ttf(const char* base, int depth, char* out, size_t out_len) {
    if (depth > 3) return false;
    DIR* d = opendir(base);
    if (!d) return false;
    struct dirent* e;
    while ((e = readdir(d)) != nullptr) {
        if (e->d_name[0] == '.') continue;
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", base, e->d_name);
        struct stat st;
        if (stat(path, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            if (_find_ttf(path, depth + 1, out, out_len)) { closedir(d); return true; }
        } else {
            size_t l = strlen(e->d_name);
            if (l >= 4 && e->d_name[l - 4] == '.' &&
                (e->d_name[l - 3] | 0x20) == 't' &&
                (e->d_name[l - 2] | 0x20) == 't' &&
                (e->d_name[l - 1] | 0x20) == 'f') {
                snprintf(out, out_len, "%s", path);
                closedir(d);
                return true;
            }
        }
    }
    closedir(d);
    return false;
}

inline bool find_ttf(char* out, size_t out_len) {
    if (!mount()) return false;
    return _find_ttf("/sdcard", 0, out, out_len);
}

// ── 字体列表：扫 /sdcard/fonts 下的 .ttf（**只扫这一层**，不递归）──
// 专目录：全卡递归会把书、词库、杂物里的 .ttf 都列出来，设置页会乱。
// 返回个数（≤ max）。name 是**文件名**（不是路径），路径由调用方拼
// /sdcard/fonts/<name> —— NVS 里也只存相对路径。
inline constexpr const char* FONT_DIR = "/sdcard/fonts";

struct FontFile {
    char     name[80];
    uint32_t size;
};

inline bool is_ttf(const char* n) {
    size_t l = strlen(n);
    return l >= 4 && n[l-4] == '.' &&
           (n[l-3] | 0x20) == 't' && (n[l-2] | 0x20) == 't' && (n[l-1] | 0x20) == 'f';
}

// ── 屏保图列表：扫 /sdcard/screensaver 下的 .png（**只扫这一层**，规矩同字体目录）──
inline constexpr const char* SCREENSAVER_DIR = "/sdcard/screensaver";

inline bool is_png(const char* n) {
    size_t l = strlen(n);
    return l >= 4 && n[l-4] == '.' &&
           (n[l-3] | 0x20) == 'p' && (n[l-2] | 0x20) == 'n' && (n[l-1] | 0x20) == 'g';
}

inline int scan_screensavers(FontFile* out, int max) {
    if (!mount() || !out || max <= 0) return 0;
    DIR* d = opendir(SCREENSAVER_DIR);
    if (!d) {
        ESP_LOGI("SD", "%s 打不开（还没传图？）", SCREENSAVER_DIR);
        return 0;
    }
    int n = 0;
    struct dirent* e;
    while ((e = readdir(d)) != nullptr && n < max) {
        if (e->d_name[0] == '.' || !is_png(e->d_name)) continue;
        char path[320];
        snprintf(path, sizeof(path), "%s/%s", SCREENSAVER_DIR, e->d_name);
        struct stat st;
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        snprintf(out[n].name, sizeof(out[n].name), "%.*s",
                 (int)sizeof(out[n].name) - 1, e->d_name);
        out[n].size = (uint32_t)st.st_size;
        n++;
    }
    closedir(d);
    ESP_LOGI("SD", "%s: %d 张 .png", SCREENSAVER_DIR, n);
    return n;
}

inline int scan_fonts(FontFile* out, int max) {
    if (!mount() || !out || max <= 0) return 0;
    DIR* d = opendir(FONT_DIR);
    if (!d) {
        ESP_LOGW("SD", "%s 打不开（还没建这个目录？）", FONT_DIR);
        return 0;
    }
    int n = 0;
    struct dirent* e;
    while ((e = readdir(d)) != nullptr && n < max) {
        if (e->d_name[0] == '.' || !is_ttf(e->d_name)) continue;
        char path[320];
        snprintf(path, sizeof(path), "%s/%s", FONT_DIR, e->d_name);
        struct stat st;
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        snprintf(out[n].name, sizeof(out[n].name), "%.*s",
                 (int)sizeof(out[n].name) - 1, e->d_name);
        out[n].size = (uint32_t)st.st_size;
        n++;
    }
    closedir(d);
    ESP_LOGI("SD", "%s: %d 个 .ttf", FONT_DIR, n);
    return n;
}

// 建字体目录（网页上传前调一下，省得用户自己建）
inline void ensure_font_dir() {
    if (!mount()) return;
    struct stat st;
    if (stat(FONT_DIR, &st) != 0) { mkdir(FONT_DIR, 0777); ESP_LOGI("SD", "建目录 %s", FONT_DIR); }
}

// 卡挂上了吗
inline bool mounted() { return g_card != nullptr; }

// 运行中重试挂载 —— 给「卡是开机后才插上的」用（网页那边会调）。
// 失败的重试要重新走一遍 SPI 总线初始化，一次一两百毫秒，所以不是每次都试，
// 每 3 次调用最多探一次卡（网页刷新几下就能认出来）。
inline bool remount_if_absent() {
    if (g_card) return true;
    static int n = 0;
    if (++n % 3 != 0) return false;
    ESP_LOGI("SD", "再试一次挂载 SD…");
    return mount();
}

} // namespace papers3::probe
