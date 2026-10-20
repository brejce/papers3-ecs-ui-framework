// apps/reader/main/reader.cpp — 阅读器 .so：入口 + 开书 + 每帧交互 + 空闲干活
//
// 这一版重写了三件事（见 docs/reader/READER_TXT_RULES.md 与 docs/design/UI_DESIGN.md §7.4）：
//   1. **窗口化读大书**：一次只把当前位置起 512KB 读进 PSRAM（旧版读满 512KB 就没有下一页了），
//      页起点用文件字节偏移记身份 → 目录跳章 / 阅读进度 / 历史续读都成立（layout.h）；
//   2. **界面按终端风 v3 重做**：点阵底 + 状态栏 + hero + // 小标题 + 点线行 + 列表项（ui.h / pages.h）；
//   3. **阅读器自己的设置页**：正文 字号 / 灰度 / 字体（pages.h 的 MODE_CFG，存 /sdcard/apps/reader.cfg）。
//
// 每帧只做该做的：按下沿处理一次点击；空闲先预取下一页，再推一章扫描（目录索引）。
#include <cstring>
#include "../common/so_api.h"
#include "../common/so_meta.h"
#include "pages.h"

using namespace app;
using namespace app::rd;

namespace app::rd {

// ══════════════════════════════════════════════════════════════
//  字体
// ══════════════════════════════════════════════════════════════
// SD 字体优先，坏了/没有就回退内置（flash fonts 分区 mmap，零 PSRAM、毫秒级）。
// ★ 没有这层兜底时：SD 里一个 .ttf 都没有 → 空句柄 → 所有 so_font_* 都是 `if (!h) return 0`
//   → 不崩、不报错、屏上一片空白，只是字没了。
// 返回 false = 用的是内置（调用方决定要不要清掉 cfg 里的记录）
bool reload_font() {
    void* f = nullptr;
    bool sd_ok = false;
    if (g_face[0]) { f = so_font_load(g_face, (float)g_size); sd_ok = (f != nullptr); }
    if (!f) f = so_font_builtin((float)g_size);
    if (!f) return false;
    if (ui::g_font) so_font_free(ui::g_font);
    ui::g_font = f;
    so_font_set_size(ui::g_font, (float)g_size);
    char b[128];
    so_snprintf(b, sizeof(b), "字体：%s @%dpx（SD 字体 %d 个）",
                g_face[0] ? g_face : "内置", g_size, g_nfonts);
    slog(b);
    return (!g_face[0]) || sd_ok;
}

// ══════════════════════════════════════════════════════════════
//  分章索引（目录 / 页脚章名 / 章前断页都靠它）
// ══════════════════════════════════════════════════════════════
void chap_resume_current() {
    if (!g_name[0] || g_fsize == 0) return;
    chap::start(g_name, (int)g_fsize);
}

// 结果页的文案（索引小的很，正文标题按需从 SD 读）
void build_chap_result() {
    const int n = chap::n_chaps();
    so_snprintf(g_chap_sum1, sizeof(g_chap_sum1), "卷%d 章%d 节%d 特殊%d",
                chap::lv(1), chap::lv(2), chap::lv(3), chap::lv(4));
    so_snprintf(g_chap_sum2, sizeof(g_chap_sum2), "索引 %d 项 / %d KB · 耗时 %lldms · 存在 /apps",
                n, chap::idx_kb(), (long long)chap::elapsed());

    int m = n < 20 ? n : 20;
    for (int i = 0; i < m; ++i) {
        char t[96];
        if (!chap::read_title(i, t, sizeof(t))) t[0] = 0;
        so_snprintf(g_chap_pool[i], sizeof(g_chap_pool[i]), "L%d  %s", chap::title_level(i), t);
        g_chap_lines[i] = g_chap_pool[i];
    }
    for (int i = m; i < 20; ++i) g_chap_lines[i] = nullptr;
    if (m == 0) {                                  // 闸门判"不分章"时也要有话说
        so_snprintf(g_chap_pool[0], sizeof(g_chap_pool[0]), "L0  未识别到章节（已记为空索引，免得每次重扫）");
        g_chap_lines[0] = g_chap_pool[0];
    }
}

void chap_begin(const char* book) {
    if (!book || !book[0]) return;
    ui::scopy(g_chap_book, book, sizeof(g_chap_book));
    const int sz = so_book_size(g_chap_book);
    if (sz <= 0) { ui::scopy(g_list_log, "这本书读不到大小", sizeof(g_list_log)); show_list(); return; }
    chap::start(g_chap_book, sz);
    g_chap_shown_pct = -1;
    if (chap::running()) {
        g_chap_sub = 1;
        show_chap_progress();
    } else {
        build_chap_result();
        g_chap_sub = 2;
        show_chap_result();
    }
}

// ══════════════════════════════════════════════════════════════
//  开书 / 续读
// ══════════════════════════════════════════════════════════════
bool open_book_by_name(const char* name) {
    if (!name || !name[0]) return false;
    const int64_t t0 = so_now_ms();

    const int sz = so_book_size(name);
    if (sz <= 0) { slog("开书失败：读不到文件大小"); return false; }
    ui::scopy(g_name, name, sizeof(g_name));
    g_fsize = (uint32_t)sz;
    const char* dot = strrchr(g_name, '.');
    g_md = dot && app::my_strncmp(dot, ".md", 3) == 0;

    // 续读：偏移 + 文件大小都对得上才认（书换了 / 改过 → 从头来）
    uint32_t off = 0, psz = 0;
    bool resumed = false;
    if (prog_get(g_name, &off, &psz) && psz == g_fsize && off > 0 && off + 64 < g_fsize) {
        resumed = true;
    } else {
        off = 0;
    }

    pages_reset(off, 0);                       // 页表从续读位置重新起（同时清页缓存）
    const int64_t t1 = so_now_ms();
    if (!win_load_at(off)) {
        slog("开书失败：窗口装载不出来（文件坏了？）");
        return false;
    }
    const int64_t t2 = so_now_ms();

    chap_resume_current();                     // 有 SD 索引 → 立刻可用；没有 → 后台扫
    g_popup = false;
    g_chap_sub = 0;
    g_toc_top = 0;
    show_read(0);
    prog_touch(false);

    const int64_t t3 = so_now_ms();
    char b[224];
    so_snprintf(b, sizeof(b),
                "开书 %s: %dKB/%s | 立页表%lldms | 装窗口%lldms | 首屏%lldms | 合计%lldms",
                g_name, sz / 1024, resumed ? "续读" : "从头",
                (long long)(t1 - t0), (long long)(t2 - t1), (long long)(t3 - t2),
                (long long)(t3 - t0));
    slog(b);
    return true;
}

bool open_book(int i) {
    if (i < 0 || i >= g_nbooks) return false;
    return open_book_by_name(g_books[i]);
}

} // namespace app::rd

// ══════════════════════════════════════════════════════════════
//  给固件的清单符号 + launcher 元信息
// ══════════════════════════════════════════════════════════════
extern "C" __attribute__((visibility("default")))
uint32_t so_app_api_version(void) { return SO_API_VERSION; }

extern "C" __attribute__((visibility("default")))
const char* so_app_name(void) { return "阅读"; }

SO_APP_META("阅读", "阅读 · txt / md · 目录 · 进度", 32, 32, 1,
                 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                 0x00, 0x00, 0x00, 0x00, 0x1F, 0xFF, 0xFF, 0xF8, 0x1F, 0xFF, 0xFF, 0xF8, 0x18, 0x07, 0xE0, 0x18,
                 0x18, 0x07, 0xE0, 0x18, 0x1B, 0xFF, 0xFF, 0xD8, 0x1B, 0xFF, 0xFF, 0xD8, 0x18, 0x07, 0xE0, 0x18,
                 0x18, 0x07, 0xE0, 0x18, 0x1B, 0xFF, 0xFF, 0xD8, 0x1B, 0xFF, 0xFF, 0xD8, 0x18, 0x07, 0xE0, 0x18,
                 0x18, 0x07, 0xE0, 0x18, 0x1B, 0xFF, 0xFF, 0xD8, 0x1B, 0xFF, 0xFF, 0xD8, 0x18, 0x07, 0xE0, 0x18,
                 0x1F, 0xFF, 0xFF, 0xF8, 0x1F, 0xFF, 0xFF, 0xF8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            );

// ══════════════════════════════════════════════════════════════
//  入口
// ══════════════════════════════════════════════════════════════
// ★ 宿主**不重复 dlopen** 同名模块：退出再进来，还是同一个实例、静态量照旧。
//   所以 page_init 必须把上一次会话的状态清干净（弹窗、扫描页、页表、字体句柄…）。
static bool g_touch_at_init = false;

extern "C" __attribute__((visibility("default")))
void page_init(void) {
    slog("===== 阅读器 新会话 =====");

    // 状态归零
    g_popup = false; g_popup_n = 0;
    g_chap_sub = 0; g_chap_book[0] = 0;
    g_toc_top = 0; g_toc_cur = -1;
    g_back_to = MODE_LIST;
    g_mode = MODE_LIST;
    g_list_log[0] = 0;
    g_win_off = 0; g_win_len = 0; g_win_eof = true;
    g_touch_at_init = (so_touch_down() != 0);      // 装载时就按着的那只手不算点击
    g_popup = false;

    cfg_load();
    prog_load();
    fonts_scan();
    if (!reload_font()) {                          // SD 字体读不出来 → 回退内置并清掉记录
        slog("cfg 里的字体加载失败 → 回退内置并清掉记录");
        g_face[0] = 0;
        cfg_save();
        reload_font();
    }

    pc_init();
    pc_clear();
    pages_reset(0, 0);                             // 页表（PSRAM，按需翻倍）也从 0 起建好
    g_nbooks = so_list_books(g_books, 8);

    char b[200];
    so_snprintf(b, sizeof(b), "就绪：内部余%dKB PSRAM余%dKB · %d 本书 · 字号%d 灰度%d 字体%s · 进度%d 本",
                so_mem_free_kb(0), so_mem_free_kb(1),
                g_nbooks, g_size, g_ink, g_face[0] ? g_face : "内置", g_nprog);
    slog(b);
    show_list();
}

// ══════════════════════════════════════════════════════════════
//  每帧回调
// ══════════════════════════════════════════════════════════════
static bool g_scan_prev = false;

extern "C" __attribute__((visibility("default")))
void page_tick(void) {
    static bool down = false;
    int32_t x = 0, y = 0;
    const int t = so_touch(&x, &y);

    if (t && !down && !g_touch_at_init) {          // 按下沿：一次点击只触发一次
        char tl[64];
        so_snprintf(tl, sizeof(tl), "点击 (%ld,%ld) mode=%d", (long)x, (long)y, g_mode);
        slog(tl);
        switch (g_mode) {
            case MODE_LIST: tick_list(x, y); break;
            case MODE_READ: tick_read(x, y); break;
            case MODE_TOC:  tick_toc(x, y);  break;
            case MODE_CFG:  tick_cfg(x, y);  break;
            default:        tick_chap(x, y); break;
        }
    }
    down = (t != 0);
    g_touch_at_init = false;                       // 只屏蔽装载那一瞬

    // ── 空闲（没触摸）──
    if (down) return;

    const bool scanning = chap::running();
    if (scanning) {
        chap::step();                              // 目录索引：一个 tick 扫一块，不抢输入
        if (g_chap_sub == 1) update_chap_progress();
        if (!chap::running()) {                    // 刚扫完
            if (g_chap_sub == 1) { build_chap_result(); show_chap_result(); }
            else if (g_mode == MODE_TOC) show_toc();
            else if (g_mode == MODE_READ) footer_refresh();
        }
    } else if (g_mode == MODE_READ) {
        pc_prefetch_step();                        // 预取下一页（优先于别的活）
    }
    g_scan_prev = scanning;
    (void)g_scan_prev;
}
