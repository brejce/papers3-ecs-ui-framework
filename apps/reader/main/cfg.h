// apps/reader/main/cfg.h — 阅读器自己的设置 + 阅读进度（都落在 /sdcard/apps/）
//
// 两个文件、两件不同的事，别混：
//   reader.cfg            显示配置（字号 / 灰度 / 字体）—— 走宿主固定的 so_cfg_load/save
//   reader_progress.cfg   每本书读到哪 —— 走通用 so_blob_load/save
//
// ★ 进度为什么存**文件字节偏移**而不是页码：页码会随字号/窗口变，字节偏移不会。
//   偏移 = 当前页第一块的文件起始位置（见 layout.h 的 PageStart）。
// ★ 为什么不用时间戳排序：so_clock_hhmm() 只有"时分"，留不住"哪本更近"。
//   文件里**行的顺序就是最近顺序**（第一行 = 最近读的那本）。
#pragma once
#include <cstdint>
#include <cstring>
#include "ui.h"
#include "../common/so_api.h"

namespace app::rd {

// ══════════════════════════════════════════════════════════════
//  一、显示配置（/sdcard/apps/reader.cfg）
// ══════════════════════════════════════════════════════════════
// 行式 `KEY VALUE`，未知 KEY 忽略（向前兼容）
inline int  g_size = 26;          // 正文字号 px（18..40）；标题按比例缩放
inline int  g_ink  = 0;           // 正文灰度（亮度档 0 浓 / 55 中 / 100 淡）
inline char g_face[80] = {0};     // SD 字体文件名（空 = 内置字体）

inline const int SZ_MIN = 18, SZ_MAX = 40;
inline const int INK_LEVELS[3] = {0, 55, 100};

inline int atoi_s(const char* s, int def) {
    int v = 0; bool any = false;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); any = true; ++s; }
    return any ? v : def;
}
inline int clamp_i(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

inline void cfg_load() {
    char buf[512];
    const int got = so_cfg_load(buf, sizeof(buf));
    if (got <= 0) return;
    buf[got < (int)sizeof(buf) ? got : (int)sizeof(buf) - 1] = 0;
    char* p = buf;
    while (*p) {
        char* e = p;
        while (*e && *e != '\n' && *e != '\r') e++;
        const char nl = *e;
        *e = 0;
        char* sp = p;
        while (*sp && *sp != ' ') sp++;
        if (*sp) {
            *sp = 0;
            const char* val = sp + 1;
            while (*val == ' ') val++;
            if (ui::streq(p, "size"))      g_size = clamp_i(atoi_s(val, 26), SZ_MIN, SZ_MAX);
            else if (ui::streq(p, "ink"))  g_ink  = clamp_i(atoi_s(val, 0), 0, 160);
            else if (ui::streq(p, "face")) ui::scopy(g_face, val, sizeof(g_face));
        }
        if (!nl) break;
        p = e + 1;
    }
}

inline void cfg_save() {
    char buf[192];
    const int n = so_snprintf(buf, sizeof(buf), "size %d\nink %d\nface %s\n", g_size, g_ink, g_face);
    so_cfg_save(buf, n);
}

// ══════════════════════════════════════════════════════════════
//  二、阅读进度（/sdcard/apps/reader_progress.cfg）
// ══════════════════════════════════════════════════════════════
// 每行一本：`书名<TAB>偏移<TAB>文件大小`，**顺序 = 最近顺序**（第一行最近）
inline const int PROG_MAX = 8;
struct Prog { char name[96]; uint32_t off; uint32_t fsize; };
inline Prog g_prog[PROG_MAX];
inline int  g_nprog = 0;
inline bool g_prog_dirty = false;

inline void prog_load() {
    char buf[2048];
    g_nprog = 0; g_prog_dirty = false;
    const int got = so_blob_load("reader_progress.cfg", buf, sizeof(buf));
    if (got <= 0) return;
    buf[got < (int)sizeof(buf) ? got : (int)sizeof(buf) - 1] = 0;
    char* p = buf;
    while (*p && g_nprog < PROG_MAX) {
        char* e = p;
        while (*e && *e != '\n' && *e != '\r') e++;
        const char nl = *e;
        *e = 0;
        // name \t off \t fsize
        char* t1 = p;
        while (*t1 && *t1 != '\t') t1++;
        if (*t1) {
            *t1 = 0;
            char* t2 = t1 + 1;
            while (*t2 && *t2 != '\t') t2++;
            if (*t2) {
                *t2 = 0;
                Prog& q = g_prog[g_nprog];
                ui::scopy(q.name, p, sizeof(q.name));
                q.off   = (uint32_t)atoi_s(t1 + 1, 0);
                q.fsize = (uint32_t)atoi_s(t2 + 1, 0);
                if (q.name[0]) g_nprog++;
            }
        }
        if (!nl) break;
        p = e + 1;
    }
}

inline int prog_find(const char* name) {
    for (int i = 0; i < g_nprog; i++)
        if (ui::streq(g_prog[i].name, name)) return i;
    return -1;
}

inline bool prog_get(const char* name, uint32_t* off, uint32_t* fsize) {
    const int i = prog_find(name);
    if (i < 0) return false;
    if (off)    *off    = g_prog[i].off;
    if (fsize)  *fsize  = g_prog[i].fsize;
    return true;
}
// 0..100；没记录 = -1
inline int prog_pct(const char* name) {
    const int i = prog_find(name);
    if (i < 0 || g_prog[i].fsize == 0) return -1;
    uint32_t off = g_prog[i].off;
    if (off > g_prog[i].fsize) off = g_prog[i].fsize;
    return (int)(off * 100u / g_prog[i].fsize);
}

// 记一本：**移到第一行**（最近），超容量丢最后一行
inline void prog_set(const char* name, uint32_t off, uint32_t fsize) {
    if (!name || !name[0]) return;
    int i = prog_find(name);
    Prog t;
    if (i >= 0) { t = g_prog[i]; }
    else {
        ui::scopy(t.name, name, sizeof(t.name));
        if (g_nprog < PROG_MAX) g_nprog++;
    }
    t.off = off; t.fsize = fsize;
    if (i < 0) i = g_nprog - 1;              // 新书先占最后一格
    for (int k = i; k > 0; k--) g_prog[k] = g_prog[k - 1];
    g_prog[0] = t;
    g_prog_dirty = true;
}

inline void prog_flush() {
    if (!g_prog_dirty) return;
    char buf[2048];
    int n = 0;
    for (int i = 0; i < g_nprog && n < (int)sizeof(buf) - 160; i++)
        n += so_snprintf(buf + n, sizeof(buf) - n, "%s\t%u\t%u\n",
                         g_prog[i].name, (unsigned)g_prog[i].off, (unsigned)g_prog[i].fsize);
    if (n > 0 && so_blob_save("reader_progress.cfg", buf, n) == n) g_prog_dirty = false;
}

// ══════════════════════════════════════════════════════════════
//  三、SD 上的字体清单（/sdcard/fonts/*.ttf）
// ══════════════════════════════════════════════════════════════
inline char g_fonts[6][80];
inline int  g_nfonts = 0;

inline void fonts_scan() { g_nfonts = so_list_fonts(g_fonts, 6); }

} // namespace app::rd
