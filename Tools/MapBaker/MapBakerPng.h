#ifndef TOOLS_MAPBAKER_MAPBAKERPNG_H
#define TOOLS_MAPBAKER_MAPBAKERPNG_H

#include <cstdint>

namespace MapBakerPng {
    // Write RGB8 image data (row-major, top row first) as a PNG using the repo's vendored zlib.
    // Writes to <path>.tmp and moves it into place atomically.
    bool Write(const char* path, int32_t width, int32_t height, const uint8_t* rgb);
}

#endif
