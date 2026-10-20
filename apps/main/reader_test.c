/* reader_test.c — 阶段 1 最小验证：.so 通过宿主 API 从 SD 读字体、画中文。
 *
 * 关键约束（elf_loader libc 白名单）：.so 里**不能**用 fopen/opendir/readdir/
 * fread/fseek/ftell/snprintf/strcat/strcpy/strcasecmp —— 这些符号不在白名单，
 * 用了会导致 dlopen 重定位失败（Can't find symbol）卡死。
 * 能用的只有白名单里的 strlen/memcpy/memset/strcmp/strchr/strrchr/printf 等。
 * 所以：扫字体目录、拼路径、读文件、格式化全走宿主 so_* API。
 *
 * 所有 so_* 是未定义符号，dlopen 重定位时由固件 g_customer_elfsyms[] 解析。
 */
#include <stdint.h>

extern int   so_list_fonts(char (*names)[80], int max);
extern void* so_font_load(const char* name, float px);
extern int   so_font_draw(void* f, int32_t x, int32_t y, const char* s, uint8_t ink, int32_t ls);
extern int   so_font_line_height(void* f);
extern int   so_fill_rect(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color);
extern int   so_set_epd_mode(int mode);
extern int   so_display(int32_t x, int32_t y, int32_t w, int32_t h);
extern int   so_wait_display(void);

/* 画「没字体/加载失败」提示块（黑块），证明 .so 本身加载+绘制 OK */
static void draw_fail(void)
{
    so_set_epd_mode(3);   // FAST/DU 色块
    so_fill_rect(0, 0, 540, 960, 0xFFFFFF);
    so_fill_rect(60, 400, 420, 160, 0x000000);
    so_display(0, 0, 540, 960);
    so_wait_display();
}

/* -fvisibility=hidden 下要显式 default 才会进 .dynsym */
__attribute__((visibility("default")))
void page_init(void)
{
    char fonts[4][80];
    int n = so_list_fonts(fonts, 4);
    if (n <= 0) { draw_fail(); return; }

    /* 按文件名加载，宿主拼 /sdcard/fonts/<name> */
    void* f = so_font_load(fonts[0], 34.0f);
    if (!f) { draw_fail(); return; }

    int lh = so_font_line_height(f);

    so_set_epd_mode(2);   // TEXT/GL16（文字 16 级灰抗锯齿）
    so_fill_rect(0, 0, 540, 960, 0xFFFFFF);

    so_font_draw(f, 40, 60, "reader.so 验证", 0, 0);
    so_font_draw(f, 40, 60 + lh * 2, "字体从 SD 加载，独立句柄", 0, 4);
    so_font_draw(f, 40, 60 + lh * 3, "不碰固件全局字体", 0, 4);
    so_font_draw(f, 40, 60 + lh * 4, "字间距 = 4px", 0, 4);

    so_display(0, 0, 540, 960);
    so_wait_display();

    /* 不 so_font_free：字体常驻，正式版由 .so 自己在退出时 free */
}
