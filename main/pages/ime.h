#pragma once

// pages/ime.h — 第 2 页：输入法
//
// 这一页只干两件事：**建实体** + **把组件接起来**。
//   拼音逻辑 → text/ime.h              （g_ime）
//   键盘     → ui/keyboard.h           （g_kb，输出端 = 本页 on_sink_key）
//   候选栏   → ui/candidate_bar.h      （g_cand，点第 i 格回调本页 on_pick）
// 所以本页真正的「存储」就是下面那几行句柄，别的都是接线。
//
// 注：命名空间叫 ime_page 而不是 ime —— 叫 ime 会把外层的 m5ui::ime 遮住。
//
// 版面（540×960）：
//   状态栏   y=12/24       BACK + 模式（顶部，底部整块留给键盘）
//   输入框   y=72 h=612    已上屏文字 + 拼音条(正在拼) + 光标
//                           文字贴在覆盖层顶边的编辑条里（im_text_y 现算，不写死）
//   编辑条   y=636 h=48    覆盖层顶边 = IM_BAND_Y；弹窗模式下「正在打什么」也画在这里
//   候选栏   y=684 h=56    8 格（50px）+ 左右箭头
//   键盘     y=752 h=208   4 行 × 46（10/9/9/7 = 35 键）
//                           行4 是功能行：en/cn · 面板键 · ← · → · space · 标点 · OK
//                           ← → 用来挪插入点（挪完再打字即为就地插入）

#include <cstdio>
#include <cstring>
#include "pages/manager.h"
#include "ui/widgets.h"
#include "ui/icons.h"
#include "ui/keyboard.h"
#include "ui/candidate_bar.h"
#include "text/ime.h"

namespace m5ui::pages::ime_page {

using namespace m5ui::ui;              // pages/ 在 ui/ 之外，theme:: 之类从这里找
using namespace m5ui::ecs::components;
using namespace m5ui::ui::widgets;
using ecs::Entity;
namespace kb = m5ui::ui::kb;

// ── 版面常量 ──
inline constexpr int32_t IM_TEXT_X    = 30;   // 输入框内文字起点
// 键盘那一块固定占**屏幕底部 292px**（候选栏 72 + 间隔 12 + 键盘 208 = 292），
// 这样同一批实体既能当"输入法页"的下半屏，也能当**覆盖层**盖在别的页上（见 manager.h）。
inline constexpr int32_t IM_FRAME_Y   = 72;    // 输入框顶（给顶部 BACK/模式让出 62px）
inline constexpr int32_t IM_FRAME_H   = 612;   // 72..684，正好接住下面的候选栏
inline constexpr int32_t IM_PAD_BOTTOM = 18;  // 文字距输入框底的内边距

inline constexpr int32_t IM_CAND_Y   = 690;   // 候选栏顶（684→690：收窄与键盘的 12px 空隙，见 2026-10-17 用户「候选区和按键之间空隙有点大」）
// ★ 原来 72px：格里的字才 34px（行高 33），上下各留 19.5px —— 是按键（46px，留 6.5px）的
//   三倍松，"这排最胖"。收到 56px（上下各留 11.5px）。省下的 16px 让整条带从 340 收到 324，
//   也就是弹窗少盖 16px 内容、输入框多 16px。
inline constexpr int32_t IM_CAND_H   = 56;
inline constexpr int32_t IM_KB_Y     = 752;   // 键盘首行
// ★ 弹窗顶部那条「编辑条」（48px）：显示正在打的那一行。
//   为什么必须有：输入框要是在屏幕下半部、被弹窗盖住，没有它就看不到自己在打什么。
//   （iOS 的输入法就是这条。）
inline constexpr int32_t IM_STRIP_H  = 48;
inline constexpr int32_t IM_BAND_Y   = IM_CAND_Y - IM_STRIP_H;   // 620 = 覆盖层顶边
inline constexpr int32_t IM_BAND_H   = 960 - IM_BAND_Y;          // 340
// 编辑条右侧「×」关闭按钮（取消输入，不交文字）
inline constexpr int32_t IM_CLOSE_X  = 476;
inline constexpr int32_t IM_CLOSE_W  = 64;    // 476~540

// ★ 那一行文字贴在**编辑条**里（弹窗顶部 48px 那条），不跟输入框外框的底走 ——
//   这样整页模式和弹窗模式显示的是**同一个位置**，弹窗模式下也能看见。
// ★ 文字的 y / 光标的 y **必须是算出来的**，不能写死常量。
// 字号是全局设置（24/28/34px），行高跟着它变（size=34 → line_height=33）。
// 主页 demo.h 一直是 `line_height()` 现算的（`g_cursor_y = g_text_y + lh + 2`）；
// 本页原来写成 constexpr、按「行高 27」推的固定值 —— 字号一放大，文字和光标就脱钩：
// 文字仍按 27 贴底、光标仍按 27 偏移，而实际行高 33，
// 于是光标陷进字形的 descender 里，看着像"贴在字上"，和主页不是一回事。
inline int32_t im_text_y() {
    return IM_BAND_Y + (IM_STRIP_H - m5ui::ttf::line_height()) / 2;   // 编辑条里垂直居中
}
inline int32_t im_cursor_y() { return im_text_y() + m5ui::ttf::line_height() + 2; }
inline constexpr int32_t IM_ROW_H    = 46;    // 键高（原 128）
inline constexpr int32_t IM_ROW_GAP  = 8;
inline constexpr int32_t IM_KEY_GAP  = 6;
inline constexpr int32_t IM_X0       = 10;    // 键盘左边距
inline constexpr int32_t IM_W        = 520;   // 键盘可用宽

// ══════════ 本页存储 ══════════
inline ime::Context  g_ime;                   // 拼音状态机
inline kb::Keyboard  g_kb;                    // 键盘组件
inline cb::Bar       g_cand;                  // 候选栏组件
inline Entity g_text, g_cursor;
inline Entity g_close;                     // 编辑条右侧 ×（只画字，触摸走 tick() 自持轮询）
inline char   g_mode_buf[48] = "en";

// ★ 输入框那一行 = **一个实体、一个字符串**：前缀 + 正在拼的拼音 + 后缀。
//
//   为什么不拆成多个实体（"正文 + 拼音条"两个，或者"前缀/拼音/后缀"三个）：
//   它们共用同一条基线、坐标互相错开，而渲染是**每个实体自己擦自己的旧框** ——
//   谁擦的那一笔都可能落在邻居刚画好的像素上。两种必然发生的情况：
//     · 上屏那一刻，拼音原位换成汉字（拼音的旧框正好压在汉字上）；
//     · 拼音变长，把后缀整体往右推（后缀的旧框正好压在拼音新字形上）。
//   把实体按任何顺序画都救不了（实体之间没有 z 序机制）。
//   合成**一个**字符串后，变化只发生在同一个实体内部，"擦旧框 + 重画"一趟做完，天然自洽。
inline char   g_line[200] = {0};

// 候选翻页用的两个「假键」：位置在候选栏上，不占键盘的 33 键网格
inline kb::Key g_prev{"", kb::K_PREV, 0};
inline kb::Key g_next{"", kb::K_NEXT, 0};

// ══════════ 刷新 ══════════
inline void refresh_cands() {
    g_cand.refresh(pages::g_ctx.app->world, g_ime.cand, g_ime.cand_n);
}

// 光标位置 = 文字起点 + (前缀 + 拼音) 的宽度 —— 也就是"正在拼的拼音之后"。
// 纯挪插入点时只动它（正文和显示串都没变）。
inline void refresh_cursor_only() {
    auto& w = pages::g_ctx.app->world;
    if (auto* cr = w.get_mut<Rect>(g_cursor))
        cr->x = IM_TEXT_X + m5ui::ttf::measure_prefix(g_ime.text, g_ime.caret)
                         + m5ui::ttf::measure_prefix(g_ime.py, g_ime.py_len);
    w.add(g_cursor, Dirty{});
}

// 那一行变了（正文 / 拼音 / 光标）：重建显示串 + 重画文字。
// from = **变化起点**（字符下标）：上屏/插入 = 光标位；退格 = 光标位 - 1；0 = 整段重画。
//        前缀长度恒等于光标位，所以正文的下标和这个串的下标天然对齐，不用换算。
inline void refresh_text(int from = 0) {
    auto& w = pages::g_ctx.app->world;
    const int off = ime::u8_offset(g_ime.text, g_ime.caret);
    snprintf(g_line, sizeof(g_line), "%.*s%s%s", off, g_ime.text, g_ime.py, g_ime.text + off);
    if (auto* t = w.get_mut<Text>(g_text)) t->str = g_line;
    if (auto* dr = w.get_mut<TextDirtyRange>(g_text)) dr->from_char = from;
    w.add(g_text, Dirty{});
    refresh_cursor_only();
}

// 按**当前字号**把输入框里的三样东西重新摆位。
// 字号是全局设置、在设置页改，改完本页实体不会自己动 —— 所以进页时重算一次。
// （y 变了要让文字整段重画；此时切页本来就会整屏刷白，不会留残影。）
inline void apply_metrics() {
    auto& w = pages::g_ctx.app->world;
    const int32_t ty = im_text_y();
    if (auto* r = w.get_mut<Rect>(g_text))   r->y = ty;
    if (auto* r = w.get_mut<Rect>(g_cursor)) r->y = im_cursor_y();
    refresh_text(0);         // 重建显示串 + 标脏（顺带把光标 x 摆正）
}

// 状态栏模式标签（覆盖层模式下没有顶部位置了，这里只更新 g_mode_buf 供调试/后续用）
inline void refresh_mode() {
    const char* m = (g_ime.mode == ime::MODE_CN)     ? "cn"
                  : (g_ime.mode == ime::MODE_SYMBOL) ? "123"
                                                     : "en";
    int pages_n = ime::page_count(g_ime);
    if (pages_n > 1)
        snprintf(g_mode_buf, sizeof(g_mode_buf), "%s  %d/%d", m, g_ime.page + 1, pages_n);
    else
        snprintf(g_mode_buf, sizeof(g_mode_buf), "%s", m);
}

// ══════════ 接线 ══════════
// 键盘的输出端：本地输入法。键盘只管报「按了什么键 + 修饰键状态」，怎么落地由这里决定。
// 以后要接 USB HID，照这个签名再写一个输出端丢给 TinyUSB 就行，keyboard.h 一行都不用改。
inline void on_sink_key(void*, const kb::Key& k, uint8_t mods) {
    if (k.kind == kb::K_ENTER) {              // 完成：把文字交给「打开输入法的那个页面」
        if (g_ime.py_len > 0 && g_ime.cand_n > 0) ime::commit(g_ime, 0);
        if (pages::g_ime_done) pages::g_ime_done(g_ime.text);   // 谁打开的就交给谁
        if (pages::g_overlay_on) pages::hide_overlay();          // 收起弹窗，留在当前页
        return;
    }
    ime::Change ch = ime::press(g_ime, k, mods);
    if (ch.text)       refresh_text(ch.from);  // 只从变化点往后重画
    else if (ch.strip) refresh_cursor_only();  // 只有插入点动了（正文和显示串都没变）
    if (ch.cand)  refresh_cands();
    if (ch.cand || ch.panel) refresh_mode();              // 状态栏带候选页码
    if (ch.panel) g_kb.set_label(kb::SLOT_MODE, (g_ime.mode == ime::MODE_CN) ? "cn" : "en");
}

// 候选栏：点了第 i 格
inline void on_pick(void* ctx) {
    int idx = (int)(intptr_t)ctx;
    if (idx < 0 || idx >= g_ime.cand_n) return;
    ime::Change ch = ime::commit(g_ime, idx);
    if (ch.text) refresh_text(ch.from);
    // ★ 只有候选列表**真的换了**才刷候选栏和状态行。
    //   上屏之后列表是"故意保留"的（要能连点同一个字反复上屏），所以这里 ch.cand 为假 ——
    //   省下的正是一次 780ms 的 TEXT 刷新。状态行的页码也没变，同样不用刷。
    if (ch.cand) { refresh_cands(); refresh_mode(); }

    // 选字会给该候选的词频 +1：攒够几次就落盘一次，别每选一个字都写 SD
    static int n = 0;
    if (ime::g_table.dirty && ++n >= 16) { n = 0; ime::table_save(ime::g_table); }
}

// 候选栏：点了箭头（ctx = -1 / +1）
inline void on_page(void* ctx) {
    const kb::Key& k = ((int)(intptr_t)ctx < 0) ? g_prev : g_next;
    ime::Change ch = ime::press(g_ime, k, g_kb.st.mods);
    if (ch.cand) refresh_cands();
    refresh_mode();
}

inline void on_back_to_main(void*) { pages::back(); }

// ══════════ 进覆盖层钩子 ══════════
// 打开这一页时，把「要接着编辑的原文」装进输入法（谁打开输入法，谁设 g_ime_seed）。
// 不设就是空着输；设了就等于是「改」而不是「新输入」。
inline void on_enter_preset() {
    apply_metrics();     // 字号可能刚在设置页改过，进页先按当前行高摆位
    snprintf(g_ime.text, sizeof(g_ime.text), "%s", pages::g_ime_seed ? pages::g_ime_seed : "");
    g_ime.caret  = ime::u8_count(g_ime.text);
    g_ime.py_len = 0; g_ime.py[0] = 0;
    ime::candidates(g_ime);
    refresh_text();
    refresh_cands();
    refresh_mode();
}

// ══════════ 对外：把键盘当弹窗吊起来（不切页）══════════
// 调用方先设 pages::g_ime_seed（要接着改的原文）和 pages::g_ime_done（完成后交给谁），
// 再调这里。被盖住的内容由 manager 在 hide_overlay() 时从实体树重画还原。
inline void open_as_overlay() {
    on_enter_preset();                  // 装原文 + 重建显示串 + 摆光标
    pages::show_overlay(IM_BAND_Y);     // 挂 Visible；那条带交给 flush_band 用 QUALITY 推
}

// ══════════ 编辑条右侧 × 关闭按钮 ══════════
// 自持触摸轮询（按下即取消）。不挂 Touchable：.so app 独占屏幕时固件 touch 系统直接
// return（见 systems/touch.h:40），所以和键盘/候选栏一样自己轮询。
// 每帧由 app.h 经 g_ime_tick 调用（build() 里登记）。
inline void tick() {
    if (!pages::g_overlay_on) return;
    if (M5.Touch.getCount() == 0) return;
    auto& d = M5.Touch.getDetail(0);
    if (d.wasPressed() &&
        d.x >= IM_CLOSE_X && d.x < IM_CLOSE_X + IM_CLOSE_W &&
        d.y >= IM_BAND_Y && d.y < IM_BAND_Y + IM_STRIP_H) {
        pages::hide_overlay();                    // 收起弹窗，留在当前页
        if (pages::g_ime_cancel) pages::g_ime_cancel();   // 通知打开者：取消了
    }
}

// ══════════ 建覆盖层（输入法现在是固件能力，不是页面）═════════
// 只建覆盖层实体（候选栏 + 键盘 + 编辑条），全部记进 g_overlay。
// 任何页调 pages::g_open_overlay() 把这套键盘当弹窗吊起来。
inline void build(ecs::World& w) {
    pages::overlay_begin();     // 覆盖层的实体表清零（输入法负责装满它）

    auto O = [](ecs::Entity e) { return pages::track_overlay(e); };

    // ── 覆盖层的容器（弹窗底）：编辑条白底 + 顶边一道 4px 黑线 ──
    // ★ 必须建在文字/候选栏/键盘之前（渲染顺序 = 创建顺序），否则会把整条带盖成一片白。
    // 键盘区（编辑条以下）整块铺点阵底：候选栏、键盘、它们之间的空隙、左右边距连成一片，
    // 不再有「点阵被白缝割裂」的问题（2026-10-17 用户：整个键盘区都用点阵背景，现在有点割裂）。
    O(rect(w, 0, IM_BAND_Y, 540, IM_STRIP_H, theme::BG_WHITE));                       // 编辑条白底（620-668）
    O(rect(w, 0, IM_BAND_Y + IM_STRIP_H, 540, IM_BAND_H - IM_STRIP_H, kb::KB_DOT));  // 键盘区点阵底（668-960）
    O(rect(w, 0, IM_BAND_Y, 540, 4, theme::BG_BLACK));

    // ★ 编辑条里的那一行文字 + 光标：弹窗模式下必须能看见「正在打什么」。
    g_text   = O(input_text(w, IM_TEXT_X, im_text_y(), g_line));
    g_cursor = O(cursor(w, IM_TEXT_X, im_cursor_y(), 18, 3));
    // 编辑条右侧「×」关闭按钮（只画字形；触摸由 tick() 自持轮询）
    g_close  = O(label(w, IM_CLOSE_X, im_text_y(), "×", 0x000000, 0xFFFFFF));

    // 候选栏组件
    g_cand.track   = O;
    g_cand.on_pick = on_pick;
    g_cand.on_page = on_page;
    g_cand.build(w, IM_CAND_Y, IM_CAND_H, 10, 520);

    // 键盘组件（输出端换成 HID 的话，这里一行不改）
    g_kb.sink    = kb::Sink{on_sink_key, nullptr};
    g_kb.x       = IM_X0;
    g_kb.y       = IM_KB_Y;
    g_kb.w       = IM_W;
    g_kb.row_h   = IM_ROW_H;
    g_kb.row_gap = IM_ROW_GAP;
    g_kb.key_gap = IM_KEY_GAP;
    g_kb.build(w, O);

    // 登记覆盖层入口：任何页都能 `pages::g_open_overlay()` 把这套键盘当弹窗吊起来
    pages::g_open_overlay = open_as_overlay;
    // 登记 × 关闭按钮的每帧轮询（app.h 主循环经 g_ime_tick 调）
    g_ime_tick = tick;

    // ★ 初始隐藏：工厂建实体时默认挂了 Visible，这里统一摘掉 ——
    //   否则开机键盘直接浮在首页（原来靠 main.cpp 的 hide(g_ime) 摘，现在没有页了）。
    for (int i = 0; i < pages::g_overlay.n; i++) w.remove<Visible>(pages::g_overlay.e[i]);

    // ★ set_label 必须在摘掉 Visible **之后**：build 刚结束时容器还挂着 Visible，
    //   set_label 会同步 draw_key(SLOT_MODE) → 开机左下角闪一下「en」键（2026-10-17 用户报）。
    //   移到隐藏之后，set_label 只改键面数据、不画不推。
    g_kb.set_label(kb::SLOT_MODE, (g_ime.mode == ime::MODE_CN) ? "cn" : "en");
}

// 开机先把候选栏/拼音条/状态栏刷成初始态（此时本页还没显示，只是把数据摆好）
inline void refresh_all() {
    refresh_cands();
    refresh_text(0);     // 重建显示串（此时是空的）+ 摆光标
    refresh_mode();
}

} // namespace m5ui::pages::ime_page
