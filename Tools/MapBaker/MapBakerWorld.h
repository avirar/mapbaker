#ifndef TOOLS_MAPBAKER_MAPBAKERWORLD_H
#define TOOLS_MAPBAKER_MAPBAKERWORLD_H

#include <cstdint>
#include <cstdio>
#include "Engine/Source/Tempest/C3Vector.h"

// Offline world renderer for MapBaker. Renders a single ADT tile top-down with the real
// client world pipeline (terrain, WMO/mapobj, M2, liquid) into an offscreen render target
// and reads the pixels back. Adapted from origin/mapeditor's EditorWorld offline layer.
namespace MapBakerWorld {
    // Style presets: 0 = "map" (noon, minimal), 1 = "realistic" (map + detail doodads +
    // dynamic shadows + specular).
    enum Preset {
        Preset_Map = 0,
        Preset_Realistic = 1,
    };

    // Load the continent directory for the given map id and set up the offline world.
    bool LoadMap(const char* directory, int32_t mapId);

    // Apply a style preset. Must be called after LoadMap.
    void ApplyPreset(Preset preset);

    // When true, skip WMO/map-object rendering (buildings) so only the ground floor/terrain
    // shows. Used to reproduce the original minimap's "city floor" look for city ADTs.
    void SetNoWmo(bool noWmo);

    // Re-point the map light to the given elevation (degrees above horizon; -1 = engine
    // default noon ~37 deg) after the day/night update. Higher (more vertical) suns flatten
    // roof shading for the original minimap's "city floor" look (bright flat footprints,
    // dark streets).
    void SetLightElevation(float deg);

    // Light colour/intensity mode. 0 = keep Light.dbc colors (client/realistic),
    // 1 = neutral (luminance-preserving gray, per-zone intensity preserved),
    // 2 = desaturate by amount (0..1), 3 = flat (neutral + fixed total intensity),
    // -1 = preset default (map: flat, realistic: client). Covers CMap::s_mapLight and
    // DayNight::m_light0/m_light1 (WMO/M2/doodad lighting). Direction/elevation is
    // still controlled by SetLightElevation.
    void SetLightMode(int32_t mode, float amount);

    // Water colour mode. 0 = keep Light.dbc zone water bands (client/realistic),
    // 1 = fixed map-ocean palette (map preset default), 2 = desaturate the client
    // bands by the light-desat amount, -1 = preset default.
    void SetWaterMode(int32_t mode);

    // Post-render brightness gain for the readback (1.0 = no change).
    void SetGain(float gain);

    // Mark a set of ADTs (64*ty+tx) as "city" tiles: they render with SetCityLightElevation
    // instead of the global light elevation. Used for the selective city-floor look
    // (Stormwind/Orgrimmar/Undercity), which the original minimap applies per-city.
    void SetCityAdts(const uint32_t* indexes, uint32_t count);

    // Light elevation (degrees) used for city ADTs; -1 = disabled (use the global one).
    void SetCityLightElevation(float deg);

    // X-ray "city floor" mode for WMOs (see MapBakerXray.h): 0 = off, 1 = depthoff
    // (depth test off, natural group order), 2 = zsort (depth off, groups sorted by
    // world-Z; lowLast draws the lowest group last = on top), 3 = zinv (Z mirror +
    // culling off). k is the zinv depth remap (default 1500). scope: 0 = all WMOs,
    // 1 = only the city ADTs from SetCityAdts (must be called after it).
    void SetXray(uint32_t mode, uint32_t lowLast, float k, uint32_t scope);

    // Trick/portal guard + WMO table dump (see MapBakerXray.h): skipAbove = skip gated
    // WMOs whose bbox top is this many units above the street AND whose bottom is above
    // the street (0 = off). dump != 0 = print a per-WMO table at first sight of each WMO.
    void SetXrayGuard(float skipAbove, uint32_t dump);

    // Optional file the WMO dump goes to instead of stderr (serve mode). NULL = stderr.
    void SetXrayDumpFile(FILE* file);

    // --- 2D WMO quad overlay ("3D terrain + 2D WMO quads", see MapBakerQuad.h) --------
    // Configure the overlay: on = composite selected quads over the render; wmoUnder =
    // also draw the 3D WMO geometry underneath (0 = terrain-only + quads); alpha and the
    // per-class (exterior/interior) flat tint colors.
    void SetQuadOverlay(uint8_t on, uint8_t wmoUnder, uint8_t alpha,
                        uint8_t extR, uint8_t extG, uint8_t extB,
                        uint8_t intR, uint8_t intG, uint8_t intB);

    // Real minimap art (per-cell Textures\Minimap .blp textures) vs the flat tint
    // fallback (see MapBakerArt.h). alpha = global multiplier on the texture alpha.
    void SetQuadArt(uint8_t on, uint8_t alpha);

    // 1 = draw the WMO groups the client's viewpoint culling skipped (city districts).
    void SetQuadFill(uint8_t on);

    // Per-quad selection (web UI): select/deselect one group by table index.
    void SetQuadSelected(uint32_t index, bool selected);

    // Bulk selection by class: 0 = all off, 1 = all on, 2 = exterior only, 3 = interior only.
    void SetQuadSelectionByClass(uint32_t mode);

    // Collection pass: render the region once so the render hook fills the global quad
    // table (MapBakerQuadTable / MapBakerQuadCount, see MapBakerQuad.h). Returns the count.
    uint32_t CollectQuads(int32_t tx0, int32_t tx1, int32_t ty0, int32_t ty1, uint32_t sizePx);

    // Sample the terrain height at the center of every city ADT (SetCityAdts) into the
    // street reference used by the trick guard. Call after LoadMap + SetCityAdts.
    void SampleStreetHeights();

    // Create (or recreate) the offscreen render target at the given square size in pixels.
    // The size must be a power of two >= 8.
    bool SetRenderSize(uint32_t sizePx);

    // World-space center of an ADT tile (engine coords, origin 17066.666, -X = north, -Y = east).
    void TileWorldCenter(int32_t tx, int32_t ty, float* outCenterX, float* outCenterY);

    // Render ADT tile (tx, ty) top-down into the render target and read the result back into
    // outRgb (sizePx * sizePx * 3 bytes, RGB8, row-major, top row = north). The tile is
    // supersampled at the render target resolution then box-downsampled to sizePx.
    bool RenderTile(int32_t tx, int32_t ty, int32_t sizePx, uint8_t* outRgb);

    // Layer output for the interactive layer viewer.
    // layer: 0 = combined RGB (same as RenderTile), 1 = terrain RGB, 2 = WMO RGBA,
    // 3 = trees RGBA, 4 = quads RGBA. rgba must match the layer's channel count.
    bool RenderTileLayer(int32_t tx, int32_t ty, int32_t sizePx, uint8_t* out, bool rgba, int32_t layer);

    // Read-back self test: clears the render target to a known color and verifies the readback.
    bool SelfTestReadback();
}

#endif
