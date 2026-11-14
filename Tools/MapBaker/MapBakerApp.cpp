#include <windows.h>

#include "MapBakerApp.h"
#include "MapBakerArt.h"
#include "MapBakerPng.h"
#include "MapBakerQuad.h"
#include "MapBakerServe.h"
#include "MapBakerWorld.h"
#include <algorithm>
#include <cmath>
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
#include "WoW/Source/WorldClient/MapObjDef.h"
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

    // --- per-group stock interior art dump (viewer's tunnel x-ray list) --------------
    void JsonEscape(FILE* f, const char* s) {
        if (!s) {
            return;
        }
        for (const unsigned char* p = reinterpret_cast<const unsigned char*>(s); *p; p++) {
            switch (*p) {
                case '"': fputs("\\\"", f); break;
                case '\\': fputs("\\\\", f); break;
                case '\b': fputs("\\b", f); break;
                case '\f': fputs("\\f", f); break;
                case '\n': fputs("\\n", f); break;
                case '\r': fputs("\\r", f); break;
                case '\t': fputs("\\t", f); break;
                default:
                    if (*p < 0x20) {
                        fprintf(f, "\\u%04x", *p);
                    } else {
                        fputc(*p, f);
                    }
            }
        }
    }

    struct DumpCell {
        MapBakerArtCell m_cell;
        const MapBakerArtImage* m_image;
        char m_png[64];
    };

    bool DumpInteriorArt(const char* outDir, int32_t mapId) {
        if (!outDir) {
            return false;
        }
        char artDir[520];
        snprintf(artDir, sizeof(artDir), "%s\\interior-art", outDir);
        CreateDirectoryA(artDir, nullptr);

        char jsonPath[520];
        snprintf(jsonPath, sizeof(jsonPath), "%s\\interior-art.json", outDir);
        FILE* json = fopen(jsonPath, "w");
        if (!json) {
            fprintf(stderr, "MapBaker: could not write %s\n", jsonPath);
            return false;
        }
        fprintf(json, "[\n");
        bool firstGroup = true;
        uint32_t groupCount = 0;
        uint32_t cellCount = 0;

        for (uint32_t i = 0; i < MapBakerQuadCount; i++) {
            const MapBakerQuadEntry& e = MapBakerQuadTable[i];
            if (e.m_smoFlags & 0x8) {
                continue;   // stock interior art only
            }
            if (!MapBakerArtHasGroup(e.m_wmoName, e.m_groupNum)) {
                continue;
            }
            MapBakerArtCell cells[256];
            const uint32_t n = MapBakerArtCellGrid(e.m_lx0, e.m_lx1, e.m_ly0, e.m_ly1,
                                                   cells, 256);
            std::vector<DumpCell> valid;
            for (uint32_t c = 0; c < n; c++) {
                const MapBakerArtImage* image =
                        MapBakerArtGet(e.m_wmoName, e.m_groupNum, cells[c].m_x, cells[c].m_y);
                if (!image || !image->m_px || image->m_w <= 0 || image->m_h <= 0) {
                    continue;
                }
                DumpCell dc;
                dc.m_cell = cells[c];
                dc.m_image = image;
                snprintf(dc.m_png, sizeof(dc.m_png), "a%u_%02d_%02d.png", i,
                         cells[c].m_x, cells[c].m_y);
                char pngPath[520];
                snprintf(pngPath, sizeof(pngPath), "%s\\%s", artDir, dc.m_png);
                const uint32_t w = static_cast<uint32_t>(image->m_w);
                const uint32_t h = static_cast<uint32_t>(image->m_h);
                std::vector<uint8_t> rgba(static_cast<size_t>(w) * h * 4);
                for (size_t p = 0; p < rgba.size(); p += 4) {
                    rgba[p + 0] = image->m_px[p + 2];
                    rgba[p + 1] = image->m_px[p + 1];
                    rgba[p + 2] = image->m_px[p + 0];
                    rgba[p + 3] = image->m_px[p + 3];
                }
                if (!MapBakerPng::WriteRgba(pngPath, static_cast<int32_t>(w),
                                            static_cast<int32_t>(h), rgba.data())) {
                    fprintf(stderr, "MapBaker: could not write art cell %s\n", pngPath);
                    continue;
                }
                valid.push_back(dc);
            }
            if (valid.empty()) {
                continue;
            }

            if (!firstGroup) {
                fprintf(json, ",\n");
            }
            firstGroup = false;
            fprintf(json,
                    "{\"id\":%u,\"wmo\":\"", i);
            JsonEscape(json, e.m_wmoName);
            fprintf(json, "\",\"grp\":%d,\"smo\":\"", e.m_groupNum);
            JsonEscape(json, e.m_smoName);
            fprintf(json,
                    "\",\"z\":[%.1f,%.1f],\"bbox\":[%.2f,%.2f,%.2f,%.2f],"
                    "\"mat\":[%.6f,%.6f,%.6f,%.6f,%.3f,%.3f],\"cells\":[",
                    (double)e.m_z0, (double)e.m_z1,
                    (double)e.m_x0, (double)e.m_x1, (double)e.m_y0, (double)e.m_y1,
                    (double)e.m_ma, (double)e.m_mb, (double)e.m_mc, (double)e.m_md,
                    (double)e.m_mtx, (double)e.m_mty);
            for (size_t c = 0; c < valid.size(); c++) {
                const DumpCell& dc = valid[c];
                if (c) {
                    fputc(',', json);
                }
                fprintf(json,
                        "{\"x\":%d,\"y\":%d,\"x0\":%.4f,\"x1\":%.4f,\"y0\":%.4f,\"y1\":%.4f,"
                        "\"w\":%d,\"h\":%d,\"img\":\"interior-art/%s\"}",
                        dc.m_cell.m_x, dc.m_cell.m_y,
                        (double)dc.m_cell.m_x0, (double)dc.m_cell.m_x1,
                        (double)dc.m_cell.m_y0, (double)dc.m_cell.m_y1,
                        dc.m_image->m_w, dc.m_image->m_h, dc.m_png);
            }
            fprintf(json, "]}");
            groupCount++;
            cellCount += static_cast<uint32_t>(valid.size());
        }
        fprintf(json, "\n]\n");
        fclose(json);
        fprintf(stderr, "MapBaker: interior art dump: %u groups, %u cells -> %s (map %d)\n",
                groupCount, cellCount, jsonPath, mapId);
        return true;
    }

    struct DumpCell3d {
        MapBakerArtCell m_cell;
        float m_x0, m_x1, m_y0, m_y1;   // square world AABB covered by the sprite
        float m_sectionZ;
        char m_full[64];
        char m_masked[64];
    };

    bool DumpInterior3d(const char* outDir, int32_t mapId) {
        if (!outDir) {
            return false;
        }
        const int32_t spritePx = s_config.px;
        char artDir[520];
        snprintf(artDir, sizeof(artDir), "%s\\interior-3d", outDir);
        CreateDirectoryA(artDir, nullptr);
        char jsonPath[520];
        snprintf(jsonPath, sizeof(jsonPath), "%s\\interior-3d.json", outDir);
        FILE* json = fopen(jsonPath, "w");
        if (!json) {
            fprintf(stderr, "MapBaker: could not write %s\n", jsonPath);
            return false;
        }
        fprintf(json, "[\n");
        bool firstGroup = true;
        uint32_t groupCount = 0;
        uint32_t cellCount = 0;
        uint32_t emptyCount = 0;

        for (uint32_t i = 0; i < MapBakerQuadCount; i++) {
            const MapBakerQuadEntry& e = MapBakerQuadTable[i];
            if (e.m_smoFlags & 0x8) {
                continue;
            }
            if (!MapBakerArtHasGroup(e.m_wmoName, e.m_groupNum)) {
                continue;
            }
            MapBakerArtCell cells[256];
            const uint32_t n = MapBakerArtCellGrid(e.m_lx0, e.m_lx1, e.m_ly0, e.m_ly1,
                                                   cells, 256);
            std::vector<DumpCell3d> valid;
            for (uint32_t c = 0; c < n; c++) {
                const MapBakerArtCell& cell = cells[c];
                const MapBakerArtImage* stock =
                        MapBakerArtGet(e.m_wmoName, e.m_groupNum, cell.m_x, cell.m_y);
                if (!stock || !stock->m_px || stock->m_w <= 0 || stock->m_h <= 0) {
                    continue;
                }

                // Cell local corners -> world, then a square AABB around the cell.
                const float lx[4] = { cell.m_x0, cell.m_x1, cell.m_x0, cell.m_x1 };
                const float ly[4] = { cell.m_y0, cell.m_y0, cell.m_y1, cell.m_y1 };
                float wx0 = 1.0e30f, wx1 = -1.0e30f, wy0 = 1.0e30f, wy1 = -1.0e30f;
                for (int32_t k = 0; k < 4; k++) {
                    const float wx = e.m_ma * lx[k] + e.m_mb * ly[k] + e.m_mtx;
                    const float wy = e.m_mc * lx[k] + e.m_md * ly[k] + e.m_mty;
                    if (wx < wx0) wx0 = wx;
                    if (wx > wx1) wx1 = wx;
                    if (wy < wy0) wy0 = wy;
                    if (wy > wy1) wy1 = wy;
                }
                const float cx = (wx0 + wx1) * 0.5f;
                const float cy = (wy0 + wy1) * 0.5f;
                const float half = std::max(wx1 - wx0, wy1 - wy0) * 0.5f * 1.05f;
                const float sx0 = cx - half;
                const float sx1 = cx + half;
                const float sy0 = cy - half;
                const float sy1 = cy + half;
                const float sectionZ = e.m_z0 + 2.0f;

                std::vector<uint8_t> rgba(static_cast<size_t>(spritePx) * spritePx * 4);
                if (!MapBakerWorld::RenderGroupCellSprite(
                            e.m_wmoName, e.m_groupNum, e.m_mtx, e.m_mty,
                            sx0, sx1, sy0, sy1, sectionZ, spritePx, rgba.data())) {
                    fprintf(stderr, "MapBaker 3d: render failed grp %d cell %d,%d\n",
                            e.m_groupNum, cell.m_x, cell.m_y);
                    continue;
                }
                uint32_t covered = 0;
                for (int32_t p = 0; p < spritePx * spritePx; p++) {
                    if (rgba[static_cast<size_t>(p) * 4 + 3] > 1) {
                        covered++;
                    }
                }
                if (!covered) {
                    emptyCount++;
                    continue;
                }

                DumpCell3d dc;
                dc.m_cell = cell;
                dc.m_x0 = sx0;
                dc.m_x1 = sx1;
                dc.m_y0 = sy0;
                dc.m_y1 = sy1;
                dc.m_sectionZ = sectionZ;
                snprintf(dc.m_full, sizeof(dc.m_full), "a%u_%02d_%02d_full.png",
                         i, cell.m_x, cell.m_y);
                snprintf(dc.m_masked, sizeof(dc.m_masked), "a%u_%02d_%02d_masked.png",
                         i, cell.m_x, cell.m_y);
                char fullPath[520];
                char maskedPath[520];
                snprintf(fullPath, sizeof(fullPath), "%s\\%s", artDir, dc.m_full);
                snprintf(maskedPath, sizeof(maskedPath), "%s\\%s", artDir, dc.m_masked);
                if (!MapBakerPng::WriteRgba(fullPath, spritePx, spritePx, rgba.data())) {
                    fprintf(stderr, "MapBaker 3d: could not write %s\n", fullPath);
                    continue;
                }

                // Masked variant: sample the stock cell alpha in cell-local space.
                std::vector<uint8_t> masked = rgba;
                const float ma = e.m_ma, mb = e.m_mb, mc = e.m_mc, md = e.m_md;
                const float det = ma * md - mb * mc;
                if (fabsf(det) > 1.0e-12f) {
                    const float invDet = 1.0f / det;
                    const float cw = cell.m_x1 - cell.m_x0;
                    const float ch = cell.m_y1 - cell.m_y0;
                    for (int32_t py = 0; py < spritePx; py++) {
                        // image row 0 = north (max world X); col 0 = west (max world Y)
                        const float wx = sx1 - (static_cast<float>(py) + 0.5f) /
                                static_cast<float>(spritePx) * (sx1 - sx0);
                        for (int32_t px = 0; px < spritePx; px++) {
                            const float wy = sy1 - (static_cast<float>(px) + 0.5f) /
                                    static_cast<float>(spritePx) * (sy1 - sy0);
                            const float dx = wx - e.m_mtx;
                            const float dy = wy - e.m_mty;
                            const float lx = (md * dx - mb * dy) * invDet;
                            const float ly = (-mc * dx + ma * dy) * invDet;
                            uint8_t mask = 0;
                            if (lx >= cell.m_x0 && lx <= cell.m_x1 &&
                                    ly >= cell.m_y0 && ly <= cell.m_y1 && cw > 0.0f && ch > 0.0f) {
                                const float u = (lx - cell.m_x0) / cw;
                                const float v = 1.0f - (ly - cell.m_y0) / ch;
                                float uC = u * static_cast<float>(stock->m_w - 1);
                                float vC = v * static_cast<float>(stock->m_h - 1);
                                if (uC < 0.0f) uC = 0.0f;
                                if (vC < 0.0f) vC = 0.0f;
                                if (uC > stock->m_w - 1) uC = static_cast<float>(stock->m_w - 1);
                                if (vC > stock->m_h - 1) vC = static_cast<float>(stock->m_h - 1);
                                const int32_t u0 = static_cast<int32_t>(uC);
                                const int32_t v0 = static_cast<int32_t>(vC);
                                const int32_t u1 = (u0 + 1 < stock->m_w) ? u0 + 1 : u0;
                                const int32_t v1 = (v0 + 1 < stock->m_h) ? v0 + 1 : v0;
                                const float fu = uC - static_cast<float>(u0);
                                const float fv = vC - static_cast<float>(v0);
                                const size_t stride = static_cast<size_t>(stock->m_w) * 4;
                                const uint8_t* a00 = stock->m_px + static_cast<size_t>(v0) * stride + static_cast<size_t>(u0) * 4;
                                const uint8_t* a10 = stock->m_px + static_cast<size_t>(v0) * stride + static_cast<size_t>(u1) * 4;
                                const uint8_t* a01 = stock->m_px + static_cast<size_t>(v1) * stride + static_cast<size_t>(u0) * 4;
                                const uint8_t* a11 = stock->m_px + static_cast<size_t>(v1) * stride + static_cast<size_t>(u1) * 4;
                                const float a0 = a00[3] * (1.0f - fu) + a10[3] * fu;
                                const float a1 = a01[3] * (1.0f - fu) + a11[3] * fu;
                                mask = static_cast<uint8_t>(a0 * (1.0f - fv) + a1 * fv + 0.5f);
                            }
                            uint8_t* p = masked.data() +
                                    (static_cast<size_t>(py) * spritePx + px) * 4;
                            p[3] = static_cast<uint8_t>(
                                    (static_cast<uint32_t>(p[3]) * mask + 127u) / 255u);
                        }
                    }
                }
                if (!MapBakerPng::WriteRgba(maskedPath, spritePx, spritePx, masked.data())) {
                    fprintf(stderr, "MapBaker 3d: could not write %s\n", maskedPath);
                    continue;
                }
                valid.push_back(dc);
            }
            if (valid.empty()) {
                continue;
            }

            if (!firstGroup) {
                fprintf(json, ",\n");
            }
            firstGroup = false;
            fprintf(json, "{\"id\":%u,\"wmo\":\"", i);
            JsonEscape(json, e.m_wmoName);
            fprintf(json, "\",\"grp\":%d,\"smo\":\"", e.m_groupNum);
            JsonEscape(json, e.m_smoName);
            fprintf(json, "\",\"cells\":[");
            for (size_t c = 0; c < valid.size(); c++) {
                const DumpCell3d& dc = valid[c];
                if (c) {
                    fputc(',', json);
                }
                fprintf(json,
                        "{\"x\":%d,\"y\":%d,\"world\":[%.3f,%.3f,%.3f,%.3f],"
                        "\"section_z\":%.3f,\"px\":%d,\"full\":\"interior-3d/%s\","
                        "\"masked\":\"interior-3d/%s\"}",
                        dc.m_cell.m_x, dc.m_cell.m_y,
                        (double)dc.m_x0, (double)dc.m_x1, (double)dc.m_y0, (double)dc.m_y1,
                        (double)dc.m_sectionZ, spritePx, dc.m_full, dc.m_masked);
            }
            fprintf(json, "]}");
            groupCount++;
            cellCount += static_cast<uint32_t>(valid.size());
            if ((groupCount % 10) == 0) {
                fprintf(stderr, "MapBaker 3d: %u groups, %u cells, %u empty\n",
                        groupCount, cellCount, emptyCount);
            }
        }
        fprintf(json, "\n]\n");
        fclose(json);
        fprintf(stderr,
                "MapBaker: interior 3D dump: %u groups, %u cells, %u empty -> %s (map %d)\n",
                groupCount, cellCount, emptyCount, jsonPath, mapId);
        return true;
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
        const bool rgba = (s_config.layer == 2 || s_config.layer == 3 ||
                           s_config.layer == 4 || s_config.layer == 6 ||
                           s_config.layer == 7);
        const int32_t channels = rgba ? 4 : 3;
        std::vector<uint8_t> pixels(static_cast<size_t>(px) * px * channels);
        const uint32_t start = OsGetAsyncTimeMs();
        const bool ok = (s_config.layer == 0)
                ? MapBakerWorld::RenderTile(tile.tx, tile.ty, px, pixels.data())
                : MapBakerWorld::RenderTileLayer(tile.tx, tile.ty, px, pixels.data(), rgba, s_config.layer);
        const uint32_t ms = OsGetAsyncTimeMs() - start;
        char name[64];
        SStrPrintf(name, sizeof(name), "tile_%d_%d.png", tile.tx, tile.ty);
        char path[520];
        SStrPrintf(path, sizeof(path), "%s\\%s", s_config.outDir, name);
        const bool wrote = rgba ? MapBakerPng::WriteRgba(path, px, px, pixels.data())
                                : MapBakerPng::Write(path, px, px, pixels.data());
        if (ok && wrote) {
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
        if (s_config.forceWmo) {
            _putenv_s("MAPBAKER_FORCE_WMO", "1");
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
        MapBakerWorld::SetWaterMode(s_config.waterMode);
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
            bool globalWmoGrid = false;
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

            // Instances are global-WMO maps: their WDT has no ADT area table, so the
            // loop above finds nothing. Derive the covered tile rectangle from the
            // bounds of the WMO definitions instead.
            if (count == 0) {
                float minX = 1.0e30f, maxX = -1.0e30f;
                float minY = 1.0e30f, maxY = -1.0e30f;
                int32_t defs = 0;
                for (CMapObjDef* def = CMap::mapObjDefHash.m_fulllist.Head(); def;
                        def = CMap::mapObjDefHash.m_fulllist.Next(def)) {
                    if (!def->m_owner) {
                        continue;
                    }
                    const CAaBox& box = def->m_bbox;
                    if (box.b.x < minX) minX = box.b.x;
                    if (box.t.x > maxX) maxX = box.t.x;
                    if (box.b.y < minY) minY = box.b.y;
                    if (box.t.y > maxY) maxY = box.t.y;
                    defs++;
                }
                if (defs > 0 && maxX > minX && maxY > minY) {
                    const float kAdtSize = 533.33331f;
                    const float kWorldOrigin = 17066.666f;
                    // Tile X runs along world -Y; tile Y runs along world -X.
                    txMin = static_cast<int32_t>(floorf((kWorldOrigin - maxY) / kAdtSize));
                    txMax = static_cast<int32_t>(floorf((kWorldOrigin - minY) / kAdtSize));
                    tyMin = static_cast<int32_t>(floorf((kWorldOrigin - maxX) / kAdtSize));
                    tyMax = static_cast<int32_t>(floorf((kWorldOrigin - minX) / kAdtSize));
                    if (txMin < 0) txMin = 0;
                    if (tyMin < 0) tyMin = 0;
                    if (txMax > 63) txMax = 63;
                    if (tyMax > 63) tyMax = 63;
                    if (txMax >= txMin && tyMax >= tyMin) {
                        count = (txMax - txMin + 1) * (tyMax - tyMin + 1);
                        globalWmoGrid = true;
                        fprintf(stderr, "MapBaker: global WMO bounds X %.1f..%.1f Y %.1f..%.1f -> tiles tx %d..%d ty %d..%d\n",
                                minX, maxX, minY, maxY, txMin, txMax, tyMin, tyMax);
                    }
                }
            }
            printf("MapBaker: map '%s' (id %d): %d valid ADTs, tx %d..%d (%d wide), ty %d..%d (%d tall)\n",
                   s_mapDirectory, s_mapIdNum, count, txMin, txMax, (txMax - txMin + 1),
                   tyMin, tyMax, (tyMax - tyMin + 1));
            if (globalWmoGrid) {
                for (int32_t ty = tyMin; ty <= tyMax; ty++) {
                    for (int32_t tx = txMin; tx <= txMax; tx++) {
                        printf("%d %d\n", tx, ty);
                    }
                }
            } else {
                for (int32_t ty = 0; ty < 64; ty++) {
                    for (int32_t tx = 0; tx < 64; tx++) {
                        if (CMap::m_areaInfo[64 * ty + tx].m_flags & 1) {
                            printf("%d %d\n", tx, ty);
                        }
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
        if (s_config.layer == 4) {
            s_config.quads = true;
            s_config.quadArt = true;
        }
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
        if (s_config.dumpInteriorArt) {
            DumpInteriorArt(s_config.outDir, s_mapIdNum);
            EventPostClose();
            return 1;
        }
        if (s_config.dumpInterior3d) {
            DumpInterior3d(s_config.outDir, s_mapIdNum);
            EventPostClose();
            return 1;
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