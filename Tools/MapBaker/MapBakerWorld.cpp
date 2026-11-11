#include "MapBakerWorld.h"
#include "MapBakerArt.h"
#include "MapBakerXray.h"
#include "MapBakerQuad.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "Engine/Source/Gx/CGxDevice/CGxDevice.h"
#include "Engine/Source/Gx/CGxTexFlags.h"
#include "Engine/Source/Gx/Gx.h"
#include "Engine/Source/Gxu/Gxu.h"
#include "Engine/Source/Model2/M2Light.h"
#include "Engine/Source/Model2/M2Scene.h"
#include "Engine/Source/ShaderEffect/ShaderEffect.h"
#include "Engine/Source/Services/Texture.h"
#include "Engine/Source/Storm/H/storm.h"
#include "Engine/Source/Tempest/C44Matrix.h"
#include "Engine/Source/Tempest/CiRect.h"
#include "Engine/Source/Tempest/CRect.h"
#include "Engine/Source/Os/Time.h"
#include "Engine/Source/Os/TimeManager.h"

#include "WoW/Source/LiquidSystem/Liquid.h"
#include "WoW/Source/WorldClient/CPortalView.h"
#include "WoW/Source/WorldClient/CWorldOcclusion.h"
#include "WoW/Source/WorldClient/DayNight.h"
#include "WoW/Source/WorldClient/DayNight__DNInfo.h"
#include "WoW/Source/WorldClient/DetailDoodad.h"
#include "WoW/Source/WorldClient/FrameTime.h"
#include "WoW/Source/WorldClient/Map.h"
#include "WoW/Source/WorldClient/MapArea.h"
#include "WoW/Source/WorldClient/MapChunk.h"
#include "WoW/Source/WorldClient/MapLight.h"
#include "WoW/Source/WorldClient/MapObj.h"
#include "WoW/Source/WorldClient/MapObjDef.h"
#include "WoW/Source/WorldClient/MapRenderChunk.h"
#include "WoW/Source/WorldClient/SortTable.h"
#include "WoW/Source/WorldClient/World.h"
#include "WoW/Source/WorldClient/WorldScene.h"
#include "WoW/Source/WorldClient/WorldView.h"

// X-ray knobs (defined here; declared extern in MapBakerXray.h, read by the engine's
// RenderMapObjDefGroups). Global scope so the engine's references link.
uint32_t MapBakerXrayMode = 0;
uint32_t MapBakerXrayLowLast = 1;
float MapBakerXrayK = 1500.0f;
uint32_t MapBakerXrayScope = 1;
uint8_t MapBakerXrayGate[4096] = {};
float MapBakerXrayStreetH[4096] = {};
float MapBakerXraySkipAbove = 0.0f;
uint32_t MapBakerXrayDump = 0;
FILE* MapBakerXrayDumpFile = nullptr;
int32_t MapBakerDebugTileTag = -1;

// 2D WMO quad overlay (defined here; declared extern in MapBakerQuad.h, read by the
// World.cpp render hook for collection and by MapBakerWorld::RenderTile for compositing).
MapBakerQuadEntry MapBakerQuadTable[MAPBAKER_QUAD_MAX] = {};
uint32_t MapBakerQuadCount = 0;
uint32_t MapBakerQuadCap = MAPBAKER_QUAD_MAX;
uint8_t MapBakerQuadCollect = 0;
uint8_t MapBakerQuadOn = 0;
uint8_t MapBakerQuadWmoUnder = 0;
uint8_t MapBakerQuadFill = 1;
uint8_t MapBakerFlatWaterLight = 0;
int32_t MapBakerWaitTextures = 0;
int32_t MapBakerM2Filter = 0;

// Transient effect models (portals, fog, light rays) are excluded from map layers: in an
// RGBA capture their additive black backgrounds become opaque black planes.
bool MapBakerM2IsEffect(const char* fileName) {
    if (!fileName || !fileName[0]) {
        return false;
    }

    const char* base = fileName;
    for (const char* p = fileName; *p; p++) {
        if (*p == '\\' || *p == '/') {
            base = p + 1;
        }
    }

    char lower[260];
    size_t i = 0;
    for (; base[i] && i + 1 < sizeof(lower); i++) {
        lower[i] = static_cast<char>(tolower(static_cast<unsigned char>(base[i])));
    }
    lower[i] = '\0';

    // Transient spell/portal/fog effects: additive planes that turn into opaque black
    // holes in a transparent RGBA layer. Map layers should contain static art only.
    static const char* effects[] = {
        "castingcircle", "lightray", "spirits", "fog", "energyfall", "energyfalls",
        "mageportal", "portal", "fx", "particle", "smoke", "spark", "glow", "beam",
        "void", "effect", "vfx"
    };
    for (size_t e = 0; e < sizeof(effects) / sizeof(effects[0]); e++) {
        if (strstr(lower, effects[e])) {
            return true;
        }
    }
    return false;
}

// "Common" tree models for the trees layer. World/unique trees are denied so they stay
// with the WMO/props layer (or are handled by the WMO toggle).
bool MapBakerM2IsTree(const char* fileName) {
    if (!fileName || !fileName[0]) {
        return false;
    }

    // Use the model basename, not the whole path: directory names such as "trees"
    // and zone names such as Silverpine would otherwise classify bushes, logs,
    // rocks and docks as trees.
    const char* base = fileName;
    for (const char* p = fileName; *p; p++) {
        if (*p == '\\' || *p == '/') {
            base = p + 1;
        }
    }

    char lower[260];
    size_t i = 0;
    for (; base[i] && i + 1 < sizeof(lower); i++) {
        lower[i] = static_cast<char>(tolower(static_cast<unsigned char>(base[i])));
    }
    lower[i] = '\0';

    static const char* deny[] = {
        "worldtree", "world_tree", "teldrassil", "nordrassil", "vordrassil",
        "ghanir", "treeoflife", "tree_of_life", "ancient", "guardian",
        "bush", "log", "mushroom", "trunk", "stump", "sapling", "spine",
        "rock", "coral", "banner", "hut", "lamp", "vfx", "fallen",
        "fruit", "root", "branch", "vine", "crop", "grass", "flower", "bridge",
        "clover", "shrub"
    };
    for (size_t d = 0; d < sizeof(deny) / sizeof(deny[0]); d++) {
        if (strstr(lower, deny[d])) {
            return false;
        }
    }

    // Most Warcraft tree models carry "tree" in the basename.
    if (strstr(lower, "tree")) {
        return true;
    }

    // Species-only names (pine01, oak02, ...). Match whole alphanumeric tokens so
    // "silverpinedocks" / "spine" / "fire" / "helmet" do not match.
    static const char* species[] = {
        "pine", "fir", "spruce", "oak", "birch", "elm", "willow", "cedar",
        "aspen", "poplar", "cypress", "conifer", "maple", "redwood",
        "eucalyptus", "larch", "hemlock", "sycamore", "beech", "alder",
        "juniper", "sequoia", "yew"
    };
    const char* p = lower;
    while (*p) {
        while (*p && !isalnum(static_cast<unsigned char>(*p))) p++;
        const char* t0 = p;
        while (*p && isalnum(static_cast<unsigned char>(*p))) p++;
        const size_t n = static_cast<size_t>(p - t0);
        for (size_t k = 0; k < sizeof(species) / sizeof(species[0]); k++) {
            if (strlen(species[k]) == n && strncmp(t0, species[k], n) == 0) {
                return true;
            }
        }
    }

    // A few palm-family names use a compound token (sholazarpalm_*).
    return strstr(lower, "palm") != nullptr;
}
uint8_t MapBakerQuadExtColor[3] = { 232, 224, 200 }; // warm tint (exterior)
uint8_t MapBakerQuadIntColor[3] = { 200, 214, 236 }; // cool tint (interior)
uint8_t MapBakerQuadAlpha = 160;

namespace {
    // ADT size in world units and the world-space origin.
    const float kAdtSize = 533.33331f;
    const float kWorldOrigin = 17066.666f;

    // A fixed animation time keeps renders deterministic (resume/compare).
    const float kFixedTimeSec = 86400.0f * 0.5f;

    CGxTex* s_rt = nullptr;
    uint32_t s_rtSize = 0;

    // Skip WMO/map-object rendering (buildings) -> ground-floor look for city tiles.
    bool s_noWmo = false;

    // When the 2D WMO quad overlay is on and "wmo under quads" is off, suppress the 3D WMO
    // draw entirely so only terrain + flat quads show (the original minimap look). Set by
    // MapBakerWorld::SetQuadOverlay. Independent of s_noWmo (the explicit --no-wmo knob).
    bool s_quadWmoSuppress = false;

    // Map-light elevation (degrees above the horizon). -1 = engine default (noon ~37 deg).
    // A higher (more vertical) sun flattens roof shading: horizontal surfaces light
    // uniformly and vertical surfaces (walls) fall to ambient only. The original minimap's
    // "city floor" look (bright flat building footprints, dark streets) needs a sun well
    // above the noon angle; 90 deg is fully vertical.
    float s_lightElev = -1.0f;

    // Terrain light color mode: -1 = preset default, 0 = client Light.dbc colors,
    // 1 = neutral (luminance gray), 2 = partially desaturated by s_lightDesat.
    int32_t s_lightMode = -1;
    float s_lightDesat = 1.0f;

    // Water colour mode: -1 = preset default, 0 = client DBC bands, 1 = fixed map
    // palette, 2 = desaturate the client bands by s_lightDesat.
    int32_t s_waterMode = -1;

    // Post-render brightness gain applied to the readback (1.0 = no change). The baked
    // minimap art is brighter than a direct render; a ~1.1-1.2 gain matches its tone.
    float s_gain = 1.0f;

    // City ADT selection: the original minimap's flat "city floor" look is applied
    // selectively (Stormwind/Orgrimmar/Undercity), so the sun elevation can be raised
    // for those ADTs only.
    bool s_cityMap[4096] = {};
    float s_cityElev = -1.0f;

    // No-op texture callback required by GxTexCreate; the render target is never locked.
    void RtUserFunc(EGxTexCommand command, uint32_t width, uint32_t height, uint32_t depth,
                    uint32_t mipLevel, void* userArg, uint32_t& texelStrideInBytes,
                    const void*& texels) {
        (void)command;
        (void)width;
        (void)height;
        (void)depth;
        (void)mipLevel;
        (void)userArg;
        texelStrideInBytes = 4 * width;
        texels = nullptr;
    }

    void ApplyFadeDistances() {
        // Keep every M2/WMO fade distance huge so nothing fades out for a top-down camera
        // that sits far above the terrain. Re-applied per tile in case ScaleFadeDist resets it.
        CWorldScene::s_fadeDistDefault[0] = 1e7f;
        CWorldScene::s_fadeDistDefault[1] = 1e7f;
        CWorldScene::s_fadeDistDefault[2] = 1e7f;
        CWorldScene::s_fadeDistDefault[3] = 1e7f;
        CWorldScene::s_fadeDistDefault[4] = 1e7f;
        CWorldScene::ScaleFadeDist(1.0f);
    }

    C3Vector DesaturateLightColor(const C3Vector& color, float amount) {
        // Luminance-preserving desaturation. C3Vector is (r,g,b) in the light
        // structures (World.cpp packs ambient.r/g/b directly).
        const float luma = 0.2126f * color.x + 0.7152f * color.y + 0.0722f * color.z;
        return C3Vector(color.x + (luma - color.x) * amount,
                        color.y + (luma - color.y) * amount,
                        color.z + (luma - color.z) * amount);
    }

    CImVector DesaturateImVector(const CImVector& color, float amount) {
        const float luma = 0.2126f * color.r + 0.7152f * color.g + 0.0722f * color.b;
        const auto mix = [&](uint8_t channel) {
            const float v = channel + (luma - channel) * amount;
            return static_cast<uint8_t>(v < 0.0f ? 0.0f : (v > 255.0f ? 255.0f : v));
        };
        CImVector out;
        out.a = color.a;
        out.r = mix(color.r);
        out.g = mix(color.g);
        out.b = mix(color.b);
        return out;
    }

    void SetFlatDnLight(DNLightState& light, uint8_t ambient, uint8_t diffuse) {
        light.m_ambient.a = 0xFF;
        light.m_ambient.r = ambient;
        light.m_ambient.g = ambient;
        light.m_ambient.b = ambient;
        light.m_diffuse.a = 0xFF;
        light.m_diffuse.r = diffuse;
        light.m_diffuse.g = diffuse;
        light.m_diffuse.b = diffuse;
        // WMO mode 2 uses m_mid0 as diffuse and m_mid1 as ambient.
        light.m_mid0 = light.m_diffuse;
        light.m_mid1 = light.m_ambient;
    }

    // DayNight::DNLightState is the light source WMOs, M2s and doodads use directly
    // (CMapObjGroup::SetLighting reads m_light1, World.cpp:SetEmissiveAmbient reads
    // m_light1.m_ambient). It is rebuilt from the camera's zone every tile, so leaving
    // it untouched makes the same WMO change colour at an ADT border.
    void NeutralizeDnLight(DNLightState& light, float amount) {
        light.m_ambient = DesaturateImVector(light.m_ambient, amount);
        light.m_diffuse = DesaturateImVector(light.m_diffuse, amount);
        light.m_mid0 = DesaturateImVector(light.m_mid0, amount);
        light.m_mid1 = DesaturateImVector(light.m_mid1, amount);
    }

    void ApplyLightColorMode() {
        if (s_lightMode == 0 || !CMap::s_mapLight) {
            return; // client Light.dbc colors
        }

        // Flat mode: ignore the zone light entirely. Scaling the zone's ambient/diffuse
        // pair to a fixed sum is not enough: zones split that sum differently, and WMOs
        // shade with ambient + diffuse*NdotL, so the same WMO still changes at an ADT
        // border. Use fixed ambient/diffuse values for both the terrain light and the
        // DayNight lights WMOs/M2s read directly.
        const float kFlatAmbient = 0.40f;
        const float kFlatDiffuse = 0.56f;
        const uint8_t kFlatAmbientByte = 102;
        const uint8_t kFlatDiffuseByte = 143;

        const bool flat = s_lightMode == 3;
        float amount = (s_lightMode == 1 || flat) ? 1.0f : s_lightDesat;
        if (!flat && amount <= 0.0f) {
            return;
        }
        if (amount > 1.0f) {
            amount = 1.0f;
        }

        CM2Light& light = CMap::s_mapLight->m_light;
        if (flat) {
            light.m_ambColor = C3Vector(kFlatAmbient, kFlatAmbient, kFlatAmbient);
            light.m_dirColor = C3Vector(kFlatDiffuse, kFlatDiffuse, kFlatDiffuse);
            light.m_specColor = C3Vector(0.0f, 0.0f, 0.0f);
        } else {
            light.m_ambColor = DesaturateLightColor(light.m_ambColor, amount);
            light.m_dirColor = DesaturateLightColor(light.m_dirColor, amount);
            light.m_specColor = DesaturateLightColor(light.m_specColor, amount);
        }

        DayNight::DNInfo* dn = DayNight::GetInfo();
        if (dn) {
            if (flat) {
                SetFlatDnLight(dn->m_light0, kFlatAmbientByte, kFlatDiffuseByte);
                SetFlatDnLight(dn->m_light1, kFlatAmbientByte, kFlatDiffuseByte);
            } else {
                NeutralizeDnLight(dn->m_light0, amount);
                NeutralizeDnLight(dn->m_light1, amount);
            }
        }

        if (getenv("MAPBAKER_DEBUG_LIGHT")) {
            static int s_printed = 0;
            if (s_printed < 4) {
                s_printed++;
                fprintf(stderr,
                        "MapBaker light: mode=%d amount=%.2f amb=(%.3f,%.3f,%.3f) "
                        "dir=(%.3f,%.3f,%.3f) dn1_amb=(%d,%d,%d) dn1_dir=(%d,%d,%d) "
                        "water=[%d,%d,%d | %d,%d,%d | %d,%d,%d | %d,%d,%d] "
                        "fog=(%d,%d,%d) alpha=(%.2f,%.2f,%.2f,%.2f)\n",
                        s_lightMode, amount,
                        light.m_ambColor.x, light.m_ambColor.y, light.m_ambColor.z,
                        light.m_dirColor.x, light.m_dirColor.y, light.m_dirColor.z,
                        dn->m_light1.m_ambient.r, dn->m_light1.m_ambient.g, dn->m_light1.m_ambient.b,
                        dn->m_light1.m_diffuse.r, dn->m_light1.m_diffuse.g, dn->m_light1.m_diffuse.b,
                        dn->m_bands.m_waterColors[0].r, dn->m_bands.m_waterColors[0].g, dn->m_bands.m_waterColors[0].b,
                        dn->m_bands.m_waterColors[1].r, dn->m_bands.m_waterColors[1].g, dn->m_bands.m_waterColors[1].b,
                        dn->m_bands.m_waterColors[2].r, dn->m_bands.m_waterColors[2].g, dn->m_bands.m_waterColors[2].b,
                        dn->m_bands.m_waterColors[3].r, dn->m_bands.m_waterColors[3].g, dn->m_bands.m_waterColors[3].b,
                        dn->m_fog.m_color.r, dn->m_fog.m_color.g, dn->m_fog.m_color.b,
                        dn->m_bands.m_oceanShallowAlpha, dn->m_bands.m_oceanDeepAlpha,
                        dn->m_bands.m_waterShallowAlpha, dn->m_bands.m_waterDeepAlpha);
            }
        }
    }

    void ApplyWaterColorMode() {
        // The map palette must not pick up per-zone M2 scene lights; the water material
        // otherwise sums them into the sun ambient/diffuse and shows ADT-shaped seams.
        MapBakerFlatWaterLight = (s_waterMode == 1) ? 1 : 0;

        if (s_waterMode == 0) {
            return; // client Light.dbc water bands
        }

        DayNight::DNInfo* info = DayNight::GetInfo();
        if (!info || !CMap::s_mapLight) {
            return;
        }

        if (s_waterMode == 1) {
            // Map preset: one ocean palette for the whole continent, matching the original
            // 3.3.5a minimap's flat ocean (30,59,93). The client bands swing hard between
            // zones (Northrend: 16,40,76 -> green 52,76,64 -> 29,58,93), which shows up as
            // per-ADT water rectangles. Feed one pre-darkened source colour for both shallow
            // and deep: the liquid shader applies its own depth gradient and darkening.
            CImVector shallow;
            shallow.a = 255;
            shallow.r = 33;
            shallow.g = 70;
            shallow.b = 113;

            CImVector deep = shallow;

            // Water is lit by ambient+diffuse, and neutral desaturation preserves each
            // zone's intensity. Normalise the palette by the current light sum so the
            // final ocean colour is the same in every zone (otherwise shallow zones
            // render ~18% brighter and show up as rectangles).
            const CM2Light& light = CMap::s_mapLight->m_light;
            float intensity = light.m_ambColor.x + light.m_dirColor.x;
            if (intensity < 0.05f) {
                intensity = 0.05f;
            }
            const float scale = 0.96f / intensity;
            const auto scaled = [&](uint8_t v) {
                const float f = v * scale;
                return static_cast<uint8_t>(f < 0.0f ? 0.0f : (f > 255.0f ? 255.0f : f));
            };
            shallow.r = scaled(shallow.r);
            shallow.g = scaled(shallow.g);
            shallow.b = scaled(shallow.b);
            deep = shallow;

            info->m_bands.m_waterColors[0] = shallow; // ocean shallow
            info->m_bands.m_waterColors[1] = deep;    // ocean deep
            info->m_bands.m_waterColors[2] = shallow; // river/lake shallow
            info->m_bands.m_waterColors[3] = deep;    // river/lake deep

            // The depth texture blends shallow->deep alpha, and the alpha bands are
            // per-zone/per-liquid-type. Even with identical colours that changes how much
            // of the lakebed shows through, producing ADT-shaped brightness steps (e.g.
            // ocean 25,26 next to lake-type water 26,26). Make water fully opaque so the
            // map ocean is one flat colour everywhere.
            info->m_bands.m_oceanShallowAlpha = 1.0f;
            info->m_bands.m_oceanDeepAlpha = 1.0f;
            info->m_bands.m_waterShallowAlpha = 1.0f;
            info->m_bands.m_waterDeepAlpha = 1.0f;
        } else {
            float amount = s_lightDesat;
            if (amount <= 0.0f) {
                return;
            }
            if (amount > 1.0f) {
                amount = 1.0f;
            }
            for (int32_t i = 0; i < 4; i++) {
                info->m_bands.m_waterColors[i] =
                    DesaturateImVector(info->m_bands.m_waterColors[i], amount);
            }
        }

        if (getenv("MAPBAKER_DEBUG_LIGHT")) {
            static int s_printed = 0;
            if (s_printed < 4) {
                s_printed++;
                fprintf(stderr,
                        "MapBaker water: mode=%d colors=[%d,%d,%d | %d,%d,%d | %d,%d,%d | %d,%d,%d] "
                        "alpha=(%.2f,%.2f,%.2f,%.2f)\n",
                        s_waterMode,
                        info->m_bands.m_waterColors[0].r, info->m_bands.m_waterColors[0].g, info->m_bands.m_waterColors[0].b,
                        info->m_bands.m_waterColors[1].r, info->m_bands.m_waterColors[1].g, info->m_bands.m_waterColors[1].b,
                        info->m_bands.m_waterColors[2].r, info->m_bands.m_waterColors[2].g, info->m_bands.m_waterColors[2].b,
                        info->m_bands.m_waterColors[3].r, info->m_bands.m_waterColors[3].g, info->m_bands.m_waterColors[3].b,
                        info->m_bands.m_oceanShallowAlpha, info->m_bands.m_oceanDeepAlpha,
                        info->m_bands.m_waterShallowAlpha, info->m_bands.m_waterDeepAlpha);
            }
        }
    }

    void UpdateWorld(const C3Vector& camPos, const C3Vector& camTarget, int32_t tileTx, int32_t tileTy) {
        World::s_currentWorldPos = camPos;

        // AOI boxes: the camera looks straight down, so use a generous radius in XY.
        const float ao = World::s_farClip;
        CWorld::s_groupAreaOfInterest.b = C3Vector(camPos.x - ao, camPos.y - ao, camPos.z - ao);
        CWorld::s_groupAreaOfInterest.t = C3Vector(camPos.x + ao, camPos.y + ao, camPos.z + ao);
        CWorld::s_objectAreaOfInterest.b = C3Vector(camPos.x - ao, camPos.y - ao, camPos.z - ao);
        CWorld::s_objectAreaOfInterest.t = C3Vector(camPos.x + ao, camPos.y + ao, camPos.z + ao);

        C3Vector pos = camPos;
        C3Vector target = camTarget;
        CWorldScene::Update(&pos, &target);
        CWorldScene::LocateViewer3();
        CWorldScene::s_cullPassStamp++;

        // Chunk rect centered on the tile with a half-ADT margin so M2/WMO geometry crossing
        // the tile border still loads and draws. The chunk X axis derives from world Y and the
        // chunk Y axis from world X. Tile (tx,ty) center = chunk (tx*16 + 8, ty*16 + 8).
        const int32_t chunkCX = tileTx * 16 + 8;
        const int32_t chunkCY = tileTy * 16 + 8;
        const int32_t chunkRadius = 16; // one full ADT radius: tile + half ADT margin per side

        CiRect chunkRect;
        chunkRect.minX = std::max((chunkCX - chunkRadius) & ~1, 0);
        chunkRect.maxX = std::min(((chunkCX + chunkRadius) & ~1) + 1, 1023);
        chunkRect.minY = std::max((chunkCY - chunkRadius) & ~1, 0);
        chunkRect.maxY = std::min(((chunkCY + chunkRadius) & ~1) + 1, 1023);

        CWorld::chunkRect = chunkRect;
        CWorld::gbChunkRect.minX = std::max(chunkRect.minX - 2, 0);
        CWorld::gbChunkRect.maxX = std::min(chunkRect.maxX + 2, 1023);
        CWorld::gbChunkRect.minY = std::max(chunkRect.minY - 2, 0);
        CWorld::gbChunkRect.maxY = std::min(chunkRect.maxY + 2, 1023);

        for (int32_t areaY = chunkRect.minY >> 4; areaY <= chunkRect.maxY >> 4; areaY++) {
            for (int32_t areaX = chunkRect.minX >> 4; areaX <= chunkRect.maxX >> 4; areaX++) {
                const int32_t index = 64 * areaY + areaX;

                if (!(CMap::m_areaInfo[index].m_flags & 1)) {
                    continue;
                }

                CMapArea* area = CMap::m_areaTable[index];

                if (!area) {
                    area = CMap::PrepareArea(areaX, areaY);
                    CMap::LoadArea(area);
                    continue;
                }

                if (area->m_asyncObject) {
                    continue;
                }

                CiRect areaRect;
                areaRect.minX = std::max(chunkRect.minX, 16 * areaX);
                areaRect.maxX = std::min(chunkRect.maxX, 16 * areaX + 15);
                areaRect.minY = std::max(chunkRect.minY, 16 * areaY);
                areaRect.maxY = std::min(chunkRect.maxY, 16 * areaY + 15);
                area->Update(1, &areaRect);
            }
        }

        CMap::PrepareUpdate(true);

        const float progression = 0.5f; // noon

        C3Vector dir(camTarget.x - camPos.x, camTarget.y - camPos.y, camTarget.z - camPos.z);
        const float length = sqrtf(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
        if (length > 0.0f) {
            dir = C3Vector(dir.x / length, dir.y / length, dir.z / length);
        }

        DayNight::DNInfo* info = DayNight::GetInfo();
        info->m_dayProgression = progression;
        info->m_timeOfDay = static_cast<uint32_t>(progression * 1440.0f);
        info->m_day = 0.0f;
        info->m_farClip = World::s_farClip;
        info->m_deltaSec = FrameTime::s_tickTimeSec;
        info->m_timeSec = kFixedTimeSec;
        info->m_cameraPos = camPos;
        info->m_lightRefPos = camPos;
        info->m_playerPos = camPos;
        info->m_cameraDir = dir;
        info->m_weatherIntensity = 0.0f;

        C3Vector camera = camPos;
        DayNight::Update(1, &camera);
        CWorld::farFog = info->m_fog.m_end;

        if (s_lightElev >= 0.0f) {
            // The default sun (set by DayNight::Update above) sits ~37 deg above the horizon at
            // noon, so roofs shade with slope and walls catch a lot of diffuse. Re-point the
            // single map light (used by terrain AND M2/WMO) to the requested elevation, keeping
            // the noon azimuth (toward -X,-Y). As the light gets more vertical, horizontal roof
            // tops light uniformly and vertical walls fall toward ambient: the flatter "city
            // floor" look of the original minimap.
            const float el = s_lightElev * 3.14159265f / 180.0f;
            const float c = cosf(el) * 0.70710678f;
            const C3Vector dir(-c, -c, -sinf(el));
            CMap::s_mapLight->m_light.SetDirection(dir);
            info->m_light0.m_dir = dir;
            info->m_light1.m_dir = dir;
        }

        // Neutral/desaturated terrain lighting (map preset default). Applied after
        // CWorldScene::Update/DayNight so it overrides the Light.dbc color cast while
        // preserving the direction/elevation set above. The client/realistic mode is a
        // no-op.
        ApplyLightColorMode();
        ApplyWaterColorMode();
    }

    void PrepareScene() {

        CWorldScene::nDoodadsRendered = 0;
        CWorldScene::s_chunksRendered = 0;

        if (!CWorld::shadowModTexture) {
            CImVector shadow;
            shadow.r = 0x40;
            shadow.g = 0x40;
            shadow.b = 0x48;
            shadow.a = 0x4C;
            CWorld::SetShadowColor(shadow);
        }

        if (!CWorldScene::s_viewerMapObjDef) {
            CWorldScene::ComputeExteriorSortDistances();
        }

        CWorldScene::UpdateViewerLiquid();
        CWorldScene::frustumStack[CWorldScene::frustumIndex].CalcPlanesFromCorners(CWorldScene::s_frustumCorners);
        CWorldScene::s_occlusionFarDepth = World::s_farClip - 33.333332f;
        CMapRenderChunk::UpdatePools();
        CMapObj::UpdatePools();
        CDetailDoodad::UpdatePools();
        memset(CWorldScene::s_clipBufferMask, 0, sizeof(CWorldScene::s_clipBufferMask));

        for (uint32_t i = 0; i < 384; i++) {
            CWorldScene::s_clipBuffer[i] = -1000000.0f;
        }

        CWorldOcclusion::ClearVolumes();
        CWorldScene::AddWorldOccluders();

        // Offline render is always exterior (no interior viewer mapobj).
        CRect fullScreen;
        fullScreen.minY = 0.0f;
        fullScreen.minX = 0.0f;
        fullScreen.maxY = 1.0f;
        fullScreen.maxX = 1.0f;

        CWorldScene::s_unk_ADF570.m_rect = fullScreen;
        CWorldScene::s_unk_ADF570.m_maxViewDepth = 0.0f;
        CWorldScene::s_unk_ADF570.m_vertCount = 0;
        CWorldScene::frustumPortalView.m_rect = fullScreen;
        CWorldScene::frustumPortalView.m_maxViewDepth = 0.0f;
        CWorldScene::frustumPortalView.m_vertCount = 0;
        CWorldScene::s_occlusionNearDepth = -10000.0f;
        CWorldScene::CullSortTable(&CWorldScene::frustumPortalView.m_rect);

        CWorldScene::CullMapObjDefGroup();
        CWorldScene::sortTable.m_pendingExteriorGroupList.UnlinkAll();

    }

    void RenderWorld() {
        PrepareScene();
        DayNight::DNInfo* info = DayNight::GetInfo();

        // MapBaker: the sky is disabled for a top-down map, so the background clear must be the
        // horizon/fog color (a deterministic noon value) instead of the default black.
        CImVector clearColor = info->m_fog.m_color;

        if (CWorldScene::s_unk_ADF570.m_maxViewDepth < 0.0f) {
            clearColor = info->m_fogInterior.m_color;
        } else if (s_liquidTypeId) {
            clearColor = info->m_fog.m_color;
        }

        g_theGxDevicePtr->SceneClear(3, clearColor);

        CWorldScene::ApplyFogState();
        if (!getenv("MAPBAKER_SKIP_TERRAIN")) {
            CWorldScene::RenderChunks();
        }
        if (!s_noWmo && !s_quadWmoSuppress && !getenv("MAPBAKER_SKIP_WMO")) {
            CWorldScene::RenderMapObjDefGroups();
        }

        if (CWorldScene::s_unk_ADF570.m_maxViewDepth >= 0.0f) {
            if (CWorldScene::s_exteriorPortalViews.Count()) {
                CWorldScene::CreateInversePortalList();
                CWorldScene::RenderPortalList(&CWorldScene::s_exteriorPortalViews, 1);
            }

            if (CWorldScene::s_portalViews.Count()) {
                CWorldScene::RenderPortalList(&CWorldScene::s_portalViews, 0);
            }

            if (!s_liquidTypeId && (CWorld::enables & Enable_Sky)) {
                DayNight::RenderSky(&CWorldScene::s_unk_ADF570);
            }
        }

        Liquid::UpdateProceduralTextures();
        CWorldScene::PrepareLiquidInstances();

        if ((CWorld::enables & Enable_Water) && CWorldScene::s_liquidRenderer && !getenv("MAPBAKER_SKIP_LIQUID")) {
            CWorldScene::s_liquidRenderer->Flush(CWorldScene::s_activeWorldView, 0);
        }
    }

    void RenderM2AndDetails(const C3Vector& camPos, bool liquids = true) {
        if (!World::s_m2Scene) {
            return;
        }
        if (getenv("MAPBAKER_SKIP_M2")) {
            return;
        }

        World::s_m2Scene->AdvanceTime(0);
        World::s_m2Scene->Animate(camPos);
        if (getenv("MAPBAKER_DEBUG_M2")) {
            static std::set<std::string> seen;
            const uint32_t count = World::s_m2Scene->m_elements.Count();
            for (uint32_t i = 0; i < count; i++) {
                const M2Element& e = World::s_m2Scene->m_elements.m_data[i];
                if (e.m_model && e.m_model->m_shared) {
                    const char* name = e.m_model->m_shared->m_fileName;
                    if (seen.insert(name).second) {
                        fprintf(stderr, "MapBaker m2: type=%d count=%u model=%s tree=%d\n", e.m_type, e.m_count, name, MapBakerM2IsTree(name) ? 1 : 0);
                    }
                }
            }
        }

        World::s_m2Scene->Draw(M2PASS_0);
        CWorldScene::RenderDetailDoodads();
        World::s_m2Scene->Draw(M2PASS_2);
        if (liquids && !getenv("MAPBAKER_SKIP_LIQUID")) {
            CWorldScene::RenderLiquidPass();   // transparent liquids (water/slime/lava)
        }
        World::s_m2Scene->Draw(M2PASS_1);
    }

    // Box-filter downsample of an RGB buffer.
    void BoxDownsampleRgb(const uint8_t* src, int32_t srcW, int32_t srcH, uint8_t* dst, int32_t dstW, int32_t dstH) {
        if (srcW == dstW && srcH == dstH) {
            memcpy(dst, src, static_cast<size_t>(srcW) * srcH * 3);
            return;
        }

        for (int32_t dy = 0; dy < dstH; dy++) {
            const float fy0 = static_cast<float>(dy) * srcH / dstH;
            const float fy1 = static_cast<float>(dy + 1) * srcH / dstH;
            int32_t sy0 = static_cast<int32_t>(fy0);
            int32_t sy1 = static_cast<int32_t>(fy1);
            if (sy1 <= sy0) {
                sy1 = sy0 + 1;
            }
            if (sy0 < 0) sy0 = 0;
            if (sy1 > srcH) sy1 = srcH;

            for (int32_t dx = 0; dx < dstW; dx++) {
                const float fx0 = static_cast<float>(dx) * srcW / dstW;
                const float fx1 = static_cast<float>(dx + 1) * srcW / dstW;
                int32_t sx0 = static_cast<int32_t>(fx0);
                int32_t sx1 = static_cast<int32_t>(fx1);
                if (sx1 <= sx0) {
                    sx1 = sx0 + 1;
                }
                if (sx0 < 0) sx0 = 0;
                if (sx1 > srcW) sx1 = srcW;

                int32_t r = 0, g = 0, b = 0, n = 0;
                for (int32_t sy = sy0; sy < sy1; sy++) {
                    const uint8_t* row = src + static_cast<size_t>(sy) * srcW * 3;
                    for (int32_t sx = sx0; sx < sx1; sx++) {
                        r += row[sx * 3 + 0];
                        g += row[sx * 3 + 1];
                        b += row[sx * 3 + 2];
                        n++;
                    }
                }

                uint8_t* out = dst + static_cast<size_t>(dy) * dstW * 3 + dx * 3;
                if (n > 0) {
                    out[0] = static_cast<uint8_t>(r / n);
                    out[1] = static_cast<uint8_t>(g / n);
                    out[2] = static_cast<uint8_t>(b / n);
                } else {
                    out[0] = out[1] = out[2] = 0;
                }
            }
        }
    }

    // Box-filter downsample of an RGBA buffer. Alpha is averaged premultiplied, then
    // unpremultiplied, so transparent edges do not darken when the layer is composited.
    void BoxDownsampleRgba(const uint8_t* src, int32_t srcW, int32_t srcH, uint8_t* dst, int32_t dstW, int32_t dstH) {
        if (srcW == dstW && srcH == dstH) {
            memcpy(dst, src, static_cast<size_t>(srcW) * srcH * 4);
            return;
        }

        for (int32_t dy = 0; dy < dstH; dy++) {
            const float fy0 = static_cast<float>(dy) * srcH / dstH;
            const float fy1 = static_cast<float>(dy + 1) * srcH / dstH;
            int32_t sy0 = static_cast<int32_t>(fy0);
            int32_t sy1 = static_cast<int32_t>(fy1);
            if (sy1 <= sy0) sy1 = sy0 + 1;
            if (sy0 < 0) sy0 = 0;
            if (sy1 > srcH) sy1 = srcH;

            for (int32_t dx = 0; dx < dstW; dx++) {
                const float fx0 = static_cast<float>(dx) * srcW / dstW;
                const float fx1 = static_cast<float>(dx + 1) * srcW / dstW;
                int32_t sx0 = static_cast<int32_t>(fx0);
                int32_t sx1 = static_cast<int32_t>(fx1);
                if (sx1 <= sx0) sx1 = sx0 + 1;
                if (sx0 < 0) sx0 = 0;
                if (sx1 > srcW) sx1 = srcW;

                float r = 0.0f, g = 0.0f, b = 0.0f, a = 0.0f;
                int32_t n = 0;
                for (int32_t sy = sy0; sy < sy1; sy++) {
                    const uint8_t* row = src + static_cast<size_t>(sy) * srcW * 4;
                    for (int32_t sx = sx0; sx < sx1; sx++) {
                        const uint8_t* p = row + static_cast<size_t>(sx) * 4;
                        const float pa = p[3] / 255.0f;
                        r += p[0] * pa;
                        g += p[1] * pa;
                        b += p[2] * pa;
                        a += pa;
                        n++;
                    }
                }

                uint8_t* out = dst + static_cast<size_t>(dy) * dstW * 4 + static_cast<size_t>(dx) * 4;
                if (n > 0 && a > 0.0f) {
                    const float inv = 1.0f / a;
                    out[0] = static_cast<uint8_t>(r * inv + 0.5f);
                    out[1] = static_cast<uint8_t>(g * inv + 0.5f);
                    out[2] = static_cast<uint8_t>(b * inv + 0.5f);
                    out[3] = static_cast<uint8_t>((a / n) * 255.0f + 0.5f);
                } else {
                    out[0] = out[1] = out[2] = out[3] = 0;
                }
            }
        }
    }

    // Read the full render target into an RGB buffer (size x size, row 0 = north).
    bool ReadRtToRgb(uint8_t* rgb, uint32_t size) {
        TSGrowableArray<CImVector> pixels;
        CiRect rect;
        rect.minX = 0;
        rect.minY = 0;
        rect.maxX = static_cast<int32_t>(size);
        rect.maxY = static_cast<int32_t>(size);

        GxReadRenderTarget(s_rt, rect, pixels);

        if (pixels.Count() != static_cast<uint32_t>(size) * size) {
            return false;
        }

        const CImVector* src = pixels.Ptr();
        for (uint32_t i = 0; i < static_cast<uint32_t>(size) * size; i++) {
            rgb[i * 3 + 0] = src[i].r;
            rgb[i * 3 + 1] = src[i].g;
            rgb[i * 3 + 2] = src[i].b;
        }

        return true;
    }

    // Read the full render target into an RGBA buffer (row 0 = north).
    bool ReadRtToRgba(uint8_t* rgba, uint32_t size) {
        TSGrowableArray<CImVector> pixels;
        CiRect rect;
        rect.minX = 0;
        rect.minY = 0;
        rect.maxX = static_cast<int32_t>(size);
        rect.maxY = static_cast<int32_t>(size);

        GxReadRenderTarget(s_rt, rect, pixels);

        if (pixels.Count() != static_cast<uint32_t>(size) * size) {
            return false;
        }

        const CImVector* src = pixels.Ptr();
        for (uint32_t i = 0; i < static_cast<uint32_t>(size) * size; i++) {
            rgba[i * 4 + 0] = src[i].r;
            rgba[i * 4 + 1] = src[i].g;
            rgba[i * 4 + 2] = src[i].b;
            rgba[i * 4 + 3] = src[i].a;
        }

        return true;
    }

    // Composite the selected 2D WMO quads into the full-res RGB readback (row 0 = north).
    // Each SELECTED group whose world bbox intersects the tile is drawn as a flat tinted,
    // axis-aligned screen rect (exterior vs interior get different colors). The mapping is
    // derived from the top-down ortho camera (forward=(0,0,-1), up=+X, so camera-right =
    // -worldY and camera-up = +worldX; "row 0 = north" = +X):
    //   px = size/2 + (size/A) * (centerY - wy)
    //   py = size/2 + (size/A) * (centerX - wx)
    // A world XY rect maps to an axis-aligned screen rect (no rotation), so per group we
    // clip to the tile, map the four edges, and alpha-fill the rect. Done at full RT
    // resolution so the box downsample anti-aliases the quad edges.
    void CompositeQuads(uint8_t* rgb, uint32_t size, float centerX, float centerY) {
        if (!MapBakerQuadOn || MapBakerQuadCount == 0) {
            return;
        }

        const float scale = static_cast<float>(size) / kAdtSize; // pixels per world unit
        const float half = kAdtSize * 0.5f;
        const float tileX0 = centerX - half;
        const float tileX1 = centerX + half;
        const float tileY0 = centerY - half;
        const float tileY1 = centerY + half;
        const float a = static_cast<float>(MapBakerQuadAlpha) / 255.0f;
        const float f = 1.0f - a;
        const int32_t sizeI = static_cast<int32_t>(size);
        const float sizeHalf = 0.5f * static_cast<float>(size);

        for (uint32_t i = 0; i < MapBakerQuadCount; i++) {
            const MapBakerQuadEntry& e = MapBakerQuadTable[i];
            if (!e.m_selected) {
                continue;
            }

            // Clip the group bbox to the tile world region.
            const float gx0 = fmaxf(e.m_x0, tileX0);
            const float gx1 = fminf(e.m_x1, tileX1);
            const float gy0 = fmaxf(e.m_y0, tileY0);
            const float gy1 = fminf(e.m_y1, tileY1);
            if (gx1 <= gx0 || gy1 <= gy0) {
                continue; // no overlap
            }

            // World->screen of the clipped rect corners.
            // px(wy): wy=gy1 (max Y) -> min px (left); wy=gy0 (min Y) -> max px (right).
            // py(wx): wx=gx1 (max X) -> min py (top);  wx=gx0 (min X) -> max py (bottom).
            int32_t pxL = (int32_t)floorf(sizeHalf + scale * (centerY - gy1));
            int32_t pxR = (int32_t)ceilf(sizeHalf + scale * (centerY - gy0));
            int32_t pyT = (int32_t)floorf(sizeHalf + scale * (centerX - gx1));
            int32_t pyB = (int32_t)ceilf(sizeHalf + scale * (centerX - gx0));
            if (pxL < 0) pxL = 0;
            if (pyT < 0) pyT = 0;
            if (pxR > sizeI) pxR = sizeI;
            if (pyB > sizeI) pyB = sizeI;
            if (pxR <= pxL || pyB <= pyT) {
                continue;
            }

            const uint8_t tr = e.m_smoFlags & 0x8 ? MapBakerQuadExtColor[0] : MapBakerQuadIntColor[0];
            const uint8_t tg = e.m_smoFlags & 0x8 ? MapBakerQuadExtColor[1] : MapBakerQuadIntColor[1];
            const uint8_t tb = e.m_smoFlags & 0x8 ? MapBakerQuadExtColor[2] : MapBakerQuadIntColor[2];

            for (int32_t py = pyT; py < pyB; py++) {
                uint8_t* row = rgb + static_cast<size_t>(py) * size * 3;
                for (int32_t px = pxL; px < pxR; px++) {
                    uint8_t* p = row + px * 3;
                    p[0] = static_cast<uint8_t>(p[0] * f + tr * a + 0.5f);
                    p[1] = static_cast<uint8_t>(p[1] * f + tg * a + 0.5f);
                    p[2] = static_cast<uint8_t>(p[2] * f + tb * a + 0.5f);
                }
            }
        }
    }

    // Composite the REAL minimap art (Textures\Minimap\<wmo>_<grp>_<x>_<y>.blp cells,
    // see MapBakerArt.h) over the terrain readback.
    //
    // IMPORTANT (matches the client exactly): the quad CELL GRID lives in WMO-LOCAL space.
    //   * CMapEntity::QueryMapObjMinimap transforms the query box into local space
    //     (mapObjDef->m_invMat) and CMapObj::QueryMapObjMinimapGroup tiles
    //     CMapObjGroup::m_bbox -- the LOCAL group bbox (MapObj.cpp, GetMinimapQuads).
    //   * CGMinimapFrame::RenderInsideTexture then draws those local quad rectangles with
    //     s_indoor.m_matrix (= mapObjDef->m_mat), i.e. the instance transform.
    // So each cell is a LOCAL rectangle transformed by the instance matrix, which makes it
    // a (possibly rotated/scaled) parallelogram in world space. The .blp name indices only
    // match that local grid -- deriving the grid from the world AABB scrambles them for
    // rotated instances (skewed/misplaced art).
    //
    // UV (CGMinimapFrame::RenderInsideQuad): across a cell's own bounds, u runs with local
    // +X (0 at minX) and v with local -Y (1 at minY, 0 at maxY). The instance rotation then
    // produces wowmin's verified world-space result automatically.
    void CompositeArt(uint8_t* pixels, uint32_t size, float centerX, float centerY, bool rgba) {
        if (!MapBakerQuadOn || !MapBakerArtOn || MapBakerQuadCount == 0) {
            return;
        }
        if (!MapBakerArtInit()) {
            return;
        }

        const float scale = static_cast<float>(size) / kAdtSize;
        const float alphaMul = static_cast<float>(MapBakerArtAlpha) / 255.0f;
        const int32_t sizeI = static_cast<int32_t>(size);
        const float sizeHalf = 0.5f * static_cast<float>(size);

        for (uint32_t i = 0; i < MapBakerQuadCount; i++) {
            const MapBakerQuadEntry& e = MapBakerQuadTable[i];
            if (!e.m_selected) {
                continue;
            }
            const float lx0 = e.m_lx0;
            const float lx1 = e.m_lx1;
            const float ly0 = e.m_ly0;
            const float ly1 = e.m_ly1;
            if (!(lx1 > lx0) || !(ly1 > ly0)) {
                continue;   // no local bbox recorded (fall back to no art for this group)
            }

            // local (lx, ly) -> world -> screen, as a 2x2 + offset (row-vector convention):
            //   wx = ma*lx + mb*ly + mtx          px = sizeHalf + scale*(centerY - wy)
            //   wy = mc*lx + md*ly + mty          py = sizeHalf + scale*(centerX - wx)
            const float a11 = -scale * e.m_mc;   // d(px)/d(lx)
            const float a12 = -scale * e.m_md;   // d(px)/d(ly)
            const float a21 = -scale * e.m_ma;   // d(py)/d(lx)
            const float a22 = -scale * e.m_mb;   // d(py)/d(ly)
            const float t0 = sizeHalf + scale * (centerY - e.m_mty);   // px at local (0,0)
            const float t1 = sizeHalf + scale * (centerX - e.m_mtx);   // py at local (0,0)
            const float det = a11 * a22 - a12 * a21;
            if (fabsf(det) < 1.0e-12f) {
                continue;
            }
            const float invDet = 1.0f / det;
            const float i00 = a22 * invDet;
            const float i01 = -a12 * invDet;
            const float i10 = -a21 * invDet;
            const float i11 = a11 * invDet;

            // Quick reject: screen bbox of the group's local bbox.
            float sx0 = 1.0e30f, sx1 = -1.0e30f, sy0 = 1.0e30f, sy1 = -1.0e30f;
            const float corners[4][2] = { { lx0, ly0 }, { lx1, ly0 }, { lx0, ly1 }, { lx1, ly1 } };
            for (int32_t k = 0; k < 4; k++) {
                const float px = a11 * corners[k][0] + a12 * corners[k][1] + t0;
                const float py = a21 * corners[k][0] + a22 * corners[k][1] + t1;
                if (px < sx0) sx0 = px;
                if (px > sx1) sx1 = px;
                if (py < sy0) sy0 = py;
                if (py > sy1) sy1 = py;
            }
            if (sx1 < -2.0f || sy1 < -2.0f || sx0 > sizeI + 2.0f || sy0 > sizeI + 2.0f) {
                continue;
            }

            MapBakerArtCell cells[256];
            const uint32_t cellCount = MapBakerArtCellGrid(lx0, lx1, ly0, ly1, cells, 256);
            for (uint32_t c = 0; c < cellCount; c++) {
                const MapBakerArtCell& cell = cells[c];
                const MapBakerArtImage* image =
                        MapBakerArtGet(e.m_wmoName, e.m_groupNum, cell.m_x, cell.m_y);
                if (!image || !image->m_px || image->m_w <= 0 || image->m_h <= 0) {
                    continue;   // no art file for this cell (skipped, per the art-only mode)
                }
                const float cw = cell.m_x1 - cell.m_x0;
                const float ch = cell.m_y1 - cell.m_y0;
                if (!(cw > 0.0f) || !(ch > 0.0f)) {
                    continue;
                }

                // Screen bbox of this cell's (rotated) parallelogram.
                const float cc[4][2] = { { cell.m_x0, cell.m_y0 }, { cell.m_x1, cell.m_y0 },
                                         { cell.m_x0, cell.m_y1 }, { cell.m_x1, cell.m_y1 } };
                float bx0 = 1.0e30f, bx1 = -1.0e30f, by0 = 1.0e30f, by1 = -1.0e30f;
                for (int32_t k = 0; k < 4; k++) {
                    const float px = a11 * cc[k][0] + a12 * cc[k][1] + t0;
                    const float py = a21 * cc[k][0] + a22 * cc[k][1] + t1;
                    if (px < bx0) bx0 = px;
                    if (px > bx1) bx1 = px;
                    if (py < by0) by0 = py;
                    if (py > by1) by1 = py;
                }
                int32_t pxL = (int32_t)floorf(bx0);
                int32_t pxR = (int32_t)ceilf(bx1);
                int32_t pyT = (int32_t)floorf(by0);
                int32_t pyB = (int32_t)ceilf(by1);
                if (pxL < 0) pxL = 0;
                if (pyT < 0) pyT = 0;
                if (pxR > sizeI) pxR = sizeI;
                if (pyB > sizeI) pyB = sizeI;
                if (pxR <= pxL || pyB <= pyT) {
                    continue;
                }

                // Bilinear reads t00/t10/t01/t11; keep the source texel inside the
                // image. The previous -1-epsilon clamp still allowed u0/v0 at the last
                // texel, so t10/t01/t11 read one past the row/image (usually benign on
                // native allocators, a page fault under Wine, and visible as edge
                // garbage). Clamp to the last *bilinear* source texel instead.
                const int32_t tw = image->m_w;
                const int32_t th = image->m_h;
                const float tuMax = (tw > 1) ? static_cast<float>(tw - 2) : 0.0f;
                const float tvMax = (th > 1) ? static_cast<float>(th - 2) : 0.0f;
                const size_t rowStride = static_cast<size_t>(tw) * 4;
                const size_t colStep = (tw > 1) ? 4 : 0;
                const size_t rowStep = (th > 1) ? rowStride : 0;

                for (int32_t py = pyT; py < pyB; py++) {
                    uint8_t* row = pixels + static_cast<size_t>(py) * size * (rgba ? 4 : 3);
                    const float dy = static_cast<float>(py) + 0.5f - t1;
                    for (int32_t px = pxL; px < pxR; px++) {
                        // Screen -> local (inverse affine), then the cell's own UV mapping:
                        //   u = (lx - cellMinX)/cellW  (u along local +X)
                        //   v = 1 - (ly - cellMinY)/cellH  (v 1 at minY, 0 at maxY)
                        const float dx = static_cast<float>(px) + 0.5f - t0;
                        const float lx = i00 * dx + i01 * dy;
                        const float ly = i10 * dx + i11 * dy;
                        if (lx < cell.m_x0 || lx > cell.m_x1 ||
                                ly < cell.m_y0 || ly > cell.m_y1) {
                            continue;
                        }
                        float uC = (lx - cell.m_x0) / cw * image->m_w;
                        float vC = (1.0f - (ly - cell.m_y0) / ch) * image->m_h;
                        if (uC < 0.0f) uC = 0.0f;
                        if (uC > tuMax) uC = tuMax;
                        if (vC < 0.0f) vC = 0.0f;
                        if (vC > tvMax) vC = tvMax;
                        const int32_t u0 = static_cast<int32_t>(uC);
                        const int32_t v0 = static_cast<int32_t>(vC);
                        const float fu = uC - static_cast<float>(u0);
                        const float fv = vC - static_cast<float>(v0);

                        const uint8_t* t00 = image->m_px +
                                static_cast<size_t>(v0) * rowStride + static_cast<size_t>(u0) * 4;
                        const uint8_t* t10 = t00 + colStep;
                        const uint8_t* t01 = t00 + rowStep;
                        const uint8_t* t11 = t01 + colStep;
                        // Bilinear over the 4 texels (CImVector/BGRA byte order).
                        float texel[4];
                        for (int32_t k = 0; k < 4; k++) {
                            const float l0 = t00[k] * (1.0f - fu) + t10[k] * fu;
                            const float l1 = t01[k] * (1.0f - fu) + t11[k] * fu;
                            texel[k] = l0 * (1.0f - fv) + l1 * fv;
                        }
                        const float a = (texel[3] / 255.0f) * alphaMul;
                        if (a <= 0.0f) {
                            continue;
                        }
                        uint8_t* p = row + px * (rgba ? 4 : 3);
                        // m_px is the engine's PIXEL_ARGB8888 output, i.e. CImVector
                        // byte order b,g,r,a (same as the terrain readback's CImVector).
                        // Keep the RGB destination in r,g,b order: byte 2 is red and
                        // byte 0 is blue. Using texel[0] as red made red/orange cave
                        // art (lava, warm tunnels) come out blue.
                        if (rgba) {
                            // Source-over onto a transparent layer, straight RGB.
                            const float dstA = p[3] / 255.0f;
                            const float outA = a + dstA * (1.0f - a);
                            if (outA <= 0.0f) {
                                continue;
                            }
                            const float inv = 1.0f / outA;
                            p[0] = static_cast<uint8_t>((texel[2] * a + p[0] * dstA * (1.0f - a)) * inv + 0.5f);
                            p[1] = static_cast<uint8_t>((texel[1] * a + p[1] * dstA * (1.0f - a)) * inv + 0.5f);
                            p[2] = static_cast<uint8_t>((texel[0] * a + p[2] * dstA * (1.0f - a)) * inv + 0.5f);
                            p[3] = static_cast<uint8_t>(outA * 255.0f + 0.5f);
                        } else {
                            const float f = 1.0f - a;
                            p[0] = static_cast<uint8_t>(p[0] * f + texel[2] * a + 0.5f);
                            p[1] = static_cast<uint8_t>(p[1] * f + texel[1] * a + 0.5f);
                            p[2] = static_cast<uint8_t>(p[2] * f + texel[0] * a + 0.5f);
                        }
                    }
                }
            }
        }
    }
}

namespace MapBakerWorld {
    bool LoadMap(const char* directory, int32_t mapId) {
        // Offline render: liquid materials must not depend on async texture warm-up
        // (the water shaders return early until the animated set is loaded).
        MapBakerWaitTextures = 1;
        World::s_farClip = 1500.0f;
        World::s_nearClip = 0.2f;
        World::s_prevFarClip = World::s_farClip;
        CWorld::enables &= ~Enable_LowDetail; // no terrain LOD

        CMap::Load(directory, mapId);
        Liquid::SetShadersEnabled((CWorld::enables2 & Enable_VertexShader) && (CWorld::enables & Enable_PixelShaders));
        Liquid::SetSpecularEnabled(CMap::enableSpecular);
        Liquid::CMaterialBank::Destroy();
        return true;
    }

    void ApplyPreset(Preset preset) {
        // Both presets: noon, no fog, no sky, no weather.
        g_theGxDevicePtr->MasterEnableSet(GxMasterEnable_Fog, 0);
        CWorld::enables &= ~Enable_Sky;

        if (preset == Preset_Map) {
            CWorld::enables &= ~Enable_Shadow;
            CWorld::enables &= ~Enable_Specular;
            CWorld::enables &= ~Enable_DetailDoodads;
            CWorld::enables &= ~Enable_Particulates;
            // Liquid specular is a SEPARATE global (set at map load from CMap::enableSpecular,
            // before this preset is applied). Left on, every water tile gets the same
            // camera-anchored highlight -> visible per-ADT brightness seams on the ocean.
            CMap::enableSpecular = false;
            Liquid::SetSpecularEnabled(0);
        } else {
            CWorld::enables |= Enable_Shadow;
            CWorld::enables |= Enable_Specular;
            CWorld::enables |= Enable_DetailDoodads;
            CMap::enableSpecular = true;
            Liquid::SetSpecularEnabled(1);
        }

        // Color-mode default: the map preset is neutral/flat like the original 2D
        // minimap; realistic keeps the client's Light.dbc zone colors. An explicit
        // --light-mode / --light-desat overrides this.
        if (s_lightMode < 0) {
            // Flat (neutral colour + fixed intensity) is the map default so WMOs, M2s and
            // terrain all use the same light in every ADT.
            s_lightMode = (preset == Preset_Map) ? 3 : 0;
        }
        if (s_waterMode < 0) {
            // Client terrain light implies client water; otherwise the map palette.
            s_waterMode = (preset == Preset_Map && s_lightMode != 0) ? 1 : 0;
        }

        ApplyFadeDistances();
    }

    void SetNoWmo(bool noWmo) {
        s_noWmo = noWmo;
    }

    void SetLightElevation(float deg) {
        s_lightElev = deg;
    }

    void SetLightMode(int32_t mode, float amount) {
        s_lightMode = mode;
        if (amount < 0.0f) {
            amount = 0.0f;
        }
        if (amount > 1.0f) {
            amount = 1.0f;
        }
        s_lightDesat = amount;
    }

    void SetWaterMode(int32_t mode) {
        s_waterMode = mode;
    }

    void SetGain(float gain) {
        s_gain = gain;
    }

    void SetCityAdts(const uint32_t* indexes, uint32_t count) {
        memset(s_cityMap, 0, sizeof(s_cityMap));
        for (uint32_t i = 0; i < count; i++) {
            const uint32_t idx = indexes[i];
            if (idx < 4096) {
                s_cityMap[idx] = true;
            }
        }
    }

    void SetCityLightElevation(float deg) {
        s_cityElev = deg;
    }

    void SetXray(uint32_t mode, uint32_t lowLast, float k, uint32_t scope) {
        MapBakerXrayMode = mode;
        MapBakerXrayLowLast = lowLast;
        MapBakerXrayK = k;
        MapBakerXrayScope = scope;
        memset(MapBakerXrayGate, 0, sizeof(MapBakerXrayGate));
        if (scope == 1) {
            // Gate on the city ADTs (must be called after SetCityAdts).
            uint32_t cityCount = 0;
            for (uint32_t i = 0; i < 4096; i++) {
                if (s_cityMap[i]) {
                    MapBakerXrayGate[i] = 1;
                    cityCount++;
                }
            }
            if (mode != 0 && cityCount == 0) {
                fprintf(stderr, "MapBaker: x-ray on but no city ADTs loaded; pass --city-file for the 'city' scope\n");
            }
        }
        if (mode != 0) {
            const char* modeName = (mode == 1) ? "depthoff" : (mode == 2) ? "zsort" : (mode == 3) ? "zinv" : "off";
            fprintf(stderr, "MapBaker: x-ray mode=%s zdir=%s k=%.0f scope=%s\n", modeName,
                    lowLast ? "lowlast" : "highlast", k, scope == 0 ? "all" : "city");
        }
    }

    void SetXrayGuard(float skipAbove, uint32_t dump) {
        MapBakerXraySkipAbove = skipAbove;
        MapBakerXrayDump = dump;
        MapBakerXrayDumpFile = nullptr;
    }

    void SetXrayDumpFile(FILE* file) {
        MapBakerXrayDumpFile = file;
    }

    void SampleStreetHeights() {
        // Street/terrain reference at the center of every city ADT. The trick guard and
        // the WMO dump use these as the city street plane. QueryHeight is a static map
        // query (no UpdateWorld needed).
        uint32_t sampled = 0;
        for (uint32_t i = 0; i < 4096; i++) {
            if (!s_cityMap[i]) {
                continue;
            }
            const int32_t tx = static_cast<int32_t>(i & 63);
            const int32_t ty = static_cast<int32_t>(i / 64);
            float cx = 0.0f, cy = 0.0f;
            TileWorldCenter(tx, ty, &cx, &cy);
            float h = 0.0f;
            const C3Vector q(cx, cy, 0.0f);
            if (CMap::QueryHeight(q, 1000.0f, &h)) {
                MapBakerXrayStreetH[i] = h;
                sampled++;
            } else {
                MapBakerXrayStreetH[i] = 0.0f; // unknown -> guard stays off for this ADT
            }
        }
        fprintf(stderr, "MapBaker: street heights sampled for %u city ADTs\n", sampled);
    }

    bool SetRenderSize(uint32_t sizePx) {
        if (sizePx < 8 || (sizePx & (sizePx - 1))) {
            fprintf(stderr, "MapBaker: render size %u is not a power of two >= 8\n", sizePx);
            return false;
        }

        if (s_rt) {
            GxTexDestroy(s_rt);
            s_rt = nullptr;
        }

        CGxTexFlags flags(GxTex_Linear, 0, 0, 0, 0, 1); // renderTarget = 1
        if (GxTexCreate(GxTex_2d, sizePx, sizePx, 0, GxTex_Argb8888, GxTex_Argb8888, flags,
                        nullptr, RtUserFunc, "MapBakerRT", s_rt) == 0) {
            fprintf(stderr, "MapBaker: GxTexCreate RT failed, error %u\n", SErrGetLastError());
            s_rt = nullptr;
            return false;
        }

        s_rtSize = sizePx;
        return true;
    }

    void TileWorldCenter(int32_t tx, int32_t ty, float* outCenterX, float* outCenterY) {
        // Tile (tx,ty) world bounds: X in [O-(ty+1)*A, O-ty*A], Y in [O-(tx+1)*A, O-tx*A].
        const float wx0 = kWorldOrigin - (ty + 1) * kAdtSize;
        const float wx1 = kWorldOrigin - ty * kAdtSize;
        const float wy0 = kWorldOrigin - (tx + 1) * kAdtSize;
        const float wy1 = kWorldOrigin - tx * kAdtSize;

        if (outCenterX) {
            *outCenterX = (wx0 + wx1) * 0.5f;
        }

        if (outCenterY) {
            *outCenterY = (wy0 + wy1) * 0.5f;
        }
    }

    // Terrain base layer: opaque terrain + liquid (the map floor), no WMO/M2/quads.
    void RenderTerrainLayer() {
        PrepareScene();
        CImVector clear;
        clear.b = 0x5d;   // #1d3b5d map background
        clear.g = 0x3b;
        clear.r = 0x1d;
        clear.a = 0xFF;
        g_theGxDevicePtr->SceneClear(3, clear);

        CWorldScene::ApplyFogState();
        CWorldScene::RenderChunks();

        Liquid::UpdateProceduralTextures();
        CWorldScene::PrepareLiquidInstances();
        if ((CWorld::enables & Enable_Water) && CWorldScene::s_liquidRenderer && !getenv("MAPBAKER_SKIP_LIQUID")) {
            // ADT water lives in the transparent list (settings->m_transparent != 0), so the
            // base terrain layer must flush both passes; the combined renderer does this via
            // RenderLiquidPass() after WMO/M2.
            CWorldScene::s_liquidRenderer->Flush(CWorldScene::s_activeWorldView, 0);
            CWorldScene::s_liquidRenderer->Flush(CWorldScene::s_activeWorldView, 1);
        }
    }

    // Transparent 3D overlay. Terrain is rendered depth-only first so WMO/trees behind
    // hills are correctly occluded; the color pass then draws only the requested layer.
    void RenderOverlayLayer(const C3Vector& camPos, int32_t layer, uint8_t background) {
        PrepareScene();
        CImVector clear;
        clear.b = background;
        clear.g = background;
        clear.r = background;
        clear.a = 0;   // native alpha capture needs a transparent clear
        g_theGxDevicePtr->SceneClear(3, clear);

        GxRsSet(GxRs_ColorWrite, 0);
        CWorldScene::RenderChunks();
        GxRsSet(GxRs_ColorWrite, 15);
        // NOTE: WMO depth is intentionally not used for the trees layer. A full WMO
        // depth pass over-occludes trees across the tile (interior floors/ceilings
        // write depth above the canopy). Terrain depth alone gives a usable trees
        // layer; WMO ordering is handled by the viewer.

        if (layer == 2) {  // WMO + non-tree M2 props
            MapBakerM2Filter = 2;
            CWorldScene::RenderMapObjDefGroups();
            RenderM2AndDetails(camPos, false);
        } else {  // common trees only
            MapBakerM2Filter = 1;
            RenderM2AndDetails(camPos, false);
        }
        MapBakerM2Filter = 0;
    }

    bool RenderTileInternal(int32_t tx, int32_t ty, int32_t sizePx, uint8_t* out, bool rgba, int32_t layer) {
        if (!s_rt || sizePx <= 0) {
            return false;
        }

        float centerX = 0.0f, centerY = 0.0f;
        TileWorldCenter(tx, ty, &centerX, &centerY);

        // Quads-only layer: 2D minimap art on a transparent background. No 3D pass.
        if (layer == 4) {
            const uint32_t n = s_rtSize;
            std::vector<uint8_t> full(static_cast<size_t>(n) * n * 4, 0);
            CompositeArt(full.data(), n, centerX, centerY, true);
            BoxDownsampleRgba(full.data(), static_cast<int32_t>(n), static_cast<int32_t>(n),
                              out, sizePx, sizePx);
            return true;
        }

        // Terrain height at the center for the camera altitude.
        float terrainH = 0.0f;
        C3Vector query(centerX, centerY, 0.0f);
        if (!CMap::QueryHeight(query, 1000.0f, &terrainH)) {
            terrainH = 0.0f;
        }

        // Street reference for the trick guard / WMO dump. Sampled here (lazy) because the
        // map chunks are only guaranteed to be loaded once the world is updating (a
        // startup-time SampleStreetHeights may hit unloaded chunks and read 0).
        const uint32_t adtIndex = 64u * static_cast<uint32_t>(ty) + static_cast<uint32_t>(tx);
        if (s_cityMap[adtIndex] && MapBakerXrayStreetH[adtIndex] == 0.0f && terrainH != 0.0f) {
            MapBakerXrayStreetH[adtIndex] = terrainH;
        }

        const float camZ = terrainH + 1000.0f;
        C3Vector camPos(centerX, centerY, camZ);
        C3Vector camTarget(centerX, centerY, camZ - 1.0f);

        // Re-apply the fade override in case it was reset.
        ApplyFadeDistances();

        // Top-down view. The engine convention is a ROTATION-ONLY view matrix (eye at the
        // origin); the camera position is carried by CWorldScene::s_activeWorldView and is
        // applied to terrain by the per-chunk world matrix (chunkPos - s_activeWorldView, see
        // CMapRenderChunk::RenderSetup). Passing a full look-at here makes CWorldScene::Update
        // double-count the camera position (it does viewMatrix.Translate(-s_activeWorldView)
        // again), which pushes the frustum ~2x camPos away and culls every chunk.
        C3Vector origin(0.0f, 0.0f, 0.0f);
        C3Vector forward = camTarget - camPos; // (0, 0, -1): straight down
        C44Matrix view;
        GxuXformCreateLookAtSgCompat(origin, forward, C3Vector(1.0f, 0.0f, 0.0f), view);
        GxXformSetView(view);

        // Ortho projection spanning exactly one ADT (533.333 units) so the tile fills the target.
        const float half = kAdtSize * 0.5f;
        // Diagnostic: shift the ortho window in world X (+ = north). Used to tell a
        // world-space boundary line (moves with the window) from a screen row-0 artifact
        // (stays at row 0).
        float shiftX = 0.0f;
        if (const char* s = getenv("MAPBAKER_SHIFT_X")) {
            shiftX = atof(s);
        }
        C44Matrix projection;
        GxuXformCreateOrtho(-half, half, -half + shiftX, half + shiftX, 1.0f, 4000.0f, projection);
        GxXformSetProjection(projection);

        // Bind the render target (the engine auto-creates a matching depth surface) and use a
        // full-target viewport.
        CGxTex* oldTarget = nullptr;
        g_theGxDevicePtr->DeviceGetRenderTarget(GxBuffers_Color, oldTarget);
        g_theGxDevicePtr->DeviceSetRenderTarget(GxBuffers_Color, s_rt, 0);

        GxXformSetViewport(0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f);
        CShaderEffect::UpdateProjMatrix();

        g_theGxDevicePtr->RsPush();

        // City ADTs render with the (higher, flatter) city sun elevation.
        const float savedElev = s_lightElev;
        if (s_cityElev >= 0.0f && s_cityMap[64 * ty + tx]) {
            s_lightElev = s_cityElev;
        }

        // Update the world (frustum, AOI, areas, day/night) for this camera.
        UpdateWorld(camPos, camTarget, tx, ty);

        // Diagnostic (MAPBAKER_DEBUG_VIEW=1): verify the per-tile camera translation reached
        // the engine. The Y component must move ~533 units per tx step; a constant Y here
        // means the draw passes are ignoring the camera's Y motion.
        if (getenv("MAPBAKER_DEBUG_VIEW")) {
            const CGxMatrixStack& vstack = g_theGxDevicePtr->m_xforms[GxXform_View];
            const C44Matrix& vm = vstack.m_mtx[vstack.m_level];
            const C44Matrix& pm = g_theGxDevicePtr->m_projNative;
            fprintf(stderr,
                    "MapBaker dbg: pre-draw  tile (%d,%d) cam=(%.1f,%.1f,%.1f) "
                    "saw=(%.1f,%.1f,%.1f) viewT=(%.3f,%.3f,%.3f)\n",
                    tx, ty,
                    camPos.x, camPos.y, camPos.z,
                    CWorldScene::s_activeWorldView.x, CWorldScene::s_activeWorldView.y,
                    CWorldScene::s_activeWorldView.z, vm.d0, vm.d1, vm.d2);
            // Full view rotation + native projection scales: a stale/tilted view
            // rotation in one mode only would shift screen content by an amount
            // proportional to world coordinates (the serve Y-parallax signature).
            fprintf(stderr,
                    "MapBaker dbg: view=(%.4f,%.4f,%.4f,%.4f | %.4f,%.4f,%.4f,%.4f | %.4f,%.4f,%.4f,%.4f | %.4f,%.4f,%.4f,%.4f)\n",
                    vm.a0, vm.a1, vm.a2, vm.a3,
                    vm.b0, vm.b1, vm.b2, vm.b3,
                    vm.c0, vm.c1, vm.c2, vm.c3,
                    vm.d0, vm.d1, vm.d2, vm.d3);
            fprintf(stderr,
                    "MapBaker dbg: proj=(%.4f,%.4f,%.4f,%.4f | %.4f,%.4f,%.4f,%.4f | %.4f,%.4f,%.4f,%.4f | %.4f,%.4f,%.4f,%.4f)\n",
                    pm.a0, pm.a1, pm.a2, pm.a3,
                    pm.b0, pm.b1, pm.b2, pm.b3,
                    pm.c0, pm.c1, pm.c2, pm.c3,
                    pm.d0, pm.d1, pm.d2, pm.d3);
            fflush(stderr);
        }

        s_lightElev = savedElev;

        GxXformSetViewport(0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f);
        CShaderEffect::UpdateProjMatrix();

        MapBakerDebugTileTag = ty * 64 + tx;
        // Optional diagnostic capture: render the WMO overlay over black and white and
        // solve per-pixel coverage from the difference. Off by default (effect models are
        // filtered instead); enable with MAPBAKER_WMO_MATTE. The native alpha path is used
        // for trees because their wind animation would smear the two captures.
        const bool matteSolve = rgba && layer == 2 && getenv("MAPBAKER_WMO_MATTE");
        std::vector<uint8_t> matteBlack;
        std::vector<uint8_t> matteWhite;
        bool matteReadOk = true;
        if (layer == 1) {
            RenderTerrainLayer();
        } else if (layer == 2 || layer == 3) {
            RenderOverlayLayer(camPos, layer, 0);
            if (matteSolve) {
                matteBlack.resize(static_cast<size_t>(s_rtSize) * s_rtSize * 3);
                matteReadOk = ReadRtToRgb(matteBlack.data(), s_rtSize);
                if (matteReadOk) {
                    // The first WMO pass drains the exterior group list. Re-run the world
                    // update so the second pass's cull sees the same loaded groups.
                    UpdateWorld(camPos, camTarget, tx, ty);
                    RenderOverlayLayer(camPos, layer, 255);
                    matteWhite.resize(static_cast<size_t>(s_rtSize) * s_rtSize * 3);
                    matteReadOk = ReadRtToRgb(matteWhite.data(), s_rtSize);
                }
            }
        } else {
            RenderWorld();
            RenderM2AndDetails(camPos);
        }

        g_theGxDevicePtr->RsPop();

        // Diagnostic: check the draw passes did not clobber the camera state.
        if (getenv("MAPBAKER_DEBUG_VIEW")) {
            fprintf(stderr, "MapBaker dbg: post-draw tile (%d,%d) saw=(%.1f,%.1f,%.1f)\n",
                    tx, ty, CWorldScene::s_activeWorldView.x, CWorldScene::s_activeWorldView.y,
                    CWorldScene::s_activeWorldView.z);
            fflush(stderr);
        }

        // Restore the previous render target before reading back.
        g_theGxDevicePtr->DeviceSetRenderTarget(GxBuffers_Color, oldTarget, 0);

        // Read back at full resolution, then box-downsample to the requested size.
        const int32_t channels = rgba ? 4 : 3;
        std::vector<uint8_t> full(static_cast<size_t>(s_rtSize) * s_rtSize * channels);
        bool readOk = matteReadOk;
        if (matteSolve) {
            if (readOk) {
                const size_t count = static_cast<size_t>(s_rtSize) * s_rtSize;
                for (size_t i = 0; i < count; i++) {
                    const int32_t dr = static_cast<int32_t>(matteWhite[i * 3 + 0]) - matteBlack[i * 3 + 0];
                    const int32_t dg = static_cast<int32_t>(matteWhite[i * 3 + 1]) - matteBlack[i * 3 + 1];
                    const int32_t db = static_cast<int32_t>(matteWhite[i * 3 + 2]) - matteBlack[i * 3 + 2];
                    int32_t alpha = 255 - (dr + dg + db) / 3;
                    if (alpha < 0) alpha = 0;
                    if (alpha > 255) alpha = 255;
                    if (alpha <= 1) {
                        full[i * 4 + 0] = 0;
                        full[i * 4 + 1] = 0;
                        full[i * 4 + 2] = 0;
                        full[i * 4 + 3] = 0;
                    } else {
                        // The black matte capture is premultiplied; recover straight colour.
                        const uint32_t inv = (255u * 255u) / static_cast<uint32_t>(alpha);
                        for (int32_t c = 0; c < 3; c++) {
                            const uint32_t v = static_cast<uint32_t>(matteBlack[i * 3 + c]) * inv / 255u;
                            full[i * 4 + c] = static_cast<uint8_t>(v > 255u ? 255u : v);
                        }
                        full[i * 4 + 3] = static_cast<uint8_t>(alpha);
                    }
                }
            }
        } else {
            readOk = rgba ? ReadRtToRgba(full.data(), s_rtSize)
                          : ReadRtToRgb(full.data(), s_rtSize);
        }
        if (!readOk) {
            fprintf(stderr, "MapBaker: readback failed for tile (%d, %d)\n", tx, ty);
            return false;
        }

        // MapBaker diagnostic (MAPBAKER_DEBUG_VIEW=1): 16x16 grid of full-res RT block means
        // (raw, before the row-0 fix), to see WHERE the pixels differ between modes.
        if (!rgba && getenv("MAPBAKER_DEBUG_VIEW")) {
            const uint32_t g = 16;
            const uint32_t bw = s_rtSize / g, bh = s_rtSize / g;
            for (uint32_t gy = 0; gy < g; gy++) {
                char line[256];
                int p = 0;
                for (uint32_t gx = 0; gx < g; gx++) {
                    uint64_t sum = 0;
                    const uint8_t* row0 = full.data() + ((size_t)(gy * bh + 1)) * s_rtSize * 3;
                    for (uint32_t y = 1; y < bh - 1; y++) {
                        const uint8_t* r = row0 + (size_t)y * s_rtSize * 3;
                        for (uint32_t x = 1; x < bw - 1; x++) {
                            sum += r[(size_t)(gx * bw + x) * 3];
                        }
                    }
                    p += snprintf(line + p, sizeof(line) - p, "%3u ",
                                  (uint32_t)(sum / ((size_t)(bw - 2) * (bh - 2))));
                }
                fprintf(stderr, "MapBaker dbg: rtgrid tile(%d,%d) y%u: %s\n", tx, ty, gy, line);
            }
            fflush(stderr);
        }

        // Diagnostic: print |row[i]-row[i-1]| for the top/bottom rows of the full-res RT.
        if (!rgba && getenv("MAPBAKER_DUMP_EDGES")) {
            const size_t rowBytes = static_cast<size_t>(s_rtSize) * 3;
            const uint8_t* p = full.data();
            for (uint32_t i = 1; i <= 16; i++) {
                double sum = 0.0;
                for (size_t x = 0; x < rowBytes; x++) {
                    sum += static_cast<double>(
                        std::abs(static_cast<int>(p[i * rowBytes + x]) - static_cast<int>(p[(i - 1) * rowBytes + x])));
                }
                fprintf(stderr, "  edges: RT row %2u: |d| mean=%.2f\n", i, sum / static_cast<double>(rowBytes));
            }
            for (int32_t i = static_cast<int32_t>(s_rtSize) - 1; i >= static_cast<int32_t>(s_rtSize) - 17; i--) {
                double sum = 0.0;
                for (size_t x = 0; x < rowBytes; x++) {
                    sum += static_cast<double>(
                        std::abs(static_cast<int>(p[i * rowBytes + x]) - static_cast<int>(p[(i - 1) * rowBytes + x])));
                }
                fprintf(stderr, "  edges: RT row %4d: |d| mean=%.2f\n", i, sum / static_cast<double>(rowBytes));
            }
        }

        // Fix the boundary clip-plane artifact. When the ortho top clip plane coincides exactly
        // with the ADT boundary (the topmost terrain vertex row), the D3D pipeline rasterizes that
        // single boundary row as a bright 1-px outlier (verified: RT row 0 |d| ~36, rows 1+ ~5).
        // The true boundary value is best approximated by the adjacent row, so copy row 1 over the
        // corrupted row 0. Applied per-tile, this removes the horizontal seam at every ADT boundary.
        {
            const size_t rowBytes = static_cast<size_t>(s_rtSize) * channels;
            memcpy(full.data(), full.data() + rowBytes, rowBytes); // row 0 := row 1
        }

        // The same clip-plane artifact exists on the left edge, but only at certain tile
        // alignments (in Northrend every third ADT, tx % 3 == 0): the first RT column samples
        // the ortho clip plane and gets the fog/clear colour instead of terrain/liquid, which
        // becomes a bright 1-px vertical line after the 2x box downsample. Verify with
        // --px 2048 --ssaa 1 on tile 571/42,16: RT col 0 is ~(86,179,212), col 1 is ~(27,51,79).
        // Copy column 1 over column 0 at full RT resolution so the downsample pair is clean.
        {
            const size_t rowBytes = static_cast<size_t>(s_rtSize) * channels;
            for (uint32_t y = 0; y < s_rtSize; y++) {
                uint8_t* row = full.data() + static_cast<size_t>(y) * rowBytes;
                memcpy(row, row + channels, channels); // column 0 := column 1
            }
        }

        // Report coverage (share of pixels that differ from the horizon/fog clear color). A
        // value near 0% means the tile rendered as empty fog; a healthy terrain tile is high.
        if (!rgba) {
            const uint8_t fr = DayNight::GetInfo()->m_fog.m_color.r;
            const uint8_t fg = DayNight::GetInfo()->m_fog.m_color.g;
            const uint8_t fb = DayNight::GetInfo()->m_fog.m_color.b;
            const uint32_t n = s_rtSize * s_rtSize;
            uint32_t covered = 0;
            for (uint32_t i = 0; i < n; i++) {
                const uint8_t r = full[i * 3 + 0], g = full[i * 3 + 1], b = full[i * 3 + 2];
                if (r != fr || g != fg || b != fb) {
                    covered++;
                }
            }
            const uint32_t chunks = CWorldScene::s_chunksRendered;
            fprintf(stderr, "MapBaker: tile (%d,%d): %u chunks, %.1f%% covered\n",
                    tx, ty, chunks, (100.0 * double(covered)) / double(n));
        }

        // Optional brightness gain (matches the brighter baked-minimap tone).
        if (s_gain != 1.0f) {
            const uint32_t n = s_rtSize * s_rtSize;
            for (uint32_t i = 0; i < n; i++) {
                uint8_t* p = full.data() + static_cast<size_t>(i) * channels;
                p[0] = static_cast<uint8_t>(std::min(255.0f, p[0] * s_gain));
                p[1] = static_cast<uint8_t>(std::min(255.0f, p[1] * s_gain));
                p[2] = static_cast<uint8_t>(std::min(255.0f, p[2] * s_gain));
            }
        }

        // 2D WMO quad overlay (city "quads + terrain" look): composite the selected group
        // cells over the (optionally WMO) render, at full RT res so the downsample
        // anti-aliases the edges. Art mode draws the real per-cell minimap .blp textures;
        // tint mode draws the flat per-class rectangles (diagnostic fallback).
        // No-op unless MapBakerQuadOn and the table is populated.
        if (layer == 0) {
            if (MapBakerArtOn) {
                CompositeArt(full.data(), s_rtSize, centerX, centerY, false);
            } else {
                CompositeQuads(full.data(), s_rtSize, centerX, centerY);
            }
        }

        if (rgba) {
            BoxDownsampleRgba(full.data(), static_cast<int32_t>(s_rtSize), static_cast<int32_t>(s_rtSize),
                              out, sizePx, sizePx);
        } else {
            BoxDownsampleRgb(full.data(), static_cast<int32_t>(s_rtSize), static_cast<int32_t>(s_rtSize),
                             out, sizePx, sizePx);
        }
        return true;
    }

    // Set the 2D WMO quad overlay parameters (called by the serve /render handler). When
    // on, the 3D WMO draw is suppressed unless wmoUnder is set (terrain-only + quads).
    void SetQuadOverlay(uint8_t on, uint8_t wmoUnder, uint8_t alpha,
                        uint8_t extR, uint8_t extG, uint8_t extB,
                        uint8_t intR, uint8_t intG, uint8_t intB) {
        MapBakerQuadOn = on;
        MapBakerQuadWmoUnder = wmoUnder;
        MapBakerQuadAlpha = alpha;
        MapBakerQuadExtColor[0] = extR; MapBakerQuadExtColor[1] = extG; MapBakerQuadExtColor[2] = extB;
        MapBakerQuadIntColor[0] = intR; MapBakerQuadIntColor[1] = intG; MapBakerQuadIntColor[2] = intB;
        s_quadWmoSuppress = (on && !wmoUnder);
    }

    // Real 2D minimap art vs the flat tint fallback (see MapBakerArt.h). alpha is a global
    // multiplier on the cell textures' own alpha (0-255).
    void SetQuadArt(uint8_t on, uint8_t alpha) {
        MapBakerArtOn = on ? 1 : 0;
        MapBakerArtAlpha = alpha;
    }

    // Draw the WMO groups the client's viewpoint culling skipped (interior districts).
    void SetQuadFill(uint8_t on) {
        MapBakerQuadFill = on ? 1 : 0;
    }

    // Select / deselect a single quad by table index (web UI). Out-of-range is ignored.
    void SetQuadSelected(uint32_t index, bool selected) {
        if (index < MapBakerQuadCount) {
            MapBakerQuadTable[index].m_selected = selected ? 1 : 0;
        }
    }

    // Bulk selection by class: 0 = all off, 1 = all on, 2 = exterior only (0x8),
    // 3 = interior only (0x8 clear).
    void SetQuadSelectionByClass(uint32_t mode) {
        for (uint32_t i = 0; i < MapBakerQuadCount; i++) {
            const bool ext = (MapBakerQuadTable[i].m_smoFlags & 0x8) != 0;
            bool s;
            switch (mode) {
                case 0: s = false; break;
                case 1: s = true; break;
                case 3: s = !ext; break;
                default: s = ext; break; // 2 = exterior only
            }
            MapBakerQuadTable[i].m_selected = s ? 1 : 0;
        }
    }

    // Run a collection pass over the region: render each tile with MapBakerQuadCollect on
    // so the render hook fills MapBakerQuadTable. Returns the group count. Fast once the
    // scene is loaded (one render per tile; the pixels are discarded).
    uint32_t CollectQuads(int32_t tx0, int32_t tx1, int32_t ty0, int32_t ty1, uint32_t sizePx) {
        if (!s_rt || sizePx <= 0) {
            return 0;
        }

        MapBakerQuadCount = 0;
        // The collection hook lives inside the WMO draw, so force that pass on; and skip
        // the quad composite during collection (the pixels are discarded anyway).
        const bool savedWmoSuppress = s_quadWmoSuppress;
        const bool savedNoWmo = s_noWmo;
        const uint8_t savedOn = MapBakerQuadOn;
        s_quadWmoSuppress = false;
        s_noWmo = false;
        MapBakerQuadOn = 0;
        MapBakerQuadCollect = 1;

        std::vector<uint8_t> scratch(static_cast<size_t>(sizePx) * sizePx * 3);
        for (int32_t ty = ty0; ty <= ty1; ty++) {
            for (int32_t tx = tx0; tx <= tx1; tx++) {
                RenderTile(tx, ty, static_cast<int32_t>(sizePx), scratch.data());
            }
        }

        MapBakerQuadCollect = 0;
        s_quadWmoSuppress = savedWmoSuppress;
        s_noWmo = savedNoWmo;
        MapBakerQuadOn = savedOn;
        fprintf(stderr, "MapBaker: quad collection complete, %u groups\n", MapBakerQuadCount);
        return MapBakerQuadCount;
    }

    bool SelfTestReadback() {
        if (!s_rt) {
            return false;
        }

        const uint32_t size = s_rtSize;

        CGxTex* oldTarget = nullptr;
        g_theGxDevicePtr->DeviceGetRenderTarget(GxBuffers_Color, oldTarget);
        g_theGxDevicePtr->DeviceSetRenderTarget(GxBuffers_Color, s_rt, 0);

        // Clear to a known color (B=0x10, G=0x20, R=0x30, A=0xFF).
        CImVector clear(0x10, 0x20, 0x30, 0xFF);
        g_theGxDevicePtr->SceneClear(1, clear);
        g_theGxDevicePtr->DeviceSetRenderTarget(GxBuffers_Color, oldTarget, 0);

        std::vector<uint8_t> rgb(size * size * 3);
        if (!ReadRtToRgb(rgb.data(), size)) {
            return false;
        }

        // Sample the center pixel.
        const size_t idx = (size / 2) * size + (size / 2);
        if (rgb[idx * 3 + 0] != 0x30 || rgb[idx * 3 + 1] != 0x20 || rgb[idx * 3 + 2] != 0x10) {
            fprintf(stderr, "MapBaker: readback self-test failed (got %u,%u,%u want 48,32,16)\n",
                    rgb[idx * 3 + 0], rgb[idx * 3 + 1], rgb[idx * 3 + 2]);
            return false;
        }

        return true;
    }

    bool RenderTile(int32_t tx, int32_t ty, int32_t sizePx, uint8_t* outRgb) {
        return RenderTileInternal(tx, ty, sizePx, outRgb, false, 0);
    }

    bool RenderTileLayer(int32_t tx, int32_t ty, int32_t sizePx, uint8_t* out, bool rgba, int32_t layer) {
        return RenderTileInternal(tx, ty, sizePx, out, rgba, layer);
    }
}
