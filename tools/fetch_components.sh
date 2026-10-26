#!/usr/bin/env bash
# fetch_components.sh — 按 components.lock 还原第三方组件（上游原始包 + 本地补丁）
#
# ── 一句话 ────────────────────────────────────────────────────────
#   仓库里**不放**第三方代码（约 165MB / 1000+ 文件），只放两个东西：
#     · components.lock  —— 钉死版本号 + 补丁的 sha256（唯一权威）
#     · patches/*.patch  —— 本地改动本身（几十 KB）
#   这个脚本按 lock 去上游把对应版本拉下来、核对官方校验表、再打上本地补丁。
#
#   所以你 clone 别人的仓库后跑一遍这个脚本，拿到的是**和他本机一模一样**的第三方代码
#   —— 版本不飘、补丁不缺、内容对得上（官方 CHECKSUMS.json 逐文件 sha256 校验）。
#
# ── 用法 ─────────────────────────────────────────────────────────
#   ./tools/fetch_components.sh            # 缺哪个拉哪个（已有且校验通过就跳过）
#   ./tools/fetch_components.sh --force    # 全部重拉重打（升版本后用）
#   ./tools/fetch_components.sh m5gfx      # 只处理某一个
#   ./tools/fetch_components.sh --verify   # 只校验现有 components/（不下载、不改动）
#
# ── 升级某个组件 ─────────────────────────────────────────────────
#   ① 改 components.lock 里的 version（补丁的 sha256 也在这里）
#   ② ./tools/fetch_components.sh --force <name>
#   ③ 按 patches/README.md 重生成该组件的补丁，把新 sha256 写回 components.lock
#   ④ ./tools/fetch_components.sh --verify  +  idf.py build
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
COMPONENTS_DIR="$ROOT/components"
PATCH_DIR="$ROOT/patches"
LOCK="$ROOT/components.lock"
VERIFY="$HERE/verify_components.py"
MANIFESTS="$COMPONENTS_DIR/.manifests"

[ -f "$LOCK" ] || { echo "✗ 缺 $LOCK —— 它是版本权威，别删"; exit 1; }

force=0
verify_only=0
only=""
for a in "$@"; do
    case "$a" in
        --force)  force=1 ;;
        --verify) verify_only=1 ;;
        -h|--help) sed -n '2,26p' "$0"; exit 0 ;;
        *) only="$a" ;;
    esac
done

# 从 components.lock 读清单（TSV：name \t namespace \t version）
lock_tsv() {
    python3 - "$LOCK" <<'PY'
import json, sys
lock = json.load(open(sys.argv[1], encoding="utf-8"))
print(lock["registry"])
for c in lock["components"]:
    print(f"{c['name']}\t{c['namespace']}\t{c['version']}")
PY
}
# 某个组件的补丁路径（按 lock 里的顺序）
lock_patches() {
    python3 - "$LOCK" "$1" <<'PY'
import json, sys
lock = json.load(open(sys.argv[1], encoding="utf-8"))
for c in lock["components"]:
    if c["name"] == sys.argv[2]:
        for p in c.get("patches", []):
            print(p["path"])
PY
}
# 某个组件安装时要删掉的上游子目录（lock 里的 prune）
lock_prune() {
    python3 - "$LOCK" "$1" <<'PY'
import json, sys
lock = json.load(open(sys.argv[1], encoding="utf-8"))
for c in lock["components"]:
    if c["name"] == sys.argv[2]:
        for p in c.get("prune", []):
            print(p)
PY
}
# git 来源（一次一条：name \t url \t dest \t member \t marker）
lock_git() {
    python3 - "$LOCK" <<'PY'
import json, sys
lock = json.load(open(sys.argv[1], encoding="utf-8"))
for g in lock.get("git_sources", []):
    print("\t".join([g["name"], g["url"], g["dest"], g["member"], g["marker"]]))
PY
}

if [ "$verify_only" -eq 1 ]; then
    echo "── 只校验现有 components/ ──"
    python3 "$VERIFY" patches
    python3 "$VERIFY" installed ${only:+"$only"}
    exit $?
fi

# 补丁文件本身先对账（改了补丁却忘了同步 lock → 现在就炸，别等打歪）
python3 "$VERIFY" patches >/dev/null

REGISTRY="$(lock_tsv | head -1)"
mkdir -p "$MANIFESTS"

fetch_one() {
    local name="$1" ns="$2" ver="$3"
    local dest="$COMPONENTS_DIR/$name"
    local url="$REGISTRY/$ns/$name/$ver/${ns}__${name}-v${ver}.zip"
    local sums="$REGISTRY/$ns/$name/$ver/CHECKSUMS.json"
    local manifest="$MANIFESTS/$name-$ver.json"

    if [ "$force" -eq 0 ] && [ -d "$dest" ] && python3 "$VERIFY" installed "$name" >/dev/null 2>&1; then
        echo "✓ $name $ver 已在位（版本/补丁校验通过）"
        return 0
    fi

    echo "↓ 拉取 $ns/$name $ver …"
    local tmp
    tmp="$(mktemp -d)"

    curl -fsSL -A "Mozilla/5.0" -o "$tmp/pkg.zip" "$url"
    # registry 的 zip **根目录就是组件根**（没有 <ns>__<name>-v<ver>/ 这层）
    unzip -q "$tmp/pkg.zip" -d "$tmp/tree"

    # ── 官方校验表：先存一份到 components/.manifests/，再核原始树 ──
    if curl -fsSL -A "Mozilla/5.0" -o "$manifest" "$sums" 2>/dev/null; then
        echo "  · 取到官方 CHECKSUMS.json"
        if ! python3 "$VERIFY" pristine "$tmp/tree" "$manifest"; then
            echo "✗ $name $ver 的下载内容和官方校验表不符 —— 拒绝安装。" >&2
            echo "  （上游内容被替换？网络劫持？还是 registry 那边重发了同一个版本号）" >&2
            exit 1
        fi
    else
        rm -f "$manifest"
        echo "  ⚠ 拿不到官方 CHECKSUMS.json —— 这次没法逐文件校验（靠 HTTPS + 版本号兜底）"
    fi

    rm -rf "$dest"
    mkdir -p "$(dirname "$dest")"
    mv "$tmp/tree" "$dest"
    chmod -R u+rwX,go+rX "$dest"
    rm -rf "$tmp"

    # 按 lock 的 prune 删掉不参与构建的上游子目录（例如 cmake_utilities/test_apps）
    local pr
    while IFS= read -r pr; do
        [ -n "$pr" ] || continue
        rm -rf "${dest:?}/$pr"
        echo "  · 删掉 $pr（lock 里的 prune）"
    done < <(lock_prune "$name")

    # ── 打补丁：打不上就立刻停，绝不"跳过继续" ──
    local p
    while IFS= read -r p; do
        [ -n "$p" ] || continue
        local pf="$ROOT/$p"
        if patch -p1 --dry-run --batch --forward -d "$dest" < "$pf" >/dev/null 2>&1; then
            echo "  ⊕ 应用 $(basename "$pf")"
            patch -p1 --batch --forward --no-backup-if-mismatch -d "$dest" < "$pf"
        elif patch -p1 --dry-run --batch --reverse -d "$dest" < "$pf" >/dev/null 2>&1; then
            echo "  · 已应用过，跳过 $(basename "$pf")"
        else
            echo "✗ 补丁打不上：$p" >&2
            echo "  上游源码和补丁对不上（版本换了？）—— 先改 components.lock 的 version，" >&2
            echo "  再按 patches/README.md 重新生成补丁，别硬来。" >&2
            exit 1
        fi
    done < <(lock_patches "$name")

    echo "✓ $name $ver 就绪"
}

changed=0
while IFS=$'\t' read -r name ns ver; do
    [ -n "${name:-}" ] || continue
    if [ -n "$only" ] && [ "$only" != "$name" ]; then continue; fi
    fetch_one "$name" "$ns" "$ver"
    changed=1
done < <(lock_tsv | tail -n +2)

# ── git 来源（stb_truetype 这类单文件第三方库）────────────────────
while IFS=$'\t' read -r gname gurl gdest gmember gmarker; do
    [ -n "${gname:-}" ] || continue
    if [ -n "$only" ] && [ "$only" != "$gname" ]; then continue; fi
    dest="$ROOT/$gdest"
    if [ "$force" -eq 0 ] && [ -f "$dest" ] && grep -q "$gmarker" "$dest" 2>/dev/null; then
        echo "✓ $gname 已在位（$gmarker）"
        changed=1
        continue
    fi
    echo "↓ 拉取 $gname（$gmarker）…"
    tmp="$(mktemp -d)"
    curl -fsSL -A "Mozilla/5.0" -o "$tmp/src.tar.gz" "$gurl"
    tar -xzf "$tmp/src.tar.gz" -C "$tmp"
    mkdir -p "$(dirname "$dest")"
    cp "$tmp"/*/"$gmember" "$dest"
    rm -rf "$tmp"
    echo "✓ $gname 就绪 → $gdest"
    changed=1
done < <(lock_git)

[ "$changed" -eq 1 ] || echo "没有要处理的组件。"

# dependencies.lock 里记的是**绝对路径**（组件管理器生成的），
# 换机器 / 换目录后就是死路径，会让解析失败。组件变动后清掉它，下次 build 自动重生成。
if [ -f "$ROOT/dependencies.lock" ]; then
    rm -f "$ROOT/dependencies.lock"
    echo "（已清掉 dependencies.lock —— 下次 idf.py build 会按当前路径重新生成）"
fi

echo
echo "完成。第三方组件在 components/ 下（**不进 git**）；版本清单见 components.lock。"
echo "随时复验：./tools/fetch_components.sh --verify"
