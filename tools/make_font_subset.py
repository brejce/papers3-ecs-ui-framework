#!/usr/bin/env -S uv run --quiet --with fonttools
# 裁剪 TTF 字体子集 —— 给 papers3 的 flash fonts 分区用
#
#   uv run tools/make_font_subset.py
#
# 保留：
#   - ASCII 0x20-0x7E 全套（大小写 26 字母 + 数字 + 全部英文标点）
#   - GB2312 全部 6763 个汉字
#   - 常用中文标点
#
# 关键选项（改了会出事）：
#   retain_gids=True  字形索引保持不变，否则 stb_truetype 读到的 gid 会错位
#   hinting=True      保留 hinting 信息
#   flavor=None       输出 TrueType 而不是 CFF-flavored（stb 只认 glyf）

import sys
from fontTools.ttLib import TTFont
from fontTools import subset

SRC = sys.argv[1] if len(sys.argv) > 1 else "HarmonyOS_SansSC-Regular.ttf"
DST = sys.argv[2] if len(sys.argv) > 2 else "HarmonyOS_SansSC-subset.ttf"


def gb2312_hanzi() -> set[str]:
    """GB2312 一级+二级汉字全集 = 6763 个，覆盖日常所有用字。"""
    out = set()
    for hi in range(0xB0, 0xF8):
        for lo in range(0xA1, 0xFF):
            try:
                out.add(bytes([hi, lo]).decode("gb2312"))
            except Exception:
                continue
    return out


def build_charset() -> str:
    """ASCII + GB2312 汉字 + **整片常用符号区**。

    只留 ASCII + 汉字是不够的：`× ÷ ° ±`、`← → ↑ ↓`、`• ※ ℃ ①`、破折号族
    （`– −` 与 ASCII `-` 不是同一个码位）全在 Latin-1 / General Punctuation /
    Arrows / Geometric Shapes 里，不显式加就会被裁掉，上屏是空白。
    源字体没有的码位 fontTools 会自动跳过，所以按区块整片加最省事。
    """
    cps = set()
    cps |= set(range(0x20, 0x7F))        # ASCII 可打印
    cps |= set(range(0xA0, 0x100))       # Latin-1 补充：× ÷ ° ± § ¶ µ « » ® ©
    cps |= set(range(0x2000, 0x2070))    # 常用标点：– — … · • † ‡ ‹ › ※
    cps |= set(range(0x2070, 0x20D0))    # 上标下标
    cps |= set(range(0x2100, 0x2150))    # 字母式符号：℃ № ™ Ω
    cps |= set(range(0x2190, 0x2200))    # 箭头 ← → ↑ ↓ ⇧ ⇒
    cps |= set(range(0x2200, 0x2300))    # 数学运算符 − ≈ ≠ ≤ ≥ ∞ √ ∫
    cps |= set(range(0x2300, 0x2400))    # 杂项技术：⌫ ⏎
    cps |= set(range(0x2460, 0x2500))    # 带圈数字 ①②③
    cps |= set(range(0x2500, 0x2580))    # 制表符 ─ │ ┌ ┐
    cps |= set(range(0x2580, 0x25A0))    # 方块元素
    cps |= set(range(0x25A0, 0x2600))    # 几何图形 ★ ☆ □ △ ○ ● ■ ◆
    cps |= set(range(0x2600, 0x2700))    # 杂项符号
    cps |= set(range(0x2700, 0x27C0))    # 装饰符号
    cps |= set(range(0x3000, 0x3040))    # CJK 标点：。，、「」『』〈〉
    cps |= set(range(0xFF00, 0xFFF0))    # 全角形式：－＋＝／＼｜（）！？
    cps |= {ord(c) for c in gb2312_hanzi()}
    # 源字体没有的码位会被 subset 跳过，不用自己过滤
    return "".join(chr(c) for c in sorted(cps))


def main() -> None:
    text = build_charset()
    print(f"字符集: {len(text)} 个")

    font = TTFont(SRC)
    print("源字体: glyf=%s CFF=%s 字形数=%d"
          % ("glyf" in font, ("CFF " in font or "CFF2" in font), font["maxp"].numGlyphs))
    if "glyf" not in font:
        sys.exit("✗ 这是 CFF/OTF 字体，stb_truetype 不认。换一个 glyf 字体。")

    opts = subset.Options()
    opts.retain_gids = True
    opts.name_IDs = ["*"]
    opts.name_legacy = True
    opts.notdef_outline = True
    opts.layout_features = ["*"]
    opts.glyph_names = False
    opts.hinting = True
    opts.recalc_bounds = True

    # ★ 丢掉「可变字体」那一组表。源字体 HarmonyOS_Sans_SC 是**可变字体**
    #   （有 fvar/gvar/STAT），gvar 单独就有 2.44MB —— 留着裁完 4.43MB，
    #   直接塞不进 3MB 的 fonts 分区。
    #   而 stb_truetype **不支持可变字体**，它只读 glyf 里的默认轮廓，
    #   gvar 一个字节都不会读。所以丢掉是纯赚，画质零损失。
    VAR_TABLES = ["gvar", "fvar", "avar", "STAT", "cvar",
                  "HVAR", "VVAR", "MVAR"]
    dropped = [t for t in VAR_TABLES if t in font]
    opts.drop_tables.extend(dropped)
    if dropped:
        print("丢掉可变字体表: %s" % ", ".join(dropped))

    s = subset.Subsetter(options=opts)
    s.populate(text=text)
    s.subset(font)
    font.flavor = None
    font.save(DST)

    import os
    print(f"✓ {DST}  {os.path.getsize(DST)/1024/1024:.2f} MB  "
          f"字形数={font['maxp'].numGlyphs}")


if __name__ == "__main__":
    main()
