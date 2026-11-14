#!/usr/bin/env python3
"""Final two-layer all-map bake.

For every map in the inventory manifest:
  scene  - opaque terrain + all WMO + all M2/trees + water, no quads
  quads  - transparent 3.3.5a minimap art

Continents (0/1/530/571) bake at 1024 px/ADT; all other maps at 4096.
Output layout:
  <root>/maps/<id>_<slug>/tiles/{scene,quads}
  <root>/maps/<id>_<slug>/out/<slug>-{scene,quads}.dzi
  <root>/maps/<id>_<slug>/out/viewer.html
  <root>/index.html
"""
import argparse
import concurrent.futures
import json
import os
import re
import shutil
import subprocess
import sys
import threading
import time

CONTINENTS = {0: 'Eastern Kingdoms', 1: 'Kalimdor', 530: 'Outland', 571: 'Northrend'}
CONTINENT_PX = 1024
INSTANCE_PX = 4096


def to_win_path(path):
    if not path or os.name == 'nt' or (len(path) >= 2 and path[1] == ':'):
        return path
    return 'Z:' + os.path.abspath(path).replace('/', '\\')


def slugify(name):
    return re.sub(r'[^a-z0-9]+', '', name.lower()) or 'map'


def pretty(name):
    s = re.sub(r'([a-z])([A-Z])', r'\1 \2', name)
    return s


def log(msg):
    print('[%s] %s' % (time.strftime('%H:%M:%S'), msg), flush=True)


class Baker:
    def __init__(self, args, rec):
        self.args = args
        self.rec = rec
        self.mid = rec['id']
        self.slug = slugify(rec['dir'])
        self.folder = '%d_%s' % (self.mid, self.slug)
        self.root = os.path.join(args.root, 'maps', self.folder)
        self.tiles_dir = os.path.join(self.root, 'tiles')
        self.out = os.path.join(self.root, 'out')
        self.logs = os.path.join(self.root, 'logs')
        self.px = CONTINENT_PX if self.mid in CONTINENTS else INSTANCE_PX
        os.makedirs(self.logs, exist_ok=True)
        os.makedirs(self.out, exist_ok=True)

    def title(self):
        return CONTINENTS.get(self.mid) or pretty(self.rec['dir'])

    def run_layer(self, layer):
        tiles = self.rec.get('tiles') or []
        if not tiles:
            return layer, 1, 'no tiles'
        tdir = os.path.join(self.tiles_dir, layer)
        os.makedirs(tdir, exist_ok=True)
        list_path = os.path.join(self.root, '_%s_tiles.txt' % layer)
        order = list(tiles)
        if layer == 'scene' and len(order) > 1:
            # First tile of a cold process can miss not-yet-streamed WMOs. Render it
            # twice; --skip-existing makes this a no-op on resumed runs.
            order = [order[0]] + order
        with open(list_path, 'w') as f:
            for tx, ty in order:
                f.write('%d %d\n' % (tx, ty))

        env = dict(os.environ)
        env.setdefault('WINEPREFIX', '/media/storage/wineprefixes/mapbaker')
        env.setdefault('DISPLAY', ':99')
        env.setdefault('WINEDLLOVERRIDES', 'd3d9,dxgi=n')
        env.setdefault('WINEDEBUG', '-all')
        env.setdefault('DXVK_LOG_LEVEL', 'error')
        env.setdefault('DXVK_STATE_CACHE', '0')

        cmd = [self.args.wine, to_win_path(self.args.exe),
               '--client', to_win_path(self.args.client),
               '--map', str(self.mid),
               '--tile-list', to_win_path(list_path),
               '--skip-existing',
               '--px', str(self.px),
               '--ssaa', '2' if layer == 'scene' or self.px <= 1024 else '1',
               '--preset', 'map', '--light-elev', '60',
               '--layer', layer,
               '--out', to_win_path(tdir)]
        if self.mid not in CONTINENTS:
            # ADT-based instances still have WMO wings outside the normal AOI.
            cmd.append('--force-wmo')
        if layer == 'quads':
            cmd += ['--quad-cache',
                    to_win_path(os.path.join(self.root, '%s.quadcache' % self.slug))]
        logf = os.path.join(self.logs, '%s.log' % layer)
        t0 = time.time()
        with open(logf, 'w', errors='replace') as lf:
            r = subprocess.run(cmd, stdout=lf, stderr=subprocess.STDOUT, env=env)
        dt = time.time() - t0
        return layer, r.returncode, '%.0fs' % dt

    def stitch(self):
        tiles_root = self.tiles_dir
        out = self.out
        for layer in ('scene', 'quads'):
            dzi = os.path.join(out, '%s-%s.dzi' % (self.slug, layer))
            if os.path.exists(dzi) and not self.args.force_stitch:
                continue
            alpha = ['--alpha'] if layer == 'quads' else []
            name = '%s-%s' % (self.slug, layer)
            work = os.path.join(self.args.root, 'work', self.folder + '-' + layer)
            big = self.rec['count'] > 120
            rj, rc = (6, 2) if big else (2, 1)
            cmd = [sys.executable, self.args.stitch_py,
                   '--tiles', os.path.join(tiles_root, layer),
                   '--out', out, '--name', name,
                   '--map-id', str(self.mid),
                   '--tx0', str(self.rec['tx0']), '--tx1', str(self.rec['tx1']),
                   '--ty0', str(self.rec['ty0']), '--ty1', str(self.rec['ty1']),
                   '--px', str(self.px),
                   '--background', '1d3b5d',
                   '--work', work,
                   '--row-jobs', str(rj), '--row-concurrency', str(rc),
                   '--tiff-compression', 'zstd', '--tiff-level', '1'] + alpha
            logf = os.path.join(self.logs, 'stitch-%s.log' % layer)
            t0 = time.time()
            with open(logf, 'w', errors='replace') as lf:
                r = subprocess.run(cmd, stdout=lf, stderr=subprocess.STDOUT)
            if r.returncode == 0:
                shutil.rmtree(work, ignore_errors=True)
            else:
                log('  %s stitch FAILED (%s), log %s' % (self.folder, layer, logf))
                return False
        # Shared OpenSeadragon tree and the final viewer.
        os.makedirs(os.path.join(out, 'openseadragon'), exist_ok=True)
        shutil.copytree(self.args.osd, os.path.join(out, 'openseadragon'),
                        dirs_exist_ok=True)
        cmd = [sys.executable, self.args.viewer_py,
               '--out', out, '--name', self.slug, '--map-id', str(self.mid),
               '--tx0', str(self.rec['tx0']), '--ty0', str(self.rec['ty0']),
               '--count', str(self.rec['count']), '--px', str(self.px),
               '--title', '%s — map %d' % (self.title(), self.mid)]
        subprocess.run(cmd, capture_output=True, text=True)
        return True


def make_index(args, records):
    cards = []
    for rec in records:
        m = rec['_meta']
        if not m.get('stitched'):
            # Index-only reruns do not go through the bake/stitch path.
            out = os.path.join(args.root, 'maps', m['folder'], 'out')
            if (os.path.exists(os.path.join(out, '%s-scene.dzi' % m['slug'])) and
                    os.path.exists(os.path.join(out, '%s-quads.dzi' % m['slug']))):
                m['stitched'] = True
        if not m.get('stitched'):
            continue
        rel = 'maps/%s' % m['folder']
        title = CONTINENTS.get(rec['id']) or pretty(rec['dir'])
        px = CONTINENT_PX if rec['id'] in CONTINENTS else INSTANCE_PX
        img = '%s/out/%s-scene-preview.jpg' % (rel, m['slug'])
        cards.append((rec['id'], title, rel, img, px, rec['count']))
    cards.sort(key=lambda c: (c[0] not in CONTINENTS, c[0]))
    def card(c):
        mid, title, rel, img, px, count = c
        return ('<a class="card" href="%s/out/viewer.html"><img src="%s" loading="lazy" alt="">'
                '<div class="body"><b>%s</b><span>map %d · %d px/ADT · %d tiles</span></div></a>'
                % (rel, img, title, mid, px, count))

    default_href = ''
    for c in cards:
        if c[0] == 550:
            default_href = '%s/out/viewer.html' % c[2]
            break
    if not default_href and cards:
        default_href = '%s/out/viewer.html' % cards[0][2]
    options = []
    for c in cards:
        href = '%s/out/viewer.html' % c[2]
        selected = ' selected' if href == default_href else ''
        options.append('<option value="%s"%s>%s</option>' % (href, selected, c[1]))

    html = '''<!doctype html><html lang="en"><head><meta charset="utf-8">
<title>MapBaker — final maps</title>
<style>
 body{margin:0;padding:36px;background:#0b1620;color:#d7e6f5;font:14px/1.5 system-ui,sans-serif}
 h1{color:#9fd0ff;margin:0 0 4px} .sub{color:#7f96ab;margin-bottom:24px}
 h2{color:#9fd0ff;font-size:17px;margin:28px 0 12px}
 .grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(250px,1fr));gap:14px}
 .card{display:block;background:#101f2d;border:1px solid #23384d;border-radius:8px;
       overflow:hidden;text-decoration:none;color:#d7e6f5}
 .card:hover{border-color:#3d8bd4}
 .card img{display:block;width:100%;height:150px;object-fit:contain;background:#162838}
 .body{padding:9px 11px}.body b{display:block;color:#9fd0ff;font-size:15px}
 .body span{color:#7f96ab;font-size:11px}
 .hero{margin:0 0 26px;border:1px solid #23384d;border-radius:9px;overflow:hidden;background:#101f2d}
 .herobar{display:flex;gap:10px;align-items:center;padding:10px 12px;border-bottom:1px solid #23384d}
 .herobar select{flex:1;background:#0f1d2a;color:#d7e6f5;border:1px solid #2a4157;border-radius:5px;padding:6px}
 .herobar a{color:#9fd0ff;white-space:nowrap;font-size:12px}
 #frame{display:block;width:100%;height:68vh;border:0;background:#0b1620}
</style></head><body>
<h1>MapBaker — final maps</h1>
<div class="sub">Scene + interior remaster + stock exterior/interior minimap quads · continents 1024 px/ADT · instances 4096 px/ADT<br>
<a href="remaster/index.html" style="color:#9fd0ff">Interior remaster gallery →</a></div>
<div class="hero">
 <div class="herobar">
  <select id="mapsel">@OPTIONS@</select>
  <a id="open" href="@DEFAULT@" target="_blank">Open full viewer ↗</a>
 </div>
 <iframe id="frame" src="@DEFAULT@" loading="eager"></iframe>
</div>
<div class="grid">
@CARDS@
</div>
<script>
const sel=document.getElementById('mapsel'), frame=document.getElementById('frame'), open=document.getElementById('open');
function setViewer(){ frame.src=sel.value; open.href=sel.value; }
sel.addEventListener('change', setViewer);
</script>
</body></html>'''.replace('@CARDS@', '\n'.join(card(c) for c in cards)).replace('@OPTIONS@', '\n'.join(options)).replace('@DEFAULT@', default_href)
    path = os.path.join(args.root, 'index.html')
    with open(path, 'w', encoding='utf-8') as f:
        f.write(html)
    log('index: %s (%d maps)' % (path, len([r for r in records if r['_meta'].get('stitched')])))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--manifest', required=True)
    ap.add_argument('--root', required=True)
    ap.add_argument('--client', required=True)
    ap.add_argument('--exe', required=True)
    ap.add_argument('--wine', default='wine')
    ap.add_argument('--workers', type=int, default=6)
    ap.add_argument('--stitch-workers', type=int, default=1)
    ap.add_argument('--only', default=None, help='comma-separated map ids')
    ap.add_argument('--skip-bake', action='store_true')
    ap.add_argument('--skip-stitch', action='store_true')
    ap.add_argument('--force-stitch', action='store_true')
    ap.add_argument('--osd', default='/media/storage/mapbaker/northrend4096-out/openseadragon')
    ap.add_argument('--stitch-py', default='Tools/MapBaker/stitch.py')
    ap.add_argument('--viewer-py', default='Tools/MapBaker/make_two_layer_viewer.py')
    args = ap.parse_args()
    os.makedirs(args.root, exist_ok=True)

    data = json.load(open(args.manifest))
    records = [r for r in data['maps'] if 'error' not in r]
    if args.only:
        want = {int(x) for x in args.only.split(',') if x.strip()}
        records = [r for r in records if r['id'] in want]
    for r in records:
        r['_meta'] = {'folder': '%d_%s' % (r['id'], slugify(r['dir'])),
                      'slug': slugify(r['dir']), 'stitched': False}
    log('final bake: %d maps, %d workers, stitch workers %d' %
        (len(records), args.workers, args.stitch_workers))
    total_tiles = sum(r['count'] for r in records)
    log('tiles: %d (scene+quads)' % total_tiles)

    bakers = {r['id']: Baker(args, r) for r in records}
    pending = {r['id']: 2 for r in records}
    failed = set()
    lock = threading.Lock()

    def do_bake(rec):
        b = bakers[rec['id']]
        if args.skip_bake:
            return [(l, 0, 'skipped') for l in ('scene', 'quads')]
        out = []
        for layer in ('scene', 'quads'):
            log('bake %s %s' % (b.folder, layer))
            out.append(b.run_layer(layer))
            log('  %s %s rc=%d %s' % (b.folder, layer, out[-1][1], out[-1][2]))
        return out

    def do_stitch(rec):
        b = bakers[rec['id']]
        ok = b.stitch()
        with lock:
            rec['_meta']['stitched'] = bool(ok)
            if not ok:
                failed.add(rec['id'])
        log('stitch %s %s' % (b.folder, 'ok' if ok else 'FAILED'))

    with concurrent.futures.ThreadPoolExecutor(max_workers=args.workers) as bake_pool, \
         concurrent.futures.ThreadPoolExecutor(max_workers=args.stitch_workers) as stitch_pool:
        futs = {bake_pool.submit(do_bake, rec): rec for rec in records}
        for fut in concurrent.futures.as_completed(futs):
            rec = futs[fut]
            try:
                results = fut.result()
                if any(rc != 0 for _, rc, _ in results):
                    failed.add(rec['id'])
            except Exception as e:
                log('bake exception for %d: %s' % (rec['id'], e))
                failed.add(rec['id'])
            with lock:
                pending[rec['id']] -= 2
                ready = pending[rec['id']] == 0
            if ready and not args.skip_stitch:
                stitch_pool.submit(do_stitch, rec)

    if not args.skip_stitch:
        # Wait for any remaining stitches.
        stitch_pool.shutdown(wait=True)
    make_index(args, records)
    log('done; failed maps: %s' % (sorted(failed) if failed else 'none'))


if __name__ == '__main__':
    main()
