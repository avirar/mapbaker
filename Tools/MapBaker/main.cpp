#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "Engine/Source/Base/Base.h"
#include "Engine/Source/BlizzardCore/SFile2-Core.h"
#include "Engine/Source/ConsoleBase/ConsoleCommand.h"
#include "Engine/Source/ConsoleBase/ConsoleVar.h"
#include "Engine/Source/Event/Event.h"
#include "Engine/Source/Os/OsFile.h"
#include "Engine/Source/Os/Path.h"
#include "Engine/Source/Os/Time.h"
#include "Engine/Source/Os/TimeManager.h"
#include "Engine/Source/Services/AsyncFile.h"
#include "Engine/Source/Storm/H/storm.h"
#include "Engine/Source/Tempest/C3Vector.h"
#include "WoW/Source/DB/DBClient/DBClient.h"
#include "WoW/Source/DB/Rec/Startup_StringsRec.h"
#include "WoW/Source/ConsoleDevice/ConsoleDevice.h"
#include "MapBakerApp.h"

static const char* s_baseArchives[] = {
    "common.MPQ",
    "common-2.MPQ",
    "expansion.MPQ",
    "lichking.MPQ",
};

static const char* s_localeArchives[] = {
    "locale-%s.MPQ",
    "expansion-locale-%s.MPQ",
    "lichking-locale-%s.MPQ",
};

static const char* s_locales[] = {
    "enUS", "enGB", "deDE", "frFR", "esES", "esMX", "ruRU", "koKR", "zhCN", "zhTW", "enCN", "enTW", "ptBR", "ptPT", "itIT",
};

static const char s_patchSuffixes[] = "23456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";

extern WowClientDB<Startup_StringsRec> g_startup_StringsDB;

static SArchive* s_archiveHandles[128];
static uint32_t s_archiveCount;
static char s_locale[8] = "enUS";

// The window size is irrelevant: MapBaker renders into an offscreen render target. A small
// window keeps the tool from covering the desktop.
static const char* s_defaultConfig =
    "SET gxWindow \"1\"\n"
    "SET gxMaximize \"0\"\n"
    "SET gxResolution \"512x512\"\n"
    "SET hwDetect \"0\"\n";

static bool ArchiveExists(const char* clientPath, const char* name) {
    char path[520];
    SStrPrintf(path, sizeof(path), "%sData\\%s", clientPath, name);
    FILE* file = fopen(path, "rb");

    if (!file) {
        return false;
    }

    fclose(file);
    return true;
}

static bool OpenArchive(const char* clientPath, const char* name, bool required) {
    if (!ArchiveExists(clientPath, name)) {
        if (required) {
            printf("MapBaker: %sData\\%s is missing\n", clientPath, name);
        }

        return !required;
    }

    if (s_archiveCount >= sizeof(s_archiveHandles) / sizeof(s_archiveHandles[0])) {
        return true;
    }

    char path[260];
    SStrPrintf(path, sizeof(path), "Data\\%s", name);

    if (!SFile::OpenArchive(path, 100 + s_archiveCount, 0, &s_archiveHandles[s_archiveCount])) {
        printf("MapBaker: SFile::OpenArchive(%s) failed, error %u\n", path, SErrGetLastError());
        return !required;
    }

    s_archiveCount++;
    return true;
}

static void DetectLocale(const char* clientPath) {
    for (uint32_t i = 0; i < sizeof(s_locales) / sizeof(s_locales[0]); i++) {
        char name[64];
        SStrPrintf(name, sizeof(name), "%s\\locale-%s.MPQ", s_locales[i], s_locales[i]);

        if (ArchiveExists(clientPath, name)) {
            SStrCopy(s_locale, s_locales[i], sizeof(s_locale));
            return;
        }
    }
}

static bool OpenArchives(const char* clientPath, const char* basePath) {
    SFile::DisableSFileCheckDisk();
    SFile::EnableDirectAccess(1);
    SFile::SetBasePath(clientPath);
    SFile::SetDataPath("Data\\");
    DetectLocale(clientPath);

    char name[64];

    for (uint32_t i = 0; i < sizeof(s_baseArchives) / sizeof(s_baseArchives[0]); i++) {
        if (!OpenArchive(clientPath, s_baseArchives[i], i == 0)) {
            return false;
        }
    }

    for (uint32_t i = 0; i < sizeof(s_localeArchives) / sizeof(s_localeArchives[0]); i++) {
        char file[64];
        SStrPrintf(file, sizeof(file), s_localeArchives[i], s_locale);
        SStrPrintf(name, sizeof(name), "%s\\%s", s_locale, file);
        OpenArchive(clientPath, name, false);
    }

    OpenArchive(clientPath, "patch.MPQ", false);

    for (const char* suffix = s_patchSuffixes; *suffix; suffix++) {
        SStrPrintf(name, sizeof(name), "patch-%c.MPQ", *suffix);
        OpenArchive(clientPath, name, false);
    }

    SStrPrintf(name, sizeof(name), "%s\\patch-%s.MPQ", s_locale, s_locale);
    OpenArchive(clientPath, name, false);

    for (const char* suffix = s_patchSuffixes; *suffix; suffix++) {
        SStrPrintf(name, sizeof(name), "%s\\patch-%s-%c.MPQ", s_locale, s_locale, *suffix);
        OpenArchive(clientPath, name, false);
    }

    SFile::SetBasePath(basePath);
    return true;
}

static void WriteDefaultConfig(const char* basePath) {
    char path[260];
    SStrPrintf(path, sizeof(path), "%sWTF\\MapBaker.wtf", basePath);
    FILE* file = fopen(path, "rb");

    if (file) {
        fclose(file);
        return;
    }

    char directory[260];
    SStrPrintf(directory, sizeof(directory), "%sWTF", basePath);
    OsCreateDirectory(directory, 0);
    file = fopen(path, "wb");

    if (file) {
        fputs(s_defaultConfig, file);
        fclose(file);
    }
}

static void Usage() {
    printf("usage: MapBaker64 [options]\n");
    printf("  --client <path>     client data root (default: exe directory)\n");
    printf("  --map <dir|id>      map directory name or numeric id (required)\n");
    printf("  --preset <name>     map (default) | realistic\n");
    printf("  --layer <name>      combined (default) | terrain | wmo | trees | quads\n");
    printf("  --px <n>            tile resolution in pixels (default 1024)\n");
    printf("  --ssaa <n>          supersampling factor (default 2)\n");
    printf("  --out <dir>         output directory for PNG tiles (default: exe dir)\n");
    printf("  --tx <n> --ty <n>   single ADT tile (tx, ty)\n");
    printf("  --tx0 <n> --ty0 <n> --tx1 <n> --ty1 <n>  inclusive ADT tile range\n");
    printf("  --all               render the whole continent\n");
    printf("  --dump-tiles        dump the ADT grid (from the WDT area table), then exit\n");
    printf("  --no-wmo            skip WMO/building rendering (ground-floor look)\n");
    printf("  --light-elev <deg>  map-light elevation above horizon (default: engine noon ~37)\n");
    printf("  --light-mode <m>    light: client | neutral | desat | flat\n");
    printf("                      neutral = desaturated zone light; flat = neutral + fixed intensity\n");
    printf("                      map preset default = flat; realistic = client\n");
    printf("  --light-desat <f>   desaturation 0..1 (implies --light-mode desat; 1 = neutral)\n");
    printf("  --ignore-zone-light ignore zone light entirely (alias for --light-mode flat)\n");
    printf("  --neutral-light     desaturated zone light, keeping each zone's intensity\n");
    printf("  --water-mode <m>    water colour: client | map | desat\n");
    printf("                      map preset default = map (fixed ocean palette), realistic = client\n");
    printf("  --gain <f>          post-render brightness gain (default 1.0)\n");
    printf("  --flat-light        alias for --light-elev 89 (flat city-floor look)\n");
    printf("  --city-file <path>  ADT list (\"tx ty\" per line) rendered with the city sun\n");
    printf("  --city-elev <deg>   light elevation for city ADTs (default 60 with --city-file)\n");
    printf("  --xray <mode>       x-ray city floors: off | depthoff | zsort | zinv (default off)\n");
    printf("  --xray-zdir <dir>   zsort direction: lowlast (floors on top, default) | highlast\n");
    printf("  --xray-k <f>        zinv depth remap K (default 1500)\n");
    printf("  --xray-scope <s>    apply x-ray to: all WMOs | city ADTs only (default city)\n");
    printf("  --xray-skip-above <f>  trick-model guard: skip gated WMOs this high above the street\n");
    printf("                          AND floating above it (e.g. Stormwind's distance cathedral). 0 = off\n");
    printf("  --xray-dump         dump a per-WMO table (name, flags, Z, gate, guard) at first sight\n");
    printf("  --quads             2D WMO quads: draw selected per-group footprints over the 3D\n");
    printf("                      terrain (minimap look; avoids the trick distance models)\n");
    printf("  --quad-tint         with --quads: draw flat per-class tint rectangles instead of the\n");
    printf("                      real per-cell minimap .blp art (diagnostic fallback)\n");
    printf("  --no-quad-wmo       with --quads: do NOT render the 3D WMO underneath\n");
    printf("                      (default is ON: 3D WMO + districts + quad art on top)\n");
    printf("  --no-quad-fill      with 3D WMO: do NOT add the groups the client's viewpoint\n");
    printf("                      cull skips (city districts; default is to fill them in)\n");
    printf("  --quad-alpha <0-255>  quad overlay alpha (default 160; art mode uses 255)\n");
    printf("  --quad-sel <mode>   which groups apply: none | all | ext (default) | int\n");
    printf("  --quad-dump <file>  write the collected group table (i/wmo/grp/smo/ext/sel/bbox)\n");
    printf("  --skip-existing     skip tiles whose PNG already exists (resume / parallel workers)\n");
    printf("  --tile-list <file>  render only the \"tx ty\" lines in this file (parallel workers)\n");
    printf("  --quad-cache <file> cache the collected WMO group table (loads instantly when it\n");
    printf("                      exists, otherwise collects once and saves it)\n");
    printf("  --serve             interactive mode: local web tuner (settings page, live re-render)\n");
    printf("  --port <n>          HTTP port for --serve (default 8742)\n");
}

int main(int argc, char** argv) {
    // Keep diagnostics visible live even when stdout is piped (block-buffered by default).
    // NB: use _IONBF, not _IOLBF. In this UCRT, _IOLBF with size==0 underflows the
    // `size - 2` validation (0x40-only mode branch) and is rejected via the
    // invalid-parameter path, which fastfails. Unbuffered is the safe equivalent here.
    setvbuf(stdout, nullptr, _IONBF, 0);

    // Run headless: ConsoleDeviceInitialize reads this to force a small windowed D3D
    // device (MapBaker renders fully offscreen and never presents). Set before the
    // device is created. Must be the very first thing we do to any env the engine reads.
    _putenv_s("MAPBAKER", "1");

    MapBakerApp::Config config;
    memset(&config, 0, sizeof(config));
    config.layer = 0;
    config.px = 1024;
    config.ssaa = 2;
    config.lightElev = -1.0f;
    config.lightMode = -1;       // preset default: map = neutral, realistic = client
    config.lightDesat = 1.0f;
    config.waterMode = -1;       // preset default: map = map palette, realistic = client
    config.gain = 1.0f;
    config.cityElev = -1.0f;
    config.xrayMode = 0;
    config.xrayLowLast = 1;
    config.xrayK = 1500.0f;
    config.xrayScope = 1;
    config.xraySkipAbove = 0.0f;
    config.xrayDump = 0;
    config.quadAlpha = 160.0f / 255.0f;
    // Art mode draws only cells that actually have a Textures\Minimap .blp, so "all"
    // is the right default: it paints exactly the art the client shipped (the real city
    // buildings are INTERIOR groups, 0x8 clear — the exterior set is mostly trick shells
    // with no art). --quad-tint falls back to exterior flat tints.
    config.quadSel = 1;
    config.quadArt = true;  // real per-cell minimap art (--quad-tint forces flat tints)
    config.quadFill = true; // draw the WMO groups the viewpoint cull skips (districts)
    config.quadWmo = true;  // 3D WMO underneath the art (the comprehensive map look)
    config.quadDump = nullptr;
    config.skipExisting = false;
    config.tileListFile = nullptr;
    config.quadCache = nullptr;
    config.serve = false;
    config.port = 8742;

    const char* clientArg = nullptr;
    const char* outArg = nullptr;
    const char* cityFileArg = nullptr;
    bool haveTile = false;
    bool haveRange = false;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            Usage();
            return 0;
        }

        if (!strcmp(argv[i], "--client") && i + 1 < argc) {
            clientArg = argv[++i];
        } else if (!strcmp(argv[i], "--map") && i + 1 < argc) {
            config.mapArg = argv[++i];
        } else if (!strcmp(argv[i], "--layer") && i + 1 < argc) {
            const char* layer = argv[++i];
            if (!strcmp(layer, "combined")) config.layer = 0;
            else if (!strcmp(layer, "terrain")) config.layer = 1;
            else if (!strcmp(layer, "wmo")) config.layer = 2;
            else if (!strcmp(layer, "trees")) config.layer = 3;
            else if (!strcmp(layer, "quads")) config.layer = 4;
            else {
                fprintf(stderr, "MapBaker: --layer expects combined | terrain | wmo | trees | quads\n");
                return 1;
            }
        } else if (!strcmp(argv[i], "--preset") && i + 1 < argc) {
            config.preset = argv[++i];
        } else if (!strcmp(argv[i], "--px") && i + 1 < argc) {
            config.px = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--ssaa") && i + 1 < argc) {
            config.ssaa = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--out") && i + 1 < argc) {
            outArg = argv[++i];
        } else if (!strcmp(argv[i], "--tx") && i + 1 < argc) {
            config.tx0 = config.tx1 = atoi(argv[++i]);
            haveTile = true;
        } else if (!strcmp(argv[i], "--ty") && i + 1 < argc) {
            config.ty0 = config.ty1 = atoi(argv[++i]);
            haveTile = true;
        } else if (!strcmp(argv[i], "--tx0") && i + 1 < argc) {
            config.tx0 = atoi(argv[++i]);
            haveRange = true;
        } else if (!strcmp(argv[i], "--ty0") && i + 1 < argc) {
            config.ty0 = atoi(argv[++i]);
            haveRange = true;
        } else if (!strcmp(argv[i], "--tx1") && i + 1 < argc) {
            config.tx1 = atoi(argv[++i]);
            haveRange = true;
        } else if (!strcmp(argv[i], "--ty1") && i + 1 < argc) {
            config.ty1 = atoi(argv[++i]);
            haveRange = true;
        } else if (!strcmp(argv[i], "--all")) {
            config.allTiles = true;
        } else if (!strcmp(argv[i], "--dump-tiles")) {
            config.dumpTiles = true;
        } else if (!strcmp(argv[i], "--no-wmo")) {
            config.noWmo = true;
        } else if (!strcmp(argv[i], "--light-elev") && i + 1 < argc) {
            config.lightElev = static_cast<float>(strtod(argv[++i], nullptr));
        } else if (!strcmp(argv[i], "--light-mode") && i + 1 < argc) {
            const char* mode = argv[++i];
            if (!strcmp(mode, "client")) {
                config.lightMode = 0;
            } else if (!strcmp(mode, "neutral")) {
                config.lightMode = 1;
                config.lightDesat = 1.0f;
            } else if (!strcmp(mode, "flat")) {
                // Neutral colour plus fixed intensity; the sun elevation stays a separate
                // switch (--light-elev / --flat-light).
                config.lightMode = 3;
                config.lightDesat = 1.0f;
            } else if (!strcmp(mode, "desat")) {
                config.lightMode = 2;
            } else {
                fprintf(stderr, "MapBaker: --light-mode expects client | neutral | desat | flat\n");
                return 1;
            }
        } else if (!strcmp(argv[i], "--light-desat") && i + 1 < argc) {
            config.lightMode = 2;
            config.lightDesat = static_cast<float>(strtod(argv[++i], nullptr));
        } else if (!strcmp(argv[i], "--ignore-zone-light")) {
            config.lightMode = 3;
            config.lightDesat = 1.0f;
        } else if (!strcmp(argv[i], "--neutral-light")) {
            config.lightMode = 1;
            config.lightDesat = 1.0f;
        } else if (!strcmp(argv[i], "--water-mode") && i + 1 < argc) {
            const char* mode = argv[++i];
            if (!strcmp(mode, "client")) {
                config.waterMode = 0;
            } else if (!strcmp(mode, "map")) {
                config.waterMode = 1;
            } else if (!strcmp(mode, "desat")) {
                config.waterMode = 2;
            } else {
                fprintf(stderr, "MapBaker: --water-mode expects client | map | desat\n");
                return 1;
            }
        } else if (!strcmp(argv[i], "--gain") && i + 1 < argc) {
            config.gain = static_cast<float>(strtod(argv[++i], nullptr));
        } else if (!strcmp(argv[i], "--flat-light")) {
            config.lightElev = 89.0f; // near-vertical sun
        } else if (!strcmp(argv[i], "--city-file") && i + 1 < argc) {
            cityFileArg = argv[++i];
        } else if (!strcmp(argv[i], "--city-elev") && i + 1 < argc) {
            config.cityElev = static_cast<float>(strtod(argv[++i], nullptr));
        } else if (!strcmp(argv[i], "--xray") && i + 1 < argc) {
            const char* mode = argv[++i];
            if (!strcmp(mode, "off")) config.xrayMode = 0;
            else if (!strcmp(mode, "depthoff")) config.xrayMode = 1;
            else if (!strcmp(mode, "zsort")) config.xrayMode = 2;
            else if (!strcmp(mode, "zinv")) config.xrayMode = 3;
            else {
                fprintf(stderr, "MapBaker: --xray expects off | depthoff | zsort | zinv\n");
                return 1;
            }
        } else if (!strcmp(argv[i], "--xray-zdir") && i + 1 < argc) {
            const char* dir = argv[++i];
            if (!strcmp(dir, "lowlast")) config.xrayLowLast = 1;
            else if (!strcmp(dir, "highlast")) config.xrayLowLast = 0;
            else {
                fprintf(stderr, "MapBaker: --xray-zdir expects lowlast | highlast\n");
                return 1;
            }
        } else if (!strcmp(argv[i], "--xray-k") && i + 1 < argc) {
            config.xrayK = static_cast<float>(strtod(argv[++i], nullptr));
        } else if (!strcmp(argv[i], "--xray-scope") && i + 1 < argc) {
            const char* scope = argv[++i];
            if (!strcmp(scope, "all")) config.xrayScope = 0;
            else if (!strcmp(scope, "city")) config.xrayScope = 1;
            else {
                fprintf(stderr, "MapBaker: --xray-scope expects all | city\n");
                return 1;
            }
        } else if (!strcmp(argv[i], "--xray-skip-above") && i + 1 < argc) {
            config.xraySkipAbove = static_cast<float>(strtod(argv[++i], nullptr));
        } else if (!strcmp(argv[i], "--xray-dump")) {
            config.xrayDump = 1;
        } else if (!strcmp(argv[i], "--quads")) {
            config.quads = true;
        } else if (!strcmp(argv[i], "--quad-tint")) {
            config.quadArt = false;
            config.quadSel = 2;   // flat tints: exterior only (previous default)
        } else if (!strcmp(argv[i], "--quad-wmo")) {
            config.quadWmo = true;
        } else if (!strcmp(argv[i], "--no-quad-wmo")) {
            config.quadWmo = false;
        } else if (!strcmp(argv[i], "--no-quad-fill")) {
            config.quadFill = false;
        } else if (!strcmp(argv[i], "--quad-fill")) {
            config.quadFill = true;
        } else if (!strcmp(argv[i], "--quad-alpha") && i + 1 < argc) {
            config.quadAlpha = static_cast<float>(strtod(argv[++i], nullptr)) / 255.0f;
        } else if (!strcmp(argv[i], "--quad-dump") && i + 1 < argc) {
            config.quadDump = argv[++i];
        } else if (!strcmp(argv[i], "--quad-sel") && i + 1 < argc) {
            const char* sel = argv[++i];
            if (!strcmp(sel, "none")) config.quadSel = 0;
            else if (!strcmp(sel, "all")) config.quadSel = 1;
            else if (!strcmp(sel, "ext")) config.quadSel = 2;
            else if (!strcmp(sel, "int")) config.quadSel = 3;
            else {
                fprintf(stderr, "MapBaker: --quad-sel expects none | all | ext | int\n");
                return 1;
            }
        } else if (!strcmp(argv[i], "--skip-existing")) {
            config.skipExisting = true;
        } else if (!strcmp(argv[i], "--tile-list") && i + 1 < argc) {
            config.tileListFile = argv[++i];
        } else if (!strcmp(argv[i], "--quad-cache") && i + 1 < argc) {
            config.quadCache = argv[++i];
        } else if (!strcmp(argv[i], "--serve")) {
            config.serve = true;
        } else if (!strcmp(argv[i], "--port") && i + 1 < argc) {
            config.port = atoi(argv[++i]);
        } else {
            Usage();
            return 1;
        }
    }

    if (!config.mapArg) {
        fprintf(stderr, "MapBaker: --map is required\n");
        Usage();
        return 1;
    }

    if (!config.preset) {
        config.preset = "map";
    }

    if (config.px <= 0 || (config.px & (config.px - 1))) {
        fprintf(stderr, "MapBaker: --px must be a power of two\n");
        return 1;
    }

    if (config.ssaa <= 0) {
        config.ssaa = 1;
    }

    if (config.lightDesat < 0.0f) {
        config.lightDesat = 0.0f;
    }
    if (config.lightDesat > 1.0f) {
        config.lightDesat = 1.0f;
    }

    if (config.px * config.ssaa < 8) {
        fprintf(stderr, "MapBaker: --px * --ssaa must be >= 8\n");
        return 1;
    }

    if (haveRange) {
        if (config.tx1 < config.tx0) std::swap(config.tx1, config.tx0);
        if (config.ty1 < config.ty0) std::swap(config.ty1, config.ty0);
    } else if (!haveTile && !config.allTiles && !config.dumpTiles && !config.tileListFile) {
        fprintf(stderr, "MapBaker: specify a tile (--tx/--ty), a range (--tx0..--ty1), --tile-list, --all, or --dump-tiles\n");
        return 1;
    }

    StormRtlInitialize();
    StormInitialize();

    char basePath[260];
    OsGetExePath(basePath, sizeof(basePath));

    char clientPath[260];
    SStrCopy(clientPath, clientArg ? clientArg : basePath, sizeof(clientPath));
    uint32_t clientLength = SStrLen(clientPath);

    if (clientLength && clientPath[clientLength - 1] != '\\' && clientPath[clientLength - 1] != '/') {
        SStrPack(clientPath, "\\", sizeof(clientPath));
    }

    if (!OpenArchives(clientPath, basePath)) {
        return 1;
    }

    char outDir[260];
    SStrCopy(outDir, outArg ? outArg : basePath, sizeof(outDir));
    if (outDir[0] && outDir[strlen(outDir) - 1] != '\\' && outDir[strlen(outDir) - 1] != '/') {
        SStrPack(outDir, "\\", sizeof(outDir));
    }

    config.clientPath = clientPath;
    config.outDir = outDir;
    config.cityFile = cityFileArg;
    // A city list without an explicit elevation defaults to a flatter 60-degree sun.
    if (config.cityFile && config.cityElev < 0.0f) {
        config.cityElev = 60.0f;
    }
    if (config.serve && config.port <= 0 || config.serve && config.port > 65535) {
        fprintf(stderr, "MapBaker: --port must be in 1..65535\n");
        return 1;
    }
    if (config.serve && config.xrayScope == 1 && !config.cityFile) {
        fprintf(stderr, "MapBaker: --serve with --xray-scope city needs --city-file (gate will be empty)\n");
    }
    MapBakerApp::SetConfig(config);

    WriteDefaultConfig(basePath);
    ConsoleInitializeClientCommand();
    CVar::Initialize("MapBaker.wtf");
    CVar::Register("locale", "Set the game locale", 0, s_locale, nullptr, CATEGORY_DEFAULT, false, nullptr, false);

    BaseInitializeGlobal();
    EventInitialize(1, 0);
    OsTimeStartup(OsTimeManager::BestAvailable);
    AsyncFileReadInitialize(0, 100);

    g_startup_StringsDB.Load(__FILE__, __LINE__);
    ConsoleDeviceInitialize("MapBaker", 1, 1);

    EventCreateContextEx(1, MapBakerApp::InitializeEngine, MapBakerApp::DestroyEngine, 0, 0);
    EventDoMessageLoop();
    EventDestroy();

    AsyncFileReadDestroy();
    OsTimeShutdown();

    return 0;
}
