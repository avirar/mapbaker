#!/usr/bin/env python3
"""Re-bake the open-world interior remaster (maps 0/1/530/571).

The new filter renders only WMO groups with stock interior minimap art cells and
skips outdoor M2 props/doodads, so the layer is the 3D counterpart of the
stock interior quads. Instances keep their existing approved interiors.
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
GAIN = '1.3'


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
    ap.add_argument('--only', default='0,1,530,571')
    ap.add_argument('--force', action='store_true')
    args = ap.parse_args()

    want = {int(x) for x in args.only.split(',') if x.strip()}
    data = json.load(open(args.manifest))
    records = [r for r in data['maps'] if 'error' not in r and r['id'] in want]
    backup_root = os.path.join(args.root, 'backup-interiors')
    os.makedirs(backup_root, exist_ok=True)

    def bake(rec):
        mid, dirn = rec['id'], rec['dir']
        slug = slugify(dirn)
        folder = '%d_%s' % (mid, slug)
        root = os.path.join(args.root, 'maps', folder)
        out = os.path.join(root, 'out')
        name = slug + '-interiors'
        dzi = os.path.join(out, name + '.dzi')
        if os.path.exists(dzi) and not args.force:
            return rec, 0, 'exists'
        px = 1024
        tiles = rec['tiles']
        tiles_dir = os.path.join(root, 'tiles', 'interiors')
        if os.path.exists(tiles_dir):
            shutil.rmtree(tiles_dir)
        os.makedirs(tiles_dir)
        list_path = os.path.join(root, '_interiors_tiles.txt')
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
               '--px', str(px), '--ssaa', '2', '--preset', 'map', '--light-elev', '60',
               '--gain', GAIN, '--layer', 'interiors', '--out', to_win_path(tiles_dir)]
        logf = os.path.join(root, 'logs', 'interiors.log')
        os.makedirs(os.path.dirname(logf), exist_ok=True)
        with open(logf, 'w') as lf:
            r = subprocess.run(cmd, stdout=lf, stderr=subprocess.STDOUT, env=env)
        if r.returncode != 0:
            return rec, r.returncode, 'bake failed'
        # Preserve the old DZI once, then replace it.
        backup = os.path.join(backup_root, folder)
        os.makedirs(backup, exist_ok=True)
        for suffix in ('.dzi', '.tif', '-preview.png'):
            src = os.path.join(out, name + suffix)
            if os.path.exists(src):
                dst = os.path.join(backup, name + suffix)
                if not os.path.exists(dst):
                    shutil.copy2(src, dst)
                os.remove(src)
        files = os.path.join(out, name + '_files')
        if os.path.isdir(files):
            if not os.path.isdir(os.path.join(backup, name + '_files')):
                shutil.copytree(files, os.path.join(backup, name + '_files'))
            shutil.rmtree(files)
        work = os.path.join(args.root, 'work', folder + '-interiors')
        cmd = [sys.executable, 'Tools/MapBaker/stitch.py',
               '--tiles', tiles_dir, '--out', out, '--name', name,
               '--map-id', str(mid), '--tx0', str(rec['tx0']), '--tx1', str(rec['tx1']),
               '--ty0', str(rec['ty0']), '--ty1', str(rec['ty1']), '--px', str(px),
               '--background', '1d3b5d', '--work', work,
               '--row-jobs', '6', '--row-concurrency', '1',
               '--tiff-compression', 'zstd', '--tiff-level', '1', '--alpha']
        with open(os.path.join(root, 'logs', 'interiors-stitch.log'), 'w') as lf:
            r = subprocess.run(cmd, stdout=lf, stderr=subprocess.STDOUT)
        if r.returncode == 0:
            shutil.rmtree(work, ignore_errors=True)
        return rec, r.returncode, ''

    log('interiors open-world: %s' % sorted(want))
    results = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.workers) as pool:
        futs = {pool.submit(bake, rec): rec for rec in records}
        for fut in concurrent.futures.as_completed(futs):
            rec = futs[fut]
            try:
                _, rc, msg = fut.result()
            except Exception as e:
                rc, msg = 1, str(e)
            results.append((rec, rc))
            log('interiors %s rc=%d %s' % (rec['dir'], rc, msg))
    log('done; failed: %s' % ([r['id'] for r, rc in results if rc != 0] or 'none'))


if __name__ == '__main__':
    main()
