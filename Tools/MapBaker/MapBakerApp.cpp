#include <windows.h>

#include "MapBakerApp.h"
#include "MapBakerArt.h"
#include "MapBakerPng.h"
#include "MapBakerQuad.h"
#include "MapBakerServe.h"
#include "MapBakerWorld.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "Engine/Source/Base/Base.h"
#include "Engine/Source/Event/Event.h"
#include "Engine/Source/FrameScript/FrameScript.h"
#include "Engine/Source/Gx/CGxDevice/CGxDevice.h"
#include "Engine/Source/Gx/Gx.h"
#include "Engine/Source/Gxu/GxuLight.h"
#include "Engine/Source/Model2/M2Cache.h"
#include "Engine/Source/Model2/M2Scene.h"
#include "Engine/Source/Os/Time.h"
#include "Engine/Source/Os/TimeManager.h"
#include "Engine/Source/Scrn/Scrn.h"
#include "Engine/Source/Services/AsyncFile.h"
#include "Engine/Source/Services/SysMessage.h"
#include "Engine/Source/Services/Texture.h"
#include "Engine/Source/Storm/H/storm.h"
#include "Engine/Source/Tempest/CAaBox.h"
#include "Engine/Source/Tempest/CImVector.h"
#include "WoW/Source/Console/ConsoleClient.h"
#include "WoW/Source/DB/DBClient/DBClient.h"
#include "WoW/Source/WorldClient/Map.h"
#include "WoW/Source/WorldClient/World.h"
#include "WoW/Source/WorldClient/WorldParam.h"
#include "WoW/Source/WorldClient/shadow.h"

namespace {
    MapBakerApp::Config s_config;
    bool s_configured = false;
    char s_mapDirectory[260];
    int32_t s_mapIdNum = -1;
    struct Tile {
        int32_t tx;
        int32_t ty;
    };
    std::vector<Tile> s_tiles;
    uint32_t s_tileIndex = 0;
    bool s_ready = false;
    bool s_selfTestOk = false;
    // No-op M2 project-texture callback (we never project textures onto the map).
    void ProjectTextureCallback(const CAaBox& box, const CImVector& color, int32_t index, void* param,
                                int32_t flags) {
        (void)box;
        (void)color;
        (void)index;
        (void)param;
        (void)flags;
    }
    bool ResolveMap() {
        const char* arg = s_config.mapArg;
        if (!arg) {
            return false;
        }
        char* end = nullptr;
        const long id = strtol(arg, &end, 10);
        const bool numeric = end && end != arg && *end == '\0';
        for (int32_t i = 0; i < g_mapDB.m_numRecords; i++) {
            MapRec* rec = &g_mapDB.m_records[i];
            if ((numeric && rec->m_id == id) || (!numeric && rec->m_directory &&
                                                 !SStrCmpI(rec->m_directory, arg, STORM_MAX_STR))) {
                s_mapIdNum = rec->m_id;
                SStrCopy(s_mapDirectory, rec->m_directory, sizeof(s_mapDirectory));
                return true;
            }
        }
        if (numeric) {
            fprintf(stderr, "MapBaker: map id %ld not in Map.dbc\n", id);
            return false;
        }
        // Fall back to treating the argument as a raw directory with id 0.
        fprintf(stderr, "MapBaker: map '%s' not in Map.dbc, loading as directory with id 0\n", arg);
        s_mapIdNum = 0;
        SStrCopy(s_mapDirectory, arg, sizeof(s_mapDirectory));
        return true;
    }
    // --- quad-table cache ---------------------------------------------------------
    // The collected table depends only on the map + client data (not on resolution), so a
    // continent-sized collection can be done once and reused by every later bake, by the
    // serve, and by all parallel workers.
    bool LoadQuadCache(const char* path) {
        FILE* f = fopen(path, "rb");
        if (!f) {
            return false;
        }
        char magic[8];
        uint32_t version = 0, count = 0, entrySize = 0;
        bool ok = fread(magic, 1, sizeof(magic), f) == sizeof(magic) &&
                  memcmp(magic, "MBQUAD02", 8) == 0 &&
                  fread(&version, sizeof(version), 1, f) == 1 && version == 2 &&
                  fread(&count, sizeof(count), 1, f) == 1 && count <= MapBakerQuadCap &&
                  fread(&entrySize, sizeof(entrySize), 1, f) == 1 &&
                  entrySize == sizeof(MapBakerQuadEntry) &&
                  (count == 0 ||
                   fread(MapBakerQuadTable, sizeof(MapBakerQuadEntry), count, f) == count);
        fclose(f);
        if (!ok) {
            fprintf(stderr, "MapBaker: quad cache '%s' is unusable (version/layout mismatch)\n", path);
            return false;
        }
        MapBakerQuadCount = count;
        fprintf(stderr, "MapBaker: quad cache loaded: %u groups from %s\n", count, path);
        return true;
    }

    bool SaveQuadCache(const char* path) {
        FILE* f = fopen(path, "wb");
        if (!f) {
            fprintf(stderr, "MapBaker: could not write quad cache '%s'\n", path);
            return false;
        }
        const uint32_t version = 2;
        const uint32_t count = MapBakerQuadCount;
        const uint32_t entrySize = sizeof(MapBakerQuadEntry);
        bool ok = fwrite("MBQUAD02", 1, 8, f) == 8 &&
                  fwrite(&version, sizeof(version), 1, f) == 1 &&
                  fwrite(&count, sizeof(count), 1, f) == 1 &&
                  fwrite(&entrySize, sizeof(entrySize), 1, f) == 1 &&
                  (count == 0 ||
                   fwrite(MapBakerQuadTable, sizeof(MapBakerQuadEntry), count, f) == count);
        fclose(f);
        if (ok) {
            fprintf(stderr, "MapBaker: quad cache saved: %u groups to %s\n", count, path);
        }
        return ok;
    }

    void BuildTileList() {
        s_tiles.clear();
        if (s_config.tileListFile) {
            // Parallel worker assignment: "tx ty" per line, '#' comments and blanks ignored.
            FILE* f = fopen(s_config.tileListFile, "r");
            if (!f) {
                fprintf(stderr, "MapBaker: cannot open tile list '%s'\n", s_config.tileListFile);
                return;
            }
            char line[256];
            while (fgets(line, sizeof(line), f)) {
                char* p = line;
                while (*p == ' ' || *p == '\t') {
                    p++;
                }
                if (*p == '#' || *p == '\r' || *p == '\n' || !*p) {
                    continue;
                }
                int tx = 0, ty = 0;
                if (sscanf(p, "%d %d", &tx, &ty) == 2) {
                    s_tiles.push_back(Tile{tx, ty});
                }
            }
            fclose(f);
            return;
        }
        if (s_config.allTiles) {
            // Render every valid ADT in the continent.
            for (int32_t ty = 0; ty < 64; ty++) {
                for (int32_t tx = 0; tx < 64; tx++) {
                    const int32_t index = 64 * ty + tx;
                    if (CMap::m_areaInfo[index].m_flags & 1) {
                        s_tiles.push_back(Tile{tx, ty});
                    }
                }
            }
            return;
        }
        for (int32_t ty = s_config.ty0; ty <= s_config.ty1; ty++) {
            for (int32_t tx = s_config.tx0; tx <= s_config.tx1; tx++) {
                s_tiles.push_back(Tile{tx, ty});
            }
        }
    }
    bool RenderOneTile() {
        if (!s_ready || s_tileIndex >= s_tiles.size()) {
            return false;
        }
        const Tile& tile = s_tiles[s_tileIndex];
        const int32_t px = s_config.px;
        if (s_config.skipExisting) {
            char existing[520];
            SStrPrintf(existing, sizeof(existing), "%s\\tile_%d_%d.png", s_config.outDir, tile.tx, tile.ty);
            FILE* f = fopen(existing, "rb");
            if (f) {
                fclose(f);
                s_tileIndex++;
                return true;   // resumed: already baked
            }
        }
        std::vector<uint8_t> rgb(static_cast<size_t>(px) * px * 3);
        const uint32_t start = OsGetAsyncTimeMs();
        const bool ok = MapBakerWorld::RenderTile(tile.tx, tile.ty, px, rgb.data());
        const uint32_t ms = OsGetAsyncTimeMs() - start;
        char name[64];
        SStrPrintf(name, sizeof(name), "tile_%d_%d.png", tile.tx, tile.ty);
        char path[520];
        SStrPrintf(path, sizeof(path), "%s\\%s", s_config.outDir, name);
        if (ok && MapBakerPng::Write(path, px, px, rgb.data())) {
            printf("MapBaker: tile (%d, %d) -> %s (%u ms)\n", tile.tx, tile.ty, name, ms);
        } else {
            fprintf(stderr, "MapBaker: failed to render/write tile (%d, %d)\n", tile.tx, tile.ty);
        }
        s_tileIndex++;
        return true;
    }
    int32_t RenderCallback(const void* data, void* param) {
        (void)data;
        (void)param;
        if (!s_ready) {
            return 1;
        }
        if (s_tileIndex >= s_tiles.size()) {
            printf("MapBaker: done, %u tiles written to %s\n", s_tiles.size(), s_config.outDir);
            if (s_config.quads && s_config.quadArt) {
                uint32_t looked = 0, hit = 0, missing = 0;
                MapBakerArtStats(&looked, &hit, &missing);
                fprintf(stderr, "MapBaker: minimap art cells: %u looked up, %u decoded, %u missing\n",
                        looked, hit, missing);
            }
            EventPostClose();
            return 1;
        }
        RenderOneTile();
        return 1;
    }}namespace MapBakerApp {
    void SetConfig(const Config& config) {
        s_config = config;
        s_configured = true;
    }
    int32_t InitializeEngine(const void* data, void* param) {
        (void)data;
        (void)param;
        if (!s_configured) {
            fprintf(stderr, "MapBaker: no configuration provided\n");
            EventPostClose();
            return 1;
        }
        BaseInitializeContext();
        TextureInitialize();
        ScrnInitialize(0);
        ConsoleScreenInitialize(nullptr);
        M2Initialize(0, 0);
        EventRegister(EVENT_ID_POLL, AsyncFileReadPollHandler);
        SysMsgInitialize();
        ClientDBInitialize();
        FrameScript_Initialize(0);
        if (!ResolveMap()) {
            EventPostClose();
            return 1;
        }
        CWorldParam::Initialize();
        CWorld::Initialize();
        World::s_m2Scene->SetProjectTextureCallback(ProjectTextureCallback, nullptr);
        ShadowInit();
        GxuLightInitialize();
        GxuLightSetGridSize(16.665001f);
        // Load the world and apply the preset.
        if (!MapBakerWorld::LoadMap(s_mapDirectory, s_mapIdNum)) {
            fprintf(stderr, "MapBaker: failed to load map '%s' (id %d)\n", s_mapDirectory, s_mapIdNum);
            EventPostClose();
            return 1;
        }
        MapBakerWorld::SetNoWmo(s_config.noWmo);
        MapBakerWorld::SetLightElevation(s_config.lightElev);
        MapBakerWorld::SetLightMode(s_config.lightMode, s_config.lightDesat);
        MapBakerWorld::SetGain(s_config.gain);
        MapBakerWorld::SetCityLightElevation(s_config.cityElev);
        if (s_config.cityFile) {
            // Load the city ADT list ("tx ty" per line) and mark those tiles for the flatter
            // city sun. The original minimap applies this selectively to Stormwind/Orgrimmar/
            // Undercity.
            uint32_t cityAdts[4096];
            uint32_t cityCount = 0;
            FILE* f = fopen(s_config.cityFile, "r");
            if (f) {
                int tx = 0, ty = 0;
                while (fscanf(f, "%d %d", &tx, &ty) == 2) {
                    if (tx >= 0 && tx < 64 && ty >= 0 && ty < 64 && cityCount < 4096) {
                        cityAdts[cityCount++] = static_cast<uint32_t>(64 * ty + tx);
                    }
                }
                fclose(f);
                MapBakerWorld::SetCityAdts(cityAdts, cityCount);
                fprintf(stderr, "MapBaker: %u city ADTs from %s (elev %.0f deg)\n",
                        cityCount, s_config.cityFile, s_config.cityElev);
            } else {
                fprintf(stderr, "MapBaker: could not open city file '%s'\n", s_config.cityFile);
            }
        }
        // X-ray city-floor mode (after the city list, so the 'city' gate can use it).
        MapBakerWorld::SetXray(s_config.xrayMode, s_config.xrayLowLast, s_config.xrayK, s_config.xrayScope);
        MapBakerWorld::SetXrayGuard(s_config.xraySkipAbove, s_config.xrayDump);
        if (s_config.xrayMode != 0 || s_config.xraySkipAbove > 0.0f) {
            // The trick guard needs a street-height reference for the gated ADTs.
            MapBakerWorld::SampleStreetHeights();
        }
        if (s_config.dumpTiles) {
            // Dump the ADT grid from the WDT area table and exit (no rendering needed).
            int32_t count = 0, txMin = 64, txMax = -1, tyMin = 64, tyMax = -1;
            for (int32_t ty = 0; ty < 64; ty++) {
                for (int32_t tx = 0; tx < 64; tx++) {
                    if (CMap::m_areaInfo[64 * ty + tx].m_flags & 1) {
                        count++;
                        if (tx < txMin) txMin = tx;
                        if (tx > txMax) txMax = tx;
                        if (ty < tyMin) tyMin = ty;
                        if (ty > tyMax) tyMax = ty;
                    }
                }
            }
            printf("MapBaker: map '%s' (id %d): %d valid ADTs, tx %d..%d (%d wide), ty %d..%d (%d tall)\n",
                   s_mapDirectory, s_mapIdNum, count, txMin, txMax, (txMax - txMin + 1),
                   tyMin, tyMax, (tyMax - tyMin + 1));
            for (int32_t ty = 0; ty < 64; ty++) {
                for (int32_t tx = 0; tx < 64; tx++) {
                    if (CMap::m_areaInfo[64 * ty + tx].m_flags & 1) {
                        printf("%d %d\n", tx, ty);
                    }
                }
            }
            EventPostClose();
            return 1;
        }
        MapBakerWorld::ApplyPreset(
            (s_config.preset && !strcmp(s_config.preset, "realistic")) ? MapBakerWorld::Preset_Realistic
                                                                       : MapBakerWorld::Preset_Map);
        // Render target size = tile resolution * supersampling.
        const uint32_t rtSize = static_cast<uint32_t>(s_config.px) * static_cast<uint32_t>(s_config.ssaa);
        if (!MapBakerWorld::SetRenderSize(rtSize)) {
            EventPostClose();
            return 1;
        }
        s_selfTestOk = MapBakerWorld::SelfTestReadback();
        if (!s_selfTestOk) {
            fprintf(stderr, "MapBaker: render-target readback self-test FAILED; aborting\n");
            EventPostClose();
            return 1;
        }
        printf("MapBaker: readback self-test OK, map '%s' (id %d), preset '%s', px=%d ssaa=%d rt=%u\n",
               s_mapDirectory, s_mapIdNum, s_config.preset, s_config.px, s_config.ssaa, rtSize);
        // Headless: the device window (a small windowed one, forced by the MAPBAKER
        // guard in ConsoleDeviceInitialize) is never presented to — we render into an
        // offscreen render target. Hide it so the baker shows no window at all.
        {
            void* win = g_theGxDevicePtr->DeviceWindow();
            if (win) {
                ::ShowWindow(static_cast<HWND>(win), SW_HIDE);
            }
        }
        // MAPBAKER_FIXED_TERRAIN=1: force the fixed-function terrain path. With the
        // vertex-shader path, per-chunk RenderSetup skips the world-matrix set and the
        // terrain VS gets its transform from the const block; with it off, RenderSetup
        // sets a fresh per-chunk world matrix (T(chunkVec - cam)) for the device stack.
        // Diagnostic for the serve-mode Y-freeze: if fixed terrain is correct in serve
        // while the VS path is not, the bug lives in the VS-path state flow.
        if (const char* env = getenv("MAPBAKER_FIXED_TERRAIN")) {
            CMap::enableTerrainShaderVertex = 0;
            fprintf(stderr, "MapBaker: MAPBAKER_FIXED_TERRAIN='%s' -> fixed-function terrain\n", env);
        }
        if (s_config.serve) {
            // Interactive mode: the scene stays loaded and MapBakerServe re-renders the
            // region on demand (local web tuner). It registers its own idle callback.
            if (!MapBakerServe::Start(s_config)) {
                fprintf(stderr, "MapBaker: serve mode failed to start (port %d in use?)\n", s_config.port);
                EventPostClose();
                return 1;
            }
            return 1;
        }
        BuildTileList();
        if (s_tiles.empty()) {
            fprintf(stderr, "MapBaker: no tiles to render\n");
            EventPostClose();
            return 1;
        }
        printf("MapBaker: %u tiles queued\n", s_tiles.size());
        // 2D WMO quad overlay (batch): one collection pass fills the per-group table via
        // the World.cpp render hook, then the selected groups are composited as flat
        // footprints over each tile (see MapBakerQuad.h). The collection re-renders the
        // region (pixels discarded), so expect roughly 2x the render time with --quads.
        if (s_config.quads) {
            int32_t cx0 = 64, cx1 = -1, cy0 = 64, cy1 = -1;
            for (const Tile& t : s_tiles) {
                if (t.tx < cx0) cx0 = t.tx;
                if (t.tx > cx1) cx1 = t.tx;
                if (t.ty < cy0) cy0 = t.ty;
                if (t.ty > cy1) cy1 = t.ty;
            }
            // The table depends only on map + client data, so a cache makes repeat bakes
            // (and every parallel worker) skip the collection entirely.
            bool haveTable = (s_config.quadCache != nullptr &&
                              LoadQuadCache(s_config.quadCache));
            if (!haveTable) {
                // Collect at a modest size: the pass only needs the render hook to visit
                // every WMO, so a continent-sized region does not pay the full tile
                // resolution twice.
                const uint32_t collectPx = (rtSize > 512u) ? 512u : rtSize;
                MapBakerWorld::CollectQuads(cx0, cx1, cy0, cy1, collectPx);
                if (s_config.quadCache) {
                    SaveQuadCache(s_config.quadCache);
                }
            }
            MapBakerWorld::SetQuadSelectionByClass(s_config.quadSel);
            MapBakerWorld::SetQuadOverlay(1, s_config.quadWmo ? 1 : 0,
                                          static_cast<uint8_t>(s_config.quadAlpha * 255.0f + 0.5f),
                                          MapBakerQuadExtColor[0], MapBakerQuadExtColor[1],
                                          MapBakerQuadExtColor[2],
                                          MapBakerQuadIntColor[0], MapBakerQuadIntColor[1],
                                          MapBakerQuadIntColor[2]);
            MapBakerWorld::SetQuadArt(s_config.quadArt ? 1 : 0, 255);
            MapBakerWorld::SetQuadFill(s_config.quadFill ? 1 : 0);
            fprintf(stderr, "MapBaker: quad overlay on (%u groups, %s, wmo-under=%d alpha=%.2f sel=%s)\n",
                    MapBakerQuadCount, s_config.quadArt ? "2D minimap art" : "flat tints",
                    (int)s_config.quadWmo, s_config.quadAlpha,
                    s_config.quadSel == 0 ? "none" : s_config.quadSel == 1 ? "all"
                    : s_config.quadSel == 3 ? "int" : "ext");
            if (s_config.quadDump) {
                FILE* f = fopen(s_config.quadDump, "w");
                if (!f) {
                    fprintf(stderr, "MapBaker: could not open quad dump '%s'\n", s_config.quadDump);
                } else {
                    for (uint32_t i = 0; i < MapBakerQuadCount; i++) {
                        const MapBakerQuadEntry& e = MapBakerQuadTable[i];
                        fprintf(f, "%u\t%s\tgrp=%d\tsmo=%s\text=%d\tsel=%d\tsmoFlags=0x%x\t"
                                "x=[%.3f..%.3f]\ty=[%.3f..%.3f]\tz=[%.1f..%.1f]\t"
                                "lx=[%.3f..%.3f]\tly=[%.3f..%.3f]\t"
                                "mat=[%.5f %.5f %.5f %.5f]\tt=[%.3f %.3f]\t"
                                "mogi=0x%x\tbatches=%u/%u/%u/%u\tcv=%u\n",
                                i, e.m_wmoName, e.m_groupNum, e.m_smoName,
                                (e.m_smoFlags & 0x8) ? 1 : 0, e.m_selected, e.m_smoFlags,
                                (double)e.m_x0, (double)e.m_x1,
                                (double)e.m_y0, (double)e.m_y1,
                                (double)e.m_z0, (double)e.m_z1,
                                (double)e.m_lx0, (double)e.m_lx1,
                                (double)e.m_ly0, (double)e.m_ly1,
                                (double)e.m_ma, (double)e.m_mb, (double)e.m_mc, (double)e.m_md,
                                (double)e.m_mtx, (double)e.m_mty,
                                e.m_mogiFlags, e.m_batchTrans, e.m_batchInt, e.m_batchExt,
                                e.m_batchTotal, e.m_hasColorVerts);
                    }
                    fclose(f);
                    fprintf(stderr, "MapBaker: quad table dumped to %s\n", s_config.quadDump);
                }
            }
        }
        s_ready = true;
        EventRegister(EVENT_ID_IDLE, RenderCallback);
        return 1;
    }
    int32_t DestroyEngine(const void* data, void* param) {
        (void)data;
        (void)param;
        EventUnregister(EVENT_ID_POLL, AsyncFileReadPollHandler);
        if (s_config.serve) {
            MapBakerServe::Stop();
        }
        return 1;
    }}