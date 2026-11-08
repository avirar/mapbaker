#ifndef TOOLS_MAPBAKER_MAPBAKERQUAD_H
#define TOOLS_MAPBAKER_MAPBAKERQUAD_H

#include <cstdint>

// "3D terrain + 2D WMO quads" overlay (MapBaker).
//
// The city WMOs are drawn as FLAT 2D footprint quads (one axis-aligned screen rect per
// WMO group) composited over the 3D terrain render, matching the original minimap's
// "quads + terrain" look. This avoids the perspective-trick distance models (e.g. the
// comically-large Stormwind cathedral facade, SMO group "cathy_facade01") and occlusion.
//
// The client already classifies each SMO group interior vs exterior via the group's raw
// flags (CMapObjGroup::m_flags): 0x8 set = EXTERIOR (drawn on the outdoor minimap),
// 0x8 clear = INTERIOR (only shown once physically inside, recursed through portals).
// See CMapObj::QueryMapObjMinimapGroup and CMapEntity::QueryInterior.
//
// Population: the (MapBaker-patched) CWorldScene::RenderMapObjDefGroups hook in
// WorldClient/World.cpp records every WMO group it visits into MapBakerQuadTable when
// MapBakerQuadCollect is set (deduped by (def pointer, groupNum)). The table is filled by
// a collection render pass over the region and read by MapBakerWorld (compositing) and the
// serve (/groups.json). The user selects which groups are "applied" (drawn as quads) via
// the web UI; that selection is stored in each entry's m_selected.
//
// Defined in MapBakerWorld.cpp; declared here for the World.cpp hook and the serve.

struct MapBakerQuadEntry {
    /* 0000 */ const void* m_def;      // CMapObjDef* (dedup key together with m_groupNum)
    /* 0008 */ int32_t m_groupNum;     // SMO group number within the WMO
    /* 000C */ char m_wmoName[64];     // WMO file path (as stored in CMapObj::m_wmoName)
    /* 004C */ char m_smoName[40];     // SMO group name (CMapObjGroup::m_groupName)
    /* 0074 */ float m_x0, m_x1;       // group world bbox, X
    /* 007C */ float m_y0, m_y1;       // group world bbox, Y
    /* 0084 */ float m_z0, m_z1;       // group world bbox, Z (informational)
    /* 008C */ uint32_t m_smoFlags;    // raw SMO group flags (0x8 ext / 0x40 int-class / 0x8000 int-fog)
    /* 0090 */ uint8_t m_selected;     // 1 = drawn as a 2D quad in the overlay
    /* 0091 */ uint8_t m_pad[3];
    // LOCAL (WMO) group bbox -- CMapObjGroup::m_bbox. This is the space the client's
    // minimap quad grid is built in (CMapEntity::QueryMapObjMinimap transforms the query
    // into local space; GetMinimapQuads tiles the local bbox). The cell indices in the
    // .blp names ONLY correspond to this local grid.
    float m_lx0, m_lx1;
    float m_ly0, m_ly1;
    // Instance matrix (mapObjDef->m_mat), local XY -> world XY:
    //   wx = m_ma*lx + m_mb*ly + m_mtx
    //   wy = m_mc*lx + m_md*ly + m_mty
    // (matches C3Vector operator*(v, m): x' = a0*x + b0*y + d0; y' = a1*x + b1*y + d1)
    float m_ma, m_mb, m_mc, m_md, m_mtx, m_mty;
};

// Group table (filled by the collection pass). Index == the stable id the web UI uses.
extern MapBakerQuadEntry MapBakerQuadTable[8192];
extern uint32_t MapBakerQuadCount;
extern uint32_t MapBakerQuadCap;
// 1 = the render hook records every visited WMO group into the table (collection pass).
extern uint8_t MapBakerQuadCollect;
// 1 = composite the selected quads into the tile readback (the 2D overlay is on).
extern uint8_t MapBakerQuadOn;
// 1 = also render the 3D WMO geometry underneath the quads (0 = terrain-only + quads).
extern uint8_t MapBakerQuadWmoUnder;
// Flat tint per class (RGB 0-255) and a shared alpha (0-255). Exterior vs interior get
// different colors so the user can see the classification while curating.
extern uint8_t MapBakerQuadExtColor[3];
extern uint8_t MapBakerQuadIntColor[3];
extern uint8_t MapBakerQuadAlpha;

#endif
