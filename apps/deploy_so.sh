#!/bin/bash
# apps/deploy_so.sh — 把一个 .so 可靠地部署到设备（传临时文件 → 回读校验 → 才改名）
#
# 为什么这么麻烦：设备跑久了内部 RAM 紧张时，httpd 会「上传返回 200 但只落几 KB」
# 或「/api/dl 发一半就断」。直接用同名覆盖会把正在用的 app 写成半个文件 → dlopen 失败
# （串口 ir=ELF: Error to load elf file, ret=-22）。所以：
#   1) 先传到 /apps/_<name>.tmp
#   2) 从设备回读、跟本地逐字节 cmp —— 只有完全一致才继续
#   3) POST /api/mv 改名（参数走 **body**，不是 query：h_op 用 read_body）
#   4) 再回读一次确认（不一致就复位重来）
#
# 用法：./deploy_so.sh <local.so> <remote-name> [设备IP]
#   例：cd apps/terminal && ../../apps/deploy_so.sh build/terminal.so terminal.so 192.168.43.14
set -u
SO="$1"; NAME="$2"; IP="${3:-192.168.43.14}"
[ -f "$SO" ] || { echo "找不到 $SO"; exit 1; }
WANT=$(stat -c%s "$SO")
TMP="_${NAME}.tmp"

reset_dev() {
    python3 - <<'PY' 2>/dev/null
import serial, time
s = serial.Serial('/dev/ttyACM0', 115200, timeout=1)
s.setDTR(False); s.setRTS(False); time.sleep(0.3)
s.setRTS(True);  time.sleep(0.15); s.setRTS(False)
s.close()
PY
}

wait_httpd() {
    for _ in $(seq 1 30); do
        [ "$(curl -s -m 2 -o /dev/null -w '%{http_code}' "http://$IP/api/stat")" = "200" ] && return 0
        sleep 2
    done
    return 1
}

try_once() {
    local code
    code=$(curl -s -m 90 -X POST --data-binary "@$SO" "http://$IP/api/ul?path=/apps&name=$TMP" \
                -o /tmp/_ul.txt -w '%{http_code}')
    [ "$code" = "200" ] || { echo "  上传 HTTP $code"; return 1; }
    sleep 1
    local got
    got=$(curl -s -m 60 "http://$IP/api/dl?path=/apps/$TMP" -o /tmp/_got.so -w '%{size_download}')
    if [ "$got" != "$WANT" ]; then echo "  回读 $got / 期望 $WANT"; return 1; fi
    cmp -s /tmp/_got.so "$SO" || { echo "  回读内容不一致"; return 1; }
    echo "  上传+校验 OK（$WANT 字节）"
    # FATFS 的 rename 不覆盖已有文件（返回 303 但没换）→ 先删目标再改名
    curl -s -m 20 -X POST --data "path=/apps/$NAME" "http://$IP/api/rm" -o /dev/null -w '  删旧=%{http_code} '
    code=$(curl -s -m 30 -X POST --data "path=/apps/$TMP&to=/apps/$NAME" "http://$IP/api/mv" -o /dev/null -w '%{http_code}')
    echo "改名=$code"
    return 0
}

for round in 1 2 3; do
    echo "[$round] $NAME ← $SO"
    if try_once; then
        sleep 1
        got=$(curl -s -m 60 "http://$IP/api/dl?path=/apps/$NAME" -o /tmp/_fin.so -w '%{size_download}')
        if [ "$got" = "$WANT" ] && cmp -s /tmp/_fin.so "$SO"; then
            echo "✅ $NAME 已就位，回读逐字节一致"
            exit 0
        fi
        echo "  改名后回读 $got —— 复位重来"
    fi
    reset_dev; wait_httpd || echo "  httpd 没起来"
done
echo "❌ 三次都没成功：设备 httpd 现在不可靠，先复位/重启电脑热点再试"
exit 1
