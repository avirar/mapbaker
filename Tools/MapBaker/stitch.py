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

    xyz = os.path.join(args.out, '%s-xyz' % args.name)
    log('writing XYZ pyramid ...')
    r = subprocess.run([VIPS, 'dzsave', vp(mosaic), vp(xyz),
                        '--layout', 'google', '--tile-size', '512',
                        '--suffix', '.jpg[Q=90]', '--background', '0 0 0'],
                       capture_output=True, text=True, env=vips_env)
    if r.returncode != 0:
        log('dzsave failed: %s' % r.stderr[-500:])
        sys.exit(1)
    log('XYZ pyramid: %s' % xyz)

    preview = os.path.join(args.out, '%s-preview.jpg' % args.name)
    subprocess.run([VIPS, 'thumbnail', vp(mosaic), vp(preview), '2048', '--height', '2048'],
                   capture_output=True, text=True, env=vips_env)
    log('preview: %s' % preview)

    # Self-contained pan/zoom viewer for the XYZ pyramid.
    max_z = 0
    while (max(W, H) >> max_z) > 512:
        max_z += 1
    viewer = os.path.join(args.out, 'viewer.html')
    with open(viewer, 'w', encoding='utf-8') as f:
        f.write(VIEWER_HTML.replace('@NAME@', args.name)
                .replace('@W@', str(W)).replace('@H@', str(H))
                .replace('@MAXZ@', str(max_z))
                .replace('@TX0@', str(args.tx0)).replace('@TY0@', str(args.ty0))
                .replace('@PX@', str(args.px)).replace('@TITLE@', args.name))
    log('viewer: %s' % viewer)
    log('DONE')


VIEWER_HTML = r'''<!doctype html>
<html><head><meta charset="utf-8"><title>@TITLE@</title>
<style>
  html,body{margin:0;height:100%;background:#0a2a44;overflow:hidden}
  canvas{display:block;cursor:grab}
  #hud{position:fixed;left:8px;top:8px;color:#cfe3f5;font:12px/1.4 Consolas,monospace;
       background:#0008;padding:6px 8px;border-radius:4px;pointer-events:none}
  #btns{position:fixed;right:8px;top:8px;display:flex;gap:4px}
  #btns button{background:#123;color:#cfe3f5;border:1px solid #356;border-radius:4px;padding:4px 8px;cursor:pointer}
</style></head><body>
<canvas id="c"></canvas>
<div id="hud"></div>
<div id="btns"><button id="zi">+</button><button id="zo">-</button><button id="zr">reset</button></div>
<script>
const NAME='@NAME@', IW=@W@, IH=@H@, MAXZ=@MAXZ@;
const TX0=@TX0@, TY0=@TY0@, PX=@PX@;
const A=533.33331, O=17066.666;
const cv=document.getElementById('c'), ctx=cv.getContext('2d');
let scale=0.06, panX=0, panY=0;      // screen px per image px, image px at screen origin
const cache=new Map();
function resize(){cv.width=innerWidth;cv.height=innerHeight;}
addEventListener('resize',resize); resize();
function fit(){scale=Math.min(cv.width/IW, cv.height/IH); panX=(cv.width-IW*scale)/2; panY=(cv.height-IH*scale)/2; draw();}
function tile(z,tx,ty){
  const k=z+'/'+tx+'/'+ty; let im=cache.get(k);
  if(!im){ im=new Image(); im.onload=()=>draw(); im.onerror=()=>{}; im.src=NAME+'-xyz/'+z+'/'+tx+'/'+ty+'.jpg'; cache.set(k,im); }
  return im;
}
function draw(){
  ctx.fillStyle='#0a2a44'; ctx.fillRect(0,0,cv.width,cv.height);
  let z=Math.round(Math.log2(scale))+MAXZ; z=Math.max(0,Math.min(MAXZ,z));
  const step=512*Math.pow(2,MAXZ-z);           // image px per tile at this z
  const sx=step*scale;                          // screen px per tile
  const x0=Math.max(0,Math.floor(-panX/sx)), y0=Math.max(0,Math.floor(-panY/sx));
  const x1=Math.min(Math.ceil(IW/step), Math.ceil((cv.width-panX)/sx)+1);
  const y1=Math.min(Math.ceil(IH/step), Math.ceil((cv.height-panY)/sx)+1);
  for(let ty=y0;ty<y1;ty++) for(let tx=x0;tx<x1;tx++){
    const im=tile(z,tx,ty); if(im && im.complete && im.width) ctx.drawImage(im, panX+tx*sx, panY+ty*sx, sx+0.5, sx+0.5);
  }
  const ix=(cv.width/2-panX)/scale, iy=(cv.height/2-panY)/scale;
  const tx=TX0+Math.floor(ix/PX), ty=TY0+Math.floor(iy/PX);
  const wx=O-(ty+(iy%PX)/PX)*A, wy=O-(tx+(ix%PX)/PX)*A;
  document.getElementById('hud').textContent =
    'map 0  ADT ('+tx+','+ty+')  world '+wx.toFixed(0)+','+wy.toFixed(0)+'  z'+z+'  '+Math.round(scale*100)+'%';
}
let drag=null;
cv.addEventListener('mousedown',e=>{drag={x:e.clientX,y:e.clientY,px:panX,py:panY};cv.style.cursor='grabbing';});
addEventListener('mouseup',()=>{drag=null;cv.style.cursor='grab';});
addEventListener('mousemove',e=>{ if(!drag) return; panX=drag.px+(e.clientX-drag.x); panY=drag.py+(e.clientY-drag.y); draw(); });
cv.addEventListener('wheel',e=>{ e.preventDefault();
  const f=e.deltaY<0?1.25:0.8; const mx=e.clientX, my=e.clientY;
  const ix=(mx-panX)/scale, iy=(my-panY)/scale;
  scale=Math.max(0.005,Math.min(4,scale*f));
  panX=mx-ix*scale; panY=my-iy*scale; draw();
},{passive:false});
document.getElementById('zi').onclick=()=>{scale*=1.4;draw();};
document.getElementById('zo').onclick=()=>{scale/=1.4;draw();};
document.getElementById('zr').onclick=fit;
fit();
</script></body></html>
'''


if __name__ == '__main__':
    main()
