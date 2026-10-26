// systems/epd_lut_tables.cpp — 波形表数据 + 档位表 + 换表实现
//
// 表定义逐字搬自 epd_lut_lab/main/luts.h（gen_luts.py 生成，别手改）。
// 依据：docs/display/EPD_LUT_MANUAL.md §5（波形总表）、§7、§10。
//
// LUT 编码（同 M5GFX Panel_EPD）：
//   每个 uint32_t 打包 16 个灰度级，每级 2 bit：
//     0 = 数据结束 / 1 = 转黑 / 2 = 转白 / 3 = 不动
//   表以 ~0u（hold）或 0u（结束）收尾。
//
// step = sizeof(表) / sizeof(uint32_t) —— 和 Panel_EPD::init() 里
//   `cfg_detail.lut_quality_step = sizeof(lut_quality) / sizeof(uint32_t);` 一致。
//
// 容量：出厂 4+32+32+10+7 = 85 行，面板按 85+16(slack) = 101 行分配；
//   换到自定义表后最坏 = 4(擦除) + 16(gc13) + 13(text12) + 10(fast) + 7(fastest) = 50 行 —— 装得下。
//   （fast 与 bw4 同占 fast 槽，不会同时出现，所以 bw4 的 5 行不计入最坏值。）

#include "epd_lut.h"

#include <esp_log.h>
#include <M5Unified.h>
#include <M5GFX.h>
#include <lgfx/v1/platforms/esp32/Panel_EPD.hpp>

namespace papers3::epd {

static const char* TAG = "LUT";

// 16 个 2bit 字段打包成一行（同 M5GFX / epd_lut_lab 的 LUT_MAKE）
#define LUT_MAKE(d0, d1, d2, d3, d4, d5, d6, d7, d8, d9, da, db, dc, dd, de, df) \
    (uint32_t)((d0 << 0) | (d1 << 2) | (d2 << 4) | (d3 << 6) | (d4 << 8) | (d5 << 10) | \
               (d6 << 12) | (d7 << 14) | (d8 << 16) | (d9 << 18) | (da << 20) | (db << 22) | \
               (dc << 24) | (dd << 26) | (de << 28) | (df << 30))

#define LUT_HOLD (~0u)

// ══════════════════════════════════════════════════════════
// 波形表（5 张，槽位见下面 kTiers）
// ══════════════════════════════════════════════════════════

// ── gc13：quality 槽。GC16 砍掉 16 行 hold → 15 动作 + 结束 = 16 项 ──
// 走完 20 行（+4 擦除）· 831ms（整屏）· **唯一能抹平残影**（开头 5 帧整轨 flash）
static constexpr uint32_t lut_quality13[] = {
    LUT_MAKE(1, 1, 1, 1, 1, 1, 1, 2, 1, 2, 2, 1, 1, 1, 1, 1),
    LUT_MAKE(2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2),
    LUT_MAKE(2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2),
    LUT_MAKE(1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1),
    LUT_MAKE(1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1),
    LUT_MAKE(1, 1, 1, 1, 1, 1, 1, 1, 2, 1, 1, 1, 1, 1, 1, 1),
    LUT_MAKE(1, 1, 2, 2, 1, 1, 1, 2, 1, 2, 1, 1, 1, 1, 1, 3),
    LUT_MAKE(1, 1, 1, 1, 1, 2, 1, 1, 2, 2, 1, 2, 1, 2, 2, 2),
    LUT_MAKE(1, 1, 3, 2, 2, 1, 2, 2, 2, 2, 2, 2, 1, 1, 2, 2),
    LUT_MAKE(3, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2),
    LUT_MAKE(3, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2),
    LUT_MAKE(1, 1, 1, 1, 3, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2),
    LUT_MAKE(1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 3, 3, 2, 2, 2, 2),
    LUT_MAKE(3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 3),
    LUT_MAKE(3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 2, 2, 2, 3),
    0u,
};

// ── text12：text 槽。GL16 砍掉 hold → 12 动作 + 结束 = 13 项 ──
// 走完 17 行（+4 擦除）· 539ms（500×184）· 残留最轻
static constexpr uint32_t lut_text12[] = {
    LUT_MAKE(2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1),
    LUT_MAKE(2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1),
    LUT_MAKE(2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1),
    LUT_MAKE(2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 2, 2, 2, 1),
    LUT_MAKE(2, 2, 2, 2, 1, 2, 2, 2, 2, 2, 2, 1, 2, 2, 2, 1),
    LUT_MAKE(1, 2, 2, 1, 1, 1, 1, 1, 3, 3, 1, 1, 3, 3, 1, 2),
    LUT_MAKE(1, 3, 3, 1, 1, 1, 1, 3, 3, 1, 1, 1, 1, 3, 1, 2),
    LUT_MAKE(1, 3, 3, 1, 2, 2, 1, 1, 1, 1, 2, 1, 1, 1, 1, 2),
    LUT_MAKE(3, 1, 3, 2, 2, 2, 1, 1, 1, 2, 2, 1, 1, 1, 2, 3),
    LUT_MAKE(1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2),
    LUT_MAKE(1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2),
    LUT_MAKE(1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 3, 3, 3, 3, 2),
    0u,
};

// ── fast：fast 槽，原厂 DU。8 动作 + hold + 结束 = 10 项 ──
// 走完 10 行 · 451ms（整屏）· 有残留（fast 槽没有擦除段）
static constexpr uint32_t lut_fast[] = {
    LUT_MAKE(2, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 1),
    LUT_MAKE(2, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 1),
    LUT_MAKE(1, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 2),
    LUT_MAKE(1, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 2),
    LUT_MAKE(1, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 2),
    LUT_MAKE(1, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 2),
    LUT_MAKE(1, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 2),
    LUT_MAKE(1, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 2),
    LUT_HOLD,
    0u,
};

// ── fastest：fastest 槽，原厂 DU4。5 动作 + hold + 结束 = 7 项 ──
// 走完 7 行 · 340ms（整屏）· 残留最重
static constexpr uint32_t lut_fastest[] = {
    LUT_MAKE(2, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 1),
    LUT_MAKE(1, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 2),
    LUT_MAKE(1, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 2),
    LUT_MAKE(1, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 2),
    LUT_MAKE(1, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 2),
    LUT_HOLD,
    0u,
};

// ── bw4：fast 槽。纯黑白定向 4 帧 + 结束 = 5 项 ──
//   每行形状固定：目标灰 0~7 推黑、8~15 推白，无 flash、无擦除段。
// 走完 5 行 · 184ms（500×231）· 小区域 OK；**反复摩擦同一片背景会积灰痕**（手册 §6.5 修正）
static constexpr uint32_t lut_bw4[] = {
    LUT_MAKE(1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2),
    LUT_MAKE(1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2),
    LUT_MAKE(1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2),
    LUT_MAKE(1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2),
    0u,
};

#undef LUT_MAKE
#undef LUT_HOLD

// ══════════════════════════════════════════════════════════
// 档位表（数据驱动 —— 手册 §8.6：别用「下标 → 槽位」的手写 switch）
// ══════════════════════════════════════════════════════════
// slot: 0=quality 1=text 2=fast 3=fastest
//   （quality/text 槽运行时自动前置 4 行 lut_eraser；fast/fastest 不加）
// epd_mode: 1=QUALITY 2=TEXT 3=FAST 4=FASTEST（EPDMode 组件值，见 render.h）；0=不强制
namespace {

struct Tier {
    const char*     name;      // "gc13"
    const char*     ms;        // 耗时短标签（口径见 note）
    const char*     desc;      // 设置页按钮下那行小字
    const char*     note;      // 长说明（开机日志用）
    uint8_t         slot;
    uint8_t         epd_mode;
    const uint32_t* data;      // nullptr = 不换表（用原厂）
    size_t          steps;     // 表内项数（= sizeof(表)/sizeof(uint32_t)）
};

constexpr Tier kTiers[] = {
    // 0
    {"默认",    "",       "",                    "部件按内容自选（原厂 4 档表）", 0, 0, nullptr, 0},
    // 1
    {"gc13",    "831ms",  "20行 无残留",          "20 行 · 831ms 整屏 · 唯一无残留",  0, 1, lut_quality13,
     sizeof(lut_quality13) / sizeof(uint32_t)},
    // 2
    {"text12",  "539ms",  "17行 残影最轻",        "17 行 · 539ms 区 · 残留最轻",      1, 2, lut_text12,
     sizeof(lut_text12) / sizeof(uint32_t)},
    // 3
    {"fast",    "451ms",  "10行 残影重",          "10 行 · 451ms 整屏 · 残留重",      2, 3, lut_fast,
     sizeof(lut_fast) / sizeof(uint32_t)},
    // 4
    {"fastest", "340ms",  "7行 残影最重",         "7 行 · 340ms 整屏 · 残留最重",     3, 4, lut_fastest,
     sizeof(lut_fastest) / sizeof(uint32_t)},
    // 5
    {"bw4",     "184ms",  "5行 纯黑白",           "5 行 · 184ms 区 · 纯黑白，摩擦会积痕", 2, 3, lut_bw4,
     sizeof(lut_bw4) / sizeof(uint32_t)},
};
constexpr size_t kTierCount = sizeof(kTiers) / sizeof(kTiers[0]);

// 编译期护栏：表一变短/变长，行数口径就变了，必须显式确认（不是静默继续）
static_assert(sizeof(lut_quality13) / sizeof(uint32_t) == 16, "gc13 应为 16 项");
static_assert(sizeof(lut_text12)    / sizeof(uint32_t) == 13, "text12 应为 13 项");
static_assert(sizeof(lut_fast)      / sizeof(uint32_t) == 10, "fast 应为 10 项");
static_assert(sizeof(lut_fastest)   / sizeof(uint32_t) == 7,  "fastest 应为 7 项");
static_assert(sizeof(lut_bw4)       / sizeof(uint32_t) == 5,  "bw4 应为 5 项");
static_assert(kTierCount == TIER_COUNT + 1, "档位数对不上 TIER_COUNT");

const Tier& tier(uint8_t t) {
    return kTiers[(t <= TIER_COUNT) ? t : 0];   // 越界 → 退回默认，不崩
}

} // namespace

// ── 查询 ──
const char* tier_name(uint8_t t)     { return tier(t).name; }
const char* tier_ms(uint8_t t)       { return tier(t).ms; }
const char* tier_desc(uint8_t t)     { return tier(t).desc; }
const char* tier_note(uint8_t t)     { return tier(t).note; }
uint8_t     tier_epd_mode(uint8_t t) { return tier(t).epd_mode; }

// ── 换表 ──
// 只重烘 LUT（reload_lut_table），按 slot 改 config_detail 对应字段。
// slot 只有 4 个且是 Panel_EPD 的稳定接口 —— 这里按 slot 分支是刻意的，
// 不是"下标→档位"那种会随表重排而错位的手写映射。
bool apply_lut(uint8_t t) {
    if (t == TIER_DEFAULT || t > TIER_COUNT) return true;   // 默认：不换表

    const Tier& e = tier(t);
    if (e.data == nullptr) return true;

    auto* p = static_cast<lgfx::Panel_EPD*>(M5.Display.panel());
    if (p == nullptr) {
        ESP_LOGE(TAG, "panel 不是 Panel_EPD，没法换表（档=%s）", e.name);
        return false;
    }

    auto d = p->config_detail();
    switch (e.slot) {
        case 0: d.lut_quality = e.data; d.lut_quality_step = e.steps; break;
        case 1: d.lut_text    = e.data; d.lut_text_step    = e.steps; break;
        case 2: d.lut_fast    = e.data; d.lut_fast_step    = e.steps; break;
        case 3: d.lut_fastest = e.data; d.lut_fastest_step = e.steps; break;
        default:
            ESP_LOGE(TAG, "档 %s 的 slot=%u 非法", e.name, e.slot);
            return false;
    }
    p->config_detail(d);

    if (!p->reload_lut_table()) {
        // ★ 换表被拒 = 波形没变。若调用方紧接着清屏，屏上就是"白屏点不动"的假死相。
        ESP_LOGE(TAG, "换表失败：档=%s slot=%u 表内=%u 项 容量=%u 行 —— 保持原波形",
                 e.name, e.slot, (unsigned)e.steps, (unsigned)p->lut_alloc_steps());
        return false;
    }
    ESP_LOGI(TAG, "波形已换：档=%s slot=%u 表内=%u 项 容量=%u 行（%s）",
             e.name, e.slot, (unsigned)e.steps, (unsigned)p->lut_alloc_steps(), e.note);
    return true;
}

} // namespace papers3::epd
