#include "MapBakerArt.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>

#include "Engine/Source/BLPFile/blp.h"
#include "Engine/Source/BlizzardCore/SFile2-Core.h"
#include "Engine/Source/Services/Types.h"

uint8_t MapBakerArtOn = 0;
uint8_t MapBakerArtAlpha = 255;

namespace {
    std::unordered_map<std::string, std::string> s_trs;  // lower-case name -> md5 file name
    bool s_trsTried = false;
    bool s_trsLoaded = false;

    std::unordered_map<std::string, MapBakerArtImage> s_cache;
    size_t s_cacheBytes = 0;
    // A continent's worth of distinct cells can run to hundreds of MB. The old 768 MB
    // bound evicted the cache mid-mosaic (wholesale delete of m_px) and could invalidate
    // an image pointer still in use by the current tile. 8 GB keeps a full continent
    // resident in each baker process; the host has 128 GB and at most a few workers.
    const size_t kCacheLimitBytes = 8ull * 1024ull * 1024ull * 1024ull;
    uint32_t s_looked = 0;
    uint32_t s_hit = 0;
    uint32_t s_missing = 0;

    void ClearCache() {
        for (auto& kv : s_cache) {
            delete[] kv.second.m_px;
        }
        s_cache.clear();
        s_cacheBytes = 0;
    }

    std::string Lower(const std::string& in) {
        std::string out(in);
        for (char& c : out) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        return out;
    }

    // "WORLD\WMO\AZEROTH\...\STORMWIND.WMO" -> "WMO\AZEROTH\...\STORMWIND"
    // (the client's MinimapSetMapObjName strips the leading "World\" component and the
    // extension from QueryMapObjFileName; the result is the trs key prefix).
    std::string WmoKeyBase(const char* wmoName) {
        std::string s(wmoName ? wmoName : "");
        const size_t slash = s.find_first_of("\\/:");
        if (slash != std::string::npos && Lower(s.substr(0, slash)) == "world") {
            s = s.substr(slash + 1);
        }
        const size_t dot = s.rfind('.');
        if (dot != std::string::npos) {
            s = s.substr(0, dot);
        }
        return s;
    }

    // Cell texture selection, copied from CMapObj::GetMinimapQuads (MapObj.cpp):
    // texel = 0.5 world units, texels per cell = 2^ceil(log2(extent*256)) clamped 32..256.
    uint32_t CellTexels(float extent) {
        if (!(extent > 0.0f)) {
            return 32;
        }
        float exponent = std::log2(extent * 256.0f);
        if (exponent < 5.0f) {
            exponent = 5.0f;   // 1 << 5 = 32
        }
        if (exponent > 8.0f) {
            exponent = 8.0f;   // 1 << 8 = 256
        }
        const int32_t ceilExp = static_cast<int32_t>(std::ceil(exponent));
        const uint32_t p = 1u << ceilExp;
        return p < 32 ? 32 : (p > 256 ? 256 : p);
    }

    const MapBakerArtImage* LoadCell(const char* wmoName, int32_t groupNum,
                                     int32_t cellX, int32_t cellY) {
        char name[320];
        snprintf(name, sizeof(name), "%s_%03d_%02d_%02d.blp",
                 WmoKeyBase(wmoName).c_str(), groupNum, cellX, cellY);
        const std::string key(name);

        auto cached = s_cache.find(key);
        if (cached != s_cache.end()) {
            return &cached->second;
        }

        if (s_cacheBytes > kCacheLimitBytes) {
            ClearCache();
        }
        s_looked++;

        // The trs keys are full minimap paths without the "World\" prefix. Try with and
        // without the prefix (some tables keep the full path).
        std::string lookup = Lower(key);
        auto it = s_trs.find(lookup);
        if (it == s_trs.end()) {
            it = s_trs.find(Lower(std::string("World\\") + key));
        }
        if (it == s_trs.end()) {
            // wowmin's discovery finding: the quad base is not always the .wmo
            // basename -- instance WMOs drop an "_instanceN" suffix
            // (e.g. wailingcaverns_instance.wmo -> wailingcaverns_<grp>_<x>_<y>).
            std::string base = WmoKeyBase(wmoName);
            const size_t us = base.rfind('_');
            if (us != std::string::npos) {
                const std::string tail = Lower(base.substr(us + 1));
                bool instanceSuffix = (tail == "instance");
                if (!instanceSuffix && tail.rfind("instance", 0) == 0) {
                    instanceSuffix = true;
                    for (size_t k = 8; k < tail.size(); k++) {
                        if (!std::isdigit(static_cast<unsigned char>(tail[k]))) {
                            instanceSuffix = false;
                            break;
                        }
                    }
                }
                if (instanceSuffix) {
                    char alt[320];
                    snprintf(alt, sizeof(alt), "%s_%03d_%02d_%02d.blp",
                             base.substr(0, us).c_str(), groupNum, cellX, cellY);
                    it = s_trs.find(Lower(alt));
                }
            }
        }
        if (it == s_trs.end()) {
            static uint32_t s_missLogged = 0;
            if (getenv("MAPBAKER_ART_DUMP") && s_missLogged++ < 12) {
                fprintf(stderr, "MapBaker art miss: key '%s'\n", key.c_str());
            }
        }

        MapBakerArtImage image = { 0, 0, nullptr };
        if (it != s_trs.end() && !it->second.empty()) {
            const std::string path = std::string("Textures\\Minimap\\") + it->second;
            CBLPFile blp;
            if (blp.Open(path.c_str(), 1)) {
                uint8_t* pixels = nullptr;
                uint32_t stride = 0;
                if (blp.Lock(PIXEL_ARGB8888, 0, pixels, stride) && pixels) {
                    const int32_t w = static_cast<int32_t>(blp.Width(0));
                    const int32_t h = static_cast<int32_t>(blp.Height(0));
                    if (w > 0 && h > 0) {
                        image.m_w = w;
                        image.m_h = h;
                        image.m_px = new uint8_t[static_cast<size_t>(w) * h * 4];
                        for (int32_t y = 0; y < h; y++) {
                            memcpy(image.m_px + static_cast<size_t>(y) * w * 4,
                                   pixels + static_cast<size_t>(y) * stride,
                                   static_cast<size_t>(w) * 4);
                        }
                    }
                }
                blp.Unlock(0);
                blp.Close();
            }
        }

        if (image.m_px) {
            s_hit++;
            s_cacheBytes += static_cast<size_t>(image.m_w) * image.m_h * 4;
        } else {
            s_missing++;
        }
        auto inserted = s_cache.emplace(key, image);
        return &inserted.first->second;
    }
}

bool MapBakerArtInit() {
    if (s_trsTried) {
        return s_trsLoaded;
    }
    s_trsTried = true;

    void* data = nullptr;
    uint32_t size = 0;
    if (!SFile::LoadFile("Textures\\Minimap\\md5translate.trs", &data, &size, 1, nullptr) ||
            !data || size == 0) {
        fprintf(stderr, "MapBaker: minimap art: Textures\\Minimap\\md5translate.trs not found "
                        "(real minimap art unavailable)\n");
        return false;
    }

    const char* p = static_cast<const char*>(data);
    const char* end = p + size;
    bool firstLine = true;
    while (p < end) {
        const char* lineStart = p;
        while (p < end && *p != '\r' && *p != '\n') {
            p++;
        }
        std::string line(lineStart, static_cast<size_t>(p - lineStart));
        while (p < end && (*p == '\r' || *p == '\n')) {
            p++;
        }
        if (firstLine && line.size() >= 3 &&
                static_cast<unsigned char>(line[0]) == 0xEF &&
                static_cast<unsigned char>(line[1]) == 0xBB &&
                static_cast<unsigned char>(line[2]) == 0xBF) {
            line.erase(0, 3);   // UTF-8 BOM
        }
        firstLine = false;
        if (line.empty() || line.rfind("dir:", 0) == 0) {
            continue;
        }
        const size_t tab = line.find('\t');
        if (tab == std::string::npos) {
            continue;
        }
        std::string key = line.substr(0, tab);
        std::string value = line.substr(tab + 1);
        while (!value.empty() && (value.back() == '\r' || value.back() == '\n' ||
                                  value.back() == ' ' || value.back() == '\t')) {
            value.pop_back();
        }
        if (key.empty() || value.empty()) {
            continue;
        }
        s_trs[Lower(key)] = value;
    }
    SFile::Unload(data);

    s_trsLoaded = !s_trs.empty();
    fprintf(stderr, "MapBaker: minimap art: md5translate.trs loaded, %u name entries\n",
            static_cast<uint32_t>(s_trs.size()));

    if (getenv("MAPBAKER_ART_DUMP")) {
        uint32_t shown = 0;
        for (const auto& kv : s_trs) {
            if (shown++ < 12) {
                fprintf(stderr, "MapBaker art dump: '%s' -> '%s'\n",
                        kv.first.c_str(), kv.second.c_str());
            }
        }
        uint32_t sw = 0;
        for (const auto& kv : s_trs) {
            if (kv.first.rfind("wmo\\azeroth\\buildings\\stormwind\\", 0) == 0 && sw < 600) {
                fprintf(stderr, "MapBaker art sw: '%s'\n", kv.first.c_str());
                sw++;
            }
        }
        fprintf(stderr, "MapBaker art dump: %u stormwind entries shown\n", sw);
    }
    return s_trsLoaded;
}

uint32_t MapBakerArtCellGrid(float x0, float x1, float y0, float y1,
                             MapBakerArtCell* out, uint32_t maxCells) {
    if (!out || !maxCells || x1 <= x0 || y1 <= y0) {
        return 0;
    }

    const float inv = 1.0f / 128.0f;   // s_minimapQuadSize = 0.5 * 256
    const float sizeX = (x1 - x0) * inv;
    const float sizeY = (y1 - y0) * inv;
    const uint32_t texelsX = CellTexels(sizeX);
    const uint32_t texelsY = CellTexels(sizeY);
    const int32_t countX = static_cast<int32_t>(std::ceil(sizeX));
    const int32_t countY = static_cast<int32_t>(std::ceil(sizeY));
    const float stepX = texelsX * 0.5f;
    const float stepY = texelsY * 0.5f;
    const float border = 1.0f;   // s_minimapTexelSize * 2

    uint32_t count = 0;
    for (int32_t y = 0; y < countY; y++) {
        for (int32_t x = 0; x < countX; x++) {
            if (count >= maxCells) {
                return count;
            }
            float minX = x * stepX + x0;
            float minY = y * stepY + y0;
            float maxX = minX + stepX;
            float maxY = minY + stepY;
            if (!x) {
                minX -= border;
            }
            if (!y) {
                minY -= border;
            }
            if (x == countX - 1) {
                maxX += border;
            }
            if (y == countY - 1) {
                maxY += border;
            }
            MapBakerArtCell& cell = out[count++];
            cell.m_x = x;
            cell.m_y = y;
            cell.m_x0 = minX;
            cell.m_x1 = maxX;
            cell.m_y0 = minY;
            cell.m_y1 = maxY;
        }
    }
    return count;
}

const MapBakerArtImage* MapBakerArtGet(const char* wmoName, int32_t groupNum,
                                       int32_t cellX, int32_t cellY) {
    if (!s_trsLoaded) {
        return nullptr;
    }
    return LoadCell(wmoName, groupNum, cellX, cellY);
}

void MapBakerArtStats(uint32_t* looked, uint32_t* hit, uint32_t* missing) {
    if (looked) {
        *looked = s_looked;
    }
    if (hit) {
        *hit = s_hit;
    }
    if (missing) {
        *missing = s_missing;
    }
}
