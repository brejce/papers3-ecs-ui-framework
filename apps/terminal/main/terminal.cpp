// apps/terminal/main/terminal.cpp — 设备上的**真**终端（v1）
//
// 和后端的分工（见 docs/terminal/TERMINAL_SHELL.md）：
//   · 命令执行在固件（m5ui::sh），.so 只调 so_sh_exec 拿回一段纯文本；
//   · 这里只负责：滚动缓冲、折行、画、收键盘（固件输入法 so_ime_open）、快捷键。
//
// 版式（540×960）：
//    0 ┌ < EXIT      12:31      BAT 87% ┐  60
//   64 │ 滚动区 30 行 × 26px            │ 844
//  850 ├──────── 点线 ────────┤
//  856 │ > 输入行（点它=开输入法）      │ 44
//  906 │ [ 上一条 ] [ clear ] [ help ]  │
//
// 刷新纪律：进屏 QUALITY 整屏；之后每条命令只推「滚动区 + 输入行 + 提示行」一次。
#include "../common/so_api.h"
#include <string.h>
#include "../common/so_meta.h"

namespace {

constexpr int SCR_W = 540, SCR_H = 960, PADX = 24, BAR_H = 60;
constexpr uint32_t L14 = 0xE8E8E8, INK = 0x000000;
constexpr int SZ_BAR = 20, SZ_TXT = 20, LH = 26, SZ_KEY = 19;

constexpr int AREA_Y = 64, LINES = 30;                  // 64 .. 844
constexpr int SEP_Y = 850;
constexpr int IN_Y = 856, IN_H = 44;
constexpr int HINT_Y = 906, HINT_H = 34;
constexpr int TXT_W = SCR_W - 2 * PADX;                  // 492

constexpr int OUT_CAP = 3072;
constexpr int SCROLL_CAP = 8192;
constexpr int HIST_MAX = 20, HIST_LEN = 160;

void* g_font = nullptr;
char* g_buf = nullptr;         // 滚动缓冲（PSRAM）
int   g_len = 0;
char  g_input[HIST_LEN] = {0};
bool  g_ime_open = false;
int   g_push_n = 0;

char  g_hist[HIST_MAX][HIST_LEN];
int   g_nhist = 0;

// ── 文本缓冲 ────────────────────────────────────────────────────
void scroll_drop(int need) {
    if (!g_buf) return;
    if (g_len + need <= SCROLL_CAP) return;
    int drop = 0;
    while (g_len + need > SCROLL_CAP && drop < g_len) {
        while (drop < g_len && g_buf[drop] != '\n') drop++;
        if (drop < g_len) drop++;
    }
    if (drop <= 0) { g_len = 0; if (g_buf) g_buf[0] = 0; return; }
    for (int i = 0; i < g_len - drop; i++) g_buf[i] = g_buf[i + drop];   // memmove 不在加载器白名单里
    g_len -= drop;
    g_buf[g_len] = 0;
}

void append(const char* s, int n) {
    if (!g_buf || !s || n <= 0) return;
    if (n > SCROLL_CAP) { s += n - SCROLL_CAP; n = SCROLL_CAP; }
    scroll_drop(n);
    memcpy(g_buf + g_len, s, (size_t)n);
    g_len += n;
    g_buf[g_len] = 0;
}
void append_s(const char* s) { if (s) append(s, (int)strlen(s)); }
void append_line(const char* s) { append_s(s); append("\n", 1); }

// ── 折行：按 492px 宽度切 ──────────────────────────────────────
int u8_len(unsigned char c) {
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

// 从 s 里取能放进 maxw 的字节数（至少一个字符）
int fit_bytes(const char* s, int maxw) {
    const int n = (int)strlen(s);
    int i = 0, chars = 0;
    while (i < n) {
        const int step = u8_len((unsigned char)s[i]);
        chars++;
        const int w = (int)so_font_measure_prefix(g_font, s, chars);
        if (w > maxw) { chars--; break; }
        i += step;
    }
    if (chars <= 0) return u8_len((unsigned char)s[0]);
    return i;
}

int layout_lines(int* st, int* en, int cap) {
    int n = 0;
    if (!g_buf) return 0;
    int i = 0;
    while (i < g_len && n < cap) {
        int j = i;
        while (j < g_len && g_buf[j] != '\n') j++;
        if (j == i) { st[n] = i; en[n] = i; n++; i = j + 1; continue; }
        int p = i;
        while (p < j && n < cap) {
            char tmp[512];
            int tl = j - p;
            if (tl > (int)sizeof(tmp) - 1) tl = (int)sizeof(tmp) - 1;
            memcpy(tmp, g_buf + p, (size_t)tl);
            tmp[tl] = 0;
            int take = fit_bytes(tmp, TXT_W);
            if (take <= 0) take = u8_len((unsigned char)g_buf[p]);
            if (p + take > j) take = j - p;
            st[n] = p;
            en[n] = p + take;
            n++;
            p += take;
        }
        i = j + 1;
    }
    return n;
}

// ── 画 ──────────────────────────────────────────────────────────
void draw_bar(int full) {
    so_set_epd_mode(2);
    so_font_set_size(g_font, (float)SZ_BAR);
    if (full) {
        so_font_draw(g_font, PADX, (BAR_H - SZ_BAR) / 2, "< EXIT", INK, 2);
    }
    if (so_clock_valid()) {
        char t[8];
        const int v = so_clock_hhmm();
        so_snprintf(t, sizeof(t), "%02d:%02d", v / 100, v % 100);
        const int32_t w = so_font_measure_prefix(g_font, t, 5);
        so_font_draw(g_font, (SCR_W - w) / 2, (BAR_H - SZ_BAR) / 2, t, 25, 2);
    }
    const int bat = so_battery_level();
    char b[12];
    if (bat < 0) so_snprintf(b, sizeof(b), "BAT --");
    else         so_snprintf(b, sizeof(b), "%d%%", bat);
    const int32_t bw = so_font_measure_prefix(g_font, b, (int)strlen(b));
    so_font_draw(g_font, SCR_W - PADX - bw, (BAR_H - SZ_BAR) / 2, b, INK, 1);
}

void draw_lines() {
    static int st[600], en[600];
    const int n = layout_lines(st, en, 600);
    const int first = (n > LINES) ? n - LINES : 0;
    int y = AREA_Y;
    for (int i = first; i < n; i++) {
        char tmp[512];
        int len = en[i] - st[i];
        if (len > (int)sizeof(tmp) - 1) len = (int)sizeof(tmp) - 1;
        if (len > 0) {
            memcpy(tmp, g_buf + st[i], (size_t)len);
            tmp[len] = 0;
            so_font_draw(g_font, PADX, y, tmp, INK, 0);
        }
        y += LH;
    }
}

void draw_input() {
    so_set_epd_mode(2);
    so_font_set_size(g_font, (float)SZ_TXT);
    so_font_draw(g_font, PADX, IN_Y + 8, ">", 25, 1);
    const int32_t ix = PADX + 18;
    int32_t w = 0;
    if (g_input[0]) w = so_font_draw(g_font, ix, IN_Y + 8, g_input, INK, 0);
    so_fill_rect(ix + w + 2, IN_Y + 12, 10, 20, INK);
}

void draw_hints() {
    so_set_epd_mode(2);
    so_font_set_size(g_font, (float)SZ_KEY);
    so_font_draw(g_font, PADX, HINT_Y + 6, "[ 上一条 ]", 25, 1);
    so_font_draw(g_font, PADX + 118, HINT_Y + 6, "[ clear ]", 25, 1);
    so_font_draw(g_font, PADX + 236, HINT_Y + 6, "[ help ]", 25, 1);
}

void push_all(const char* why) {
    so_set_epd_mode(2);
    so_display(0, 0, SCR_W, SCR_H);
    so_wait_display();
    char b[64];
    so_snprintf(b, sizeof(b), "推%02d %s", ++g_push_n, why);
    so_log(b);
    so_log_file(b);
}

void redraw(const char* why) {
    so_set_epd_mode(3);
    so_fill_rect(0, AREA_Y - 4, SCR_W, SCR_H - AREA_Y + 4, L14);
    so_set_epd_mode(2);
    for (int x = PADX; x < SCR_W - PADX; x += 6) so_fill_rect(x, SEP_Y, 3, 2, 180);
    draw_lines();
    draw_bar(0);
    draw_input();
    draw_hints();
    push_all(why);
}

// ── 执行 ────────────────────────────────────────────────────────
void hist_add(const char* cmd) {
    if (!cmd[0]) return;
    if (g_nhist > 0 && strcmp(g_hist[g_nhist - 1], cmd) == 0) return;
    if (g_nhist == HIST_MAX) {
        for (int i = 0; i < (HIST_MAX - 1) * HIST_LEN; i++)
            (&g_hist[0][0])[i] = (&g_hist[1][0])[i];
        g_nhist--;
    }
    so_snprintf(g_hist[g_nhist], HIST_LEN, "%s", cmd);
    g_nhist++;
}

void run(const char* raw) {
    char cmd[HIST_LEN];
    so_snprintf(cmd, sizeof(cmd), "%s", raw ? raw : "");
    char* s = cmd;
    while (*s == ' ') s++;
    int L = (int)strlen(s);
    while (L > 0 && (s[L - 1] == ' ' || s[L - 1] == '\n')) s[--L] = 0;
    if (!L) return;

    append_s("> ");
    append_line(s);
    hist_add(s);

    if (strcmp(s, "clear") == 0) { g_len = 0; g_buf[0] = 0; return; }
    if (strcmp(s, "exit") == 0 || strcmp(s, "quit") == 0) { so_exit(); return; }
    if (strcmp(s, "history") == 0) {
        for (int i = 0; i < g_nhist; i++) {
            char t[HIST_LEN + 10];
            so_snprintf(t, sizeof(t), "  %2d  %s", i + 1, g_hist[i]);
            append_line(t);
        }
        return;
    }
    if (strcmp(s, "help") == 0) {
        append_line("文件   pwd cd ls cat head tail wc df mkdir rm mv cp write append");
        append_line("系统   free ps uptime date ver log");
        append_line("网络   wifi [scan|on|off|conn]  ip");
        append_line("配置   cfg [get <k>|set <k> <v>|save|reset]");
        append_line("电源   reboot        界面   help clear exit history");
        append_line("/apps 自动补成 /sdcard/apps；只能动 /sdcard，apps/*.so 只读");
        return;
    }

    static char out[OUT_CAP];
    int code = 0, flags = 0;
    const int n = so_sh_exec(s, out, OUT_CAP, &code, &flags);
    if (n < 0)      append_line("sh: 固件不支持 so_sh_exec（需要 API v1.14，刷机）");
    else if (n > 0) append(out, n);
    if (code == 1)      append_line("（命令不存在，敲 help）");
    else if (code == 2) append_line("（用法不对，敲 help）");
    else if (code == 3) append_line("（路径被拒绝：只能动 /sdcard）");
    else if (code == 4) append_line("（执行失败，看上一行）");
    if (flags & 1) append_line("设备正在重启…");
}

// ── 输入法回调 ──────────────────────────────────────────────────
void on_ime_done(const char* text) {
    g_ime_open = false;
    if (!text) { redraw("取消输入"); return; }
    so_snprintf(g_input, sizeof(g_input), "%s", text);
    if (g_input[0]) {
        run(g_input);
        g_input[0] = 0;
    }
    so_set_epd_mode(1);                 // QUALITY：键盘收起后清残影
    redraw("命令完成");
    char b[64];
    so_snprintf(b, sizeof(b), "执行完成：缓冲 %d 字节", g_len);
    so_log(b);
}

void on_ime_cancel() {
    g_ime_open = false;
    redraw("取消输入");
}

void boot_text() {
    g_len = 0;
    if (g_buf) g_buf[0] = 0;
    append_line("m5ui terminal · v1");
    append_line("命令真的在这台设备上跑：文件 / 内存 / 任务 / 配置 / 网络 / 重启");
    append_line("点下面那行输入；敲 help 看命令");
    append_line("");
    append_line("root@m5ui:/sdcard #");
    append_line("（工作目录跟着 cd 走，重启复位到 /sdcard）");
}

} // namespace

extern "C" __attribute__((visibility("default")))
void page_init(void) {
    if (!g_buf) g_buf = (char*)so_psram_alloc(SCROLL_CAP);
    if (!g_font) g_font = so_font_builtin((float)SZ_TXT);
    so_set_epd_mode(2);
    so_font_set_size(g_font, (float)SZ_TXT);
    boot_text();

    so_set_epd_mode(1);                       // 进屏 QUALITY 整屏
    so_set_epd_mode(3);
    so_fill_rect(0, 0, SCR_W, SCR_H, L14);
    so_set_epd_mode(2);
    so_fill_rect(0, BAR_H - 1, SCR_W, 1, INK);
    draw_bar(1);
    for (int x = PADX; x < SCR_W - PADX; x += 6) so_fill_rect(x, SEP_Y, 3, 2, 180);
    draw_lines();
    draw_input();
    draw_hints();
    so_set_epd_mode(1);
    so_display(0, 0, SCR_W, SCR_H);
    so_wait_display();
    so_log("终端：进屏（真 shell）");
}

extern "C" __attribute__((visibility("default")))
void page_tick(void) {
    static bool was = false;
    int32_t x = 0, y = 0;
    const int t = so_touch(&x, &y);
    const bool down = (t != 0);
    if (down && !was) {
        if (y < BAR_H && x < 200) { so_exit(); return; }
        if (y >= HINT_Y - 6 && y < HINT_Y + HINT_H) {
            if (x < PADX + 110) {                                  // [ 上一条 ]
                if (g_nhist > 0) so_snprintf(g_input, sizeof(g_input), "%s", g_hist[g_nhist - 1]);
                if (!g_ime_open) {
                    g_ime_open = true;
                    if (so_ime_open(g_input, on_ime_done, on_ime_cancel) != 0) g_ime_open = false;
                }
            } else if (x < PADX + 228) {                           // [ clear ]
                g_len = 0;
                g_buf[0] = 0;
                redraw("clear");
            } else {                                               // [ help ]
                run("help");
                redraw("help");
            }
            was = down;
            return;
        }
        // 输入行 / 输出区 点一下都拉键盘（这机器没物理键）
        if (!g_ime_open) {
            g_ime_open = true;
            if (so_ime_open(g_input, on_ime_done, on_ime_cancel) != 0) {
                g_ime_open = false;
                append_line("输入法打不开（固件太旧）");
                redraw("无输入法");
            }
        }
    }
    was = down;
}

extern "C" __attribute__((visibility("default")))
uint32_t so_app_api_version(void) { return SO_API_VERSION; }

extern "C" __attribute__((visibility("default")))
const char* so_app_name(void) { return "终端"; }

SO_APP_META("终端", "真 shell · 文件/内存/任务/配置/重启", 32, 32, 1,
                 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1F, 0xFF, 0xFF, 0xF8, 0x1F, 0xFF, 0xFF, 0xF8,
                 0x18, 0x00, 0x00, 0x18, 0x18, 0x00, 0x00, 0x18, 0x18, 0x00, 0x00, 0x18, 0x18, 0x00, 0x00, 0x18,
                 0x18, 0x60, 0x00, 0x18, 0x18, 0x61, 0x80, 0x18, 0x18, 0x19, 0x80, 0x18, 0x18, 0x1E, 0x00, 0x18,
                 0x18, 0x06, 0x00, 0x18, 0x18, 0x1E, 0x00, 0x18, 0x18, 0x19, 0x80, 0x18, 0x18, 0x61, 0xBF, 0x98,
                 0x18, 0x60, 0x3F, 0x98, 0x18, 0x00, 0x3F, 0x98, 0x18, 0x00, 0x00, 0x18, 0x18, 0x00, 0x00, 0x18,
                 0x1F, 0xFF, 0xFF, 0xF8, 0x1F, 0xFF, 0xFF, 0xF8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00)
;
