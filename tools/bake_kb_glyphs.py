#!/usr/bin/env python3
"""设备 `bakekb <px>` 导出的 kb_glyphs.txt → main/ui/kb_glyphs.h

用法（抓回来再转）：
    python3 tools/sercli.py cmd "bakekb 34"
    python3 tools/sercli.py get /sdcard/kb_glyphs.txt /tmp/kb_glyphs.txt
    python3 tools/bake_kb_glyphs.py /tmp/kb_glyphs.txt main/ui/kb_glyphs.h

为什么必须用设备烤：键面字形是 stb_truetype 在**设备上、当前字体**下光栅化的结果
（字形包围盒、yoff、advance 都跟字体走）。PC 侧重新光栅化必然对不上像素。

换字体（例如把 fonts 分区从鸿蒙换成 Maple Mono）之后，这张表必须重烤一次，
否则键面还是旧字体的形状、宽度也对不上。
"""
import sys

src = sys.argv[1] if len(sys.argv) > 1 else '/tmp/kb_glyphs.txt'
dst = sys.argv[2] if len(sys.argv) > 2 else 'main/ui/kb_glyphs.h'

px = 34
glyphs = []          # (cp, w, h, xo, yo, adv, bytes)
cur = None
tokens = []

with open(src, encoding='utf-8') as f:
    for line in f:
        line = line.strip()
        if not line:
            continue
        if line.startswith('#'):
            if line.startswith('# px'):
                px = int(line.split()[2])
            continue
        if line.startswith('cp '):
            if cur:
                glyphs.append(cur)
            p = line.split()
            d = {p[i]: p[i + 1] for i in range(0, len(p) - 1, 2)}
            cur = {'cp': int(d['cp'], 16), 'w': int(d['w']), 'h': int(d['h']),
                   'xo': int(d['xo']), 'yo': int(d['yo']), 'adv': int(d['adv']),
                   'n': int(d['n']), 'data': []}
            tokens = []
        else:
            tokens.extend(line.split())
            if cur is not None and len(tokens) >= cur['n']:
                cur['data'] = [int(t, 16) for t in tokens[:cur['n']]]
                tokens = tokens[cur['n']:]
if cur:
    if not cur['data']:
        cur['data'] = [int(t, 16) for t in tokens[:cur['n']]]
    glyphs.append(cur)

for g in glyphs:
    assert len(g['data']) == g['w'] * g['h'], f"cp {g['cp']:x} 字节数不对"

# ── 拼 TAB + BITS ──
tab = []
blob = []
off = 0
for g in glyphs:
    ln = g['w'] * g['h']
    tab.append(f"    {{0x{g['cp']:x}, {g['w']}, {g['h']}, {g['xo']}, {g['yo']}, "
               f"{g['adv']}, {off}u, {ln}u}},")
    blob.extend(g['data'])
    off += ln

lines = []
for i in range(0, len(blob), 24):
    lines.append('    ' + ','.join(f'0x{b:02x}' for b in blob[i:i + 24]) + ',')

out = f"""// 自动生成，勿手改。来源：设备 `bakekb {px}` 导出 → tools/bake_kb_glyphs.py 转 C 数组
// 键盘键面字形位图：直接编进代码（flash .rodata），画键面时读常量贴图 ——
// 不占 PSRAM、不查字形缓存、不受字体设置影响（键面字号固定 {px}px）。
// 覆盖：ASCII 0x20-0x7E + ←(0x2190) →(0x2192)
// ★ 换字库（fonts 分区）之后必须重烤：字形包围盒/advance 都跟着字体走。
#pragma once
#include <cstdint>

namespace papers3::ui::kbgl {{

struct G {{ uint32_t cp; int16_t w, h, xo, yo, adv; uint32_t off, len; }};

inline constexpr G TAB[] = {{
{chr(10).join(tab)}
}};
inline constexpr int N = {len(glyphs)};

// 8bit 灰度（0=墨，255=白），行优先，紧贴包围盒
inline constexpr uint8_t BITS[] = {{
{chr(10).join(lines)}
}};

// 二分查找（TAB 按码点升序）
inline const G* find(uint32_t cp) {{
    int lo = 0, hi = N - 1;
    while (lo <= hi) {{
        const int mid = (lo + hi) / 2;
        if (TAB[mid].cp == cp) return &TAB[mid];
        if (TAB[mid].cp < cp) lo = mid + 1; else hi = mid - 1;
    }}
    return nullptr;
}}

}}  // namespace papers3::ui::kbgl
"""

with open(dst, 'w', encoding='utf-8') as f:
    f.write(out)

print(f"{dst}: {len(glyphs)} 个字形 · {px}px · BITS {len(blob)} 字节 · TAB {len(glyphs)} 条")
