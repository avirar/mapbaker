#!/usr/bin/env python3
"""Generate the unified 4-layer viewers and gallery for every manifest map."""
import json
import os
import re
import shutil
import subprocess

import numpy as np
from PIL import Image

ROOT = '/media/storage/mapbaker/final'
MANIFEST = ROOT + '/manifest.json'
CONTINENTS = {0: 'Eastern Kingdoms', 1: 'Kalimdor', 530: 'Outland', 571: 'Northrend'}
CROP = {329: (37, 24, 9), 617: (30, 29, 1)}


def slugify(s):
    return re.sub(r'[^a-z0-9]+', '', s.lower()) or 'map'


def pretty(s):
    return re.sub(r'([a-z])([A-Z])', r'\1 \2', s)


def main():
    data = json.load(open(MANIFEST))
    cards = []
    failed = []
    for rec in data['maps']:
        if 'error' in rec:
            continue
        mid = rec['id']
        slug = slugify(rec['dir'])
        folder = '%d_%s' % (mid, slug)
        out = os.path.join(ROOT, 'maps', folder, 'out')
        need = [slug + '-scene.dzi', slug + '-interiors.dzi',
                slug + '-quads-ext.dzi', slug + '-quads-int.dzi']
        if not all(os.path.exists(os.path.join(out, n)) for n in need):
            failed.append((mid, 'missing layer'))
            continue
        if mid in CROP:
            tx0, ty0, count = CROP[mid]
        else:
            tx0, ty0, count = rec['tx0'], rec['ty0'], rec['count']
        px = 1024 if mid in CONTINENTS else 4096
        title = (CONTINENTS.get(mid) or pretty(rec['dir'])) + ' — map %d' % mid

        has_ext = 0
        try:
            ext_preview = os.path.join(out, slug + '-quads-ext-preview.png')
            a = np.asarray(Image.open(ext_preview).convert('RGBA'))[:, :, 3]
            has_ext = 1 if float(a.mean()) > 0.5 else 0
        except Exception:
            has_ext = 0
        has_int = 0
        try:
            int_preview = os.path.join(out, slug + '-quads-int-preview.png')
            a = np.asarray(Image.open(int_preview).convert('RGBA'))[:, :, 3]
            has_int = 1 if float(a.mean()) > 0.5 else 0
        except Exception:
            has_int = 0
        has_groups = 0
        try:
            jpath = os.path.join(out, 'interior-art.json')
            if os.path.exists(jpath):
                has_groups = 1 if len(json.load(open(jpath))) > 0 else 0
        except Exception:
            has_groups = 0
        has_3d = 0
        try:
            jpath3 = os.path.join(out, 'interior-3d.json')
            if os.path.exists(jpath3):
                has_3d = 1 if len(json.load(open(jpath3))) > 0 else 0
        except Exception:
            has_3d = 0

        fit = ''
        if mid not in CONTINENTS:
            preview = os.path.join(out, slug + '-interiors-preview.png')
            try:
                im = Image.open(preview).convert('RGBA')
                a = np.asarray(im)[:, :, 3]
                ys, xs = np.where(a > 8)
                if len(xs):
                    pw, ph = im.size
                    x0, x1 = float(xs.min()), float(xs.max() + 1)
                    y0, y1 = float(ys.min()), float(ys.max() + 1)
                    if ((x1 - x0) * (y1 - y0)) / (pw * ph) < 0.85:
                        mx = (x1 - x0) * 0.03
                        my = (y1 - y0) * 0.03
                        x0 = max(0, x0 - mx)
                        y0 = max(0, y0 - my)
                        x1 = min(pw, x1 + mx)
                        y1 = min(ph, y1 + my)
                        full_w = (rec['tx1'] - rec['tx0'] + 1) * px
                        full_h = (rec['ty1'] - rec['ty0'] + 1) * px
                        if mid in CROP:
                            c_tx0, c_ty0, c_count = CROP[mid]
                            full_w = c_count * px
                            full_h = c_count * px
                        fit = '%.1f,%.1f,%.1f,%.1f' % (
                            x0 * full_w / pw, y0 * full_h / ph,
                            (x1 - x0) * full_w / pw, (y1 - y0) * full_h / ph)
            except Exception as e:
                print('fit error', mid, e)

        osd = os.path.join(out, 'openseadragon')
        if not os.path.isdir(osd):
            src = os.path.join(ROOT, 'maps', '0_azeroth', 'out', 'openseadragon')
            if os.path.isdir(src):
                shutil.copytree(src, osd)
        cmd = ['python3', 'Tools/MapBaker/make_interior_viewer.py',
               '--out', out, '--name', slug, '--map-id', str(mid),
               '--tx0', str(tx0), '--ty0', str(ty0), '--count', str(count),
               '--px', str(px), '--title', title,
               '--has-ext', str(has_ext), '--has-int', str(has_int),
               '--has-groups', str(has_groups), '--has-3d', str(has_3d)]
        if fit:
            cmd += ['--fit', fit]
        p = subprocess.run(cmd, capture_output=True, text=True)
        if p.returncode != 0:
            failed.append((mid, p.stderr[-200:]))
            continue
        # The gallery lives in /final/remaster/, one level below the delivery root.
        rel = '../maps/%s' % folder
        # Composite the interior layer over the scene for a useful thumbnail. The raw
        # interior preview is transparent, which looks almost empty for sparse maps.
        try:
            scene_im = Image.open(os.path.join(out, slug + '-scene-preview.jpg')).convert('RGB')
            inter_im = Image.open(os.path.join(out, slug + '-interiors-preview.png')).convert('RGBA')
            if inter_im.size != scene_im.size:
                inter_im = inter_im.resize(scene_im.size)
            scene_im.paste(inter_im, (0, 0), inter_im)
            card_path = os.path.join(out, slug + '-interior-card.jpg')
            scene_im.save(card_path, quality=90)
            img = '%s/out/%s-interior-card.jpg' % (rel, slug)
        except Exception:
            img = '%s/out/%s-scene-preview.jpg' % (rel, slug)
        cards.append((mid, title, rel, img, px, rec['count']))

    cards.sort(key=lambda c: (c[0] not in CONTINENTS, c[0]))

    def card(c):
        mid, title, rel, img, px, count = c
        return ('<a class="card" href="%s/out/viewer.html"><img src="%s" loading="lazy" alt="">'
                '<div class="body"><b>%s</b><span>%d px/ADT · %d tiles</span></div></a>'
                % (rel, img, title, px, count))

    html = '''<!doctype html><html lang="en"><head><meta charset="utf-8">
<title>MapBaker — interior remaster</title>
<style>
 body{margin:0;padding:34px;background:#0b1620;color:#d7e6f5;font:14px/1.5 system-ui,sans-serif}
 h1{color:#9fd0ff;margin:0 0 4px}.sub{color:#7f96ab;margin-bottom:20px}
 .grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(250px,1fr));gap:14px}
 .card{display:block;background:#101f2d;border:1px solid #23384d;border-radius:8px;overflow:hidden;text-decoration:none;color:#d7e6f5}
 .card:hover{border-color:#3d8bd4}.card img{display:block;width:100%;height:160px;object-fit:contain;background:#162838}
 .body{padding:9px 11px}.body b{display:block;color:#9fd0ff;font-size:15px}.body span{color:#7f96ab;font-size:11px}
</style></head><body>
<h1>MapBaker — interior remaster</h1>
<div class="sub">Scene + interior remaster + stock exterior/interior minimap quads.
Instances and battlegrounds also expose a per-group tunnel x-ray list.
Both quad layers are off by default; use the toggles.</div>
<div class="grid">
''' + '\n'.join(card(c) for c in cards) + '''
</div></body></html>'''
    os.makedirs(os.path.join(ROOT, 'remaster'), exist_ok=True)
    with open(os.path.join(ROOT, 'remaster', 'index.html'), 'w') as f:
        f.write(html)
    print('viewers', len(cards), 'failed', failed)
    print('gallery', os.path.join(ROOT, 'remaster', 'index.html'))


if __name__ == '__main__':
    main()
