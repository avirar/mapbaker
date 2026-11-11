# MapBaker

Offline, layered map renderer for World of Warcraft 3.3.5a. MapBaker renders every ADT
through the game's real world pipeline — terrain, WMO buildings, M2 props, trees and
liquids — from a top-down orthographic camera, then stitches the tiles into deep-zoom
pyramids you can browse in a browser.

The output is split into independently toggleable layers instead of one flat image:

| Layer | Contents | Format |
|---|---|---|
| `terrain` | Terrain, terrain shadows, ADT water/lava/slime | opaque RGB |
| `wmo` | WMO buildings + all non-tree M2 props | transparent RGBA |
| `trees` | Common tree M2s only | transparent RGBA |
| `quads` | The client's original 2D minimap artwork | transparent RGBA |
| `combined` | Legacy single-image render (all of the above) | opaque RGB |

Each layer gets a BigTIFF master, a DZI deep-zoom pyramid and a shared browser viewer
with layer toggles, a minimap-opacity slider, reordering and a checkerboard background
when terrain is hidden.

---

## Requirements

- World of Warcraft **3.3.5a (12340)** game data. No game data is included here.
- **Linux:** clang-cl, lld-link, llvm-lib, llvm-rc, CMake, Ninja, ccache, `xwin`,
  Wine + DXVK, Xvfb or a real X display.
- **Windows:** Visual Studio 2022 with the Desktop C++ workload, CMake.
- **Stitching:** libvips 8.x (`vips`, `dzsave`, BigTIFF support).
- **Runtime:** Python 3.10+ for the batch driver, stitcher and viewer generator.
- A GPU that supports a 8192×8192 D3D9 render target for 4096 px/ADT with SSAA 2.
- About 1 GB RAM per bake worker; an SSD for large stitch work directories.

---

## Build

### Linux (Windows PE + Wine/DXVK)

```bash
# once: stage the MSVC headers/libs
cargo install --locked xwin
xwin --accept-license splat --output /path/to/xwin

# build
XWIN_ROOT=/path/to/xwin Tools/MapBaker/linux/build-mapbaker.sh
# -> bin/Win/MapBaker64.exe
```

`Tools/MapBaker/linux/env.sh` provides the Wine helpers used below:

```bash
source Tools/MapBaker/linux/env.sh
xvfb_start
```

### Windows

```powershell
cmake -S . -B build/x64 -G "Visual Studio 17 2022" -A x64
cmake --build build/x64 --config Release --target MapBaker --parallel
# -> bin\Win\MapBaker64.exe
```

Stop any running `MapBaker64.exe` before rebuilding: the executable is locked while
it is running.

---

## Quick start: one tile

Linux/Wine (the client path is a Wine `Z:` path; `bake_parallel.py` converts paths for
you, but the executable itself needs this form):

```bash
source Tools/MapBaker/linux/env.sh
xvfb_start
mkdir -p /path/to/out
wine_mapbaker \
  --client 'Z:\path\to\3.3.5a' \
  --map 571 --tx 34 --ty 23 \
  --px 1024 --ssaa 2 --preset map --light-elev 60 \
  --layer wmo \
  --out 'Z:\path\to\out'
```

Windows:

```bat
bin\Win\MapBaker64.exe --client C:\Games\3.3.5a ^
  --map 571 --tx 34 --ty 23 ^
  --px 1024 --ssaa 2 --preset map --light-elev 60 ^
  --layer wmo --out C:\mapbaker\out
```

The four transparent layers (`wmo`, `trees`, `quads`) write RGBA PNGs. `terrain` and
`combined` write RGB PNGs.

---

## Bake a continent in four layers

Northrend 571, 1024 px/ADT, local storage. Adjust the grid for another map.

```bash
CLIENT=/path/to/3.3.5a
ROOT=/path/to/mapbaker/northrend1024
MAP=571
TX0=11 TX1=49 TY0=9 TY1=37
PX=1024
EXE=bin/Win/MapBaker64.exe
CACHE=/path/to/northrend.quadcache

mkdir -p "$(dirname "$CACHE")"

for layer in terrain wmo trees quads; do
  extra="--light-elev 60 --layer $layer"
  python3 Tools/MapBaker/bake_parallel.py \
    --client "$CLIENT" --map "$MAP" \
    --tx0 "$TX0" --tx1 "$TX1" --ty0 "$TY0" --ty1 "$TY1" \
    --px "$PX" --ssaa 2 --workers 6 \
    --out "$ROOT/tiles/$layer" \
    --quad-cache "$CACHE" \
    --exe "$EXE" \
    --extra "$extra"
done
```

Notes:

- `bake_parallel.py` always selects the `map` preset and adds `--quads`. Passing a
  shared `--quad-cache` means the WMO group table is collected once and reused by the
  other layers and workers.
- Each worker handles a contiguous band of ADT rows, so it streams the client data
  locally and never shares the D3D device between threads.
- `--skip-existing` is applied by the driver; rerunning bakes only missing tiles.
- Output: `tiles/<layer>/tile_<tx>_<ty>.png`.

---

## Stitch the layers

```bash
ROOT=/path/to/mapbaker/northrend1024
OUT=$ROOT/out
MAP=571
TX0=11 TX1=49 TY0=9 TY1=37
PX=1024

mkdir -p "$OUT"

for layer in terrain wmo trees quads; do
  alpha=""
  [ "$layer" != terrain ] && alpha="--alpha"
  python3 Tools/MapBaker/stitch.py \
    --tiles "$ROOT/tiles/$layer" \
    --out "$OUT" \
    --name "northrend-$layer" \
    --map-id "$MAP" \
    --tx0 "$TX0" --tx1 "$TX1" --ty0 "$TY0" --ty1 "$TY1" \
    --px "$PX" \
    --background 1d3b5d \
    --work "$ROOT/work-$layer" \
    --row-jobs 6 --row-concurrency 2 \
    --tiff-compression zstd --tiff-level 1 \
    $alpha
done
```

- `--alpha` writes a transparent background tile and PNG DZI tiles. Without it the
  stitcher writes JPEG DZI tiles filled with `--background`.
- The viewer needs a local OpenSeadragon build. Put it under `out/openseadragon/`
  (the `stitch.py --osd <dir>` option can copy it while stitching):

  ```bash
  cp -r /path/to/openseadragon "$OUT/openseadragon"
  ```
- `zstd` level 1 is recommended for large masters: lossless, about 6.5× faster than
  deflate and roughly 24% smaller on Northrend.
- The work directory needs room for the row strips and the giant mosaic. Put it on a
  local SSD, not a network share.

Output per layer:

```
out/northrend-terrain.tif          # pyramidal BigTIFF master
out/northrend-terrain.dzi          # DZI descriptor
out/northrend-terrain_files/       # 512 px DZI tiles
out/northrend-terrain-preview.jpg
```

The transparent layers use `.png` previews and PNG DZI tiles.

---

## Build the layer viewer

```bash
python3 Tools/MapBaker/make_layer_viewer.py \
  --out /path/to/mapbaker/northrend1024/out \
  --name northrend \
  --map-id 571 \
  --tx0 11 --ty0 9 \
  --px 1024

cd /path/to/mapbaker/northrend1024/out
python3 -m http.server 8765 --bind 0.0.0.0
```

Open `http://localhost:8765/layer-viewer.html`.

The page stacks the four DZIs and provides:

- layer toggles that stay highlighted while active;
- a minimap-quad opacity slider;
- up/down reordering (for example, WMO above minimap quads);
- a checkerboard when terrain is off;
- an ADT/world-coordinate HUD.

The layer viewer expects `openseadragon/` next to the DZIs; `stitch.py --osd <dir>`
copies it during stitching if you prefer.

---

## Layers in detail

### `terrain`

The opaque base: terrain chunks, baked terrain normals/shadows, and ADT liquids. It
does not include WMO, M2 or minimap art. The clear colour is the map background
`#1d3b5d`.

ADT water is flushed in both the opaque and transparent liquid passes; missing the
second pass leaves lakebeds bare.

### `wmo`

Transparent WMO buildings plus every non-tree M2 prop. Terrain is rendered depth-only
first so buildings behind hills are occluded; the colour pass then draws only the
WMO/prop layer. Viewpoint-culled city districts are added back by the quad-fill path,
so interiors are not left as bare terrain.

Transient effect models (portals, fog, light rays, particles, ribbons) are excluded:
their additive black backgrounds would otherwise become opaque black planes in an RGBA
capture.

### `trees`

Common tree M2s only. Classification uses the model basename, not the directory name:

- names containing `tree` are trees;
- species-only names (`pine`, `oak`, `birch`, `aspen`, `palm`, …) match whole tokens;
- world/unique trees (`worldtree`, `teldrassil`, `nordrassil`, `vordrassil`,
  `ghanir`, …) are excluded;
- bushes, logs, mushrooms, trunks, stumps, saplings, rocks, roots, vines, bridges and
  similar props are excluded.

The Northrend pilot classifies roughly 110 common tree models. The trees layer intentionally
does not use a WMO depth pass: interior floors and ceilings would over-occlude the
canopy. Trees are composited above terrain/WMO in the viewer.

### `quads`

The original 3.3.5a minimap artwork, drawn as raw per-cell textures with their own
alpha. No shading or tint is applied. The WMO group table is collected once and can be
reused through `--quad-cache`.

### `combined`

The original single-image path: terrain, WMO, M2 and minimap art in one opaque PNG.
Useful as a reference and for comparison with the layered output.

---

## Look: lighting and water

`--preset map` is tuned for map output:

- flat light: fixed ambient `0.40` and diffuse `0.56`; the zone's colour cast is
  ignored. `--light-elev 60` is the tuned sun elevation;
- no sky, fog, weather or liquid specular;
- flat ocean palette (`30,59,93`) instead of per-zone water colours;
- opaque water, so the lakebed cannot tint adjacent ADTs differently;
- water uses only the flat sun; zone M2-scene lights are skipped.

This neutralises ADT-shaped seams. Zone-specific terrain normals and baked terrain
shadows remain, because they are part of the map's look.

Useful overrides:

| option | effect |
|---|---|
| `--light-mode client` | keep zone light colours |
| `--light-mode neutral` | desaturated zone light, keeping zone intensity |
| `--light-mode desat --light-desat <0..1>` | partial desaturation |
| `--light-mode flat` | fixed flat light (map default) |
| `--water-mode client` | keep the client's per-zone water bands |
| `--water-mode map` | flat ocean palette (map default) |
| `--water-mode desat` | desaturate the client water bands |
| `--gain <f>` | post-render brightness gain |
| `--flat-light` | alias for `--light-elev 89` |

---

## Map IDs

`Tools/MapBaker/maps-to-bake.md` contains the complete comma-separated list of normal
maps players can enter: 91 IDs. The large continents are:

- `0` Eastern Kingdoms
- `1` Kalimdor
- `530` Outland
- `571` Northrend

The list excludes test/development maps, transport/boat maps, unused maps and PvP
barracks. Sort the list if a script needs ascending IDs.

---

## Performance

Measured on the 8-core reference machine, local SATA SSD, 6 workers:

| workload | result |
|---|---|
| Northrend 571, 1024 px/ADT, SSAA 2, per layer | ~30–36 s (1131 tiles) |
| Northrend 571, 4096 px/ADT, SSAA 2, combined render | 292 s (1131 tiles) |
| Northrend 4096 stitch, zstd-1 master | 383 s |
| Northrend 4096 output | 13.27 GB BigTIFF + 5.3 GB DZI |

4096 px/ADT with SSAA 2 renders into an 8192×8192 target, the practical D3D9 texture
limit. For full-continent 4096 stitches, expect about 100 GB of transient work space.

---

## Repository layout

| Path | Contents |
|---|---|
| `Tools/MapBaker/` | Baker CLI, layer renderers, batch driver, stitcher, viewer generator |
| `Tools/MapBaker/linux/` | Linux cross-build toolchain and Wine helpers |
| `Engine/`, `WoW/` | Rendering stack used by the baker (Gx, Model2, world, liquids) |
| `bin/Win/` | Built `MapBaker64.exe` |

---

## Caveats

- A running `MapBaker64.exe` locks the executable; stop bakers before rebuilding.
- Batch mode does not create `--out`; create the directory first.
- WMO streaming is asynchronous. If a tile at the start of a long worker band looks
  sparse, rebake it after a neighbouring tile so the area is loaded.
- The stitcher's `--work` directory is much larger than the final output.
- The layer viewer needs HTTP; opening the page through `file://` blocks DZI requests.
