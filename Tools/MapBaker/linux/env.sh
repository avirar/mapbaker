#!/usr/bin/env bash
# Shared runtime environment for running MapBaker under Wine/DXVK on a headless box.
# Source this file, then run wine_mapbaker.
#
#   source Tools/MapBaker/linux/env.sh
#   xvfb_start
#   wine_mapbaker --client 'Z:\path\to\client' --map 0 --dump-tiles
export WINEPREFIX="${WINEPREFIX:-/media/storage/wineprefixes/mapbaker}"
export WINEARCH=win64
export DISPLAY="${DISPLAY:-:99}"
export WINEDLLOVERRIDES="${WINEDLLOVERRIDES:-d3d9,dxgi=n}"
export WINEDEBUG="${WINEDEBUG:--all}"
export DXVK_LOG_LEVEL="${DXVK_LOG_LEVEL:-error}"
export DXVK_HUD="${DXVK_HUD:-0}"
export DXVK_STATE_CACHE="${DXVK_STATE_CACHE:-0}"
export DXVK_LOG_PATH="${DXVK_LOG_PATH:-/media/storage/mapbaker/logs/dxvk}"

XWIN_ROOT="${XWIN_ROOT:-/media/storage/toolchains/xwin}"
MAPBAKER_EXE="${MAPBAKER_EXE:-/media/storage/projects/wowdecomp/bin/Win/MapBaker64.exe}"
MAPBAKER_EXE_WIN="${MAPBAKER_EXE_WIN:-Z:\\media\\storage\\projects\\wowdecomp\\bin\\Win\\MapBaker64.exe}"

xvfb_start() {
    if ! pgrep -f "Xvfb ${DISPLAY}" >/dev/null 2>&1; then
        Xvfb "$DISPLAY" -screen 0 1280x720x24 -nolisten tcp +extension GLX +render -noreset \
            >/media/storage/mapbaker/logs/xvfb.log 2>&1 &
        sleep 1
    fi
}

winpath() {
    # Convert an absolute Unix path to the Wine Z: path. Keeps spaces/backslashes safe.
    local p="$1"
    case "$p" in
        [A-Za-z]:*) printf '%s' "$p" ;;
        *) printf 'Z:%s' "${p//\//\\}" ;;
    esac
}

wine_mapbaker() {
    xvfb_start
    cd /media/storage/projects/wowdecomp/bin/Win
    wine "$MAPBAKER_EXE_WIN" "$@"
}
