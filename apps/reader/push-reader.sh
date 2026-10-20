#!/bin/bash
# 把 reader.so 传到设备并复位 —— 一条命令搞定，不用手抄 IP。
#   ./push-reader.sh              # 自动扫所在网段找设备
#   ./push-reader.sh 192.168.3.57 # 或手给 IP
#
# 传完自动复位（launcher 只在开机扫一次 /sdcard/apps，不复位看不到新代码）。
set -e
IP="$1"
DIR="$(cd "$(dirname "$0")" && pwd)"
SO="$DIR/build/reader.so"
FILES="main/reader.cpp main/layout.h main/pages.h main/ui.h main/cfg.h main/md_parse.h main/txt_parse.h"

[ -f "$SO" ] || { echo "没有 $SO —— 先 ./build.sh"; exit 1; }
# ★ 闸门：任一源文件比产物新 → 拒绝上传（防把上一次的旧 .so 传上去白排查一轮）
for f in $FILES; do
  if [ "$DIR/$f" -nt "$SO" ]; then
    echo "✗ build/reader.so 比 $f 旧 —— 这次改动还没编进去。先 ./build.sh"
    exit 2
  fi
done

if [ -z "$IP" ]; then
  echo "扫描所在网段找设备…"
  IP=$(python3 - <<'PY'
import socket, concurrent.futures, subprocess, re
out = subprocess.run(['ip','-4','addr'], capture_output=True, text=True).stdout
nets = re.findall(r'inet (\d+\.\d+\.\d+)\.\d+/\d+', out)
nets = [n for n in nets if not n.startswith('127.') and not n.startswith('192.168.122')]
def probe(ip):
    s = socket.socket(); s.settimeout(0.4)
    try:
        s.connect((ip, 80)); return ip
    except Exception:
        return None
    finally:
        s.close()
for net in nets:
    ips = [f"{net}.{i}" for i in range(2, 254)]
    with concurrent.futures.ThreadPoolExecutor(64) as ex:
        for r in ex.map(probe, ips):
            if r:
                print(r); raise SystemExit
PY
)
fi
[ -n "$IP" ] || { echo "没找到设备（设备开着吗？网页文件服务开着吗？）"; exit 1; }
echo "设备: $IP"

echo "上传 $(stat -c%s "$SO") bytes…"
curl -s --max-time 60 -X POST "http://$IP/api/ul?path=/apps&name=reader.so" --data-binary @"$SO" -w " (HTTP %{http_code}, %{time_total}s)\n"
# 回读校验：设备端的 /api/dl 偶尔会连接卡住（SD 忙/一次没读完），失败**不算致命**，
# 但要明确说出来，别让人以为传成功了。
if curl -s --max-time 40 "http://$IP/api/dl?path=/apps/reader.so" -o /tmp/_verify_reader.so \
   && cmp -s /tmp/_verify_reader.so "$SO"; then
  echo "✓ 回读一致"
else
  echo "⚠ 回读校验没通过（/api/dl 超时或内容不一致）—— 复位后再看一次："
  echo "  curl -s \"http://$IP/?path=/apps\" | grep -o 'reader.so[^<]*'"
fi

if [ -e /dev/ttyACM0 ]; then
  ~/esp-env/tools/python_env/idf5.5_py3.13_env/bin/python - <<'PY'
import serial, time
s = serial.Serial('/dev/ttyACM0', 115200, timeout=1)
s.setDTR(False); s.setRTS(False); time.sleep(0.3)
s.setRTS(True);  time.sleep(0.15); s.setRTS(False)
time.sleep(1); s.close()
print("已复位 —— 等 10 秒后在 launcher 里点「阅读」")
PY
else
  echo "（没接 USB，复位得你自己来：拔插一下或按复位键）"
fi
