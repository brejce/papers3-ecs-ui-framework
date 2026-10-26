#!/usr/bin/env -S uv run --quiet --with fonttools
"""裁剪 Maple Mono 子集 —— 给 papers3 的 flash `fonts` 分区用（3MB）

    uv run tools/make_font_maple.py <MapleMono-NF-CN-Regular.ttf> [输出.ttf]

字符集 = **GB2312 一级汉字 3755 + ASCII + 终端/排版符号区 + 拼音补字**：

* 一级 3755（0xB0A1~0xD7F9）：日常用字。**不要**顺手换成 GB2312 全集 6763 ——
  二级字多 3008 个，裁完约 4.3MB，塞不进 3MB 分区。
* 符号区：制表符 ─│┌（终端界面树线）、箭头、几何图形、中文标点、全角标点。
  这些不在汉字区里，不显式列就会被裁掉，上屏是空白（见 docs/display/TTF_FONT.md）。
* EXTRA_HANZI：**一级字库之外、但拼音表里必须有字的音节**。
  例如 `ei`（诶/誒）、`n`（嗯）、`cen`（岑/涔）、`nou`（耨）—— 这些音节只对应
  二级字，一级字库裁掉后输入法里就查不到候选（`tools/make_pinyin_index.py`
  按字体 cmap 反推音节，字不在字体里 = 音节不存在）。补字后要重跑拼音索引。

⚠️ 输出必须 ≤ 0x300000（3MB，见 partitions.csv 的 fonts 行），脚本会自己断言。
⚠️ 改了字符集 → **必须重跑** `tools/make_pinyin_index.py`，否则索引和字体对不上
   （候选会被 ttf::has_codepoint 二次过滤，表现为"有拼音没字"）。
"""

import os
import sys

from fontTools import subset
from fontTools.ttLib import TTFont

SRC = sys.argv[1] if len(sys.argv) > 1 else "MapleMono-NF-CN-Regular.ttf"
DST = sys.argv[2] if len(sys.argv) > 2 else "fonts/MapleMono-CN-Regular-L1-tree.ttf"
PART_SIZE = 0x300000          # fonts 分区 3MB（partitions.csv）

# ── 符号区：和 fonts/MapleMono-CN-Regular-L1-tree.ttf 里实际有的码位一致 ──
SYMBOL_RANGES = [
    (0x0020, 0x007E),         # ASCII 可打印
    (0x00B7, 0x00B7),         # ·
    (0x2014, 0x2014),         # —
    (0x2018, 0x2019),         # ‘’ “”
    (0x201C, 0x201D),
    (0x2026, 0x2026),         # …
    (0x2030, 0x2030),         # ‰
    (0x2032, 0x2033),         # ′″
    (0x2103, 0x2103),         # ℃
    (0x2116, 0x2116),         # №
    (0x2122, 0x2122),         # ™
    (0x2190, 0x2199),         # ← ↑ → ↓ ↔ ↕ ↖ ↗ ↘ ↙
    (0x219D, 0x219E),
    (0x21A0, 0x21A0),
    (0x21A2, 0x21A7),
    (0x21A9, 0x21AA),
    (0x21AD, 0x21AD),
    (0x21B0, 0x21B3),
    (0x21BE, 0x21BE),
    (0x21C4, 0x21C4),
    (0x21C6, 0x21C6),
    (0x21C9, 0x21C9),
    (0x21D0, 0x21D4),         # ⇐ ⇑ ⇒ ⇓ ⇔
    (0x21DA, 0x21DB),
    (0x21DE, 0x21DF),
    (0x21E4, 0x21EA),         # ⇤ ⇥ ⇦ ⇧ ⇨ ⇩ ⇪
    (0x2500, 0x25A1),         # 制表符 / 方块元素 / 几何图形（终端树线靠这一片）
    (0x25B3, 0x25B3),         # △
    (0x25C6, 0x25C7),         # ◆ ◇
    (0x25CB, 0x25CB),         # ○
    (0x25CF, 0x25CF),         # ●
    (0x3001, 0x3002),         # 、。
    (0x3008, 0x3011),         # 〈〉《》「」『』【】
    (0xFF01, 0xFF01),         # ！
    (0xFF08, 0xFF09),         # （）
    (0xFF0C, 0xFF0C),         # ，
    (0xFF1A, 0xFF1B),         # ：；
    (0xFF1F, 0xFF1F),         # ？
    (0xFF5E, 0xFF5E),         # ～
    (0xFFE5, 0xFFE5),         # ￥
]

# ── 拼音补字：一级字库外、但缺了就"整个音节没有候选"的字 ──
#   来源：tools/make_pinyin_index.py 生成的音节表 vs 全 CJK 音节表对比，
#   缺的 14 个音节（ei / n / cen / nou / eng / fo / chua / den / fiao / hm / lo / m / nun / rua）
EXTRA_HANZI = (
    "诶誒"          # ei
    "嗯"            # n
    "岑嵾梣涔笒"    # cen
    "啂槈檽獳羺耨譳鎒"  # nou
    "鞥"            # eng
    "仏坲梻"        # fo
    "欻歘"          # chua
    "扥扽"          # den
    "覅"            # fiao
    "噷"            # hm
    "囖"            # lo
    "呣"            # m
    "黁"            # nun
    "挼"            # rua
)


def gb2312_level1() -> str:
    """GB2312 一级汉字 = 3755 个（0xB0A1~0xD7F9），按拼音排序的那一批常用字。"""
    out = []
    for hi in range(0xB0, 0xD8):
        for lo in range(0xA1, 0xFF):
            try:
                out.append(bytes([hi, lo]).decode("gb2312"))
            except Exception:
                continue
    return "".join(out)


def build_charset() -> str:
    cps = set()
    for a, b in SYMBOL_RANGES:
        cps |= set(range(a, b + 1))
    cps |= {ord(c) for c in gb2312_level1()}
    cps |= {ord(c) for c in EXTRA_HANZI}
    return "".join(chr(c) for c in sorted(cps))


def main() -> None:
    if not os.path.exists(SRC):
        sys.exit(f"✗ 找不到源字体 {SRC}（完整 MapleMono-NF-CN-Regular.ttf，约 19MB，不进仓库）")

    text = build_charset()
    print(f"字符集: {len(text)} 个（GB2312 一级 {len(gb2312_level1())} + 符号 + 补字 {len(EXTRA_HANZI)}）")

    font = TTFont(SRC)
    if "glyf" not in font:
        sys.exit("✗ 这是 CFF/OTF 字体，stb_truetype 不认。换 glyf 字体。")
    print("源字体: 字形数=%d cmap=%d" % (font["maxp"].numGlyphs, len(font.getBestCmap())))

    opts = subset.Options()
    opts.retain_gids = True       # gid 不变：stb 读 loca/glyf 时不会错位
    opts.name_IDs = ["*"]
    opts.name_legacy = True
    opts.notdef_outline = True
    opts.layout_features = ["*"]
    opts.glyph_names = False
    opts.hinting = True
    opts.recalc_bounds = True

    # 丢掉可变字体那一组表（gvar 单独 2.44MB；stb 不支持可变字体，纯赚）
    VAR_TABLES = ["gvar", "fvar", "avar", "STAT", "cvar", "HVAR", "VVAR", "MVAR"]
    dropped = [t for t in VAR_TABLES if t in font]
    opts.drop_tables.extend(dropped)
    if dropped:
        print("丢掉可变字体表: %s" % ", ".join(dropped))

    s = subset.Subsetter(options=opts)
    s.populate(text=text)
    s.subset(font)
    font.flavor = None
    os.makedirs(os.path.dirname(DST) or ".", exist_ok=True)
    font.save(DST)

    sz = os.path.getsize(DST)
    print(f"✓ {DST}  {sz/1024/1024:.2f} MB ({sz} 字节)  字形数={font['maxp'].numGlyphs}"
          f"  余 {PART_SIZE - sz} 字节")
    if sz > PART_SIZE:
        sys.exit(f"✗ 超过 fonts 分区 {PART_SIZE} 字节 —— 别烧，会写坏分区")


if __name__ == "__main__":
    main()
