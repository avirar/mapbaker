#ifndef TOOLS_MAPBAKER_MAPBAKERXRAY_H
#define TOOLS_MAPBAKER_MAPBAKERXRAY_H

#include <cstdint>
#include <cstdio>

// X-ray "city floor" rendering knobs.
//
// Written once at startup by MapBaker (MapBakerWorld.cpp); read per-WMO-group by the
// (MapBaker-patched) CWorldScene::RenderMapObjDefGroups in WorldClient/World.cpp.
// The gate is evaluated per WMO (by its center ADT), so a building renders identically
// no matter which tile it appears in.
//
// Modes (MapBakerXrayMode):
//   0 = off        normal depth-tested WMO rendering (roofs win in top-down)
//   1 = depthoff   gated WMOs drawn with depth test/write disabled, natural group
//                  order (painter's algorithm; last-drawn wins)
//   2 = zsort      gated WMOs drawn with depth disabled, sorted by group world-Z so
//                  the chosen end is drawn LAST (and thus sits on top)
//   3 = zinv       gated WMOs Z-inverted (z -> 2*pivot - z) with backface culling
//                  off; depth stays on, so lower floors win the depth fight
extern uint32_t MapBakerXrayMode;
// zsort direction: 1 = lowest-Z group drawn last (x-ray floors), 0 = highest-Z drawn
// last (x-ray roofs). Ignored by the other modes.
extern uint32_t MapBakerXrayLowLast;
// zinv only: the WMO's view-depth d (camera-to-surface) is mapped to K - d. Needs
// roughly d_min + 1 < K < d_terrain + d_floor; 1500 works for a 1000-unit camera.
extern float MapBakerXrayK;
// Gate scope: 0 = all WMOs, 1 = only WMOs whose center ADT is flagged in the gate.
extern uint32_t MapBakerXrayScope;
// Gate bitmap, indexed 64*ty + tx (0..4095).
extern uint8_t MapBakerXrayGate[4096];
// Per-ADT street/terrain reference height (QueryHeight at the ADT center), indexed
// 64*ty + tx; 0 = unknown. Filled by MapBaker for the gated ADTs; used as the street
// plane by the trick-model guard.
extern float MapBakerXrayStreetH[4096];
// Trick/portal guard (x-ray on + gated WMOs only): WoW ships some buildings (e.g.
// Stormwind cathedral) as raised + inflated "distance" models that the client swaps
// based on the player's position. Top-down they blanket the city, so a gated WMO is
// skipped entirely (not drawn) when its bbox TOP is more than this many units above
// the street of its center ADT AND its bbox bottom is also more than 25 units above
// that street (a genuine building sits on the ground, so its bottom is at/below the
// street). 0 = off.
extern float MapBakerXraySkipAbove;
// 1 = dump a per-WMO table (name, flags, bbox, center ADT, gate state, guard state)
// at first sight of each WMO. Diagnostic only.
extern uint32_t MapBakerXrayDump;
// Optional FILE* the WMO dump is written to instead of stderr (serve mode opens
// <out>\wmo_dump.txt and serves it at /dump.txt). NULL = stderr.
extern FILE* MapBakerXrayDumpFile;
// Debug: current tile tag (ty*64+tx), set by MapBakerWorld::RenderTile before the
// draw; used to scope per-tile draw-time probes.
extern int32_t MapBakerDebugTileTag;

#endif
