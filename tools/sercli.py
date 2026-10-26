#!/usr/bin/env python3
"""tools/sercli.py — 通过 USB 串口给设备传/取文件、跑命令（不需要 WiFi）

用法：
  python3 tools/sercli.py [--port /dev/ttyACM0] ls /apps
  python3 tools/sercli.py put apps/terminal/build/terminal.so /apps/terminal.so
  python3 tools/sercli.py get /apps/reader/log.txt ./reader.log
  python3 tools/sercli.py cmd "cfg get snd_vol"
  python3 tools/sercli.py reboot

协议（固件侧 main/platform/sercli.h）：
  文本命令一行一条；设备回复统一以 "papers3: " 开头，其余是日志，脚本自动丢弃。
  put <path> <size> <crc32>  → "papers3: go" → 原样发 size 字节 → "papers3: ok <size>"
  get <path>                 → "papers3: size <n>" → n 字节 → "papers3: eof"
只用标准库（不依赖 pyserial）：直接开 tty + termios 原始模式。
"""
import argparse
import os
import select
import sys
import termios
import time
import tty
import zlib

PFX = b"papers3: "


class Ser:
    def __init__(self, port, baud=115200):
        self.fd = os.open(port, os.O_RDWR | os.O_NOCTTY)
        attrs = termios.tcgetattr(self.fd)
        tty.setraw(self.fd)
        attrs = termios.tcgetattr(self.fd)
        attrs[4] = attrs[5] = termios.B115200
        termios.tcsetattr(self.fd, termios.TCSANOW, attrs)
        self.buf = b""

    def close(self):
        try:
            os.close(self.fd)
        except OSError:
            pass

    def write(self, data: bytes):
        os.write(self.fd, data)

    def _fill(self, timeout):
        r, _, _ = select.select([self.fd], [], [], timeout)
        if not r:
            return False
        try:
            chunk = os.read(self.fd, 4096)
        except OSError:
            return False
        if not chunk:
            return False
        self.buf += chunk
        return True

    def read_line(self, timeout=3.0, echo_logs=False):
        """读一行；只返回 papers3: 开头的（其它当日志丢掉）"""
        end = time.time() + timeout
        while True:
            nl = self.buf.find(b"\n")
            if nl >= 0:
                line, self.buf = self.buf[:nl], self.buf[nl + 1:]
                line = line.rstrip(b"\r")
                if line.startswith(PFX):
                    return line[len(PFX):].decode("utf-8", "replace")
                if echo_logs and line:
                    print("  [dev] " + line.decode("utf-8", "replace"))
                continue
            if time.time() > end:
                return None
            self._fill(0.1)

    def read_exact(self, n, timeout=30.0):
        end = time.time() + timeout
        out = bytearray()
        if self.buf:
            take = min(n, len(self.buf))
            out += self.buf[:take]
            self.buf = self.buf[take:]
        while len(out) < n:
            if time.time() > end:
                break
            if not self._fill(0.2):
                continue
            take = min(n - len(out), len(self.buf))
            out += self.buf[:take]
            self.buf = self.buf[take:]
        return bytes(out)

    def drain(self, t=0.3):
        while self._fill(t):
            pass
        self.buf = b""


def do_cmd(s: Ser, cmd: str, timeout=8.0, logs=False):
    s.write(cmd.encode() + b"\n")
    lines = []
    end = time.time() + timeout
    while time.time() < end:
        ln = s.read_line(timeout=1.0, echo_logs=logs)
        if ln is None:
            break
        lines.append(ln)
        if ln.startswith("ok") or ln.startswith("err") or ln.startswith("code "):
            break
        if ln.startswith("size ") or ln.startswith("eof"):
            break
    for ln in lines:
        print(ln)
    return lines


def do_put(s: Ser, local: str, remote: str):
    data = open(local, "rb").read()
    crc = zlib.crc32(data) & 0xFFFFFFFF
    size = len(data)
    print(f"→ {local} → {remote}  {size} 字节  crc={crc:08x}")
    s.write(f"put {remote} {size} {crc:08x}\n".encode())
    ln = s.read_line(timeout=6.0)
    if ln is None:
        print("✗ 设备没回话（插好线？串口被别的程序占了？）")
        return 1
    print("  设备:", ln)
    if not ln.startswith("go"):
        return 1
    CH = 4096
    t0 = time.time()
    sent = 0
    for off in range(0, size, CH):
        chunk = data[off:off + CH]
        s.write(chunk)
        sent += len(chunk)
        # 等设备 ack 再发下一块（USB-CDC 无流控，写 SD 时会丢字节）
        ack = s.read_line(timeout=10.0)
        if ack is None or not ack.startswith("ack"):
            print(f"\n✗ 第 {off // CH + 1} 块没收到 ack（收到: {ack}）")
            return 1
        pct = sent * 100 // size
        print(f"\r  发送 {sent}/{size}  ({pct}%)", end="", flush=True)
    print()
    ln = s.read_line(timeout=30.0)
    print(f"  设备: {ln}   用时 {time.time() - t0:.1f}s ({size / 1024 / max(0.01, time.time() - t0):.1f} KB/s)")
    if ln is None or not ln.startswith("ok"):
        return 1
    return 0


def do_get(s: Ser, remote: str, local: str):
    s.write(f"get {remote}\n".encode())
    ln = s.read_line(timeout=6.0)
    if ln is None:
        print("✗ 设备没回话")
        return 1
    if not ln.startswith("size "):
        print("✗", ln)
        return 1
    n = int(ln.split()[1])
    print(f"← {remote}  {n} 字节")
    t0 = time.time()
    data = s.read_exact(n, timeout=60.0)
    ln2 = s.read_line(timeout=5.0)
    if len(data) != n or ln2 != "eof":
        print(f"✗ 收不全：{len(data)}/{n} 尾部={ln2}")
        return 1
    open(local, "wb").write(data)
    print(f"  已存 {local}  用时 {time.time() - t0:.1f}s")
    crc = zlib.crc32(data) & 0xFFFFFFFF
    print(f"  crc={crc:08x}")
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="/dev/ttyACM0")
    ap.add_argument("--logs", action="store_true", help="把设备日志（非 papers3: 行）也打出来，调试 panic 用")
    sub = ap.add_subparsers(dest="op", required=True)
    p = sub.add_parser("put"); p.add_argument("local"); p.add_argument("remote")
    g = sub.add_parser("get"); g.add_argument("remote"); g.add_argument("local")
    c = sub.add_parser("cmd"); c.add_argument("text")
    r = sub.add_parser("reboot")
    for name in ("ls", "free", "ps", "df", "ver"):
        a = sub.add_parser(name); a.add_argument("args", nargs="*")
    args = ap.parse_args()

    s = Ser(args.port)
    s.drain(0.4)
    try:
        if args.op == "put":
            return do_put(s, args.local, args.remote)
        if args.op == "get":
            return do_get(s, args.remote, args.local)
        if args.op == "cmd":
            do_cmd(s, args.text, logs=args.logs)
            return 0
        if args.op == "reboot":
            do_cmd(s, "reboot", timeout=3)
            return 0
        do_cmd(s, " ".join([args.op] + args.args))
        return 0
    finally:
        s.close()


if __name__ == "__main__":
    sys.exit(main())
