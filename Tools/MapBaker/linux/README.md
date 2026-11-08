# MapBaker on Linux

MapBaker is a Windows/D3D9 tool. This directory builds it on Linux as a Windows
x64 PE using clang-cl, lld-link and the MSVC CRT/SDK staged by
[`xwin`](https://github.com/Jake-Shadle/xwin), then runs it under Wine + DXVK.

## Prerequisites

- clang-cl, lld-link, llvm-lib, llvm-rc, ccache, cmake, ninja
- `xwin` (cargo install xwin, or a prebuilt release)
- Wine + a DXVK d3d9/dxgi pair (GE-Proton's runtime is fine)
- Xvfb (headless) or a real X display

The scripts default to:

| path | meaning |
|---|---|
| `/media/storage/toolchains/xwin` | xwin splat output |
| `/media/storage/ccache` | ccache dir |
| `/media/storage/wineprefixes/mapbaker` | Wine prefix |
| `/media/storage/games/ChromieCraft_3.3.5a` | client data |

Override `XWIN_ROOT`, `WINEPREFIX`, `DISPLAY`, `MAPBAKER_EXE`, etc. from the
environment.

## Stage the MSVC headers/libs

```bash
cargo install --locked xwin
CARGO_HOME=/media/storage/toolchains/cargo \
  xwin --accept-license splat --output /media/storage/toolchains/xwin
```

## Build

```bash
Tools/MapBaker/linux/build-mapbaker.sh
# -> bin/Win/MapBaker64.exe
```

The toolchain adds the portability flags the decompiled MSVC sources need:
`-fms-extensions`, SEH codegen, `-fno-strict-aliasing`, no FP contraction,
`-Wno-register`, `-Wno-c++11-narrowing`. The runtime is `/MD`, matching the
existing MSVC build.

## Run under Wine + DXVK

```bash
source Tools/MapBaker/linux/env.sh
xvfb_start
wine_mapbaker --client 'Z:\media\storage\games\ChromieCraft_3.3.5a' --map 0 --dump-tiles
```

For a single tile:

```bash
mkdir -p /media/storage/mapbaker/out
wine_mapbaker --client 'Z:\media\storage\games\ChromieCraft_3.3.5a' --map 0 \
  --tx 30 --ty 48 --px 1024 --ssaa 2 --preset map --light-elev 60 --quads \
  --quad-cache 'Z:\media\storage\mapbaker\reference\ek.quadcache' \
  --out 'Z:\media\storage\mapbaker\out'
```

Select WineD3D instead of DXVK with `WINEDLLOVERRIDES=d3d9,dxgi=b`; select DXVK
with `WINEDLLOVERRIDES=d3d9,dxgi=n`.

## Mass bake

`bake_parallel.py` accepts `--exe` / `--wine` and converts Linux paths to Wine
`Z:` paths automatically.

```bash
python3 Tools/MapBaker/bake_parallel.py \
  --client /media/storage/games/ChromieCraft_3.3.5a --map 0 \
  --tx0 24 --tx1 44 --ty0 20 --ty1 61 --px 1024 --ssaa 2 \
  --out /media/storage/mapbaker/ek1024 --workers 6 \
  --quad-cache /media/storage/mapbaker/reference/ek.quadcache \
  --exe /media/storage/projects/wowdecomp/bin/Win/MapBaker64.exe \
  --extra "--light-elev 60"
```

`stitch.py` uses the system `vips` automatically; `--osd` copies the vendored
OpenSeadragon tree next to `viewer.html`.
