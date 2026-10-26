// main/platform/sercli.h — 串口命令行 + 文件传输（USB-Serial-JTAG，插线就能用，不依赖 WiFi）
//
// 为什么要有它：设备走手机热点，人一走开网络就断，网页传文件就废了。插根 USB 线
// 永远可用，而且走的是同一条 /dev/ttyACM0（日志也在这条线上，所以**回复统一加
// "papers3: " 前缀**，PC 侧脚本靠前缀把回复和日志分开）。
//
// 协议（纯文本命令 + 块传输，PC 侧见 tools/sercli.py）：
//   help                        → 帮助
//   ls|pwd|cd|cat|rm|mv|cp|mkdir|df|free|ps|date|ver|wifi|ip|cfg|reboot …
//                               → 直接交给 papers3::sh（同一套命令，不重复实现）
//   put <path> <size> <crc32>   → 设备回 "papers3: go"，随后设备**原样收** size 字节，
//                                 先写 <path>.part，校验 crc32 通过才改名成 <path>；
//                                 回 "papers3: ok <size>" 或 "papers3: err <原因>"
//   get <path>                  → 设备回 "papers3: size <n>"，随后 n 字节原始数据，再 "papers3: eof"
//
// 安全：路径一律过 sh::norm()（只能落在 /sdcard 里）；收文件失败绝不会留下半个正式文件。
#pragma once

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <sys/stat.h>
#include <unistd.h>

#include "driver/usb_serial_jtag.h"
#include "esp_rom_crc.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "platform/sh.h"

namespace papers3::sercli {

static const char* TAG = "SERCLI";

// ── 收发原语 ──
inline int  wr(const char* s, int n) { return usb_serial_jtag_write_bytes(s, (size_t)n, pdMS_TO_TICKS(500)); }
inline void say(const char* s) { wr(s, (int)strlen(s)); }
inline void sayf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
inline void sayf(const char* fmt, ...) {
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    say(b);
}
// 约定：所有回复都以它开头
inline void rep(const char* s) { say("papers3: "); say(s); say("\n"); }
inline void repf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
inline void repf(const char* fmt, ...) {
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    rep(b);
}

// 把一段文本按行加前缀回给 PC（sh 的输出是多行）
inline void rep_text(const char* text) {
    const char* p = text;
    while (p && *p) {
        const char* e = strchr(p, '\n');
        const int len = e ? (int)(e - p) : (int)strlen(p);
        char line[600];
        if (len > 0) {
            say("papers3: ");
            wr(p, len > (int)sizeof(line) - 1 ? (int)sizeof(line) - 1 : len);
            say("\n");
        }
        if (!e) break;
        p = e + 1;
    }
}

// 读满 n 字节（超时返回已读字节数）
inline int rd_full(uint8_t* dst, int n, int timeout_ms) {
    int got = 0;
    const int64_t t0 = esp_timer_get_time() / 1000;
    while (got < n) {
        const int r = usb_serial_jtag_read_bytes(dst + got, (size_t)(n - got), pdMS_TO_TICKS(200));
        if (r > 0) { got += r; continue; }
        if ((int64_t)(esp_timer_get_time() / 1000) - t0 > timeout_ms) break;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return got;
}

// ── 文件传输 ──
inline uint8_t* xbuf() {                       // 4KB 传输缓冲（PSRAM：内部 RAM 金贵）
    static uint8_t* p = nullptr;
    if (!p) p = (uint8_t*)heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
    return p;
}

inline void do_put(char* args) {
    // put <path> <size> <crc32>
    char* path = strtok(args, " ");
    char* szs  = strtok(nullptr, " ");
    char* crcs = strtok(nullptr, " ");
    if (!path || !szs || !crcs) { rep("err usage: put <path> <size> <crc32>"); return; }
    const long size = atol(szs);
    const uint32_t want_crc = (uint32_t)strtoul(crcs, nullptr, 16);
    if (size <= 0 || size > 32L * 1024 * 1024) { repf("err size %ld", size); return; }

    char full[400];
    sh::norm(path, full, sizeof(full));
    char part[420];
    snprintf(part, sizeof(part), "%s.part", full);

    uint8_t* buf = xbuf();
    if (!buf) { rep("err no mem"); return; }

    FILE* f = fopen(part, "wb");
    if (!f) { repf("err cannot write %s", part); return; }
    rep("go");

    long left = size;
    uint32_t crc = 0;
    while (left > 0) {
        const int want = left > 4096 ? 4096 : (int)left;
        const int got = rd_full(buf, want, 8000);
        if (got <= 0) break;
        if (fwrite(buf, 1, (size_t)got, f) != (size_t)got) { left = -1; break; }
        crc = esp_rom_crc32_le(crc, buf, (uint32_t)got);
        left -= got;
        // ★ 分块确认：写 SD 可能堵住几十~几百 ms，期间 USB 收缓冲会溢出丢字节
        //   （USB-CDC 没有硬件流控）。所以每收一块就回一个 ack，PC 收到才发下一块。
        repf("ack %ld", size - left);
    }
    fclose(f);

    if (left != 0) { unlink(part); repf("err incomplete %ld bytes left", left); return; }
    if (crc != want_crc) { unlink(part); repf("err crc %08x != %08x", (unsigned)crc, (unsigned)want_crc); return; }
    unlink(full);                                    // FATFS 的 rename 不覆盖已有文件
    if (rename(part, full) != 0) { unlink(part); rep("err rename"); return; }
    repf("ok %ld", size);
}

inline void do_get(char* args) {
    char* path = strtok(args, " ");
    if (!path) { rep("err usage: get <path>"); return; }
    char full[400];
    sh::norm(path, full, sizeof(full));
    struct stat st;
    if (stat(full, &st) != 0 || S_ISDIR(st.st_mode)) { repf("err no such file %s", full); return; }
    FILE* f = fopen(full, "rb");
    if (!f) { repf("err cannot open %s", full); return; }

    uint8_t* buf = xbuf();
    if (!buf) { fclose(f); rep("err no mem"); return; }

    repf("size %ld", (long)st.st_size);
    size_t r;
    while ((r = fread(buf, 1, 4096, f)) > 0) {
        if (wr((const char*)buf, (int)r) <= 0) { fclose(f); return; }   // PC 那头断了
    }
    fclose(f);
    rep("eof");
}

// ── 一条命令 ──
inline void handle_line(char* line) {
    int L = (int)strlen(line);
    while (L > 0 && (line[L - 1] == '\r' || line[L - 1] == '\n' || line[L - 1] == ' ')) line[--L] = 0;
    if (!L) return;

    if (!strncmp(line, "put ", 4)) { do_put(line + 4); return; }
    if (!strncmp(line, "get ", 4)) { do_get(line + 4); return; }
    if (!strcmp(line, "help")) {
        rep("命令：ls cd pwd cat head tail wc df free ps uptime date ver log wifi ip cfg reboot");
        rep("文件：put <path> <size> <crc32> / get <path>");
        rep("路径简写 /apps → /sdcard/apps；写操作只在 /sdcard 内");
        return;
    }

    // 其余交给 shell（同一套命令实现）
    static char out[3072];
    int code = 0, flags = 0;
    const int n = sh::exec(line, out, sizeof(out), &code, &flags);
    if (n > 0) rep_text(out);
    if (code && code != 0) repf("code %d", code);
    if (flags & sh::FLAG_REBOOT) {
        say("papers3: rebooting\n");
        vTaskDelay(pdMS_TO_TICKS(300));
        esp_restart();
    }
}

inline void task(void*) {
    ESP_LOGI(TAG, "串口命令行就绪：put/get 传文件，其余命令同 shell");
    static char line[240];
    int n = 0;
    for (;;) {
        uint8_t c = 0;
        const int r = usb_serial_jtag_read_bytes(&c, 1, pdMS_TO_TICKS(200));
        if (r <= 0) { vTaskDelay(pdMS_TO_TICKS(10)); continue; }
        if (c == '\r') continue;
        if (c == '\n') { line[n] = 0; if (n) handle_line(line); n = 0; continue; }
        if (n < (int)sizeof(line) - 1) line[n++] = (char)c;
        else n = 0;                                   // 行太长：丢弃
    }
}

inline bool start() {
    // 装 USJ 驱动（控制台只用它输出，输入这边要自己开）
    usb_serial_jtag_driver_config_t cfg = {};
    cfg.tx_buffer_size = 4096;
    cfg.rx_buffer_size = 4096;
    const esp_err_t e = usb_serial_jtag_driver_install(&cfg);
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "usb_serial_jtag_driver_install 失败: %s", esp_err_to_name(e));
        return false;
    }
    // core0（和 WiFi/epd 同核；UI 核只跑界面）
    xTaskCreatePinnedToCore(task, "sercli", 6144, nullptr, 3, nullptr, 0);
    return true;
}

} // namespace papers3::sercli
