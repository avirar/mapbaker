#!/usr/bin/env python3
"""Inventory every bakeable map: valid ADT tiles, bounds and directory name.

Runs MapBaker with --dump-tiles once per map (no rendering) and writes a JSON
manifest that bake_all.py uses to size per-map jobs.  Safe to rerun; results are
replaced atomically.
"""
import argparse
import concurrent.futures
import json
import os
import re
import subprocess
import sys
import time

SUMMARY = re.compile(
    r"map '(?P<dir>.*?)' \(id (?P<id>\d+)\): (?P<count>\d+) valid ADTs, "
    r"tx (?P<tx0>\d+)\.\.(?P<tx1>\d+) \((?P<w>\d+) wide\), "
    r"ty (?P<ty0>\d+)\.\.(?P<ty1>\d+) \((?P<h>\d+) tall\)")


def to_win_path(path):
    if not path or os.name == 'nt' or (len(path) >= 2 and path[1] == ':'):
        return path
    return 'Z:' + os.path.abspath(path).replace('/', '\\')


def read_ids(path):
    text = open(path, encoding='utf-8').read()
    m = re.search(r'```\n(.*?)\n```', text, re.S)
    if not m:
        ids = re.findall(r'\b\d+\b', text.split('## Excluded')[0])
    else:
        ids = m.group(1).strip().split(',')
    return sorted(set(int(x.strip()) for x in ids if x.strip().isdigit()))


def dump_one(mid, args, env):
    cmd = [args.wine, to_win_path(args.exe),
           '--client', to_win_path(args.client),
           '--map', str(mid), '--dump-tiles']
    t0 = time.time()
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=args.timeout,
                           env=env)
    except subprocess.TimeoutExpired:
        return {'id': mid, 'error': 'timeout'}
    m = None
    tiles = []
    in_tiles = False
    for line in r.stdout.splitlines():
        sm = SUMMARY.search(line)
        if sm:
            m = sm.groupdict()
            in_tiles = True
            continue
        if in_tiles:
            parts = line.strip().split()
            if len(parts) == 2 and parts[0].isdigit() and parts[1].isdigit():
                tiles.append([int(parts[0]), int(parts[1])])
    if not m:
        return {'id': mid, 'error': 'no summary (rc %d)' % r.returncode,
                'stderr': r.stderr[-500:]}
    rec = {
        'id': mid,
        'dir': m['dir'],
        'count': int(m['count']),
        'tx0': int(m['tx0']), 'tx1': int(m['tx1']),
        'ty0': int(m['ty0']), 'ty1': int(m['ty1']),
        'width': int(m['w']), 'height': int(m['h']),
        'tiles': tiles,
        'seconds': round(time.time() - t0, 2),
    }
    if len(tiles) != rec['count']:
        rec['warning'] = 'tile count mismatch: parsed %d' % len(tiles)
    return rec


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--maps-file', default='Tools/MapBaker/maps-to-bake.md')
    ap.add_argument('--client', required=True)
    ap.add_argument('--exe', required=True)
    ap.add_argument('--wine', default='wine')
    ap.add_argument('--out', required=True)
    ap.add_argument('--workers', type=int, default=3)
    ap.add_argument('--timeout', type=int, default=180)
    args = ap.parse_args()

    ids = read_ids(args.maps_file)
    env = dict(os.environ)
    env.setdefault('WINEPREFIX', '/media/storage/wineprefixes/mapbaker')
    env.setdefault('DISPLAY', ':99')
    env.setdefault('WINEDLLOVERRIDES', 'd3d9,dxgi=n')
    env.setdefault('WINEDEBUG', '-all')
    env.setdefault('DXVK_LOG_LEVEL', 'error')
    env.setdefault('DXVK_STATE_CACHE', '0')

    os.makedirs(os.path.dirname(os.path.abspath(args.out)) or '.', exist_ok=True)
    print('inventory: %d map ids, %d workers' % (len(ids), args.workers), flush=True)
    results = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.workers) as pool:
        futs = {pool.submit(dump_one, mid, args, env): mid for mid in ids}
        for fut in concurrent.futures.as_completed(futs):
            rec = fut.result()
            results.append(rec)
            if 'error' in rec:
                print('  map %3d ERROR %s' % (rec['id'], rec['error']), flush=True)
            else:
                print('  map %3d %-28s %4d tiles  tx %d..%d ty %d..%d' %
                      (rec['id'], rec['dir'], rec['count'],
                       rec['tx0'], rec['tx1'], rec['ty0'], rec['ty1']), flush=True)

    results.sort(key=lambda r: r['id'])
    ok = [r for r in results if 'error' not in r]
    total = sum(r['count'] for r in ok)
    payload = {
        'generated': time.strftime('%Y-%m-%dT%H:%M:%S'),
        'client': os.path.abspath(args.client),
        'total_maps': len(results),
        'ok_maps': len(ok),
        'failed_maps': [r['id'] for r in results if 'error' in r],
        'total_tiles': total,
        'maps': results,
    }
    tmp = args.out + '.tmp'
    with open(tmp, 'w', encoding='utf-8') as f:
        json.dump(payload, f, indent=1)
    os.replace(tmp, args.out)
    print('wrote %s: %d maps, %d tiles, %d failed' %
          (args.out, len(results), total, len(payload['failed_maps'])), flush=True)


if __name__ == '__main__':
    main()
