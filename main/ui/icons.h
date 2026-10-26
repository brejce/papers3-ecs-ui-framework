#pragma once

// ──────────────────────────────────────────────
// icons.h — 字体里**没有**的符号，直接用图元画
//
// 本项目字体是鸿蒙 GB2312 裁切子集：只有汉字 + ASCII + 常用中文标点，
// **没有 ←→↑↓、⌫、⇧、★□△ 这些符号** —— 硬凑字符上屏就是方块。
// 所以这类图标一律自绘：CustomDraw 回调 + M5GFX 图元。
//
// 回调约定：进函数时底色/描边已经画好，这里只画图案本身；坐标是部件左上角。
// 注意别在回调里调 startWrite/endWrite —— 渲染已经开了一个事务。
// ──────────────────────────────────────────────

#include <M5Unified.h>
#include <cstdint>

namespace papers3::ui::icons {

// 实心三角箭头（候选翻页）
inline void triangle(int32_t x, int32_t y, int32_t w, int32_t h, bool right, uint32_t fg) {
    const int32_t cx = x + w / 2, cy = y + h / 2;
    const int32_t hh = 10;   // 三角半高
    const int32_t hw = 7;    // 三角半宽
    if (right) M5.Display.fillTriangle(cx - hw, cy - hh, cx - hw, cy + hh, cx + hw, cy, fg);
    else       M5.Display.fillTriangle(cx + hw, cy - hh, cx + hw, cy + hh, cx - hw, cy, fg);
}

inline void arrow_left(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t fg, uint32_t) {
    triangle(x, y, w, h, false, fg);
}
inline void arrow_right(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t fg, uint32_t) {
    triangle(x, y, w, h, true, fg);
}

// 退格图标（⌫ 的形状）：左边一个尖角 + 右边方块，中间镂空一个叉
inline void backspace(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t fg, uint32_t bg) {
    const int32_t cx = x + w / 2, cy = y + h / 2;
    const int32_t hw = 13, hh = 9, tip = 10;
    M5.Display.fillTriangle(cx - hw, cy, cx - hw + tip, cy - hh, cx - hw + tip, cy + hh, fg);
    M5.Display.fillRect(cx - hw + tip, cy - hh, (hw - tip) + hw, hh * 2, fg);
    const int32_t s = 4;
    M5.Display.drawLine(cx - s, cy - s, cx + s, cy + s, bg);   // 用底色画叉 = 镂空
    M5.Display.drawLine(cx + s, cy - s, cx - s, cy + s, bg);
}

} // namespace papers3::ui::icons
