#pragma once

#include "ecs/world.hpp"
#include "ecs/components.h"
#include "systems/flex.h"
#include "ui/theme.h"
#include "text/ttf.h"

namespace papers3::ui {

// ──────────────────────────────────────────────
// Widget — 工厂函数集
//
// "创建一个按钮"不再是 new Button(...)
// 而是给一个 Entity 挂上需要的组件组合：
//   Rect + Text + Style + Touchable + OnClick
//
// "创建一个 Label"是挂上：
//   Rect + Text + Style
//
// 没有继承，没有虚函数。
// ──────────────────────────────────────────────

namespace widgets {

// ── 基础可见元素 ──

inline ecs::Entity rect(
    ecs::World& w,
    int32_t x, int32_t y, int32_t width, int32_t height,
    uint32_t bg
) {
    using namespace ecs::components;
    auto e = w.spawn();
    w.add(e, Visible{});
    w.add(e, Rect{x, y, width, height});
    w.add(e, Style{0, bg, 0, 0, 0});
    // 纯色块用不到灰度 → FAST(DU)，234ms。
    // ★ 页面第一块 rect(0,0,540,960,白) 就是靠它「盖住上一页」的：
    //   整屏白底一次推完约 377ms，比 QUALITY 清场的 1209ms 快三倍。
    //   （QUALITY 留给每 N 次切页的重影清场，见 pages/manager.h）
    w.add(e, EPDMode{3});
    w.add(e, Dirty{});
    return e;
}

inline ecs::Entity label(
    ecs::World& w,
    int32_t x, int32_t y, const char* text,
    uint32_t fg = 0, uint32_t bg = 0
) {
    using namespace ecs::components;
    auto e = w.spawn();
    w.add(e, Visible{});
    w.add(e, Rect{x, y, 0, 0});     // 尺寸由渲染时按文字实测（TextBox 记住上次画上去的大小）
    w.add(e, Text{text});
    w.add(e, Style{fg, bg, 0, 0, 0});
    w.add(e, TextBox{});
    w.add(e, EPDMode{2});           // 文字：TEXT(GL16) 笔画最清晰
    w.add(e, Dirty{});
    return e;
}

// 位图图标（launcher 的 app 图标）：Rect = 图标尺寸，数据挂在 Bitmap 上。
// 1bpp 的 0 位透明（保留底层点阵），4bpp 按灰度写。
inline ecs::Entity bitmap(
    ecs::World& w,
    int32_t x, int32_t y,
    const uint8_t* data, int32_t wpx, int32_t hpx, uint8_t bpp,
    uint32_t bg = 0xFFFFFF
) {
    using namespace ecs::components;
    auto e = w.spawn();
    w.add(e, Visible{});
    w.add(e, Rect{x, y, wpx, hpx});
    w.add(e, Bitmap{data, (int16_t)wpx, (int16_t)hpx, bpp, 0x000000, bg});
    w.add(e, Style{0, bg, 0, 0, 0, 0, 0});   // bg 只做兜底擦除；位图实体不铺底（见 render）
    w.add(e, EPDMode{2});                     // 图标带灰度 → TEXT 波形
    w.add(e, Dirty{});
    return e;
}

inline ecs::Entity button(
    ecs::World& w,
    int32_t x, int32_t y, int32_t width, int32_t height,
    const char* text,
    void (*on_click)(void*), void* ctx = nullptr
) {
    using namespace ecs::components;
    auto e = w.spawn();
    w.add(e, Visible{});
    w.add(e, Rect{x, y, width, height});
    w.add(e, Text{text});
    // 终端风（2026-10-17 用户要求 launcher 统一）：直角 + 1px 黑描边 + 无阴影。
    //   1px 走 FixedBorderW（固定规格，不吃全局描边档）；5px 阴影去掉（黑块稀缺，也是首页
    //   左下角那个黑色残影的来源 —— app 按钮的阴影）。按下只加粗描边，元素和文字不动。
    w.add(e, Style{0x000000, 0xFFFFFF, 0x000000, 0x000000, 0, 0, 0});
    w.add(e, FixedBorderW{1});
    w.add(e, Touchable{0xFF});
    w.add(e, OnClick{on_click, ctx});
    // ★ 含文字的部件**必须**走 TEXT/GL16，不能用 FASTEST —— 档位是双重作用：
    //   既决定刷屏波形，也决定**写像素时存几位**。epd_fastest(DU4) 是 2 级波形，
    //   写帧缓冲的那一刻就把字形抗锯齿的灰压成非黑即白，之后拿什么波形刷都恢复不了，
    //   文字看着就是"毛刺/被啃过"——首页按钮字符难看的根因就是这个。
    //   设置页早就绕过这一条：它自己的 opt_btn() 用的就是 EPDMode{2}，
    //   所以"设置里除了返回按钮其他都很清晰"（BACK 走的是这个通用 button()）。
    //   纯色块（rect / cursor）没有文字，才配用 FAST/FASTEST。
    //   代价：按下反馈 171ms → 780ms；清晰度优先。
    w.add(e, EPDMode{2});
    w.add(e, Dirty{});
    return e;
}

// 输入框外框（Memphis：直角 + 2px 黑描边 + 3px 硬阴影；只画一次）
inline ecs::Entity input_frame(
    ecs::World& w,
    int32_t x, int32_t y, int32_t width, int32_t height
) {
    using namespace ecs::components;
    auto e = w.spawn();
    w.add(e, Visible{});
    w.add(e, Rect{x, y, width, height});
    w.add(e, Style{0, 0xFFFFFF, 0x000000, 0, 0, 2, 3});
    w.add(e, EPDMode{2});
    w.add(e, Dirty{});
    return e;
}

// 输入框文字（左对齐，文字变了才重绘，TextBox 记旧尺寸擦残影）
inline ecs::Entity input_text(
    ecs::World& w,
    int32_t x, int32_t y, const char* text
) {
    using namespace ecs::components;
    auto e = w.spawn();
    w.add(e, Visible{});
    w.add(e, Rect{x, y, 0, 0});
    w.add(e, Text{text});
    w.add(e, Style{0, 0xFFFFFF, 0, 0, 0});
    w.add(e, TextBox{});
    w.add(e, LeftAlign{});
    w.add(e, TextDirtyRange{0});
    w.add(e, EPDMode{2});
    w.add(e, Dirty{});
    return e;
}

// 光标横条（独立实体，FASTEST 波形，移动时只刷新自己）
inline ecs::Entity cursor(
    ecs::World& w,
    int32_t x, int32_t y, int32_t width, int32_t height
) {
    using namespace ecs::components;
    auto e = w.spawn();
    w.add(e, Visible{});
    w.add(e, Rect{x, y, width, height});
    w.add(e, Style{0x000000, 0xFFFFFF, 0, 0, 0});
    w.add(e, Cursor{});
    w.add(e, EPDMode{4});   // FASTEST/DU4
    w.add(e, Dirty{});
    return e;
}

// 键盘按键（终端风）：1px 描边、无阴影。键密集，阴影会压到隔壁键。
// 见 docs/design/UI_DESIGN.md §6.2 / §8。
// ── 键盘专用字号（**不跟全局「字号」设置走**）──
// 键宽、行高、居中都是按这个字号实测排的版。全局字号改了键盘会挤/会空，
// 所以键帽和候选格把自己的字号写死在这儿，由渲染按部件切（FontSize 组件）。
// 要调键盘的字，只调这一处。
inline constexpr int16_t KB_FONT_PX = 34;

inline ecs::Entity key(
    ecs::World& w,
    int32_t x, int32_t y, int32_t width, int32_t height,
    const char* text,
    void (*on_click)(void*), void* ctx = nullptr
) {
    using namespace ecs::components;
    auto e = w.spawn();
    w.add(e, Visible{});
    w.add(e, Rect{x, y, width, height});
    w.add(e, Text{text});
    w.add(e, Style{0x000000, 0xFFFFFF, 0x000000, 0x000000, 0, 1, 0});
    w.add(e, FixedBorderW{1});   // 终端风：固定 1px 框，不吃全局描边档
    w.add(e, Touchable{0xFF});
    w.add(e, OnClick{on_click, ctx});
    // ★ 键面有字母/汉字，同样是"含文字"→ 必须 TEXT/GL16（理由见 button()）。
    //   代价明确：按一个键从 171ms 变成 780ms，连打时手感会明显变慢。
    //   —— 这是拿速度换清晰度，用户拍板要清晰。
    w.add(e, EPDMode{2});
    w.add(e, FontSize{KB_FONT_PX});   // ★ 键盘自己管字号（全局字号改了它不动）
    w.add(e, Dirty{});
    return e;
}

// 候选格：中文候选必须清晰 → TEXT(GL16) 16 级灰度（比按键慢，但候选是给人读的）
inline ecs::Entity cand_cell(
    ecs::World& w,
    int32_t x, int32_t y, int32_t width, int32_t height,
    const char* text,
    void (*on_click)(void*), void* ctx = nullptr
) {
    using namespace ecs::components;
    auto e = w.spawn();
    w.add(e, Visible{});
    w.add(e, Rect{x, y, width, height});
    w.add(e, Text{text});
    w.add(e, Style{0x000000, 0xFFFFFF, 0x000000, 0x000000, 0, 1, 0});
    w.add(e, FixedBorderW{1});   // 终端风：候选格固定 1px 框
    w.add(e, Touchable{0xFF});
    w.add(e, OnClick{on_click, ctx});
    w.add(e, EPDMode{2});   // 有中文 → TEXT/GL16
    w.add(e, FontSize{KB_FONT_PX});   // ★ 同上：候选格也不跟全局字号走
    w.add(e, Dirty{});
    return e;
}

// ── 开关（toggle）──
// 直角矩形轨道 + 方块钮 + **ON/OFF 字样**，和全局 Neo-Brutalist 口径一致
// （不用胶囊+圆钮那套）。字样不是装饰：墨水屏没有颜色，只靠反白+钮左右，
// 用户分不清哪边是开 —— 必须把状态写成字。
//
// ★ 描边/阴影交给**渲染通用路径**画（Style.border_w / Style.shadow），
//   CustomDraw **只画钮和字**。早先的写法是 Style 里 border_w=0、由 CustomDraw 画整个胶囊，
//   结果按下时 render 的 `bw = border_w + (pressed?2:0)` 凭空长出 2px **矩形**边框
//   —— 按胶囊却有个矩形框响应触摸，而且内部还被刷白再重画。别再用那种写法。
inline void draw_toggle_face(int32_t x, int32_t y, int32_t w, int32_t h, bool on) {
    const int32_t pad = 5;
    const int32_t kw  = (w - pad * 2) / 2;        // 钮占轨道一半
    const int32_t kh  = h - pad * 2;
    const int32_t kx  = on ? (x + w - pad - kw) : (x + pad);
    M5.Display.fillRect(kx, y + pad, kw, kh, on ? TFT_WHITE : TFT_BLACK);

    // 字样画在钮**不在**的那半边，颜色与底色相反。
    // ★ 字号固定 22px，**不跟全局字号**：开关只有 132 宽、半边可用约 60px，
    //   全局字号到 34px 时 "OFF" 约 60px 就挤爆了。画完必须还原全局字号，
    //   否则同一渲染循环里后面的实体全被带跑（和 draw_big_text 一个道理）。
    const int32_t fx = on ? (x + pad) : (x + pad + kw);   // 自由半边起点
    const int32_t fw = w - kw - pad * 2;                  // 自由半边宽
    const char*   s  = on ? "ON" : "OFF";
    const uint32_t fg = on ? theme::FG_WHITE : theme::FG_BLACK;
    const uint32_t bg = on ? theme::BG_BLACK : theme::BG_WHITE;

    const float keep = papers3::ttf::size_px();
    papers3::ttf::set_size(22.0f);
    int32_t tw = 0, th = 0;
    papers3::ttf::measure(s, &tw, &th);
    papers3::ttf::draw_text(fx + (fw - tw) / 2, y + (h - th) / 2, s, fg, bg);
    papers3::ttf::set_size(keep);          // 还原（画完不还原会把后面所有实体带跑）
}

// ── 输入框的「点一下就吊起键盘」开关 ──
// 装上 = flag 开（点框体直接弹键盘）；不装 = flag 关（只能靠旁边的「输入」按钮按需吊起）。
// 做法就是"挂不挂 Touchable + OnClick" —— **零新增组件类型**（组件类型上限 32）。
// 输入框的文字实体 h=0（尺寸由渲染实测），命中区太小，所以挂给**框体**那个实体。
inline void make_tap_edit(ecs::World& w, ecs::Entity e, void (*fn)(void*), void* ctx = nullptr) {
    using namespace ecs::components;
    w.add(e, Touchable{0xFF});
    w.add(e, OnClick{fn, ctx});
}

// 开关部件：位置 + 触摸 + 描边/阴影；底色由 on 决定（开=黑底，关=白底），
// 钮和字样交给调用方挂 CustomDraw{draw_toggle_face 的包装}（状态在页面自己手里，部件不存状态）。
inline ecs::Entity toggle(
    ecs::World& w,
    int32_t x, int32_t y, int32_t width, int32_t height,
    void (*on_click)(void*), void* ctx = nullptr,
    bool on = false
) {
    using namespace ecs::components;
    auto e = w.spawn();
    const uint32_t bg = on ? theme::BG_BLACK : theme::BG_WHITE;
    w.add(e, Visible{});
    w.add(e, Rect{x, y, width, height});
    w.add(e, Text{""});                        // 空文字 → 渲染只画底/描边/阴影，字自绘
    // 直角 + 3px 黑描边 + 4px 硬阴影：和设置行同一套；按下只改描边(+2)与阴影(收 0)
    w.add(e, Style{theme::FG_BLACK, bg, theme::FG_BLACK, theme::FG_BLACK, 0, 3, 4});
    w.add(e, Touchable{0xFF});
    w.add(e, OnClick{on_click, ctx});
    // ★ 这个部件本身 Text 是空的，但 CustomDraw(draw_toggle_face) **要画 ON/OFF 字样** ——
    //   自绘的文字也在同一个事务里，所以同样受"写像素时量化"的影响 → 走 TEXT/GL16。
    w.add(e, EPDMode{2});
    w.add(e, Dirty{});
    return e;
}

// 开关状态变了要连底色一起换（开=黑底白钮 / 关=白底黑钮）
inline void set_toggle(ecs::World& w, ecs::Entity e, bool on) {
    using namespace ecs::components;
    if (auto* st = w.get_mut<Style>(e)) {
        st->bg = on ? theme::BG_BLACK : theme::BG_WHITE;
    }
    w.add(e, Dirty{});
}

// ── 设置行：外框（含触摸/阴影）+ 左对齐文字 = 2 个实体 ──
// 为什么不直接用 button()：行的文字要**左对齐**，而 render 的 LeftAlign 分支
// 只铺底色 + 画字、不画描边，所以描边必须由另一个实体承担。
// 值直接接在标签后面（"显示与波形 · TEXT 780ms"），不做右对齐 ——
// 右对齐要多一个实体、还要每次按字体实测宽度改 Rect；设置页行多，不划算。
inline ecs::Entity setting_row(
    ecs::World& w,
    int32_t x, int32_t y, int32_t cw, int32_t ch,
    void (*on_click)(void*), void* ctx = nullptr,
    bool inverted = false
) {
    using namespace ecs::components;
    auto e = w.spawn();
    // 落到 uint32_t 再聚合：三元的类型是 int，直接塞进 Style 会触发 -Werror=narrowing
    const uint32_t bg = inverted ? theme::BG_BLACK : theme::BG_WHITE;   // 反白 = 黑底
    w.add(e, Visible{});
    w.add(e, Rect{x, y, cw, ch});
    w.add(e, Text{""});                       // 空文字 → 只画框不画字
    w.add(e, Style{theme::FG_BLACK, bg, theme::FG_BLACK, theme::FG_BLACK, 0, 3, 5});
    w.add(e, Touchable{0xFF});
    w.add(e, OnClick{on_click, ctx});
    w.add(e, EPDMode{2});                     // 反白/灰阶要 16 级波形，FAST 会把灰压没
    w.add(e, Dirty{});
    return e;
}

// 左对齐文字（设置行的标签、说明行）。和 input_text 同一路子，只是不带光标语义。
inline ecs::Entity setting_text(
    ecs::World& w,
    int32_t x, int32_t y, const char* text,
    uint32_t fg = 0x000000, uint32_t bg = 0xFFFFFF
) {
    using namespace ecs::components;
    auto e = w.spawn();
    w.add(e, Visible{});
    w.add(e, Rect{x, y, 0, 0});               // 尺寸由渲染实测
    w.add(e, Text{text});
    w.add(e, Style{fg, bg, 0, 0, 0, 0, 0});
    w.add(e, TextBox{});
    w.add(e, LeftAlign{});
    w.add(e, TextDirtyRange{0});
    w.add(e, EPDMode{2});                     // 灰字必须 16 级波形
    w.add(e, Dirty{});
    return e;
}

inline ecs::Entity flex(
    ecs::World& w,
    int32_t x, int32_t y, int32_t width, int32_t height,
    ecs::components::FlexLayout::Direction dir = ecs::components::FlexLayout::COLUMN
) {
    using namespace ecs::components;
    auto e = w.spawn();
    w.add(e, Visible{});
    w.add(e, Rect{x, y, width, height});
    w.add(e, FlexLayout{dir, FlexLayout::JUSTIFY_START, FlexLayout::FLEX_START,
                        0, 0, 0, 0, 0});
    w.add(e, systems::flex_detail::Children{});
    return e;
}

inline void add_child(ecs::World& w, ecs::Entity parent, ecs::Entity child) {
    using namespace systems::flex_detail;
    auto* ch = w.get_mut<Children>(parent);
    if (ch && ch->count < 16) {
        ch->entities[ch->count++] = child;
    }
    w.add(child, Parent{parent});
    w.add(child, ecs::components::Dirty{});
}

} // namespace widgets

} // namespace papers3::ui