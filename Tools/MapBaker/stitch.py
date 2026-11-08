#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""M4 stitch: assemble MapBaker ADT tiles into a pyramidal BigTIFF + XYZ pyramid.

Input : a directory of tile_<tx>_<ty>.png (px x px each), a tile rectangle
Output: <out>/<name>.tif          tiled, pyramidal, BigTIFF master
        <out>/<name>-xyz/{z}/{x}/{y}.jpg   XYZ (Google) pyramid, 512 px tiles
        <out>/<name>-preview.jpg   small preview
        <out>/viewer.html          self-contained pan/zoom viewer

Missing tiles (or tiles that are a single flat colour -- the baker writes those for
ADT slots with no terrain) are replaced with the background colour.

Usage:
  python stitch.py --tiles Z:/.../ek1024 --out Z:/.../mapbaker --name ek1024 \
      --tx0 24 --tx1 44 --ty0 20 --ty1 61 --px 1024
"""
import argparse
import concurrent.futures
import os
import subprocess
import sys
import time
import zlib

VIPS = r'C:\GIT\wowdecomp-deps\vips\vips-dev-8.18\bin\vips.exe'


def log(msg):
    print('[%s] %s' % (time.strftime('%H:%M:%S'), msg), flush=True)


def vp(path):
    """vips CLI path form. Its array-list parser eats backslashes, so always use '/'."""
    return os.path.abspath(path).replace('\\', '/')


def write_solid_png(path, size, rgb):
    """Minimal RGB PNG writer for the background tile (no PIL)."""
    raw = bytearray()
    row = bytes(rgb) * size
    for _ in range(size):
        raw.append(0)
        raw += row

    def chunk(tag, data):
        c = bytearray()
        c += len(data).to_bytes(4, 'big')
        c += tag
        c += data
        crc = zlib.crc32(tag + data) & 0xFFFFFFFF
        c += crc.to_bytes(4, 'big')
        return bytes(c)

    png = b'\x89PNG\r\n\x1a\n'
    ihdr = size.to_bytes(4, 'big') + size.to_bytes(4, 'big') + bytes([8, 2, 0, 0, 0])
    png += chunk(b'IHDR', ihdr)
    png += chunk(b'IDAT', zlib.compress(bytes(raw), 6))
    png += chunk(b'IEND', b'')
    with open(path, 'wb') as f:
        f.write(png)


def tile_is_flat(path):
    """True if the tile is essentially one colour ('vips deviate' = overall std dev)."""
    try:
        out = subprocess.run([VIPS, 'deviate', vp(path)], capture_output=True, text=True, timeout=60)
        if out.returncode != 0:
            return False
        return float(out.stdout.strip()) < 1.5
    except Exception:
        return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--tiles', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--name', default='map')
    ap.add_argument('--tx0', type=int, required=True)
    ap.add_argument('--tx1', type=int, required=True)
    ap.add_argument('--ty0', type=int, required=True)
    ap.add_argument('--ty1', type=int, required=True)
    ap.add_argument('--px', type=int, default=1024)
    ap.add_argument('--background', default='0a2a44')
    ap.add_argument('--work', default=None,
                    help='directory for the big .v intermediates (default: --out; put this on a '
                         'local SSD when --out is a network share)')
    ap.add_argument('--row-jobs', type=int, default=max(1, (os.cpu_count() or 4)),
                    help='parallel vips arrayjoin processes for the row strips')
    ap.add_argument('--concurrency', type=int, default=max(1, os.cpu_count() or 4),
                    help='VIPS_CONCURRENCY for the big single ops (tiffsave/dzsave)')
    args = ap.parse_args()
    vips_env = dict(os.environ)
    vips_env['VIPS_CONCURRENCY'] = str(args.concurrency)

    os.makedirs(args.out, exist_ok=True)
    work = args.work if args.work else args.out
    os.makedirs(work, exist_ok=True)
    bg_rgb = tuple(int(args.background[i:i + 2], 16) for i in (0, 2, 4))
    empty = os.path.join(work, '_empty.png')
    write_solid_png(empty, args.px, bg_rgb)
    log('background tile %s (%dx%d #%s)' % (empty, args.px, args.px, args.background))

    n_tx = args.tx1 - args.tx0 + 1
    n_ty = args.ty1 - args.ty0 + 1
    W, H = n_tx * args.px, n_ty * args.px
    log('grid %dx%d tiles = %dx%d px (%.2f Gpx)' % (n_tx, n_ty, W, H, W * H / 1e9))

    # Scan the grid first (missing/flat detection), then build the row strips in parallel:
    # each row is an independent vips arrayjoin process (PNG decode + .v write).
    rows = []
    used, missing, flat = 0, 0, 0
    row_jobs = []
    for ty in range(args.ty0, args.ty1 + 1):
        row_files = []
        for tx in range(args.tx0, args.tx1 + 1):
            p = os.path.join(args.tiles, 'tile_%d_%d.png' % (tx, ty))
            if not os.path.exists(p):
                missing += 1
                row_files.append(empty)
                continue
            if os.path.getsize(p) < 120 * 1024 and tile_is_flat(p):
                flat += 1
                row_files.append(empty)
                continue
            used += 1
            row_files.append(p)
        row_v = os.path.join(work, 'row_%03d.v' % ty)
        rows.append(row_v)
        row_jobs.append((ty, row_files, row_v))
    log('tiles: %d used, %d missing, %d flat -> background; building %d row strips '
        'with %d parallel jobs' % (used, missing, flat, len(row_jobs), args.row_jobs))

    def build_row(job):
        ty, row_files, row_v = job
        # vips arrayjoin takes the input list as ONE space-separated argument.
        r = subprocess.run([VIPS, 'arrayjoin', ' '.join(vp(f) for f in row_files), vp(row_v),
                            '--across', str(n_tx)], capture_output=True, text=True, env=vips_env)
        return ty, r.returncode, r.stderr

    done_rows = 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.row_jobs) as pool:
        futures = [pool.submit(build_row, job) for job in row_jobs]
        for fut in concurrent.futures.as_completed(futures):
            ty, rc, err = fut.result()
            if rc != 0:
                log('arrayjoin failed for row %d: %s' % (ty, err[-500:]))
                sys.exit(1)
            done_rows += 1
            log('row %d/%d done' % (done_rows, len(row_jobs)))

    mosaic = os.path.join(work, '%s-mosaic.v' % args.name)
    log('assembling mosaic (%.2f GB) ...' % (W * H * 3 / 1e9))
    r = subprocess.run([VIPS, 'arrayjoin', ' '.join(vp(r) for r in rows), vp(mosaic),
                        '--across', '1'], capture_output=True, text=True, env=vips_env)
    if r.returncode != 0:
        log('mosaic arrayjoin failed: %s' % r.stderr[-500:])
        sys.exit(1)
    log('mosaic assembled: %s' % mosaic)

    tif = os.path.join(args.out, '%s.tif' % args.name)
    log('writing BigTIFF pyramid (this is the long step) ...')
    r = subprocess.run([VIPS, 'tiffsave', vp(mosaic), vp(tif),
                        '--tile', '--tile-width', '512', '--tile-height', '512',
                        '--pyramid', '--bigtiff', '--compression', 'deflate',
                        '--predictor', 'horizontal'], capture_output=True, text=True,
                       env=vips_env)
    if r.returncode != 0:
        log('tiffsave failed: %s' % r.stderr[-500:])
        sys.exit(1)
    log('BigTIFF: %s (%.1f MB)' % (tif, os.path.getsize(tif) / 1e6))

    # DZI (Deep Zoom Image) is the reliable pyramid for OpenSeadragon. NOTE: vips
    # dzsave --layout google writes {z}/{row}/{col}.jpg (x/y swapped vs the XYZ spec),
    # which scrambles every standard XYZ viewer at zoom; DZI is unambiguous.
    dzi = os.path.join(args.out, args.name)
    log('writing DZI pyramid ...')
    r = subprocess.run([VIPS, 'dzsave', vp(mosaic), vp(dzi),
                        '--layout', 'dz', '--tile-size', '512', '--overlap', '1',
                        '--suffix', '.jpg[Q=90]'],
                       capture_output=True, text=True, env=vips_env)
    if r.returncode != 0:
        log('dzsave failed: %s' % r.stderr[-500:])
        sys.exit(1)
    log('DZI pyramid: %s.dzi + %s_files/' % (dzi, dzi))

    preview = os.path.join(args.out, '%s-preview.jpg' % args.name)
    subprocess.run([VIPS, 'thumbnail', vp(mosaic), vp(preview), '2048', '--height', '2048'],
                   capture_output=True, text=True, env=vips_env)
    log('preview: %s' % preview)

    viewer = os.path.join(args.out, 'viewer.html')
    with open(viewer, 'w', encoding='utf-8') as f:
        f.write(VIEWER_HTML.replace('@NAME@', args.name)
                .replace('@TX0@', str(args.tx0)).replace('@TY0@', str(args.ty0))
                .replace('@PX@', str(args.px)).replace('@TITLE@', args.name))
    log('viewer: %s' % viewer)
    log('DONE')


VIEWER_HTML = r'''<!doctype html>
<html>
<head>
<meta charset="utf-8">
<title>@TITLE@</title>
<style>
  html,body{margin:0;height:100%;background:#0a2a44;overflow:hidden}
  #osd{width:100vw;height:100vh}
  #hud{position:fixed;left:10px;top:10px;z-index:10;color:#d7e6f5;
       font:12px/1.5 Consolas,monospace;background:rgba(0,0,0,.55);
       padding:8px 12px;border-radius:5px;pointer-events:none;white-space:pre}
  #hud b{color:#9fd0ff;font-weight:600}
</style>
</head>
<body>
<div id="osd"></div>
<div id="hud">loading…</div>
<script>
const TX0=@TX0@, TY0=@TY0@, PX=@PX@;
const A=533.33331, O=17066.666;
function initOSD(){
  const viewer=OpenSeadragon({
    id:'osd',
    prefixUrl:'openseadragon/images/',
    tileSources:'@NAME@.dzi',
    showNavigator:true,navigatorPosition:'BOTTOM_RIGHT',navigatorHeight:220,navigatorWidth:140,
    gestureSettingsMouse:{clickToZoom:false,dblClickToZoom:true},
    maxZoomPixelRatio:3,visibilityRatio:1,constrainDuringPan:true,
    animationTime:0.35,blendTime:0.1,imageLoaderLimit:12,timeout:60000
  });
  const hud=document.getElementById('hud');
  function update(){
    const img=viewer.world.getItemAt(0); if(!img) return;
    const c=viewer.viewport.getCenter(true);
    const p=img.viewportToImageCoordinates(c);
    const ix=p.x, iy=p.y;
    const tx=TX0+Math.floor(ix/PX), ty=TY0+Math.floor(iy/PX);
    const wy=O-(TX0+ix/PX)*A, wx=O-(TY0+iy/PX)*A;
    hud.innerHTML='<b>map 0</b>  ADT <b>('+tx+', '+ty+')</b>\n'+
      'world <b>'+wx.toFixed(1)+', '+wy.toFixed(1)+'</b>\n'+
      'image '+ix.toFixed(0)+', '+iy.toFixed(0)+' px   zoom '+
      (viewer.viewport.getZoom(true)*100).toFixed(0)+'%';
  }
  viewer.addHandler('open',update);
  ['animation','animation-finish','resize','zoom','pan','canvas-scroll'].forEach(function(e){viewer.addHandler(e,update);});
  window.addEventListener('keydown',function(e){ if(e.key==='f'||e.key==='F') viewer.viewport.goHome(); });
}
(function(){
  var s=document.createElement('script');
  s.src='openseadragon/openseadragon.min.js';
  s.onload=initOSD;
  s.onerror=function(){
    var c=document.createElement('script');
    c.src='https://cdn.jsdelivr.net/npm/openseadragon@6.1.1/build/openseadragon/openseadragon.min.js';
    c.onload=initOSD; document.head.appendChild(c);
  };
  document.head.appendChild(s);
})();
</script>
</body></html>
'''


if __name__ == '__main__':
    main()
