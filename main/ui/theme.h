#pragma once

// ui/theme.h — 设计令牌
//
// Memphis 风格经 EPD 化简后的取值：白底、纯黑文字、直角、硬阴影、零装饰。
// 撞色全砍 —— 墨水屏只有黑白灰，"颜色"的深浅靠**墨色档位**（见 text/ttf.h 的 ink_level）。
//
// 用法：theme::BG_WHITE 之类。pages/ 在 ui/ 之外，靠 `using namespace m5ui::ui;` 找到它。

#include <cstdint>

namespace m5ui::ui::theme {

constexpr uint32_t BG_WHITE    = 0xFFFFFF;
constexpr uint32_t BG_BLACK    = 0x000000;
constexpr uint32_t FG_BLACK    = 0x000000;
constexpr uint32_t FG_WHITE    = 0xFFFFFF;
constexpr uint32_t BORDER_GRAY = 0x888888;
constexpr uint32_t ACCENT      = 0x333333;

// ── 三档灰（Neo-Brutalist 撞色在 16 级灰里的落点，见 docs/guide/DEVELOPER.md 第 7 节）──
// 五档强调色只有粉色够深（亮度 89），青(0x99)/橙(0xAA) 只差 8 级分不开、
// 2 级 DU 下全变白。所以设备上「强调」只用反白/加粗描边/阴影偏移，
// 灰只用于**次要信息和弱分隔**。
//
// ★ 凡是用到这三个灰的部件，EPDMode 必须是 {2} TEXT/GL16 ——
//   FAST/DU 是 2 级波形，会把 0x99 直接量化成白＝文字消失。
constexpr uint32_t G55         = 0x555555;   // 主强调（唯一够深的一档）
constexpr uint32_t G99         = 0x999999;   // 次要文字（副标、说明）
constexpr uint32_t GDD         = 0xDDDDDD;   // 弱分隔、浅底

// 设计规则（实现在 widgets.h / systems/render.h）：
//   直角（radius 恒 0）、描边 2px（按钮 3px）、硬阴影无模糊（按钮 5px / 输入框 3px / 键盘键 2px）
//   按下 = 阴影收缩到 0 + 描边加粗 2px，元素和文字**原地不动**

} // namespace m5ui::ui::theme
