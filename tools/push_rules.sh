#!/usr/bin/env bash
# push_rules.sh — 把 reader_rules.txt 推到设备 SD（分卷/分章规则**随时更新，不用刷固件**）
#
# 用法：
#   ./tools/push_rules.sh                 # 自动扫本机所在 /24 找设备（80 端口在跑 + 像 papers3 网页）
#   ./tools/push_rules.sh 192.168.43.14   # 指定 IP
#
# 做的事：确认 /sdcard/m5ui 存在 → 上传 → 回读比对 → 打印校验结果。
# 生效时机：**重新进入阅读器 app**（page_init 读一次规则）；不用重启设备。
#
# 注：设备端读规则这件事必须由**宿主**做（.so 不能自己开文件，白名单没有 fopen），
#     见 docs/reader/READER_TXT_RULES.md §9。
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$HERE/reader_rules.txt"
DST_DIR="/papers3"
DST_NAME="reader_rules.txt"

[ -f "$SRC" ] || { echo "✗ 找不到 $SRC"; exit 1; }

# ── 找设备 ──
ip="${1:-}"
if [ -z "$ip" ]; then
  me="$(ip -4 -o addr show scope global 2>/dev/null | awk '{print $4}' | cut -d/ -f1 | head -1)"
  net="${me%.*}"
  [ -n "$net" ] || { echo "✗ 拿不到本机网段，请直接给 IP"; exit 1; }
  echo "扫描 $net.0/24 找设备…"
  for i in $(seq 1 254); do
    ( curl -s -m 1 "http://$net.$i/" 2>/dev/null | grep -qi 'papers3\|文件管理' && echo "$net.$i" > /tmp/_m5ip ) &
  done
  wait
  ip="$(cat /tmp/_m5ip 2>/dev/null | head -1)"; rm -f /tmp/_m5ip
  [ -n "$ip" ] || { echo "✗ 没扫到设备（热点没连？网页开关关了？）"; exit 1; }
fi
echo "设备: $ip"

# ── 目录（缺了就建）──
curl -s -m 3 -X POST "http://$ip/api/mkdir?path=$DST_DIR" >/dev/null 2>&1

# ── 上传 ──
n=$(wc -c < "$SRC")
printf '上传 %s (%s 字节)… ' "$DST_NAME" "$n"
curl -s -m 5 -X POST --data-binary "@$SRC" \
     "http://$ip/api/ul?path=$DST_DIR&name=$DST_NAME" >/dev/null || { echo "✗ 上传失败"; exit 1; }
echo ok

# ── 回读比对（别只看"上传成功"）──
curl -s -m 5 "http://$ip/api/dl?path=$DST_DIR/$DST_NAME" -o /tmp/_m5rules.txt || { echo "✗ 回读失败"; exit 1; }
if cmp -s "$SRC" /tmp/_m5rules.txt; then
  echo "✓ 回读一致 —— 现在**重新进入阅读器**即生效（不用重启设备）"
else
  echo "✗ 回读不一致：本地 $n 字节 / 设备 $(wc -c < /tmp/_m5rules.txt) 字节"
  exit 1
fi
rm -f /tmp/_m5rules.txt
