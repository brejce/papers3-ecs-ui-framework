#!/usr/bin/env python3
"""
墨水屏屏保图像工具：把任意图片（或内置图案）转成设备能读的 .gray4

★ **不自己造格式**：m5gfx（LovyanGFX 系）自带 PNG/JPEG/BMP 解码器（`drawPng`/`drawJpg`，
  解码器 pngle 已经链接进固件），SD 卡上放标准 PNG 就能画。这个脚本只负责产出那张 PNG：
  设备端不干重活，PC 端一次把 16 级灰量化好。

═══ ★ 作者该用哪些灰度值（重要）═══
面板写像素时的量化是（见 vendored m5gfx `Panel_EPD.inl`）：

    out = clamp((v + Bayer[i] - 8) >> 4, 0, 15)

把它当等式解：要让输出**恒定**等于第 L 级，v + Bayer[i] - 8 必须对 Bayer 的全部
16 个取值（0..15）都落在 [16L, 16L+15] 里 —— 唯一的解是 **v = 16L + 8**。
别的 v 会产生「4×4 有序抖动」：一片本该是平的灰会变成相邻两级交错的小麻点。

所以产出的 PNG 里的灰一律取 **16L+8** —— 设备端画出来就是你要的那一级，干净的平灰。

用法
    # 内置图案（原来那个"月亮+星星+远山"，见 ~/Project/frame/screensaver.cpp）
    uv run --with pillow python tools/make_screensaver.py --builtin moon -o screen.png

    # 任意图片
    uv run --with pillow python tools/make_screensaver.py photo.jpg -o screen.png
    uv run --with pillow python tools/make_screensaver.py photo.jpg -o screen.png --dither  # 照片建议开

产出的 PNG 传到 SD 卡：/sdcard/screensaver/xxx.png （PNG 本身就是"设备上会看到的样子"）
"""

import argparse
import os
import sys

from PIL import Image, ImageDraw

W_DEFAULT, H_DEFAULT = 540, 960  # M5PaperS3


def lv(n: int) -> int:
    """第 n 级 → 面板实际显示的 8bit 灰度。设备端也用这条式子。"""
    return min(255, 16 * n + 8)


# ══════════════════════════════════════════════════════
# 内置图案：月亮 + 星星 + 远山
# 逐句搬自 ~/Project/frame/screensaver.cpp（几何全部保持一致，
# 只是把 _GRAY[n] 换成"级别 n"，好让面板一级不差地画出来）
# ══════════════════════════════════════════════════════
def render_moon(w: int = W_DEFAULT, h: int = H_DEFAULT) -> Image.Image:
    im = Image.new("L", (w, h), 2)          # 深空底色 = 第 2 级
    d = ImageDraw.Draw(im)

    # ---- 月亮（右上方）：先画满月，再用同色天空咬掉一块成月牙 ----
    mx, my, mr = w * 3 // 4, h // 4, h // 5
    d.ellipse([mx - mr, my - mr, mx + mr, my + mr], fill=15)
    d.ellipse([mx + mr // 3 - mr, my - mr // 4 - mr,
               mx + mr // 3 + mr, my - mr // 4 + mr], fill=2)
    d.ellipse([mx - mr - 3, my - mr - 3, mx + mr + 3, my + mr + 3], outline=10)
    d.ellipse([mx - mr - 6, my - mr - 6, mx + mr + 6, my + mr + 6], outline=6)

    # ---- 星星（r>=3 的多画一个十字星芒）----
    stars = [
        (w // 6,   h // 8,   3, 14),
        (w // 3,   h // 5,   2, 15),
        (w // 2,   h // 7,   1, 13),
        (w * 2 // 3, h // 3, 2, 15),
        (w * 5 // 6, h // 6, 1, 14),
        (w // 5,   h // 3,   2, 13),
        (w * 4 // 5, h // 2, 2, 15),
        (w // 4,   h // 2,   1, 14),
        (w // 2,   h // 3,   2, 15),
        (w * 7 // 8, h // 4, 1, 13),
        (w * 3 // 4, h * 2 // 3, 2, 14),
        (w // 8,   h * 2 // 3, 1, 13),
    ]
    for x, y, r, c in stars:
        d.ellipse([x - r, y - r, x + r, y + r], fill=c)
        if r >= 3:
            for dx, dy in ((-2, 0), (2, 0), (0, -2), (0, 2)):
                px, py = x + dx, y + dy
                if 0 <= px < w and 0 <= py < h:
                    im.putpixel((px, py), c)

    # ---- 远山剪影：底部 1/4 铺黑，再用 6 个黑三角把山脊顶上去 ----
    d.rectangle([0, h * 3 // 4, w - 1, h - 1], fill=0)
    peaks = [0, h * 3 // 4, w // 8, h * 3 // 5, w // 4, h * 3 // 4 - 10,
             w * 3 // 8, h * 2 // 3, w // 2, h * 3 // 4 - 5,
             w * 5 // 8, h * 3 // 5, w * 3 // 4, h * 3 // 4 - 8, w, h * 3 // 4]
    for i in range(6):
        x0, y0 = peaks[2 * i], peaks[2 * i + 1]
        x1, y1 = peaks[2 * i + 2], peaks[2 * i + 3]
        d.polygon([(0, h), (x0, y0), (x1, y1)], fill=0)

    # ---- 山脊上的三道亮线（雪线）----
    for cx, cy, ln in ((w // 8, h * 3 // 5, 15),
                       (w * 3 // 8, h * 2 // 3, 12),
                       (w * 5 // 8, h * 3 // 5, 15)):
        d.line([(cx - 5, cy - 1), (cx - 5 + ln, cy - 1)], fill=12)

    # ---- 两道流星 ----
    d.line([(w // 10, h // 10), (w // 5, h // 6)], fill=13)
    d.line([(w // 10 - 1, h // 10), (w // 5 - 2, h // 6)], fill=11)
    d.line([(w * 4 // 5, h // 8), (w * 9 // 10, h // 6)], fill=13)

    return im


# ══════════════════════════════════════════════════════
# 图片 → 级别（0..15）
# ══════════════════════════════════════════════════════
def to_levels(img: Image.Image, dither: bool = False) -> Image.Image:
    g = img.convert("L")
    w, h = g.size

    if not dither:
        # 最省事也最"像稿子"的做法：直接取最近的级别。平色块/线稿用这个。
        return g.point(lambda v: max(0, min(15, (v - 8 + 8) // 16)))

    # Floyd–Steinberg 误差扩散，目标亮度就是 16L+8（照片建议开，避免大面积色带）
    buf = [float(v) for v in g.getdata()]
    out = bytearray(w * h)
    for y in range(h):
        for x in range(w):
            i = y * w + x
            v = buf[i]
            L = max(0, min(15, int((v - 8) / 16 + 0.5)))
            out[i] = L
            err = v - lv(L)
            for dx, dy, k in ((1, 0, 7 / 16), (-1, 1, 3 / 16), (0, 1, 5 / 16), (1, 1, 1 / 16)):
                nx, ny = x + dx, y + dy
                if 0 <= nx < w and 0 <= ny < h:
                    buf[ny * w + nx] += err * k
    im = Image.new("L", (w, h))
    im.putdata(list(out))
    return im


def save_png(levels: Image.Image, path: str) -> None:
    """级别 → PNG：灰值一律 16L+8（推导见文件头），存成 8bit 单通道灰度。"""
    out = levels.point(lambda L: lv(int(L)))
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    out.save(path, optimize=True)


def main() -> int:
    ap = argparse.ArgumentParser(description="生成墨水屏屏保 PNG（16 级灰）")
    ap.add_argument("image", nargs="?", help="输入图片；用 --builtin 时不用给")
    ap.add_argument("--builtin", choices=["moon"], help="用内置图案")
    ap.add_argument("-o", "--out", required=True, help="输出 .png")
    ap.add_argument("--dither", action="store_true", help="误差扩散（照片建议开）")
    ap.add_argument("--size", help="强制尺寸 WxH，默认按图片/540x960")
    args = ap.parse_args()

    if args.builtin == "moon":
        w, h = W_DEFAULT, H_DEFAULT
        if args.size:
            w, h = (int(v) for v in args.size.lower().split("x"))
        levels = render_moon(w, h)                 # 本身就是级别，无需再量化
    elif args.image:
        im = Image.open(args.image)
        if args.size:
            tw, th = (int(v) for v in args.size.lower().split("x"))
            # ★ 先按目标宽高比「居中裁剪」，再缩放 —— 直接 resize 会把图压扁/拉长（比例不对）
            src_ratio = im.width / im.height
            dst_ratio = tw / th
            if src_ratio > dst_ratio:               # 原图更宽 → 裁左右
                new_w = int(im.height * dst_ratio)
                x0 = (im.width - new_w) // 2
                im = im.crop((x0, 0, x0 + new_w, im.height))
            elif src_ratio < dst_ratio:             # 原图更高 → 裁上下
                new_h = int(im.width / dst_ratio)
                y0 = (im.height - new_h) // 2
                im = im.crop((0, y0, im.width, y0 + new_h))
            im = im.resize((tw, th), Image.LANCZOS)
        elif im.size != (W_DEFAULT, H_DEFAULT):
            print(f"[提示] 输入 {im.size} ≠ {W_DEFAULT}x{H_DEFAULT}，按原尺寸输出；"
                  f"要适配屏幕加 --size {W_DEFAULT}x{H_DEFAULT}", file=sys.stderr)
        levels = to_levels(im, args.dither)
    else:
        ap.error("要么给图片，要么 --builtin moon")

    save_png(levels, args.out)
    lo, hi = levels.getextrema()
    print(f"OK  {args.out}  {levels.size[0]}x{levels.size[1]}  16 级灰  "
          f"用到级别 {lo}..{hi}  {os.path.getsize(args.out)} 字节")
    print(f"   传到 SD 卡：/sdcard/screensaver/{os.path.basename(args.out)}")
    print(f"   设备端：M5.Display.drawPng(缓冲, 长度, 0, 0, {levels.size[0]}, {levels.size[1]})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
