// apps/reader/test/stub_so.cpp — 宿主导出接口的**PC 桩**（把 .so 的排版引擎搬到电脑上跑）
//
// 目的：窗口滑动 + 分页 + 折行这套逻辑最大的风险是「跨窗口漏行/重行」，那只能靠
// 逐页比对文本证明，不该靠肉眼看设备。这里用最小的桩把 layout.h 原样跑起来：
//   · 文件：直接读电脑上的 books/*.txt
//   · 字体：等宽假字体（中文 = px 宽、ASCII = px/2），折行结果和真字体同构
//   · so_font_draw：把画出来的文字**收集**起来，供逐页比对
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstdarg>
#include <cstdint>
#include <ctime>
#include "../common/so_api.h"

// ── 书籍目录（run.sh 通过环境变量传）──
static const char* g_dir = "books";

static void full_path(const char* name, char* out, size_t cap) {
    snprintf(out, cap, "%s/%s", g_dir, name);
}

// ── 收集器：so_font_draw 画了什么按顺序攒起来 ──
static char  g_col[1 << 21];
static long  g_col_n = 0;
static bool  g_collect = false;

void collect_reset() { g_col_n = 0; g_collect = true; }
void collect_stop()  { g_collect = false; g_col[g_col_n] = 0; }
const char* collect_buf() { g_col[g_col_n] = 0; return g_col; }
long collect_len() { return g_col_n; }

// ── 假字体 ──
struct FakeFont { float px; };
static FakeFont g_ff[4];
static int g_ff_n = 0;

void* fake_font(float px) { g_ff[g_ff_n].px = px; return &g_ff[g_ff_n++ % 4]; }

static int adv_of(float px, unsigned char c) {
    if (c < 0x80) return (int)(px * 0.5f + 0.5f);
    return (int)(px + 0.5f);
}
static int u8l(unsigned char c) {
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

extern "C" {

void* so_psram_alloc(size_t n) { return malloc(n); }
void  so_psram_free(void* p)   { free(p); }

int so_snprintf(char* out, int cap, const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int r = vsnprintf(out, (size_t)cap, fmt, ap);
    va_end(ap);
    return r;
}

int so_list_books(char (*names)[256], int max) {
    FILE* p = popen("ls -1 books | head -20", "r");
    if (!p) return 0;
    int n = 0;
    char line[512];
    while (n < max && fgets(line, sizeof(line), p)) {
        int L = (int)strlen(line);
        while (L > 0 && (line[L-1] == '\n' || line[L-1] == '\r')) line[--L] = 0;
        if (!L) continue;
        snprintf(names[n], 256, "%s", line);
        n++;
    }
    pclose(p);
    return n;
}

int so_read_book(const char* name, char* buf, int cap) {
    char path[512]; full_path(name, path, sizeof(path));
    FILE* f = fopen(path, "rb");
    if (!f) return -1;
    size_t got = fread(buf, 1, (size_t)cap, f);
    fclose(f);
    return (int)got;
}
int so_read_book_at(const char* name, char* buf, int off, int want) {
    char path[512]; full_path(name, path, sizeof(path));
    FILE* f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, (long)off, SEEK_SET);
    size_t got = fread(buf, 1, (size_t)want, f);
    fclose(f);
    return (int)got;
}
int so_book_size(const char* name) {
    char path[512]; full_path(name, path, sizeof(path));
    FILE* f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fclose(f);
    return (int)sz;
}

int so_blob_load(const char* name, char* buf, int cap) { (void)name; (void)buf; (void)cap; return -1; }
int so_blob_save(const char* name, const char* buf, int len) { (void)name; (void)buf; return len; }
int so_cfg_load(char* buf, int cap) { (void)buf; (void)cap; return -1; }
int so_cfg_save(const char* buf, int len) { (void)buf; return len; }
int so_list_fonts(char (*names)[80], int max) { (void)names; (void)max; return 0; }
int so_list_files(const char*, const char*, char*, int, int) { return 0; }

void*  so_font_load(const char* name, float px) { (void)name; return fake_font(px); }
void*  so_font_builtin(float px) { return fake_font(px); }
void   so_font_free(void* f) { (void)f; }
void   so_font_set_size(void* f, float px) { if (f) ((FakeFont*)f)->px = px; }
float  so_font_size(void* f) { return f ? ((FakeFont*)f)->px : 26.0f; }
int    so_font_line_height(void* f) { return (int)(so_font_size(f) * 1.35f + 0.5f); }
int32_t so_font_measure_prefix(void* f, const char* s, int32_t n) {
    if (!f || !s || n <= 0) return 0;
    const float px = ((FakeFont*)f)->px;
    int32_t w = 0; int cnt = 0;
    for (const char* p = s; *p && cnt < n; ) {
        const unsigned char c = (unsigned char)*p;
        w += adv_of(px, c);
        cnt++;
        p += u8l(c);
    }
    return w;
}
int so_font_measure(void* f, const char* s, int32_t* w, int32_t* h) {
    if (w) *w = so_font_measure_prefix(f, s, 100000);
    if (h) *h = so_font_line_height(f);
    return 0;
}
int so_font_draw(void* f, int32_t x, int32_t y, const char* s, uint8_t ink, int32_t ls) {
    (void)f; (void)x; (void)y; (void)ink; (void)ls;
    if (g_collect && s) {
        const size_t n = strlen(s);
        if (g_col_n + (long)n < (long)sizeof(g_col) - 1) { memcpy(g_col + g_col_n, s, n); g_col_n += (long)n; }
    }
    return 0;
}
int so_font_text_ink(void* f, const char* s, int32_t ls, int32_t* x0, int32_t* y0, int32_t* x1, int32_t* y1) {
    const int32_t w = so_font_measure_prefix(f, s, 100000) + (int32_t)strlen(s) * 0 + (ls > 0 ? 0 : 0);
    if (x0) *x0 = 0;
    if (y0) *y0 = 0;
    if (x1) *x1 = w;
    if (y1) *y1 = so_font_line_height(f);
    return 0;
}
int so_font_ink_bounds(void* f, int32_t cp, int32_t* x0, int32_t* y0, int32_t* x1, int32_t* y1) {
    (void)f; (void)cp;
    if (x0) *x0 = 0;
    if (y0) *y0 = 0;
    if (x1) *x1 = 10;
    if (y1) *y1 = 10;
    return 0;
}
int so_font_has(void* f, int32_t cp) { (void)f; (void)cp; return 1; }

int so_fill_rect(int32_t, int32_t, int32_t, int32_t, uint32_t) { return 0; }
int so_set_epd_mode(int) { return 0; }
int so_display(int32_t, int32_t, int32_t, int32_t) { return 0; }
int so_wait_display(void) { return 0; }
int so_fb_save(uint8_t*, int32_t, int32_t, int32_t, int32_t) { return 0; }
int so_fb_restore(const uint8_t*, int32_t, int32_t, int32_t, int32_t) { return 0; }
int so_fb_save4(uint8_t*, int32_t, int32_t, int32_t, int32_t) { return 0; }
int so_fb_restore4(const uint8_t*, int32_t, int32_t, int32_t, int32_t) { return 0; }
int so_fb_bytes(int32_t w, int32_t h) { return ((w + 1) / 2) * h; }
void so_begin_write(void) {}
int  so_end_write(void) { return 0; }
int  so_touch(int32_t* x, int32_t* y) { if (x) *x = 0; if (y) *y = 0; return 0; }
int  so_touch_down(void) { return 0; }
int64_t so_now_ms(void) { return (int64_t)clock() * 1000 / CLOCKS_PER_SEC; }
void so_log(const char* m) { (void)m; }
void so_log_file(const char* m) { (void)m; }
void so_exit(void) {}
int  so_battery_level(void) { return 88; }
int  so_cpu_load(void) { return 0; }
int  so_mem_used_pct(void) { return 0; }
int  so_mem_size_kb(int) { return 0; }
int  so_mem_free_kb(int) { return 0; }
int  so_clock_hhmm(void) { return 1230; }
int  so_clock_valid(void) { return 1; }
int  so_is_charging(void) { return 0; }

} // extern "C"

void stub_set_book_dir(const char* d) { g_dir = d; }
