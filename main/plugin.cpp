/* plugin.cpp — 给 .so 插件提供的宿主 API（字体 + 文件 + 绘制 + 退出）。
 *
 * 关键约束：elf_loader 的 libc 符号表是**白名单**，只有 string.h/stdio.h/stdlib.h
 * 里那几十个（printf/malloc/strcmp/memcpy…），**没有任何文件系统 API**：
 * fopen/opendir/readdir/fread/fseek/ftell/snprintf/strcat/strcpy 全不在。
 * 所以 .so 里不能自己读文件、扫目录、拼路径、格式化 —— 这些全是固件的能力，
 * 宿主导出一套语义 API 给 .so 用。.so 只留纯逻辑（解析/排版/状态机）。
 *
 * 宿主侧（本文件）是固件，不受白名单限制，想用什么 libc 用什么。
 */

#include <M5Unified.h>
#include <M5GFX.h>
#include <lgfx/v1/platforms/esp32/Panel_EPD.hpp>   // so_fb_save4/restore4 要 copy_region_out/in
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include "esp_elf.h"     // esp_elfsym / ESP_ELFSYM_EXPORT / ESP_ELFSYM_END
#include "esp_dlfcn.h"   // dlopen / dlsym / dlerror
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "platform/rtc_time.h"   // so_clock_hhmm：RTC 供时 + 网络校准
#include "esp_timer.h"   // so_now_ms（给 .so 自测各阶段耗时）
#include "freertos/FreeRTOS.h"   // core1 触摸采样任务（touch_task_start）
#include "freertos/task.h"
#include "text/ttf.h"    // FontCtx（多实例字体）
#include "platform/sd.h" // 文件能力（宿主侧）
#include "pages/manager.h" // so_exit 回 launcher 用
#include "systems/render.h" // apply_epd_mode —— 波形档必须和固件页面走同一条路
#include "platform/nvs.h"  // cfg::init()（幂等）—— 页快照自检的结论要存 NVS
#include "platform/wifi_ap.h"   // so_net_*：热点开/关（给设置 app）
#include "platform/wifi_sta.h"  // so_net_*：STA 状态/连接
#include "platform/web_files.h" // so_net_*：网页文件服务开/关
#include "platform/wifi_list.h"  // so_wifi_*：多热点列表
#include "platform/sound.h"     // so_sound_test：反馈音试听
#include "platform/sh.h"        // so_sh_exec：设备上的真 shell（apps/terminal）
#include "systems/epd_lut.h"    // so_fwcfg_apply：切波形档要装对应的表

// ── 导出给 .so 调用的绘制原语 ─────────────────────────────────────────

// 画一个实心矩形到帧缓冲（不推屏）。波形档由调用方用 so_set_epd_mode 设
// （跟 so_fill_rect 一致：绘制原语只写帧缓冲、不碰档位、不推屏）。
// ★ 早期版本这里画完就 display+waitDisplay（每画一笔推一次屏）—— 是反模式：
//   推屏一次几百 ms，连续画几个元素就卡死。已修。
extern "C" int so_rect(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color)
{
    if (w <= 0 || h <= 0) return -1;
    M5.Display.fillRect(x, y, w, h, color);
    return 0;
}

// 用 M5GFX 内置点阵字画一行文字到帧缓冲（不推屏）。
// 注：m5ui 正文/UI 都用 TTF（so_font_draw），这是内置点阵字，通常只作兜底。
extern "C" int so_text(int32_t x, int32_t y, const char* s, uint32_t color)
{
    if (!s || !*s) return -1;
    M5.Display.setTextColor(color);
    M5.Display.drawString(s, x, y);
    return 0;
}

// 字符串格式化（.so 白名单没有 snprintf/sprintf）
extern "C" int so_snprintf(char* out, int cap, const char* fmt, ...)
{
    if (!out || cap <= 0) return 0;
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(out, (size_t)cap, fmt, ap);
    va_end(ap);
    return r;
}

// ── 导出给 .so 的 TTF 字体 API（多实例，句柄 = FontCtx*）────────────────
//
// 为什么字体能力由固件提供：栅格化是「基本能力」，固件页面（设置/键盘/主页）和
// .so 插件共用同一份 stb_truetype + 字形合成逻辑。.so 只持句柄，换字体/字号/字间距
// 都是句柄自己的事，不动固件全局字体 g_default。

extern "C" void* so_font_load(const char* name, float px)
{
    if (!name) return nullptr;
    char path[320];
    snprintf(path, sizeof(path), "%s/%s", m5ui::sd::FONT_DIR, name);   // /sdcard/fonts/<name>

    auto* c = (m5ui::ttf::FontCtx*)heap_caps_malloc(sizeof(m5ui::ttf::FontCtx), MALLOC_CAP_8BIT);
    if (!c) return nullptr;
    new (c) m5ui::ttf::FontCtx();
    if (!m5ui::ttf::load_from_sd(*c, path, px)) {
        m5ui::ttf::free_font(*c);
        heap_caps_free(c);
        return nullptr;
    }
    return c;
}

extern "C" void so_font_free(void* h)
{
    if (!h) return;
    auto* c = (m5ui::ttf::FontCtx*)h;
    m5ui::ttf::free_font(*c);
    heap_caps_free(c);
}

extern "C" void so_font_set_size(void* h, float px)
{
    if (h) m5ui::ttf::set_size(*(m5ui::ttf::FontCtx*)h, px);
}

extern "C" int so_font_measure(void* h, const char* s, int32_t* out_w, int32_t* out_h)
{
    if (!h || !s) return -1;
    m5ui::ttf::measure(*(m5ui::ttf::FontCtx*)h, s, out_w, out_h);
    return 0;
}

// 量前 char_count 个字符（字符数，非字节）的像素宽度 —— 折行二分用。
// ★ 性能关键：.so 侧折行用「二分 + 本函数」= O(log 字数) 次跨边界调用；
//   若改成逐字 so_font_measure = O(字数) 次跨边界，翻一页上千次跳转，慢一个量级。
extern "C" int32_t so_font_measure_prefix(void* h, const char* s, int32_t char_count)
{
    if (!h || !s || char_count <= 0) return 0;
    return m5ui::ttf::measure_prefix(*(m5ui::ttf::FontCtx*)h, s, (int)char_count);
}

// ── 批处理的脏区累计（纯本文件的矩形并集；**不再动 m5gfx 头文件**）──
//   .so 侧：so_begin_write() → 连续画一批 → so_end_write()；
//   end 时把这一批写过的所有矩形并成一个、只推一次（面板本来就是按矩形推的）。
//   只有 g_write_open 为真才记账 —— 非批处理的调用零开销，行为不变。
//   ★ 位置必须在 so_font_draw / so_fill_rect **之前**（它们要调 dirty_add）。
static bool g_write_open = false;
static int32_t g_dx0 = 0, g_dy0 = 0, g_dx1 = 0, g_dy1 = 0;
static void dirty_add(int32_t x, int32_t y, int32_t w, int32_t h)
{
    if (!g_write_open || w <= 0 || h <= 0) return;
    const int32_t x1 = x + w, y1 = y + h;
    if (g_dx1 <= g_dx0) { g_dx0 = x; g_dy0 = y; g_dx1 = x1; g_dy1 = y1; return; }
    if (x  < g_dx0) g_dx0 = x;
    if (y  < g_dy0) g_dy0 = y;
    if (x1 > g_dx1) g_dx1 = x1;
    if (y1 > g_dy1) g_dy1 = y1;
}
static void dirty_reset(void) { g_dx0 = 0; g_dy0 = 0; g_dx1 = 0; g_dy1 = 0; }

extern "C" int so_font_draw(void* h, int32_t x, int32_t y, const char* s, uint8_t ink, int32_t ls)
{
    if (!h || !s) return 0;
    m5ui::ttf::FontCtx& c = *(m5ui::ttf::FontCtx*)h;
    if (!c.ready) return 0;
    // ★ 批处理：把这次要画的文字记进脏区（用实测墨迹 + 各 4px 余量）
    { int32_t mw = 0, mh = 0; m5ui::ttf::measure(c, s, &mw, &mh);
      if (mw > 0 && mh > 0) dirty_add(x - 4, y - 4, mw + 8, mh + 8); }
    int pen = x;
    const char* p = s;
    int32_t cp;
    while ((cp = m5ui::ttf::utf8_next(p)) != 0) {
        m5ui::ttf::Glyph g = m5ui::ttf::rasterize(c, cp);
        if (g.gray) {
            m5ui::ttf::push_glyph(g, pen + g.xoff, y + c.ascent + g.yoff, ink);
            m5ui::ttf::free_glyph(g);
        }
        pen += g.advance + ls;
    }
    return pen - x;
}

extern "C" int so_font_line_height(void* h)
{
    if (!h) return 0;
    return m5ui::ttf::line_height(*(m5ui::ttf::FontCtx*)h);
}

// 读当前字号（.so 排版临时改字号后要还原，需要读回原值）
extern "C" float so_font_size(void* h)
{
    if (!h) return 0;
    return m5ui::ttf::size_px(*(m5ui::ttf::FontCtx*)h);
}

extern "C" int so_font_has(void* h, int32_t cp)
{
    if (!h) return 0;
    return m5ui::ttf::has_codepoint(*(m5ui::ttf::FontCtx*)h, cp);
}

// ── 字形真墨迹框 ────────────────────────────────────────────────
// ★ 不能拿 advance 当墨迹宽：字形会超出 advance（左右 overhang、% / 斜杠 / 斜体），
//   用 advance−2 估出来的框会在边缘留下残墨（真机上被抓到过）。
//   这里直接用 stbtt 的字形位图框（Glyph 的 xoff/yoff/w/h，就是 GetCodepointBitmapBox 的结果），
//   和 so_font_draw 走的是同一份数据 —— 坐标口径与绘制完全一致。
//
//   返回的是**相对文字原点 (x,y) 的偏移**：画的时候 glyph 位图左上角落在
//   (pen + xoff, y + ascent + yoff)，所以 dx0 = xoff、dy0 = ascent + yoff。
static int _ink_of_glyph(const m5ui::ttf::FontCtx& c, m5ui::ttf::Glyph& g, int pen,
                         int* l, int* t, int* r, int* b)
{
    if (!g.gray || g.w <= 0 || g.h <= 0) return 0;      // 空白字形（空格等）不参与
    const int gx0 = pen + g.xoff;
    const int gy0 = c.ascent + g.yoff;
    const int gx1 = gx0 + g.w;
    const int gy1 = gy0 + g.h;
    if (gx0 < *l) *l = gx0;
    if (gy0 < *t) *t = gy0;
    if (gx1 > *r) *r = gx1;
    if (gy1 > *b) *b = gy1;
    return 1;
}

extern "C" int so_font_ink_bounds(void* h, int32_t cp, int32_t* x0, int32_t* y0, int32_t* x1, int32_t* y1)
{
    if (!h) return -1;
    m5ui::ttf::FontCtx& c = *(m5ui::ttf::FontCtx*)h;
    if (!c.ready) return -1;
    m5ui::ttf::Glyph g = m5ui::ttf::rasterize(c, cp);
    int l = 1 << 20, t = 1 << 20, r = -(1 << 20), b = -(1 << 20);
    const int any = _ink_of_glyph(c, g, 0, &l, &t, &r, &b);
    m5ui::ttf::free_glyph(g);
    if (!any) {
        if (x0) *x0 = 0;
        if (y0) *y0 = 0;
        if (x1) *x1 = 0;
        if (y1) *y1 = 0;
        return 0;
    }
    if (x0) *x0 = l;
    if (y0) *y0 = t;
    if (x1) *x1 = r;
    if (y1) *y1 = b;
    return 0;
}

// 一整串文字的墨迹框（所有字形并集），相对文字原点；ls = 字间距（同 so_font_draw）。
// .so 侧要「擦旧值 + 画新值」时把两个串的框并起来就是**精确**的擦除范围。
extern "C" int so_font_text_ink(void* h, const char* s, int32_t ls,
                               int32_t* x0, int32_t* y0, int32_t* x1, int32_t* y1)
{
    if (!h || !s) return -1;
    m5ui::ttf::FontCtx& c = *(m5ui::ttf::FontCtx*)h;
    if (!c.ready) return -1;
    int l = 1 << 20, t = 1 << 20, r = -(1 << 20), b = -(1 << 20);
    int pen = 0, any = 0;
    const char* p = s;
    int32_t cp;
    while ((cp = m5ui::ttf::utf8_next(p)) != 0) {
        m5ui::ttf::Glyph g = m5ui::ttf::rasterize(c, cp);
        any |= _ink_of_glyph(c, g, pen, &l, &t, &r, &b);
        pen += g.advance + ls;
        m5ui::ttf::free_glyph(g);
    }
    if (!any) {
        if (x0) *x0 = 0;
        if (y0) *y0 = 0;
        if (x1) *x1 = 0;
        if (y1) *y1 = 0;
        return 0;
    }
    if (x0) *x0 = l;
    if (y0) *y0 = t;
    if (x1) *x1 = r;
    if (y1) *y1 = b;
    return 0;
}

// ── PSRAM 分配（md/txt 解析要 PSRAM 块数组，heap_caps_malloc 不在白名单）──

// .so 里的 malloc/calloc/realloc/free 在白名单里，但只能落到内部 RAM（512KB），
// 读一本大书（块数组 + 正文缓冲）会爆。所以宿主导出 PSRAM 版。
extern "C" void* so_psram_alloc(size_t n)
{
    if (n == 0) return nullptr;
    return heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
}

extern "C" void so_psram_free(void* p)
{
    if (p) heap_caps_free(p);
}

// ── 文件 API（宿主侧，.so 不能碰文件系统）─────────────────────────

// 扫 /sdcard/fonts 下的 .ttf（只扫一层），返回个数。names[i] 是文件名。
extern "C" int so_list_fonts(char (*names)[80], int max)
{
    if (!names || max <= 0) return 0;
    m5ui::sd::FontFile ff[32];
    int n = m5ui::sd::scan_fonts(ff, max > 32 ? 32 : max);
    for (int i = 0; i < n && i < max; i++) {
        strncpy(names[i], ff[i].name, 79);
        names[i][79] = 0;
    }
    return n;
}

static bool _is_book(const char* n) {
    size_t l = strlen(n);
    if (l < 4) return false;
    const char* dot = strrchr(n, '.');
    if (!dot) return false;
    const char* e = dot + 1;
    size_t el = strlen(e);
    auto ieq = [&](const char* s) {
        if (strlen(s) != el) return false;
        for (size_t i = 0; i < el; i++) if ((e[i] | 0x20) != s[i]) return false;
        return true;
    };
    return ieq("md") || ieq("markdown") || ieq("txt");
}

// 扫 /sdcard/books 下的 .md/.markdown/.txt（只扫一层），返回个数。
extern "C" int so_list_books(char (*names)[256], int max)
{
    if (!names || max <= 0) return 0;
    if (!m5ui::sd::mount()) return 0;
    DIR* d = opendir("/sdcard/books");
    if (!d) return 0;
    int n = 0;
    struct dirent* e;
    while ((e = readdir(d)) != nullptr && n < max) {
        if (e->d_name[0] == '.' || !_is_book(e->d_name)) continue;
        strncpy(names[n], e->d_name, 255);
        names[n][255] = 0;
        n++;
    }
    closedir(d);
    return n;
}

// 读一本书进 buf（宿主拼路径 + fopen + fread），上限 cap 字节。返回字节数；<0 失败。
extern "C" int so_read_book(const char* name, char* buf, int cap)
{
    if (!name || !buf || cap <= 0) return -1;
    char path[320];
    snprintf(path, sizeof(path), "/sdcard/books/%s", name);
    FILE* f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return -1; }
    long want = sz > (long)cap ? (long)cap : sz;
    size_t got = fread(buf, 1, (size_t)want, f);
    fclose(f);
    return (int)got;
}

// 从 off 处续读一段（开书后台加载用）。返回读到字节数；0 = 已到文件尾；<0 = 失败。
// ★ 是宿主侧的 fseek+fread —— .so 白名单里没有任何文件 API，文件活儿本来就该固件做。
extern "C" int so_read_book_at(const char* name, char* buf, int off, int want)
{
    if (!name || !buf || off < 0 || want <= 0) return -1;
    char path[320];
    snprintf(path, sizeof(path), "/sdcard/books/%s", name);
    FILE* f = fopen(path, "rb");
    if (!f) return -1;
    if (fseek(f, (long)off, SEEK_SET) != 0) { fclose(f); return -1; }
    size_t got = fread(buf, 1, (size_t)want, f);
    fclose(f);
    return (int)got;
}

// 书的总字节数（分章扫描要报进度，也要判断 SD 上的索引缓存是否过期）
extern "C" int so_book_size(const char* name)
{
    if (!name) return -1;
    char path[320];
    snprintf(path, sizeof(path), "/sdcard/books/%s", name);
    FILE* f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fclose(f);
    return sz > 0 ? (int)sz : -1;
}

// ── /sdcard/apps/ 下的通用小文件存取（.so 没有文件 API，索引缓存靠这两个）──
// name 只允许**纯文件名**（不许含 '/' 或 ".."）→ 越不出 /sdcard/apps。
// 这是"能力"不是"业务"：谁存什么由 .so 决定（分章索引就存这儿）。
static bool appfile_path(const char* name, char* out, size_t cap)
{
    if (!name || !name[0]) return false;
    if (strchr(name, '/') || strstr(name, "..")) return false;
    snprintf(out, cap, "/sdcard/apps/%s", name);
    return true;
}

extern "C" int so_blob_load(const char* name, char* buf, int cap)
{
    if (!buf || cap <= 0) return -1;
    char path[320];
    if (!appfile_path(name, path, sizeof(path))) return -1;
    FILE* f = fopen(path, "rb");
    if (!f) return -1;
    size_t got = fread(buf, 1, (size_t)cap, f);
    fclose(f);
    return (int)got;
}

extern "C" int so_blob_save(const char* name, const char* buf, int len)
{
    if (!buf || len <= 0) return -1;
    char path[320];
    if (!appfile_path(name, path, sizeof(path))) return -1;
    FILE* f = fopen(path, "wb");
    if (!f) return -1;
    size_t put = fwrite(buf, 1, (size_t)len, f);
    fclose(f);
    return (int)put;
}

// ── 阅读器配置持久化（/sdcard/apps/reader.cfg）────────────────────
static const char* CFG_PATH = "/sdcard/apps/reader.cfg";

extern "C" int so_cfg_load(char* buf, int cap)
{
    if (!buf || cap <= 0) return -1;
    FILE* f = fopen(CFG_PATH, "rb");
    if (!f) return -1;
    size_t got = fread(buf, 1, (size_t)cap - 1, f);
    fclose(f);
    buf[got] = 0;
    return (int)got;
}

extern "C" int so_cfg_save(const char* buf, int len)
{
    if (!buf || len < 0) return -1;
    FILE* f = fopen(CFG_PATH, "wb");
    if (!f) return -1;
    size_t w = fwrite(buf, 1, (size_t)len, f);
    fclose(f);
    return (int)w;
}

// ── 纯色块 / 波形档 / 推屏 ────────────────────────────────────────
// ── 内置字体（flash 分区 "fonts"，mmap → 零 PSRAM、毫秒级）──
// .so 想要「一进 app 就能画字」不该先往 SD 传 3MB 字体再等 5 秒读进 PSRAM；
// 这份就是固件自己用的那份。句柄用法与 so_font_load 完全一致（同样 so_font_free 释放）。
// ⚠️ FontCtx 是**每个 FontCtx 一份**，mmap 的句柄是独立的；固件页面用的是 g_default，不受影响。
extern "C" void* so_font_builtin(float px)
{
    auto* c = (m5ui::ttf::FontCtx*)heap_caps_malloc(sizeof(m5ui::ttf::FontCtx), MALLOC_CAP_8BIT);
    if (!c) return nullptr;
    new (c) m5ui::ttf::FontCtx();
    if (!m5ui::ttf::load(*c, "fonts", px)) {
        m5ui::ttf::free_font(*c);
        heap_caps_free(c);
        return nullptr;
    }
    return c;
}

// ── 电池：宿主是唯一轮询点（.so 白名单里没有 M5Unified，拿不到 M5.Power）──
// 读 ADC 有耗时，所以宿主缓存一份，.so 只读缓存。节奏照用户 MicroPython 版
// BatteryIcon：充电状态 1s、电量 60s。惰性刷新（.so 来读时才检查间隔）——
// 效果等同「主循环一处 tick」，但不用往主循环里插钩子。
static int     g_bat_level = -1;   // 0..100，-1 = 读不到
static int     g_bat_chg   = 2;    // 0 放电 / 1 充电 / 2 未知（与 M5.Power 三态一致）
static int64_t g_bat_lvl_ms = 0, g_bat_chg_ms = 0;

// ── 系统占用率（给 .so 显示的活数据，不是假数）──
// CPU：用 FreeRTOS 的空闲任务运行时间计数器算 —— 需要 sdkconfig 里
//      CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS（+ 计数器用 esp_timer，µs 单位）。
//      每次调用给出「距上次调用这段时间里，空闲任务占了多少」→ 占用率 = 100 − 空闲%。
//      第一次调用只做采样、返回 -1（没有两个点算不出比例）。
extern "C" int so_cpu_load(void)
{
#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
    // ★ 双核：**不能只读调用核的空闲计数器** —— EPD 推屏任务跑在另一个核上，
    //   只算一个核会把占用率低报（实测：推屏时也只显示 10%）。
    //   这里用 uxTaskGetSystemState 把**所有任务**的 CPU 时间加起来，
    //   再除以「两个核在这段时间里提供的总 CPU 时间」→ 系统级占用率。
    const int n = (int)uxTaskGetNumberOfTasks();
    if (n <= 0) return -1;
    static TaskStatus_t* st = nullptr;
    if (st == nullptr) {
        st = (TaskStatus_t*)heap_caps_malloc(sizeof(TaskStatus_t) * (n + 4), MALLOC_CAP_8BIT);
        if (st == nullptr) return -1;
    }
    const int got = (int)uxTaskGetSystemState(st, n + 4, nullptr);
    if (got <= 0) return -1;

    uint64_t busy = 0;                       // 所有任务累计 CPU 时间（µs）
    for (int i = 0; i < got; ++i) {
        const char* nm = st[i].pcTaskName ? st[i].pcTaskName : "";
        // 空闲任务不计入 busy（它的名字以 IDLE 开头）
        if (nm[0] == 'I' && nm[1] == 'D' && nm[2] == 'L' && nm[3] == 'E') continue;
        busy += st[i].ulRunTimeCounter;
    }
    static uint64_t last_busy = 0;
    static int64_t  last_us   = 0;
    const int64_t now = esp_timer_get_time();
    if (last_us == 0) { last_busy = busy; last_us = now; return -1; }   // 第一次只采样
    const uint64_t db = busy - last_busy;
    const int64_t  dt = now - last_us;
    last_busy = busy; last_us = now;
    if (dt <= 0) return -1;
    const int64_t total = dt * 2;             // 双核：这段时间总共能提供 dt×2 的 CPU 时间
    int load = (int)((int64_t)db * 100 / total);
    if (load < 0) load = 0;
    if (load > 100) load = 100;
    return load;
#else
    return -1;   // 没开统计就不给假数
#endif
}

// 内部内存占用率（0..100，-1 = 读不到）：free / total，只算内部 RAM（PSRAM 不算进"核心"）
// 时钟：**时间源是板载 RTC，网络（SNTP）只做校准**（见 main/platform/rtc_time.h）。
// 返回 HH*100+MM；**-1 = 时间还不可信**（RTC 无有效值且还没联网校准）——
// 这时 .so 该显示占位符（如 --:--），不要自己造一个假时间上去。
extern "C" int so_clock_hhmm(void)
{
    return m5ui::rtctime::hhmm();
}

// 0/1：时间是否可信（.so 想区分“没校准”和“刚好 00:00”就查它）
extern "C" int so_clock_valid(void)
{
    return m5ui::rtctime::valid() ? 1 : 0;
}

extern "C" int so_mem_used_pct(void)
{
    // ★ 2026-10-15 用户要求：把 PSRAM 也算进来。
    //   MALLOC_CAP_DEFAULT = 默认分配器能看到的全部堆（内部 SRAM + PSRAM，
    //   前提是 CONFIG_SPIRAM_USE_MALLOC/SPIRAM 打开）—— 也就是"代码实际能申请到的总内存"。
    //   ⚠️ 两者性质差别很大：内部 SRAM 才 ~512KB、还要供 DMA/帧缓冲；PSRAM 8MB 但慢。
    //      合成一个百分比会掩盖"内部其实已经 89%"这种紧张状况。要分开看就各取一次：
    //        heap_caps_get_total_size(MALLOC_CAP_INTERNAL) / (MALLOC_CAP_SPIRAM)
    size_t total = heap_caps_get_total_size(MALLOC_CAP_DEFAULT);
    size_t freeb = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
    if (total == 0) {                       // 万一没开 PSRAM 的默认堆：退回只看内部
        total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
        freeb = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    }
    if (total == 0) return -1;
    int used = (int)(((total - freeb) * 100) / total);
    if (used < 0) used = 0;
    if (used > 100) used = 100;
    return used;
}

// 内存明细（KB）：给 .so 想分开显示时用。which: 0=内部SRAM 1=PSRAM
extern "C" int so_mem_size_kb(int which)
{
    const uint32_t cap = (which == 1) ? MALLOC_CAP_SPIRAM : MALLOC_CAP_INTERNAL;
    return (int)(heap_caps_get_total_size(cap) / 1024);
}
extern "C" int so_mem_free_kb(int which)
{
    const uint32_t cap = (which == 1) ? MALLOC_CAP_SPIRAM : MALLOC_CAP_INTERNAL;
    return (int)(heap_caps_get_free_size(cap) / 1024);
}

extern "C" int so_battery_level(void)
{
    const int64_t now = esp_timer_get_time() / 1000;
    if (g_bat_lvl_ms == 0 || now - g_bat_lvl_ms >= 60000) {
        const int32_t lv = M5.Power.getBatteryLevel();
        g_bat_level  = (lv < 0 || lv > 100) ? -1 : (int)lv;
        g_bat_lvl_ms = now;
    }
    return g_bat_level;
}

extern "C" int so_is_charging(void)
{
    const int64_t now = esp_timer_get_time() / 1000;
    if (g_bat_chg_ms == 0 || now - g_bat_chg_ms >= 1000) {
        g_bat_chg    = (int)M5.Power.isCharging();
        g_bat_chg_ms = now;
    }
    return g_bat_chg;
}

extern "C" int so_fill_rect(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color)
{
    if (w <= 0 || h <= 0) return -1;
    dirty_add(x, y, w, h);                 // ★ 批处理：记脏区
    M5.Display.fillRect(x, y, w, h, color);
    return 0;
}

// 1=QUALITY 2=TEXT 3=FAST 4=FASTEST（语义档，不是最终波形 —— 见下）
extern "C" int so_set_epd_mode(int mode)
{
    if (mode < 1 || mode > 4) return -1;
    // ★ 必须走 render.h 的 apply_epd_mode()，**不能**在这里直接 M5.Display.setEpdMode()。
    //   直连会绕开全局档位强制：设置页选了 gc13 也白选，.so 的推屏永远按自己的 mode 走。
    //   实测症状（2026-10-15）：阅读器书目列表整屏推一次 **1447ms** —— 正好等于
    //   epd_text/GL16 的 36 帧（1448ms）；而设置页明明选的是 gc13（831ms）。
    //   走 apply_epd_mode 之后：默认配置下"文字"语义统一落到 gc13，
    //   全局档（wave_def=0）也真的能强制住 .so 的推屏。
    m5ui::systems::apply_epd_mode((uint8_t)mode);
    return 0;
}

extern "C" int so_display(int32_t x, int32_t y, int32_t w, int32_t h)
{
    if (w <= 0 || h <= 0) return -1;
    M5.Display.display(x, y, w, h);
    return 0;
}

// 帧缓冲整页快照：把 _buf 一块读进 .so 的 8bit 灰度缓冲（1 字节/像素）。
// 8bit grayscale 是 M5GFX 公开读接口的格式（_buf 内部 4bit 是私有细节，别碰）。
// .so 用 so_psram_alloc 分一块 w*h 字节的缓冲，画完一页调本函数存下来，翻页时再 restore。
extern "C" int so_fb_save(uint8_t* dst, int32_t x, int32_t y, int32_t w, int32_t h)
{
    if (!dst || w <= 0 || h <= 0) return -1;
    M5.Display.readRect(x, y, w, h, (lgfx::grayscale_t*)dst);
    return 0;
}

// 帧缓冲整页恢复：把 .so 缓冲里的 8bit 灰度写回 _buf（配 so_display 推屏）。
extern "C" int so_fb_restore(const uint8_t* src, int32_t x, int32_t y, int32_t w, int32_t h)
{
    if (!src || w <= 0 || h <= 0) return -1;
    M5.Display.pushImage(x, y, w, h, (const lgfx::grayscale_t*)src);
    return 0;
}

// ── ★ 4bpp 精确快照（页缓存专用）──────────────────────────────────────
// 为什么不能用上面那对 readRect/pushImage：面板的写路径是 RGB565→8bit→4bit 带
// 4×4 位置相关抖动，往返之后**只有灰阶 0/8/15 能原样回来**，1-7 和 9-14 各裂成
// 相邻两级 → 重推出来是"淡淡的麻点"，页缓存会越看越花。
// 这一对直接搬 _buf 里的 nibble，逐位精确（m5gfx 的 lab hook，见 Panel_EPD.hpp）。
// 已核对实现：目标缓冲按**区域相对下标**打包（i&1），不是绝对 x ——
// 所以 x0 是奇数也不会错位，任意子区域都能用。
//
// ★ 调用方注意：copy_region_in 直接写 _buf，**不更新 display() 的脏矩形**，
//   所以恢复之后必须紧跟一次覆盖该区域的 so_display() 才会推上屏。
// ── 页快照自检（第一次用到时跑一次）──────────────────────────────────
// 全屏 out 原版 vs 快版，逐字节比对；一致才把导出指到快版。
//
// ★ 两个缓冲必须先 memset 0：copy_region_out 对"越界的目标像素"是**跳过不写**，
//   不置零就会拿两块不同的垃圾字节去比，误报不一致。
// ★ in 的快版会把 _buf 写花（如果有 bug）→ 紧接着用**原版 in** 把原内容写回，
//   所以自检失败也不会把屏幕缓冲留在坏状态。
static bool g_fb_selftest_done = false;
static bool g_fb_fast_ok       = false;

// 搬运代码的**版本号**。改了 copy_region_*_fast 里任何一行都要 +1 ——
// 否则 NVS 里的旧结论会被沿用，自检等于白做。
static constexpr uint8_t FB_CHK_VS = 1;
static const char*       FB_CHK_NS = "fbchk";

static void fb_selftest();     // 见下（完整自检）

static void fb_selftest()
{
    if (g_fb_selftest_done) return;
    g_fb_selftest_done = true;

    auto* p = static_cast<lgfx::Panel_EPD*>(M5.Display.panel());
    if (!p) { ESP_LOGW("PLUGIN", "页快照自检：panel 不是 Panel_EPD → 用原版"); return; }

    const int32_t W = 540, H = 960;
    const size_t  n = lgfx::Panel_EPD::region_bytes(W, H);
    uint8_t* a = (uint8_t*)heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    uint8_t* b = (uint8_t*)heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    uint8_t* c = (uint8_t*)heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    if (!a || !b || !c) {
        ESP_LOGW("PLUGIN", "页快照自检：PSRAM 不够（要 3×%uKB）→ 用原版", (unsigned)(n / 1024));
        if (a) heap_caps_free(a);
        if (b) heap_caps_free(b);
        if (c) heap_caps_free(c);
        return;
    }

    struct Reg { int32_t x, y, w, h; const char* why; };
    // 4 个区域：全屏是页缓存实际用的；其余覆盖子区域与**越界块**（走逐像素兜底那条路）
    static const Reg regs[] = {
        {   0,   0, 540, 960, "全屏" },
        {  20, 100, 300, 200, "子区域" },
        { -50, -30, 400, 400, "越左上边界" },
        { 500, 900, 200, 200, "越右下边界" },
    };

    const int64_t t0 = esp_timer_get_time();
    bool out_ok = true, in_ok = true;
    int64_t t_orig_full = 0, t_fast_full = 0;

    // ① out：每个区域原版 vs 快版逐字节比对（顺便给全屏计时）
    for (const Reg& rg : regs) {
        memset(a, 0, n); memset(b, 0, n);
        const int64_t q0 = esp_timer_get_time();
        const bool o1 = p->copy_region_out     (rg.x, rg.y, rg.w, rg.h, a);
        const int64_t q1 = esp_timer_get_time();
        const bool o2 = p->copy_region_out_fast(rg.x, rg.y, rg.w, rg.h, b);
        const int64_t q2 = esp_timer_get_time();
        if (rg.w == 540 && rg.h == 960) { t_orig_full = (q1 - q0) / 1000; t_fast_full = (q2 - q1) / 1000; }
        const size_t nb = lgfx::Panel_EPD::region_bytes(rg.w, rg.h);
        if (!o1 || !o2 || memcmp(a, b, nb) != 0) {
            out_ok = false;
            ESP_LOGW("PLUGIN", "页快照自检：out 不一致（%s x=%d y=%d w=%d h=%d）",
                     rg.why, (int)rg.x, (int)rg.y, (int)rg.w, (int)rg.h);
        }
    }

    // ★ 计时：全屏 out 原版 vs 快版。这行能在翻页之前就告诉我们是快了还是白干。
    ESP_LOGI("PLUGIN", "页快照计时：全屏 out 原版 %lldms · 快版 %lldms（%s）",
             (long long)t_orig_full, (long long)t_fast_full,
             t_fast_full > 0 ? (t_fast_full < t_orig_full ? "快版更快" : "快版没赚到") : "计时无效");

    // ② in：把各区域的**原内容写回去**（内容相同 ⇒ 应当是无操作），再用原版 out 全屏比对。
    //    ★ 每个区域测完都用**原版 in** 从 backup 恢复现场，所以失败也不会留坏状态。
    if (out_ok) {
        memset(c, 0, n);
        p->copy_region_out(0, 0, W, H, c);              // backup（c 常驻）
        for (const Reg& rg : regs) {
            const size_t nb = lgfx::Panel_EPD::region_bytes(rg.w, rg.h);
            memset(a, 0, nb);
            p->copy_region_out(rg.x, rg.y, rg.w, rg.h, a);
            p->copy_region_in_fast(rg.x, rg.y, rg.w, rg.h, a);   // 原样写回
            memset(b, 0, n);
            p->copy_region_out(0, 0, W, H, b);
            if (memcmp(b, c, n) != 0) {
                in_ok = false;
                ESP_LOGW("PLUGIN", "页快照自检：in 不一致（%s x=%d y=%d w=%d h=%d）",
                         rg.why, (int)rg.x, (int)rg.y, (int)rg.w, (int)rg.h);
            }
            p->copy_region_in(0, 0, W, H, c);           // ★ 无条件恢复现场（可信的原版）
        }
    }

    g_fb_fast_ok = out_ok && in_ok;
    ESP_LOGI("PLUGIN", "页快照自检（%lldms）：out 4 区域 %s · in 4 区域 %s → %s",
             (long long)((esp_timer_get_time() - t0) / 1000),
             out_ok ? "一致" : "※不一致※", in_ok ? "一致" : "※不一致※",
             g_fb_fast_ok ? "启用快版" : "退回原版");

    heap_caps_free(a); heap_caps_free(b); heap_caps_free(c);
}

// ── 开机判定入口 ──────────────────────────────────────────────────────
// 结论存 NVS ⇒ **自检只在第一次开机（或 FB_CHK_VS 变了）跑一次**，
// 之后每次开机直接读结论 —— 不会每次开机都卡 2.7 秒。
void fb_check_init()
{
    if (g_fb_selftest_done) return;
    m5ui::cfg::init();                 // nvs_flash_init，幂等（fb_check_init 在 namespace 之外）

    nvs_handle_t h;
    if (nvs_open(FB_CHK_NS, NVS_READONLY, &h) == ESP_OK) {
        uint8_t vs = 0, res = 0;
        const bool has = (nvs_get_u8(h, "vs", &vs) == ESP_OK) &&
                         (nvs_get_u8(h, "res", &res) == ESP_OK);
        nvs_close(h);
        if (has && vs == FB_CHK_VS && (res == 1 || res == 2)) {
            g_fb_fast_ok       = (res == 1);
            g_fb_selftest_done = true;
            ESP_LOGI("PLUGIN", "页快照快版：沿用 NVS 结论（搬运代码版本 %u）→ %s（不重跑自检）",
                     (unsigned)vs, g_fb_fast_ok ? "快版" : "原版");
            return;
        }
    }

    fb_selftest();                     // 没有结论（或版本变了）→ 跑一次

    if (nvs_open(FB_CHK_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "vs",  FB_CHK_VS);
        nvs_set_u8(h, "res", g_fb_fast_ok ? 1 : 2);
        nvs_commit(h);
        nvs_close(h);
        ESP_LOGI("PLUGIN", "页快照快版：结论已存 NVS（版本 %u → %s），以后开机不再自检",
                 (unsigned)FB_CHK_VS, g_fb_fast_ok ? "快版" : "原版");
    } else {
        ESP_LOGW("PLUGIN", "页快照快版：NVS 打不开 → 结论不保存（每次开机都会自检）");
    }
}

extern "C" int so_fb_save4(uint8_t* dst, int32_t x, int32_t y, int32_t w, int32_t h)
{
    if (!dst || w <= 0 || h <= 0) return -1;
    auto* p = static_cast<lgfx::Panel_EPD*>(M5.Display.panel());
    if (!p) return -1;
    fb_check_init();
    const bool ok = g_fb_fast_ok ? p->copy_region_out_fast(x, y, w, h, dst)
                                 : p->copy_region_out(x, y, w, h, dst);
    return ok ? 0 : -1;
}

extern "C" int so_fb_restore4(const uint8_t* src, int32_t x, int32_t y, int32_t w, int32_t h)
{
    if (!src || w <= 0 || h <= 0) return -1;
    auto* p = static_cast<lgfx::Panel_EPD*>(M5.Display.panel());
    if (!p) return -1;
    fb_check_init();
    const bool ok = g_fb_fast_ok ? p->copy_region_in_fast(x, y, w, h, src)
                                 : p->copy_region_in(x, y, w, h, src);
    return ok ? 0 : -1;
}

// 一块 w×h 的 4bpp 区域要多少字节（.so 分配缓冲前问这个，别自己算）
extern "C" int so_fb_bytes(int32_t w, int32_t h)
{
    if (w <= 0 || h <= 0) return -1;
    return (int)lgfx::Panel_EPD::region_bytes(w, h);
}

// ── 推屏期间漏掉的触摸：锁存 + 补报 ──────────────────────────────────
// 实测问题：等波形时主循环整个卡住 ~900ms（`M5.Display.waitDisplay()` 是忙等），
// 这期间用户"抬手 → 再按下"的过程里**抬手没被采样到** → .so 里的 `down` 一直是 true
// → 第二次按下的按下沿不触发 → 那一整下被**吞掉**（用户原话"感觉丢了"）。
// 做法：推屏期间照样刷触摸；只要出现过触点就锁存一次"按下"；之后 so_touch 先补一次
//       "抬起"、再补一次"按下"，让 .so 的按下沿判定正常触发一次。
static volatile bool    g_tap_latch = false;
static volatile bool    g_tap_sent0 = false;
static volatile int32_t g_tap_x = 0, g_tap_y = 0;

// ★ 锁存的正确语义：只锁"等待期间**新发生**的那一下"（先见到抬起、再见到按下）。
//   原来的实现是"见到任何触点就锁"✗ —— 包括**正在被处理的那只手指**
//   （用户点完按钮、手指还没抬起来，.so 就已经进等待循环了）
//   → 每次推屏都会把这只老手指再"补报"一次 → 幻触源源不断 → 表现就是"乱跑"。
//   （只有"等的时候手指是抬着的，等待中途又按了一下"才是真正需要补报的那种。）
static bool g_wait_armed = false;

extern "C" int so_wait_display(void)
{
    // ★ 不用 M5.Display.waitDisplay()：它是忙等，全程不采样触摸（上面的根因）。
    // ★ 也不能用 M5.Display.displayBusy() 代替 —— 那查的是「更新队列满没满」，
    //   display() 一返回就是空的 → 循环秒退，等于没等（实测过，推屏耗时变成假的 2ms）。
    //   真正要等的是 panel 的 _display_busy（Panel_EPD::waitDisplay 用的同一个条件），
    //   所以给它加了个公开访问器 isDrawing()。
    auto* p = static_cast<lgfx::Panel_EPD*>(M5.Display.panel());
    if (!p) { M5.Display.waitDisplay(); return 0; }

    g_wait_armed = false;   // 进等待：先忘掉"现在是不是按着"，只认之后新按下的
    while (p->isDrawing()) {
        // ★ 这里可以用 update()：此刻主循环正卡在这个 wait 里，**没有别的读者**，
        //   而且 update() 喂的是 Touch_Class 的共享缓存（4ms 节流），不会丢数据。
        //   ⚠️ 但绝不能在**任务**里这么做（两个读者会互相抢 GT911 的点数据 —— 实测过，
        //   主页按不动 + 三分区收不到触摸）。
        M5.Touch.update(m5gfx::millis());
        const int n = M5.Touch.getCount();
        if (n == 0) {
            g_wait_armed = true;                        // 见到抬起 → 下一次按下才算"新的一下"
        } else if (g_wait_armed && !g_tap_latch) {
            auto& d = M5.Touch.getDetail(0);            // 只有"抬起之后又按下"才锁存
            g_tap_x = d.x; g_tap_y = d.y;
            g_tap_latch = true; g_tap_sent0 = false;
            g_wait_armed = false;
        }
        M5.delay(1);
    }
    return 0;
}

// ★ 给 .so 的"只看有没有手指"接口：**不消耗**锁存的那次补报，也不动按下沿状态。
//   背景：.so 里有两处会轮询触摸 —— page_tick（做按下沿判定，应该由它消费）
//   和预取绘制里的"摸到手指就收手"。后者如果也调 so_touch()，就会把给 page_tick
//   准备的 [抬起 → 按下] 补报序列吃掉 → 用户看到的就是
//   "点击的坐标在不该消费的时候被消费了"（坐标被错的地方用掉）。
extern "C" int so_touch_down(void)
{
    M5.Touch.update(m5gfx::millis());
    return M5.Touch.getCount() > 0 ? 1 : 0;
}

// 读触摸（.so 的 page_tick 里轮询）。返回 1=有触点（*x/*y 填坐标），0=无。
// ★ 只有 page_tick 该调这个 —— 它带"补报"语义，调一次就消耗一格。
extern "C" int so_touch(int32_t* x, int32_t* y)
{
    // ★ 必须**自己刷新**触摸状态再读，不能用主循环那份快照。
    //   主循环是：`app.update()` 里 `M5.update()`（刷新触摸）→ 然后 `plugin::tick()`
    //   里可能一口气跑几百 ms（预取一整页 ~600ms、推屏 ~900ms）。这期间触摸状态是
    //   **陈旧的** —— 手指已经按下了也看不见，要等下一轮 `M5.update()` 才认。
    //   症状：用户说"得等一会儿才响应""要按好几下才成功"。
    //   实测代价：预取里那条"摸到手指就收手"的检查因此**一次都没触发过**
    //   （它读的就是这份陈旧状态，日志里 `预取让位` 一直是 0 次）。
    //   一次 I2C 读约 1ms，so_touch 一轮也就调一两次，代价可以忽略。
    // ★ 刷新共享缓存 + 读缓存（**不是**读原始点）：主循环每轮也调 M5.update()，
    //   两边喂的是同一个 Touch_Class 缓存 → 谁先读都不丢数据。
    //   ⚠️ 绕过缓存去读原始点会互相抢（GT911 的点"读一次就被消费"）——
    //   实测后果：固件页面按不动 + 阅读页三分区有时收不到触摸。
    M5.Touch.update(m5gfx::millis());

    if (M5.Touch.getCount()) {                 // 手指真按着
        if (g_tap_latch) {
            // 有漏掉的一下待补：先让 .so 看到"抬起"，它才会重新认"按下"
            if (!g_tap_sent0) { g_tap_sent0 = true; return 0; }
            g_tap_latch = false; g_tap_sent0 = false;
        }
        auto& d = M5.Touch.getDetail(0);
        if (x) *x = d.x;
        if (y) *y = d.y;
        return 1;
    }

    if (g_tap_latch) {                         // 手指已抬 → 把推屏期间漏掉的那一下补报出来
        if (!g_tap_sent0) { g_tap_sent0 = true; return 0; }   // 先补"抬起"
        g_tap_latch = false; g_tap_sent0 = false;             // 再补"按下"
        if (x) *x = g_tap_x;
        if (y) *y = g_tap_y;
        return 1;
    }
    return 0;
}

#if 0   // ★ 已撤销（原因见 main.cpp 里那段注释：GT911 的点"读一次就被消费"，
        //   第二个读者会抢掉主循环/so_touch 的数据 → 主页按不动 + 三分区收不到触摸）
// ── ★ 触摸采样任务（core 1 常驻）────────────────────────────────────
// 为什么需要：主循环被 .so 的长操作占住时，触摸**没人看** —— 真实翻页的光栅化
// (283~383ms)、页快照恢复(105ms)、存快照(74ms)、整页重画(280~380ms)…… 这些路径
// 都不查触摸，用户在这几百毫秒里"抬手 → 再按下"，那一整下**永远不会被看见**
// → 表现就是"按了没反应 / 要点好几下"。反复在"哪段加个检查"上打补丁是补不完的。
// 对照：固件自家页面（设置等）几乎从不错过按下 —— 因为它从不长阻塞、每帧都刷触摸、
// 而且只推一小块脏区。阅读页的结构改不掉（整页重画 + 等整屏波形），所以改成**分核采样**：
// 不管主循环在干什么，触摸都有人在看 → 点一下算一下。
// 采样本身很便宜：Touch.update() 内部有 4ms 节流，且 GT911 的 INT 引脚不低时连 I2C 都不发。
// ★ 并发安全：Touch_Class 里加了互斥（见 components/m5unified/.../Touch_Class.cpp）。
// ★ 放在 namespace 里（plugin.h 的声明在 m5ui::plugin 内）——
//   plugin.cpp 的 `namespace m5ui::plugin {` 在第 926 行，这里是**它的上面**，
//   不自己包一层就会变成全局符号 → 链接期报 undefined reference。
namespace m5ui::plugin {

static void touch_sample_task(void*)
{
    for (;;) {
        // ★ 只读原始点（绝不能调 M5.Touch.update()：会吃掉固件页面的 touch_begin 状态）
        m5gfx::touch_point_t tp[4];
        if (M5.Touch.readRawOnly(tp, 4) > 0 && !g_tap_latch) {
            g_tap_x = tp[0].x; g_tap_y = tp[0].y;
            g_tap_latch = true; g_tap_sent0 = false;
        }
        vTaskDelay(1);          // 10ms 一采样（tick=100Hz）；人手一次点击 50~150ms → 采到 5~15 次，必不丢
    }
}

void touch_task_start()
{
    static bool started = false;
    if (started) return;
    started = true;
    // core 1（IDF 的 main task / 主循环在 core 0）；优先级 2：高于 main(1)，不抢 EPD 驱动任务
    xTaskCreatePinnedToCore(touch_sample_task, "tch", 3072, nullptr, 2, nullptr, 1);
}

}   // namespace m5ui::plugin
#endif

// ★ 批处理（用户 2026-10-15 建议：用 startWrite/endWrite 包一批次）─────────────
//   Panel_EPD 自己会累计"上次推屏之后写过的像素"（_range_mod，Panel_EPD.cpp:914），
//   所以"包一批"的正确做法就是：begin → 随便画（写帧缓冲）→ end（此时一次性推脏区）。
//   ⚠️ 这里**不调** M5GFX 的 startWrite()/endWrite()：那是给屏总线准备的，
//      而这个面板的写操作走的是帧缓冲（不是总线），调用它反而触到 SPI 事务、
//      有可能干扰推屏任务。真正起作用的是本文件自己的脏区累计（见 dirty_add）。
extern "C" void so_begin_write(void)
{
    dirty_reset();
    g_write_open = true;
}

extern "C" int so_end_write(void)
{
    if (!g_write_open) return 0;
    g_write_open = false;
    if (g_dx1 <= g_dx0) return 0;                       // 这一批什么都没写 → 一次都不推
    if (g_dx0 < 0)   g_dx0 = 0;                         // 夹到屏内
    if (g_dy0 < 0)   g_dy0 = 0;
    if (g_dx1 > 540) g_dx1 = 540;
    if (g_dy1 > 960) g_dy1 = 960;
    M5.Display.display(g_dx0, g_dy0, g_dx1 - g_dx0, g_dy1 - g_dy0);
    so_wait_display();
    return 1;
}

// 计时 / 打日志（.so 自测各阶段耗时用）。.so 里没有 esp_timer、也没有 ESP_LOGx。
extern "C" int64_t so_now_ms(void) { return esp_timer_get_time() / 1000; }

extern "C" void so_log(const char* msg) { ESP_LOGI("SO", "%s", msg ? msg : ""); }

// 追加一行到 /sdcard/apps/reader.log（msg=nullptr → 清空重写）。
// 有它就不必掐串口时间点：点完随时从网页 /apps/reader.log 取结果。
extern "C" void so_log_file(const char* msg)
{
    FILE* f = fopen("/sdcard/apps/reader.log", msg ? "a" : "w");
    if (!f) return;
    if (msg) fprintf(f, "%s\n", msg);
    fclose(f);
}

// 退出 app：停掉每帧回调 + 回 launcher
//（launcher 打开 app 时摘掉了自己的 Visible，这里恢复）。
extern "C" void so_exit(void)
{
    m5ui::plugin::close();
    m5ui::pages::show(m5ui::pages::g_launcher);
}

// ══════════════════════════════════════════════════════════════════════════
// 固件设置读写 —— 给设置 app（apps/settings）用
// ══════════════════════════════════════════════════════════════════════════
// 键名与 NVS "m5ui" 里的键一致：
//   整数 size ink bw idle ap web wifi wave_def wave_sel ss dp snd snd_vol
//   字符串 font wifi_ssid wifi_pass scr_img
//
// ★ 分工（非常重要）：
//   so_fwcfg_set_*  **只改内存**，不写 flash（NVS 写会磨损分区）；
//   改完一批调一次 so_fwcfg_save() 落盘，再调 so_fwcfg_apply() 让改动当场生效。
//   size / ink / bw / wave_def 在 render.h 里**每次绘制现读** → 改完自然生效；
//   font（要重载字体）和 wave_sel（要装 LUT 表）必须靠 so_fwcfg_apply()。
//
// ★ ink（墨色档）已按用户 2026-10-15 的决定废弃 —— 不再出现在设置 UI 里。
//   读写能力保留，只为兼容 NVS 里的旧值（别让旧值变成读不出来的孤儿项）。
int so_fwcfg_get_int(const char* key, int def) {
    if (!key) return def;
    auto& c = m5ui::cfg::g_cfg;
    if (!strcmp(key, "size"))     return c.size;
    if (!strcmp(key, "ink"))      return c.ink;
    if (!strcmp(key, "bw"))       return c.bw;
    if (!strcmp(key, "idle"))     return c.idle;
    if (!strcmp(key, "ap"))       return c.ap;
    if (!strcmp(key, "web"))      return c.web;
    if (!strcmp(key, "wifi"))     return c.wifi;
    if (!strcmp(key, "wave_def")) return c.wave_def;
    if (!strcmp(key, "wave_sel")) return c.wave_sel;
    if (!strcmp(key, "ss"))       return c.ss;
    if (!strcmp(key, "dp"))       return c.dp;
    if (!strcmp(key, "snd"))      return c.snd;          // 触摸反馈音开关
    if (!strcmp(key, "snd_vol"))  return c.snd_vol;      // 音量 0~100
    return def;
}

// 范围兜底与 nvs.h::load() 完全一致 —— 越界值一律夹回去，绝不写进 NVS
int so_fwcfg_set_int(const char* key, int v) {
    if (!key) return 0;
    auto& c = m5ui::cfg::g_cfg;
    if (!strcmp(key, "size"))     { if (v < 16) v = 16; if (v > 48) v = 48; c.size = (uint16_t)v; return v; }
    if (!strcmp(key, "ink"))      { if (v < 0) v = 0; if (v > 255) v = 255; c.ink = (uint8_t)v;  return v; }
    if (!strcmp(key, "bw"))       { if (v < 1) v = 1; if (v > 8) v = 8;     c.bw  = (uint8_t)v;  return v; }
    if (!strcmp(key, "idle"))     { if (v < 1) v = 1; if (v > 120) v = 120; c.idle = (uint16_t)v; return v; }
    if (!strcmp(key, "ap"))       { c.ap   = v ? 1 : 0; return c.ap; }
    if (!strcmp(key, "web"))      { c.web  = v ? 1 : 0; return c.web; }
    if (!strcmp(key, "wifi"))     { c.wifi = v ? 1 : 0; return c.wifi; }
    if (!strcmp(key, "wave_def")) { c.wave_def = v ? 1 : 0; return c.wave_def; }
    if (!strcmp(key, "wave_sel")) { if (v < 1) v = 1; if (v > 5) v = 5; c.wave_sel = (uint8_t)v;  return v; }
    if (!strcmp(key, "ss"))       { c.ss = v ? 1 : 0; return c.ss; }
    if (!strcmp(key, "dp"))       { c.dp = v ? 1 : 0; return c.dp; }
    if (!strcmp(key, "snd"))      { c.snd = v ? 1 : 0; return c.snd; }
    if (!strcmp(key, "snd_vol"))  { if (v < 0) v = 0; if (v > 100) v = 100; c.snd_vol = (uint8_t)v; return v; }
    return 0;
}

static char* cfg_str_slot(const char* key, size_t* cap) {
    auto& c = m5ui::cfg::g_cfg;
    if (!strcmp(key, "font"))     { *cap = sizeof(c.font);      return c.font; }
    if (!strcmp(key, "wifi_ssid")){ *cap = sizeof(c.wifi_ssid); return c.wifi_ssid; }
    if (!strcmp(key, "wifi_pass")){ *cap = sizeof(c.wifi_pass); return c.wifi_pass; }
    if (!strcmp(key, "scr_img"))  { *cap = sizeof(c.scr_img);   return c.scr_img; }
    if (!strcmp(key, "snd_file")) { *cap = sizeof(c.snd_file);  return c.snd_file; }
    *cap = 0; return nullptr;
}

int so_fwcfg_get_str(const char* key, char* out, int cap) {
    if (!key || !out || cap <= 0) return -1;
    size_t n = 0;
    char* slot = cfg_str_slot(key, &n);
    if (!slot) { out[0] = 0; return -1; }
    // ★ 只拷"到 NUL 为止"，且不超过调用方缓冲：
    //   原来 cap 小于字段容量就直接拒写（拿到空串），后改成无条件拷 cap-1 字节
    //   （会读到字段尾后面的内存，也可能拷进 NUL 之后的垃圾）。两种都不对。
    size_t len = strlen(slot);
    if (len > (size_t)cap - 1) len = (size_t)cap - 1;
    memcpy(out, slot, len);
    out[len] = 0;
    return (int)len;
}

int so_fwcfg_set_str(const char* key, const char* v) {
    if (!key) return -1;
    size_t n = 0;
    char* slot = cfg_str_slot(key, &n);
    if (!slot) return -1;
    snprintf(slot, n, "%s", v ? v : "");
    return (int)strlen(slot);
}

// 一次落盘（多项改完调一次 —— 别一项一个 commit）
int so_fwcfg_save(void) { return m5ui::cfg::save() ? 0 : -1; }

// 让改动当场生效：重载字体 + 按需装波形表。
//   （size/ink/bw/wave_def 是每帧现读，本来就不用 apply）
void so_fwcfg_apply(void) {
    auto& c = m5ui::cfg::g_cfg;
    m5ui::ttf::set_size((float)c.size);
    if (m5ui::cfg::g_cfg.font[0]) {
        char p[200];
        snprintf(p, sizeof(p), "/sdcard/%.180s", c.font);
        if (!m5ui::ttf::load_from_sd(p, (float)c.size))
            ESP_LOGW("SET", "字体 %s 加载失败（保留原字体）", p);
    } else {
        m5ui::ttf::load("fonts", (float)c.size);
    }
    if (!c.wave_def && c.wave_sel != m5ui::epd::TIER_GC13)
        m5ui::epd::apply_lut(c.wave_sel);
    m5ui::sound::reload();     // 反馈音：音量 + 音效文件跟着设置走
}

// 恢复出厂：全部回默认值 → 落盘 → 复位（复位由 app 自己提示，这里只做复位）
int so_fwcfg_reset(void) {
    m5ui::cfg::g_cfg = m5ui::cfg::Config{};
    m5ui::cfg::save();
    ESP_LOGW("SET", "恢复出厂设置，即将复位");
    return 0;
}

// ══════════════════════════════════════════════════════════════════════════
// 网络：状态 + 开关（给设置 app）
// ══════════════════════════════════════════════════════════════════════════
int so_net_state(void)    { return m5ui::wifi_sta::g_state; }   // 0 空闲 1 连接中 2 已连 3 失败
int so_net_ap_on(void)    { return m5ui::wifi_ap::g_running && m5ui::wifi_ap::g_want_ap ? 1 : 0; }
int so_net_web_on(void)   { return m5ui::web_files::running() ? 1 : 0; }
int so_net_auto(void)     { return m5ui::cfg::g_cfg.wifi ? 1 : 0; }

// 设备热点开关（会真的拉起/放下 WiFi 驱动）
int so_net_set_ap(int on) {
    if (!m5ui::wifi_ap::set_ap(on != 0)) return -1;
    m5ui::cfg::g_cfg.ap = on ? 1 : 0;
    return so_net_ap_on();
}

// 网页文件服务开关
int so_net_set_web(int on) {
    if (on) m5ui::web_files::start();
    else    m5ui::web_files::stop();
    m5ui::cfg::g_cfg.web = on ? 1 : 0;
    return so_net_web_on();
}

int so_net_ip(char* out, int cap) {
    if (!out || cap < 4) return -1;
    const char* p = (m5ui::wifi_sta::g_state == 2) ? m5ui::wifi_sta::g_ip : "";
    snprintf(out, cap, "%s", p);
    return (int)strlen(out);
}

int so_net_ssid(char* out, int cap) {
    if (!out || cap < 4) return -1;
    snprintf(out, cap, "%s", m5ui::wifi_sta::g_ssid);
    return (int)strlen(out);
}

// 用配置里存的 SSID/口令连一次（没有存的就当失败）
int so_net_connect(void) {
    if (!m5ui::cfg::g_cfg.wifi_ssid[0]) return -1;
    return m5ui::wifi_sta::connect(m5ui::cfg::g_cfg.wifi_ssid, m5ui::cfg::g_cfg.wifi_pass) ? 0 : -1;
}
void so_net_disconnect(void) { m5ui::wifi_sta::disconnect(); }

// 忘掉已存的热点（清 SSID/口令并落盘）
void so_net_forget(void) {
    m5ui::cfg::g_cfg.wifi_ssid[0] = 0;
    m5ui::cfg::g_cfg.wifi_pass[0] = 0;
    m5ui::cfg::save();
    m5ui::wifi_sta::disconnect();
}

// ── 多热点：扫描 / 已存列表 / 连接 / 忘掉 ──
int so_wifi_scan(char* names, int cap, int max, int8_t* rssi) {
    if (!names || cap < 2 || max <= 0) return 0;
    char tmp[16][33];
    int8_t rt[16];
    const int n = m5ui::wifi_sta::scan(tmp, rt, max < 16 ? max : 16);
    for (int i = 0; i < n; ++i) {
        snprintf(names + i * cap, cap, "%s", tmp[i]);
        if (rssi) rssi[i] = rt[i];
    }
    return n;
}
int so_wifi_saved(char* names, int cap, int max) {
    if (!names || cap < 2 || max <= 0) return 0;
    m5ui::wifi_list::load();
    int n = 0;
    for (int i = 0; i < m5ui::wifi_list::MAX_ENTRIES && n < max; ++i) {
        if (m5ui::wifi_list::g_entries[i].ssid[0]) {
            snprintf(names + n * cap, cap, "%s", m5ui::wifi_list::g_entries[i].ssid);
            ++n;
        }
    }
    return n;
}
int so_wifi_connect(const char* ssid, const char* pass) {
    if (!ssid || !ssid[0]) return -1;
    m5ui::wifi_list::load();
    char p[65] = "";
    const int idx = m5ui::wifi_list::find(ssid);
    if (idx >= 0) {
        snprintf(p, sizeof(p), "%s", m5ui::wifi_list::g_entries[idx].pass);
    } else {
        // 新热点：存下（口令可空 = 开放网络）
        m5ui::wifi_list::add(ssid, pass);
        m5ui::wifi_list::save();
        snprintf(p, sizeof(p), "%s", pass ? pass : "");
    }
    // 同步「上次连的」主字段（开机自动连用）
    snprintf(m5ui::cfg::g_cfg.wifi_ssid, sizeof(m5ui::cfg::g_cfg.wifi_ssid), "%s", ssid);
    snprintf(m5ui::cfg::g_cfg.wifi_pass, sizeof(m5ui::cfg::g_cfg.wifi_pass), "%s", p);
    m5ui::cfg::save();
    return m5ui::wifi_sta::connect(ssid, p) ? 0 : -1;
}
int so_wifi_forget(const char* ssid) {
    if (!ssid) return -1;
    m5ui::wifi_list::load();
    m5ui::wifi_list::remove(ssid);
    m5ui::wifi_list::save();
    if (strcmp(m5ui::cfg::g_cfg.wifi_ssid, ssid) == 0) {
        m5ui::cfg::g_cfg.wifi_ssid[0] = 0;
        m5ui::cfg::g_cfg.wifi_pass[0] = 0;
        m5ui::cfg::save();
    }
    return 0;
}

// 打开固件输入法覆盖层（.so 需要文本输入时用，比如设置页输 SSID/口令）。
// seed = 预填文本（可 null）；done = 完成回调，收到**固件内部缓冲区**的最终文本，要立刻拷走。
// 设置页「试听」：重新加载音效并播一遍（不看 snd 开关）
extern "C" int so_sound_test(void) {
    m5ui::sound::reload();
    m5ui::sound::test();
    return 0;
}

// ── 设备上的真 shell（apps/terminal 用）────────────────────────────────
//   一次一条命令：out 里拿到纯文本（行间 '\n'，UTF-8 安全截断），*code 是退出码，
//   *flags bit0 = 已经排了重启。命令实现在 main/platform/sh.h。
extern "C" int so_sh_exec(const char* line, char* out, int cap, int* code, int* flags) {
    if (!out || cap < 8) return -2;
    return m5ui::sh::exec(line, out, cap, code, flags);
}

int so_ime_open(const char* seed, void (*done)(const char*), void (*cancel)(void)) {
    m5ui::pages::g_ime_seed = seed;
    m5ui::pages::g_ime_done = done;
    m5ui::pages::g_ime_cancel = cancel;
    if (!m5ui::pages::g_open_overlay) return -1;
    m5ui::pages::g_open_overlay();
    return 0;
}

// ══════════════════════════════════════════════════════════════════════════
// 通用目录列举（给屏保图选择用）—— .so 里没有文件系统 API，只能由宿主列出
//   返回：条目数（0 = 没有）；names 是 max×(cap) 的二维数组
//   ext：只要这个后缀的（nullptr/空 = 全要）
// ══════════════════════════════════════════════════════════════════════════
int so_list_files(const char* dir, const char* ext, char* names, int cap, int max) {
    if (!dir || !names || max <= 0 || cap <= 0) return 0;
    DIR* d = opendir(dir);
    if (!d) return 0;
    int n = 0;
    const int el = ext ? (int)strlen(ext) : 0;
    struct dirent* e;
    while ((e = readdir(d)) && n < max) {
        const char* nm = e->d_name;
        if (nm[0] == '.') continue;
        if (e->d_type == DT_DIR) continue;
        const int l = (int)strlen(nm);
        if (el && (l <= el || strcmp(nm + l - el, ext) != 0)) continue;
        snprintf(names + (size_t)n * cap, (size_t)cap, "%s", nm);
        ++n;
    }
    closedir(d);
    return n;
}

// ── 符号表（C 链接，全局作用域）：名字必须和 elf_loader 里 extern 的一致 ──

extern "C" const struct esp_elfsym g_customer_elfsyms[] = {
    ESP_ELFSYM_EXPORT(so_rect),
    ESP_ELFSYM_EXPORT(so_text),
    ESP_ELFSYM_EXPORT(so_snprintf),
    ESP_ELFSYM_EXPORT(so_font_load),
    ESP_ELFSYM_EXPORT(so_font_builtin),
    ESP_ELFSYM_EXPORT(so_battery_level),
    ESP_ELFSYM_EXPORT(so_cpu_load),
    ESP_ELFSYM_EXPORT(so_mem_used_pct),
    ESP_ELFSYM_EXPORT(so_mem_size_kb),
    ESP_ELFSYM_EXPORT(so_mem_free_kb),
    // ── 固件设置读写 / 网络（设置 app 用）──
    ESP_ELFSYM_EXPORT(so_fwcfg_get_int),
    ESP_ELFSYM_EXPORT(so_fwcfg_set_int),
    ESP_ELFSYM_EXPORT(so_fwcfg_get_str),
    ESP_ELFSYM_EXPORT(so_fwcfg_set_str),
    ESP_ELFSYM_EXPORT(so_fwcfg_save),
    ESP_ELFSYM_EXPORT(so_fwcfg_apply),
    ESP_ELFSYM_EXPORT(so_fwcfg_reset),
    ESP_ELFSYM_EXPORT(so_net_state),
    ESP_ELFSYM_EXPORT(so_net_ap_on),
    ESP_ELFSYM_EXPORT(so_net_web_on),
    ESP_ELFSYM_EXPORT(so_net_set_ap),
    ESP_ELFSYM_EXPORT(so_net_set_web),
    ESP_ELFSYM_EXPORT(so_net_ip),
    ESP_ELFSYM_EXPORT(so_net_ssid),
    ESP_ELFSYM_EXPORT(so_net_connect),
    ESP_ELFSYM_EXPORT(so_net_disconnect),
    ESP_ELFSYM_EXPORT(so_net_forget),
    ESP_ELFSYM_EXPORT(so_wifi_scan),
    ESP_ELFSYM_EXPORT(so_wifi_saved),
    ESP_ELFSYM_EXPORT(so_wifi_connect),
    ESP_ELFSYM_EXPORT(so_wifi_forget),
    ESP_ELFSYM_EXPORT(so_ime_open),
    ESP_ELFSYM_EXPORT(so_sound_test),
    ESP_ELFSYM_EXPORT(so_sh_exec),
    ESP_ELFSYM_EXPORT(so_list_files),
    ESP_ELFSYM_EXPORT(so_clock_hhmm),
    ESP_ELFSYM_EXPORT(so_clock_valid),
    ESP_ELFSYM_EXPORT(so_is_charging),
    ESP_ELFSYM_EXPORT(so_font_free),
    ESP_ELFSYM_EXPORT(so_font_set_size),
    ESP_ELFSYM_EXPORT(so_font_measure),
    ESP_ELFSYM_EXPORT(so_font_measure_prefix),
    ESP_ELFSYM_EXPORT(so_font_draw),
    ESP_ELFSYM_EXPORT(so_font_line_height),
    ESP_ELFSYM_EXPORT(so_font_size),
    ESP_ELFSYM_EXPORT(so_font_has),
    ESP_ELFSYM_EXPORT(so_font_ink_bounds),
    ESP_ELFSYM_EXPORT(so_font_text_ink),
    ESP_ELFSYM_EXPORT(so_psram_alloc),
    ESP_ELFSYM_EXPORT(so_psram_free),
    ESP_ELFSYM_EXPORT(so_list_fonts),
    ESP_ELFSYM_EXPORT(so_list_books),
    ESP_ELFSYM_EXPORT(so_read_book),
    ESP_ELFSYM_EXPORT(so_read_book_at),
    ESP_ELFSYM_EXPORT(so_book_size),
    ESP_ELFSYM_EXPORT(so_blob_load),
    ESP_ELFSYM_EXPORT(so_blob_save),
    ESP_ELFSYM_EXPORT(so_cfg_load),
    ESP_ELFSYM_EXPORT(so_cfg_save),
    ESP_ELFSYM_EXPORT(so_fill_rect),
    ESP_ELFSYM_EXPORT(so_set_epd_mode),
    ESP_ELFSYM_EXPORT(so_display),
    ESP_ELFSYM_EXPORT(so_fb_save),
    ESP_ELFSYM_EXPORT(so_fb_restore),
    ESP_ELFSYM_EXPORT(so_fb_save4),
    ESP_ELFSYM_EXPORT(so_fb_restore4),
    ESP_ELFSYM_EXPORT(so_fb_bytes),
    ESP_ELFSYM_EXPORT(so_wait_display),
    ESP_ELFSYM_EXPORT(so_touch),
    ESP_ELFSYM_EXPORT(so_touch_down),
    ESP_ELFSYM_EXPORT(so_begin_write),
    ESP_ELFSYM_EXPORT(so_end_write),
    ESP_ELFSYM_EXPORT(so_now_ms),
    ESP_ELFSYM_EXPORT(so_log),
    ESP_ELFSYM_EXPORT(so_log_file),
    ESP_ELFSYM_EXPORT(so_exit),
    ESP_ELFSYM_END
};

// ── 加载 ─────────────────────────────────────────────────────────

namespace m5ui::plugin {

static const char* TAG = "PLUGIN";

// 当前运行的 .so app（launcher 打开后置上，so_exit 清掉）
static void* g_app_handle = nullptr;
static void (*g_app_tick)() = nullptr;   // 可选：.so 每帧回调（交互用）

void test()
{
    void* h = dlopen("page_cpp.so", RTLD_NOW);
    if (!h) {
        ESP_LOGW(TAG, "dlopen 失败: %s", dlerror());
        return;
    }
    ESP_LOGI(TAG, "dlopen OK: %p", h);

    int (*ctor_check)(void) = (int (*)(void))dlsym(h, "page_ctor_check");
    if (ctor_check) {
        int v = ctor_check();
        ESP_LOGI(TAG, "page_ctor_check() = %d (%s)", v,
                 v == 42 ? "构造器跑了" : "构造器没跑!");
    }

    void (*page_init)(void) = (void (*)(void))dlsym(h, "page_init");
    if (!page_init) {
        ESP_LOGW(TAG, "dlsym(page_init) 失败: %s", dlerror());
        return;
    }
    page_init();
}

// 最近一次 load_app 失败的原因 —— launcher 会把它显示在**屏幕上**。
// 只看串口等于没报错：用户看到的只是"退回主页/白屏"，看不出原因。
static char g_last_error[160] = {0};

const char* last_error()
{
    return g_last_error;
}

// ★ API 版本握手（Flipper FAP 的 api_version 那套）
// 返回 true = 可以加载；false = 已把原因写进 g_last_error。
// 顺序：dlopen 成功 → 有版本号 → 主版本一致 → .so 不比固件新 → 才允许调 page_init。
static bool api_check(void* h, const char* name)
{
    using ver_fn_t = uint32_t (*)(void);
    ver_fn_t vf = (ver_fn_t)dlsym(h, "so_app_api_version");
    if (vf == nullptr) {
        // 老 .so 没这个符号。**不**放行 —— 放行就是把"版本对不上"变成运行期崩溃。
        snprintf(g_last_error, sizeof(g_last_error),
                 "%s 没带 API 版本号（旧 app）—— 固件 API %u.%u，请重编该 app",
                 name, API_VERSION_MAJOR, API_VERSION_MINOR);
        return false;
    }

    const uint32_t v  = vf();
    const uint16_t mj = (uint16_t)(v >> 16);
    const uint16_t mn = (uint16_t)(v & 0xFFFF);

    if (mj != API_VERSION_MAJOR) {
        snprintf(g_last_error, sizeof(g_last_error),
                 "API 主版本不匹配：app %u.%u ≠ 固件 %u.%u —— 请重编该 app",
                 mj, mn, API_VERSION_MAJOR, API_VERSION_MINOR);
        return false;
    }
    if (mn > API_VERSION_MINOR) {
        snprintf(g_last_error, sizeof(g_last_error),
                 "app 比固件新：app %u.%u > 固件 %u.%u —— 请先升级固件",
                 mj, mn, API_VERSION_MAJOR, API_VERSION_MINOR);
        return false;
    }
    ESP_LOGI(TAG, "API 版本 OK：app %u.%u / 固件 %u.%u", mj, mn,
             API_VERSION_MAJOR, API_VERSION_MINOR);
    return true;
}

bool load_app(const char* name)
{
    g_last_error[0] = 0;

    void* h = dlopen(name, RTLD_NOW);
    if (!h) {
        snprintf(g_last_error, sizeof(g_last_error), "dlopen 失败：%s", dlerror());
        ESP_LOGE(TAG, "%s", g_last_error);
        return false;
    }
    ESP_LOGI(TAG, "dlopen %s OK: %p", name, h);

    // ★ 先核对 API 版本，再决定要不要跑它
    if (!api_check(h, name)) {
        ESP_LOGE(TAG, "%s", g_last_error);
        return false;
    }

    void (*page_init)(void) = (void (*)(void))dlsym(h, "page_init");
    if (!page_init) {
        snprintf(g_last_error, sizeof(g_last_error), "找不到 page_init()：%s", dlerror());
        ESP_LOGE(TAG, "%s", g_last_error);
        return false;
    }

    // page_tick 可选：有就每帧调（.so 自持交互），没有就是一次性页面
    g_app_handle = h;
    g_app_tick = (void (*)(void))dlsym(h, "page_tick");
    ESP_LOGI(TAG, "page_tick %s", g_app_tick ? "有（每帧回调）" : "无（一次性页面）");

    // ★ 换 app 时清掉"待补报的那一下"：不然上一个 app 会话尾巴上锁存的一次按下，
    //   会补给新 app 的第一次 tick —— 表现就是"刚进来就被自动点了一下"。
    g_tap_latch = false;
    g_tap_sent0 = false;

    ESP_LOGI(TAG, "调用 %s page_init() ...", name);
    page_init();
    ESP_LOGI(TAG, "%s page_init() 返回", name);

    // ★ 吞掉"打开 app 那一下"的触摸 —— 否则 app 一进来就误触发一次点击。
    //   调用链全在**同一次** M5.update() 的状态里跑完：
    //     app.update()（M5.update() 读到"按下"）→ launcher 按钮 → load_app() → page_init()
    //   期间 M5.update() 没机会再跑，而 page_init() 可能耗时上秒（阅读器 1.41s）。
    //   等主循环走到 plugin::tick() 时，.so 调 so_touch() 读到的还是"刚按下"那一帧：
    //   实测打开阅读器后 **1ms** 就报 `点击 (333,170)` → 直接把第一本书打开了。
    //   （人不可能在卡了 1.4 秒后 1ms 内再点一下 —— 一眼能看出这不是用户操作。）
    //   这里把触摸状态刷到最新（手指已抬起 → getCount()==0），并给控制器留上报时间。
    for (int i = 0; i < 3; i++) { M5.update(); M5.delay(8); }

    return true;
}

// 每帧回调（主循环调）
void tick()
{
    if (!g_app_tick) return;

    // ★ 性能探针（spike）：把这一帧 app 花的字体成本拆成「光栅化 / 落图」。
    //   只在这一帧**真画了字形**时才打日志，所以不会每帧刷屏 ——
    //   按一次按钮画一屏 = 一条，翻一页 = 一条。
    //   （.so 自己那行 `绘制Xms | 推屏Yms` 里的"绘制"就是这里的两个数之和 + 少量杂项。）
    const int64_t h0 = m5ui::ttf::gcache::hits();
    const int64_t m0 = m5ui::ttf::gcache::misses();
    m5ui::ttf::perf_reset();
    g_app_tick();
    const int64_t n = m5ui::ttf::g_perf_glyphs;
    if (n > 0) {
        const int64_t r  = m5ui::ttf::g_perf_raster_us;
        const int64_t p  = m5ui::ttf::g_perf_push_us;
        const int64_t dh = m5ui::ttf::gcache::hits()   - h0;
        const int64_t dm = m5ui::ttf::gcache::misses() - m0;
        ESP_LOGI(TAG, "绘制拆分: %lld 字形 | 光栅化 %lldus (%.2fms/字) | 落图 %lldus (%.2fms/字) | 光栅占比 %.0f%%"
                      " | 字形缓存 命中%lld/查%lld (%.0f%%) 存%d条%uKB | PSRAM余%uKB",
                 (long long)n,
                 (long long)r, (double)r / (double)n / 1000.0,
                 (long long)p, (double)p / (double)n / 1000.0,
                 (r + p) ? 100.0 * (double)r / (double)(r + p) : 0.0,
                 (long long)dh, (long long)(dh + dm),
                 (dh + dm) ? 100.0 * (double)dh / (double)(dh + dm) : 0.0,
                 m5ui::ttf::gcache::count(), (unsigned)(m5ui::ttf::gcache::bytes() / 1024),
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    }
}

// 退出（so_exit 调）：停掉每帧回调。不 dlclose —— .so 常驻，
// 避免字符串常量/后续引用悬空。
void close()
{
    g_app_tick = nullptr;
    g_app_handle = nullptr;
}

// 是否有 .so app 在跑
bool active()
{
    return g_app_handle != nullptr;
}

} // namespace m5ui::plugin
