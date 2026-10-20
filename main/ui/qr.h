#pragma once

// ui/qr.h — 画二维码（用 M5GFX 自带的 lgfx_qrcode）
//
// 为什么不直接用 M5.Display.qrcode()：渲染是在已有的事务里跑的（startWrite 已经开着），
// 不想再套一层；而且它不返回成败，version 猜错就是静默什么都不画。
// 这里自己算 version、自己按模块 fillRect，失败能看出来。

#include <cstdint>
#include "lgfx/utility/lgfx_qrcode.h"   // m5gfx 组件里带的（自带头文件的 extern "C"）
#include <M5Unified.h>

namespace m5ui::ui::qr {

static uint8_t g_buf[4096];   // 模块位图（v10 也才 400 来字节，留足）

// 选一个「装得下」的最小 version。QR 的 version 决定容量，太大扫起来模块变小，
// 太小直接生成失败 —— 所以从 1 往上试，试到成功为止。失败返回 0。
inline uint8_t pick_version(const char* text) {
    if (!text || !text[0]) return 0;
    for (uint8_t v = 1; v <= 20; v++) {
        if (lgfx_qrcode_getBufferSize(v) > sizeof(g_buf)) break;
        QRCode q;
        if (lgfx_qrcode_initText(&q, g_buf, v, ECC_LOW, text) == 0) return v;
    }
    return 0;
}

// 画在 [x, y] 起、边长 size 的方块里。quiet = 静区（模块数）—— 扫码必须有静区。
// 返回实际用的 version（0 = 画不出来）。
inline uint8_t draw(const char* text, int32_t x, int32_t y, int32_t size, int quiet = 2) {
    uint8_t v = pick_version(text);
    if (!v) return 0;

    QRCode q;
    if (lgfx_qrcode_initText(&q, g_buf, v, ECC_LOW, text) != 0) return 0;

    const int n  = q.size + quiet * 2;        // 含静区的总模块数
    const int px = size / n;                  // 一个模块几个像素
    if (px < 2) return 0;                     // 太小就别硬画（扫不出来）
    const int32_t ox = x + (size - px * n) / 2;
    const int32_t oy = y + (size - px * n) / 2;

    M5.Display.fillRect(x, y, size, size, TFT_WHITE);   // 静区（也是底色）
    for (int my = 0; my < q.size; my++) {
        for (int mx = 0; mx < q.size; mx++) {
            if (lgfx_qrcode_getModule(&q, mx, my))
                M5.Display.fillRect(ox + (mx + quiet) * px, oy + (my + quiet) * px, px, px, TFT_BLACK);
        }
    }
    return v;
}

} // namespace m5ui::ui::qr
