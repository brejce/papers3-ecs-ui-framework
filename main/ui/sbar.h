#pragma once

// ──────────────────────────────────────────────────────────────
// ui/sbar.h — 全局统一状态栏（唯一实现）
//
// 绘制口径**照 apps/settings 的状态栏**（用户 2026-10-25 的口径：
// 「只有设置 app 画的状态栏的行为是对的」）：
//   · 字号 22 —— 和设置 app 的 SZ_BAR 同一档（以前这里是 16，比正文还小一档）
//   · 时钟按**真墨迹**居中（先量 measure_extent 的 l/r，再算起点），和设置同一套算法
//   · 垂直位置 = (H - line_height) / 2，也是设置的算法
//   · **一趟画完、只推一次屏**：底（FAST 写 → 4×4 点阵）+ 左标签 + 时钟 + BAT + 分隔线
//     全部写进帧缓冲，最后 epd_push 一次。以前是「推底 → 推字 → 推线」三次，
//     进 app 时那一秒钟里时间会被推两三遍（肉眼看得见闪）。
//   · tick()：分钟没跳一次都不推；跳了只擦/推「旧 ∪ 新」墨迹并集，同样只推一次。
//     draw() 会把当前分钟记进 g_last —— 否则进 app 后第一帧 tick 又把时间重画一遍
//     （这就是设置 app 不会、别的 app 会的原因）。
//
// 点阵底：0xE8E8E8 必须在 FAST 档下**写**（写进帧缓冲时就是 1bit + 4×4 Bayer），
// 之后再怎么推都是点阵；用 GC13 档**写**才会是平灰。所以擦/底永远走 mode 3 写、
// 字走 mode 1 写，推屏档位不再影响底纹。
//
// 页面用法（宿主页面）：
//     papers3::ui::sbar::set("< BACK", back_cb);   // 注册左键语义
//     papers3::ui::sbar::draw();                   // 进页时画（只推一次屏）
//     papers3::ui::sbar::tick();                   // 每帧调：分钟变了自动刷时间
//
// .so app 用法：so_bar_set("< EXIT", cb_or_null)  —— 画和点都归固件，
//   cb 为 nullptr 表示左键 = 退出 app（so_exit）。
// ──────────────────────────────────────────────────────────────

#include <cstring>
#include <esp_timer.h>
#include "M5Unified.h"
#include "text/ttf.h"
#include "systems/render.h"      // apply_epd_mode / epd_push
#include "platform/rtc_time.h"   // 时钟

namespace papers3::ui::sbar {

inline constexpr int32_t  H      = 60;         // 高度（所有页面统一）
inline constexpr int32_t  PADX   = 24;
inline constexpr int32_t  SZ     = 22;         // ★ 字号：与设置 app 的 SZ_BAR 同档
inline constexpr uint32_t L14    = 0xE8E8E8;   // 点阵底
inline constexpr uint32_t INK    = 0x000000;
inline constexpr uint32_t PAPER  = 0xFFFFFF;
inline constexpr uint8_t  GRAY   = 25;         // 次要信息（时钟 / BAT 字样）
inline constexpr int32_t  HIT_Y  = H + 14;     // 左键热区高度
inline constexpr int32_t  HIT_X  = 190;        // 左键热区宽度
inline constexpr int64_t  CLK_US = 5000000;    // 时钟 5s 看一次（同设置 app）

using Cb = void (*)(void);

inline char g_left[24] = "< BACK";
inline Cb   g_cb       = nullptr;
inline int  g_last     = -1;              // 上次画上去的分钟值（-1 = 没画过）
inline char g_clock[16] = "--:--";
inline int  g_bat      = -1;

inline void set(const char* left, Cb cb) {
    if (left) snprintf(g_left, sizeof(g_left), "%s", left);
    g_cb = cb;
}

// 文字的纵向起点：按**行高**居中（与设置 app 一致：line_height 含 descent）
inline int32_t ty() {
    papers3::ttf::set_size((float)SZ);
    return (H - papers3::ttf::line_height()) / 2;
}

// 时钟横坐标：按真墨迹居中（advance 求和含首字左/末字右留白，会让时钟偏离中线）
inline int32_t clock_x(const char* s) {
    papers3::ttf::set_size((float)SZ);
    int32_t l = 0, r = 0;
    papers3::ttf::measure_extent(s, &l, &r);
    if (r > l) return (540 - (r - l)) / 2 - l;
    int32_t w = 0, h = 0;
    papers3::ttf::measure(s, &w, &h);
    return (540 - w) / 2;
}

// 画整条（含点阵底）。left == nullptr 时用之前 set 的标签。**整条只推一次屏。**
inline void draw(const char* left = nullptr, bool full = true) {
    if (left) snprintf(g_left, sizeof(g_left), "%s", left);

    // ① 底：mode 3 **写**（1bit + Bayer）→ 点阵。只改帧缓冲，不上屏。
    systems::apply_epd_mode(3);
    M5.Display.fillRect(0, 0, 540, H, L14);

    // ② 字：mode 1（GC13）写，抗锯齿灰
    systems::apply_epd_mode(1);
    papers3::ttf::set_size((float)SZ);
    const int32_t y = ty();
    if (full) papers3::ttf::draw_text(PADX, y, g_left, INK, PAPER);

    const int t = papers3::rtctime::hhmm();
    g_last = t;                        // ★ 记下画的是哪一分钟：别让进 app 后第一帧 tick 重画
    if (t < 0) snprintf(g_clock, sizeof(g_clock), "--:--");
    else       snprintf(g_clock, sizeof(g_clock), "%02d:%02d", t / 100, t % 100);
    papers3::ttf::draw_text(clock_x(g_clock), y, g_clock, GRAY, PAPER);

    g_bat = (int)M5.Power.getBatteryLevel();
    char b[16];
    if (g_bat < 0) snprintf(b, sizeof(b), "--");
    else           snprintf(b, sizeof(b), "%d", g_bat);
    const int32_t cw = papers3::ttf::measure_prefix("0", 1);   // '0' 的 advance = 数字格宽
    papers3::ttf::draw_text(540 - PADX - 3 * cw - 12 - 44, y, "BAT", GRAY, PAPER);
    papers3::ttf::draw_text(540 - PADX - (int32_t)strlen(b) * cw, y, b, INK, PAPER);
    papers3::ttf::draw_text(540 - PADX + 3, y, "%", INK, PAPER);

    // ③ 分隔线（同一趟，别再单独推）
    M5.Display.fillRect(0, H - 1, 540, 1, INK);

    // ④ **只推一次**：底、字、线一起上屏
    systems::epd_push(0, 0, 540, H);
    M5.Display.waitDisplay();
}

// 每帧调：分钟变了只重画中间时间那一小块（不动左右），并且**只推一次**
inline void tick() {
    static int64_t last_us = -(int64_t)CLK_US;   // 进来先查一次，之后 5s 一次
    const int64_t now = esp_timer_get_time();
    if (now - last_us < CLK_US) return;
    last_us = now;

    const int t = papers3::rtctime::hhmm();
    if (t == g_last) return;                     // 分钟没跳 → 一次都不推

    papers3::ttf::set_size((float)SZ);
    // ① 先量**旧时间**的墨迹范围（改缓冲之前）
    const int32_t old_x = clock_x(g_clock);
    int32_t ol = 0, orr = 0;
    papers3::ttf::measure_extent(g_clock, &ol, &orr);

    g_last = t;
    if (t < 0) snprintf(g_clock, sizeof(g_clock), "--:--");
    else       snprintf(g_clock, sizeof(g_clock), "%02d:%02d", t / 100, t % 100);
    const int32_t new_x = clock_x(g_clock);
    int32_t nl = 0, nr = 0;
    papers3::ttf::measure_extent(g_clock, &nl, &nr);

    // ② 擦/推「旧 ∪ 新」的真实墨迹，±3px 余量（固定 180px 宽会每分钟闪一大块）
    int32_t x0 = (old_x + ol < new_x + nl) ? (old_x + ol) : (new_x + nl);
    int32_t x1 = (old_x + orr > new_x + nr) ? (old_x + orr) : (new_x + nr);
    x0 -= 3; x1 += 3;
    if (x0 < 0) x0 = 0;
    if (x1 > 540) x1 = 540;
    const int32_t w = x1 - x0;
    if (w <= 0) return;

    // ③ 擦（mode 3 写 → 仍是点阵）→ 写新字（mode 1）→ **一次**推屏
    systems::apply_epd_mode(3);
    M5.Display.fillRect(x0, 0, w, H - 1, L14);   // H-1：别把 1px 分隔线擦掉
    systems::apply_epd_mode(1);
    papers3::ttf::set_size((float)SZ);
    papers3::ttf::draw_text(new_x, ty(), g_clock, GRAY, PAPER);
    systems::epd_push(x0, 0, w, H);
    M5.Display.waitDisplay();
}

inline bool hits(int32_t x, int32_t y) { return y < HIT_Y && x < HIT_X; }

// 左键被点：有回调走回调，没有就当"退出当前页"
inline void press_left() {
    if (g_cb) { g_cb(); return; }
    M5.Display.fillRect(0, 0, 540, H, L14);   // 兜底：别把状态栏留在屏幕上
}

}  // namespace papers3::ui::sbar
