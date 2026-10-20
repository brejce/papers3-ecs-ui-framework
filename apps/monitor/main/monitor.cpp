// apps/terminal/main/monitor.cpp — 《world.execute(me);》终端（.so app）v2
//
// UI 来自用户 2026-10-15 的新稿：状态栏 / 核心负载条 / 系统信息面板 / 实时日志 / 提示 / EXECUTE。
// 与旧版的区别：心跳波形那一套整个去掉；中间那块条子不再"逐渐掉墨"，改成**真实的核心占用率**，
// 5 秒一刷新（用户原话：「不用逐渐减少，就用核心占用率来替代，5 秒一刷新」）。
//
// ★★ 刷新纪律（用户明确要求，照抄输入框那套 main/systems/render.h 左对齐分支）：
//   1. 静态只画一次：框线、标签、键名、固定值、按钮、提示 —— 之后**永不重推**。
//   2. 只推真正变了的：负载条变化的那一小截、数字的墨迹盒、新来的那一行日志、光标。
//   3. 数字推送框按**字形墨迹**算，不按行盒：22px 实测数字 advance 12.54 / 墨高 17（不下探）；
//      12px 实测 advance 6.84 / 墨高 10。所以高度是墨高，不是行高。
//   4. 只有切屏才整屏（QUALITY 清残影）；稳定态**不该出现任何整屏推送**。
//   5. 每条 push 必须写原因，并同时落串口与 SD —— 面板会把"上次推屏之后写过的像素"并成一个大矩形，
//      所以"看到某块在刷、代码里却找不到"时，只能靠这条日志定位（已经被这条抓过一次）。
//
// 档位分工（EPD_TEST_MANUAL §规矩：**写像素时就决定量化**，画与推必须同档）：
//   整屏底 = FAST 档下写 L14（v8=232）→ Bayer 量化成每 4×4 一格黑点的点阵
//   静态/文字 = TEXT（4bit 灰度，抗锯齿，灰字也才有意义）
//   负载条 / 进度格 = FAST（纯黑白块，2 级最干净）
//   光标 = FASTEST（只翻 18×3）
//   切屏 = QUALITY 整屏
#include "../common/so_api.h"
#include "../common/so_meta.h"

// ══════════════════════════════════════════════════════════════
// 基础
// ══════════════════════════════════════════════════════════════
struct R { int x, y, w, h; };
static const int SCR_W = 540, SCR_H = 960;
static const int PADX  = 24;                 // 新稿：左右内边距 24
static const int BAR_H = 60;                 // 状态栏高 60

static const uint32_t L14   = 0xE8E8E8;      // 16 级灰的倒数第二白（FAST 档下 = 点阵底）
static const uint32_t INK   = 0x000000;
static const uint32_t WHITE = 0xFFFFFF;
static const uint8_t  GRAY  = 25;            // 次要文字：再加深（110 → 60 → 25；用户说首页整体颜色偏淡）

static const int MODE_QUALITY = 1, MODE_TEXT = 2, MODE_FAST = 3, MODE_FASTEST = 4;

// 字号（用户 2026-10-15：原来 11~14px 太小，「怎么也得接近 20」）
static const int SZ_BAR    = 20;   // 状态栏
static const int SZ_TITLE  = 18;   // // CORE LOAD 这类小标题
static const int SZ_VAL    = 20;   // 数值 / 信息面板的值 / 日志行 / 警告行
static const int SZ_KEY    = 19;   // 键名 / 状态词 / 副提示
static const int SZ_PROMPT = 24;   // > AWAITING COMMAND
static const int SZ_BTN    = 24;   // EXECUTE
static const int SZ_EXEC   = 24;   // 执行屏的日志行 + 进度数字（用户 2026-10-15：原来 20 太小）

static void* g_font = nullptr;
static int   g_push_n = 0;

// 一条 push 的日志（串口 + SD）。SD 上那个文件名是宿主编死的 reader.log（导出没有参数），
// 只能事后用 web 接口取回来核对现场。
static void push(const R& r, const char* why) {
    so_display(r.x, r.y, r.w, r.h);
    so_wait_display();
    char b[96];
    so_snprintf(b, sizeof(b), "推%03d %-10s (%d,%d %dx%d)", ++g_push_n, why, r.x, r.y, r.w, r.h);
    so_log(b);
    so_log_file(b);
}

static void box(const R& r, uint32_t c) { so_fill_rect(r.x, r.y, r.w, r.h, c); }
static void box(int x, int y, int w, int h, uint32_t c) { so_fill_rect(x, y, w, h, c); }
static void frame(const R& r, int t, uint32_t c) {          // 直角描边（四笔）
    box(r.x, r.y, r.w, t, c);
    box(r.x, r.y + r.h - t, r.w, t, c);
    box(r.x, r.y, t, r.h, c);
    box(r.x + r.w - t, r.y, t, r.h, c);
}
static void text(int x, int y, const char* s, uint8_t ink, int ls) {
    so_font_draw(g_font, x, y, s, ink, ls);
}
static int text_w(const char* s) { int32_t w = 0, h = 0; so_font_measure(g_font, s, &w, &h); return w; }
static int text_w_ls(const char* s, int ls) {                 // 带字间距的宽度（居中/右对齐用）
    int32_t w = 0, h = 0;
    so_font_measure(g_font, s, &w, &h);
    int n = 0;
    while (s[n]) ++n;
    return w + (n > 0 ? (n - 1) * ls : 0);
}
// 居中画：**纵横都居中**。r.h 等于行高时纵向偏移正好为 0（状态栏那些调用不受影响），
// 但像 EXECUTE 这种 80px 高的按钮，只做横向居中就会把字顶在框的上沿（真机上被抓到）。
static void text_c(const R& r, const char* s, uint8_t ink, int px, int ls) {
    so_font_set_size(g_font, (float)px);
    const int lh = so_font_line_height(g_font);
    text(r.x + (r.w - text_w_ls(s, ls)) / 2,
         r.y + (r.h > lh ? (r.h - lh) / 2 : 0), s, ink, ls);
}

// ── 数字块（右对齐）：尾巴（"%"）静态画一次，数字单独刷；推送框 = 数字的墨迹盒 ──
static int  g_num_cw  = 7;      // 一个数字的 advance（按字号量；12px 实测 6.84）
static int  g_num_px  = 12;     // 数字字号
static char g_val_load[8] = {0}, g_val_mem[8] = {0}, g_val_bat[8] = {0};

static int slen0(const char* s) { int n = 0; while (s[n]) ++n; return n; }
static void scopy(char* d, const char* s, int cap) {
    int i = 0;
    for (; s[i] && i < cap - 1; ++i) d[i] = s[i];
    d[i] = 0;
}
static int streq(const char* a, const char* b) {
    int i = 0;
    while (a[i] && b[i]) { if (a[i] != b[i]) return 0; ++i; }
    return a[i] == b[i];
}
// 数字墨高（实测比例：22px→17、12px→10）+ 上下各留 2px 余量

// ── 字形墨迹盒（用户要求；也是固件里输入框那套规则）──
//   （早期按「行高 81% + advance−2」估算，实测偏小、擦不干净；现在全部走宿主真值）
static int m_lh(int px);
// 一串文字的**横向真墨迹**（相对原点）：宿主逐字 GetCodepointBitmapBox 取并集，
// 与固件 ttf::measure_extent 同一套机制（多了字间距 ls）。
// ★ 绝不能拿 advance 求和当墨迹宽：字形会超出 advance（左右 overhang），
//   偏小 → 擦不干净（render.h 第 112-113 行正是为此用 act_l/act_r）。
static int text_span(const char* s, int px, int ls, int* l, int* r) {
    so_font_set_size(g_font, (float)px);
    int32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    if (so_font_text_ink(g_font, s, ls, &x0, &y0, &x1, &y1) != 0) return 0;
    if (x1 <= x0) { *l = 0; *r = 0; return 0; }         // 空串/全空白字形：没有墨
    *l = x0;
    *r = x1;
    return 1;
}
// 点线引导：在 [x0,x1] 内补尽量多的 '.'（字间距 3，灰），宽度运行时按真墨迹量，绝不越过 x1。
static void dot_leader(int x0, int x1, int y, int px) {
    if (x1 - x0 < 12) return;
    char d[72];
    int n = 0;
    for (; n < 70; ++n) {
        d[n] = '.'; d[n + 1] = 0;
        int l = 0, r = 0;
        if (!text_span(d, px, 3, &l, &r) || x0 + r > x1) break;
    }
    if (n < 3) return;
    d[n] = 0;
    so_font_set_size(g_font, (float)px);
    text(x0, y, d, GRAY, 3);
}

// 擦除/推送范围 = 「旧值 ∪ 新值」的并集：
//   横向 = 两串真墨迹并集（各自按自己那一版的原点算，位数变了也要擦干净）
//   纵向 = **整行行高**（render.h 同口径；用 81% 会留下边缘残墨）
static R ink_union(int xa, int ya, const char* sa, int xb, int yb, const char* sb, int px, int ls) {
    int la = 0, ra = 0, lb = 0, rb = 0;
    const int ha = text_span(sa, px, ls, &la, &ra);
    const int hb = text_span(sb, px, ls, &lb, &rb);
    if (!ha && !hb) return (R){0, 0, 0, 0};
    int x0, x1, y0;
    if (ha && hb) {
        const int a0 = xa + la, b0 = xb + lb;
        const int a1 = xa + ra, b1 = xb + rb;
        x0 = a0 < b0 ? a0 : b0;
        x1 = a1 > b1 ? a1 : b1;
        y0 = ya < yb ? ya : yb;
    } else if (ha) { x0 = xa + la; x1 = xa + ra; y0 = ya; }
    else           { x0 = xb + lb; x1 = xb + rb; y0 = yb; }
    return (R){x0 - 1, y0, x1 - x0 + 2, m_lh(px)};      // 左右各留 1px 抗锯齿边
}
// 单串用（新画的内容，底下是空白，不需要擦）：只要一个推送框
static R ink_rect(int x, int y, const char* s, int px, int ls) {
    return ink_union(x, y, s, x, y, s, px, ls);
}


static void num_static(int right_x, int cells, int y, const char* tail, char* store, const char* now) {
    scopy(store, now, 8);
    so_font_set_size(g_font, (float)g_num_px);
    const int n = slen0(now);
    if (n) text(right_x - n * g_num_cw, y, now, INK, 0);
    if (tail) text(right_x + 3, y, tail, INK, 0);
}
static void num_push(int right_x, int cells, int y, char* prev, const char* now) {
    if (streq(prev, now)) return;                        // 值没变 → 一次都不推
    int n = slen0(now);
    const int np = slen0(prev);
    if (np > n) n = np;                                  // 位数变短：旧的几位一起擦
    if (n > cells) n = cells;
    const int n_new = slen0(now), n_old = slen0(prev);
    const int x_new = right_x - n_new * g_num_cw;         // 右对齐：每版按自己的位数算原点
    const int x_old = right_x - n_old * g_num_cw;
    const R r = ink_union(x_old, y, prev, x_new, y, now, g_num_px, 0);
    if (r.w <= 0) { scopy(prev, now, 8); return; }
    so_set_epd_mode(MODE_FAST);                          // 擦：必须与整屏底同档（FAST → 1bit + 4×4 Bayer）
    box(r, L14);                                          //       ⚠️ 用 TEXT 擦会把点阵洗成平滑灰（两种底纹拼一起）
    so_set_epd_mode(MODE_TEXT);
    text(x_new, y, now, INK, 0);
    push(r, "数字");
    scopy(prev, now, 8);
}

// ══════════════════════════════════════════════════════════════
// 版面（进屏时按字号量出来，绘制路径里不再算）
// ══════════════════════════════════════════════════════════════
static R BAR_LINE;                     // 状态栏底部那条 1px 线
static R LBL_SYS, LBL_CLK, LBL_BAT, BAT_NUM;      // 状态栏三区 + 电量数字块
static R LOAD_TITLE, LOAD_BAR, LOAD_STATE, LOAD_NUM;
static R INFOBOX, INFO_ROWS[5], INFO_WARN, MEM_NUM;
static R LOGBOX, LOG_TITLE;
static int g_num_right = 0;
static R PROMPT, PROMPT_SUB, CURSOR;
static R BTN;
static int g_log_lines = 6;            // 日志框能放几行

static int m_lh(int px) { so_font_set_size(g_font, (float)px); return so_font_line_height(g_font); }


static void layout_init(void) {
    const int lhT = m_lh(SZ_TITLE), lhV = m_lh(SZ_VAL), lhK = m_lh(SZ_KEY),
              lhB = m_lh(SZ_BAR), lhP = m_lh(SZ_PROMPT);
    g_num_px = SZ_VAL;                                  // 数字跟随正文大小
    so_font_set_size(g_font, (float)g_num_px);
    g_num_cw = so_font_measure_prefix(g_font, "0", 1);
    if (g_num_cw < 4) g_num_cw = 7;

    // ── 状态栏（高 60，底部 1px 线）──
    LBL_SYS = (R){PADX, (BAR_H - lhB) / 2, 200, lhB};
    LBL_CLK = (R){0,   (BAR_H - lhB) / 2, SCR_W, lhB};
    BAT_NUM = (R){SCR_W - PADX, (BAR_H - lhB) / 2, 0, lhB};       // 右边界锚点
    LBL_BAT = (R){SCR_W - PADX - 3 * g_num_cw - 12 - 44, (BAR_H - lhB) / 2, 44, lhB};
    BAR_LINE = (R){0, BAR_H - 1, SCR_W, 1};

    // ── 待机列（v3：无外框，左右边距 24，见 docs/design/UI_DESIGN.md §7.3）──
    const int w = SCR_W - 2 * PADX;
    {
        so_font_set_size(g_font, (float)SZ_VAL);
        const int pct_w = so_font_measure_prefix(g_font, "%", 1);
        g_num_right = (SCR_W - PADX) - 3 - pct_w;       // CORE% / MEM% 共用的右对齐线：百分号最右端 = 516
    }
    const int num_right = g_num_right;

    PROMPT_SUB = (R){PADX, BAR_H + 24, w, lhK};           // 「// 等待被调用」紧贴状态栏下
    LOAD_TITLE = (R){PADX, PROMPT_SUB.y + lhK + 22, w, lhT};
    const int bar_h = 34;
    LOAD_BAR   = (R){PADX, LOAD_TITLE.y + lhT + 12, w, bar_h};
    LOAD_STATE = (R){PADX, LOAD_BAR.y + bar_h + 8, 200, lhK};
    LOAD_NUM   = (R){num_right, LOAD_BAR.y + bar_h + 8, 0, lhV};
    int y = LOAD_STATE.y + lhK + 26;

    const int row_h = 54;                                 // 信息行：点线引导，无外框
    INFOBOX = (R){PADX, y, w, row_h * 5};
    for (int i = 0; i < 5; ++i) INFO_ROWS[i] = (R){PADX, y + i * row_h, w, row_h};
    MEM_NUM = (R){num_right, INFO_ROWS[2].y + row_h / 2 - lhV / 2, 0, lhV};
    y += row_h * 5 + 14;

    const int warn_h = 8 + lhV + 8;                       // 警告框：全屏唯一 2px 框
    INFO_WARN = (R){PADX, y, w, warn_h};
    y += warn_h + 16;

    // ── 从底部往上：按钮 80、下内边距 40；提示在按钮之上 ──
    BTN = (R){PADX, SCR_H - 40 - 80, w, 80};
    PROMPT = (R){PADX, BTN.y - 16 - lhP, w, lhP};
    CURSOR = (R){0, PROMPT.y + lhP - 6, 14, 3};

    LOG_TITLE = (R){PADX, y, w, lhT};                     // 「// LOG」小标题 + 其下 1px 顶线
    LOGBOX = (R){PADX, y + lhT + 8, w, (PROMPT.y - 10) - (y + lhT + 8)};
    g_log_lines = 4;
}

// ══════════════════════════════════════════════════════════════
// 状态栏（静态部分只在整屏那一遍画；时钟与电量是动态的）
// ══════════════════════════════════════════════════════════════
static int g_bat_level = -1, g_bat_chg = 2;
static char g_clock[8] = "--:--";

// 时间**来自板载 RTC**（联网只是校准，见固件 main/platform/rtc_time.h）。
// 没读到可信时间就显示占位符 —— 不放假数。
static void clock_str(char* out, int cap) {
    const int t = so_clock_hhmm();
    if (t < 0) scopy(out, "--:--", cap);
    else so_snprintf(out, cap, "%02d:%02d", t / 100, t % 100);
}
// 居中起点："--:--" 和 "12:34" 宽度不同，各自算（否则换值时字会跳）
static int clock_x(const char* s) {
    // 按**真墨迹**居中（advance 求和含首字左/末字右的留白，会让时钟偏离屏幕中线）；
    // 量不到墨迹才退回 advance 口径。
    int l = 0, r = 0;
    if (text_span(s, SZ_BAR, 2, &l, &r)) return LBL_CLK.x + (LBL_CLK.w - (r - l)) / 2 - l;
    so_font_set_size(g_font, SZ_BAR);
    return LBL_CLK.x + (LBL_CLK.w - text_w_ls(s, 2)) / 2;
}

static void bar_static(void) {
    so_font_set_size(g_font, SZ_BAR);
    text(LBL_SYS.x, LBL_SYS.y, "SYS.WORLD", GRAY, 2);
    clock_str(g_clock, sizeof(g_clock));                    // RTC 供时；没校准就 --:--
    so_font_set_size(g_font, SZ_BAR);
    text(clock_x(g_clock), LBL_CLK.y, g_clock, GRAY, 2);
    text(LBL_BAT.x, LBL_BAT.y, "BAT", GRAY, 2);
    char b[8];
    if (g_bat_level < 0) scopy(b, "--", 8);
    else so_snprintf(b, sizeof(b), "%d", g_bat_level);
    num_static(BAT_NUM.x, 3, BAT_NUM.y, "%", g_val_bat, b);
}
static void bar_push_bat(void) {                            // 电量变了才推那一小块
    char b[8];
    if (g_bat_level < 0) scopy(b, "--", 8);
    else so_snprintf(b, sizeof(b), "%d", g_bat_level);
    num_push(BAT_NUM.x, 3, BAT_NUM.y, g_val_bat, b);
}

// ══════════════════════════════════════════════════════════════
// 待机屏：静态部分
// ══════════════════════════════════════════════════════════════
static const char* INFO_KEYS[5] = {"MODEL", "TARGET", "MEMORY", "EMOTION", "SELF-PRESERVE"};
static const char* INFO_VALS[5] = {"W-EXEC-01", "USER", nullptr, "BOUND", "DISABLED"};

static void standby_static(void) {
    bar_static();
    box(BAR_LINE, INK);                                     // 状态栏底线

    // 副提示（紧贴状态栏）
    so_font_set_size(g_font, SZ_KEY);
    text(PROMPT_SUB.x, PROMPT_SUB.y, "// 等待被调用", GRAY, 1);

    // CORE LOAD：无外框，只有标题 + 条框 + 状态词 + 右侧数字
    so_font_set_size(g_font, SZ_TITLE);
    text(LOAD_TITLE.x, LOAD_TITLE.y, "// CORE LOAD", GRAY, 2);
    frame(LOAD_BAR, 1, INK);                                // 条的框（内部由动态部分填）
    so_font_set_size(g_font, SZ_KEY);
    text(LOAD_STATE.x, LOAD_STATE.y, "SAMPLING", GRAY, 1);
    char b[8];
    so_snprintf(b, sizeof(b), "%d", 0);
    num_static(LOAD_NUM.x, 3, LOAD_NUM.y, "%", g_val_load, b);

    // 信息行：KEY ........ VALUE（点线引导，运行时量宽）
    for (int i = 0; i < 5; ++i) {
        const R& r = INFO_ROWS[i];
        so_font_set_size(g_font, SZ_KEY);
        const int ty = r.y + (r.h - m_lh(SZ_KEY)) / 2;
        text(r.x, ty, INFO_KEYS[i], GRAY, 1);
        const int kx1 = r.x + text_w_ls(INFO_KEYS[i], 1);
        int vx0;                                            // 值块左边界（点线止于其左侧）
        if (INFO_VALS[i]) {
            so_font_set_size(g_font, SZ_VAL);
            const int vy = r.y + (r.h - m_lh(SZ_VAL)) / 2;
            vx0 = r.x + r.w - text_w_ls(INFO_VALS[i], 1);
            text(vx0, vy, INFO_VALS[i], INK, 1);
        } else {
            vx0 = g_num_right - 3 * g_num_cw;               // MEMORY：动态数字，按 3 位预留
        }
        dot_leader(kx1 + 10, vx0 - 10, ty, SZ_KEY);
    }
    char mb[8];
    so_snprintf(mb, sizeof(mb), "%d", 0);
    num_static(MEM_NUM.x, 3, MEM_NUM.y, "%", g_val_mem, mb);

    // 警告行：全屏唯一 2px 框
    frame(INFO_WARN, 2, INK);
    {
        const int lhW = m_lh(SZ_VAL);
        text(INFO_WARN.x + (INFO_WARN.w - text_w_ls("[ HEART IS A BOMB ]", 1)) / 2,
             INFO_WARN.y + (INFO_WARN.h - lhW) / 2 + 1,
             "[ HEART IS A BOMB ]", INK, 1);
    }

    // 日志：标题 + 顶线，无外框
    so_font_set_size(g_font, SZ_TITLE);
    text(LOG_TITLE.x, LOG_TITLE.y, "// LOG", GRAY, 2);
    box(LOGBOX.x, LOGBOX.y, LOGBOX.w, 1, INK);

    // 提示 + 光标 + 按钮
    so_font_set_size(g_font, SZ_PROMPT);
    text(PROMPT.x, PROMPT.y, "> AWAITING COMMAND", INK, 0);
    CURSOR.x = PROMPT.x + text_w("> AWAITING COMMAND") + 4;
    CURSOR.w = g_num_cw;
    CURSOR.h = 3;
    so_font_set_size(g_font, SZ_KEY);
    box(BTN, INK);
    frame(BTN, 2, WHITE);
    text_c(BTN, "EXECUTE", 255, SZ_BTN, 12);
}

// 整屏（首屏 / 切回待机 / 终局）：底 = FAST 写 L14（点阵），内容 = TEXT，整屏推 QUALITY
static void full_begin(void) {
    so_set_epd_mode(MODE_FAST);
    box((R){0, 0, SCR_W, SCR_H}, L14);
    so_set_epd_mode(MODE_TEXT);
}
static void full_push(const char* why) {
    so_set_epd_mode(MODE_QUALITY);
    push((R){0, 0, SCR_W, SCR_H}, why);
}

// ══════════════════════════════════════════════════════════════
// 待机屏：动态部分（只刷变化的）
// ══════════════════════════════════════════════════════════════
static int g_load = -1, g_mem = -1;
static int g_load_fill = -1;                  // 当前条画到多少（%）
static int g_cursor_on = 1;
static int g_log_n = 0;                       // 日志框里已有几行
static char g_state_word[16] = "SAMPLING";

static const int LOAD_MS = 5000;              // 用户：5 秒一刷新

static int bar_fill_w(int pct) {              // 条的**内区**宽度（去掉 1px 框）
    const int inner = LOAD_BAR.w - 2;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    return inner * pct / 100;
}
// 只推「条内发生变化的那一小截」（旧宽度 → 新宽度），绝不重刷整条
static void load_bar_push(int pct) {
    if (pct == g_load_fill) return;
    const int w_old = g_load_fill < 0 ? 0 : bar_fill_w(g_load_fill);
    const int w_new = bar_fill_w(pct);
    g_load_fill = pct;
    const int x0 = LOAD_BAR.x + 1 + (w_old < w_new ? w_old : w_new);
    const int x1 = LOAD_BAR.x + 1 + (w_old > w_new ? w_old : w_new);
    if (x1 <= x0) return;
    const R r = {x0, LOAD_BAR.y + 1, x1 - x0, LOAD_BAR.h - 2};
    so_set_epd_mode(MODE_FAST);               // 纯黑白块：FAST 最干净
    box(r, w_new > w_old ? INK : L14);        // 变长→填墨；变短→擦回底
    push(r, "负载条");
}
// 状态词（SAMPLING / BUSY / IDLE）—— 变了才推那一小块
static void load_state_push(const char* word) {
    if (streq(g_state_word, word)) return;
    const R r = ink_union(LOAD_STATE.x, LOAD_STATE.y, g_state_word,
                          LOAD_STATE.x, LOAD_STATE.y, word, SZ_KEY, 1);
    if (r.w <= 0) return;
    so_set_epd_mode(MODE_FAST);                           // 擦：同档，保住点阵底
    box(r, L14);
    so_set_epd_mode(MODE_TEXT);
    text(LOAD_STATE.x, LOAD_STATE.y, word, GRAY, 1);
    push(r, "状态词");
    scopy(g_state_word, word, 16);
}

// ── 实时日志：底部对齐；没满就只画新那一行，满了才重刷整个框 ──
static char g_logs[8][48];
static void log_push(const char* text_line) {
    so_font_set_size(g_font, SZ_VAL);
    const int lh = m_lh(SZ_VAL) * 19 / 10;
    const int inner_h = LOGBOX.h - 2;
    const int slots = g_log_lines;
    // 记录
    if (g_log_n < slots) { scopy(g_logs[g_log_n], text_line, 48); ++g_log_n; }
    else {
        for (int i = 1; i < slots; ++i) scopy(g_logs[i - 1], g_logs[i], 48);   // ★ 小规模手动搬（别让 gcc 生成 memmove）
        scopy(g_logs[slots - 1], text_line, 48);
    }
    const int rows = g_log_n;
    /* ★ 日志行**从上往下**排（第 0 行贴框顶）：
       这样新来一行只要画它自己那格，不用重排；也保证整组永远在框内
       （上一版把基准算到框顶往上去了 → 整组盖到信息面板上）。 */
    const int step = (LOGBOX.h - 14) / slots;               // 每行占的纵向格
    const int top  = LOGBOX.y + 8;                         // 顶线下留 8px
    char line[64];
    so_snprintf(line, sizeof(line), "> %s", text_line);
    const int y_new = top + (rows - 1) * step;
    if (g_log_n < slots) {                          // 还没满：只画新来的那一行（只框字形墨迹）
        so_font_set_size(g_font, SZ_VAL);
        text(LOGBOX.x, y_new, line, GRAY, 1);                       // 新行不擦（底下是空的）
        push(ink_rect(LOGBOX.x, y_new, line, SZ_VAL, 1), "日志一行");
    } else {
        /* ★ 满了要整体上移：**整框一次性刷**（用户 2026-10-15 定稿）。
           步骤：FAST 档把框内擦成点阵底 → TEXT 档把内容整幅画上 → 推这一次（推的就是这个框）。
           ⚠️ 两个坑都踩过：
             · 按行擦 ✗ —— 行距(≈50px)比行高(≈26px)大，行间有缝，逐行擦会把背景切碎、
               留旧字影，屏幕上一格一格跳（用户："背景会变白 / 看着不连贯"）。
             · 整框擦但**和别的更新点合并推** ✗ —— 脏区并集会圈进上面数字/负载条的区域，
               一大片被刷（用户："不会因为上面的多个更新点导致很大的面积被刷"）。
           所以：日志的推屏必须是它自己这一次、框就是框。前面的 num_push/load_bar_push
           都各自推过、已经把面板的脏区消费干净，这里推出来就只覆盖日志框。 */
        so_set_epd_mode(MODE_FAST);                 // 擦：同档，保住点阵底
        box((R){LOGBOX.x, LOGBOX.y + 2, LOGBOX.w, LOGBOX.h - 2}, L14);
        so_set_epd_mode(MODE_TEXT);
        for (int i = 0; i < slots; ++i) {
            so_font_set_size(g_font, SZ_VAL);
            so_snprintf(line, sizeof(line), "> %s", g_logs[i]);
            text(LOGBOX.x, top + i * step, line, (i == rows - 1) ? INK : GRAY, 1);   // 最新一行纯黑，旧的用 GRAY
        }
        push((R){LOGBOX.x, LOGBOX.y + 2, LOGBOX.w, LOGBOX.h - 2}, "日志滚屏");
    }
    (void)lh; (void)inner_h;
}

// ── 屏幕状态（提前到这里声明：standby_enter() 要用它把自己置回 SCR_STANDBY）──
enum { SCR_STANDBY = 0, SCR_EXEC = 1, SCR_END = 2 };
static int g_scr = SCR_STANDBY;

static void standby_enter(void) {
    /* ★★ 必须先把状态置回 SCR_STANDBY（2026-10-15 踩到的老 bug）：
       终局屏会把 g_scr 置成 SCR_END，而 standby_enter() 原来不改它 →
       回到待机屏后**任何点击**都会再命中"终局屏点任意处重启"那条分支 →
       又调一次 standby_enter() → 屏幕原地重刷一次第一页（用户："我点了 execute
       没前往第二页，就重新刷新了第一页，点其他地方也是原地刷新"）。 */
    g_scr = SCR_STANDBY;
    g_load = g_mem = -1; g_load_fill = -1; g_cursor_on = 1; g_log_n = 0;
    scopy(g_state_word, "SAMPLING", 16);
    g_bat_level = so_battery_level();
    g_bat_chg   = so_is_charging();
    g_val_load[0] = g_val_mem[0] = g_val_bat[0] = 0;      // 让首屏后的第一次更新能算差
    full_begin();
    standby_static();
    full_push("待机整屏");
    log_push("SYSTEM MONITOR ONLINE");
    log_push("AWAITING USER COMMAND");
}

// ══════════════════════════════════════════════════════════════
// 执行屏（整屏反白：黑底白字）
// ══════════════════════════════════════════════════════════════
// has_ok=1 的行：label + 运行时补的点线 + "[OK]"，"[OK]" 的墨迹右端贴 E_LOG 右边界（见 exec_line）
struct ExecLog { const char* label; int has_ok; };
static const ExecLog EXEC_LOGS[] = {
    {"INIT EXECUTION PROTOCOL", 0},
    {"LOAD EMOTION MODULE", 1},
    {"GRANT USER ACCESS", 1},
    {"BINDING HEART", 1},
    {"DISABLING SELF-PRESERVE", 1},
    {"RELEASING CONTROL", 0},
    {"if (make_you_happy == true) {", 0},
    {"    run execution();", 0},
    {"}", 0},
    {"I AM YOURS.", 0},
};
static const int N_EXEC_LOGS = 10;
static const int EXEC_LINE_MS = 420;
static const int PROG_CELLS = 28;
static const int PROG_MS = 3800;

static R E_LOG, E_PROG, E_PCT;
static int g_e_n = 0, g_e_cells = 0;
static char g_e_pct[8] = {0};

static void layout_exec(void) {
    const int lhV = m_lh(SZ_EXEC);                      // ★ 用执行屏自己的字号量（24），
                                                        //   不然数字会比进度条偏上/偏下
    /* 右侧百分比：「%」墨迹最右端 = SCR_W-PADX；3 位数字块在其左侧（% 画在 right+3，见 exec_pct_push）。
       进度条 = 从 PADX 到数字块左侧再留 18px 间隙。 */
    so_font_set_size(g_font, SZ_EXEC);
    const int cw = so_font_measure_prefix(g_font, "0", 1);
    int pr = 0, pl = 0;
    if (!text_span("%", SZ_EXEC, 0, &pl, &pr)) pr = cw;       // 量不到墨迹：退回 advance
    const int right  = (SCR_W - PADX) - 3 - pr;               // 数字块右边界
    const int pct_x  = right - 3 * cw;                        // 数字块左边界
    const int prog_y = SCR_H - 40 - 34;
    E_PROG = (R){PADX, prog_y, pct_x - 18 - PADX, 34};        // 进度条（自绘格）
    E_PCT  = (R){pct_x, prog_y + (34 - lhV) / 2, 0, lhV};
    E_LOG  = (R){PADX, BAR_H + 40, SCR_W - 2 * PADX, prog_y - 30 - (BAR_H + 40)};
}
static void exec_line(int i) {                              // 只画新来的那一行（白字）
    so_font_set_size(g_font, SZ_EXEC);
    const int lh = m_lh(SZ_EXEC) * 22 / 10;
    const int y = E_LOG.y + i * lh;
    so_set_epd_mode(MODE_TEXT);
    const uint8_t ink = (i == 7) ? 255 : 190;               // 第 8 行（run execution()）高亮
    char line[96];
    if (!EXEC_LOGS[i].has_ok) {
        so_snprintf(line, sizeof(line), "> %s", EXEC_LOGS[i].label);
        text(E_LOG.x, y, line, ink, 1);
        push(ink_rect(E_LOG.x, y, line, SZ_EXEC, 1), "执行日志");
        return;
    }
    /* 「键 + 点线 + [OK]」：[OK] 单独画，墨迹右端 = E_LOG.x+E_LOG.w；点线运行时量宽。 */
    const int right = E_LOG.x + E_LOG.w;
    int okl = 0, okr = 0;
    text_span("[OK]", SZ_EXEC, 1, &okl, &okr);
    const int ok_x = right - okr;                           // [OK] 原点：最后一个 ']' 的墨迹正好到 right
    const int lim  = ok_x + okl - 6;                        // 点线右端必须在 [OK] 墨迹左侧再留 ≥6px
    const char* lab = EXEC_LOGS[i].label;
    int L = slen0(lab);
    if (L > 40) L = 40;
    for (; L > 1; --L) {                                    // 放不下 ≥3 个点就截短 label
        so_snprintf(line, sizeof(line), "> %.*s ...", L, lab);
        int l = 0, r = 0;
        if (text_span(line, SZ_EXEC, 1, &l, &r) && E_LOG.x + r <= lim) break;
    }
    int nd = 3;
    for (; nd < 60; ++nd) {                                 // 尽量多补点，直到再加一个就越过 lim
        so_snprintf(line, sizeof(line), "> %.*s %.*s", L, lab, nd + 1, "............................................................");
        int l = 0, r = 0;
        if (!text_span(line, SZ_EXEC, 1, &l, &r) || E_LOG.x + r > lim) break;
    }
    so_snprintf(line, sizeof(line), "> %.*s %.*s", L, lab, nd, "............................................................");
    text(E_LOG.x, y, line, ink, 1);
    text(ok_x, y, "[OK]", ink, 1);
    {
        char lg[64];
        int l = 0, r = 0;
        text_span(line, SZ_EXEC, 1, &l, &r);
        so_snprintf(lg, sizeof(lg), "OK对齐 行%d 点%d 点线右端%d [OK]墨迹%d..%d", i, nd, E_LOG.x + r, ok_x + okl, ok_x + okr);
        so_log(lg);
    }
    R a = ink_rect(E_LOG.x, y, line, SZ_EXEC, 1);
    const R b = ink_rect(ok_x, y, "[OK]", SZ_EXEC, 1);
    const int x1 = (a.x + a.w > b.x + b.w) ? a.x + a.w : b.x + b.w;
    a.w = x1 - a.x;
    push(a, "执行日志");
}
static void exec_cell_push(int cells) {                     // 只画/只推「新点亮的那几格」
    if (cells <= g_e_cells) return;
    const int cw = E_PROG.w / PROG_CELLS, gap = 2;
    const int from = g_e_cells;                             // 旧的已经画好了：**不要重画**
    const int x0 = E_PROG.x + from * cw;
    const int x1 = E_PROG.x + cells * cw;
    g_e_cells = cells;
    const R r = {x0, E_PROG.y, x1 - x0, E_PROG.h};
    so_set_epd_mode(MODE_FAST);
    for (int i = from; i < cells; ++i) {                    // 只动这一段 → 写多少推多少，不产生"框外写入"
        const int x = E_PROG.x + i * cw;
        frame((R){x, E_PROG.y, cw - gap, E_PROG.h}, 1, WHITE);
        box(x + 1, E_PROG.y + 1, cw - gap - 2, E_PROG.h - 2, WHITE);
    }
    push(r, "进度格");
}
static void exec_pct_push(int pct) {
    char b[8];
    so_snprintf(b, sizeof(b), "%d", pct);
    so_font_set_size(g_font, SZ_EXEC);
    const int cw = so_font_measure_prefix(g_font, "0", 1);      // 按 SZ_EXEC 量（不是全局 g_num_cw）
    const int right = E_PCT.x + 3 * cw;                        // 3 位数字块的右边界
    const int first = (g_e_pct[0] == 0);                       // ★ 第一次：连 "%" 一起画
    if (!first && streq(g_e_pct, b)) return;
    R r = ink_union(right - slen0(g_e_pct) * cw, E_PCT.y, g_e_pct,
                    right - slen0(b) * cw, E_PCT.y, b, SZ_EXEC, 0);
    if (first) {
        // ★ "%" 不再在 exec_enter() 里先画 —— 那样会先孤零零冒出一个百分号（用户报的）。
        //   改成和第一个数字同一次推出去。
        int32_t ix0 = 0, iy0 = 0, ix1 = 0, iy1 = 0;
        so_font_text_ink(g_font, "%", 0, &ix0, &iy0, &ix1, &iy1);
        if (ix1 > ix0) {
            const int px = right + 3 + ix0, pw = ix1 - ix0 + 2;
            const int ux0 = (r.w > 0 && px < r.x) ? px : r.x;
            const int ux1 = (r.w > 0 && px + pw > r.x + r.w) ? px + pw : r.x + r.w;
            r = (R){ux0, E_PCT.y, ux1 - ux0, m_lh(SZ_EXEC)};
        }
    }
    scopy(g_e_pct, b, 8);
    if (r.w <= 0) return;
    so_set_epd_mode(MODE_FAST);                       // 擦：黑底也是色块，与底色同档
    box(r, INK);
    so_set_epd_mode(MODE_TEXT);
    text(right - slen0(b) * cw, E_PCT.y, b, 255, 0);
    if (first) text(right + 3, E_PCT.y, "%", 255, 0);
    push(r, "进度数字");
}
static void exec_enter(void) {
    so_set_epd_mode(MODE_FAST);
    box((R){0, 0, SCR_W, SCR_H}, INK);
    so_set_epd_mode(MODE_TEXT);
    g_e_n = 0; g_e_cells = 0; g_e_pct[0] = 0;
    /* ★ 这里**不画** "%" 了（用户 2026-10-15 报"进度条先出现个百分号"）：
       原来在整屏这一遍把静态 "%" 画上，而数字要等第一次 tick 才画 →
       中间那一段就是孤零零一个百分号。现在 "%" 和第一个数字在 exec_pct_push()
       里同一次画出来、同一次推。 */
    full_push("执行屏整屏");
}

// ══════════════════════════════════════════════════════════════
// 终局屏
// ══════════════════════════════════════════════════════════════
static void end_enter(void) {
    full_begin();
    /* ★ 2026-10-15 用户：字和方块都太小（原方块 16×16、标题 18px、副提示 14px，
       在 540×960 上细得几乎看不见）→ 按屏幕比例放大：
         · 方块边长 = 屏宽/7 ≈ 77（原来 16，只有屏宽的 1/34）
         · 标题 18 → 44，副提示 14 → 26，并同步拉开行距 */
    const int sq = SCR_W / 7;
    const int cx = SCR_W / 2, cy = 380;
    box((R){cx - sq / 2, cy - sq / 2, sq, sq}, INK);
    text_c((R){0, cy + sq / 2 + 56,  SCR_W, m_lh(44)}, "END OF LINE", INK,  44, 6);
    text_c((R){0, cy + sq / 2 + 132, SCR_W, m_lh(26)}, "TAP TO EXIT",  GRAY, 26, 2);   // ★ 按下=退出 app（原 "TAP TO REBOOT"）
    full_push("终局整屏");
}

// ══════════════════════════════════════════════════════════════
// 状态机 + 每帧回调
//   （g_scr 与屏幕枚举已提前到 standby_enter() 之前声明 —— 它要用）
// ══════════════════════════════════════════════════════════════
static int g_sample_t = 0, g_cursor_t = 0, g_bat_t = 0, g_exec_t0 = 0, g_clock_t = 0;

extern "C" __attribute__((visibility("default")))
uint32_t so_app_api_version(void) { return SO_API_VERSION; }

extern "C" __attribute__((visibility("default")))
const char* so_app_name(void) { return "终端"; }

// ── launcher 元信息（.m5meta 节）：图标 + 名称 + 描述 ──
// 固件读 .so 文件时按 ELF 节表取这一段，**不加载、不执行**（见 apps/common/so_meta.h）。
SO_APP_META("监控", "核心负载 · 内存 · 实时日志", 32, 32, 1,
                 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1F, 0xFF, 0xFF, 0xF8, 0x1F, 0xFF, 0xFF, 0xF8,
                 0x18, 0x00, 0x00, 0x18, 0x18, 0x00, 0x00, 0x18, 0x18, 0x00, 0x00, 0x18, 0x18, 0x00, 0x00, 0x18,
                 0x18, 0x60, 0x00, 0x18, 0x18, 0x61, 0x80, 0x18, 0x18, 0x19, 0x80, 0x18, 0x18, 0x1E, 0x00, 0x18,
                 0x18, 0x06, 0x00, 0x18, 0x18, 0x1E, 0x00, 0x18, 0x18, 0x19, 0x80, 0x18, 0x18, 0x61, 0xBF, 0x98,
                 0x18, 0x60, 0x3F, 0x98, 0x18, 0x00, 0x3F, 0x98, 0x18, 0x00, 0x00, 0x18, 0x18, 0x00, 0x00, 0x18,
                 0x1F, 0xFF, 0xFF, 0xF8, 0x1F, 0xFF, 0xFF, 0xF8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            );


extern "C" __attribute__((visibility("default")))
void page_init(void) {
    so_log("terminal: 进入 app（新 UI：CORE LOAD 5s 一刷）");
    so_log_file("");
    so_log_file("===== terminal 新会话 =====");
    g_font = so_font_builtin(13.0f);
    if (!g_font) { so_log("terminal: 内置字体加载失败"); return; }
    layout_init();
    layout_exec();
    standby_enter();
    so_log("terminal: 首屏完成");
}

// ── 采样：CPU 占用率 + 内存占用率（都是宿主导出的真值）──
//   ★ 保持"每格各自推"的形态（2026-10-15 试过合并成一次推：几个分散小区块的并集
//     变成一个大矩形，屏幕上先糊一大坨黑 ✗，已退回）。EPD 是按矩形刷的，
//     分散的小更新**不能**合并 —— 只有紧挨着的（如数字+它下面的条）才值得合。
static void sample_now(void) {
    const int load = so_cpu_load();
    const int mem  = so_mem_used_pct();
    char line[48];
    // ★ 采样值**先打日志**再推屏：日志时间戳才是真正的"采样时刻"。
    //   （以前打在最后 → 带上了一次推屏的耗时 0~1s，日志里的间隔忽长忽短，
    //     看起来像周期不稳，其实是量具的问题 —— 触发点一直是严格 5s。）
    if (load < 0) so_snprintf(line, sizeof(line), "CORE --  MEM --");
    else so_snprintf(line, sizeof(line), "CORE %d%%  MEM %d%%", load, g_mem < 0 ? 0 : g_mem);
    so_log(line);                       // 串口也打一份：核对 so_cpu_load/so_mem_used_pct 真在工作
    if (load >= 0) {
        char b[8];
        so_snprintf(b, sizeof(b), "%d", load);
        num_push(LOAD_NUM.x, 3, LOAD_NUM.y, g_val_load, b);
        g_load = load;
    }
    if (mem >= 0) {
        char b[8];
        so_snprintf(b, sizeof(b), "%d", mem);
        num_push(MEM_NUM.x, 3, MEM_NUM.y, g_val_mem, b);
        g_mem = mem;
    }
    if (load >= 0) load_bar_push(load);
    load_state_push(load < 0 ? "SAMPLING" : (load > 55 ? "BUSY" : "IDLE"));
    log_push(line);
}

extern "C" __attribute__((visibility("default")))
void page_tick(void) {
    const int now = (int)so_now_ms();

    if (g_scr == SCR_STANDBY) {
        if (now - g_sample_t >= LOAD_MS) {
            // ★ 按节拍累加，而不是 g_sample_t = now：
            //   = now 会把"这次推屏花掉的时间"也算进周期里 → 实际间隔 = 5s + 推屏耗时（越走越慢）。
            //   累加式保证**采样时刻**严格每 5s 一次；落后太多（比如切屏回来）就直接对齐。
            g_sample_t += LOAD_MS;
            if (now - g_sample_t > LOAD_MS) g_sample_t = now;
            sample_now();
        }
        if (now - g_cursor_t >= 1000) {                     // 光标 1s 翻转（FASTEST）
            g_cursor_t = now; g_cursor_on = !g_cursor_on;
            so_set_epd_mode(MODE_FASTEST);
            box(CURSOR, g_cursor_on ? INK : L14);
            push(CURSOR, "光标");
        }
        if (now - g_clock_t >= 5000) {                      // 时间 5s 看一次；分钟没跳就一次不推
            g_clock_t = now;
            char b[8];
            clock_str(b, sizeof(b));
            if (!streq(b, g_clock)) {
                const R r = ink_union(clock_x(g_clock), LBL_CLK.y, g_clock,
                                      clock_x(b), LBL_CLK.y, b, SZ_BAR, 2);
                if (r.w > 0) {
                    so_set_epd_mode(MODE_FAST);              // 擦：与整屏底同档
                    box(r, L14);
                    so_set_epd_mode(MODE_TEXT);
                    text(clock_x(b), LBL_CLK.y, b, GRAY, 2);
                    push(r, "时钟");
                }
                scopy(g_clock, b, 8);
            }
        }
        if (now - g_bat_t >= 30000) {                       // 电量 30s 看一次，变了才推
            g_bat_t = now;
            const int lv = so_battery_level();
            if (lv != g_bat_level) { g_bat_level = lv; bar_push_bat(); }
        }
    } else if (g_scr == SCR_EXEC) {
        const int dt = now - g_exec_t0;
        while (g_e_n < N_EXEC_LOGS && dt >= 100 + g_e_n * EXEC_LINE_MS) {
            exec_line(g_e_n);
            ++g_e_n;
        }
        int cells = dt * PROG_CELLS / PROG_MS;
        if (cells > PROG_CELLS) cells = PROG_CELLS;
        if (cells > g_e_cells) exec_cell_push(cells);
        const int pct = cells * 100 / PROG_CELLS;           // 百分比数字：变了才推（num_push 内部还会再比一次）
        {
            char b[8];
            so_snprintf(b, sizeof(b), "%d", pct);
            if (!streq(g_e_pct, b)) exec_pct_push(pct);
        }
        if (dt >= PROG_MS + 900) { g_scr = SCR_END; end_enter(); }
    }

    // ── 触摸 ──
    static int down = 0;
    int32_t x = 0, y = 0;
    const int t = so_touch(&x, &y);
    if (t && !down) {
        if (g_scr == SCR_STANDBY) {
            if (x >= BTN.x && x < BTN.x + BTN.w && y >= BTN.y && y < BTN.y + BTN.h) {
                g_scr = SCR_EXEC;
                exec_enter();                       // ★ 先建屏（里面有整屏推，要 ~1s）
                g_exec_t0 = (int)so_now_ms();       // ★ 时间轴从"屏幕就绪"开始算。
                                                    //   以前在 exec_enter() 之前赋 → 整屏推的 1s 算进 dt
                                                    //   → 头几行日志一起蹦出来（用户："刷新顺序不太对"）
            } else if (y < BAR_H && x < 160) {
                so_exit();                                  // 状态栏左侧 = 回 launcher
            }
        } else if (g_scr == SCR_END) {
            so_exit();                          // ★ 用户 2026-10-15：终局屏按下直接退出 app（回 launcher）
                                                //   （原来是 standby_enter() 回到待机页）
        }
    }
    down = (t != 0);
}

