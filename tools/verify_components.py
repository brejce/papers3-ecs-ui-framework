#!/usr/bin/env python3
"""tools/verify_components.py — 核对 components/ 里的第三方组件是不是 components.lock 钉的那一版

三个子命令：

    # 1) 打补丁**之前**：解压出来的原始树必须和 registry 官方 CHECKSUMS.json 完全一致
    python3 tools/verify_components.py pristine <组件目录> <CHECKSUMS.json>

    # 2) 任一时点：已安装的组件 = 官方那一版 + 只打了 lock 里列的补丁
    python3 tools/verify_components.py installed [组件名]      # 不给名字 = 全部

    # 3) 只查 lock 里补丁文件的 sha256（防「补丁被改过」）
    python3 tools/verify_components.py patches

为什么要它：第三方代码不进仓库，仓库只留 components.lock + patches/。所以「clone 下来
跑一遍脚本，拿到的到底是不是我本机验证过的那一份」必须能自证 —— 否则上游 registry 那边
悄悄换了内容、或者补丁被打歪，都没人发现（历史上就踩过：补丁被组件管理器冲掉，.so 一开
就 Guru Meditation）。

退出码：0 = 一致；1 = 不一致（打印差异）；2 = 用法/环境错误。
"""
from __future__ import annotations

import hashlib
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LOCK = os.path.join(ROOT, "components.lock")
COMPONENTS = os.path.join(ROOT, "components")
MANIFESTS = os.path.join(COMPONENTS, ".manifests")


def sha256(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def load_lock() -> dict:
    with open(LOCK, encoding="utf-8") as f:
        return json.load(f)


# ── 1) 原始树 vs 官方 CHECKSUMS.json ─────────────────────────────
def cmd_pristine(dest: str, checksums: str) -> int:
    with open(checksums, encoding="utf-8") as f:
        man = json.load(f)
    files = man.get("files", [])
    bad, missing = [], []
    for e in files:
        p = os.path.join(dest, e["path"])
        if not os.path.isfile(p):
            missing.append(e["path"])
            continue
        got = sha256(p)
        if got != e["hash"]:
            bad.append((e["path"], e["hash"][:12], got[:12]))
    if missing or bad:
        print(f"✗ 原始树和官方校验表不一致：缺 {len(missing)} 个 / 哈希不同 {len(bad)} 个", file=sys.stderr)
        for m in missing[:10]:
            print("   缺:", m, file=sys.stderr)
        for p, want, got in bad[:10]:
            print(f"   变: {p} 官方 {want}… 实际 {got}…", file=sys.stderr)
        return 1
    print(f"  ✓ 原始树与官方校验表一致（{len(files)} 个文件）")
    return 0


# ── 2) 已安装树 = 官方版 + lock 里的补丁 ─────────────────────────
def patch_touches(entry: dict) -> set[str]:
    out: set[str] = set()
    for p in entry.get("patches", []):
        out |= set(p.get("touches", []))
    return out


def pruned(entry: dict) -> tuple[str, ...]:
    """安装时主动删掉的上游子目录（见 lock 里的 prune）—— 校验时它们「缺失」是正常的。"""
    return tuple(entry.get("prune", []))


def cmd_installed(only: str | None) -> int:
    lock = load_lock()
    rc = 0
    for c in lock["components"]:
        if only and c["name"] != only:
            continue
        name, ver = c["name"], c["version"]
        dest = os.path.join(COMPONENTS, name)
        man_path = os.path.join(MANIFESTS, f"{name}-{ver}.json")
        print(f"[{name} {ver}]")
        if not os.path.isdir(dest):
            print("  ✗ 没装（跑 ./tools/fetch_components.sh）", file=sys.stderr)
            rc = 1
            continue
        if not os.path.isfile(man_path):
            print(f"  ! 没有官方校验表 {os.path.relpath(man_path, ROOT)}（老安装？跑 --force 重拉可补上）")
        else:
            with open(man_path, encoding="utf-8") as f:
                man = json.load(f)
            touched = patch_touches(c)
            prune = pruned(c)
            bad = 0
            checked = 0
            for e in man.get("files", []):
                if e["path"] in touched:
                    continue                      # 这些文件本来就被补丁改过
                if any(e["path"].startswith(pf) for pf in prune):
                    continue                      # 安装时主动删掉的（test_apps 之类）
                p = os.path.join(dest, e["path"])
                checked += 1
                if not os.path.isfile(p) or sha256(p) != e["hash"]:
                    bad += 1
                    if bad <= 5:
                        print(f"  ✗ 被改过（不是补丁范围）：{e['path']}", file=sys.stderr)
            print(f"  {'✓' if bad == 0 else '✗'} 未打补丁的文件：{checked} 个"
                  f"{' 全部一致' if bad == 0 else f'，{bad} 个不一致'}")
            if bad:
                rc = 1
        # 补丁：sha256 对得上 + 树上确实有这么一处改动
        for p in c.get("patches", []):
            pth = os.path.join(ROOT, p["path"])
            if not os.path.isfile(pth):
                print(f"  ✗ 补丁文件不存在：{p['path']}", file=sys.stderr)
                rc = 1
                continue
            if sha256(pth) != p["sha256"]:
                print(f"  ✗ 补丁被改过：{p['path']}（sha256 与 components.lock 不符）", file=sys.stderr)
                rc = 1
                continue
            marker = first_added_line(pth)
            ok = True
            for rel in p.get("touches", []):
                tgt = os.path.join(dest, rel)
                if not os.path.isfile(tgt) or (marker and marker not in open(tgt, encoding="utf-8", errors="replace").read()):
                    print(f"  ✗ 补丁没打上：{rel}", file=sys.stderr)
                    ok = False
                    rc = 1
            if ok:
                print(f"  ✓ 补丁已应用：{os.path.basename(p['path'])}")
    return rc


def first_added_line(patch_path: str) -> str:
    """补丁里第一条新增行 —— 拿它当「已应用」标记（和 fetch 脚本同一判据）。"""
    with open(patch_path, encoding="utf-8", errors="replace") as f:
        for line in f:
            if line.startswith("+") and not line.startswith("+++"):
                return line[1:].rstrip()
    return ""


# ── 3) 只查补丁哈希 ─────────────────────────────────────────────
def cmd_patches() -> int:
    lock = load_lock()
    rc = 0
    for c in lock["components"]:
        for p in c.get("patches", []):
            pth = os.path.join(ROOT, p["path"])
            if not os.path.isfile(pth):
                print(f"✗ 缺补丁 {p['path']}", file=sys.stderr)
                rc = 1
            elif sha256(pth) != p["sha256"]:
                print(f"✗ 补丁 sha256 不符 {p['path']}（改了补丁就要同步 components.lock）", file=sys.stderr)
                rc = 1
            else:
                print(f"✓ {p['path']}")
    return rc


def main(argv: list[str]) -> int:
    if len(argv) < 2:
        print(__doc__)
        return 2
    cmd = argv[1]
    if cmd == "pristine":
        if len(argv) != 4:
            print("用法: verify_components.py pristine <目录> <CHECKSUMS.json>", file=sys.stderr)
            return 2
        return cmd_pristine(argv[2], argv[3])
    if cmd == "installed":
        return cmd_installed(argv[2] if len(argv) > 2 else None)
    if cmd == "patches":
        return cmd_patches()
    print(f"未知子命令 {cmd}", file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
