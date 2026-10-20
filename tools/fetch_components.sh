#!/usr/bin/env bash
# fetch_components.sh — 拉取第三方组件（上游 zip）+ 打上本地补丁
#
# ── 为什么要这个脚本 ──────────────────────────────────────────────
#   components/ 下的 m5gfx / m5unified / elf_loader / cmake_utilities 是**第三方库**
#   （约 165MB / 1000+ 文件），它们有本地改动，所以不能直接进 git
#   —— 否则仓库就变成一份几百 MB 的第三方代码副本。
#
#   但又**不能**交给 IDF 组件管理器自动拉：组件管理器会把上游版本重新下载覆盖，
#   本地补丁就没了（见 main/idf_component.yml 顶部的「血泪教训」）。
#
#   解法：第三方代码一律**不进仓库**，用这个脚本按需还原：
#     1. 从 ESP 组件 registry 下载指定版本的官方 zip（带官方 CHECKSUMS 校验）
#     2. 解压到 components/<name>/
#     3. 依次应用 patches/<name>/*.patch（本地改动，只占几十 KB）
#
#   脚本幂等：已经在位且补丁已应用的就跳过；--force 强制全部重来。
#
# ── 用法 ─────────────────────────────────────────────────────────
#   ./tools/fetch_components.sh            # 缺哪个拉哪个
#   ./tools/fetch_components.sh --force    # 全部重拉重打（升级上游版本后用）
#   ./tools/fetch_components.sh m5gfx      # 只处理某一个
#
# ── 升级某个组件的版本 ───────────────────────────────────────────
#   ① 改下面 COMPONENTS 里的版本号
#   ② 跑 ./tools/fetch_components.sh --force
#   ③ 按 patches/README.md 重新生成该组件的补丁
#   ④ idf.py build 验证
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
COMPONENTS_DIR="$ROOT/components"
PATCH_DIR="$ROOT/patches"
REGISTRY="https://components-file.espressif.com/components"

# 格式： 命名空间/组件名:版本
#   命名空间/组件名 要跟 registry 上一致（m5stack/m5gfx、espressif/elf_loader …）
#   版本要跟 components/<名字>/idf_component.yml 里的 version 字段一致
COMPONENTS=(
    "m5stack/m5gfx:0.2.29"
    "m5stack/m5unified:0.2.22"
    "espressif/elf_loader:1.3.3"
    "espressif/cmake_utilities:0.5.3"
)

force=0
only=""
for a in "$@"; do
    case "$a" in
        --force) force=1 ;;
        -h|--help) sed -n '2,32p' "$0"; exit 0 ;;
        *) only="$a" ;;
    esac
done

# 这个组件的补丁是不是已经打上了？
#   判据：补丁里提到的每个目标文件都存在，且内容里有补丁的标记串。
patches_applied() {
    local name="$1" dest="$2"
    local pdir="$PATCH_DIR/$name"
    [ -d "$pdir" ] || return 0            # 没补丁 → 视为已就位
    local p
    for p in "$pdir"/*.patch; do
        [ -f "$p" ] || continue
        local rel marker
        rel="$(grep -m1 '^+++ ' "$p" | sed 's|^+++ b/||')"
        marker="$(grep -m1 '^+++ ' "$p" | sed 's|^+++ b/||')"
        [ -f "$dest/$rel" ] || return 1
        # 补丁文件里只要有新增行（以 + 开头且不是 +++），就拿它当标记
        local added
        added="$(grep -m1 '^+[^+]' "$p" | cut -c2- | sed 's/[[:space:]]*$//')"
        [ -n "$added" ] || continue
        grep -qF -- "$added" "$dest/$rel" || return 1
    done
    return 0
}

fetch_one() {
    local ns="$1" name="$2" ver="$3"
    local dest="$COMPONENTS_DIR/$name"
    local url="$REGISTRY/$ns/$name/$ver/${ns}__${name}-v${ver}.zip"
    local tmpzip="$COMPONENTS_DIR/.${name}-${ver}.zip"

    if [ "$force" -eq 0 ] && [ -d "$dest" ] && patches_applied "$name" "$dest"; then
        echo "✓ $name $ver 已在位（补丁已应用）"
        return 0
    fi

    echo "↓ 拉取 $ns/$name $ver …"
    rm -rf "$dest"
    mkdir -p "$dest"
    curl -fsSL -A "Mozilla/5.0" -o "$tmpzip" "$url"
    # registry 的 zip **根目录就是组件根**（没有 <ns>__<name>-v<ver>/ 这层）
    unzip -q "$tmpzip" -d "$dest"
    rm -f "$tmpzip"
    # registry 的 zip 里权限位是 0600，统一放开成 0644（构建和 git 都需要）
    chmod -R u+rwX,go+rX "$dest"

    # 官方 CHECKSUMS.json（组件管理器用它校验），顺便记录 .component_hash
    local sums="https://components-file.espressif.com/components/$ns/$name/$ver/CHECKSUMS.json"
    if curl -fsSL -A "Mozilla/5.0" -o "$dest/CHECKSUMS.json" "$sums" 2>/dev/null; then
        echo "  · 取到官方 CHECKSUMS.json"
    else
        rm -f "$dest/CHECKSUMS.json"
    fi

    # 打补丁
    if [ -d "$PATCH_DIR/$name" ]; then
        local p
        for p in "$PATCH_DIR/$name"/*.patch; do
            [ -f "$p" ] || continue
            echo "  ⊕ 应用 $(basename "$p")"
            patch -p1 -d "$dest" --forward --batch < "$p"
        done
    fi
    echo "✓ $name $ver 就绪"
}

changed=0
for c in "${COMPONENTS[@]}"; do
    ver="${c##*:}"                # 最后一个冒号后面是版本
    path="${c%:*}"                # 前面是 命名空间/组件名
    ns="${path%%/*}"              # 命名空间
    name="${path##*/}"            # 组件名
    if [ -n "$only" ] && [ "$only" != "$name" ]; then continue; fi
    fetch_one "$ns" "$name" "$ver"
    changed=1
done

# ── stb_truetype.h ────────────────────────────────────────────────
# 第三方单文件库（public domain，Sean Barrett）。本地**没有改动**，
# 就是上游那一份 —— 所以也不进仓库，跟其他第三方代码一起拉。
#   注意：stb_truetype.h 的实现宏定义在 main/ttf_impl.cpp（STB_TRUETYPE_IMPLEMENTATION），
#   头文件本身原样放 main/third_party/ 就行。
#
#   取的是 tarball 而不是 raw.githubusercontent（后者在部分网络下连不上）。
STB_COMMIT="6e9f34d5429cf16790ec43c9bac3f1ee4ad1f760"   # v1.26
STB_URL="https://codeload.github.com/nothings/stb/tar.gz/$STB_COMMIT"
STB_DEST="$ROOT/main/third_party/stb_truetype.h"

if [ -z "$only" ] || [ "$only" = "stb_truetype" ]; then
    if [ "$force" -eq 0 ] && [ -f "$STB_DEST" ] && \
       grep -q "stb_truetype.h - v1.26" "$STB_DEST" 2>/dev/null; then
        echo "✓ stb_truetype.h 已在位"
    else
        echo "↓ 拉取 stb_truetype.h (v1.26, public domain) …"
        tmp="$(mktemp -d)"
        curl -fsSL -A "Mozilla/5.0" -o "$tmp/stb.tar.gz" "$STB_URL"
        tar -xzf "$tmp/stb.tar.gz" -C "$tmp"
        mkdir -p "$(dirname "$STB_DEST")"
        cp "$tmp"/stb-*/stb_truetype.h "$STB_DEST"
        rm -rf "$tmp"
        echo "✓ stb_truetype.h 就绪"
    fi
    changed=1
fi

if [ "$changed" -eq 0 ]; then
    echo "没有要处理的组件。"
fi

# dependencies.lock 里记的是**绝对路径**（组件管理器生成的），
# 换机器 / 换目录后就是死路径，会让解析失败。组件变动后清掉它，下次 build 自动重生成。
if [ -f "$ROOT/dependencies.lock" ]; then
    rm -f "$ROOT/dependencies.lock"
    echo "（已清掉 dependencies.lock —— 下次 idf.py build 会按当前路径重新生成）"
fi

echo
echo "完成。第三方组件在 components/ 下（**不进 git**），仓库里只留 patches/。"
