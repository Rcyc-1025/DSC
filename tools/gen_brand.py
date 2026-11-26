#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""DSC 品牌素材生成器（位图版）。

不再自绘矢量图：读 resources/brand/src 下的开源素材（Microsoft Fluent Emoji，MIT），
合成应用图标（深蓝圆角底 + 鲸鱼）并导出多尺寸 PNG / ICO，同时生成 Android 各密度
启动图标、自适应图标前景与 drawable 插图。只用 Python 标准库。

用法：
  python tools/gen_brand.py            # 生成/覆盖
  python tools/gen_brand.py --check    # 只校验仓库里的产物与脚本一致
"""
import array
import math
import os
import struct
import sys
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BRAND = os.path.join(ROOT, 'resources', 'brand')
SRC = os.path.join(BRAND, 'src')
CHECK = '--check' in sys.argv

ICON_SIZES = [512, 256, 128, 64, 48, 32, 24, 16]
ICO_SIZES = [16, 24, 32, 48, 64, 128, 256]

# 图标底色：与 src/core/theme.c 的深蓝一致
BG_TOP = (0x2A, 0x7F, 0xBA)
BG_BOTTOM = (0x0B, 0x35, 0x5F)


def fail(msg):
    sys.stderr.write(msg + '\n')
    sys.exit(1)


# ---------------------------------------------------------------- PNG 读取

def load_png(path):
    """读 8 位非隔行 PNG（RGB/RGBA/灰度/调色板），返回 (w, h, array('f') RGBA)。"""
    with open(path, 'rb') as f:
        data = f.read()
    if data[:8] != b'\x89PNG\r\n\x1a\n':
        fail('不是 PNG：%s' % path)
    pos = 8
    idat = b''
    plte = None
    trns = None
    w = h = bd = ct = inter = 0
    while pos < len(data):
        ln = struct.unpack('>I', data[pos:pos + 4])[0]
        typ = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + ln]
        pos += 12 + ln
        if typ == b'IHDR':
            w, h, bd, ct, _comp, _filt, inter = struct.unpack('>IIBBBBB', body)
        elif typ == b'IDAT':
            idat += body
        elif typ == b'PLTE':
            plte = body
        elif typ == b'tRNS':
            trns = body
        elif typ == b'IEND':
            break
    if bd != 8 or inter != 0:
        fail('只支持 8 位非隔行 PNG：%s' % path)
    ch = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ct]
    raw = zlib.decompress(idat)
    stride = w * ch
    plane = bytearray(h * stride)
    prev = bytearray(stride)
    p = 0
    for y in range(h):
        f = raw[p]
        p += 1
        line = bytearray(raw[p:p + stride])
        p += stride
        if f == 1:
            for i in range(ch, stride):
                line[i] = (line[i] + line[i - ch]) & 255
        elif f == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 255
        elif f == 3:
            for i in range(stride):
                a = line[i - ch] if i >= ch else 0
                line[i] = (line[i] + ((a + prev[i]) >> 1)) & 255
        elif f == 4:
            for i in range(stride):
                a = line[i - ch] if i >= ch else 0
                b = prev[i]
                c = prev[i - ch] if i >= ch else 0
                pp = a + b - c
                pa = abs(pp - a)
                pb = abs(pp - b)
                pc = abs(pp - c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 255
        plane[y * stride:(y + 1) * stride] = line
        prev = line

    buf = array.array('f', [0.0]) * (w * h * 4)
    for y in range(h):
        base = y * stride
        obase = y * w * 4
        for x in range(w):
            i = base + x * ch
            o = obase + x * 4
            if ct == 6:
                buf[o] = plane[i]
                buf[o + 1] = plane[i + 1]
                buf[o + 2] = plane[i + 2]
                buf[o + 3] = plane[i + 3]
            elif ct == 2:
                buf[o] = plane[i]
                buf[o + 1] = plane[i + 1]
                buf[o + 2] = plane[i + 2]
                buf[o + 3] = 255.0
            elif ct == 0:
                v = plane[i]
                buf[o] = buf[o + 1] = buf[o + 2] = v
                buf[o + 3] = 255.0
            elif ct == 4:
                v = plane[i]
                buf[o] = buf[o + 1] = buf[o + 2] = v
                buf[o + 3] = plane[i + 1]
            else:
                idx = plane[i]
                buf[o] = plte[idx * 3]
                buf[o + 1] = plte[idx * 3 + 1]
                buf[o + 2] = plte[idx * 3 + 2]
                buf[o + 3] = trns[idx] if (trns and idx < len(trns)) else 255.0
    return w, h, buf


# ---------------------------------------------------------------- PNG 写出

def png_bytes(w, h, buf):
    raw = bytearray()
    stride = w * 4
    for y in range(h):
        raw.append(0)
        o = y * stride
        for x in range(w):
            i = o + x * 4
            a = buf[i + 3]
            if a <= 0.0:
                raw.extend((0, 0, 0, 0))
                continue
            if a >= 255.0:
                raw.extend((int(buf[i] + 0.5), int(buf[i + 1] + 0.5), int(buf[i + 2] + 0.5), 255))
                continue
            k = 255.0 / a
            r = min(255, int(buf[i] * k + 0.5))
            g = min(255, int(buf[i + 1] * k + 0.5))
            b = min(255, int(buf[i + 2] * k + 0.5))
            raw.extend((r, g, b, int(a + 0.5)))

    def chunk(typ, body):
        return (struct.pack('>I', len(body)) + typ + body +
                struct.pack('>I', zlib.crc32(typ + body) & 0xffffffff))

    ihdr = struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0)
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', ihdr) +
            chunk(b'IDAT', zlib.compress(bytes(raw), 9)) + chunk(b'IEND', b''))


def ico_bytes(pngs):
    head = struct.pack('<HHH', 0, 1, len(pngs))
    entries = []
    offset = 6 + 16 * len(pngs)
    body = b''
    for size, data in pngs:
        w = 0 if size >= 256 else size
        h = 0 if size >= 256 else size
        entries.append(struct.pack('<BBBBHHII', w, h, 0, 0, 1, 32, len(data), offset))
        offset += len(data)
        body += data
    return head + b''.join(entries) + body


# ---------------------------------------------------------------- 合成

def new_canvas(size):
    return array.array('f', [0.0]) * (size * size * 4)


def blend(canvas, size, x, y, r, g, b, a):
    i = (y * size + x) * 4
    ia = 1.0 - a
    canvas[i] = r * a + canvas[i] * ia
    canvas[i + 1] = g * a + canvas[i + 1] * ia
    canvas[i + 2] = b * a + canvas[i + 2] * ia
    canvas[i + 3] = 255.0 * a + canvas[i + 3] * ia


def rounded_bg(size, radius_ratio=0.225):
    """深蓝渐变圆角底，圆角与边缘 4x4 超采样做抗锯齿。"""
    canvas = new_canvas(size)
    rad = radius_ratio * size
    ss = 4
    for y in range(size):
        t = y / float(size - 1)
        r0 = BG_TOP[0] + (BG_BOTTOM[0] - BG_TOP[0]) * t
        g0 = BG_TOP[1] + (BG_BOTTOM[1] - BG_TOP[1]) * t
        b0 = BG_TOP[2] + (BG_BOTTOM[2] - BG_TOP[2]) * t
        for x in range(size):
            hit = 0
            for sy in range(ss):
                py = y + (sy + 0.5) / ss
                for sx in range(ss):
                    px = x + (sx + 0.5) / ss
                    cx = min(max(px, rad), size - rad)
                    cy = min(max(py, rad), size - rad)
                    if (px - cx) ** 2 + (py - cy) ** 2 <= rad * rad:
                        hit += 1
            if hit:
                a = hit / float(ss * ss)
                i = (y * size + x) * 4
                canvas[i] = r0
                canvas[i + 1] = g0
                canvas[i + 2] = b0
                canvas[i + 3] = 255.0 * a
    return canvas


def blit(canvas, size, img, w, h, box):
    """把 img 面积平均缩放后叠加到 canvas 的 box=(x0,y0,x1,y1)。"""
    x0, y0, x1, y1 = box
    dw = x1 - x0
    dh = y1 - y0
    for dy in range(dh):
        ty0 = h * dy / float(dh)
        ty1 = h * (dy + 1) / float(dh)
        sy0 = int(ty0)
        sy1 = max(sy0 + 1, int(math.ceil(ty1)))
        for dx in range(dw):
            tx0 = w * dx / float(dw)
            tx1 = w * (dx + 1) / float(dw)
            sx0 = int(tx0)
            sx1 = max(sx0 + 1, int(math.ceil(tx1)))
            r = g = b = a = 0.0
            n = 0
            for sy in range(sy0, min(sy1, h)):
                for sx in range(sx0, min(sx1, w)):
                    i = (sy * w + sx) * 4
                    al = img[i + 3] / 255.0
                    r += img[i] * al
                    g += img[i + 1] * al
                    b += img[i + 2] * al
                    a += al
                    n += 1
            if not n or a <= 0.0:
                continue
            prem = a / n
            blend(canvas, size, x0 + dx, y0 + dy,
                  (r / n) / (a / n), (g / n) / (a / n), (b / n) / (a / n), prem)


def icon_canvas(size, src_path, zoom=0.78, dy=-0.030):
    w, h, img = load_png(src_path)
    canvas = rounded_bg(size)
    side = int(round(size * zoom))
    cx = size / 2.0
    cy = size / 2.0 + size * dy
    box = (int(round(cx - side / 2.0)), int(round(cy - side / 2.0)),
           int(round(cx + side / 2.0)), int(round(cy + side / 2.0)))
    blit(canvas, size, img, w, h, box)
    return png_bytes(size, size, canvas)


def build_icons():
    whale = os.path.join(SRC, 'fluent-whale-3d.png')
    out = []
    buf = []
    for size in ICON_SIZES:
        png = icon_canvas(size, whale)
        out.append((os.path.join('resources', 'brand', 'icon-%d.png' % size), png))
        if size in ICO_SIZES:
            buf.append((size, png))
    out.append((os.path.join('resources', 'brand', 'dsc.ico'), ico_bytes(buf)))
    return out


def build_art():
    """UI 插图：直接用原始素材（不放大，避免糊）。"""
    out = []
    for name, fname in (('whale', 'fluent-whale-3d.png'),
                        ('whale-spout', 'fluent-spouting-3d.png')):
        src = os.path.join(SRC, fname)
        with open(src, 'rb') as f:
            data = f.read()
        out.append((os.path.join('resources', 'brand', '%s.png' % name), data))
    return out


# ---------------------------------------------------------------- Android

# Android 各密度启动图标边长（mdpi 48dp 基准）
ANDROID_MIPMAP = [('mdpi', 48), ('hdpi', 72), ('xhdpi', 96), ('xxhdpi', 144), ('xxxhdpi', 192)]
ANDROID_RES = os.path.join('android', 'app', 'src', 'main', 'res')


def circle_mask(canvas, size):
    """把画布裁成圆形（4x4 超采样）。"""
    rad = size / 2.0
    ss = 4
    for y in range(size):
        for x in range(size):
            hit = 0
            for sy in range(ss):
                py = y + (sy + 0.5) / ss
                for sx in range(ss):
                    px = x + (sx + 0.5) / ss
                    if (px - rad) ** 2 + (py - rad) ** 2 <= rad * rad:
                        hit += 1
            i = (y * size + x) * 4
            canvas[i + 3] = canvas[i + 3] * hit / float(ss * ss)


def round_icon_canvas(size, src_path, zoom=0.74, dy=-0.020):
    w, h, img = load_png(src_path)
    canvas = rounded_bg(size, radius_ratio=0.5)
    circle_mask(canvas, size)
    side = int(round(size * zoom))
    cx = size / 2.0
    cy = size / 2.0 + size * dy
    box = (int(round(cx - side / 2.0)), int(round(cy - side / 2.0)),
           int(round(cx + side / 2.0)), int(round(cy + side / 2.0)))
    blit(canvas, size, img, w, h, box)
    return png_bytes(size, size, canvas)


def adaptive_foreground(size, src_path, zoom=0.56, dy=-0.020):
    """自适应图标前景：透明底，鲸鱼放在 72dp 安全区内（108dp 画布的 2/3）。"""
    w, h, img = load_png(src_path)
    canvas = new_canvas(size)
    side = int(round(size * zoom))
    cx = size / 2.0
    cy = size / 2.0 + size * dy
    box = (int(round(cx - side / 2.0)), int(round(cy - side / 2.0)),
           int(round(cx + side / 2.0)), int(round(cy + side / 2.0)))
    blit(canvas, size, img, w, h, box)
    return png_bytes(size, size, canvas)


def build_android():
    whale = os.path.join(SRC, 'fluent-whale-3d.png')
    out = []
    for dens, size in ANDROID_MIPMAP:
        base = os.path.join(ANDROID_RES, 'mipmap-' + dens)
        out.append((os.path.join(base, 'ic_launcher.png'), icon_canvas(size, whale)))
        out.append((os.path.join(base, 'ic_launcher_round.png'), round_icon_canvas(size, whale)))
    out.append((os.path.join(ANDROID_RES, 'drawable-nodpi', 'ic_launcher_foreground.png'),
                adaptive_foreground(432, whale)))
    # UI 插图：与 Windows 用同一张素材
    with open(whale, 'rb') as f:
        out.append((os.path.join(ANDROID_RES, 'drawable-nodpi', 'whale.png'), f.read()))
    # 自适应图标（Android 8+）
    adaptive = ('<?xml version="1.0" encoding="utf-8"?>\n'
                '<!-- 0.1.6：自适应启动图标（前景为 resources/brand 的 Fluent 鲸鱼，MIT） -->\n'
                '<adaptive-icon xmlns:android="http://schemas.android.com/apk/res/android">\n'
                '    <background android:drawable="@drawable/ic_launcher_background" />\n'
                '    <foreground android:drawable="@drawable/ic_launcher_foreground" />\n'
                '</adaptive-icon>\n')
    out.append((os.path.join(ANDROID_RES, 'mipmap-anydpi-v26', 'ic_launcher.xml'),
                adaptive.encode('utf-8')))
    out.append((os.path.join(ANDROID_RES, 'mipmap-anydpi-v26', 'ic_launcher_round.xml'),
                adaptive.encode('utf-8')))
    bg = ('<?xml version="1.0" encoding="utf-8"?>\n'
          '<!-- 0.1.6：图标底色（与 resources/brand 的深蓝渐变一致） -->\n'
          '<shape xmlns:android="http://schemas.android.com/apk/res/android"'
          ' android:shape="rectangle">\n'
          '    <gradient android:type="linear" android:angle="270"'
          ' android:startColor="#FF2A7FBA" android:endColor="#FF0B355F" />\n'
          '</shape>\n')
    out.append((os.path.join(ANDROID_RES, 'drawable', 'ic_launcher_background.xml'),
                bg.encode('utf-8')))
    return out


# ---------------------------------------------------------------- 写入

def emit(items):
    bad = []
    for rel, data in items:
        path = os.path.join(ROOT, rel)
        d = os.path.dirname(path)
        if not os.path.isdir(d):
            os.makedirs(d)
        if CHECK:
            if not os.path.isfile(path):
                bad.append(rel + '（缺文件）')
                continue
            with open(path, 'rb') as f:
                if f.read() != data:
                    bad.append(rel + '（内容不一致）')
            continue
        with open(path, 'wb') as f:
            f.write(data)
        print('written: %s（%d 字节）' % (rel, len(data)))
    if CHECK:
        if bad:
            fail('品牌素材与脚本不一致：\n  ' + '\n  '.join(bad))
        print('check: 品牌素材与脚本一致（%d 个文件）' % len(items))


def main():
    emit(build_icons() + build_art() + build_android())


if __name__ == '__main__':
    main()
