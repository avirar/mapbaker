# MapBaker — true-3D top-down minimap tile baker

Offline map baker for WoW 3.3.5a (branch `feature/mapbaker`). It renders ADT tiles with the
**real client world pipeline** (terrain, WMO, M2, liquids) into an offscreen render target,
composites the **client's own 2D minimap art** on top, and stitches the result into a
gigapixel map: PNG tiles, a pyramidal BigTIFF master, a DZI deep-zoom pyramid, and a
browser viewer.

The final delivery uses four independent layers:

| layer | contents |
|---|---|
| `scene` | opaque 3D terrain + WMO + trees + water — the high-res replacement for the outdoor minimap |
| `interiors` | transparent 3D render of the **stock interior minimap locations only** |
| `quads-ext` | the client's stock outdoor minimap, one `Textures\Minimap\<dir>\map<tx>_<ty>.blp` image per ADT |
| `quads-int` | the stock WMO interior minimap art cells, as a whole-map x-ray overlay |

`interiors` is filtered to WMO groups that actually have stock interior art cells, and it
does not render outdoor M2 props. Instances keep their full interior render. The viewer
defaults to **scene + interiors only**; both quad layers are off until toggled. For
instances and battlegrounds the viewer also exposes a per-group tunnel x-ray list: tick a
WMO group and its stock minimap cells are drawn over the scene with the same local → world
instance matrix the client uses (e.g. Stratholme `instancehall03` pink tunnel over
`stronghold01` red roof).

---

## Quick start

```bat
:: build (kills a running instance first — the exe is locked while running)
cmake --build build/x64 --config Release --target MapBaker --parallel

:: one ADT tile
bin\Win\MapBaker64.exe --client C:\Games\ChromieCraft_3.3.5a --map 0 ^
    --tx 30 --ty 48 --px 1024 --ssaa 2 --preset map --light-elev 60 ^
    --quads --out out\sw

:: interactive web tuner
bin\Win\MapBaker64.exe --client C:\Games\ChromieCraft_3.3.5a --map 0 ^
    --tx0 30 --tx1 33 --ty0 47 --ty1 50 --px 512 --ssaa 1 ^
    --preset map --light-elev 60 --quads --serve --port 8742 --out out\serve
:: open http://127.0.0.1:8742

:: full Eastern Kingdoms (882 ADT slots, 21x42), parallel + cached collection
python Tools\MapBaker\bake_parallel.py --client C:\Games\ChromieCraft_3.3.5a --map 0 ^
    --tx0 24 --tx1 44 --ty0 20 --ty1 61 --px 1024 --ssaa 2 ^
    --out Z:\aviroth-testrealm-data\mapbaker\ek1024 --workers 3 ^
    --quad-cache Z:\aviroth-testrealm-data\mapbaker\ek1024\ek.quadcache ^
    --extra "--light-elev 60"

:: stitch -> BigTIFF + DZI + viewer (libvips)
python Tools\MapBaker\stitch.py --tiles Z:\...\ek1024 --out C:\mapbaker\ek1024 ^
    --work C:\Users\%USERNAME%\AppData\Local\Temp\mbstitch --name ek1024 ^
    --tx0 24 --tx1 44 --ty0 20 --ty1 61 --px 1024
```

Serve the output folder over HTTP for the viewer (`file://` blocks the DZI fetch):

```bat
python -m http.server 8765 --directory C:\mapbaker\ek1024
:: open http://127.0.0.1:8765/viewer.html
```

---

## How it works

### 1. Offline 3D render (`MapBakerWorld.cpp`)
For each ADT: load the continent, place a top-down orthographic camera at the tile centre
(`z = terrain + 1000`), call the client's world pipeline, read the render target back
(`GxReadRenderTarget`), optionally apply a brightness gain, composite the 2D art, then
box-downsample the supersampled buffer to `--px`.

### 2. 2D minimap art (`MapBakerArt.*`, `CompositeArt`)
The original minimap does not tint WMO footprints — it draws per-cell `.blp` artwork:

- The cell grid is built in **WMO-local space** from `CMapObjGroup::m_bbox` (the MOGP
  header bbox), exactly like `CMapObj::GetMinimapQuads` (texel = 0.5 world units, cell
  textures 32..256 px, 1.0-unit border on the outer cells).
- Each cell is named `<WMO path minus "World\"/ext>_<grp:03d>_<x:02d>_<y:02d>.blp`,
  resolved through `Textures\Minimap\md5translate.trs` to a file in `Textures\Minimap\`.
- Cells are decoded with the engine's `CBLPFile` (DXT1/3/5 → BGRA) and placed through the
  instance matrix (`mapObjDef->m_mat`), rasterised as screen-space parallelograms with
  bilinear sampling (UV: u along local +X, v = 1 at minY).
- Only groups the user selected are drawn; cells without an art file are skipped.

**Default selection is ALL groups.** The real city-building art lives on the *interior*
groups (0x8 clear); the exterior set is mostly trick shells (e.g. the comically-large
Stormwind cathedral facade, SMO group `cathy_facade01`). Because groups without art simply
draw nothing, "all" paints exactly the art the client shipped.

### 3. District fill (`MapBakerAddMissingGroupsToSortTable`, World.cpp)
The 2004 client renders WMO groups by viewpoint: the outdoor cull
(`CWorldScene::AddMapObjDefGroup`) only accepts groups whose **MOGI** flags contain `0x8`
(exterior) or `0x10000` (always visible); interior districts are only drawn when the camera
stands inside that WMO (`LocateViewer3` → `RenderInterior` → portal traversal →
`AddMapObjDefGroupToSortTable`, which has no flag filter).

A top-down bake has no such viewpoint, so the districts (Stormwind's `bazaar01`,
`thief01`, `entrance01`, `Taventrance*`, ... — 267 of 298 SW groups) would stay bare terrain.
MapBaker calls the same no-filter interior-path callback for every group of every loaded WMO
overlapping the tile, before the draw loop. Depth-tested, so it lands **between
terrain+WMO and the 2D art overlay**. Disable with `--no-quad-fill`.

### 4. Water and lighting
- **Liquid specular is disabled** for the `map` preset (`Liquid::s_specularEnabled` is a
  separate global from `CWorld::enables`). Left on, the camera-anchored specular highlight
  makes every ocean ADT stand out (measured +8.8 luminance step at ADT borders vs 0.00 in
  the original minimap reference).
- **WMO water renders with the correct liquid type**: the water materials early-out until
  their animated texture set finishes loading asynchronously. `MapBakerWaitTextures`
  (set in `LoadMap`) makes `CMaterialSettings::GetTexture` use `TextureGetGxTex(..., wait=1)`,
  so the LiquidType/LiquidMaterial DBC-selected material (water/slime/lava) loads on the
  first tile.
- **Terrain textures are synchronous too**: the same `MapBakerWaitTextures` flag now also
  forces `TextureGetGxTex(..., wait=1)` for terrain/M2/WMO textures. Without it, a long
  worker sequence can draw a chunk before its terrain BLP finishes loading, leaving whole
  1/16-ADT chunks grey. A 128-tile 4096 repro went from 78.5% grey in the affected area to
  0%; per-tile time was unchanged (620 → 637 ms). 1024 and 4096 deliveries use this.
- Terrain tone varies by location **on purpose**: baked per-chunk normals (MCNR), baked
  terrain shadow maps (MCSH/MCAL) and `Light.dbc` zone lights are the client's own
  rendering. It matches the original minimap reference.

---

## CLI reference

| option | meaning |
|---|---|
| `--client <path>` | client data root (default: exe dir) |
| `--map <dir\|id>` | map directory name or numeric id (**required**) |
| `--preset <name>` | `map` (default) or `realistic` |
| `--px <n>` | tile resolution, default 1024 |
| `--ssaa <n>` | supersampling factor, default 2 |
| `--out <dir>` | PNG tile output directory (created by *you* — batch mode does not create it) |
| `--tx/--ty`, `--tx0..--ty1`, `--all` | tile selection |
| `--tile-list <file>` | render only `tx ty` lines from a file (worker assignment) |
| `--skip-existing` | skip tiles whose PNG already exists (resume) |
| `--dump-tiles` | dump the ADT grid from the WDT area table, then exit |
| `--no-wmo` | terrain only |
| `--light-elev <deg>` | sun elevation above the horizon (default: engine noon ~37°); `--flat-light` = 89 |
| `--light-mode <client\|neutral\|desat\|flat>` | light **colour/intensity**: `client` keeps `Light.dbc` zone colours; `neutral` luminance-gray with each zone's intensity; `desat` uses `--light-desat`; `flat` = neutral + fixed total intensity. `map` preset defaults to **flat**, `realistic` to `client`; explicit flags win. Covers terrain and `DayNight::m_light0/m_light1` (WMO/M2 lighting) |
| `--light-desat <0..1>` | partial desaturation (implies `--light-mode desat`; `1` = neutral) |
| `--ignore-zone-light` | alias for `--light-mode flat` |
| `--neutral-light` | alias for `--light-mode neutral` |
| `--water-mode <client\|map\|desat>` | water **colour**: `client` keeps `LightIntBand` zone water; `map` uses one flat ocean palette `(30,59,93)` (map preset default); `desat` desaturates the client bands. The map palette is scaled by the neutral light intensity so every ADT matches |
| `--gain <f>` | post-render brightness gain |
| `--city-file <path>` / `--city-elev <deg>` | city ADT list + its sun elevation |
| `--xray <off\|depthoff\|zsort\|zinv>` etc. | x-ray city-floor modes (legacy/indoor look) |
| `--layer quads-ext` | stock outdoor minimap layer: one client `map<tx>_<ty>.blp` per ADT (RGBA) |
| `--layer quads-int` | stock WMO interior art cells (same as `--layer quads --quad-sel int`) |
| `--dump-interior-art` | write `<out>/interior-art.json` + per-cell PNGs for the per-group tunnel x-ray viewer, then exit |
| `--quads` | enable the 2D WMO-art overlay (collect groups + composite) |
| `--quad-cache <file>` | binary group-table cache (`MBQUAD02` v2): loads instantly if present, else collects once and saves |
| `--quad-dump <file>` | dump the collected group table (names, flags, bbox, local bbox, matrix, MOGI flags, batch layout) |
| `--quad-tint` | flat per-class tints instead of real art (diagnostic) |
| `--no-quad-wmo` | do not render the 3D WMO under the art |
| `--no-quad-fill` | do not add the viewpoint-culled groups (no district fill) |
| `--quad-alpha <0-255>` | tint alpha (art uses the texture's own alpha, 255) |
| `--quad-sel <none\|all\|ext\|int>` | bulk group selection (default `all` in art mode) |
| `--serve --port <n>` | interactive web tuner |

Useful env vars: `MAPBAKER_DEBUG_LIQUID`, `MAPBAKER_DEBUG_VIEW`, `MAPBAKER_SKIP_LIQUID`,
`MAPBAKER_SKIP_TERRAIN`, `MAPBAKER_SKIP_WMO`, `MAPBAKER_SKIP_M2`, `MAPBAKER_ART_DUMP`.

---

## Serve mode (web tuner)

`--serve` keeps the scene loaded and renders on demand. Endpoints:

- `/` — settings page (presets, x-ray, quads, per-group list, alpha/tints, CLI mirror)
- `/status` — JSON: render progress + applied settings (incl. `qon/qart/qfill/qwmo/qcount`)
- `/groups.json` — the collected WMO group table (one object per instance+group)
- `/render?…` — queue a render with query-string settings
- `/montage.png`, `/crop2x.png` — stitched region and 2× city crop
- `/presets` — saved preset list (GET/POST)
- `/dump.txt` — the x-ray per-WMO dump, if enabled

Settings are applied on the event thread; the page refreshes automatically via the `gen`
counter.

---

## Parallel bakes, resume, cache

- `Tools/MapBaker/bake_parallel.py` splits the region into contiguous row bands and runs one
  **process** per band. Threads are not an option: the client engine + D3D9 device is a
  single global instance.
- Workers share `--quad-cache`, so the (expensive) WMO collection runs once, not per worker.
- `--skip-existing` + the driver's missing-tile scan make runs resumable; re-running after a
  crash only bakes what is missing.
- Rough numbers on an i5-6500T / HD 530 / 16 GB: ~64 tiles/min at 512 px/ADT; 882 tiles at
  1024 px/ADT with SSAA 2 in ~3–4 minutes across 3 workers (render is partly GPU-bound;
  RAM is the real limiter at ~1 GB per worker).

### Quad table cache
`--quad-cache` stores the collected table (`MBQUAD02`, version 2). It is independent of
resolution — collect once and reuse for every bake, serve session and worker.
Deleting the file forces a fresh collection.

---

## Final delivery pipeline

The four-layer delivery is built from the inventory manifest:

```bash
# stock outdoor minimap tiles -> <slug>-quads-ext.dzi
python Tools/MapBaker/bake_quads_ext.py --manifest <manifest.json> --root <delivery> \
    --client <client> --exe bin/Win/MapBaker64.exe --workers 4

# corrected open-world interior remaster (0/1/530/571)
python Tools/MapBaker/bake_interiors_openworld.py --manifest <manifest.json> \
    --root <delivery> --client <client> --exe bin/Win/MapBaker64.exe --workers 3

# per-group stock interior art for instance/battleground viewers
python Tools/MapBaker/dump_interior_art.py --manifest <manifest.json> --root <delivery> \
    --client <client> --exe bin/Win/MapBaker64.exe --workers 4

# unified four-layer viewers + /remaster/ gallery
python Tools/MapBaker/make_final_viewers.py
```

`stitch.py` intentionally no longer writes `viewer.html` unless `--viewer` is passed;
otherwise a layer stitch would overwrite the unified viewer. The final index is written
by `bake_all.py --skip-bake --skip-stitch`.

---

## Stitching (`Tools/MapBaker/stitch.py`)

Requires libvips 8.x. Steps: background-filled row
strips (tiles missing or single-colour are replaced with `--background`) → mosaic `.v` →
**pyramidal BigTIFF** (512-px tiles, `--tiff-compression` / `--tiff-level`) → **DZI**
pyramid (512 px, 1-px overlap) → preview. `--viewer` optionally writes a single-layer
`viewer.html`; the final pipeline leaves that to `make_interior_viewer.py`, which writes
the unified four-layer viewer and both `viewer.html` and `viewer-interiors.html`.

For large 4096 stitches prefer `--tiff-compression zstd --tiff-level 1`: it is lossless,
~6.5× faster than deflate and ~24% smaller on Northrend. Default remains deflate for
compatibility.

- **Always prefer DZI.** libvips 8.18 `dzsave --layout google` writes `{z}/{row}/{col}.jpg`
  (x/y swapped vs the XYZ spec), which scrambles every standard XYZ viewer at zoom. DZI is
  unambiguous and verified against the mosaic (mean |d| ≈ 2.7 = JPEG noise).
- Put `--work` on a **local SSD** when `--out` is a network share: writing the 2.8 GB
  mosaic over SMB stalls.
- Row strips are built in parallel (`--row-jobs`), and `VIPS_CONCURRENCY` is raised for the
  BigTIFF/DZI stages.

Outputs (Eastern Kingdoms example): `tiles/` (882 PNG), `ek1024.tif` (21,504 × 43,008,
8 pyramid levels, ~0.9 GB), `ek1024.dzi` + `ek1024_files/` (4,744 tiles), preview, viewer.

---

## Notes & caveats

- Kill a running `MapBaker64.exe` before relinking (it locks the binary).
- Batch mode does **not** create `--out`; a missing directory silently skips writes while
  still reporting "tiles written".
- The serve page is a raw string literal in `MapBakerServe.cpp`; a single MSVC string
  literal is capped at 16,384 chars — the page is split into adjacent `R"HTML(...)"` chunks.
- The quad table holds up to 65,536 groups (`MAPBAKER_QUAD_MAX`); the EK continent collects
  ~4,400.
- The art cell cache accepts up to 8 GB (a full continent stays resident; it is never
  evicted mid-tile, which used to invalidate in-use image pointers).
- `--light-elev 60` is the tuned value for the baked minimap look; `map` preset = noon, no
  fog/sky/weather, shadows/specular/detail doodads off. The `map` preset uses **flat** light
  by default: Northrend's `Light.dbc` zone records are strongly cyan and would otherwise paint
  Borean Tundra/Dragonblight/Icecrown blue. Pass `--light-mode client` (or `--neutral-light`,
  `--light-desat <0..1>`) to restore the zone colours. `flat` also normalises the total light
  intensity, because WMOs/M2s are lit directly from `DayNight::m_light1` and would otherwise
  change colour/brightness at an ADT border. Direction and elevation stay with `--light-elev`.
- The client's per-zone water bands swing hard (Northrend: dark blue 16,40,76 → green
  52,76,64 → blue 29,58,93). The `map` preset therefore defaults to `--water-mode map`,
  which writes one flat ocean palette into `DayNight::DNInfo::m_bands.m_waterColors` and
  scales it by `ambient + diffuse`, so adjacent ocean ADTs render the same colour.
- The ortho left clip plane can corrupt the first render-target column on some tile
  alignments (Northrend: every `tx % 3 == 0`), producing a bright 1-px vertical seam.
  `RenderTile` copies column 1 over column 0 at RT resolution, mirroring the existing
  row-0 fix; the box downsample then sees a clean pair.

## Source layout

```
Tools/MapBaker/
  main.cpp            CLI parsing + defaults
  MapBakerApp.*       engine bring-up, tile queue, batch render loop
  MapBakerWorld.*     offscreen world render, camera, presets, quad compositing, collection
  MapBakerArt.*       md5translate.trs loading, BLP cell decode + cache
  MapBakerQuad.h      collected group table (the serve/World.cpp/MapBakerWorld data contract)
  MapBakerServe.*     HTTP server + embedded web tuner + presets
  MapBakerPng.*       minimal PNG writer
  MapBakerXray.h      x-ray / trick-guard knobs shared with the engine hooks
  bake_parallel.py    row-band worker driver (resume + shared cache)
  stitch.py           vips stitcher: BigTIFF + DZI + viewer
```

Engine/client patches are additive and dormant unless MapBaker drives them:
`GxReadRenderTarget`, headless console device, `MapRenderChunk` readback support, and the
hooks in `WoW/Source/WorldClient/World.cpp` (collection, district fill, x-ray draw).
