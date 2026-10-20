#pragma once

// ──────────────────────────────────────────────
// keyboard.h — 屏幕键盘组件（独立成型，自持绘制/命中）
//
// 职责边界（只有这三件，别的都不管）：
//   1. 键面数据 + 网格几何（行结构 / 每行几键 / 每键多大）
//   2. 面板切换（字母页 / 数字标点页 / 英文符号页）、修饰键状态（大小写）
//   3. 把「按了哪个键」交出去 —— 落地方式由**输出端 Sink** 决定
//
// ★ 实体预算（2026-10-09）：键盘**只有一个 ECS 实体** —— 一个容器
//   （Rect + Style + CustomDraw + Touchable + OnClick + EPDMode{2}），35 个键面
//   全部由 CustomDraw 画进帧缓冲。以前是 35 键 × 1 实体/键，纯为蹭通用 render/touch；
//   改后省 34 个实体额度（MAX_ENTITIES=256 且永不回收）。
//
//   三个接口怎么接的：
//     · 绘制：容器的 CustomDraw 把 35 个键面画进帧缓冲；整块推送交给渲染器的
//       epd_push —— 键盘只以覆盖层形态出现，画容器时带必然生效（band 拦截），
//       由 pages::flush_band() 用 QUALITY 一次推完。
//     · 命中：容器挂 Touchable+OnClick（零新增机制）。touch 系统松开沿派发
//       OnClick，回调里现读 M5.Touch 坐标、对着 geo[] 算命中的是哪个键 ——
//       键是数据不是实体，命中不靠实体查询。
//     · 切页/弹窗显隐：容器挂/摘 Visible，与其它实体机制完全一致。
//
// 输出端可插拔：
//   ① 本地输入法：按键喂给 ime.h 的状态机（进输入框 / 拼拼音）—— 本项目实现的就是这个
//   ② USB HID：直接取 hid_for_key() 发标准 boot 报文（修饰键字节 + 6 个键码）
//      —— **这是本组件要具备的能力，不是本项目要实现的功能**：
//      键→HID 映射已经备好，以后加输出端不用再补映射表。
//      USB 设备侧（esp_tinyusb / CDC+HID 复合）不在本组件范围内。
//
// 依赖方向：keyboard.h 不认识 ime.h；是 ime.h 认识 keyboard.h（本地输出端消费按键）。
// ──────────────────────────────────────────────

#include <cstdint>
#include <cstring>
#include "ecs/world.hpp"
#include "ecs/components.h"
#include "ui/widgets.h"
#include "ui/icons.h"
#include "text/ttf.h"
#include "platform/nvs.h"       // cfg::g_cfg（墨色档；描边档不再吃 —— 键盘固定 1px）
#include "systems/render.h"     // epd_push / apply_epd_mode（键盘自己画、自己推）

namespace m5ui::ui::kb {

// ── 按键语义 ──
enum Kind : uint8_t {
    K_CHAR = 0,    // 输入 ch 这个字符
    K_SHIFT,       // 大小写（修饰键，键盘自己消化）
    K_BACKSPACE,   // 退格
    K_SPACE,       // 空格
    K_ENTER,       // 回车 / 完成
    K_MODE,        // 中/英切换（本地输出端语义；HID 无对应）
    K_SYMBOL,      // 面板切换 ?123 / ABC（键盘自己消化）
    K_PREV,        // 候选上一页
    K_NEXT,        // 候选下一页
    K_LEFT,        // 光标左移（正文不动，只挪插入点）
    K_RIGHT,       // 光标右移
};

struct Key {
    const char* label;   // 键面（UTF-8；**只能用字体里真有的字**，否则上屏是方块）
    uint8_t     kind;
    uint32_t    ch;      // kind == K_CHAR 时有效（码位）
};

// ── USB HID 键盘映射（usage page 0x07）──
// 标准 boot 报文：byte0 = 修饰键位图，byte2..7 = 最多 6 个 usage id。
// 这里只做「键 → HID」映射；发报文是输出端的事。
enum Mod : uint8_t {
    MOD_LCTRL = 0x01, MOD_LSHIFT = 0x02, MOD_LALT = 0x04, MOD_LGUI = 0x08,
    MOD_RCTRL = 0x10, MOD_RSHIFT = 0x20, MOD_RALT = 0x40, MOD_RGUI = 0x80,
};

enum Usage : uint8_t {
    USAGE_NONE  = 0x00,
    USAGE_ENTER = 0x28, USAGE_ESC = 0x29, USAGE_BKSP = 0x2A,
    USAGE_TAB   = 0x2B, USAGE_SPACE = 0x2C, USAGE_DEL = 0x4C,
    USAGE_RIGHT = 0x4F, USAGE_LEFT = 0x50, USAGE_DOWN = 0x51, USAGE_UP = 0x52,
};

struct HidCode {
    uint8_t usage;   // usage id；0 = 发不出（交给电脑自己的输入法）
    uint8_t mod;     // 需要同时按住的修饰键（大写字母 / 上档符号）
};

// 字符 → HID（US 布局）。非 ASCII 返回 {0,0}：
// 中文标点这类电脑键盘**按不出来**（得靠电脑自己的输入法），别硬发。
inline HidCode hid_for_char(uint32_t c) {
    if (c >= 'a' && c <= 'z') return {(uint8_t)(0x04 + (c - 'a')), 0};
    if (c >= 'A' && c <= 'Z') return {(uint8_t)(0x04 + (c - 'A')), MOD_LSHIFT};
    if (c >= '1' && c <= '9') return {(uint8_t)(0x1E + (c - '1')), 0};
    switch (c) {
        case '0':  return {0x27, 0};
        case ' ':  return {USAGE_SPACE, 0};
        case '-':  return {0x2D, 0};          case '_':  return {0x2D, MOD_LSHIFT};
        case '=':  return {0x2E, 0};          case '+':  return {0x2E, MOD_LSHIFT};
        case '[':  return {0x2F, 0};          case '{':  return {0x2F, MOD_LSHIFT};
        case ']':  return {0x30, 0};          case '}':  return {0x30, MOD_LSHIFT};
        case '\\': return {0x31, 0};          case '|':  return {0x31, MOD_LSHIFT};
        case ';':  return {0x33, 0};          case ':':  return {0x33, MOD_LSHIFT};
        case '\'': return {0x34, 0};          case '"':  return {0x34, MOD_LSHIFT};
        case '`':  return {0x35, 0};          case '~':  return {0x35, MOD_LSHIFT};
        case ',':  return {0x36, 0};          case '<':  return {0x36, MOD_LSHIFT};
        case '.':  return {0x37, 0};          case '>':  return {0x37, MOD_LSHIFT};
        case '/':  return {0x38, 0};          case '?':  return {0x38, MOD_LSHIFT};
        case '!':  return {0x1E, MOD_LSHIFT}; case '@':  return {0x1F, MOD_LSHIFT};
        case '#':  return {0x20, MOD_LSHIFT}; case '$':  return {0x21, MOD_LSHIFT};
        case '%':  return {0x22, MOD_LSHIFT}; case '^':  return {0x23, MOD_LSHIFT};
        case '&':  return {0x24, MOD_LSHIFT}; case '*':  return {0x25, MOD_LSHIFT};
        case '(':  return {0x26, MOD_LSHIFT}; case ')':  return {0x27, MOD_LSHIFT};
        default:   return {USAGE_NONE, 0};
    }
}

inline HidCode hid_for_key(const Key& k) {
    switch (k.kind) {
        case K_CHAR:      return hid_for_char(k.ch);
        case K_SHIFT:     return {USAGE_NONE, MOD_LSHIFT};   // 本身是修饰键
        case K_BACKSPACE: return {USAGE_BKSP, 0};
        case K_SPACE:     return {USAGE_SPACE, 0};
        case K_ENTER:     return {USAGE_ENTER, 0};
        case K_PREV:      return {USAGE_LEFT, 0};            // 翻页 → 电脑上的左右方向键
        case K_NEXT:      return {USAGE_RIGHT, 0};
        case K_LEFT:      return {USAGE_LEFT, 0};            // 光标左移 → 电脑上的左方向键
        case K_RIGHT:     return {USAGE_RIGHT, 0};
        default:          return {USAGE_NONE, 0};            // K_MODE / K_SYMBOL：无对应
    }
}

// ── 网格几何：4 行（10 / 9 / 9 / 7 = 35 键）──
// 三个面板行结构完全一样，所以只换 slot[] 的键面数据，几何/实体纹丝不动。
// 行4 从 5 键加到 7 键，是为了塞进光标 ← →：没有它们就只能追加，改不了中间的字。
// 高度不变（还是 4 行），只是每键窄一点 —— 键宽按最大字号实测过（见 build 里 fw 的注释）。
static constexpr uint8_t ROW_COUNT  = 4;
static constexpr uint8_t ROW_KEYS[ROW_COUNT] = {10, 9, 9, 7};
static constexpr uint8_t KEY_TOTAL  = 35;

// 面板数：0 字母 / 1 数字·中文标点 / 2 英文符号
static constexpr uint8_t PANEL_COUNT = 3;

// 固定槽位（面板无关）
static constexpr uint8_t SLOT_SHIFT = 19;   // 行3 首键：字母页 = 大小写
static constexpr uint8_t SLOT_DEL   = 27;   // 行3 末键：退格（键面自绘）
static constexpr uint8_t SLOT_MODE  = 28;   // 行4 首键：en / cn（键面归输出端管）
static constexpr uint8_t SLOT_PANEL = 29;   // 行4 第 2 键：面板切换（键面 = 下一个面板名）
static constexpr uint8_t SLOT_LEFT  = 30;   // 行4 第 3 键：光标左移
static constexpr uint8_t SLOT_RIGHT = 31;   // 行4 第 4 键：光标右移

// 键盘区底色 = 点阵（与 terminal/设置同款：FASTEST 档写 L14 → 1bit Bayer 点阵）
static constexpr uint32_t KB_DOT = 0xE8E8E8;

// 键面标签一律用「中英都能看懂」的写法：en/cn、?123、#+=、ABC、space、OK。
// 大小写键和退格键是**图元自绘**（字体里没有 ⇧ / ⌫），所以 label 留空。
// 光标键用文字 ← → ：这两个码位已经加进内置子集（Arrows 区块）。
// **不要**改用 icons::triangle —— 那是候选栏翻页的图形，同屏出现两种三角会混淆。
// 面板键的 label = **按下去会去哪个面板**（字母页写 ?123，数字页写 #+=，符号页写 ABC）。
#define KBK_(l, k, c) {l, k, c}

// 行4 七格（35 键版）：en/cn · 面板键 · ← · → · space · 标点 · OK
#define KBK_ROW4(panellab) \
    KBK_("en",K_MODE,0), KBK_(panellab,K_SYMBOL,0), KBK_("←",K_LEFT,0), KBK_("→",K_RIGHT,0), \
    KBK_("space",K_SPACE,0)

// 面板 0：字母
static const Key LAYOUT_ALPHA[KEY_TOTAL] = {
    KBK_("q",K_CHAR,'q'), KBK_("w",K_CHAR,'w'), KBK_("e",K_CHAR,'e'), KBK_("r",K_CHAR,'r'), KBK_("t",K_CHAR,'t'),
    KBK_("y",K_CHAR,'y'), KBK_("u",K_CHAR,'u'), KBK_("i",K_CHAR,'i'), KBK_("o",K_CHAR,'o'), KBK_("p",K_CHAR,'p'),

    KBK_("a",K_CHAR,'a'), KBK_("s",K_CHAR,'s'), KBK_("d",K_CHAR,'d'), KBK_("f",K_CHAR,'f'), KBK_("g",K_CHAR,'g'),
    KBK_("h",K_CHAR,'h'), KBK_("j",K_CHAR,'j'), KBK_("k",K_CHAR,'k'), KBK_("l",K_CHAR,'l'),

    KBK_("",K_SHIFT,0), KBK_("z",K_CHAR,'z'), KBK_("x",K_CHAR,'x'), KBK_("c",K_CHAR,'c'), KBK_("v",K_CHAR,'v'),
    KBK_("b",K_CHAR,'b'), KBK_("n",K_CHAR,'n'), KBK_("m",K_CHAR,'m'), KBK_("",K_BACKSPACE,0),

    KBK_ROW4("?123"), KBK_("。",K_CHAR,0x3002), KBK_("OK",K_ENTER,0),
};

// 面板 1：数字 + 中文标点
static const Key LAYOUT_SYMBOL[KEY_TOTAL] = {
    KBK_("1",K_CHAR,'1'), KBK_("2",K_CHAR,'2'), KBK_("3",K_CHAR,'3'), KBK_("4",K_CHAR,'4'), KBK_("5",K_CHAR,'5'),
    KBK_("6",K_CHAR,'6'), KBK_("7",K_CHAR,'7'), KBK_("8",K_CHAR,'8'), KBK_("9",K_CHAR,'9'), KBK_("0",K_CHAR,'0'),

    KBK_("，",K_CHAR,0xFF0C), KBK_("。",K_CHAR,0x3002), KBK_("、",K_CHAR,0x3001), KBK_("；",K_CHAR,0xFF1B), KBK_("：",K_CHAR,0xFF1A),
    KBK_("？",K_CHAR,0xFF1F), KBK_("！",K_CHAR,0xFF01), KBK_("“",K_CHAR,0x201C), KBK_("”",K_CHAR,0x201D),

    KBK_("（",K_CHAR,0xFF08), KBK_("）",K_CHAR,0xFF09), KBK_("《",K_CHAR,0x300A), KBK_("》",K_CHAR,0x300B), KBK_("【",K_CHAR,0x3010),
    KBK_("】",K_CHAR,0x3011), KBK_("—",K_CHAR,0x2014), KBK_("…",K_CHAR,0x2026), KBK_("",K_BACKSPACE,0),

    KBK_ROW4("#+="), KBK_("￥",K_CHAR,0xFFE5), KBK_("OK",K_ENTER,0),
};

// 面板 2：英文符号（行3 末格固定是退格）
// 顺序按「中文用户实际会打的英文符号」排：最常用的 `. , - _` 放第一行第一个，
// 减号在第 3 个 —— 之前整个键盘没有减号，这条就是为它加的。
static const Key LAYOUT_MORE[KEY_TOTAL] = {
    KBK_(".",K_CHAR,'.'), KBK_(",",K_CHAR,','), KBK_("-",K_CHAR,'-'), KBK_("_",K_CHAR,'_'), KBK_("'",K_CHAR,'\''),
    KBK_("\"",K_CHAR,'"'), KBK_("?",K_CHAR,'?'), KBK_("!",K_CHAR,'!'), KBK_(":",K_CHAR,':'), KBK_(";",K_CHAR,';'),

    KBK_("/",K_CHAR,'/'), KBK_("\\",K_CHAR,'\\'), KBK_("@",K_CHAR,'@'), KBK_("#",K_CHAR,'#'), KBK_("(",K_CHAR,'('),
    KBK_(")",K_CHAR,')'), KBK_("&",K_CHAR,'&'), KBK_("%",K_CHAR,'%'), KBK_("$",K_CHAR,'$'),

    KBK_("=",K_CHAR,'='), KBK_("+",K_CHAR,'+'), KBK_("*",K_CHAR,'*'), KBK_("[",K_CHAR,'['), KBK_("]",K_CHAR,']'),
    KBK_("{",K_CHAR,'{'), KBK_("}",K_CHAR,'}'), KBK_("|",K_CHAR,'|'), KBK_("",K_BACKSPACE,0),

    KBK_ROW4("ABC"), KBK_("￥",K_CHAR,0xFFE5), KBK_("OK",K_ENTER,0),
};

#undef KBK_ROW4

#undef KBK_

// ── 输出端：键盘只报「按了什么键 + 修饰键状态」，落地由输出端管 ──
struct Sink {
    // mods 见 Mod：既是 HID 报文内容，也决定本地输出的大小写
    void (*on_key)(void* ctx, const Key& k, uint8_t mods) = nullptr;
    void* ctx = nullptr;
};

// ── 键盘状态 ──
struct State {
    uint8_t panel = 0;   // 0 = 字母 / 1 = 数字·中文标点 / 2 = 英文符号（见 PANEL_COUNT）
    uint8_t mods  = 0;   // 见 Mod
};

// ── 自绘键面：大小写键 A/a 两个字分别上色（当前档黑 / 另一档灰）──
// 注意：调用前 ttf 字号必须已是键盘字号（draw_face 统一 set_size）。
inline void draw_shift_key(const State& st, int32_t x, int32_t y, int32_t w, int32_t h,
                           uint32_t, uint32_t) {
    const uint32_t DIM = 0xA8A8A8;   // 非当前档：约 66% 灰（要在 TEXT/GL16 波形下才出得来）
    const bool upper = (st.mods & MOD_LSHIFT) != 0;
    const char* seg[3] = {"A", "/", "a"};
    int32_t tw[3] = {0, 0, 0}, th = 0, total = 0;
    for (int i = 0; i < 3; i++) {
        m5ui::ttf::measure(seg[i], &tw[i], &th);
        total += tw[i];
    }
    const uint32_t col[3] = { upper ? 0x000000 : DIM, 0x000000, upper ? DIM : 0x000000 };
    int32_t px = x + (w - total) / 2;
    const int32_t py = y + (h - th) / 2;
    for (int i = 0; i < 3; i++) {
        m5ui::ttf::draw_text(px, py, seg[i], col[i], 0xFFFFFF);
        px += tw[i];
    }
}

// ── 键盘实例 ──
// 单实例组件：回调都是裸函数指针，拿不到 this，所以留一个实例指针。
struct Keyboard;

inline Keyboard* g_inst = nullptr;   // 由 build() 设置

inline void kb_draw_thunk(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t fg, uint32_t bg);

struct Keyboard {
    State       st;
    Sink        sink;
    ecs::World* world = nullptr;
    ecs::Entity self  = ecs::INVALID_ENTITY;   // ★ 唯一实体：容器（自绘全部键面）

    Key         slot[KEY_TOTAL] = {};

    // 每键几何（build 时预计算；绘制/命中共用这一份 —— 同源，不会出现「画在这、点不中」）
    struct KeyGeo { int16_t x, y, w, h; };
    KeyGeo      geo[KEY_TOTAL] = {};
    int32_t     total_h = 0;               // 键盘区总高（4×46 + 3×8 = 208）
    int8_t      pressed_i = -1;            // 正在按的键（触摸反馈用）

    // SLOT_MODE 的键面是输出端给的（en / cn，跟中文模式走），不是布局表里的常量。
    // 布局表里那个 "en" 只是初值 —— 切面板时必须把这个还原回去，
    // 否则「中文模式 → 点一下 ?123」会把键面冲回 en，而状态栏还写着 cn。
    const char* mode_lab = "en";
    int32_t     x = 10, y = 0, w = 520, row_h = 46, row_gap = 8, key_gap = 6;

    // ── 建：预计算几何 + 建那一个容器实体 ──
    // reg 把实体登记给页面/覆盖层（切显隐用），原型同 track()
    void build(ecs::World& w0, ecs::Entity (*reg)(ecs::Entity)) {
        using namespace ecs::components;
        world = &w0;
        g_inst = this;

        // 几何：和旧版逐键 spawn 的算法一字不差（fw[] 按 34px 字号实测的标签宽留余量）
        int32_t ry = y;
        int ki = 0;
        for (int r = 0; r < ROW_COUNT; r++) {
            const int n = ROW_KEYS[r];
            int32_t x0;
            if (r == ROW_COUNT - 1) {                    // 功能行：空格键要宽
                // 行4 固定 7 键，宽度按**最大字号 34px** 实测的标签宽留的余量：
                //   space 76.8 / ?123 62.1 / ABC 57.0 / #+= 51.8 / OK 41.8 / ← → 29.0 / en 32.7
                // 合计 484 + 6×6(间隔) = 520 = IM_W，正好铺满一行。
                static const int32_t fw[7] = {52, 84, 50, 50, 134, 50, 64};
                int32_t total = (n - 1) * key_gap;
                for (int i = 0; i < n; i++) total += fw[i];
                x0 = x + (w - total) / 2;
                for (int i = 0; i < n; i++) {
                    geo[ki] = {(int16_t)x0, (int16_t)ry, (int16_t)fw[i], (int16_t)row_h};
                    x0 += fw[i] + key_gap;
                    ki++;
                }
            } else {
                int32_t kw = (w - (n - 1) * key_gap) / n;
                int32_t total = n * kw + (n - 1) * key_gap;
                x0 = x + (w - total) / 2;
                for (int i = 0; i < n; i++) {
                    geo[ki] = {(int16_t)x0, (int16_t)ry, (int16_t)kw, (int16_t)row_h};
                    x0 += kw + key_gap;
                    ki++;
                }
            }
            ry += row_h + row_gap;
        }
        total_h = (ry - row_gap) - y;                    // 208

        // 唯一的实体：容器。白底、无框无影（键面的 1px 描边由 draw_face 自己画）。
        // ★ 不挂 Touchable/OnClick：touch 系统给命中实体标脏是硬编码的，
        //   两次脏标落在 520×208 容器上 = 每点一键整块键盘重推两遍。
        //   触摸走 tick() 自轮询（app.h 每帧调），见那边注释。
        self = w0.spawn();
        w0.add(self, Visible{});
        w0.add(self, Rect{x, y, w, total_h});
        w0.add(self, Style{0x000000, KB_DOT, 0x000000, 0x000000, 0, 0, 0});   // 底 = 点阵
        w0.add(self, CustomDraw{kb_draw_thunk});
        w0.add(self, EPDMode{3});                        // 容器底走 FAST（2 级 → 点阵打底；FASTEST 实测出黑）
        w0.add(self, Dirty{});
        if (reg) reg(self);

        apply_panel(0);                                  // 填 slot[]（mark_dirty，画交给渲染）
    }

    // 面板号 → 布局表
    static const Key* layout_of(uint8_t p) {
        return p == 0 ? LAYOUT_ALPHA : (p == 1 ? LAYOUT_SYMBOL : LAYOUT_MORE);
    }

    bool shown() const {
        using namespace ecs::components;
        return world && self != ecs::INVALID_ENTITY && world->has<Visible>(self);
    }

    void mark_dirty() {
        using namespace ecs::components;
        if (world && self != ecs::INVALID_ENTITY)
            world->add(self, Dirty{});
    }

    // 切面板：只换 slot[] 键面数据，几何/实体不动；整块重画交给渲染
    void apply_panel(uint8_t panel) {
        if (panel >= PANEL_COUNT) panel = 0;
        st.panel = panel;
        const Key* src = layout_of(st.panel);
        for (int i = 0; i < KEY_TOTAL; i++) slot[i] = src[i];
        // SLOT_MODE 键面归输出端管，别被布局表里的 "en" 冲掉
        slot[SLOT_MODE].label = mode_lab;
        mark_dirty();
    }

    // 改某一格的键面（动态文案由外面给，比如 en/cn）。
    // 只重画**这一个键**（原来整块 mark_dirty 会整个键盘重推一遍，浪费）。
    // 不可见时不画（shown() 为假，draw_key 也只是白写帧缓冲，无屏推）。
    void set_label(int i, const char* s) {
        if (i < 0 || i >= KEY_TOTAL) return;
        slot[i].label = s;
        if (i == SLOT_MODE) mode_lab = s;   // 记住，切面板时还原（见 apply_panel）
        if (shown()) redraw_key_full(i);    // label 变了 → 全量重画这一个键
    }

    // ── 命中：点在哪个键上（-1 = 没在键盘上）──
    int hit(int32_t px, int32_t py) const {
        for (int i = 0; i < KEY_TOTAL; i++) {
            const KeyGeo& g = geo[i];
            if (px >= g.x && px < g.x + g.w && py >= g.y && py < g.y + g.h) return i;
        }
        return -1;
    }

    // ── 触摸：自己轮询（app.h 每帧调 tick()）──
    //
    // 为什么不挂 Touchable/OnClick：touch 系统给命中的实体标脏是硬编码的
    // （按下沿 Pressed+Dirty、松开沿再 Dirty）。容器挂了 Touchable，
    // 这两次脏标就落在 520×208 的容器上 → 每点一键整个键盘区被全幅重推两遍
    // （实测：烧录后用户报「整个字符按键区都会刷新」）。自己轮询就没有这回事：
    // 按下/松开都只推被按那一个键的小块，实体从不被外部标脏。
    //
    // 语义与旧版逐键实体一致：按下沿出反馈、松手（含长按 hold_end/滑出）交键。
    // K_SHIFT / K_SYMBOL 松手后键面由 press() 自己重画（翻 A/a 或整块换面板），
    // 所以不先弹起，避免多刷一次同一个键。
    void tick() {
        if (!shown() || !world) { pressed_i = -1; return; }
        if (M5.Touch.getCount() == 0) {
            if (pressed_i >= 0) release_key();       // 松手（就算滑出去了）：弹起 + 交键
            return;
        }
        auto& d = M5.Touch.getDetail(0);
        if (d.wasPressed()) {
            const int i = hit(d.x, d.y);
            if (i >= 0) {                            // 按下反馈：描边 1px→2px，只推这一小块
                pressed_i = (int8_t)i;
                draw_key(i, true);
            }
        } else if (d.wasReleased() && pressed_i >= 0) {
            release_key();
        }
    }

    void release_key() {
        const int i = pressed_i;
        pressed_i = -1;
        const bool self_redraw = (slot[i].kind == K_SHIFT || slot[i].kind == K_SYMBOL);
        if (!self_redraw) draw_key(i, false);        // 普通键：弹起恢复常态
        press(i);                                    // K_SHIFT/K_SYMBOL 内部自己重画
    }

    // 按下第 i 个键（语义消化 + 交输出端）
    // 触摸只在这里进来（松开沿），所以没有「按下态」可画 —— 键面直接以最终态重画：
    // K_SYMBOL 整块换面（mark_dirty 整推）、K_SHIFT 重画 A/a、其余交给输出端。
    void press(int i) {
        if (i < 0 || i >= KEY_TOTAL) return;
        const Key k = slot[i];                   // 值拷贝：apply_panel 会重写 slot[]
        if (k.kind == K_SYMBOL) { apply_panel((uint8_t)((st.panel + 1) % PANEL_COUNT)); return; }
        if (k.kind == K_SHIFT) {
            st.mods ^= MOD_LSHIFT;
            if (st.panel == 0) redraw_key_full(SLOT_SHIFT);  // A/a 两档重画
            return;
        }
        if (sink.on_key) sink.on_key(sink.ctx, k, st.mods);
    }

    // ── 绘制 ──
    // 全量画键面进帧缓冲（不推屏）：边框+内部白走 FASTEST（2 级，最干净、和点阵底同档），
    //   文字/图标走 TEXT（抗锯齿）。键面描边固定 1px、按下 2px，不吃全局描边档。
    void draw_face(int i, bool pressed) {
        const KeyGeo& g = geo[i];
        const int32_t bw = pressed ? 2 : 1;
        systems::apply_epd_mode(4);              // FASTEST：边框最干净
        M5.Display.startWrite();
        M5.Display.fillRect(g.x, g.y, g.w, g.h, TFT_BLACK);           // 描边层
        M5.Display.fillRect(g.x + bw, g.y + bw, g.w - 2*bw, g.h - 2*bw, TFT_WHITE);  // 内部白
        M5.Display.endWrite();

        systems::apply_epd_mode(2);              // TEXT：文字抗锯齿（画在内部白上）
        M5.Display.startWrite();
        const float keep = m5ui::ttf::size_px();
        m5ui::ttf::set_size((float)widgets::KB_FONT_PX);   // ★ 键面字号键盘自管
        if (i == SLOT_DEL) {
            icons::backspace(g.x, g.y, g.w, g.h, TFT_BLACK, TFT_WHITE);
        } else if (i == SLOT_SHIFT && st.panel == 0) {
            draw_shift_key(st, g.x, g.y, g.w, g.h, TFT_BLACK, TFT_WHITE);
        } else if (slot[i].label && slot[i].label[0]) {
            int32_t tw = 0, th = 0;
            m5ui::ttf::measure(slot[i].label, &tw, &th);
            m5ui::ttf::draw_text(g.x + (g.w - tw) / 2, g.y + (g.h - th) / 2,
                                 slot[i].label, cfg::g_cfg.ink, TFT_WHITE);
        }
        m5ui::ttf::set_size(keep);
        M5.Display.endWrite();
    }

    // 按下/弹起反馈：只改边框那一圈（1px↔2px），只推 4 条窄带 —— 中间白底+字不动、不闪。
    // （照用户 MicroPython 版：边框走 FASTEST 刷新，键面中间不刷）
    void draw_key(int i, bool pressed) {
        const KeyGeo& g = geo[i];
        const uint32_t c = pressed ? TFT_BLACK : TFT_WHITE;
        systems::apply_epd_mode(4);              // FASTEST：2 级，边框最快最干净
        M5.Display.startWrite();
        M5.Display.fillRect(g.x + 1, g.y + 1, g.w - 2, 1, c);          // 上（内圈 1px）
        M5.Display.fillRect(g.x + 1, g.y + g.h - 2, g.w - 2, 1, c);    // 下
        M5.Display.fillRect(g.x + 1, g.y + 1, 1, g.h - 2, c);          // 左
        M5.Display.fillRect(g.x + g.w - 2, g.y + 1, 1, g.h - 2, c);    // 右
        M5.Display.endWrite();
        systems::epd_push(g.x, g.y, g.w, 2);                          // 上带
        systems::epd_push(g.x, g.y + g.h - 2, g.w, 2);                // 下带
        systems::epd_push(g.x, g.y + 2, 2, g.h - 4);                  // 左带
        systems::epd_push(g.x + g.w - 2, g.y + 2, 2, g.h - 4);        // 右带
    }

    // 换键面/label 变了：全量重画 + 推整个键（文字变了才走这个，不是按下反馈）
    void redraw_key_full(int i) {
        draw_face(i, false);
        const KeyGeo& g = geo[i];
        systems::epd_push(g.x, g.y, g.w, g.h);
    }

    // 容器 CustomDraw：全部键面画进帧缓冲。
    // 整块推送不在这里做 —— 渲染器画完容器会自己 epd_push(容器矩形)，
    // 键盘只以覆盖层形态出现，那次推送必然落在带内被拦下，由 flush_band 一次推完。
    void draw_all() {
        for (int i = 0; i < KEY_TOTAL; i++) draw_face(i, false);
    }
};

inline void kb_draw_thunk(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t fg, uint32_t bg) {
    (void)x; (void)y; (void)w; (void)h; (void)fg; (void)bg;
    if (g_inst) g_inst->draw_all();
}

// app.h 每帧调：键盘自持触摸轮询（容器不挂 Touchable，见 tick() 注释）
inline void tick_g() {
    if (g_inst) g_inst->tick();
}

} // namespace m5ui::ui::kb
