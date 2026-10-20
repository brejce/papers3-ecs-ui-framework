#pragma once

// ──────────────────────────────────────────────
// ttf.h — TrueType 字体实时栅格化（多实例版）
//
// 为什么不用 M5GFX 自带的 loadFont()：
//   loadFont() 吃 VLW（预烘好的位图点阵，固定字号+固定字符集）。
//   你要的是 TTF——任意字号、任意字符现场轮廓→位图。唯一选择 stb_truetype。
//
// 踩过的三个硬约束（不然后面必炸）：
//   1. stb_truetype 只认 **glyf 轮廓**（真 TrueType）。CFF/OTF（Spline）直接不认。
//      思源黑体 SourceHanSans / Noto Sans CJK 都是 CFF → 用不了。
//      可用：鸿蒙 HarmonyOS_SansSC（glyf，8.48MB，29824 字形）、文泉驿微米黑、霞鹜文楷。
//   2. TTC（字体集合）不支持。wqy-microhei.ttc 必须先抽单字体再裁。
//   3. **8.48MB 字体 > 8MB PSRAM** —— 整份读进内存必然 malloc 失败。
//      唯一解：esp_partition_mmap 把 flash 映射成可随机访问指针，stb_truetype
//      直接在映射区上解析（它只读自己需要的字节），零拷贝。
//
// 管线：
//   flash fonts 分区 →mmap→ TTF 指针
//     →stbtt 栅格化 8bit 灰度字形→内部 RAM（不占 PSRAM）
//     →8bit 灰度直接 pushImage（16 级灰抗锯齿，不再 1bit 阈值化）
//     →pushImage 上屏
//
// ★ 2026-10-10 多实例化：原来 10 个 static 全局（单例），改成 struct FontCtx。
//   核心函数全部加 FontCtx& 参数（重载），固件页面继续用无参单例版（调用点不动）；
//   .so 插件通过宿主导出的 so_font_* 拿到自己的 FontCtx 句柄，多份字体并存、
//   各自换字体/字号，互不干扰。
//
// ⚠️ STB_TRUETYPE_IMPLEMENTATION 不在这里 define 了 —— 挪到 main/ttf_impl.cpp，
//   否则 plugin.cpp 若 include 本头会重复生成 stbtt 实现（链接期重复定义）。
//   本头只 include stb_truetype.h 拿**声明**（STBTT_DEF = extern）。
// ──────────────────────────────────────────────

#include "esp_partition.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include <cstdio>
#include <cstring>
#include <M5Unified.h>

#include "third_party/stb_truetype.h"   // 只拿声明；实现在 ttf_impl.cpp
#include "text/glyph_cache.h"           // 字形位图 LRU 缓存（PSRAM）

namespace m5ui::ttf {

static const char* TAG = "TTF";

// ── 一个字体实例的全部状态（原来那 10 个 static 全局）──
struct FontCtx {
    stbtt_fontinfo  font;
    const uint8_t*  mapped = nullptr;   // mmap 出来的 TTF 指针（flash 虚拟地址）
    esp_partition_mmap_handle_t map_handle = 0;
    uint8_t*        buf = nullptr;      // SD 读进 PSRAM 的 TTF 指针
    size_t          ttf_len = 0;
    float           scale = 0.f;        // 像素 / em 单位
    float           px = 0.f;           // 当前字号（像素）—— 给「临时改字号再还原」用
    int             ascent = 0;         // 基线以上像素高
    int             descent = 0;        // 基线以下像素深（正值）
    bool            ready = false;
    // ★ 字体身份（每次**成功加载**分配一个新值）。字形缓存的键里有它。
    //   为什么需要：键如果只有 (码点, 字号)，固件内置字体和 .so 从 SD 载入的字体
    //   在**同字号**下会互相命中 —— .so 会拿到固件字体的字形（画错字）。
    //   同一个 FontCtx 只是**改字号**时 fid 不变（键里的 px 已经区分开了）。
    //   0 = 未加载，永远不参与命中。
    uint16_t        fid = 0;
};

// 字体身份发号器（每次成功加载 +1）
inline uint16_t g_fid_seq = 0;

// 固件全局默认字体（页面共用）。inline 变量：跨编译单元唯一一份。
inline FontCtx g_default;

inline bool loaded(const FontCtx& c) { return c.ready; }
inline bool loaded()                  { return g_default.ready; }

// ── 试字体：在一份**还没提交**的缓冲上做全部校验 ──
// ★ 只写进 out_*，一个全局变量都不碰。校验不过就等于什么都没发生 ——
//   这是「先验后提交」的核心：CFF 字体会在 InitFont 阶段成功、到字形检查才露馅，
//   若在校验前就把字体换掉，失败后 ready 还是 true 而 font 指向没字形的字体，
//   之后所有 draw_text 都画不出东西（屏幕看着像坏了）。
// asc/desc 返回的是**像素**：stbtt 给的是 hhea 设计单位，必须乘 scale
// （踩过的坑：误当像素 → 928px，标签高度爆掉，屏幕才 960 高）。
static bool probe_font(const uint8_t* data, float pixel_size,
                       stbtt_fontinfo* out_font, float* out_scale,
                       int* out_asc, int* out_desc) {
    int off = stbtt_GetFontOffsetForIndex(data, 0);
    if (off < 0) return false;
    stbtt_fontinfo fi;
    if (!stbtt_InitFont(&fi, data, off)) return false;
    if (stbtt_FindGlyphIndex(&fi, 'A') == 0) return false;   // CFF/OTF 或空子集
    const float sc = stbtt_ScaleForPixelHeight(&fi, pixel_size);
    int asc = 0, desc = 0, gap = 0;
    stbtt_GetFontVMetrics(&fi, &asc, &desc, &gap);
    (void)gap;
    *out_font  = fi;
    *out_scale = sc;
    *out_asc   = (int)(asc  * sc);
    *out_desc  = -(int)(desc * sc);   // desc 是负值，取绝对值当「基线以下深度」
    return true;
}

// 映射 fonts 分区。pixel_size = 目标字号（汉字的 em 高度，像素）
inline bool load(FontCtx& c, const char* label, float pixel_size) {
    const esp_partition_t* part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, label);
    if (!part) {
        ESP_LOGE(TAG, "partition '%s' not found", label);
        return false;
    }

    // 已经映射过就先解映射 —— 否则重复 mmap 会漏 handle
    if (c.mapped) {
        esp_partition_munmap(c.map_handle);
        c.mapped = nullptr;
        c.map_handle = 0;
    }

    // 关键：不 malloc！mmap 直接把 flash 映射成可随机读指针。
    // 8.48MB 字体 > 8MB PSRAM，只有 mmap 这条路。
    const void* ptr = nullptr;
    esp_partition_mmap_handle_t h = 0;
    esp_err_t err = esp_partition_mmap(part, 0, part->size,
                                        ESP_PARTITION_MMAP_DATA, &ptr, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mmap %s failed: %s", label, esp_err_to_name(err));
        return false;
    }

    // ★ 先验后提交：校验不过就把这次映射还回去，**原字体原样保留**
    stbtt_fontinfo fi;
    float sc = 0.f;
    int asc = 0, desc = 0;
    if (!probe_font((const uint8_t*)ptr, pixel_size, &fi, &sc, &asc, &desc)) {
        ESP_LOGE(TAG, "分区 '%s' 里的字体不可用（CFF/OTF 或空子集）—— 保留原字体", label);
        esp_partition_munmap(h);
        return false;
    }

    // ── 到这里才提交。切回 flash 字体 → 把 SD 那份 PSRAM 缓冲放掉 ──
    if (c.buf) { heap_caps_free(c.buf); c.buf = nullptr; }

    c.mapped     = (const uint8_t*)ptr;
    c.map_handle = h;
    c.px         = pixel_size;
    c.font       = fi;
    c.scale      = sc;
    c.ascent     = asc;
    c.descent    = desc;
    c.ttf_len    = part->size;
    c.ready      = true;
    c.fid        = ++g_fid_seq;
    gcache::flush();     // ★ 换字体了：新 fid 让旧条目再也命不中，顺手把内存还掉

    ESP_LOGI(TAG, "loaded '%s' %u bytes (flash mmap), scale=%.4f, ascent=%d descent=%d",
             label, (unsigned)c.ttf_len, c.scale, c.ascent, c.descent);
    return true;
}

// 从 SD 读 TTF 进 PSRAM，然后 stbtt 初始化。pixel_size = 目标字号。
// 计时：SD 读耗时（t0→t1）+ 校验耗时（t1→t2）。
//
// ★ 先验后提交：整份读到内存后先 probe_font() 校验，**通过才动状态**。
//   校验不过（CFF/OTF、空子集、坏文件）→ 释放缓冲、原字体原样保留、返回 false。
inline bool load_from_sd(FontCtx& c, const char* path, float pixel_size) {
    if (!path) return false;
    int64_t t0 = esp_timer_get_time();

    FILE* f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "fopen '%s' failed (SD mounted?)", path);
        return false;
    }
    fseek(f, 0, SEEK_END);
    long fsz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fsz <= 0) { fclose(f); ESP_LOGE(TAG, "empty file '%s'", path); return false; }

    // PSRAM 只剩几 MB，先按容量挡一道 —— 别等 malloc 失败才发现
    size_t free_ps = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    if ((size_t)fsz + 65536 > free_ps) {
        fclose(f);
        ESP_LOGE(TAG, "'%s' %ld bytes 太大：PSRAM 只剩 %u 字节", path, fsz, (unsigned)free_ps);
        return false;
    }

    uint8_t* buf = (uint8_t*)heap_caps_malloc((size_t)fsz, MALLOC_CAP_SPIRAM);
    if (!buf) {
        fclose(f);
        ESP_LOGE(TAG, "malloc %ld bytes PSRAM failed (font too big?)", fsz);
        return false;
    }

    size_t got = fread(buf, 1, (size_t)fsz, f);
    fclose(f);
    int64_t t1 = esp_timer_get_time();
    if (got != (size_t)fsz) {
        ESP_LOGE(TAG, "fread %u/%ld bytes", (unsigned)got, fsz);
        heap_caps_free(buf);
        return false;
    }

    // ── 校验（此时状态一个都没动）──
    stbtt_fontinfo fi;
    float sc = 0.f;
    int asc = 0, desc = 0;
    if (!probe_font(buf, pixel_size, &fi, &sc, &asc, &desc)) {
        ESP_LOGE(TAG, "'%s' 不是可用的 TTF（CFF/OTF 或空子集）—— 保留原字体", path);
        heap_caps_free(buf);
        return false;
    }
    int64_t t2 = esp_timer_get_time();

    // ── 到这里才提交 ──
    // 上一份 SD 字体缓冲要放掉；默认字体是 flash mmap 的，不在 PSRAM 里，不能 free。
    if (c.buf) { heap_caps_free(c.buf); c.buf = nullptr; }

    c.font     = fi;          // 现在才换字体
    c.px       = pixel_size;
    c.scale    = sc;
    c.ascent   = asc;
    c.descent  = desc;
    c.buf      = buf;         // 常驻，后续栅格化要用；下次换字体时再放
    c.ttf_len  = (size_t)fsz;
    c.mapped   = nullptr;
    c.ready    = true;
    c.fid      = ++g_fid_seq;
    gcache::flush();     // ★ 换字体了：见 load() 里的同一条说明

    ESP_LOGI(TAG, "loaded from SD '%s' %ld bytes: read=%lld ms, probe=%lld ms, ascent=%d descent=%d",
             path, fsz,
             (long long)((t1 - t0) / 1000),
             (long long)((t2 - t1) / 1000),
             c.ascent, c.descent);
    return true;
}

// 释放一个实例的资源（mmap 解映射 + PSRAM 缓冲释放），置回未 ready。
// .so 卸载字体 / 退出时用。
inline void free_font(FontCtx& c) {
    if (c.mapped) { esp_partition_munmap(c.map_handle); c.mapped = nullptr; c.map_handle = 0; }
    if (c.buf)    { heap_caps_free(c.buf); c.buf = nullptr; }
    c.ttf_len = 0;
    c.ready   = false;
    gcache::flush();     // ★ 字体没了，缓存的位图全部作废
}

// ── 单例包装（固件页面调用点不动）──
inline bool load(const char* label, float pixel_size) { return load(g_default, label, pixel_size); }
inline bool load_from_sd(const char* path, float pixel_size) {
    if (!g_default.ready) return false;     // 没有可回退的原字体，不敢换（单例热切换语义）
    return load_from_sd(g_default, path, pixel_size);
}

// 当前字体是不是来自 SD（相对 flash mmap 说的）—— 设置页用来显示「使用中」
inline bool        from_sd(const FontCtx& c) { return c.buf != nullptr; }
inline bool        from_sd()                 { return g_default.buf != nullptr; }
// 当前字体的来源描述（给设置页显示）
inline const char* source(const FontCtx& c)  { return c.buf ? "SD" : "flash"; }
inline const char* source()                  { return g_default.buf ? "SD" : "flash"; }

// 注：这里**没有**「把 SD 字体写进 flash 分区」的路径，是故意的 ——
//   fonts(3MB) 分区 = **默认字体**，只在刷固件时写；
//   换字体 = 换 SD 卡上的文件（load_from_sd 读进 PSRAM，运行时不碰 flash）。

// 改字号。★ 必须同时重算 ascent/descent ——
//   只改 scale 的话 line_height() 还是旧值，改字号后行高/居中全错
//   （line_height() = ascent + descent，这两个是像素值、随 scale 变）。
// 注意单例版是**全局**的：调完所有页面的文字尺寸都变了，调用方要整屏重排。
inline void set_size(FontCtx& c, float pixel_size) {
    if (!c.ready) return;
    c.px    = pixel_size;
    c.scale = stbtt_ScaleForPixelHeight(&c.font, pixel_size);
    int asc = 0, desc = 0, gap = 0;
    stbtt_GetFontVMetrics(&c.font, &asc, &desc, &gap);
    (void)gap;
    c.ascent  = (int)(asc  * c.scale);
    c.descent = -(int)(desc * c.scale);
}
inline void set_size(float pixel_size) { set_size(g_default, pixel_size); }

// 当前字号（像素）。自绘部件要「临时改字号画完还原」时用它存旧值 ——
// 别去 include platform/nvs.h 读配置（UI 层不该依赖平台层）。
inline float size_px(const FontCtx& c) { return c.px; }
inline float size_px()                 { return g_default.px; }

inline int   line_height(const FontCtx& c) { return c.ascent + c.descent; }
inline int   line_height()                 { return g_default.ascent + g_default.descent; }
inline int   ascent_px(const FontCtx& c)   { return c.ascent; }
inline int   ascent_px()                   { return g_default.ascent; }
inline float scale(const FontCtx& c)       { return c.scale; }
inline float scale()                       { return g_default.scale; }

// UTF-8 → codepoint
inline int32_t utf8_next(const char*& p) {
    const uint8_t* s = (const uint8_t*)p;
    uint8_t c = *s;
    if (c < 0x80)           { p += 1; return c; }
    if ((c & 0xE0) == 0xC0) { p += 2; return ((c & 0x1F) << 6) | (s[1] & 0x3F); }
    if ((c & 0xF0) == 0xE0) { p += 3; return ((c & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F); }
    if ((c & 0xF8) == 0xF0) { p += 4; return ((c & 0x07) << 18) | ((s[1] & 0x3F) << 12) | ((s[2] & 0x3F) << 6) | (s[3] & 0x3F); }
    p += 1; return 0xFFFD;
}

inline int has_codepoint(const FontCtx& c, int32_t cp) {
    return stbtt_FindGlyphIndex(&c.font, cp) != 0;
}
inline int has_codepoint(int32_t cp) { return has_codepoint(g_default, cp); }

// 量前 char_count 个字符的像素宽度（字符下标，非字节）—— 用来定位光标横条
inline int32_t measure_prefix(const FontCtx& c, const char* utf8, int char_count) {
    if (!c.ready || !utf8) return 0;
    int w = 0;
    const char* p = utf8;
    int32_t cp;
    int n = 0;
    while (n < char_count && (cp = utf8_next(p)) != 0) {
        int adv = 0;
        stbtt_GetCodepointHMetrics(&c.font, cp, &adv, nullptr);
        w += (int)(adv * c.scale);
        n++;
    }
    return w;
}
inline int32_t measure_prefix(const char* utf8, int char_count) {
    return measure_prefix(g_default, utf8, char_count);
}

// 量一段文字的像素宽高
inline void measure(const FontCtx& c, const char* utf8, int32_t* out_w, int32_t* out_h) {
    if (!c.ready) { *out_w = *out_h = 0; return; }
    int w = 0;
    const char* p = utf8;
    int32_t cp;
    while ((cp = utf8_next(p)) != 0) {
        int adv = 0;
        stbtt_GetCodepointHMetrics(&c.font, cp, &adv, nullptr);
        w += (int)(adv * c.scale);          // 设计单位 × scale = 像素（同 ascent 的坑）
    }
    *out_w = w;
    *out_h = line_height(c);
}
inline void measure(const char* utf8, int32_t* out_w, int32_t* out_h) {
    measure(g_default, utf8, out_w, out_h);
}

// 量文字的实际渲染范围（含左右 overhang），out_left 可能为负。用来精确擦除。
inline void measure_extent(const FontCtx& c, const char* utf8, int32_t* out_left, int32_t* out_right) {
    *out_left = *out_right = 0;
    if (!c.ready || !utf8 || !utf8[0]) return;
    int pen = 0;
    bool any = false;
    const char* p = utf8;
    int32_t cp;
    while ((cp = utf8_next(p)) != 0) {
        int x0, y0, x1, y1;
        stbtt_GetCodepointBitmapBox(&c.font, cp, c.scale, c.scale, &x0, &y0, &x1, &y1);
        if (x1 > x0) {
            if (!any) { *out_left = pen + x0; *out_right = pen + x1; any = true; }
            else {
                if (pen + x0 < *out_left)  *out_left  = pen + x0;
                if (pen + x1 > *out_right) *out_right = pen + x1;
            }
        }
        int adv = 0;
        stbtt_GetCodepointHMetrics(&c.font, cp, &adv, nullptr);
        pen += (int)(adv * c.scale);
    }
}
inline void measure_extent(const char* utf8, int32_t* out_left, int32_t* out_right) {
    measure_extent(g_default, utf8, out_left, out_right);
}

struct Glyph {
    uint8_t* gray = nullptr;  // 8bit 灰度（LGFX 约定：0=黑，255=白）
    int w = 0, h = 0;
    int xoff = 0, yoff = 0;   // 相对基线原点，y 向下为正
    int advance = 0;
    // ★ false = 位图由**字形缓存**持有（只读；free_glyph 不能释放它）。
    //   见 text/glyph_cache.h 顶部"用它的两个前提"。
    bool owned = true;
};

// 栅格化一个 codepoint → 8bit 灰度位图（内部 RAM，不占 PSRAM）
// ── 性能探针（spike，只测量、不改行为）──────────────────────────────
//   把"一个字形"的成本拆成两段：
//     ① 光栅化 g_perf_raster_us：轮廓 → 8bit 灰度位图（stbtt / FreeType 那段）
//     ② 落图   g_perf_push_us  ：位图 → 帧缓冲
//   ② 里每个"非白连续段"都要 readRect 回读底灰 + 逐像素混色 + pushImage ——
//   怀疑大头在这里而不是光栅化，这两个计数器就是拿来定论的。
//   累计值由 plugin::tick() 每帧读走打印（见 main/plugin.cpp）。
inline int64_t g_perf_raster_us = 0;
inline int64_t g_perf_push_us   = 0;
inline int64_t g_perf_glyphs    = 0;
inline void perf_reset() { g_perf_raster_us = 0; g_perf_push_us = 0; g_perf_glyphs = 0; }

// 作用域计时：不管从哪个 return 出去都会累加（省得每个 return 前手写一遍）
struct PerfScope {
    int64_t  t0;
    int64_t* dst;
    explicit PerfScope(int64_t* d) : t0(esp_timer_get_time()), dst(d) {}
    ~PerfScope() { *dst += esp_timer_get_time() - t0; }
};

inline Glyph rasterize(const FontCtx& c, int32_t cp) {
    Glyph g;
    if (!c.ready || cp <= 0) return g;

    // ★★ 字形缓存：命中就直接拿位图，整段「轮廓解析 + 扫描线填充」都省掉。
    //   键 = (codepoint, 字号取整) —— 换字号得到不同键，不用清缓存。
    //   命中时 owned=false：位图归缓存，free_glyph 不释放、push_glyph 不改写。
    //   这一段**不在探针①里** —— 探针量的是"真花了多少光栅化"，
    //   命中路径本来就该是 0，命中率单独统计（看 plugin::tick 那行日志）。
    {
        int16_t cw = 0, ch = 0, cx = 0, cy = 0, ca = 0;
        if (gcache::lookup(cp, (int16_t)(c.px + 0.5f), c.fid, &g.gray, &cw, &ch, &cx, &cy, &ca)) {
            g.w = cw; g.h = ch; g.xoff = cx; g.yoff = cy; g.advance = ca;
            g.owned = false;
            return g;
        }
    }

    PerfScope _perf(&g_perf_raster_us);      // ★ 探针①（只统计未命中、真光栅化的）

    // ★★ 优化 a：codepoint → glyph index **只查一次**。
    //   原来用的三个 *Codepoint* 接口，内部各自都要调一次 stbtt_FindGlyphIndex
    //   （stb_truetype.h:2631 就是 GetGlyphHMetrics(info, FindGlyphIndex(info,cp), ...)），
    //   => 同一个字查了 **3 遍 cmap**。6899 字的 CJK cmap 每遍都是一次表内搜索，
    //   实测每字形光栅化 0.5~0.6ms，这三遍占了可观比重。
    //   行为完全等价：Glyph* 系列就是 Codepoint* 系列去掉那次查找，其余逐字一致。
    //   （对应 .so 白名单那条经验的反面：能一次算清的别算三遍。）
    const int gi = stbtt_FindGlyphIndex(&c.font, cp);

    int adv = 0;
    stbtt_GetGlyphHMetrics(&c.font, gi, &adv, nullptr);
    g.advance = (int)(adv * c.scale);

    int x0, y0, x1, y1;
    stbtt_GetGlyphBitmapBox(&c.font, gi, c.scale, c.scale, &x0, &y0, &x1, &y1);
    int bw = x1 - x0, bh = y1 - y0;
    if (bw <= 0 || bh <= 0) return g;      // 空格等无轮廓字形：只占 advance

    g.gray = (uint8_t*)heap_caps_malloc(bw * bh, MALLOC_CAP_INTERNAL);
    if (!g.gray) return g;
    g.w = bw; g.h = bh; g.xoff = x0; g.yoff = y0;

    stbtt_MakeGlyphBitmap(&c.font, g.gray, bw, bh, bw, c.scale, c.scale, gi);

    // stbtt 覆盖值：0=背景(白)、255=墨(黑)；LGFX grayscale：0=黑、255=白 → 取反
    for (int i = 0, n = bw * bh; i < n; ++i) g.gray[i] = 255 - g.gray[i];

    // ★ 存进字形缓存（内部拷一份到 PSRAM）。拷不动就跳过，不影响正确性。
    gcache::insert(cp, (int16_t)(c.px + 0.5f), c.fid, g.gray, bw, bh, x0, y0, g.advance);
    return g;
}
inline Glyph rasterize(int32_t cp) { return rasterize(g_default, cp); }

inline void free_glyph(Glyph& g) {
    // ★ 缓存借出来的位图（owned=false）绝不能 free —— 那会砸掉缓存里的共享副本
    if (g.gray && g.owned) heap_caps_free(g.gray);
    g.gray = nullptr;
}

// ── 墨色 ──
// 墨水屏只有灰阶，"颜色"就是深浅：fg 的亮度直接当墨的档位用
// （0 = 纯黑，越大越浅，255 = 看不见）。以前这里把 fg 直接扔了、恒用黑墨，
// 所以传灰色不生效 —— 大小写键 A/a 的双色就是这么哑掉的。
inline uint8_t ink_level(uint32_t fg) {
    uint8_t r = (uint8_t)((fg >> 16) & 0xFF);
    uint8_t g = (uint8_t)((fg >> 8) & 0xFF);
    uint8_t b = (uint8_t)(fg & 0xFF);
    return (uint8_t)((r * 30 + g * 59 + b * 11) / 100);   // 近似亮度
}

// 只写字形真正有墨的像素；覆盖度为 0 的孔洞/字间空隙绝不写回。
// readRect 得到的是面板 4bit 灰阶展开的 17*L；把它还原成 16*L+8
// 再与抗锯齿覆盖度合成，否则 17*L 二次量化会把灰底抖成相邻两级。
// 每个不透明游程单独读取和写入，避免把无墨处的背景重新量化。
inline void push_glyph(Glyph& g, int gx, int gy, uint8_t ink) {
    if (!g.gray) return;
    ++g_perf_glyphs;                         // ★ 探针：计数
    PerfScope _perf(&g_perf_push_us);        // ★ 探针②：落图耗时
    // 字形尺寸在常见字号下远小于此；异常超限直接不画，不能越界或伪造白底。
    constexpr int MAX_GLYPH = 128;
    if (g.w > MAX_GLYPH) return;
    uint8_t under[MAX_GLYPH];
    // ★ 混色结果写在这里，**不写回 g.gray** —— 位图可能来自字形缓存，必须只读。
    //   （原来是把混色值就地写回 pixels[k]，缓存被画一次就花了。）
    uint8_t blend[MAX_GLYPH];
    const int screen_w = M5.Display.width(), screen_h = M5.Display.height();
    for (int row = 0; row < g.h; ++row) {
        const int y = gy + row;
        if (y < 0 || y >= screen_h) continue;
        int col = 0;
        while (col < g.w) {
            while (col < g.w && g.gray[row * g.w + col] == 255) ++col;
            const int first = col;
            while (col < g.w && g.gray[row * g.w + col] != 255) ++col;
            const int left = gx + first, right = gx + col;
            const int clip_l = left < 0 ? 0 : left;
            const int clip_r = right > screen_w ? screen_w : right;
            if (clip_l >= clip_r) continue;
            const int count = clip_r - clip_l;
            // pixelcopy_t 是有状态的：positions 会随每次读取前进，绝不能 static 复用。
            lgfx::v1::pixelcopy_t pc(nullptr, lgfx::v1::grayscale_t::depth,
                                     lgfx::v1::grayscale_t::depth, false, nullptr);
            M5.Display.getPanel()->readRect(clip_l, y, count, 1, under, &pc);
            const uint8_t* src = g.gray + row * g.w + first + (clip_l - left);
            for (int k = 0; k < count; ++k) {
                const int level = under[k] / 17; // _read_pixel 返回 17*L，L=0..15
                const int bg = 16 * level + 8;  // 二次量化不变的固定点
                const int cov = 255 - src[k];
                blend[k] = (uint8_t)(bg + (ink - bg) * cov / 255);
            }
            M5.Display.pushImage(clip_l, y, count, 1,
                                 (const lgfx::grayscale_t*)blend);
        }
    }
}

// ── 画一行文字 ──
// (x, y) = 文字框左上角（不是基线）。fg 的亮度 = 墨的深浅档（见 ink_level）；
// 要纯灰字就得让这个部件走 16 级波形（EPDMode{2} TEXT），2 级波形会把灰压成黑白。
// ls = 字间距（像素），在 pen 前进时加 —— .so 阅读器排版用；单例版传 0。
// 返回文字实际宽度。
inline int32_t draw_text(const FontCtx& c, int32_t x, int32_t y, const char* utf8,
                         uint32_t fg, uint32_t bg, int32_t ls = 0) {
    (void)bg;   // bg 参数保留（一堆调用方还传着），但字形不再按它铺底 —— 见 push_glyph
    if (!c.ready || !utf8) return 0;
    const uint8_t ink = ink_level(fg);

    int pen = x;
    const char* p = utf8;
    int32_t cp;

    while ((cp = utf8_next(p)) != 0) {
        Glyph g = rasterize(c, cp);
        if (g.gray) {
            push_glyph(g, pen + g.xoff, y + c.ascent + g.yoff, ink);
            free_glyph(g);
        }
        pen += g.advance + ls;
    }
    return pen - x;
}
inline int32_t draw_text(int32_t x, int32_t y, const char* utf8,
                         uint32_t fg, uint32_t bg) {
    return draw_text(g_default, x, y, utf8, fg, bg, 0);
}

// 从第 char_from 个字符开始画（前面的只推进 pen 不画），用于局部重绘。
// fg/bg 必须传：和 draw_text 一样，要按底色合成字形（否则反白行会出白盒子）。
inline int32_t draw_text_from(const FontCtx& c, int32_t x, int32_t y, const char* utf8, int char_from,
                              uint32_t fg = 0x000000, uint32_t bg = 0xFFFFFF) {
    (void)bg;   // 同 draw_text：底从帧缓冲读，参数只是 API 兼容
    if (!c.ready || !utf8) return 0;
    const uint8_t ink = ink_level(fg);
    int pen = x;
    const char* p = utf8;
    int32_t cp;
    int n = 0;
    while ((cp = utf8_next(p)) != 0) {
        int adv = 0;
        stbtt_GetCodepointHMetrics(&c.font, cp, &adv, nullptr);
        int advance = (int)(adv * c.scale);
        if (n >= char_from) {
            Glyph g = rasterize(c, cp);
            if (g.gray) {
                push_glyph(g, pen + g.xoff, y + c.ascent + g.yoff, ink);
            }
            free_glyph(g);
        }
        pen += advance;
        n++;
    }
    return pen - x;
}
inline int32_t draw_text_from(int32_t x, int32_t y, const char* utf8, int char_from,
                              uint32_t fg = 0x000000, uint32_t bg = 0xFFFFFF) {
    return draw_text_from(g_default, x, y, utf8, char_from, fg, bg);
}

} // namespace m5ui::ttf
