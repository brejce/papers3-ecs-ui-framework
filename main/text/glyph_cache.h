#pragma once

// ──────────────────────────────────────────────────────────────
// glyph_cache.h — 字形位图 LRU 缓存（PSRAM）
//
// 为什么要它：实测每字形光栅化 0.5~0.6ms（28px CJK），占「绘制」的约 60%；
// 而中文正文是 Zipf 分布，常用字反复出现。把**已经光栅化好的 8bit 位图**按
// (codepoint, 字号) 存起来，命中就直接拿位图 —— 轮廓解析 + 扫描线填充整段省掉。
//
// 存储：条目表 + 每条位图都在 **PSRAM**（内部 RAM 只剩一百多 KB，放不下）。
// 预算：字节上限 + 条目上限 双闸，超了从 LRU 尾部淘汰。
//
// ⚠️ 用它的两个前提（已在 ttf.h 落实，改那边时别踩回去）：
//   1. **push_glyph 必须只读位图**。它原来把混色结果写回 g.gray（拿当前屏幕底灰
//      和目标墨色把位图烧成最终灰度），那样缓存一次就花。现在混进独立的 blend 行缓冲。
//   2. **Glyph.owned == false 表示"位图属于缓存"**，free_glyph 不能 free 它。
//
// 换字体（load/load_from_sd/free_font）要调 flush()：键里只有 (码点, 字号)，
// 不带字体身份，不清会把上一款字体的位图当成新字体的返回。
// ──────────────────────────────────────────────────────────────

#include <cstdint>
#include <cstring>
#include "esp_heap_caps.h"
#include "esp_log.h"

namespace m5ui::ttf {
namespace gcache {

inline constexpr int    BUCKETS     = 512;          // 哈希桶数（2 的幂）
inline constexpr int    MAX_ENTRIES = 1536;         // 条目上限
inline constexpr size_t BUDGET      = 512 * 1024;   // 位图字节预算（见 512KB 依据）
inline constexpr size_t MAX_GLYPH_B = 128 * 128;    // 单个字形位图上限（超了不缓存）

struct Ent {
    int32_t  cp;
    int16_t  px;
    uint16_t fid;         // ★ 字体身份 —— 见 FontCtx.fid（同字号不同字体绝不能互相命中）
    int16_t  w, h, xoff, yoff, advance;
    uint32_t bytes;
    uint8_t* gray;        // PSRAM，w*h 字节（紧贴墨迹的包围盒，不是满格）
    Ent*     hnext;       // 桶内链
    Ent*     newer;       // LRU：更近使用
    Ent*     older;
};

// 全部放 PSRAM（条目表本身也放，内部 RAM 抠不出几十 KB）
inline Ent*    g_tab    = nullptr;
inline Ent*    g_bucket[BUCKETS] = {};
inline int     g_n      = 0;
inline size_t  g_bytes  = 0;
inline Ent*    g_mru    = nullptr;
inline Ent*    g_lru    = nullptr;
inline int64_t g_hit = 0, g_miss = 0, g_evict = 0;

// 惰性初始化：第一次查/存才分配。分配失败 = 缓存静默关掉（不影响正确性）。
inline bool ready() {
    if (g_tab) return true;
    g_tab = (Ent*)heap_caps_calloc(MAX_ENTRIES, sizeof(Ent), MALLOC_CAP_SPIRAM);
    if (!g_tab) {
        ESP_LOGW("GCACHE", "条目表分配失败（PSRAM 不够）→ 字形缓存关闭");
        return false;
    }
    ESP_LOGI("GCACHE", "字形缓存就绪：%d 条上限 / %u KB 预算 / 条目表 %u KB(PSRAM)",
             MAX_ENTRIES, (unsigned)(BUDGET / 1024),
             (unsigned)(MAX_ENTRIES * sizeof(Ent) / 1024));
    return true;
}

inline uint32_t hash_of(int32_t cp, int16_t px, uint16_t fid) {
    return ((uint32_t)cp * 2654435761u) ^ ((uint32_t)px * 40503u)
         ^ ((uint32_t)fid * 2246822519u);
}

// ── LRU 链 ──
inline void lru_unlink(Ent* e) {
    if (e->older) e->older->newer = e->newer; else g_lru = e->newer;
    if (e->newer) e->newer->older = e->older; else g_mru = e->older;
    e->older = e->newer = nullptr;
}
inline void lru_push_mru(Ent* e) {
    e->older = g_mru; e->newer = nullptr;
    if (g_mru) g_mru->newer = e; else g_lru = e;
    g_mru = e;
}

// 从桶链和 LRU 链里摘掉并释放（位图在 PSRAM）
inline void drop(Ent* e) {
    Ent** p = &g_bucket[hash_of(e->cp, e->px, e->fid) & (BUCKETS - 1)];
    while (*p && *p != e) p = &(*p)->hnext;
    if (*p) *p = e->hnext;
    lru_unlink(e);
    if (e->gray) heap_caps_free(e->gray);
    e->gray = nullptr;
    e->cp = 0; e->px = 0; e->fid = 0;
    --g_n;
    g_bytes -= e->bytes;
    e->bytes = 0;
}

inline void flush() {
    if (!g_tab) return;
    for (int i = 0; i < MAX_ENTRIES; ++i)
        if (g_tab[i].gray) { heap_caps_free(g_tab[i].gray); g_tab[i].gray = nullptr; }
    memset(g_tab, 0, sizeof(Ent) * MAX_ENTRIES);
    memset(g_bucket, 0, sizeof(g_bucket));
    g_n = 0; g_bytes = 0; g_mru = g_lru = nullptr;
    // 计数器**不清** —— 那是统计量，清了就看不到累计命中率了
    ESP_LOGI("GCACHE", "缓存已清空（换字体）");
}

// 查：命中就填 *out 并把条目提到 MRU。返回 false = 未命中（调用方去光栅化）。
inline bool lookup(int32_t cp, int16_t px, uint16_t fid, uint8_t** gray, int16_t* w, int16_t* h,
                   int16_t* xoff, int16_t* yoff, int16_t* advance) {
    // ★ 必须调 ready()（惰性分配条目表），不能只判 !g_tab ——
    //   那样 g_tab 永远分配不出来，缓存静默失效（症状：日志里"命中0/查0"）。
    if (!ready()) return false;
    for (Ent* e = g_bucket[hash_of(cp, px, fid) & (BUCKETS - 1)]; e; e = e->hnext) {
        if (e->cp == cp && e->px == px && e->fid == fid) {
            lru_unlink(e);
            lru_push_mru(e);
            ++g_hit;
            *gray = e->gray; *w = e->w; *h = e->h;
            *xoff = e->xoff; *yoff = e->yoff; *advance = e->advance;
            return true;
        }
    }
    ++g_miss;
    return false;
}

// 存：拷一份位图进 PSRAM。满了先淘汰 LRU 尾部。
inline void insert(int32_t cp, int16_t px, uint16_t fid, const uint8_t* src, int w, int h,
                   int xoff, int yoff, int advance) {
    if (!ready() || !src || w <= 0 || h <= 0) return;   // ready() 同 lookup 那条说明
    const size_t bytes = (size_t)w * (size_t)h;
    if (bytes > MAX_GLYPH_B) return;                    // 异常大，别塞缓存

    // 已在表里就别重复插（理论上 lookup 已挡，防御性）
    for (Ent* e = g_bucket[hash_of(cp, px, fid) & (BUCKETS - 1)]; e; e = e->hnext)
        if (e->cp == cp && e->px == px && e->fid == fid) return;

    while ((g_bytes + bytes > BUDGET || g_n >= MAX_ENTRIES) && g_lru) {
        drop(g_lru);
        ++g_evict;
    }
    if (g_n >= MAX_ENTRIES) return;

    // 找一个空槽（drop 会把 gray 置 nullptr）
    Ent* e = nullptr;
    for (int i = 0; i < MAX_ENTRIES; ++i)
        if (!g_tab[i].gray && g_tab[i].cp == 0 && g_tab[i].bytes == 0) { e = &g_tab[i]; break; }
    if (!e) return;

    uint8_t* buf = (uint8_t*)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
    if (!buf) return;                                    // PSRAM 紧 → 这次不缓存
    memcpy(buf, src, bytes);

    e->cp = cp; e->px = px; e->fid = fid;
    e->w = (int16_t)w; e->h = (int16_t)h;
    e->xoff = (int16_t)xoff; e->yoff = (int16_t)yoff; e->advance = (int16_t)advance;
    e->bytes = (uint32_t)bytes;
    e->gray = buf;
    const uint32_t b = hash_of(cp, px, fid) & (BUCKETS - 1);
    e->hnext = g_bucket[b];
    g_bucket[b] = e;
    lru_push_mru(e);
    ++g_n;
    g_bytes += bytes;
}

// 给日志用：命中率 + 占用
inline int64_t hits()   { return g_hit; }
inline int64_t misses() { return g_miss; }
inline int64_t evicts() { return g_evict; }
inline int     count()  { return g_n; }
inline size_t  bytes()  { return g_bytes; }

} // namespace gcache
} // namespace m5ui::ttf
