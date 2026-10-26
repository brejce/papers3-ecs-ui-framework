#pragma once

#include "ecs/world.hpp"
#include "ecs/components.h"
#include <M5Unified.h>
#include <esp_timer.h>
#include "text/ttf.h"
#include "platform/nvs.h"      // 读全局字号（没带 FontSize 的部件按它走）
#include "systems/epd_lut.h"   // 全局波形档（换表 + 强制档）

namespace papers3::systems {

// ──────────────────────────────────────────────
// render_system — 把「脏了」的实体画到墨水屏
//
// 约定（和 sketch_jun4a / frame 一致）：
//   1. 只画挂了 Dirty 的实体，画完就摘掉脏标记 —— 刷一次几百毫秒，绝不每帧重画
//   2. 先 setEpdMode 再绘制 —— 档位决定「写像素时的量化级别」，必须画之前设
//   3. 每个实体自己 startWrite/endWrite + display(x,y,w,h)：一次事务推一块区域
//   4. 文字先量尺寸（textWidth / fontHeight）再画，居中；先擦掉上次画过的框，防残影
// ──────────────────────────────────────────────

// EPDMode 组件的 1~4 档 → lgfx 波形（M5PaperS3 / Panel_EPD 实测耗时）
//   1 = QUALITY (GC16) 780ms  16 级灰度  首屏、图片、防重影全刷
//   2 = TEXT    (GL16) 780ms  16 级灰度  文字（笔画细，双稳态最清晰）
//   3 = FAST    (DU)   234ms  2 级灰度   色块、按钮，日常默认
//   4 = FASTEST (DU4)  171ms  2 级灰度   极简反馈；跳过重叠检查，连刷会撕裂
//
// ★ 全局波形档（设置页的 5 档）优先：cfg.wave_def == 0（默认配置关）时，
//   部件自己声明的 mode 被忽略，整机统一走 cfg.wave_sel 那一档；
//   对应的自定义波形表已在开机时装好（见 systems/epd_lut.h）。
//   cfg.wave_def == 1（默认配置开）时，走下面这张原表 = 部件按内容自选。
inline void apply_epd_mode(uint8_t mode) {
    if (cfg::g_cfg.wave_def == 0) {
        uint8_t forced = epd::tier_epd_mode(cfg::g_cfg.wave_sel);
        if (forced != 0) mode = forced;
    } else {
        // ── ★ 默认配置口径（2026-10-15 拍板）：**画字走 gc13** ──
        //   「文字」语义（2=TEXT，以及没指定的 0）统一落到 quality 槽
        //   （开机的 quality 槽**总是**装 lut_quality13，见 main.cpp）。
        //   为什么不是 epd_text(GL16)，依据 docs/display/EPD_LUT_MANUAL.md：
        //     §6.4 档位是**双重作用**：fast/fastest 写帧缓冲时就是 1bit+Bayer 抖动，
        //          文字是 TTF 抗锯齿灰度 → 只有 quality/text 槽能出平滑灰；
        //     §3   text 与 quality 两槽**耗时几乎一样**（都前置 4 行擦除段），
        //          "text 是快档"是误解；
        //     §0   quality 族砍掉 16 行 hold → GC13 整屏 831ms（GL16 是 1448ms），
        //          画质一模一样，而且是**唯一无残留**的一族（§0 / §9 地板价）。
        if (mode == 2 || mode == 0) mode = 1;
    }
    switch (mode) {
        case 1:  M5.Display.setEpdMode(epd_mode_t::epd_quality); break;   // = gc13（画字档）
        case 2:  M5.Display.setEpdMode(epd_mode_t::epd_text);    break;   // text 槽（全局选 text12 时才走到）
        case 3:  M5.Display.setEpdMode(epd_mode_t::epd_fast);    break;
        case 4:  M5.Display.setEpdMode(epd_mode_t::epd_fastest); break;
        default: M5.Display.setEpdMode(epd_mode_t::epd_quality); break;   // 未知 → 走画字档，别掉到 fast
    }
}

// ── 「带」(band)：覆盖层开/关时要整条重推的那块区域 ──
// 覆盖层（弹窗）用 QUALITY 一次推完那一条带；带内的实体这一趟**不许各自推屏**，
// 否则就是"弱波形先推一遍、强波形再推一遍"——既慢，中间还会留下可见的中间态。
// ★ 性能探针（临时）：一帧推了几次屏、推屏累计花了多久。谁慢了一眼能看见。
inline uint32_t g_push_n  = 0;
inline int64_t  g_push_us = 0;
inline int64_t  g_push_px = 0;          // 推过的像素数（看"谁把整屏推了一遍"）
inline int64_t  g_trace_until = 0;      // 这个时刻之前，每帧都打日志（排查退出/切页耗时）

inline int32_t g_band_y = -1, g_band_h = 0;
inline void  band_begin(int32_t y, int32_t h) { g_band_y = y; g_band_h = h; }
inline bool  band_intersects(int32_t y, int32_t h) {
    return g_band_y >= 0 && y + h > g_band_y && y < g_band_y + g_band_h;
}

// 把帧缓冲里的一块矩形推到墨水屏 —— 这一步才真正让屏幕变色
// （前面的绘制只是改帧缓冲，屏幕不动）
inline void epd_push(int32_t x, int32_t y, int32_t w, int32_t h) {
    if (w <= 0 || h <= 0) return;
    if (band_intersects(y, h)) return;      // 带内 → 交给 pages::flush_band()
    const int64_t t0 = esp_timer_get_time();
    M5.Display.display(x, y, w, h);
    g_push_n++;
    g_push_px += (int64_t)w * h;
    g_push_us += esp_timer_get_time() - t0;
}


// 位图落帧缓冲：1bpp / 4bpp 灰度 → 8bit 扫描线 → pushImage
// ★ 1bpp 的 0 位是**透明**（保留底层像素）—— 这样图标能直接坐在点阵底上，
//   不会抠出一块白方块。做法和 ttf::push_glyph 一样：先 readRect 读回该行，再按位覆盖。
inline void draw_bitmap(int32_t x, int32_t y, const ecs::components::Bitmap& bm) {
    if (!bm.data || bm.w <= 0 || bm.h <= 0) return;
    const int w = bm.w, h = bm.h;
    const int S = (bm.scale >= 1) ? bm.scale : 1;   // 整数倍放大（最近邻）
    if (w * S > 128) return;                        // 行缓冲上限
    const int stride = (w * bm.bpp + 7) / 8;
    uint8_t line[128];
    uint8_t big[128];
    M5.Display.startWrite();
    for (int row = 0; row < h; ++row) {
        const uint8_t* src = bm.data + row * stride;
        if (bm.bpp == 1) {
            // 读回底层（灰度），量化到「二次量化不变的固定点」，再把 1 位压成墨
            lgfx::v1::pixelcopy_t pc(nullptr, lgfx::v1::grayscale_t::depth,
                                     lgfx::v1::grayscale_t::depth, false, nullptr);
            M5.Display.getPanel()->readRect(x, y + row * S, w, 1, line, &pc);
            int c = 0;
            while (c < w) {
                while (c < w && ((src[c >> 3] >> (7 - (c & 7))) & 1) == 0) {
                    line[c] = (uint8_t)(16 * (line[c] / 17) + 8);   // 透明：保留底层
                    ++c;
                }
                while (c < w && ((src[c >> 3] >> (7 - (c & 7))) & 1) != 0) {
                    line[c] = 0;                                     // 墨
                    ++c;
                }
            }
        } else {
            for (int c = 0; c < w; ++c) {
                const uint8_t b = src[c >> 1];
                const int v = (c & 1) ? (b & 0x0F) : (b >> 4);
                line[c] = (uint8_t)(v * 17);
            }
        }
        if (S > 1) {
            for (int c = 0; c < w; ++c)
                for (int k = 0; k < S; ++k) big[c * S + k] = line[c];
            for (int k = 0; k < S; ++k)
                M5.Display.pushImage(x, y + row * S + k, w * S, 1, (const lgfx::grayscale_t*)big);
        } else {
            M5.Display.pushImage(x, y + row, w, 1, (const lgfx::grayscale_t*)line);
        }
    }
    M5.Display.endWrite();
}

inline void render(ecs::World& w) {
    using namespace ecs::components;

    for (auto e : w.query<Rect, Style, Dirty>()) {
        // 没挂 Visible 的实体不画（也不留脏标）—— 切回这一页时由 pages::show 重新标脏
        if (!w.has<Visible>(e)) { w.remove<Dirty>(e); continue; }

        auto* r = w.get<Rect>(e);
        auto* s = w.get<Style>(e);
        if (!r || !s) { w.remove<Dirty>(e); continue; }

        auto* t = w.get<Text>(e);
        const bool has_text = (t && t->str && t->str[0]);

        // ── 光标：独立刷新，擦旧画新 ──
        //   ★ 必须走 GC13（mode 1），不能 DU4：拼音/英文一个字一个字敲，
        //     编辑条那条 48px 高的小带每键推一次，DU4 不清残留电荷 ——
        //     敲几十下之后那一行的旧字形就"烧"在屏上了（实测：键盘上方文字固化）。
        if (auto* cur = w.get_mut<Cursor>(e)) {
            apply_epd_mode(1);   // GC13：小矩形，代价可接受；残影不再累积
            M5.Display.startWrite();
            if (cur->drawn) {
                M5.Display.fillRect(cur->px, cur->py, r->w, r->h, s->bg);   // 擦旧光标
            }
            M5.Display.fillRect(r->x, r->y, r->w, r->h, s->fg);              // 画新光标
            M5.Display.endWrite();
            // 只推「旧 ∪ 新」这一小条
            int32_t x0 = cur->drawn ? (cur->px < r->x ? cur->px : r->x) : r->x;
            int32_t x1 = cur->drawn ? ((cur->px + r->w > r->x + r->w) ? cur->px + r->w : r->x + r->w) : (r->x + r->w);
            epd_push(x0, r->y, x1 - x0, r->h);
            cur->px = r->x; cur->py = r->y; cur->drawn = true;
            w.remove<Dirty>(e);
            continue;
        }

        // ── 字号：部件可以自己带（键盘 / 提示行就是不跟全局字号走的那一类）──
        //   ★ 必须在「量尺寸」之前，也必须在**两个文字分支之前** ——
        //     左对齐分支（下面那个）末尾会 continue，字号块要是放在分支后面，
        //     对 setting_text 这类实体就是静默失效（它在真机上表现为"字号写了没用"）。
        {
            auto* fs = w.get<FontSize>(e);
            const float want = (fs && fs->px > 0) ? (float)fs->px : (float)cfg::g_cfg.size;
            if (ttf::size_px() != want) ttf::set_size(want);
        }

        // ── 左对齐文字（输入框文字/待输入池/拼音提示条）：局部重绘 + 实际范围擦除 ──
        if (w.has<LeftAlign>(e)) {
            // ★ TextNoBg：不铺 Style.bg 底，字形直接压在下层像素上。
            //   点阵底（浅灰 + FAST 量化）上，铺底会抠出一块纯灰方块，就这一句把它去掉。
            const bool no_bg = w.has<TextNoBg>(e);
            uint8_t lm = 2;                                   // 默认 TEXT/GL16
            if (auto* em = w.get<EPDMode>(e)) lm = em->mode;  // 实体可自己声明（拼音提示条用 FASTEST）
            apply_epd_mode(lm);
            // 墨色：只压黑字（和下面居中文字同一条口径）
            const uint32_t ink_eff = (s->fg == 0x000000) ? cfg::g_cfg.ink : s->fg;
            int32_t act_l = 0, act_r = 0;
            if (has_text && ttf::loaded()) ttf::measure_extent(t->str, &act_l, &act_r);
            // ★ 擦除/推屏高度 = 行高 **+2**：`line_height()` 是 ascent + descent，而 stb 把
            //   descent 截断取整（鸿蒙 28px：ascent 22 + descent 5 = 27），可 y/g/q/p 的
            //   真实下伸是 5.8px —— 行盒底只到 r->y+26，尾巴最后 1~2 行像素**没人擦**，
            //   文字变短/位移时就会留下字脚残影，光标那一小条白底也正好压在这两行上。
            int32_t th = ttf::loaded() ? ttf::line_height() + 2 : 0;

            int32_t from = 0;
            if (auto* dr = w.get<TextDirtyRange>(e)) from = dr->from_char;

            // 上次画上去的框（TextBox）：文字变短要擦旧的宽度，实体**位移**要擦旧的位置
            int32_t prev_x = r->x, prev_r = r->x;
            if (auto* tb = w.get_mut<TextBox>(e)) {
                if (tb->w > 0) { prev_x = tb->px; prev_r = tb->px + tb->w; }
                tb->px = r->x; tb->w = act_r; tb->h = th;
            }

            // 实体整体位移了（如跟着光标走的拼音条）→ 旧位置的像素要擦、整段要重画。
            // ★ 没位移时绝不能把 x0 拉到 prev_x：x0 是变化点（比 prev_x 靠右），
            //   拉回去就会把 from 之前的字一起擦掉，而重画又从 from 开始 —— 前几个字凭空消失。
            const bool moved = (prev_x != r->x);
            const int32_t from_eff = moved ? 0 : from;

            // ── 空文字：只擦「上次真画过的那块」，**不要凭空算一个新框**。 ──
            // ★ 挂了 TextNoBg 的连擦都不擦：它没有底色可铺，旧字形由页面自己补 FAST 底重抖。
            if (!has_text) {
                if (!no_bg && prev_r > prev_x) {        // 上次真画过才需要擦
                    M5.Display.startWrite();
                    M5.Display.fillRect(prev_x, r->y, prev_r - prev_x, th, s->bg);
                    M5.Display.endWrite();
                    epd_push(prev_x, r->y, prev_r - prev_x, th);
                }
                w.remove<Dirty>(e);
                continue;
            }

            int32_t px = r->x + ttf::measure_prefix(t->str, from_eff);
            int32_t x0 = (from_eff == 0) ? (r->x + act_l) : px;
            int32_t x1 = r->x + act_r;                 // 变长：擦到新右边界
            if (prev_r > x1) x1 = prev_r;              // 变短：旧尾巴也要擦
            if (x1 < px + 4) x1 = px + 4;
            if (moved && prev_x < x0) x0 = prev_x;     // 只有真位移才额外擦旧位置
            x1 += 4;

            M5.Display.startWrite();
            if (!no_bg) M5.Display.fillRect(x0, r->y, x1 - x0, th, s->bg);
            if (ttf::loaded()) ttf::draw_text_from(r->x, r->y, t->str, from_eff, ink_eff, s->bg);
            M5.Display.endWrite();
            epd_push(x0, r->y, x1 - x0, th);
            w.remove<Dirty>(e);
            continue;
        }

        // ── 档位：组件自己声明，没声明就按「有没有文字」给默认 ──
        uint8_t mode = has_text ? 2 : 3;                  // 文字→TEXT，色块→FAST
        if (auto* em = w.get<EPDMode>(e)) mode = em->mode;
        apply_epd_mode(mode);                             // ★ 必须在任何绘制之前

        // ── 文字先量尺寸 ──（字号已在两个分支之前统一设好）
        // TTF 字体已加载 → 走 TTF 量尺寸；否则回退 M5GFX 内置点阵字体
        const bool use_ttf = ttf::loaded();
        int32_t tw = 0, th = 0;
        if (has_text) {
            if (use_ttf) {
                ttf::measure(t->str, &tw, &th);
            } else {
                tw = M5.Display.textWidth(t->str);
                th = M5.Display.fontHeight();
            }
        }

        // ── 要擦掉的范围 = 本次的框 ∪ 上次文字实际占的框 ──
        int32_t box_w = r->w, box_h = r->h;
        if (box_w <= 0) box_w = tw + 8;                   // Label 的 Rect 可能是 0×0，用文字尺寸兜底
        if (box_h <= 0) box_h = th + 8;
        if (box_w <= 0 || box_h <= 0) { w.remove<Dirty>(e); continue; }

        int32_t clr_w = box_w, clr_h = box_h;
        if (auto* tb = w.get_mut<TextBox>(e)) {
            if (tb->w > clr_w) clr_w = tb->w;             // 文字变短时，旧的宽框也要擦掉
            if (tb->h > clr_h) clr_h = tb->h;
            tb->w = box_w; tb->h = box_h;                 // 记整框（含 +8 边距），只记 tw 会右边留 4px 残影
        }

        // ── 画到帧缓冲 ──
        // 按下：只改「描边 + 阴影」，元素与文字位置固定不动
        const bool pressed = w.has<Pressed>(e);
        const int32_t sh = s->shadow;                        // 常态阴影偏移（决定足迹）
        const int32_t sh_eff = pressed ? 0 : sh;             // 按下：阴影收缩到 0
        // 描边：**本来就带框的部件**统一用全局「描边粗细」（设置里选 2/3/4px）；
        // 本来没描边的（border_w=0，纯色块/底/反白块）保持没有。
        // 挂了 FixedBorderW 的部件用自己固定的边框（终端风键盘键/候选格 = 1px）。按下再加粗 2px。
        int32_t bw;
        if (auto* fb = w.get<FixedBorderW>(e)) bw = fb->px + (pressed ? 2 : 0);
        else bw = (s->border_w > 0 ? cfg::g_cfg.bw : 0) + (pressed ? 2 : 0);
        // 墨色：只改**黑字**的深浅（白字/反白、以及语义灰 G99 那些都不动）
        const uint32_t ink_eff = (s->fg == 0x000000) ? cfg::g_cfg.ink : s->fg;
        const int32_t fw = clr_w + (sh > 0 ? sh : 0);        // 含阴影的足迹
        const int32_t fh = clr_h + (sh > 0 ? sh : 0);

        M5.Display.startWrite();

        // ★ 位图实体（图标）：不铺底、不擦足迹 —— 图标是静态的，且 1bpp 的 0 位要
        //   读回**真正在下面**的点阵底（一旦先填一块 bg，读回的就是那块实心灰，白费功夫）。
        const bool is_bmp = w.has<Bitmap>(e);

        // 擦足迹（含阴影区，用底色=画布色）
        if (!is_bmp) {
            if (s->radius > 0) M5.Display.fillRoundRect(r->x, r->y, fw, fh, s->radius, s->bg);
            else               M5.Display.fillRect(r->x, r->y, fw, fh, s->bg);
        }

        // 硬阴影（纯色偏移，无模糊）；按下收缩到 0 —— 元素本身不移动
        if (sh_eff > 0) {
            M5.Display.fillRect(r->x + sh_eff, r->y + sh_eff, clr_w, clr_h, s->fg);
        }

        // 元素（位置固定，不随按下移动）
        if (is_bmp) {
            // 位图：不画底/框，直接落图（draw_bitmap 在下面的分支里）
        } else if (bw > 0) {
            const uint32_t edge = pressed ? s->accent : s->border;
            M5.Display.fillRect(r->x, r->y, clr_w, clr_h, edge);                          // 描边
            M5.Display.fillRect(r->x + bw, r->y + bw, clr_w - 2*bw, clr_h - 2*bw, s->bg); // 内部
        } else if (s->radius > 0) {
            M5.Display.drawRoundRect(r->x, r->y, clr_w, clr_h, s->radius, s->border);
        } else {
            M5.Display.fillRect(r->x, r->y, clr_w, clr_h, s->bg);
        }

        if (auto* cd = w.get<CustomDraw>(e)) {
            // 自绘部件（翻页箭头 / 退格图标）：字体里没这些符号，直接画图元
            if (cd->fn) cd->fn(r->x, r->y, clr_w, clr_h, s->fg, s->bg);
        } else if (auto* bm = w.get<Bitmap>(e)) {
            // 位图图标（launcher 的 app 图标）：直接按 Rect 落图
            draw_bitmap(r->x, r->y, *bm);
        } else if (has_text) {
            const int32_t tx = r->x + (clr_w - tw) / 2;   // 固定居中，不随按下移动
            const int32_t ty = r->y + (clr_h - th) / 2;
            if (use_ttf) {
                ttf::draw_text(tx, ty, t->str, ink_eff, s->bg);
            } else {
                M5.Display.setTextColor(ink_eff, s->bg);
                M5.Display.drawString(t->str, tx, ty);
            }
        }

        M5.Display.endWrite();

        // ── 推含阴影的整块 ──
        epd_push(r->x, r->y, fw, fh);

        w.remove<Dirty>(e);
    }

    // 跑完一趟把字号恢复成全局 —— 页面的建页/布局代码都按全局字号算，
    // 别把键盘的 34 留在全局状态里（比如 hide_overlay 里要用 line_height()）。
    if (ttf::size_px() != (float)cfg::g_cfg.size) ttf::set_size((float)cfg::g_cfg.size);
}

} // namespace papers3::systems
