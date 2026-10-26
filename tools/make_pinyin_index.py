#!/usr/bin/env -S uv run --quiet --with fonttools --with pypinyin
# 生成拼音索引：
#   main/text/pinyin_index.h —— 内置表（编进固件，SD 没卡也能用）
#   pinyin.idx               —— 可升级词库（拷到 SD 卡 /papers3/pinyin.idx）
#
#   uv run tools/make_pinyin_index.py [字体.ttf] [输出.h]
#
# 关键点：**索引的字符集直接取自字体 cmap**，不是写死 GB2312。
#   换字体（例如以后裁切得更小 / 换成别的白名单字体）→ 重跑本脚本，索引自动跟着变。
#   运行时还有第二道闸：候选生成时用 ttf::has_codepoint() 过滤，
#   所以就算索引没重跑、字体变小了，也只是候选变少，绝不会画出方块。
#
# 候选排序用 Jun Da 字频表（tools/junda_freq.txt，GBK；没有就不排序）。

import os
import sys

FONT = sys.argv[1] if len(sys.argv) > 1 else "HarmonyOS_SansSC-subset.ttf"
OUT = sys.argv[2] if len(sys.argv) > 2 else "main/text/pinyin_index.h"
IDX = sys.argv[3] if len(sys.argv) > 3 else "pinyin.idx"
FREQ = "tools/junda_freq.txt"
MAX_CAND = 32      # 每音节存这么多候选（运行时按 8 个一页翻）

from fontTools.ttLib import TTFont
from pypinyin import pinyin, Style


def font_charset(path: str) -> set:
    cm = TTFont(path, lazy=True).getBestCmap()
    return set(cm.keys())


def load_rank(path: str) -> dict:
    """Jun Da 字频表 → {汉字: rank}（rank 越小越常用）"""
    if not os.path.exists(path):
        print(f"⚠ 没有 {path}，候选将按码位排序")
        return {}
    out = {}
    with open(path, "rb") as f:
        for raw in f.read().split(b"\n"):
            line = raw.strip()
            if not line or line.startswith(b"/*"):
                continue
            col = line.split(b"\t")
            if len(col) < 3 or not col[1]:
                continue
            try:
                ch = col[1].decode("gbk")
                rank = int(col[0])
            except Exception:
                continue
            if len(ch) == 1:
                out[ch] = rank
    return out


def main() -> None:
    cs = font_charset(FONT)
    hanzi = sorted(c for c in cs if 0x4E00 <= c <= 0x9FFF)
    rank = load_rank(FREQ)
    print(f"字体 {FONT}: 汉字 {len(hanzi)} 个, 字频表 {len(rank)} 条")

    # 汉字 → 它的所有拼音读法（无声调；ü 由 pypinyin 写成 v）
    groups: dict[str, list[str]] = {}
    for cp in hanzi:
        ch = chr(cp)
        try:
            reads = {r[0] for r in pinyin(ch, style=Style.NORMAL, heteronym=True, errors="ignore") if r[0]}
        except Exception:
            continue
        for rd in reads:
            if rd.isascii() and rd.isalpha():
                groups.setdefault(rd.lower(), []).append(ch)

    # 每组按字频降序；不在字频表里的排最后（再按码位）
    BIG = 10 ** 9
    for syl, chars in groups.items():
        chars.sort(key=lambda c: (rank.get(c, BIG), ord(c)))

    order = sorted(groups.keys())
    lines = []
    lines.append("// 自动生成，勿手改 —— tools/make_pinyin_index.py")
    lines.append(f"// 源：{os.path.basename(FONT)} 的 cmap（{len(hanzi)} 汉字） + Jun Da 字频排序")
    lines.append("#pragma once")
    lines.append("#include <cstdint>")
    lines.append("")
    lines.append("namespace papers3::ime {")
    lines.append("")
    lines.append("struct PinyinEntry {")
    lines.append("    const char* pinyin;    // 无声调拼音（ü 写作 v）")
    lines.append(f"    uint16_t    cand[{MAX_CAND}];  // 候选码位，按字频降序")
    lines.append("    uint8_t     count;")
    lines.append("};")
    lines.append("")
    lines.append("// 按 pinyin 字典序排列 —— 运行时二分查找定位")
    lines.append("static const PinyinEntry PINYIN_INDEX[] = {")
    for syl in order:
        chars = groups[syl][:MAX_CAND]
        cps = ", ".join("0x%04X" % ord(c) for c in chars)
        lines.append('    {"%s", {%s}, %d},' % (syl, cps, len(chars)))
    lines.append("};")
    lines.append("")
    lines.append("static const int PINYIN_INDEX_COUNT = "
                 "sizeof(PINYIN_INDEX) / sizeof(PINYIN_INDEX[0]);")
    lines.append("")
    lines.append("} // namespace papers3::ime")
    lines.append("")

    os.makedirs(os.path.dirname(OUT) or ".", exist_ok=True)
    with open(OUT, "w") as f:
        f.write("\n".join(lines))
    print(f"✓ {OUT}  音节 {len(order)} 个  {os.path.getsize(OUT)/1024:.1f} KB")

    # ── 导出二进制索引 ──
    # 拷到 SD 卡 /papers3/pinyin.idx 就是**可升级词库**（结构见 main/text/pinyin_store.h）
    import struct
    with open(IDX, "wb") as f:
        f.write(b"PYIX")
        f.write(struct.pack("<HHI", 2, MAX_CAND, len(order)))   # v2：每条带 cnt[]
        for syl in order:
            chars = groups[syl][:MAX_CAND]
            f.write(syl.encode("ascii")[:11].ljust(12, b"\0"))
            f.write(struct.pack("<BB", len(chars), 0))
            f.write(b"".join(struct.pack("<H", ord(c)) for c in chars))
            f.write(b"\0" * (2 * (MAX_CAND - len(chars))))
            f.write(b"\0" * MAX_CAND)              # cnt[]：新词库的使用计数全 0
    print(f"✓ {IDX}  {os.path.getsize(IDX)/1024:.1f} KB  → 拷到 SD 卡的 /papers3/pinyin.idx")

    for probe in ("ni", "hao", "wo", "shi", "zhong"):
        if probe in groups:
            print("   %-5s → %s" % (probe, "".join(groups[probe][:MAX_CAND])))


if __name__ == "__main__":
    main()
