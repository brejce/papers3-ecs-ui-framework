#!/bin/bash
# 把 terminal.so 传到设备并复位 —— 一条命令搞定，不用手抄 IP。
#   ./push-terminal.sh            # 自动扫热点网段找设备
#   ./push-terminal.sh 10.0.0.7   # 或手给 IP
#
# 找设备的判据：热点网段里哪个 IP 的 80 端口在跑（设备自带 Web 文件服务）。
# 传完自动复位 —— launcher 只在开机扫一次 /sdcard/apps，不复位看不到新 app。
set -e
IP="$1"
DIR="$(cd "$(dirname "$0")" && pwd)"
SO="$DIR/build/terminal.so"
SRC="$DIR/main/terminal.cpp"
[ -f "$SO" ] || { echo "没有 $SO —— 先 ./build.sh"; exit 1; }
# ★ 闸门：产物比源码旧 / 源码还没编过 → 直接拒绝上传。
#   （刚踩过：build.sh 失败我只看了 tail 没看见 error，结果把**上一次的旧 .so**传上去了，
#     设备上跑的还是老逻辑，白排查一轮。）
if [ "$SRC" -nt "$SO" ]; then
  echo "✗ build/terminal.so 比 main/terminal.cpp 旧 —— 这次改动还没编进去。先 ./build.sh"
  exit 2
fi

if [ -z "$IP" ]; then
  echo "扫描热点网段找设备…"
  IP=$(python3 - <<'PY'
import socket, concurrent.futures, subprocess, re
# 只扫本机所在的那个 /24
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
    ips = [f"{net}.{i}" for i in range(2, 80)]
    with concurrent.futures.ThreadPoolExecutor(40) as ex:
        for r in ex.map(probe, ips):
            if r:
                print(r); raise SystemExit
PY
)
fi
[ -n "$IP" ] || { echo "没找到设备（USB 串口有没有插？设备开着吗？）"; exit 1; }
echo "设备: $IP"

echo "上传 $(stat -c%s "$SO") bytes…"
curl -s --max-time 30 -X POST "http://$IP/api/ul?path=/apps&name=terminal.so" --data-binary @"$SO"
echo
curl -s --max-time 30 "http://$IP/api/dl?path=/apps/terminal.so" -o /tmp/_verify.so
cmp /tmp/_verify.so "$SO" && echo "✓ 回读一致"

# 复位（开串口这个动作本身就会复位芯片一次）
if [ -e /dev/ttyACM0 ]; then
  ~/esp-env/tools/python_env/idf5.5_py3.13_env/bin/python - <<'PY'
import serial, time
s = serial.Serial('/dev/ttyACM0', 115200, timeout=1)
s.setDTR(False); s.setRTS(False); time.sleep(0.3)
s.setRTS(True);  time.sleep(0.15); s.setRTS(False)
time.sleep(1); s.close()
print("已复位 —— 等 10 秒后在 launcher 里点「终端」")
PY
else
  echo "（没接 USB，复位得你自己来：拔插一下或按复位键）"
fi
