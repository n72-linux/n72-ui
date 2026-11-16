#!/usr/bin/env python3
"""
svg2glyph.py OUT.c NAME CODEPOINT SVG SIZE[,SIZE...] : an LVGL font per SIZE
(NAME_SIZE) holding one glyph, an SVG icon's path (a 24x24 Material-style
icon: absolute M L H V C A Z commands), at CODEPOINT. 8 bits per pixel, 8x8
samples a pixel, nonzero fill. Sized like Montserrat's FontAwesome symbols
(the 24-unit box at 1.08 em, centred where they are: 9/24 em above the
baseline at 24 px). No dependencies.
"""
import math
import re
import sys

out, name, cp, svg = sys.argv[1], sys.argv[2], int(sys.argv[3], 0), sys.argv[4]
sizes = [int(s) for s in sys.argv[5].split(',')]
d = re.search(r'\sd="([^"]+)"', open(svg).read()).group(1)
lic = sys.argv[6] if len(sys.argv) > 6 else ''

def tokens(d):
    for t in re.findall(r'[A-Za-z]|-?(?:\d+\.?\d*|\.\d+)(?:e-?\d+)?', d):
        yield t

def flatten(d):
    """polygons (lists of points) in the 24-unit box"""
    polys, cur = [], []
    x = y = sx = sy = 0.0
    it = list(tokens(d))
    i, cmd = 0, None
    def num():
        nonlocal i
        v = float(it[i]); i += 1
        return v
    while i < len(it):
        if it[i].isalpha():
            cmd = it[i]; i += 1
        if cmd == 'M':
            if len(cur) > 1:
                polys.append(cur)
            x, y = num(), num(); sx, sy = x, y; cur = [(x, y)]; cmd = 'L'
        elif cmd == 'L':
            x, y = num(), num(); cur.append((x, y))
        elif cmd == 'H':
            x = num(); cur.append((x, y))
        elif cmd == 'V':
            y = num(); cur.append((x, y))
        elif cmd == 'C':
            x1, y1, x2, y2, x3, y3 = [num() for _ in range(6)]
            for k in range(1, 17):
                t = k / 16; u = 1 - t
                cur.append((u**3*x + 3*u*u*t*x1 + 3*u*t*t*x2 + t**3*x3,
                            u**3*y + 3*u*u*t*y1 + 3*u*t*t*y2 + t**3*y3))
            x, y = x3, y3
        elif cmd == 'A':
            rx, ry, rot, large, sweep, x2, y2 = [num() for _ in range(7)]
            cur.extend(arc(x, y, rx, ry, math.radians(rot), int(large), int(sweep), x2, y2))
            x, y = x2, y2
        elif cmd in 'Zz':
            if len(cur) > 1:
                polys.append(cur)
            cur = []; x, y = sx, sy
        else:
            raise SystemExit('svg2glyph: command %s not handled' % cmd)
    if len(cur) > 1:
        polys.append(cur)
    return polys

def arc(x1, y1, rx, ry, phi, fa, fs, x2, y2):
    """SVG endpoint arc -> points (F.6.5 of the SVG spec)"""
    c, s = math.cos(phi), math.sin(phi)
    dx, dy = (x1 - x2) / 2, (y1 - y2) / 2
    x1p, y1p = c*dx + s*dy, -s*dx + c*dy
    rx, ry = abs(rx), abs(ry)
    lam = x1p**2/rx**2 + y1p**2/ry**2
    if lam > 1:
        rx, ry = rx*math.sqrt(lam), ry*math.sqrt(lam)
    num = rx*rx*ry*ry - rx*rx*y1p*y1p - ry*ry*x1p*x1p
    co = math.sqrt(max(0, num / (rx*rx*y1p*y1p + ry*ry*x1p*x1p)))
    if fa == fs:
        co = -co
    cxp, cyp = co*rx*y1p/ry, -co*ry*x1p/rx
    cx, cy = c*cxp - s*cyp + (x1+x2)/2, s*cxp + c*cyp + (y1+y2)/2
    def ang(ux, uy, vx, vy):
        a = math.atan2(ux*vy - uy*vx, ux*vx + uy*vy)
        return a
    t1 = ang(1, 0, (x1p-cxp)/rx, (y1p-cyp)/ry)
    dt = ang((x1p-cxp)/rx, (y1p-cyp)/ry, (-x1p-cxp)/rx, (-y1p-cyp)/ry)
    if not fs and dt > 0:
        dt -= 2*math.pi
    elif fs and dt < 0:
        dt += 2*math.pi
    n = max(4, int(abs(dt) / (math.pi / 32)))
    pts = []
    for k in range(1, n + 1):
        t = t1 + dt * k / n
        pts.append((c*rx*math.cos(t) - s*ry*math.sin(t) + cx,
                    s*rx*math.cos(t) + c*ry*math.sin(t) + cy))
    return pts

def raster(polys, scale, ss=8):
    xs = [p[0] for q in polys for p in q]; ys = [p[1] for q in polys for p in q]
    x0, x1 = math.floor(min(xs)*scale), math.ceil(max(xs)*scale)
    y0, y1 = math.floor(min(ys)*scale), math.ceil(max(ys)*scale)
    w, h = x1 - x0, y1 - y0
    edges = []
    for q in polys:
        for a, b in zip(q, q[1:] + q[:1]):
            if a[1] != b[1]:
                edges.append((a[0]*scale - x0, a[1]*scale - y0, b[0]*scale - x0, b[1]*scale - y0))
    acc = [[0]*w for _ in range(h)]
    for py in range(h*ss):
        sy = (py + 0.5) / ss
        xs = []
        for ax, ay, bx, by in edges:
            if (ay <= sy < by) or (by <= sy < ay):
                xs.append((ax + (sy - ay) * (bx - ax) / (by - ay), 1 if by > ay else -1))
        xs.sort()
        wind = 0
        for k in range(len(xs) - 1):
            wind += xs[k][1]
            if wind == 0:
                continue
            xa, xb = xs[k][0], xs[k + 1][0]
            for px in range(max(0, math.ceil(xa*ss - 0.5)), min(w*ss, math.ceil(xb*ss - 0.5))):
                acc[py // ss][px // ss] += 1
    return [[min(255, v * 255 // (ss*ss)) for v in r] for r in acc], w, h, x0, y0

polys = flatten(d)
c = ['/*', ' * %s: one glyph (U+%04X) from %s,' % (name, cp, svg.split('/')[-1]),
     ' * made by tools/svg2glyph.py. %s' % lic, ' */', '#include "lvgl/lvgl.h"', '']
for size in sizes:
    scale = size * 1.08 / 24
    bm, w, h, x0, y0 = raster(polys, scale)
    adv = round(24 * scale * 16)
    centre = size * 9 / 24			# above the baseline
    ofs_y = round(centre - h / 2)
    n = '%s_%d' % (name, size)
    data = ', '.join('0x%02x' % v for r in bm for v in r)
    c += ['static const uint8_t %s_bitmap[] = { %s };' % (n, data), '',
          'static const lv_font_fmt_txt_glyph_dsc_t %s_glyphs[] = {' % n,
          '\t{ .bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0 },',
          '\t{ .bitmap_index = 0, .adv_w = %d, .box_w = %d, .box_h = %d, .ofs_x = %d, .ofs_y = %d },' %
          (adv, w, h, x0, ofs_y), '};', '',
          'static const lv_font_fmt_txt_cmap_t %s_cmaps[] = {' % n,
          '\t{ .range_start = 0x%X, .range_length = 1, .glyph_id_start = 1,' % cp,
          '\t  .type = LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY },', '};', '',
          'static const lv_font_fmt_txt_dsc_t %s_dsc = {' % n,
          '\t.glyph_bitmap = %s_bitmap, .glyph_dsc = %s_glyphs, .cmaps = %s_cmaps,' % (n, n, n),
          '\t.cmap_num = 1, .bpp = 8,', '};', '',
          'const lv_font_t %s = {' % n,
          '\t.get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,',
          '\t.get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,',
          '\t.line_height = %d, .base_line = %d,' % (round(size * 1.125), round(size * 0.21)),
          '\t.dsc = &%s_dsc,' % n, '};', '']
    if '--show' in sys.argv:
        for r in bm:
            print(''.join(' .:-=+*#%@'[v * 9 // 255] for v in r))
open(out, 'w').write('\n'.join(c))
