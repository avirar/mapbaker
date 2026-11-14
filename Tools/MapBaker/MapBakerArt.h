#ifndef TOOLS_MAPBAKER_MAPBAKERART_H
#define TOOLS_MAPBAKER_MAPBAKERART_H

#include <cstdint>

// Real 2D minimap art (the textured "quads" the original client draws).
//
// The client's minimap does NOT paint flat colors for WMO groups: it tiles each group's
// world bbox into cells and loads a per-cell .blp texture from the MPQ:
//   * cell size: texel = 0.5 world units; each cell texture is a power of two 32..256 px
//     (256 px = 128 world units), chosen per axis from the bbox extent
//     (see CMapObj::GetMinimapQuads, MapObj.cpp)
//   * name:     "<WMO path minus 'World\' and the .WMO extension>_<group:03d>_<x:02d>_<y:02d>.blp"
//   * resolved through Textures\Minimap\md5translate.trs to an md5 file name under
//     Textures\Minimap\ (see CGMinimapFrame::SetupQuad / LoadMD5Names)
//
// This module loads that table and decodes the .blp cells with the engine's CBLPFile
// (DXT1/3/5 -> BGRA), caching per (wmo, group, x, y). MapBakerWorld composites the cells
// at the same verified world->screen mapping as the flat tints.

extern uint8_t MapBakerArtOn;      // 1 = draw the real art cells (replaces the flat tints)
extern uint8_t MapBakerArtAlpha;   // global multiplier on the texture alpha (0-255)

// One cell of a group's minimap texture grid (world-space bbox).
struct MapBakerArtCell {
    int32_t m_x;
    int32_t m_y;
    float m_x0, m_x1;   // world X range
    float m_y0, m_y1;   // world Y range
};

// One decoded cell texture (BGRA, CImVector byte order: b,g,r,a).
struct MapBakerArtImage {
    int32_t m_w;
    int32_t m_h;
    uint8_t* m_px;      // m_w*m_h*4 bytes, or null for a cached "missing" entry
};

// One stock outdoor minimap tile (Textures\Minimap\<dir>\map<tx>_<ty>.blp).
// Same byte order as MapBakerArtImage (b,g,r,a); the caller converts to RGBA.
struct MapBakerTerrainImage {
    int32_t m_w;
    int32_t m_h;
    uint8_t* m_px;
};

// Load Textures\Minimap\md5translate.trs. Safe to call repeatedly (loads once).
// Returns false (and logs once) if the client data has no minimap translation table.
bool MapBakerArtInit();

// Tile a world-space bbox into cells exactly like CMapObj::GetMinimapQuads.
// Returns the number of cells written to `out` (bounded by maxCells).
uint32_t MapBakerArtCellGrid(float x0, float x1, float y0, float y1,
                             MapBakerArtCell* out, uint32_t maxCells);

// Look up + decode one cell texture (cached). m_px is null if the cell has no art.
const MapBakerArtImage* MapBakerArtGet(const char* wmoName, int32_t groupNum,
                                       int32_t cellX, int32_t cellY);

// True when the WMO group has at least one stock minimap cell in md5translate.trs.
// Used by the open-world interior remaster to render only stock interior locations.
bool MapBakerArtHasGroup(const char* wmoName, int32_t groupNum);

// Stock outdoor minimap tile for one ADT: Textures\Minimap\<mapDir>\map<tx>_<ty>.blp.
// The client's outdoor minimap uses these images, not WMO art cells. Returns null when
// the tile has no stock image. Cached.
const MapBakerTerrainImage* MapBakerTerrainGet(const char* mapDirectory,
                                               int32_t tx, int32_t ty);

// Cumulative counters (for the run log / UI): lookups, hits, misses.
void MapBakerArtStats(uint32_t* looked, uint32_t* hit, uint32_t* missing);

#endif
