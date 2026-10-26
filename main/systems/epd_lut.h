#pragma once

// systems/epd_lut.h — 波形档（LUT）全局配置
//
// 依据：docs/display/EPD_LUT_MANUAL.md §5（波形总表）、§7（实验台用法）、§10（动画分档）。
// 本文件只**声明**；表数据、档位表、换表实现都在 epd_lut_tables.cpp。
//
// ── 5 个档（保留：gc13 / text12 / fast / fastest / bw4）──────
//   档名      表              槽        表内行  +擦除  走完行  实测            残留
//   gc13      lut_quality13   quality   16     +4     20     831ms（整屏）   无 ★ 唯一能抹平残影
//   text12    lut_text12      text      13     +4     17     539ms（500×184）最轻
//   fast      lut_fast        fast      10      —     10     451ms（整屏）   重
//   fastest   lut_fastest     fastest    7      —      7     340ms（整屏）   最重
//   bw4       lut_bw4         fast       5      —      5     184ms（500×231）小区域 OK；反复摩擦积痕
//
//   ★ quality/text 槽运行时自动前置面板自带的 4 行擦除块；fast/fastest 不加。
//   ★ fast 与 bw4 **同占 fast 槽** —— 同一时刻只装一个，取决于选中哪档。
//   ★ 槽位来自 epd_lut_lab/tools/gen_luts.py 的 entries，**不要手写下标→槽位 switch**（手册 §8.6）。
//
// ── 用法指向（2026-10-15 拍板：不造 bw12 那张表）──────────
//   整屏 / 换档清场 → gc13（唯一无残留）
//   移动、位移类     → fastest（手册 §10.1 原推荐 bw12，决定不造）
//   原地动作         → bw4（呼吸 / 眨眼 / 只动自己那一小块）
//   ⚠️ bw4 只有 4 帧：**同一片背景被反复摩擦会积灰痕**（手册 §6.5 修正）。
//      它只适合"每帧只动自己足迹"的原地动作，别拿它跑位移。
//
// ── 换表机制（vendored Panel_EPD 的 lab hook）────────────
//   改 config_detail() 里对应槽的 lut_xxx / lut_xxx_step 指针 → reload_lut_table()。
//   **不能**重跑 init()（会重分配帧缓冲、泄漏、多开一个 task_update）。
//   只能在推屏空闲时（waitDisplay 之后）做 —— 手册 §8.4。失败必须报错，别静默。
//   容量：出厂 4+32+32+10+7 = 85 行，面板按 85+16 = 101 行分配；
//   换到自定义表后最坏 = 4(擦除) + 16(gc13) + 13(text12) + 10(fast) + 7(fastest) = 50 行，装得下。
//
// ── 「默认配置」──────────────────────────────────────
//   wave_def = 1：档位由部件按内容自选（render.h 的映射），用原厂表 —— 现状行为。
//   wave_def = 0：整机强制走 wave_sel 那一档，并装上对应的自定义表。

#include <cstdint>
#include <cstddef>

namespace papers3::epd {

// 档位编号（0 = 默认，不在可选列表里）
inline constexpr uint8_t TIER_DEFAULT = 0;
inline constexpr uint8_t TIER_GC13    = 1;
inline constexpr uint8_t TIER_TEXT12  = 2;
inline constexpr uint8_t TIER_FAST    = 3;
inline constexpr uint8_t TIER_FASTEST = 4;
inline constexpr uint8_t TIER_BW4     = 5;
inline constexpr uint8_t TIER_COUNT   = 5;   // 可选档数（不含默认）

// ── 查询（实现见 epd_lut_tables.cpp）──
const char* tier_name(uint8_t t);       // "默认"/"gc13"/"text12"/"fast"/"fastest"/"bw4"
const char* tier_ms(uint8_t t);         // 该档耗时的短标签："831ms" / ""（默认档）
const char* tier_desc(uint8_t t);       // 该档一行的短描述（设置页按钮下的小字）
const char* tier_note(uint8_t t);       // 长说明（开机日志 / 文档）
uint8_t     tier_epd_mode(uint8_t t);   // → EPDMode 组件值 1=QUALITY 2=TEXT 3=FAST 4=FASTEST；0 = 不强制

// ── 换表 ──
// 把选中的档装到面板。默认档 = 不换表（返回 true）。
// 返回 false = 换表被拒（容量不够 / panel 类型不对）—— 调用方必须记日志、保持原波形。
// ★ 只能在推屏空闲时调。
bool apply_lut(uint8_t t);

} // namespace papers3::epd
