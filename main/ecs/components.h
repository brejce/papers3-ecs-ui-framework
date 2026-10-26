#pragma once

#include <cstdint>

namespace papers3::ecs::components {

// ──────────────────────────────────────────────
// 组件定义 — 纯数据，没有方法
//
// 设计原则：
//   - 每个组件只管一件事
//   - 可以按需组合挂到 Entity 上
//   - System 查询这些组件来做行为
// ──────────────────────────────────────────────

// ── 位置和尺寸（所有可见组件都需要） ──
struct Rect {
    int32_t x = 0;
    int32_t y = 0;
    int32_t w = 0;
    int32_t h = 0;
};

// ── 文字内容 ──
struct Text {
    const char* str = "";
};

// ── 文字框：上一次真正画到屏幕上的文字尺寸 ──
// 渲染时用来擦掉旧内容：文字变短时，旧的宽框也得擦干净，不然留残影
// px 是上次画上去的 x —— 左对齐实体（如跟着光标走的拼音提示条）**位移**时，
// 只按当前 x 擦会漏掉旧位置，必须记住旧 x 一起擦。
struct TextBox {
    int32_t w  = 0;
    int32_t h  = 0;
    int32_t px = 0;
};

// ── 光标（独立实体，最快波形独立刷新） ──
struct Cursor {
    int32_t px = 0, py = 0;   // 上次画的位置（擦旧光标用）
    bool    drawn = false;    // 是否已画过
};

// ── 文字左对齐（默认居中） ──
struct LeftAlign {};

// ── 文字不铺底（字形直接压在下层像素上）──
// 终端风的底是点阵（浅灰走 FAST 量化出来的 Bayer 抖动），普通左对齐文字会用 Style.bg
// 先填一块实心底，在抖动底上就抠出一个纯灰方块。挂上它以后渲染器跳过 fillRect，
// 只画字形；ttf::push_glyph 本来就会 readRect 读回下层做混色，抗锯齿照样正确。
// ★ 只给「内容不变」的静态字用 —— 文字变了没人擦旧字形，得自己补一块 FAST 底重抖。
struct TextNoBg {};

// ── 文字局部重绘范围（只重绘从 from_char 开始的字符，0=整段） ──
struct TextDirtyRange {
    int32_t from_char = 0;
};

// ── 颜色（从主题取或自定义） ──
// Memphis/EPD 版：直角、黑描边、硬阴影（纯色偏移，无模糊）
struct Style {
    uint32_t fg        = 0;    // 前景色（文字/阴影）
    uint32_t bg        = 0;    // 背景色（也用作擦除底色）
    uint32_t border    = 0;    // 边框色
    uint32_t accent    = 0;    // 强调色（按下时描边/贴地）
    int32_t  radius    = 0;    // 圆角半径（0 = 直角，Memphis 一律 0）
    int32_t  border_w  = 0;    // 边框宽度（0 = 无边框；2 或 3）
    int32_t  shadow    = 0;    // 硬阴影偏移（0 = 无；向 +x/+y 偏移 3 或 5）
};

// ── 自绘元素 ──
// 字体里没有的符号（候选翻页箭头、退格图标…）别去凑字符，直接画图元。
// 渲染时会先画好底色/描边，再调这个回调；坐标是这个部件的左上角。
struct CustomDraw {
    void (*fn)(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t fg, uint32_t bg) = nullptr;
};

// ── 位图（图标）：1bpp 或 4bpp 灰度，行优先 ──
// 为什么不走 CustomDraw：那个回调没有 ctx，一个页面 N 个图标就只有一个全局指针可用。
// 这里把「哪块数据、多大、几位」挂在实体上，渲染器按 Rect 位置直接落图。
struct Bitmap {
    const uint8_t* data = nullptr;   // 位图字节（行优先，(w*bpp+7)/8 字节一行）
    int16_t  w = 0, h = 0;
    uint8_t  bpp = 1;                // 1 = 1bit（位=1 画墨）；4 = 4bit 灰（0=黑 15=白）
    uint32_t fg = 0x000000;
    uint32_t bg = 0xFFFFFF;
    int8_t   scale = 1;              // 整数倍最近邻放大（1 = 原尺寸）。app 图标是 32×32 的点阵，
                                     // 启动器一行有两行字（名称 26 + 简介 19 ≈ 54px），图标得跟上，
                                     // 所以按 2× 画成 64×64 —— 像素风放大不该插值，硬边才对。
};

// ── 可触摸 ──
struct Touchable {
    uint32_t mask = 0;         // TouchMode bit flags
};

// ── 按下状态（按钮等可交互组件） ──
struct Pressed {
    bool is_pressed = false;
};

// ── 点击回调 ──
struct OnClick {
    void (*fn)(void* ctx) = nullptr;
    void* ctx = nullptr;
};

// ── 可见性 ──
// 要画出来 / 能点到的实体都必须带 Visible（widgets.h 的工厂都会挂上）。
// 切页 = 摘掉 / 挂上 Visible —— 渲染和触摸都只认 Visible，没有「默认可见」的隐含状态。
struct Visible {};

// ── 双击回调 ──
// 挂了 OnDoubleClick 的实体走双击判定（窗口 400ms，见 systems/touch.h）：
// 第一下**不立刻**触发 OnClick，等窗口过期才补发单次 —— 所以双击不会顺带点出一次单击。
struct OnDoubleClick {
    void (*fn)(void* ctx) = nullptr;
    void* ctx = nullptr;
};

// ── 长按回调 ──
struct OnLongPress {
    void (*fn)(void* ctx) = nullptr;
    void* ctx = nullptr;
};

// ── 脏标记（需要重绘） ──
struct Dirty {};

// ── EPD 刷新模式 ──
struct EPDMode {
    uint8_t mode = 0;  // 1=QUALITY, 2=TEXT, 3=FAST, 4=FASTEST
};

// ── 部件自带字号（不跟全局「字号」设置走） ──
//
// 为什么要有它：键盘的键宽/行高/居中是按 34px **实测排的版**（见 keyboard.h build() 里
// fw[] 的注释），全局字号一改，键盘就挤得难看或空得难看。所以键盘那几个部件把自己的
// 字号写在这里，渲染时按部件切字号 —— "键盘上的字符由键盘自己管"。
//
// px = 0 表示"没声明 → 用全局 cfg::g_cfg.size"。
struct FontSize {
    int16_t px = 0;
};

// ── 固定边框宽度（终端风部件用：键盘键/候选格固定 1px，不吃全局「描边粗细」档）──
// 挂了这个组件，render 的描边走这个值，忽略 cfg::g_cfg.bw。
struct FixedBorderW {
    int16_t px = 1;
};

// ── Flex 布局 ──
struct FlexLayout {
    enum Direction : uint8_t { ROW = 0, COLUMN = 1 };
    enum Justify  : uint8_t { JUSTIFY_START=0, JUSTIFY_END, JUSTIFY_CENTER, SPACE_BETWEEN, SPACE_AROUND, SPACE_EVENLY };
    enum Align    : uint8_t { FLEX_START=0, FLEX_END, ALIGN_CENTER, STRETCH };

    Direction direction = COLUMN;
    Justify  justify    = JUSTIFY_START;
    Align    align      = FLEX_START;
    int32_t  gap        = 0;
    int32_t  pad_top    = 0;
    int32_t  pad_right  = 0;
    int32_t  pad_bottom = 0;
    int32_t  pad_left   = 0;
};

// ── Flex 子项属性 ──
struct FlexItem {
    int32_t grow = 0;  // 0 = 固定，>0 = 按比例分剩余空间
};

// ── 电池状态（BatteryIcon 专用） ──
struct Battery {
    int32_t level    = 50;
    bool    charging = false;
};

// ── 进度值（ProgressBar 专用） ──
struct Progress {
    int32_t value = 0;   // 0-100
};

// ── 定时器间隔（轮询任务用） ──
struct Interval {
    uint32_t ms       = 0;   // 间隔
    uint32_t last_ms  = 0;   // 上次执行时间
    void (*fn)(void* ctx) = nullptr;
    void* ctx = nullptr;
};

} // namespace papers3::ecs::components