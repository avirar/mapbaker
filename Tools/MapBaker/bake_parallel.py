#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Parallel/resumable MapBaker driver.

The client engine + D3D9 device is a single global instance, so the renderer is NOT
thread-safe. Parallelism is therefore one *process* per tile band. Each worker:
  * gets a text list of "tx ty" tiles (its own contiguous band),
  * runs with --skip-existing so re-runs only bake what is missing,
  * performs its own quad-collection pass over its band.

Typical RAM: ~1 GB per worker. Keep workers <= free_RAM_GB.

Usage:
  python bake_parallel.py --client C:/Games/ChromieCraft_3.3.5a --map 0 \
      --tx0 24 --tx1 44 --ty0 20 --ty1 61 --px 1024 --ssaa 2 \
      --out Z:/aviroth-testrealm-data/mapbaker/ek1024 --workers 3
"""
import argparse
import os
import subprocess
import sys
import time

EXE = r'C:\GIT\wowdecomp\bin\Win\MapBaker64.exe'


def log(msg):
    print('[%s] %s' % (time.strftime('%H:%M:%S'), msg), flush=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--client', required=True)
    ap.add_argument('--map', default='0')
    ap.add_argument('--tx0', type=int, required=True)
    ap.add_argument('--tx1', type=int, required=True)
    ap.add_argument('--ty0', type=int, required=True)
    ap.add_argument('--ty1', type=int, required=True)
    ap.add_argument('--px', type=int, default=1024)
    ap.add_argument('--ssaa', type=int, default=2)
    ap.add_argument('--out', required=True)
    ap.add_argument('--workers', type=int, default=3)
    ap.add_argument('--quad-cache', default=None,
                    help='shared quad-table cache (collected once, loaded by every worker)')
    ap.add_argument('--extra', default='', help='extra MapBaker args (e.g. "--light-elev 60")')
    ap.add_argument('--dry-run', action='store_true')
    args = ap.parse_args()

    os.makedirs(args.out, exist_ok=True)

    # Missing tiles only (resume-friendly).
    rows = list(range(args.ty0, args.ty1 + 1))
    per = max(1, (len(rows) + args.workers - 1) // args.workers)
    total_missing = 0
    jobs = []
    for w in range(args.workers):
        band = rows[w * per:(w + 1) * per]
        if not band:
            continue
        tiles = []
        for ty in band:
            for tx in range(args.tx0, args.tx1 + 1):
                p = os.path.join(args.out, 'tile_%d_%d.png' % (tx, ty))
                if not os.path.exists(p):
                    tiles.append((tx, ty))
        if not tiles:
            log('worker %d: band ty %d..%d already complete' % (w, band[0], band[-1]))
            continue
        total_missing += len(tiles)
        list_path = os.path.join(args.out, '_worker_%d.txt' % w)
        with open(list_path, 'w') as f:
            for tx, ty in tiles:
                f.write('%d %d\n' % (tx, ty))
        jobs.append((w, band, tiles, list_path))

    log('%d workers, %d tiles missing of %d slots'
        % (len(jobs), total_missing,
           (args.tx1 - args.tx0 + 1) * (args.ty1 - args.ty0 + 1)))
    if args.dry_run or not jobs:
        return

    procs = []
    t0 = time.time()
    for (w, band, tiles, list_path) in jobs:
        log_path = os.path.join(args.out, '_worker_%d.log' % w)
        cmd = [EXE, '--client', args.client, '--map', args.map,
               '--tile-list', list_path, '--skip-existing',
               '--px', str(args.px), '--ssaa', str(args.ssaa),
               '--preset', 'map', '--quads', '--out', args.out]
        if args.quad_cache:
            cmd += ['--quad-cache', args.quad_cache]
        if args.extra:
            cmd += args.extra.split()
        logf = open(log_path, 'w')
        log('worker %d: ty %d..%d, %d tiles -> %s' % (w, band[0], band[-1], len(tiles), log_path))
        procs.append((w, cmd, logf, log_path, band))

    running = []
    for (w, cmd, logf, log_path, band) in procs:
        p = subprocess.Popen(cmd, stdout=logf, stderr=subprocess.STDOUT)
        running.append((w, p, logf, log_path, band))

    bad = 0
    for (w, p, logf, log_path, band) in running:
        code = p.wait()
        logf.close()
        done = sum(1 for ty in band for tx in range(args.tx0, args.tx1 + 1)
                   if os.path.exists(os.path.join(args.out, 'tile_%d_%d.png' % (tx, ty))))
        log('worker %d exited %d (band has %d/%d tiles) log=%s'
            % (w, code, done, len(band) * (args.tx1 - args.tx0 + 1), log_path))
        if code != 0:
            bad += 1

    dt = time.time() - t0
    missing = sum(1 for ty in range(args.ty0, args.ty1 + 1)
                  for tx in range(args.tx0, args.tx1 + 1)
                  if not os.path.exists(os.path.join(args.out, 'tile_%d_%d.png' % (tx, ty))))
    log('done in %.1f min; %d missing tiles; %d worker(s) failed' % (dt / 60, missing, bad))
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
