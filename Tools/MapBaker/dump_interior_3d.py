#!/usr/bin/env python3
"""Dump per-cell 3D section sprites for the interior tunnel remaster.

For every stock interior-art cell in the selected map(s), MapBaker renders the owning
WMO group top-down with a section cut at (group world Z minimum + 2), then writes:

  interior-3d/<id>_<x>_<y>_full.png     actual 3D floor/walls
  interior-3d/<id>_<x>_<y>_masked.png   same render clipped to the stock cell alpha
  interior-3d.json                      group/cell world AABBs + image paths

The viewer's per-group 3D checkbox draws these sprites at the same world placement.
"""
import argparse
import concurrent.futures
import json
import os
import re
import shutil
import subprocess
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
    ap.add_argument('--workers', type=int, default=1,
                    help='one wine process per map; 3D sprite generation is CPU-light')
    ap.add_argument('--only', default=None, help='comma-separated map ids')
    ap.add_argument('--force', action='store_true')
    args = ap.parse_args()

    data = json.load(open(args.manifest))
    recs = [r for r in data['maps'] if 'error' not in r and r['id'] not in CONTINENTS]
    if args.only:
        want = {int(x) for x in args.only.split(',') if x.strip()}
        recs = [r for r in recs if r['id'] in want]

    def dump(rec):
        mid, dirn = rec['id'], rec['dir']
        slug = slugify(dirn)
        folder = '%d_%s' % (mid, slug)
        root = os.path.join(args.root, 'maps', folder)
        out = os.path.join(root, 'out')
        jpath = os.path.join(out, 'interior-3d.json')
        if os.path.exists(jpath) and not args.force:
            return rec, 0, 'exists'
        cache = os.path.join(root, slug + '.quadcache')
        if not os.path.exists(cache):
            return rec, 1, 'no quad cache'
        tiles = rec['tiles']
        crop = CROP.get(mid)
        if crop:
            tx0, tx1, ty0, ty1 = crop
            tiles = [[tx, ty] for tx, ty in tiles if tx0 <= tx <= tx1 and ty0 <= ty <= ty1]
        if not tiles:
            return rec, 1, 'no tiles'
        tx, ty = tiles[0]
        art_dir = os.path.join(out, 'interior-3d')
        if os.path.isdir(art_dir):
            shutil.rmtree(art_dir)
        if os.path.exists(jpath):
            os.remove(jpath)
        env = dict(os.environ)
        env.setdefault('WINEPREFIX', '/media/storage/wineprefixes/mapbaker')
        env.setdefault('DISPLAY', ':99')
        env.setdefault('WINEDLLOVERRIDES', 'd3d9,dxgi=n')
        env.setdefault('WINEDEBUG', '-all')
        env.setdefault('DXVK_LOG_LEVEL', 'error')
        env.setdefault('DXVK_STATE_CACHE', '0')
        cmd = [args.wine, to_win_path(args.exe), '--client', to_win_path(args.client),
               '--map', str(mid), '--tx', str(tx), '--ty', str(ty),
               '--px', '1024', '--ssaa', '1', '--preset', 'map', '--light-elev', '60',
               '--gain', '1.3', '--quad-cache', to_win_path(cache),
               '--dump-interior-3d', '--force-wmo', '--out', to_win_path(out)]
        logf = os.path.join(root, 'logs', 'interior-3d-dump.log')
        os.makedirs(os.path.dirname(logf), exist_ok=True)
        with open(logf, 'w') as lf:
            r = subprocess.run(cmd, stdout=lf, stderr=subprocess.STDOUT, env=env)
        if r.returncode != 0:
            return rec, r.returncode, 'dump failed'
        if not os.path.exists(jpath):
            return rec, 1, 'no json'
        return rec, 0, ''

    log('interior 3D dump: %d maps' % len(recs))
    failed = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.workers) as pool:
        futs = {pool.submit(dump, rec): rec for rec in recs}
        for fut in concurrent.futures.as_completed(futs):
            rec = futs[fut]
            try:
                _, rc, msg = fut.result()
            except Exception as e:
                rc, msg = 1, str(e)
            if rc != 0:
                failed.append(rec['id'])
            log('3d %s rc=%d %s' % (rec['dir'], rc, msg))
    log('done; failed: %s' % (sorted(failed) if failed else 'none'))


if __name__ == '__main__':
    main()
