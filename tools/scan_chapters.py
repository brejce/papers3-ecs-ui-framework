#!/usr/bin/env python3
"""scan_chapters.py — 阅读器分卷/分章引擎（**规则数据驱动**），PC 侧参考实现 / 回归基准

规则从 `tools/reader_rules.txt` 读（格式见该文件头），与将来 .so 里的 C++ 实现**同一套语义**：
  V/C/S 标记字 → P 前缀 → D 数字形态 → R 拒绝对 → L 长度 → G 候选数闸门

分层：宿主要做的只有一件事 —— 把那个文件读成一段字节交给 .so；**规则怎么解释是 .so 的事**
（业务逻辑在 .so / 能力在固件，见 docs/architecture/READER_API_SURFACE.md）。

用法：
  python3 tools/scan_chapters.py books/*.txt                    # 汇总（默认规则）
  python3 tools/scan_chapters.py --rules R.txt books/*.txt      # 用指定规则文件
  python3 tools/scan_chapters.py --list 40 BOOK.txt             # 列前 40 章
  python3 tools/scan_chapters.py --full BOOK.txt                # 卷/章/节/特殊行都列
"""
import sys, os
import re

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_RULES = os.path.join(HERE, "reader_rules.txt")
DIGITS_ASCII = "0123456789"
SEP_AFTER_KW = " \t\u3000:：（([、.·-0123456789"

# ── 内置默认规则（.so 里编进去的那份，内容与 reader_rules.txt 必须一致）──
BUILTIN = {
    "v_mark": "卷部篇集",
    "c_mark": "章回",
    "s_mark": "节",
    "prefix": ["第"],
    "num": {"A", "H"},
    "hanzi": "零〇一二三四五六七八九十百千两",
    "keywords": ["前传", "序章", "序言", "楔子", "引子", "尾声", "终章", "后记", "番外",
                 "外传", "完本感言", "上架感言"],
    "reject": [("回", "合"), ("集", "团")],
    "max_title": 30,
    "min_hits": 2,
    "head_lines": 15,
    "drop_words": ["声明", "----", "作者上传", "txt8080", "www.", "请记住", "首发"],
    "author_key": "作者",
    "book_wrap": "《》",
}


def _copy(r):
    return {"v_mark": r["v_mark"], "c_mark": r["c_mark"], "s_mark": r["s_mark"],
            "prefix": list(r["prefix"]), "num": set(r["num"]), "hanzi": r["hanzi"],
            "keywords": list(r["keywords"]), "reject": list(r["reject"]),
            "max_title": r["max_title"], "min_hits": r["min_hits"],
            "head_lines": r["head_lines"], "drop_words": list(r["drop_words"]),
            "author_key": r["author_key"], "book_wrap": r["book_wrap"]}


def _find_version(lines, scan=50):
    """在头几行里找版本行（跳过空行/注释/BOM）。

    ★ 别只检查 lines[0]：用户和工具都会在文件开头写注释（实测踩过 ——
      注释在前 → 误判"文件头不对" → 静默退回内置，改了规则却"没生效"，最难查的一类现象）。
    """
    for ln in lines[:scan]:
        s = ln.lstrip("\ufeff").strip()
        if not s or s.startswith("#"):
            continue
        if s.startswith("!reader-rules"):
            rest = s[len("!reader-rules"):].strip()
            try:
                return int(rest.split()[0]) if rest else -1
            except ValueError:
                return -1
        break                      # 第一条有效行不是版本行 → 就当没有
    return None


def load_rules(path=None):
    """读规则文件；不在/缺版本行 → 内置默认（与设备端同策略：永远有底），并**说明原因**"""
    R = _copy(BUILTIN)
    if not path or not os.path.exists(path):
        R["source"] = "builtin(没有规则文件)"
        return R
    lines = open(path, encoding="utf-8").read().splitlines()
    ver = _find_version(lines)
    if ver is None:
        R["source"] = "builtin(缺 !reader-rules 版本行)"
        return R
    if ver != 1:
        R["source"] = f"builtin(版本 {ver} 不认识，本程序只认 1)"
        return R
    R["prefix"] = []
    R["reject"] = []
    R["num"] = set()
    for ln in lines:
        ln = ln.split("#", 1)[0].strip()
        if not ln or ln.startswith("[") or ln.startswith("!reader-rules"):
            continue                              # 注释 / 版本行 / [book:…] 段
        parts = ln.split()
        k, v = parts[0], parts[1:]
        if k == "V":            R["v_mark"] = "".join(v)
        elif k == "C":          R["c_mark"] = "".join(v)
        elif k == "S":          R["s_mark"] = "".join(v)
        elif k == "P":          R["prefix"].append("".join(v))
        elif k == "D":          R["num"] |= set("".join(v))
        elif k == "H":          R["hanzi"] = "".join(v)
        elif k == "K":          R["keywords"] = v
        elif k == "R" and len(v) >= 2: R["reject"].append((v[0], v[1]))
        elif k == "L":          R["max_title"] = int(v[0])
        elif k == "G":          R["min_hits"] = int(v[0])
        elif k == "T":          R["head_lines"] = int(v[0])
        elif k == "X":          R["drop_words"] = v
        elif k == "A":          R["author_key"] = "".join(v)
        elif k == "B":          R["book_wrap"] = "".join(v)
        # 不认识的 KEY → 忽略（向前兼容）
    R["source"] = path
    return R


def classify(line: str, R: dict):
    """返回 (level, title) 或 None。level 1=卷级 2=章级 3=节级 4=独立关键字行(→卷级)"""
    s = line.strip("\r\n \t\u3000")
    if not s:
        return None
    # ① 独立关键字（前传/序章/番外…）：后面必须是行尾或分隔符，
    #    否则"结局全部一样。"这类正文句子会被误收（实测）
    for k in R["keywords"]:
        if s.startswith(k):
            rest = s[len(k):]
            if (rest == "" or rest[0] in SEP_AFTER_KW) and len(s) <= len(k) + 20:
                return (4, s)
    # ② 前缀 + 数字 + 标记字
    pre = None
    for p in R["prefix"]:
        if s.startswith(p):
            pre = p
            break
    if not pre:
        return None
    pos = len(pre)
    n = 0
    while pos < len(s):
        c = s[pos]
        if "A" in R["num"] and c in DIGITS_ASCII:
            pos += 1
        elif "H" in R["num"] and c in R["hanzi"]:
            pos += 1
        else:
            break
        n += 1
        if n > 7:                       # 编号太长 → 不是章节号
            return None
    if n == 0 or pos >= len(s):
        return None
    mk = s[pos]
    if mk in R["v_mark"]:
        lv = 1
    elif mk in R["c_mark"]:
        lv = 2
    elif mk in R["s_mark"]:
        lv = 3
    else:
        return None
    # ③ 长度门槛：正文里"第一回合，自己秘密抢先出击……"（53 字）靠它挡。
    #    ⚠️ **不能拿标点当判据**：真章名本来就带 ，？（）——
    #      实测误杀过 7 条真章节（「第1592章 向使当初身便死，一生真伪复谁知？」等）
    if len(s) > R["max_title"]:
        return None
    # ④ 拒绝对：回+合（第一回合）/ 集+团（第16集团军）
    for a, b in R["reject"]:
        if mk == a and s[pos + 1:pos + 2] == b:
            return None
    return (lv, s)


def scan(path: str, R: dict):
    """流式扫（对应设备端分块读）：返回 (index, stats)"""
    idx, off, bad = [], 0, 0
    with open(path, "rb") as f:
        for raw in f:
            try:
                line = raw.decode("utf-8")
            except UnicodeDecodeError:      # 真书里确有坏字节（实测 1 处）
                bad += 1
                line = raw.decode("utf-8", errors="replace")
            c = classify(line, R)
            if c:
                idx.append({"off": off, "level": c[0], "title": c[1]})
            off += len(raw)
    st = {"bytes": off, "bad_utf8": bad,
          "lv1": sum(1 for e in idx if e["level"] == 1),
          "lv2": sum(1 for e in idx if e["level"] == 2),
          "lv3": sum(1 for e in idx if e["level"] == 3),
          "sp":  sum(1 for e in idx if e["level"] == 4)}
    if st["lv1"] + st["lv2"] < R["min_hits"]:        # 闸门：判定不可靠 → 整本不分章
        st["gated"] = True
        return [], st
    return idx, st


def main():
    args = sys.argv[1:]
    mode, n, rules = "sum", 20, DEFAULT_RULES
    while args and args[0].startswith("--"):
        a = args.pop(0)
        if a == "--rules" and args:
            rules = args.pop(0)
        elif a in ("--list", "--full"):
            mode = a[2:]
            if args and args[0].isdigit():
                n = int(args.pop(0))
    R = load_rules(rules)
    if len(args) == 1 and mode == "sum":
        print(f"规则来源: {R['source']}")
    for p in args:
        idx, st = scan(p, R)
        print("=" * 72)
        print(f"{os.path.basename(p)}  {st['bytes']/1048576:.1f}MB  "
              f"卷{st['lv1']} 章{st['lv2']} 节{st['lv3']} 特殊{st['sp']}  "
              f"坏字节行{st['bad_utf8']}" + ("  [闸门放弃分章]" if st.get("gated") else ""))
        if mode == "sum":
            for e in idx[:6]:
                print(f"   L{e['level']} off={e['off']:>9}  {e['title'][:34]}")
        else:
            show = idx if mode == "full" else [e for e in idx if e["level"] == 2]
            for e in show[:n]:
                print(f"   L{e['level']} off={e['off']:>9}  {e['title'][:34]}")


if __name__ == "__main__":
    main()
