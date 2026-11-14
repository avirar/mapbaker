#!/usr/bin/env python3
"""Bake the stock outdoor minimap tiles (quads-ext) for every manifest map.

The client's outdoor minimap is one Textures\\Minimap\\<dir>\\map<tx>_<ty>.blp image
per ADT. MapBaker layer 7 places those images 1:1 on the ADT grid. Missing tiles
are written transparent, so every map gets a complete DZI.
"""
import argparse
import concurrent.futures
import json
import os
import re
import shutil
import subprocess
import sys
import time

CONTINENTS = {0, 1, 530, 571}
CROP = {329: (37, 39, 24, 26), 617: (30, 30, 29, 29)}


def slugify(name):
    return re.sub(r'[^a-z0-9]+', '', name.lower()) or 'map'


def log(msg):
    print('[%s] %s' % (time.strftime('%H:%M:%S'), msg), flush=True)


def to_win_path(path):
    return 'Z:' + os.path.abspath(path).replace('/', '\\')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--manifest', required=True)
    ap.add_argument('--root', required=True)
    ap.add_argument('--client', required=True)
    ap.add_argument('--exe', required=True)
    ap.add_argument('--wine', default='wine')
    ap.add_argument('--workers', type=int, default=4)
    ap.add_argument('--only', default=None, help='comma-separated map ids')
    ap.add_argument('--force', action='store_true')
    args = ap.parse_args()

    data = json.load(open(args.manifest))
    records = [r for r in data['maps'] if 'error' not in r]
    if args.only:
        want = {int(x) for x in args.only.split(',') if x.strip()}
        records = [r for r in records if r['id'] in want]

    def bake(rec):
        mid, dirn = rec['id'], rec['dir']
        slug = slugify(dirn)
        folder = '%d_%s' % (mid, slug)
        root = os.path.join(args.root, 'maps', folder)
        out = os.path.join(root, 'out')
        name = slug + '-quads-ext'
        dzi = os.path.join(out, name + '.dzi')
        if os.path.exists(dzi) and not args.force:
            return rec, 0, 'exists'
        os.makedirs(out, exist_ok=True)
        px = 1024 if mid in CONTINENTS else 4096
        crop = CROP.get(mid)
        tiles = rec['tiles']
        if crop:
            tx0, tx1, ty0, ty1 = crop
            tiles = [[tx, ty] for tx, ty in tiles if tx0 <= tx <= tx1 and ty0 <= ty <= ty1]
        tiles_dir = os.path.join(root, 'tiles', 'quads-ext')
        if os.path.exists(tiles_dir):
            shutil.rmtree(tiles_dir)
        os.makedirs(tiles_dir)
        list_path = os.path.join(root, '_quads_ext_tiles.txt')
        with open(list_path, 'w') as f:
            for tx, ty in tiles:
                f.write('%d %d\n' % (tx, ty))
        env = dict(os.environ)
        env.setdefault('WINEPREFIX', '/media/storage/wineprefixes/mapbaker')
        env.setdefault('DISPLAY', ':99')
        env.setdefault('WINEDLLOVERRIDES', 'd3d9,dxgi=n')
        env.setdefault('WINEDEBUG', '-all')
        env.setdefault('DXVK_LOG_LEVEL', 'error')
        env.setdefault('DXVK_STATE_CACHE', '0')
        cmd = [args.wine, to_win_path(args.exe), '--client', to_win_path(args.client),
               '--map', str(mid), '--tile-list', to_win_path(list_path),
               '--px', str(px), '--ssaa', '1', '--preset', 'map', '--light-elev', '60',
               '--layer', 'quads-ext', '--out', to_win_path(tiles_dir)]
        logf = os.path.join(root, 'logs', 'quads-ext.log')
        os.makedirs(os.path.dirname(logf), exist_ok=True)
        with open(logf, 'w') as lf:
            r = subprocess.run(cmd, stdout=lf, stderr=subprocess.STDOUT, env=env)
        if r.returncode != 0:
            return rec, r.returncode, 'bake failed'
        if crop:
            tx0, tx1, ty0, ty1 = crop
        else:
            tx0, tx1, ty0, ty1 = rec['tx0'], rec['tx1'], rec['ty0'], rec['ty1']
        for p in (dzi, os.path.join(out, name + '.tif'),
                  os.path.join(out, name + '-preview.png')):
            if os.path.exists(p):
                os.remove(p)
        files = os.path.join(out, name + '_files')
        if os.path.isdir(files):
            shutil.rmtree(files)
        work = os.path.join(args.root, 'work', folder + '-quads-ext')
        cmd = [sys.executable, 'Tools/MapBaker/stitch.py',
               '--tiles', tiles_dir, '--out', out, '--name', name,
               '--map-id', str(mid), '--tx0', str(tx0), '--tx1', str(tx1),
               '--ty0', str(ty0), '--ty1', str(ty1), '--px', str(px),
               '--background', '1d3b5d', '--work', work,
               '--row-jobs', '4', '--row-concurrency', '1',
               '--tiff-compression', 'zstd', '--tiff-level', '1', '--alpha']
        with open(os.path.join(root, 'logs', 'quads-ext-stitch.log'), 'w') as lf:
            r = subprocess.run(cmd, stdout=lf, stderr=subprocess.STDOUT)
        if r.returncode == 0:
            shutil.rmtree(work, ignore_errors=True)
        return rec, r.returncode, ''

    log('quads-ext: %d maps' % len(records))
    failed = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.workers) as pool:
        futs = {pool.submit(bake, rec): rec for rec in records}
        for fut in concurrent.futures.as_completed(futs):
            rec = futs[fut]
            try:
                _, rc, msg = fut.result()
            except Exception as e:
                rc, msg = 1, str(e)
            if rc != 0:
                failed.append(rec['id'])
            log('quads-ext %s rc=%d %s' % (rec['dir'], rc, msg))
    log('done; failed: %s' % (sorted(failed) if failed else 'none'))


if __name__ == '__main__':
    main()
