#!/usr/bin/env python3
"""
svg2glyph.py OUT.c NAME CODEPOINT SVG SIZE[,SIZE...] [LIC]
  or multi-glyph:
svg2glyph.py OUT.c NAME START_CP SIZE[,SIZE...] SVG1 [SVG2 ...] [--lic LIC] [--show]

Generates an LVGL font per SIZE (NAME_SIZE) holding one or more SVG icon paths
(24x24 Material-style icons: M/m L/l H/h V/v C/c S/s Q/q A/a Z/z commands) at
consecutive codepoints starting at CODEPOINT / START_CP. 4 bits per pixel, 8x8
supersampling, nonzero fill. Byte-aligned per glyph. No external dependencies.
"""
import math
import re
import sys

def parse_args(argv):
    show = False
    lic = ''
    pos = []
    i = 1
    while i < len(argv):
        a = argv[i]
        if a == '--show':
            show = True
        elif a == '--lic' and i + 1 < len(argv):
            lic = argv[i + 1]
            i += 1
        else:
            pos.append(a)
        i += 1
    if len(pos) < 5:
        raise SystemExit('usage: svg2glyph.py OUT.c NAME CP SIZE[,SIZE...] SVG1 [SVG2 ...] [--lic LIC] [--show]')
    out, name, cp = pos[0], pos[1], int(pos[2], 0)
    # Support both legacy (OUT NAME CP SVG SIZES [LIC]) and multi-glyph (OUT NAME CP SIZES SVG...)
    if re.fullmatch(r'\d+(?:,\d+)*', pos[3]):
        sizes = [int(s) for s in pos[3].split(',')]
        svgs = pos[4:]
    else:
        svgs = [pos[3]]
        sizes = [int(s) for s in pos[4].split(',')]
        if len(pos) > 5 and not lic:
            lic = pos[5]
    return out, name, cp, sizes, svgs, lic, show

def tokens(d):
    for t in re.findall(r'[A-Za-z]|-?(?:\d+\.?\d*|\.\d+)(?:[eE][+-]?\d+)?', d):
        yield t

def arc(x1, y1, rx, ry, phi, fa, fs, x2, y2):
    """SVG endpoint arc -> points (F.6.5 of the SVG spec)"""
    if x1 == x2 and y1 == y2:
        return []
    if rx == 0 or ry == 0:
        return [(x2, y2)]
    c, s = math.cos(phi), math.sin(phi)
    dx, dy = (x1 - x2) / 2, (y1 - y2) / 2
    x1p, y1p = c * dx + s * dy, -s * dx + c * dy
    rx, ry = abs(rx), abs(ry)
    lam = x1p ** 2 / rx ** 2 + y1p ** 2 / ry ** 2
    if lam > 1:
        rx, ry = rx * math.sqrt(lam), ry * math.sqrt(lam)
    den = rx * rx * y1p * y1p + ry * ry * x1p * x1p
    if den == 0:
        return [(x2, y2)]
    num = rx * rx * ry * ry - den
    co = math.sqrt(max(0.0, num / den))
    if fa == fs:
        co = -co
    cxp, cyp = co * rx * y1p / ry, -co * ry * x1p / rx
    cx, cy = c * cxp - s * cyp + (x1 + x2) / 2, s * cxp + c * cyp + (y1 + y2) / 2
    def ang(ux, uy, vx, vy):
        return math.atan2(ux * vy - uy * vx, ux * vx + uy * vy)
    t1 = ang(1, 0, (x1p - cxp) / rx, (y1p - cyp) / ry)
    dt = ang((x1p - cxp) / rx, (y1p - cyp) / ry, (-x1p - cxp) / rx, (-y1p - cyp) / ry)
    if not fs and dt > 0:
        dt -= 2 * math.pi
    elif fs and dt < 0:
        dt += 2 * math.pi
    n = max(4, int(abs(dt) / (math.pi / 32)))
    pts = []
    for k in range(1, n + 1):
        t = t1 + dt * k / n
        pts.append((c * rx * math.cos(t) - s * ry * math.sin(t) + cx,
                    s * rx * math.cos(t) + c * ry * math.sin(t) + cy))
    return pts

def flatten(d):
    """polygons (lists of points) in the 24-unit box"""
    polys, cur = [], []
    x = y = sx = sy = 0.0
    last_c = None
    last_ctrl = None
    it = list(tokens(d))
    i, cmd = 0, None
    def num():
        nonlocal i
        v = float(it[i]); i += 1
        return v
    def cubic(x1, y1, x2, y2, x3, y3):
        nonlocal x, y, last_ctrl
        for k in range(1, 17):
            t = k / 16; u = 1 - t
            cur.append((u ** 3 * x + 3 * u * u * t * x1 + 3 * u * t * t * x2 + t ** 3 * x3,
                        u ** 3 * y + 3 * u * u * t * y1 + 3 * u * t * t * y2 + t ** 3 * y3))
        last_ctrl = (x2, y2)
        x, y = x3, y3
    def quad(x1, y1, x2, y2):
        nonlocal x, y, last_ctrl
        for k in range(1, 13):
            t = k / 12; u = 1 - t
            cur.append((u * u * x + 2 * u * t * x1 + t * t * x2,
                        u * u * y + 2 * u * t * y1 + t * t * y2))
        last_ctrl = (x1, y1)
        x, y = x2, y2
    while i < len(it):
        if it[i].isalpha():
            cmd = it[i]; i += 1
        rel = cmd.islower()
        uc = cmd.upper()
        if uc == 'M':
            if len(cur) > 1:
                polys.append(cur)
            nx, ny = num(), num()
            if rel and cur:
                x, y = x + nx, y + ny
            elif rel and (x != 0.0 or y != 0.0):
                x, y = x + nx, y + ny
            else:
                x, y = nx, ny
            sx, sy = x, y
            cur = [(x, y)]
            cmd = 'l' if rel else 'L'
            last_ctrl = None
        elif uc == 'L':
            nx, ny = num(), num()
            x, y = (x + nx, y + ny) if rel else (nx, ny)
            cur.append((x, y))
            last_ctrl = None
        elif uc == 'H':
            nx = num()
            x = x + nx if rel else nx
            cur.append((x, y))
            last_ctrl = None
        elif uc == 'V':
            ny = num()
            y = y + ny if rel else ny
            cur.append((x, y))
            last_ctrl = None
        elif uc == 'C':
            x1, y1, x2, y2, x3, y3 = [num() for _ in range(6)]
            if rel:
                x1, y1, x2, y2, x3, y3 = x + x1, y + y1, x + x2, y + y2, x + x3, y + y3
            cubic(x1, y1, x2, y2, x3, y3)
        elif uc == 'S':
            x2, y2, x3, y3 = [num() for _ in range(4)]
            if rel:
                x2, y2, x3, y3 = x + x2, y + y2, x + x3, y + y3
            if last_c in ('C', 'S') and last_ctrl is not None:
                x1, y1 = 2 * x - last_ctrl[0], 2 * y - last_ctrl[1]
            else:
                x1, y1 = x, y
            cubic(x1, y1, x2, y2, x3, y3)
        elif uc == 'Q':
            x1, y1, x2, y2 = [num() for _ in range(4)]
            if rel:
                x1, y1, x2, y2 = x + x1, y + y1, x + x2, y + y2
            quad(x1, y1, x2, y2)
        elif uc == 'T':
            x2, y2 = num(), num()
            if rel:
                x2, y2 = x + x2, y + y2
            if last_c in ('Q', 'T') and last_ctrl is not None:
                x1, y1 = 2 * x - last_ctrl[0], 2 * y - last_ctrl[1]
            else:
                x1, y1 = x, y
            quad(x1, y1, x2, y2)
        elif uc == 'A':
            rx, ry, rot, large, sweep, x2, y2 = [num() for _ in range(7)]
            if rel:
                x2, y2 = x + x2, y + y2
            cur.extend(arc(x, y, rx, ry, math.radians(rot), int(large), int(sweep), x2, y2))
            x, y = x2, y2
            last_ctrl = None
        elif uc == 'Z':
            if len(cur) > 1:
                polys.append(cur)
            cur = []
            x, y = sx, sy
            last_ctrl = None
        else:
            raise SystemExit('svg2glyph: command %s not handled' % cmd)
        last_c = uc
    if len(cur) > 1:
        polys.append(cur)
    return polys

def raster(polys, scale, ss=8):
    xs = [p[0] for q in polys for p in q]; ys = [p[1] for q in polys for p in q]
    x0, x1 = math.floor(min(xs) * scale), math.ceil(max(xs) * scale)
    y0, y1 = math.floor(min(ys) * scale), math.ceil(max(ys) * scale)
    w, h = x1 - x0, y1 - y0
    edges = []
    for q in polys:
        for a, b in zip(q, q[1:] + q[:1]):
            if a[1] != b[1]:
                edges.append((a[0] * scale - x0, a[1] * scale - y0, b[0] * scale - x0, b[1] * scale - y0))
    acc = [[0] * w for _ in range(h)]
    for py in range(h * ss):
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
            for px in range(max(0, math.ceil(xa * ss - 0.5)), min(w * ss, math.ceil(xb * ss - 0.5))):
                acc[py // ss][px // ss] += 1
    return [[min(255, v * 255 // (ss * ss)) for v in r] for r in acc], w, h, x0, y0

out, name, cp, sizes, svgs, lic, show = parse_args(sys.argv)

all_polys = []
svg_names = []
for svg in svgs:
    text = open(svg).read()
    paths = re.findall(r'\sd="([^"]+)"', text)
    if not paths:
        raise SystemExit('svg2glyph: no path d="..." in %s' % svg)
    polys = []
    for d in paths:
        polys.extend(flatten(d))
    all_polys.append(polys)
    svg_names.append(svg.split('/')[-1])

c = [
    '/*',
    ' * %s: %d glyph(s) (U+%04X..U+%04X) from %s,' % (
        name, len(svgs), cp, cp + len(svgs) - 1, ', '.join(svg_names)),
    ' * made by tools/svg2glyph.py.%s' % (' ' + lic if lic else ''),
    ' */',
    '#include "lvgl/lvgl.h"',
    '',
]

for size in sizes:
    scale = size * 1.08 / 24
    adv = round(24 * scale * 16)
    centre = size * 9 / 24
    n = '%s_%d' % (name, size)
    all_bytes = []
    glyph_rows = ['\t{ .bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0 },']
    for idx, (svg_name, polys) in enumerate(zip(svg_names, all_polys)):
        bm, w, h, x0, y0 = raster(polys, scale)
        ofs_y = round(centre - h / 2)
        px = [(v * 15 + 127) // 255 for r in bm for v in r]
        if len(px) & 1:
            px.append(0)
        bidx = len(all_bytes)
        for k in range(0, len(px), 2):
            all_bytes.append(px[k] << 4 | px[k + 1])
        glyph_rows.append(
            '\t{ .bitmap_index = %d, .adv_w = %d, .box_w = %d, .box_h = %d, .ofs_x = %d, .ofs_y = %d }, /* U+%04X %s */' %
            (bidx, adv, w, h, x0, ofs_y, cp + idx, svg_name)
        )
        if show:
            print('=== %s %dpx U+%04X (%s) %dx%d ===' % (name, size, cp + idx, svg_name, w, h))
            for r in bm:
                print(''.join(' .:-=+*#%@'[v * 9 // 255] for v in r))
    data = ', '.join('0x%02x' % b for b in all_bytes)
    c += [
        'static const uint8_t %s_bitmap[] = { %s };' % (n, data), '',
        'static const lv_font_fmt_txt_glyph_dsc_t %s_glyphs[] = {' % n,
        *glyph_rows,
        '};', '',
        'static const lv_font_fmt_txt_cmap_t %s_cmaps[] = {' % n,
        '\t{ .range_start = 0x%X, .range_length = %d, .glyph_id_start = 1,' % (cp, len(svgs)),
        '\t  .type = LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY },',
        '};', '',
        'static const lv_font_fmt_txt_dsc_t %s_dsc = {' % n,
        '\t.glyph_bitmap = %s_bitmap, .glyph_dsc = %s_glyphs, .cmaps = %s_cmaps,' % (n, n, n),
        '\t.cmap_num = 1, .bpp = 4,',
        '};', '',
        'const lv_font_t %s = {' % n,
        '\t.get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,',
        '\t.get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,',
        '\t.line_height = %d, .base_line = %d,' % (round(size * 1.125), round(size * 0.21)),
        '\t.dsc = &%s_dsc,' % n,
        '};', '',
    ]
open(out, 'w').write('\n'.join(c))
