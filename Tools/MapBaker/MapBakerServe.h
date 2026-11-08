#ifndef TOOLS_MAPBAKER_MAPBAKERSERVE_H
#define TOOLS_MAPBAKER_MAPBAKERSERVE_H

#include "MapBakerApp.h"

// Interactive mode: the scene stays loaded and the tile region is re-rendered on demand
// with different settings (x-ray mode/K, trick guard, gain, light, resolution). A small
// local web page (settings panel + montage/crop PNGs + named presets) is served over
// HTTP on 127.0.0.1. The HTTP server runs on its own thread; all engine work (settings
// apply + tile render) happens on the event thread via the idle callback registered in
// Start, so the D3D device is never touched cross-thread.
namespace MapBakerServe {
    // Called from MapBakerApp::InitializeEngine after world + render-target setup.
    // Binds 127.0.0.1:<config.port>, starts the server thread, queues the initial
    // render job (the CLI settings) and registers the idle callback.
    // Returns false if the port could not be bound.
    bool Start(const MapBakerApp::Config& config);

    // Stop the server (event thread; called from DestroyEngine).
    void Stop();
}

#endif
