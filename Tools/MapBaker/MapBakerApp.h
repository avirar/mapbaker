#ifndef TOOLS_MAPBAKER_MAPBAKERAPP_H
#define TOOLS_MAPBAKER_MAPBAKERAPP_H

#include <cstdint>

// Engine bootstrap + render driver for MapBaker. Runs inside the OpenCode event context.
namespace MapBakerApp {
    struct Config {
        const char* clientPath;  // client data root (contains Data\ and WTF\)
        const char* mapArg;      // map directory name or numeric id
        const char* preset;      // "map" or "realistic"
        int32_t px;              // final tile resolution (square), default 1024
        int32_t ssaa;            // supersampling factor for the render target, default 2
        const char* outDir;      // output directory for PNG tiles
        int32_t tx0, ty0, tx1, ty1; // inclusive ADT tile range
        bool allTiles;           // render the whole continent (from the area table)
        bool dumpTiles;          // dump the ADT grid from the WDT area table, then exit
        bool noWmo;              // skip WMO/building rendering (ground-floor look)
        float lightElev;         // map-light elevation in degrees (-1 = engine default noon)
        float gain;              // post-render brightness gain (1.0 = no change)
        const char* cityFile;    // ADT list ("tx ty" per line) rendered with the city sun
        float cityElev;          // light elevation for city ADTs (-1 = use lightElev)
        uint32_t xrayMode;       // 0 off, 1 depthoff, 2 zsort, 3 zinv (x-ray city floors)
        uint32_t xrayLowLast;    // zsort: 1 = lowest Z drawn last (floors on top)
        float xrayK;             // zinv depth remap (default 1500)
        uint32_t xrayScope;      // 0 = all WMOs, 1 = city ADTs only (default)
        float xraySkipAbove;     // trick-model guard: skip gated WMOs this high above the
                                 // street AND floating above it (0 = off)
        uint32_t xrayDump;       // 1 = dump the per-WMO table at first sight
        bool quads;              // 2D WMO quad overlay (collect group footprints + composite)
        bool quadArt;            // draw the real per-cell minimap .blp art (default) vs flat tints
        bool quadWmo;            // also render the 3D WMO underneath the quads (hybrid)
        bool quadFill;           // with the 3D WMO: also draw the groups the viewpoint cull skips
        float quadAlpha;         // quad alpha, 0..1 (default ~0.63)
        uint32_t quadSel;        // bulk selection: 0 none, 1 all, 2 exterior (default), 3 interior
        const char* quadDump;    // optional: write the collected group table to this file
        bool skipExisting;       // skip tiles whose PNG already exists (resume / parallel workers)
        const char* tileListFile; // optional text file of "tx ty" lines (parallel worker assignment)
        const char* quadCache;   // optional quad-table cache: load if present, else save after collect
        bool serve;              // interactive mode: local web tuner, re-render on demand
        int32_t port;            // HTTP port for serve mode (default 8742)
    };

    void SetConfig(const Config& config);

    // Event context init/destroy callbacks.
    int32_t InitializeEngine(const void* data, void* param);
    int32_t DestroyEngine(const void* data, void* param);
}

#endif
