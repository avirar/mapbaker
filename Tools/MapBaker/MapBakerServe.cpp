#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>
#include <objbase.h>
#include <atomic>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#pragma comment(lib, "dbghelp.lib")

#include "Engine/Source/Event/Event.h"
#include "WoW/Source/WorldClient/Map.h"

#include "MapBakerServe.h"
#include "MapBakerArt.h"
#include "MapBakerPng.h"
#include "MapBakerQuad.h"
#include "MapBakerWorld.h"

namespace {

    // One render job: the settings sent by the page via /render (or the CLI defaults for
    // the initial job). Applied once on the event thread, then the whole region is
    // rendered tile by tile.
    struct Job {
        uint32_t mode;     // 0 off, 1 depthoff, 2 zsort, 3 zinv
        uint32_t zdir;     // zsort: 1 lowlast (floors on top), 0 highlast
        float k;           // zinv depth remap
        uint32_t scope;    // 0 all WMOs, 1 city ADTs
        float skip;        // trick/portal guard (0 = off)
        uint32_t dump;     // per-WMO table dump
        float gain;
        float light;       // global light elevation (deg; -1 = engine noon)
        float citylight;   // city ADT light elevation (deg; -1 = use global)
        int32_t px;        // tile resolution (512/1024/2048)
        int32_t ssaa;      // supersampling factor
        bool nowmo;        // skip WMO rendering entirely
        // 2D WMO quad overlay (see MapBakerQuad.h): flat per-group footprints over the
        // 3D terrain. The group table is filled by a collection pass (one extra render of
        // the region) the first time quads are turned on, or on qcollect.
        uint32_t qon;        // composite the selected quads
        uint32_t qart;       // draw the real per-cell minimap .blp art (1) vs flat tints (0)
        uint32_t qfill = 1;  // with the 3D WMO: also draw the viewpoint-culled groups
        uint32_t qwmo;       // also render the 3D WMO underneath (0 = terrain-only)
        uint32_t qalpha;     // quad alpha 0..255
        uint32_t qext[3];    // exterior tint (RGB)
        uint32_t qint[3];    // interior tint (RGB)
        uint32_t qmode = 0;        // bulk selection: 0 keep, 1 all, 2 exterior, 3 interior
        std::string qsel;          // explicit selection (comma list of group indices)
        bool qselPresent = false;  // qsel param was in the request ("" = select none)
        uint32_t qcollect = 0;     // force a re-collection of the group table
    };

    // Shared state (server thread <-> event thread), guarded by s_cs.
    struct State {
        bool pending = false;  // a job is waiting for the event thread
        Job pendingJob;
        Job applied;           // settings of the last completed job
        bool rendering = false;
        const char* phase = "idle";  // idle | render | composite
        int32_t done = 0, total = 0;
        uint64_t gen = 0;      // bumped per completed job (page cache-buster)
        uint64_t lastMs = 0;
    };

    CRITICAL_SECTION s_cs;
    State s_state;

    // Region (fixed for the process lifetime).
    std::vector<std::pair<int32_t, int32_t>> s_tiles;  // (tx, ty), ty rows top->bottom
    int32_t s_txMin = 0, s_txMax = 0, s_tyMin = 0, s_tyMax = 0;
    int32_t s_w = 0, s_h = 0;  // region size in tiles
    int32_t s_gateTx0 = 0, s_gateTx1 = -1, s_gateTy0 = 0, s_gateTy1 = -1;
    bool s_gateValid = false;  // city gate bbox available (drives the 2x crop)

    uint32_t s_rtSize = 0;  // current render-target size (px * ssaa)
    std::string s_outDir;   // output directory (trailing backslash)
    std::string s_baseCmd;  // CLI line base for the page's "copy command" box
    std::vector<uint8_t> s_montage;  // current montage RGB (event thread only)
    std::vector<uint8_t> s_tileBuf;  // per-tile readback (event thread only)

    // Event-thread-only run state (no lock: only ServeIdleCallback touches it).
    bool s_active = false;
    int32_t s_idx = 0;
    uint64_t s_t0 = 0;
    Job s_runJob;

    SOCKET s_listen = INVALID_SOCKET;
    std::thread s_thread;
    std::atomic<bool> s_run{false};

    // ---------------------------------------------------------------------------
    // Small helpers
    // ---------------------------------------------------------------------------

    std::string ReadWholeFile(const std::string& path) {
        FILE* f = fopen(path.c_str(), "rb");
        if (!f) {
            return "";
        }
        std::string out;
        char buf[4096];
        size_t n = 0;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
            out.append(buf, n);
        }
        fclose(f);
        return out;
    }

    bool WriteWholeFile(const std::string& path, const std::string& data) {
        FILE* f = fopen(path.c_str(), "wb");
        if (!f) {
            return false;
        }
        const bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
        fclose(f);
        return ok;
    }

    std::string UrlDecode(const std::string& in) {
        std::string out;
        out.reserve(in.size());
        for (size_t i = 0; i < in.size(); i++) {
            const char c = in[i];
            if (c == '%' && i + 2 < in.size() && isxdigit((unsigned char)in[i + 1]) &&
                isxdigit((unsigned char)in[i + 2])) {
                unsigned v = 0;
                sscanf(&in[i + 1], "%02x", &v);
                out.push_back(static_cast<char>(v));
                i += 2;
            } else if (c == '+') {
                out.push_back(' ');
            } else {
                out.push_back(c);
            }
        }
        return out;
    }

    bool GetParam(const std::string& query, const char* key, std::string* out) {
        // Both prefixes ("?key=" and "&key=") are the same length; pos ends up pointing at
        // the first character of the value. (NB: an earlier version under-counted by one for
        // the leading "?" case, so the FIRST parameter of a query always parsed as 0.)
        const std::string k = std::string("?") + key + "=";
        size_t pos = query.find(k);
        if (pos == std::string::npos) {
            pos = query.find(std::string("&") + key + "=");
            if (pos == std::string::npos) {
                return false;
            }
        }
        pos += k.size(); // point at the value
        size_t end = query.find('&', pos);
        *out = UrlDecode(query.substr(pos, end == std::string::npos ? std::string::npos : end - pos));
        return true;
    }

    int32_t GetParamI(const std::string& query, const char* key, int32_t def) {
        std::string v;
        if (!GetParam(query, key, &v) || v.empty()) {
            return def;
        }
        return (int32_t)atoi(v.c_str());
    }

    double GetParamF(const std::string& query, const char* key, double def) {
        std::string v;
        if (!GetParam(query, key, &v) || v.empty()) {
            return def;
        }
        return atof(v.c_str());
    }

    // ---------------------------------------------------------------------------
    // Thread stack dumper (MAPBAKER_TRACE=1). Walks every thread in the process
    // and prints its call stack with module+offset resolution. Used to catch a
    // concurrent writer that clobbers s_montage during a fill.
    // ---------------------------------------------------------------------------

    static bool s_symReady = false;

    // Walk every thread in the process and print its call stack to stderr as
    // `  #N module+0xOFFSET symbol`. Thread names (thread descriptions) are printed
    // on the thread header line when available. Used by the MAPBAKER_TRACE monitor
    // to catch a concurrent writer clobbering s_montage.
    void DumpAllThreadStacks(const char* tag) {
        fprintf(stderr, "MapBaker trace: [%s] dumping all thread stacks (pid %lu)\n",
                tag, (unsigned long)GetCurrentProcessId());
        fflush(stderr);

        if (!s_symReady) {
            if (SymInitialize(GetCurrentProcess(), nullptr, TRUE)) {
                SymSetOptions(SymGetOptions() | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
                s_symReady = true;
            }
        }

        const HANDLE hProc = GetCurrentProcess();

        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snap == INVALID_HANDLE_VALUE) {
            fprintf(stderr, "MapBaker trace: CreateToolhelp32Snapshot failed: %lu\n",
                    GetLastError());
            return;
        }

        THREADENTRY32 te{};
        te.dwSize = sizeof(te);
        if (!Thread32First(snap, &te)) {
            fprintf(stderr, "MapBaker trace: Thread32First failed: %lu\n", GetLastError());
            CloseHandle(snap);
            return;
        }

        do {
            const DWORD tid = te.th32ThreadID;
            HANDLE hThr = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                                     THREAD_QUERY_INFORMATION,
                                     FALSE, tid);
            if (!hThr) {
                fprintf(stderr, "MapBaker trace: thread %lu OpenThread failed (err %lu)\n",
                        tid, GetLastError());
                continue;
            }

            // Thread description (name), if the engine set one. This SDK returns a
            // heap string the caller must release with CoTaskMemFree.
            std::string desc;
            PWSTR wdesc = nullptr;
            if (GetThreadDescription(hThr, &wdesc) == S_OK && wdesc && wdesc[0]) {
                char dA[128] = {};
                WideCharToMultiByte(CP_UTF8, 0, wdesc, -1, dA, (int)sizeof(dA) - 1,
                                    nullptr, nullptr);
                desc = dA;
                CoTaskMemFree(wdesc);
            }

            // Suspend the thread so its context is coherent, then read it.
            SuspendThread(hThr);
            CONTEXT ctx{};
            ctx.ContextFlags = CONTEXT_ALL;
            if (!GetThreadContext(hThr, &ctx)) {
                fprintf(stderr, "MapBaker trace: thread %lu GetThreadContext failed (err %lu)\n",
                        tid, GetLastError());
                ResumeThread(hThr);
                CloseHandle(hThr);
                continue;
            }

            fprintf(stderr, "MapBaker trace: --- thread %lu%s%s%s ---\n", tid,
                    desc.empty() ? "" : " name='", desc.c_str(),
                    desc.empty() ? "" : "'");

            STACKFRAME64 sf{};

            IMAGEHLP_MODULE64 mod{};
            mod.SizeOfStruct = sizeof(mod);
            mod.BaseOfImage = 0;

            // IMAGEHLP_SYMBOL64 has a 1-byte Name[1]; the canonical pattern is a
            // flexible buffer with MaxNameLength pointing into it.
            char symBuf[sizeof(IMAGEHLP_SYMBOL64) + MAX_SYM_NAME + 1] = {};
            IMAGEHLP_SYMBOL64* sym = (IMAGEHLP_SYMBOL64*)symBuf;
            sym->SizeOfStruct = sizeof(IMAGEHLP_SYMBOL64);
            sym->MaxNameLength = MAX_SYM_NAME;
            char symName[256] = {};

            const int maxFrames = 48;
            for (int f = 0; f < maxFrames; ++f) {
                sf.AddrPC.Offset = 0;
                sf.AddrReturn.Offset = 0;
                if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, hProc, hThr, &sf, &ctx,
                                 nullptr, nullptr, nullptr, nullptr)) {
                    break;
                }
                const DWORD64 addr = sf.AddrReturn.Offset;
                if (addr == 0) {
                    break;
                }

                char modName[128] = {"??+"};
                if (SymGetModuleInfo64(hProc, addr, &mod)) {
                    _snprintf_s(modName, sizeof(modName), _TRUNCATE, "%s", mod.ImageName);
                }
                if (SymGetSymFromAddr64(hProc, addr, nullptr, sym) && sym->Name[0]) {
                    _snprintf_s(symName, sizeof(symName), _TRUNCATE, "%s", sym->Name);
                }
                fprintf(stderr, "  #%-2d %s+0x%08X %s\n", f, modName,
                        (unsigned)(addr - mod.BaseOfImage), symName);
            }

            fprintf(stderr, "MapBaker trace: --- end thread %lu ---\n", tid);
            fflush(stderr);

            ResumeThread(hThr);
            CloseHandle(hThr);
        } while (Thread32Next(snap, &te));

        CloseHandle(snap);
    }

    std::string JsonEscape(const std::string& s) {
        std::string o;
        o.reserve(s.size() + 8);
        for (char c : s) {
            if (c == '"' || c == '\\') {
                o.push_back('\\');
            }
            o.push_back(c);
        }
        return o;
    }

    bool SendAll(SOCKET sock, const void* data, size_t len) {
        size_t off = 0;
        while (off < len) {
            const int n = send(sock, static_cast<const char*>(data) + off, (int)(len - off), 0);
            if (n <= 0) {
                return false;
            }
            off += (size_t)n;
        }
        return true;
    }

    void SendResponse(SOCKET sock, const char* status, const char* type, const void* data, size_t len) {
        char hdr[512];
        const int hlen = sprintf(hdr, "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %u\r\n"
                                     "Cache-Control: no-store\r\nConnection: close\r\n\r\n",
                                 status, type, (unsigned)len);
        SendAll(sock, hdr, (size_t)hlen);
        if (len) {
            SendAll(sock, data, len);
        }
    }

    void SendText(SOCKET sock, const char* status, const char* body) {
        SendResponse(sock, status, "text/plain; charset=utf-8", body, strlen(body));
    }

    void ServeFile(SOCKET sock, const std::string& path, const char* type) {
        FILE* f = fopen(path.c_str(), "rb");
        if (!f) {
            SendText(sock, "404 Not Found", "not found\n");
            return;
        }
        fseek(f, 0, SEEK_END);
        const long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        std::string buf;
        if (sz > 0) {
            buf.resize((size_t)sz);
            if (fread(buf.data(), 1, (size_t)sz, f) != (size_t)sz) {
                fclose(f);
                SendText(sock, "500 Internal Server Error", "read error\n");
                return;
            }
        }
        fclose(f);
        SendResponse(sock, sz > 0 ? "200 OK" : "404 Not Found", type, buf.data(), buf.size());
    }

    // ---------------------------------------------------------------------------
    // The web page (embedded; served at /)
    // ---------------------------------------------------------------------------

    const char* kHtml = R"HTML(<!doctype html>
<html>
<head>
<meta charset="utf-8">
<title>MapBaker x-ray tuner</title>
<style>
:root { color-scheme: dark; }
* { box-sizing: border-box; }
body { margin: 0; font: 13px/1.45 "Segoe UI", sans-serif; background: #14171c; color: #d8dee9; }
header { display: flex; align-items: center; gap: 12px; padding: 10px 14px; background: #1c2129;
         border-bottom: 1px solid #2a3140; position: sticky; top: 0; z-index: 5; }
header h1 { font-size: 14px; margin: 0; font-weight: 600; white-space: nowrap; }
#status { margin-left: auto; font-size: 12px; color: #9aa4b2; text-align: right; }
main { display: flex; gap: 14px; padding: 14px; align-items: flex-start; }
aside { width: 320px; flex: 0 0 320px; background: #1a1f27; border: 1px solid #2a3140;
        border-radius: 8px; padding: 12px; position: sticky; top: 58px; max-height: calc(100vh - 72px);
        overflow-y: auto; }
section { background: #1a1f27; border: 1px solid #2a3140; border-radius: 8px; padding: 12px; }
fieldset { border: 1px solid #2a3140; border-radius: 6px; margin: 0 0 10px; padding: 8px 10px 10px; }
legend { font-size: 11px; text-transform: uppercase; letter-spacing: .06em; color: #7d8794; padding: 0 4px; }
.row { display: flex; align-items: center; gap: 8px; margin: 5px 0; }
.row label { flex: 0 0 88px; color: #aeb6c2; font-size: 12px; }
.row input, .row select { flex: 1; min-width: 0; background: #11141a; color: #d8dee9;
        border: 1px solid #343d4d; border-radius: 4px; padding: 4px 6px; font: inherit; }
.row input[type=checkbox] { flex: 0 0 auto; width: 14px; height: 14px; }
.hint { font-size: 11px; color: #6d7686; margin: 2px 0 6px 96px; }
#renderBtn { width: 100%; margin-top: 6px; padding: 9px; font: 600 14px/1 inherit; color: #fff;
             background: #2f81f7; border: 0; border-radius: 6px; cursor: pointer; }
#renderBtn:disabled { background: #3a4150; color: #8b93a2; cursor: default; }
#progress { height: 6px; background: #11141a; border-radius: 3px; overflow: hidden; margin-top: 10px; }
#progress > div { height: 100%; width: 0%; background: #3fb950; transition: width .3s; }
h2 { font-size: 11px; text-transform: uppercase; letter-spacing: .06em; color: #7d8794; margin: 14px 0 6px; }
.preset { display: flex; align-items: center; gap: 6px; margin: 3px 0; }
.preset .name { flex: 1; cursor: pointer; color: #79b8ff; overflow: hidden; text-overflow: ellipsis; }
.preset .del { color: #6d7686; cursor: pointer; padding: 0 4px; }
#psave { background: #11141a; color: #d8dee9; border: 1px solid #343d4d; border-radius: 4px;
         padding: 4px 10px; cursor: pointer; font: inherit; flex: 0 0 auto; }
#cli { width: 100%; font: 11px/1.5 Consolas, monospace; background: #11141a; color: #9aa4b2;
       border: 1px solid #343d4d; border-radius: 4px; padding: 6px; resize: vertical; min-height: 56px; }
.clirow { display: flex; gap: 6px; margin-bottom: 6px; }
.clirow button { background: #11141a; color: #d8dee9; border: 1px solid #343d4d; border-radius: 4px;
                 padding: 3px 10px; cursor: pointer; font: inherit; }
#right { flex: 1; display: flex; flex-direction: column; gap: 14px; min-width: 0; }
.imgs { display: flex; gap: 14px; align-items: flex-start; }
.imgwrap { min-width: 0; }
.imgwrap img { display: block; max-width: 100%; border-radius: 4px; border: 1px solid #2a3140;
               cursor: zoom-in; }
#refwrap { flex: 0 0 auto; display: none; width: 560px; max-width: 50%; }
#refimg { display: none; }
a { color: #79b8ff; text-decoration: none; }
.cap { font-size: 11px; color: #6d7686; margin: 4px 0 0; }
.qbtns { display: flex; gap: 4px; margin: 4px 0; flex-wrap: wrap; }
.qbtns button { background: #11141a; color: #d8dee9; border: 1px solid #343d4d; border-radius: 4px;
                padding: 2px 8px; cursor: pointer; font: inherit; font-size: 11px; }
.qlist { max-height: 230px; overflow-y: auto; border: 1px solid #2a3140; border-radius: 4px;
         padding: 4px; font-size: 11px; margin-top: 4px; }
.qlist label { display: flex; gap: 6px; align-items: center; padding: 1px 2px; cursor: pointer; }
.qlist .tag { font-size: 10px; padding: 0 4px; border-radius: 3px; flex: 0 0 auto; }
.qlist .tag.e { background: #3a3320; color: #e8d9a0; }
.qlist .tag.i { background: #20303a; color: #a0c8e8; }
.qlist .meta { color: #6d7686; margin-left: auto; }
</style>
</head>
<body>
<header>
  <h1>MapBaker x-ray tuner</h1>
  <span id="status">connecting&hellip;</span>
</header>
<main>
  <aside>
    <fieldset>
      <legend>x-ray</legend>
      <div class="row"><label>mode</label>
        <select id="mode">
          <option value="0">off</option>
          <option value="1">depthoff</option>
          <option value="2">zsort</option>
          <option value="3">zinv</option>
        </select></div>
      <div class="row"><label>z-sort dir</label>
        <select id="zdir">
          <option value="1">lowlast (floors on top)</option>
          <option value="0">highlast</option>
        </select></div>
      <div class="row"><label>K (zinv)</label><input id="k" type="number" min="300" max="4000" step="50" value="1500"></div>
      <div class="row"><label>skip-above</label><input id="skip" type="number" min="0" max="2000" step="25" value="0"></div>
      <div class="hint">trick guard: skip floating WMOs (cathedral distance model) taller than this above street; 0 = off</div>
      <div class="row"><label>scope</label>
        <select id="scope">
          <option value="1">city ADTs (gate)</option>
          <option value="0">all WMOs</option>
        </select></div>
    </fieldset>
    <fieldset>
      <legend>lighting</legend>
      <div class="row"><label>light-elev</label><input id="light" type="number" min="-1" max="90" step="1" value="60"></div>
      <div class="row"><label>city-elev</label><input id="citylight" type="number" min="-1" max="90" step="1" value="60"></div>
      <div class="hint">-1 = engine default noon (~37&deg;); higher = flatter city sun</div>
      <div class="row"><label>gain</label><input id="gain" type="number" min="0.2" max="3" step="0.05" value="1"></div>
    </fieldset>
    <fieldset>
      <legend>render</legend>
      <div class="row"><label>px</label>
        <select id="px"><option>512</option><option selected>1024</option><option>2048</option></select></div>
      <div class="row"><label>ssaa</label>
        <select id="ssaa"><option>1</option><option selected>2</option></select></div>
      <div class="row"><label>no WMO</label><input id="nowmo" type="checkbox"></div>
      <div class="row"><label>WMO dump</label><input id="dump" type="checkbox"></div>
      <div class="hint" id="dumphint">per-WMO table at <a id="dumplink" href="/dump.txt" target="_blank">/dump.txt</a></div>
    </fieldset>
    <fieldset>
      <legend>WMO quads (2D)</legend>
      <div class="row"><label>2D quads</label><input id="qon" type="checkbox"></div>
      <div class="row"><label>minimap art</label><input id="qart" type="checkbox" checked title="draw the real per-cell Textures\Minimap .blp art (uncheck = flat tints)"></div>
      <div class="row"><label>3D WMO under</label><input id="qwmo" type="checkbox" checked title="render the real 3D WMO beneath the quad art"></div>
      <div class="row"><label>fill districts</label><input id="qfill" type="checkbox" checked title="with 3D WMO on: also draw the groups the client's viewpoint cull skips (city districts, streets, plazas)"></div>
      <div class="row"><label>recollect</label><input id="qcollect" type="checkbox" title="force a fresh group-collection pass (slower render)"></div>
      <div class="row"><label>alpha</label><input id="qalpha" type="range" min="0" max="255" value="160"><span id="qalv" style="font-size:11px;color:#6d7686;">160</span></div>
      <div class="row"><label>ext tint</label><input id="qext" type="color" value="#e8e0c8"></div>
      <div class="row"><label>int tint</label><input id="qint" type="color" value="#c8d6ec"></div>
      <div class="qbtns">
        <button id="qall" type="button">all</button>
        <button id="qnone" type="button">none</button>
        <button id="qextb" type="button">ext only</button>
        <button id="qintb" type="button">int only</button>
        <button id="qrefresh" type="button">reload</button>
      </div>
      <div class="hint">real per-cell minimap <code>.blp</code> art over the 3D terrain (uncheck "minimap art" for flat tints). <b>E</b> = exterior (SMO 0x8), <b>I</b> = interior. First render with 2D quads on collects the groups.</div>
      <div id="qlist" class="qlist"></div>
    </fieldset>
    <button id="renderBtn">Render</button>
    <div id="progress"><div></div></div>
    <h2>presets</h2>
    <div class="row"><input id="pname" placeholder="preset name"><button id="psave">save current</button></div>
    <div id="presetlist"></div>
    <h2>CLI command</h2>
    <div class="clirow"><button id="cliup">update</button><button id="clipy">copy</button></div>
    <textarea id="cli" readonly></textarea>
  </aside>
  <section id="right">
    <div class="imgs">
      <div class="imgwrap" style="flex: 1;">
        <img id="montage" src="data:," alt="montage">
        <p class="cap">full region montage &mdash; click to open full resolution</p>
      </div>
      <div class="imgwrap" id="refwrap">
        <div class="row"><label>reference</label><input type="file" id="ref" accept="image/*"></div>
        <div class="row"><label>width</label><input type="range" id="refw" min="200" max="1200" value="520"><span id="refwl" style="font-size:11px;color:#6d7686;">520px</span></div>
        <img id="refimg">
        <p class="cap">original minimap reference (stays local, never uploaded)</p>
      </div>
    </div>
    <div class="imgwrap" id="cropwrap">
      <img id="crop" src="data:," alt="city crop">
      <p class="cap">gated city region, 2x zoom &mdash; click to open full resolution</p>
    </div>
  </section>
</main>
<script>
const $ = id => document.getElementById(id);
const modeNames = ['off', 'depthoff', 'zsort', 'zinv'];
let gen = 0, formSynced = false, baseCmd = '';

let groups = [], groupsLoaded = false;  // /groups.json: {i,wmo,smo,ext,xy,z,sel}
function selIndices() {
  const out = [];
  $('qlist').querySelectorAll('input[type=checkbox]').forEach(c => {
    if (c.checked) out.push(+c.dataset.i);
  });
  return out;
}
function qs() {
  let p = 'mode=' + $('mode').value + '&zdir=' + $('zdir').value + '&k=' + $('k').value +
    '&skip=' + $('skip').value + '&scope=' + $('scope').value + '&light=' + $('light').value +
    '&citylight=' + $('citylight').value + '&gain=' + $('gain').value + '&px=' + $('px').value +
    '&ssaa=' + $('ssaa').value + '&nowmo=' + ($('nowmo').checked ? 1 : 0) +
    '&dump=' + ($('dump').checked ? 1 : 0) +
    '&qon=' + ($('qon').checked ? 1 : 0) + '&qart=' + ($('qart').checked ? 1 : 0) +
    '&qfill=' + ($('qfill').checked ? 1 : 0) + '&qwmo=' + ($('qwmo').checked ? 1 : 0) +
    '&qcollect=' + ($('qcollect').checked ? 1 : 0) + '&qalpha=' + $('qalpha').value +
    '&qext=' + $('qext').value.slice(1) + '&qint=' + $('qint').value.slice(1);
  // Explicit per-group selection: only sent once the list is loaded (empty = all off).
  if (groupsLoaded) p += '&qsel=' + selIndices().join(',');
  return p;
}
function formFrom(s) {
  $('mode').value = s.mode; $('zdir').value = s.zdir; $('k').value = s.k; $('skip').value = s.skip;
  $('scope').value = s.scope; $('light').value = s.light; $('citylight').value = s.citylight;
  $('gain').value = s.gain; $('px').value = s.px; $('ssaa').value = s.ssaa;
  $('nowmo').checked = !!s.nowmo; $('dump').checked = !!s.dump;
  $('qon').checked = !!s.qon; $('qart').checked = !!s.qart; $('qfill').checked = !!s.qfill;
  $('qwmo').checked = !!s.qwmo;
  $('qalpha').value = s.qalpha; $('qalv').textContent = s.qalpha;
  $('qext').value = '#' + s.qext; $('qint').value = '#' + s.qint;
  cliUpdate();
}
function cliUpdate() {
  const p = new URLSearchParams(qs());
  let c = baseCmd + ' --px ' + p.get('px') + ' --ssaa ' + p.get('ssaa') +
    ' --light-elev ' + p.get('light') + ' --city-elev ' + p.get('citylight') +
    ' --gain ' + p.get('gain') + ' --xray ' + modeNames[+p.get('mode')] +
    ' --xray-zdir ' + (+p.get('zdir') ? 'lowlast' : 'highlast') +
    ' --xray-k ' + p.get('k') + ' --xray-scope ' + (+p.get('scope') ? 'city' : 'all');
  if (p.get('skip') !== '0') c += ' --xray-skip-above ' + p.get('skip');
  if (p.get('nowmo') === '1') c += ' --no-wmo';
  if (p.get('dump') === '1') c += ' --xray-dump';
  if (p.get('qon') === '1') {
    c += ' --quads --quad-alpha ' + p.get('qalpha');
    if (p.get('qart') !== '1') c += ' --quad-tint';
    if (p.get('qfill') !== '1') c += ' --no-quad-fill';
    if (p.get('qwmo') === '1') c += ' --quad-wmo';
    // NB: per-group selection is serve-only (the CLI has no group list).
  }
  $('cli').value = c;
}
function statusText(j) {
  if (j.rendering) {
    if (j.phase === 'collect') return 'collecting WMO groups\u2026';
    const pct = Math.round(100 * j.done / Math.max(1, j.total));
    return j.phase === 'composite' ? 'compositing montage/crop\u2026'
                                   : 'rendering tile ' + j.done + '/' + j.total + ' (' + pct + '%)\u2026';
  }
  const s = j.settings;
  return 'idle \u00b7 ' + modeNames[s.mode] + ' K=' + s.k + ' skip=' + s.skip + ' gain=' + s.gain +
    ' light=' + s.light + ' px=' + s.px + ' ssaa=' + s.ssaa +
    (s.qon ? ' \u00b7 quads(' + s.qcount + ' groups)' : '') +
    ' \u00b7 ' + j.total + ' tiles in ' + (j.lastMs / 1000).toFixed(1) + 's';
}
async function tick() {
  try {
    const j = await (await fetch('/status', { cache: 'no-store' })).json();
    if (j.baseCmd) baseCmd = j.baseCmd;
    $('status').textContent = statusText(j);
    document.querySelector('#progress > div').style.width =
      (j.rendering ? 100 * j.done / Math.max(1, j.total) : 0) + '%';
    $('renderBtn').disabled = j.rendering;
    if (!formSynced && j.settings) { formSynced = true; formFrom(j.settings); }
    if (j.gen !== gen) {
      gen = j.gen;
      if (gen > 0) {
        $('montage').src = '/montage.png?g=' + gen;
        $('crop').src = '/crop2x.png?g=' + gen;
        loadGroups();
      }
      $('cropwrap').style.display = j.hasCrop ? '' : 'none';
      $('dumphint').style.display = (j.settings && j.settings.dump) ? '' : 'none';
    }
  } catch (e) {
    $('status').textContent = 'server unreachable';
  }
  setTimeout(tick, 700);
}
$('renderBtn').onclick = async () => {
  $('renderBtn').disabled = true;
  try {
    const j = await (await fetch('/render?' + qs(), { cache: 'no-store' })).json();
    if (!j.ok) alert(j.error || 'render failed');
  } catch (e) { alert('render request failed: ' + e); }
};
async function loadPresets() {
  try {
    const t = await (await fetch('/presets', { cache: 'no-store' })).text();
    const list = $('presetlist');
    list.innerHTML = '';
    t.trim().split('\n').forEach(line => {
      const f = line.split('\t');
      if (f.length < 13) return;
      const d = document.createElement('div'); d.className = 'preset';
      const n = document.createElement('span'); n.className = 'name'; n.textContent = f[0];
      n.title = 'load into the form';
      n.onclick = () => {
        $('mode').value = f[1]; $('zdir').value = f[2]; $('k').value = f[3]; $('skip').value = f[4];
        $('scope').value = f[5]; $('light').value = f[6]; $('citylight').value = f[7];
        $('gain').value = f[8]; $('px').value = f[9]; $('ssaa').value = f[10];
        $('nowmo').checked = f[11] === '1'; $('dump').checked = f[12] === '1';
        cliUpdate();
      };
      const del = document.createElement('span'); del.className = 'del'; del.textContent = '\u00d7';
      del.title = 'delete preset';
      del.onclick = async () => { await fetch('/presets?op=del&name=' + encodeURIComponent(f[0])); loadPresets(); };
      d.appendChild(n); d.appendChild(del); list.appendChild(d);
    });
  } catch (e) {}
}
$('psave').onclick = async () => {
  const name = $('pname').value.trim();
  if (!name) return;
  await fetch('/presets?op=save&name=' + encodeURIComponent(name) + '&' + qs(), { cache: 'no-store' });
  $('pname').value = '';
  loadPresets();
};
)HTML"
// Split raw literal: a single MSVC string literal is capped at 16384 chars and the page
// grew past it; adjacent string literals concatenate.
R"HTML(
async function loadGroups() {
  try {
    const j = await (await fetch('/groups.json', { cache: 'no-store' })).json();
    groups = j || [];
    groupsLoaded = groups.length > 0;
    renderGroups();
  } catch (e) {}
}
function renderGroups() {
  const list = $('qlist');
  list.innerHTML = '';
  if (!groupsLoaded) {
    const d = document.createElement('div');
    d.className = 'meta';
    d.innerHTML = 'no groups yet &mdash; render once with 2D quads on to collect them';
    list.appendChild(d);
    return;
  }
  const sel = new Set(selIndices());
  const order = groups.slice().sort((a, b) =>
    a.wmo === b.wmo ? a.i - b.i : (a.wmo < b.wmo ? -1 : 1));
  order.forEach(g => {
    const l = document.createElement('label');
    const c = document.createElement('input');
    c.type = 'checkbox'; c.dataset.i = g.i; c.checked = sel.has(g.i);
    const tag = document.createElement('span');
    tag.className = 'tag ' + (g.ext ? 'e' : 'i');
    tag.textContent = g.ext ? 'E' : 'I';
    const t = document.createElement('span');
    const w = (g.wmo.split('\\').pop() || '').replace('.WMO', '');
    t.textContent = g.smo + (w && w.toLowerCase() !== (g.smo || '').toLowerCase() ? ' \u00b7 ' + w : '');
    const m = document.createElement('span');
    m.className = 'meta';
    m.textContent = Math.round(g.xy[1] - g.xy[0]) + '\u00d7' + Math.round(g.xy[3] - g.xy[2]);
    l.appendChild(c); l.appendChild(tag); l.appendChild(t); l.appendChild(m);
    l.title = 'group ' + g.i + ' \u00b7 ' + g.wmo + ' \u00b7 z ' +
      Math.round(g.z[0]) + '..' + Math.round(g.z[1]);
    list.appendChild(l);
  });
}
function setGroups(mode) {
  $('qlist').querySelectorAll('input[type=checkbox]').forEach(c => {
    const g = groups.find(x => x.i === +c.dataset.i);
    if (mode === 'all') c.checked = true;
    else if (mode === 'none') c.checked = false;
    else c.checked = (mode === 'ext') === !!g.ext;
  });
}
$('qall').onclick = () => setGroups('all');
$('qnone').onclick = () => setGroups('none');
$('qextb').onclick = () => setGroups('ext');
$('qintb').onclick = () => setGroups('int');
$('qrefresh').onclick = loadGroups;
$('qalpha').oninput = e => { $('qalv').textContent = e.target.value; };
$('cliup').onclick = cliUpdate;
$('clipy').onclick = () => { $('cli').select(); try { document.execCommand('copy'); } catch (e) {} };
$('ref').onchange = e => {
  const f = e.target.files[0];
  if (!f) return;
  const rd = new FileReader();
  rd.onload = () => { $('refimg').src = rd.result; $('refimg').style.display = ''; };
  rd.readAsDataURL(f);
};
$('refw').oninput = e => { $('refimg').style.width = e.target.value + 'px'; $('refwl').textContent = e.target.value + 'px'; };
$('montage').onclick = () => window.open($('montage').src, '_blank');
$('crop').onclick = () => window.open($('crop').src, '_blank');
tick(); loadPresets(); cliUpdate();
</script>
</body>
</html>
)HTML";

    // ---------------------------------------------------------------------------
    // Settings -> engine (event thread only)
    // ---------------------------------------------------------------------------

    FILE* s_dumpFile = nullptr;  // wmo_dump.txt handle, owned by the event thread

    // Apply the job's per-group quad selection to the (already populated) table.
    // Precedence: an explicit qsel list > a qmode bulk class > keep current.
    void ApplyQuadSelection(const Job& j) {
        if (j.qselPresent) {
            MapBakerWorld::SetQuadSelectionByClass(0); // all off
            std::size_t start = 0;
            while (start < j.qsel.size()) {
                const std::size_t comma = j.qsel.find(',', start);
                const std::string token = j.qsel.substr(start, comma == std::string::npos
                        ? std::string::npos : comma - start);
                if (!token.empty()) {
                    MapBakerWorld::SetQuadSelected((uint32_t)atoi(token.c_str()), true);
                }
                if (comma == std::string::npos) {
                    break;
                }
                start = comma + 1;
            }
            return;
        }
        if (j.qmode != 0) {
            MapBakerWorld::SetQuadSelectionByClass(j.qmode);
        }
    }

    void ApplyJob(const Job& j) {
        MapBakerWorld::SetXray(j.mode, j.zdir, j.k, j.scope);
        MapBakerWorld::SetXrayGuard(j.skip, j.dump);
        if (s_dumpFile) {
            fclose(s_dumpFile);
            s_dumpFile = nullptr;
        }
        if (j.dump) {
            char p[600];
            snprintf(p, sizeof(p), "%swmo_dump.txt", s_outDir.c_str());
            s_dumpFile = fopen(p, "w");
        }
        MapBakerWorld::SetXrayDumpFile(s_dumpFile);
        MapBakerWorld::SetGain(j.gain);
        MapBakerWorld::SetLightElevation(j.light);
        MapBakerWorld::SetCityLightElevation(j.citylight);
        MapBakerWorld::SetNoWmo(j.nowmo);

        const uint32_t rt = (uint32_t)j.px * (uint32_t)j.ssaa;
        if (rt != s_rtSize) {
            fprintf(stderr, "MapBaker serve: render target %u -> %u px\n", s_rtSize, rt);
            MapBakerWorld::SetRenderSize(rt);
            s_rtSize = rt;
        }

        // 2D WMO quad overlay. The table must be populated before compositing; the
        // collection pass renders the region once (pixels discarded) and fills it via the
        // World.cpp render hook. Auto-runs the first time quads are turned on, or on
        // qcollect. Runs here (event thread) before the job's own tile loop.
        if (j.qon && (j.qcollect || MapBakerQuadCount == 0)) {
            const uint64_t t0 = GetTickCount64();
            fprintf(stderr, "MapBaker serve: collecting WMO groups (region %dx%d tiles)\n",
                    s_w, s_h);
            s_state.phase = "collect";
            MapBakerWorld::CollectQuads(s_txMin, s_txMax, s_tyMin, s_tyMax, (uint32_t)rt);
            // Fresh table defaults: art mode paints only cells that have a real .blp, so
            // select everything (the city buildings are interior groups); tint mode keeps
            // the outdoor-minimap exterior set.
            MapBakerWorld::SetQuadSelectionByClass(j.qart ? 1 : 2);
            s_state.phase = "render";
            fprintf(stderr, "MapBaker serve: collected %u WMO groups in %.1fs\n",
                    MapBakerQuadCount, (GetTickCount64() - t0) / 1000.0);
        }
        ApplyQuadSelection(j);
        MapBakerWorld::SetQuadOverlay(
                j.qon ? 1 : 0, j.qwmo ? 1 : 0, (uint8_t)j.qalpha,
                (uint8_t)j.qext[0], (uint8_t)j.qext[1], (uint8_t)j.qext[2],
                (uint8_t)j.qint[0], (uint8_t)j.qint[1], (uint8_t)j.qint[2]);
        MapBakerWorld::SetQuadArt(j.qart ? 1 : 0, 255);
        MapBakerWorld::SetQuadFill(j.qfill ? 1 : 0);

        fprintf(stderr, "MapBaker serve: apply mode=%u zdir=%u k=%.0f skip=%.0f scope=%u "
                        "light=%.0f citylight=%.0f gain=%.3f px=%d ssaa=%d nowmo=%u dump=%u "
                        "qon=%u qart=%u qfill=%u qwmo=%u qalpha=%u groups=%u sel=%s\n",
                j.mode, j.zdir, j.k, j.skip, j.scope, j.light, j.citylight, j.gain, j.px, j.ssaa,
                (unsigned)j.nowmo, j.dump, j.qon, j.qart, j.qfill, j.qwmo, j.qalpha, MapBakerQuadCount,
                j.qselPresent ? (j.qsel.empty() ? "none" : j.qsel.c_str()) : "keep");
    }

    // Compose the montage + 2x city crop from the rendered tiles and write the PNGs.
    void FinalizeRun(const Job& j) {
        char p[600];
        const int32_t W = s_w * j.px;
        const int32_t H = s_h * j.px;

        snprintf(p, sizeof(p), "%smontage.png", s_outDir.c_str());

        // MapBaker diagnostic (MAPBAKER_DEBUG_VIEW=1): 8x8 block means of every montage
        // cell right before the PNG write (mfw). If mfw != mgc, s_montage was clobbered
        // between fill and write; if mfw == file content but != rtgrid, the corruption is
        // upstream of the fill.
        // MapBaker diagnostic (MAPBAKER_SERVE_RAW=1): dump the in-memory montage exactly
        // as the PNG writer receives it (raw RGB, no encoder). If this file is correct and
        // montage.png is not, the encoder is at fault; if both are wrong, the corruption
        // is in memory before the write.
        fprintf(stderr, "MapBaker dbg: mptr write   ptr=%p size=%zu\n",
                (const void*)s_montage.data(), s_montage.size());
        if (getenv("MAPBAKER_SERVE_RAW")) {
            char rp[600];
            snprintf(rp, sizeof(rp), "%smontage.raw", s_outDir.c_str());
            FILE* rf = fopen(rp, "wb");
            if (rf) {
                fwrite(s_montage.data(), 1, s_montage.size(), rf);
                fclose(rf);
            }
            fprintf(stderr, "MapBaker serve: wrote %s (%u bytes)\n", rp, (uint32_t)s_montage.size());
            fflush(stderr);
        }

        if (getenv("MAPBAKER_DEBUG_VIEW")) {
            // Exact pixel spot check: a few (x,y) RGB values of s_montage right before the
            // write. Compare against the same coordinates in montage.raw / montage.png to
            // tie the in-memory buffer to the on-disk file pixel-for-pixel.
            const int32_t spots[][2] = { {10, 10}, {500, 10}, {10, 500}, {500, 500},
                                         {700, 200}, {200, 700}, {900, 900}, {511, 511} };
            for (int32_t i = 0; i < 8; i++) {
                const int32_t x = spots[i][0], y = spots[i][1];
                const uint8_t* q = s_montage.data() + ((size_t)y * (size_t)W + (size_t)x) * 3;
                fprintf(stderr, "MapBaker dbg: pixspot (%d,%d) = %d,%d,%d\n", x, y, q[0], q[1], q[2]);
            }
            fflush(stderr);
        }

        if (getenv("MAPBAKER_DEBUG_VIEW")) {
            const int32_t g = 8;
            const int32_t bw = j.px / g;
            for (int32_t cty = 0; cty < s_h; cty++) {
                for (int32_t ctx = 0; ctx < s_w; ctx++) {
                    // Cell (ctx, cty) first pixel: X = ctx*px, Y = cty*px; montage is
                    // (s_w*px) wide. (An earlier version dropped a factor of px here.)
                    const uint8_t* base = s_montage.data() +
                                         ((size_t)cty * (size_t)(s_w * j.px) * (size_t)j.px + (size_t)ctx * (size_t)j.px) * 3;
                    const int32_t tx = s_txMin + ctx;
                    const int32_t ty = s_tyMin + cty;
                    for (int32_t gy = 0; gy < g; gy++) {
                        char line[256];
                        int q = 0;
                        for (int32_t gx = 0; gx < g; gx++) {
                            uint64_t sum = 0;
                            for (int32_t y = 1; y < bw - 1; y++) {
                                const uint8_t* r = base + ((size_t)(gy * bw + y) * (size_t)j.px + (size_t)(gx * bw + 1)) * 3;
                                for (int32_t x = 1; x < bw - 1; x++) {
                                    sum += r[(size_t)x * 3];
                                }
                            }
                            q += snprintf(line + q, sizeof(line) - q, "%3u ", (uint32_t)(sum / ((size_t)(bw - 2) * (bw - 2))));
                        }
                        fprintf(stderr, "MapBaker dbg: mfw tile(%d,%d) y%d: %s\n", tx, ty, gy, line);
                    }
                }
            }
            fprintf(stderr, "MapBaker dbg: mfw path=%s W=%d H=%d montageBytes=%u\n",
                    p, W, H, (uint32_t)s_montage.size());
            fflush(stderr);
        }

        if (!MapBakerPng::Write(p, W, H, s_montage.data())) {
            fprintf(stderr, "MapBaker serve: failed to write %s\n", p);
        }

        if (s_gateValid) {
            // 2x zoom of the gated city region (clipped to the rendered region).
            int32_t gtx0 = (s_gateTx0 > s_txMin) ? s_gateTx0 : s_txMin;
            int32_t gtx1 = (s_gateTx1 < s_txMax) ? s_gateTx1 : s_txMax;
            int32_t gty0 = (s_gateTy0 > s_tyMin) ? s_gateTy0 : s_tyMin;
            int32_t gty1 = (s_gateTy1 < s_tyMax) ? s_gateTy1 : s_tyMax;

            if (gtx1 >= gtx0 && gty1 >= gty0) {
                const int32_t x0 = (gtx0 - s_txMin) * j.px;
                const int32_t y0 = (gty0 - s_tyMin) * j.px;
                const int32_t cw = (gtx1 - gtx0 + 1) * j.px;
                const int32_t ch = (gty1 - gty0 + 1) * j.px;
                // Keep the crop buffer bounded: 2x when it fits 8192^2, else 1x.
                const int32_t factor = (2 * cw <= 8192 && 2 * ch <= 8192) ? 2 : 1;

                std::vector<uint8_t> dst((size_t)cw * factor * (size_t)ch * factor * 3, 0);
                for (int32_t oy = 0; oy < ch * factor; oy++) {
                    const int32_t sy = y0 + oy / factor;
                    for (int32_t ox = 0; ox < cw * factor; ox++) {
                        const int32_t sx = x0 + ox / factor;
                        uint32_t acc[3] = {0, 0, 0};
                        for (int32_t dy = 0; dy < factor; dy++) {
                            for (int32_t dx = 0; dx < factor; dx++) {
                                // The factor x factor box of the LAST output row/col
                                // extends one past the crop when the gate reaches the
                                // region edge (sy+dy == H, sx+dx == W). Clamp: without
                                // this the full-region gate heap-reads past s_montage.
                                const int32_t sy2 = (sy + dy >= y0 + ch) ? y0 + ch - 1 : sy + dy;
                                const int32_t sx2 = (sx + dx >= x0 + cw) ? x0 + cw - 1 : sx + dx;
                                const uint8_t* s = s_montage.data() +
                                                   ((size_t)sy2 * (size_t)W + (size_t)sx2) * 3;
                                acc[0] += s[0];
                                acc[1] += s[1];
                                acc[2] += s[2];
                            }
                        }
                        uint8_t* d = dst.data() + ((size_t)oy * (size_t)cw * factor + (size_t)ox) * 3;
                        d[0] = static_cast<uint8_t>(acc[0] / factor / factor);
                        d[1] = static_cast<uint8_t>(acc[1] / factor / factor);
                        d[2] = static_cast<uint8_t>(acc[2] / factor / factor);
                    }
                }

                snprintf(p, sizeof(p), "%scrop2x.png", s_outDir.c_str());
                if (!MapBakerPng::Write(p, cw * factor, ch * factor, dst.data())) {
                    fprintf(stderr, "MapBaker serve: failed to write %s\n", p);
                }
            }
        }

        if (j.qon && j.qart) {
            uint32_t looked = 0, hit = 0, missing = 0;
            MapBakerArtStats(&looked, &hit, &missing);
            fprintf(stderr, "MapBaker serve: minimap art cells: %u looked up, %u decoded, "
                            "%u missing\n", looked, hit, missing);
        }

        const uint64_t ms = GetTickCount64() - s_t0;
        EnterCriticalSection(&s_cs);
        s_state.applied = j;
        s_state.gen++;
        s_state.lastMs = ms;
        s_state.rendering = false;
        s_state.phase = "idle";
        LeaveCriticalSection(&s_cs);

        fprintf(stderr, "MapBaker serve: %d tiles in %.1fs -> montage.png%s\n", s_state.total,
                ms / 1000.0, s_gateValid ? " + crop2x.png" : "");
    }

    // Event-thread idle callback: one tile per idle tick (live progress + the message
    // loop keeps pumping AsyncFile polls between tiles).
    int32_t ServeIdleCallback(const void* data, void* param) {
        (void)data;
        (void)param;

        Job job;
        bool runTile = false;
        {
            EnterCriticalSection(&s_cs);
            if (!s_active && s_state.pending) {
                s_runJob = s_state.pendingJob;
                s_state.pending = false;
                s_active = true;
                s_idx = 0;
                s_t0 = GetTickCount64();
                s_state.rendering = true;
                s_state.phase = "render";
                s_state.done = 0;
                s_state.total = (int32_t)s_tiles.size();
            }
            if (s_active) {
                job = s_runJob;
                runTile = true;
            }
            LeaveCriticalSection(&s_cs);
        }

        if (!runTile) {
            return 1;
        }

        const bool first = (s_idx == 0);
        if (first) {
            ApplyJob(job);
            s_tileBuf.assign((size_t)job.px * (size_t)job.px * 3, 0);
            s_montage.assign((size_t)s_w * (size_t)job.px * (size_t)s_h * (size_t)job.px * 3, 0);
        }

        // MapBaker diagnostic (MAPBAKER_DEBUG_VIEW=1): 8x8 block means of the tile buffer
        // right after RenderTile (tbg) and of the just-filled montage cell (mgc).
        // Compares the in-memory pipeline stages against the engine rtgrid to localize
        // the serve Y-freeze between readback, fill, and write.
        auto grid8 = [](const uint8_t* buf, int32_t sizePx, const char* label, int32_t tx, int32_t ty) {
            if (!getenv("MAPBAKER_DEBUG_VIEW") || sizePx <= 0) {
                return;
            }
            const int32_t g = 8;
            const int32_t bw = sizePx / g;
            for (int32_t gy = 0; gy < g; gy++) {
                char line[256];
                int p = 0;
                for (int32_t gx = 0; gx < g; gx++) {
                    uint64_t sum = 0;
                    for (int32_t y = 1; y < bw - 1; y++) {
                        const uint8_t* r = buf + ((size_t)(gy * bw + y) * (size_t)sizePx + (size_t)(gx * bw + 1)) * 3;
                        for (int32_t x = 1; x < bw - 1; x++) {
                            sum += r[(size_t)x * 3];
                        }
                    }
                    p += snprintf(line + p, sizeof(line) - p, "%3u ", (uint32_t)(sum / ((size_t)(bw - 2) * (bw - 2))));
                }
                fprintf(stderr, "MapBaker dbg: %s tile(%d,%d) y%d: %s\n", label, tx, ty, gy, line);
            }
            fflush(stderr);
        };
        // Same, but for a cell INSIDE a wider image (the montage): reads the
        // cell at the image's real row stride. (grid8 on the montage cell used
        // to assume a px-wide image, which is exactly how the linear-fill bug
        // stayed invisible: it re-read the tile's linear bytes at tile stride
        // and saw "correct" content no matter how the bytes were placed.)
        auto cellGrid8 = [](const uint8_t* m, int32_t imgW, int32_t cx, int32_t cy,
                            int32_t px, const char* label, int32_t tx, int32_t ty) {
            if (!getenv("MAPBAKER_DEBUG_VIEW") || px <= 0) {
                return;
            }
            const int32_t g = 8;
            const int32_t bw = px / g;
            for (int32_t gy = 0; gy < g; gy++) {
                char line[256];
                int p = 0;
                for (int32_t gx = 0; gx < g; gx++) {
                    uint64_t sum = 0;
                    for (int32_t y = 1; y < bw - 1; y++) {
                        const uint8_t* r = m +
                            (((size_t)(cy + gy * bw + y) * (size_t)imgW + (size_t)(cx + gx * bw + 1)) * 3);
                        for (int32_t x = 1; x < bw - 1; x++) {
                            sum += r[(size_t)x * 3];
                        }
                    }
                    p += snprintf(line + p, sizeof(line) - p, "%3u ",
                                  (uint32_t)(sum / ((size_t)(bw - 2) * (bw - 2))));
                }
                fprintf(stderr, "MapBaker dbg: %s tile(%d,%d) y%d: %s\n", label, tx, ty, gy, line);
            }
            fflush(stderr);
        };

        const std::pair<int32_t, int32_t> t = s_tiles[s_idx];
        if (MapBakerWorld::RenderTile(t.first, t.second, job.px, s_tileBuf.data())) {
            // MapBaker fix (2026-11-08): place the tile cell ROW-BY-ROW at the
            // montage stride (montage width = s_w*px), NOT as one linear byte
            // block. A linear memcpy of a px-wide image into a (s_w*px)-wide
            // canvas scatters the tile across the full canvas width (montage
            // row r becomes [tile row 2r | tile row 2r+1]) — the root cause of
            // the serve-mode "strip" montages. For s_w == 1 the linear copy is
            // coincidentally correct, which is why the bug only appeared in
            // multi-column regions.
            const size_t MW = (size_t)s_w * (size_t)job.px;
            const size_t rowBytes = (size_t)job.px * 3;
            const size_t off = (((size_t)(t.second - s_tyMin) * (size_t)job.px) * MW +
                                (size_t)(t.first - s_txMin) * (size_t)job.px) * 3;  // cell top-left
            const uint8_t* src = s_tileBuf.data();
            for (int32_t r = 0; r < job.px; r++) {
                memcpy(s_montage.data() + off + (size_t)r * MW * 3,
                       src + (size_t)r * rowBytes, rowBytes);
            }

            // MapBaker diagnostic (MAPBAKER_TRACE=1): baseline spot snapshot taken
            // IMMEDIATELY after the memcpy (guaranteed-correct moment), before any
            // probe. The tight poll below (no syscalls between reads) catches a
            // concurrent write landing even inside the microsecond gap after the
            // last probe read; iteration 0 catches a write that already landed.
            //
            // SPOTS ARE READ VIA VOLATILE: at /O2 the compiler may cache/CSE loads of
            // s_montage across the poll loop (the code itself never writes it), which
            // would make the monitor blind to an external writer. The volatile cast
            // forces a real load every time.
            uint8_t traceBase[8] = {};
            uint8_t traceTile[8] = {};
            bool traceOn = false;
            const int32_t spotMW = s_w * job.px;
            const int32_t spotMH = s_h * job.px;
            const int32_t spotFX[8] = { 102, (int32_t)(0.75f * spotMW), 102, (int32_t)(0.75f * spotMW),
                                        102, (int32_t)(0.75f * spotMW), 102, (int32_t)(0.75f * spotMW) };
            const int32_t spotFY[8] = { 102, 102, (int32_t)(0.375f * spotMH), (int32_t)(0.375f * spotMH),
                                        (int32_t)(0.625f * spotMH), (int32_t)(0.625f * spotMH),
                                        (int32_t)(0.875f * spotMH), (int32_t)(0.875f * spotMH) };
            auto readSpots = [&](uint8_t* out) {
                const volatile uint8_t* m = s_montage.data();
                for (int i = 0; i < 8; i++) {
                    out[i] = m[(size_t)spotFY[i] * (size_t)spotMW * 3 + (size_t)spotFX[i] * 3];
                }
            };
            // Reads the tile-buffer pixel that lands at each montage spot. Spots that
            // fall OUTSIDE the cell being filled print 255 (no comparison possible).
            auto readTileSpots = [&](uint8_t* out) {
                const size_t cellW = (size_t)job.px;
                const size_t cellH = (size_t)job.px;
                const size_t ox = (size_t)(t.first - s_txMin) * cellW;
                const size_t oy = (size_t)(t.second - s_tyMin) * cellH;
                const volatile uint8_t* m = s_tileBuf.data();
                for (int i = 0; i < 8; i++) {
                    if ((size_t)spotFX[i] >= ox && (size_t)spotFX[i] < ox + cellW &&
                        (size_t)spotFY[i] >= oy && (size_t)spotFY[i] < oy + cellH) {
                        out[i] = m[((size_t)spotFY[i] - oy) * cellW * 3 +
                                   ((size_t)spotFX[i] - ox) * 3];
                    } else {
                        out[i] = 255;
                    }
                }
            };
            auto printSpots = [&](const char* label, const uint8_t* v) {
                fprintf(stderr, "MapBaker trace: fill%d %s: %u %u %u %u %u %u %u %u\n",
                        s_idx, label, v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7]);
            };
            if (getenv("MAPBAKER_TRACE")) {
                readSpots(traceBase);
                readTileSpots(traceTile);
                printSpots("base", traceBase);
                printSpots("tile", traceTile);
                traceOn = true;
            }

            grid8(s_tileBuf.data(), job.px, "tbg", t.first, t.second);
            cellGrid8(s_montage.data(), s_w * job.px,
                      (t.first - s_txMin) * job.px, (t.second - s_tyMin) * job.px,
                      job.px, "mgc", t.first, t.second);

            if (traceOn) {
                uint8_t mgcSpots[8] = {};
                readSpots(mgcSpots);
                printSpots("mgc ", mgcSpots);
                // Phase 1: tight busy-poll (no Sleep between reads). Catches a write
                // landing within ~10-40 ms after the memcpy, including inside the gap
                // between the mgc probe reads and this loop's first read.
                uint8_t cur[8] = {};
                bool caught = false;
                const uint32_t tightIters = 2000000;
                for (uint32_t i = 0; i < tightIters && !caught; i++) {
                    readSpots(cur);
                    if (memcmp(traceBase, cur, sizeof(cur)) != 0) {
                        char tag[64];
                        snprintf(tag, sizeof(tag), "fill%d-tight(iter%u)", s_idx, i);
                        fprintf(stderr, "MapBaker trace: fill%d spots changed at tight iter %u\n",
                                s_idx, i);
                        fflush(stderr);
                        DumpAllThreadStacks(tag);
                        caught = true;
                    }
                }
                // Phase 2: sleep poll for a later (delayed/periodic) write.
                if (!caught) {
                    for (int i = 0; i < 40; i++) {
                        Sleep(5);
                        readSpots(cur);
                        if (memcmp(traceBase, cur, sizeof(cur)) != 0) {
                            char tag[64];
                            snprintf(tag, sizeof(tag), "fill%d-sleep(iter%d)", s_idx, i);
                            fprintf(stderr, "MapBaker trace: fill%d spots changed at sleep iter %d\n",
                                    s_idx, i);
                            fflush(stderr);
                            DumpAllThreadStacks(tag);
                            caught = true;
                            break;
                        }
                    }
                }
            }

            // MapBaker diagnostic (MAPBAKER_SERVE_RAW=1): full-montage raw dump right
            // after each fill + pointer/size probe. Locates the corruption to a stage:
            // montage_fill3 == correct but montage_done != correct => clobber in the
            // done-block gap; ptr change => a second job re-entered (assign reallocated).
            fprintf(stderr, "MapBaker dbg: mptr fill%d ptr=%p size=%zu\n",
                    s_idx, (const void*)s_montage.data(), s_montage.size());
            if (getenv("MAPBAKER_SERVE_RAW")) {
                char fp[700];
                snprintf(fp, sizeof(fp), "%smontage_fill%d.raw", s_outDir.c_str(), s_idx);
                FILE* ff = fopen(fp, "wb");
                if (ff) {
                    fwrite(s_montage.data(), 1, s_montage.size(), ff);
                    fclose(ff);

                    // Local C: copy of the same fwrite source — isolates the Z: SMB
                    // write path from any in-process corruption (dbg10).
                    char lp[700];
                    snprintf(lp, sizeof(lp), "C:\\Users\\Luke\\AppData\\Local\\Temp\\"
                                            "mb_serve_local\\montage_fill%d.raw", s_idx);
                    CreateDirectoryA("C:\\Users\\Luke\\AppData\\Local\\Temp\\mb_serve_local",
                                     nullptr);
                    FILE* lf = fopen(lp, "wb");
                    if (lf) {
                        fwrite(s_montage.data(), 1, s_montage.size(), lf);
                        fclose(lf);
                    }

                    // Disk read-back: reopen the file just written (Z:) and read the
                    // 8 spot bytes back. disk==memory(tile) => external/Z: corruption;
                    // disk==strip while memory==tile => fwrite read different bytes.
                    if (getenv("MAPBAKER_TRACE")) {
                        FILE* rf = fopen(fp, "rb");
                        if (rf) {
                            uint8_t dv[8];
                            for (int i = 0; i < 8; i++) {
                                const size_t boff = ((size_t)spotFY[i] * (size_t)spotMW +
                                                     (size_t)spotFX[i]) * 3;
                                if (fseek(rf, (long)boff, SEEK_SET) != 0 ||
                                    fread(&dv[i], 1, 1, rf) != 1) {
                                    dv[i] = 255;
                                }
                            }
                            fclose(rf);
                            printSpots("disk", dv);
                        } else {
                            fprintf(stderr, "MapBaker trace: fill%d disk re-open failed\n",
                                    s_idx);
                        }
                    }
                }
            }

            if (traceOn) {
                // Post-dump memory check (always prints the values now, dbg10).
                uint8_t cur[8];
                readSpots(cur);
                printSpots("post", cur);
                if (memcmp(traceBase, cur, sizeof(cur)) != 0) {
                    fprintf(stderr, "MapBaker trace: fill%d spots differ after raw dump\n",
                            s_idx);
                    fflush(stderr);
                    DumpAllThreadStacks("fill-postdump-change");
                }
            }
        } else {
            fprintf(stderr, "MapBaker serve: tile (%d, %d) failed\n", t.first, t.second);
        }

        const bool done = (s_idx == (int32_t)s_tiles.size() - 1);
        EnterCriticalSection(&s_cs);
        s_idx++;
        s_state.done = s_idx;
        if (done) {
            s_state.phase = "composite";
        }
        LeaveCriticalSection(&s_cs);

        if (done) {
            fprintf(stderr, "MapBaker dbg: mptr done   ptr=%p size=%zu\n",
                    (const void*)s_montage.data(), s_montage.size());
            if (getenv("MAPBAKER_SERVE_RAW")) {
                char fp[700];
                snprintf(fp, sizeof(fp), "%smontage_done.raw", s_outDir.c_str());
                FILE* ff = fopen(fp, "wb");
                if (ff) {
                    fwrite(s_montage.data(), 1, s_montage.size(), ff);
                    fclose(ff);
                }
            }
            FinalizeRun(job);
            s_active = false;
        }
        return 1;
    }

    // ---------------------------------------------------------------------------
    // HTTP handlers (server thread; engine is only touched on the event thread)
    // ---------------------------------------------------------------------------

    bool ParseJob(const std::string& q, Job* j, std::string* err) {
        double k = GetParamF(q, "k", 1500.0);
        if (k < 300.0 || k > 4000.0) {
            *err = "K must be 300..4000";
            return false;
        }
        double skip = GetParamF(q, "skip", 0.0);
        if (skip < 0.0 || skip > 2000.0) {
            *err = "skip-above must be 0..2000";
            return false;
        }
        double gain = GetParamF(q, "gain", 1.0);
        if (gain < 0.2 || gain > 3.0) {
            *err = "gain must be 0.2..3";
            return false;
        }
        double light = GetParamF(q, "light", -1.0);
        if (light < -1.0 || light > 90.0) {
            *err = "light-elev must be -1..90";
            return false;
        }
        double citylight = GetParamF(q, "citylight", -1.0);
        if (citylight < -1.0 || citylight > 90.0) {
            *err = "city-elev must be -1..90";
            return false;
        }
        const int32_t mode = GetParamI(q, "mode", 3);
        if (mode < 0 || mode > 3) {
            *err = "mode must be 0..3";
            return false;
        }
        const int32_t zdir = GetParamI(q, "zdir", 1);
        if (zdir != 0 && zdir != 1) {
            *err = "zdir must be 0 or 1";
            return false;
        }
        const int32_t scope = GetParamI(q, "scope", 1);
        if (scope != 0 && scope != 1) {
            *err = "scope must be 0 or 1";
            return false;
        }
        const int32_t dump = GetParamI(q, "dump", 0);
        const int32_t nowmo = GetParamI(q, "nowmo", 0);
        if ((dump != 0 && dump != 1) || (nowmo != 0 && nowmo != 1)) {
            *err = "dump/nowmo must be 0 or 1";
            return false;
        }
        const int32_t qon = GetParamI(q, "qon", 0);
        const int32_t qart = GetParamI(q, "qart", 1);
        const int32_t qfill = GetParamI(q, "qfill", 1);
        const int32_t qwmo = GetParamI(q, "qwmo", 0);
        const int32_t qcollect = GetParamI(q, "qcollect", 0);
        if ((qon != 0 && qon != 1) || (qart != 0 && qart != 1) ||
            (qfill != 0 && qfill != 1) ||
            (qwmo != 0 && qwmo != 1) || (qcollect != 0 && qcollect != 1)) {
            *err = "qon/qart/qfill/qwmo/qcollect must be 0 or 1";
            return false;
        }
        const int32_t qalpha = GetParamI(q, "qalpha", 160);
        if (qalpha < 0 || qalpha > 255) {
            *err = "qalpha must be 0..255";
            return false;
        }
        // Tint colors as #RRGGBB (without the hash, via the page's color inputs).
        std::string qext, qint;
        GetParam(q, "qext", &qext);
        GetParam(q, "qint", &qint);
        uint32_t ext[3] = { 232, 224, 200 };
        uint32_t intc[3] = { 200, 214, 236 };
        if (qext.size() == 6) {
            unsigned v;
            sscanf(qext.c_str(), "%06x", &v);
            ext[0] = (v >> 16) & 0xFF; ext[1] = (v >> 8) & 0xFF; ext[2] = v & 0xFF;
        }
        if (qint.size() == 6) {
            unsigned v;
            sscanf(qint.c_str(), "%06x", &v);
            intc[0] = (v >> 16) & 0xFF; intc[1] = (v >> 8) & 0xFF; intc[2] = v & 0xFF;
        }
        const int32_t qmode = GetParamI(q, "qmode", 0);
        if (qmode < 0 || qmode > 3) {
            *err = "qmode must be 0..3";
            return false;
        }
        const int32_t px = GetParamI(q, "px", 1024);
        if (px != 512 && px != 1024 && px != 2048) {
            *err = "px must be 512, 1024 or 2048";
            return false;
        }
        const int32_t ssaa = GetParamI(q, "ssaa", 2);
        if (ssaa < 1 || ssaa > 4) {
            *err = "ssaa must be 1..4";
            return false;
        }
        if ((uint32_t)px * (uint32_t)ssaa > 4096) {
            *err = "px * ssaa must be <= 4096 (render-target limit)";
            return false;
        }

        j->mode = (uint32_t)mode;
        j->zdir = (uint32_t)zdir;
        j->k = (float)k;
        j->scope = (uint32_t)scope;
        j->skip = (float)skip;
        j->dump = (uint32_t)dump;
        j->gain = (float)gain;
        j->light = (float)light;
        j->citylight = (float)citylight;
        j->px = px;
        j->ssaa = ssaa;
        j->nowmo = nowmo != 0;
        j->qon = (uint32_t)qon;
        j->qart = (uint32_t)qart;
        j->qfill = (uint32_t)qfill;
        j->qwmo = (uint32_t)qwmo;
        j->qalpha = (uint32_t)qalpha;
        memcpy(j->qext, ext, sizeof(j->qext));
        memcpy(j->qint, intc, sizeof(j->qint));
        j->qmode = (uint32_t)qmode;
        std::string qsel;
        j->qselPresent = GetParam(q, "qsel", &qsel);
        j->qsel = std::move(qsel);
        j->qcollect = (uint32_t)qcollect;
        return true;
    }

    void HandleStatus(SOCKET sock) {
        EnterCriticalSection(&s_cs);
        char set[512];
        sprintf(set,
                "{\"mode\":%u,\"zdir\":%u,\"k\":%.6g,\"skip\":%.6g,\"scope\":%u,\"light\":%.6g,"
                "\"citylight\":%.6g,\"gain\":%.6g,\"px\":%d,\"ssaa\":%d,\"nowmo\":%s,\"dump\":%u,"
                "\"qon\":%u,\"qart\":%u,\"qfill\":%u,\"qwmo\":%u,\"qalpha\":%u,\"qext\":\"%02x%02x%02x\","
                "\"qint\":\"%02x%02x%02x\",\"qcount\":%u}",
                s_state.applied.mode, s_state.applied.zdir, s_state.applied.k, s_state.applied.skip,
                s_state.applied.scope, s_state.applied.light, s_state.applied.citylight,
                s_state.applied.gain, s_state.applied.px, s_state.applied.ssaa,
                s_state.applied.nowmo ? "true" : "false", s_state.applied.dump,
                s_state.applied.qon, s_state.applied.qart, s_state.applied.qfill,
                s_state.applied.qwmo, s_state.applied.qalpha,
                s_state.applied.qext[0], s_state.applied.qext[1], s_state.applied.qext[2],
                s_state.applied.qint[0], s_state.applied.qint[1], s_state.applied.qint[2],
                MapBakerQuadCount);
        char body[2048];
        sprintf(body,
                "{\"rendering\":%s,\"phase\":\"%s\",\"done\":%d,\"total\":%d,\"gen\":%llu,"
                "\"lastMs\":%llu,\"hasCrop\":%s,\"settings\":%s,\"baseCmd\":\"%s\"}",
                s_state.rendering ? "true" : "false", s_state.phase, s_state.done, s_state.total,
                (unsigned long long)s_state.gen, (unsigned long long)s_state.lastMs,
                s_gateValid ? "true" : "false", set, JsonEscape(s_baseCmd).c_str());
        LeaveCriticalSection(&s_cs);
        SendResponse(sock, "200 OK", "application/json", body, strlen(body));
    }

    // /groups.json: the WMO quad table (the web UI's per-group list). One object per
    // (WMO, group) the collection pass saw: stable index, names, the exterior bit,
    // world bbox, current selection. Read from the server thread; the table is only
    // mutated by the event thread during a collection pass, and the page refreshes the
    // list after a render completes, so the two never meaningfully overlap.
    void HandleGroups(SOCKET sock) {
        std::string body = "[";
        for (uint32_t i = 0; i < MapBakerQuadCount; i++) {
            const MapBakerQuadEntry& e = MapBakerQuadTable[i];
            char item[640];
            sprintf(item, "%s{\"i\":%u,\"wmo\":\"%s\",\"smo\":\"%s\",\"ext\":%d,"
                          "\"xy\":[%.3f,%.3f,%.3f,%.3f],\"z\":[%.1f,%.1f],\"sel\":%d}",
                    i ? "," : "", i,
                    JsonEscape(e.m_wmoName).c_str(), JsonEscape(e.m_smoName).c_str(),
                    (e.m_smoFlags & 0x8) ? 1 : 0,
                    (double)e.m_x0, (double)e.m_x1, (double)e.m_y0, (double)e.m_y1,
                    (double)e.m_z0, (double)e.m_z1, e.m_selected ? 1 : 0);
            body += item;
        }
        body += "]";
        SendResponse(sock, "200 OK", "application/json", body.c_str(), (uint32_t)body.size());
    }

    void HandleRender(SOCKET sock, const std::string& query) {
        std::string err;
        Job j;
        if (!ParseJob(query, &j, &err)) {
            char body[256];
            sprintf(body, "{\"ok\":false,\"error\":\"%s\"}", JsonEscape(err).c_str());
            SendResponse(sock, "200 OK", "application/json", body, strlen(body));
            return;
        }

        EnterCriticalSection(&s_cs);
        if (s_state.pending || s_state.rendering) {
            LeaveCriticalSection(&s_cs);
            const char* body = "{\"ok\":false,\"error\":\"render in progress\"}";
            SendResponse(sock, "200 OK", "application/json", body, strlen(body));
            return;
        }
        s_state.pendingJob = j;
        s_state.pending = true;
        s_state.total = (int32_t)s_tiles.size();
        LeaveCriticalSection(&s_cs);
        const char* body = "{\"ok\":true}";
        SendResponse(sock, "200 OK", "application/json", body, strlen(body));
    }

    bool ValidPresetName(const std::string& n) {
        if (n.empty() || n.size() > 40) {
            return false;
        }
        for (char c : n) {
            if (c == '\t' || c == '\n' || c == '\r' || c < 32) {
                return false;
            }
        }
        return true;
    }

    // presets.txt: one line per preset, tab-separated:
    // name mode zdir k skip scope light citylight gain px ssaa nowmo dump
    void HandlePresets(SOCKET sock, const std::string& query) {
        const std::string path = s_outDir + "presets.txt";
        std::string op = "list";
        std::string name;
        GetParam(query, "op", &op);
        GetParam(query, "name", &name);

        if (op == "save") {
            std::string err;
            Job j;
            if (!ValidPresetName(name) || !ParseJob(query, &j, &err)) {
                char body[256];
                sprintf(body, "{\"ok\":false,\"error\":\"%s\"}",
                        JsonEscape(ValidPresetName(name) ? err : "bad preset name").c_str());
                SendResponse(sock, "200 OK", "application/json", body, strlen(body));
                return;
            }
            char line[512];
            sprintf(line, "%s\t%u\t%u\t%.6g\t%.6g\t%u\t%.6g\t%.6g\t%.6g\t%d\t%d\t%u\t%u\n",
                    name.c_str(), j.mode, j.zdir, (double)j.k, (double)j.skip, j.scope,
                    (double)j.light, (double)j.citylight, (double)j.gain, j.px, j.ssaa,
                    (unsigned)j.nowmo, j.dump);

            std::string old = ReadWholeFile(path);
            std::string out;
            size_t pos = 0;
            while (pos <= old.size()) {
                const size_t nl = old.find('\n', pos);
                const size_t len = (nl == std::string::npos ? old.size() : nl) - pos;
                const std::string cur = old.substr(pos, len);
                // Replace an existing preset with the same name.
                if (cur.size() > name.size() + 1 && cur.substr(0, name.size() + 1) == name + "\t") {
                    pos = (nl == std::string::npos ? old.size() : nl + 1);
                    continue;
                }
                out += cur;
                if (nl != std::string::npos) {
                    out.push_back('\n');
                }
                pos = (nl == std::string::npos ? old.size() : nl + 1);
            }
            out += line;
            if (!WriteWholeFile(path, out)) {
                SendText(sock, "500 Internal Server Error", "write failed\n");
                return;
            }
            const char* body = "{\"ok\":true}";
            SendResponse(sock, "200 OK", "application/json", body, strlen(body));
            return;
        }

        if (op == "del") {
            if (!ValidPresetName(name)) {
                SendText(sock, "400 Bad Request", "bad name\n");
                return;
            }
            std::string old = ReadWholeFile(path);
            std::string out;
            size_t pos = 0;
            while (pos <= old.size()) {
                const size_t nl = old.find('\n', pos);
                const size_t len = (nl == std::string::npos ? old.size() : nl) - pos;
                const std::string cur = old.substr(pos, len);
                if (cur.size() > name.size() + 1 && cur.substr(0, name.size() + 1) == name + "\t") {
                    pos = (nl == std::string::npos ? old.size() : nl + 1);
                    continue;
                }
                out += cur;
                if (nl != std::string::npos) {
                    out.push_back('\n');
                }
                pos = (nl == std::string::npos ? old.size() : nl + 1);
            }
            if (!WriteWholeFile(path, out)) {
                SendText(sock, "500 Internal Server Error", "write failed\n");
                return;
            }
            const char* body = "{\"ok\":true}";
            SendResponse(sock, "200 OK", "application/json", body, strlen(body));
            return;
        }

        // Default: list.
        std::string body = ReadWholeFile(path);
        if (body.empty()) {
            body = "\n";
        }
        SendResponse(sock, "200 OK", "text/plain; charset=utf-8", body.c_str(), body.size());
    }

    void HandleConnection(SOCKET sock) {
        char hdr[16384];
        int total = 0;
        bool complete = false;
        while (total < (int)sizeof(hdr) - 1) {
            // Read as many available bytes as fit (recv returns the count; the bytes land
            // directly in hdr[total..]). The header is tiny, so this is normally one recv.
            const int n = recv(sock, hdr + total, (int)(sizeof(hdr) - 1 - total), 0);
            if (n <= 0) {
                return;
            }
            total += n;
            // Header ends with "\r\n\r\n": once we have >= 4 bytes, check the last four.
            if (total >= 4 && hdr[total - 4] == '\r' && hdr[total - 3] == '\n' &&
                hdr[total - 2] == '\r' && hdr[total - 1] == '\n') {
                complete = true;
                break;
            }
        }
        if (!complete) {
            return;
        }
        hdr[total] = '\0';

        char method[16] = {};
        char target[2048] = {};
        if (sscanf(hdr, "%15s %2047s", method, target) != 2) {
            return;
        }
        if (strcmp(method, "GET") != 0) {
            SendText(sock, "405 Method Not Allowed", "GET only\n");
            return;
        }

        char* qmark = strchr(target, '?');
        std::string path = qmark ? std::string(target, qmark) : std::string(target);
        const std::string query = qmark ? std::string(qmark) : std::string();

        if (path == "/" || path == "/index.html") {
            SendResponse(sock, "200 OK", "text/html; charset=utf-8", kHtml, strlen(kHtml));
        } else if (path == "/status") {
            HandleStatus(sock);
        } else if (path == "/render") {
            // MapBaker diagnostic: echo the raw target so query mangling is visible.
            fprintf(stderr, "MapBaker serve: render target='%s'\n", target);
            fflush(stderr);
            HandleRender(sock, query);
        } else if (path == "/montage.png") {
            ServeFile(sock, s_outDir + "montage.png", "image/png");
        } else if (path == "/crop2x.png") {
            ServeFile(sock, s_outDir + "crop2x.png", "image/png");
        } else if (path == "/dump.txt") {
            ServeFile(sock, s_outDir + "wmo_dump.txt", "text/plain; charset=utf-8");
        } else if (path == "/groups.json") {
            HandleGroups(sock);
        } else if (path == "/presets") {
            HandlePresets(sock, query);
        } else if (path == "/favicon.ico") {
            SendResponse(sock, "204 No Content", "text/plain", nullptr, 0);
        } else {
            SendText(sock, "404 Not Found", "not found\n");
        }
    }

    void ServerThread() {
        // Winsock is started by Start() (before the listen socket is created); the
        // matching WSACleanup happens here on the way out.
        // Accept loop with a timed select so Stop() can break out promptly. Each
        // connection is served on its own (detached) thread: the page auto-loads the
        // ~82 MB montage + 46 MB crop after every render, and a serial loop would let
        // that large transfer block the small /status polling and any new /render.
        timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 500 * 1000;
        while (s_run) {
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(s_listen, &fds);
            const int r = select(0, &fds, nullptr, nullptr, &tv);
            if (r < 0) {
                fprintf(stderr, "MapBaker serve: select error %ld (retrying)\n",
                        (long)WSAGetLastError());
                continue;
            }
            if (r == 0) {
                continue; // timed out; re-check s_run
            }
            SOCKET c = accept(s_listen, nullptr, nullptr);
            if (c == INVALID_SOCKET) {
                fprintf(stderr, "MapBaker serve: accept error %ld (retrying)\n",
                        (long)WSAGetLastError());
                continue;
            }
            std::thread([c]() {
                DWORD to = 15000; // 15 s on the request-header recv; drop hung clients
                setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, (const char*)&to, sizeof(to));
                HandleConnection(c);
                closesocket(c);
            }).detach();
        }
        WSACleanup();
    }

    // ---------------------------------------------------------------------------
    // Setup helpers
    // ---------------------------------------------------------------------------

    std::vector<std::pair<int32_t, int32_t>> BuildTiles(const MapBakerApp::Config& cfg) {
        std::vector<std::pair<int32_t, int32_t>> v;
        if (cfg.allTiles) {
            for (int32_t ty = 0; ty < 64; ty++) {
                for (int32_t tx = 0; tx < 64; tx++) {
                    if (CMap::m_areaInfo[64 * ty + tx].m_flags & 1) {
                        v.push_back(std::make_pair(tx, ty));
                    }
                }
            }
            return v;
        }
        for (int32_t ty = cfg.ty0; ty <= cfg.ty1; ty++) {
            for (int32_t tx = cfg.tx0; tx <= cfg.tx1; tx++) {
                v.push_back(std::make_pair(tx, ty));
            }
        }
        return v;
    }

    std::string BuildBaseCmd(const MapBakerApp::Config& c) {
        std::string s = "MapBaker64.exe --client " + std::string(c.clientPath) + " --map " +
                        std::string(c.mapArg) + " --preset " + std::string(c.preset);
        if (c.allTiles) {
            s += " --all";
        } else {
            s += " --tx0 " + std::to_string(c.tx0) + " --tx1 " + std::to_string(c.tx1) +
                 " --ty0 " + std::to_string(c.ty0) + " --ty1 " + std::to_string(c.ty1);
        }
        s += " --out " + std::string(c.outDir);
        if (c.cityFile) {
            s += " --city-file " + std::string(c.cityFile);
        }
        return s;
    }

    Job JobFromConfig(const MapBakerApp::Config& c) {
        Job j;
        j.mode = c.xrayMode;
        j.zdir = c.xrayLowLast;
        j.k = c.xrayK;
        j.scope = c.xrayScope;
        j.skip = c.xraySkipAbove;
        j.dump = c.xrayDump;
        j.gain = c.gain;
        j.light = c.lightElev;
        j.citylight = c.cityElev;
        j.px = c.px;
        j.ssaa = c.ssaa;
        j.nowmo = c.noWmo;
        j.qon = c.quads ? 1 : 0;
        j.qart = c.quadArt ? 1 : 0;
        j.qfill = c.quadFill ? 1 : 0;
        j.qwmo = c.quadWmo ? 1 : 0;
        j.qalpha = (uint32_t)(c.quadAlpha * 255.0f + 0.5f);
        // Tints: MUST match the /render parser defaults and the page's color-input
        // defaults. Leaving these zero-initialized rendered the startup job with
        // BLACK quads, and the page's formFrom then propagated the black from
        // /status into the pickers, so every follow-up render kept it too.
        j.qext[0] = 232; j.qext[1] = 224; j.qext[2] = 200;
        j.qint[0] = 200; j.qint[1] = 214; j.qint[2] = 236;
        return j;
    }

}  // namespace

namespace MapBakerServe {

    bool Start(const MapBakerApp::Config& config) {
        // Winsock must be up before socket() below; nothing else in the process starts it.
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            fprintf(stderr, "MapBaker serve: WSAStartup failed (error %lu)\n",
                    (unsigned long)WSAGetLastError());
            return false;
        }

        InitializeCriticalSection(&s_cs);
        s_outDir = config.outDir;
        s_tiles = BuildTiles(config);
        if (s_tiles.empty()) {
            fprintf(stderr, "MapBaker serve: no tiles to render\n");
            return false;
        }

        s_txMin = s_txMax = s_tiles[0].first;
        s_tyMin = s_tyMax = s_tiles[0].second;
        for (const auto& t : s_tiles) {
            s_txMin = (t.first < s_txMin) ? t.first : s_txMin;
            s_txMax = (t.first > s_txMax) ? t.first : s_txMax;
            s_tyMin = (t.second < s_tyMin) ? t.second : s_tyMin;
            s_tyMax = (t.second > s_tyMax) ? t.second : s_tyMax;
        }
        s_w = s_txMax - s_txMin + 1;
        s_h = s_tyMax - s_tyMin + 1;

        // Gate bbox (city ADT list) for the 2x crop.
        if (config.cityFile) {
            FILE* f = fopen(config.cityFile, "r");
            if (f) {
                s_gateTx0 = 999999;
                s_gateTy0 = 999999;
                s_gateTx1 = -1;
                s_gateTy1 = -1;
                int tx = 0, ty = 0, n = 0;
                while (fscanf(f, "%d %d", &tx, &ty) == 2) {
                    if (tx < 0 || ty < 0 || tx >= 64 || ty >= 64) {
                        continue;
                    }
                    s_gateTx0 = (tx < s_gateTx0) ? tx : s_gateTx0;
                    s_gateTx1 = (tx > s_gateTx1) ? tx : s_gateTx1;
                    s_gateTy0 = (ty < s_gateTy0) ? ty : s_gateTy0;
                    s_gateTy1 = (ty > s_gateTy1) ? ty : s_gateTy1;
                    n++;
                }
                fclose(f);
                s_gateValid = n > 0;
                fprintf(stderr, "MapBaker serve: gate bbox (%d..%d, %d..%d) from %s\n", s_gateTx0,
                        s_gateTx1, s_gateTy0, s_gateTy1, config.cityFile);
            }
        }

        s_baseCmd = BuildBaseCmd(config);
        s_rtSize = (uint32_t)config.px * (uint32_t)config.ssaa;

        // Queue the initial job (CLI settings) so the page has an image right away.
        const Job init = JobFromConfig(config);
        EnterCriticalSection(&s_cs);
        s_state.applied = init;
        s_state.pendingJob = init;
        s_state.pending = true;
        s_state.total = (int32_t)s_tiles.size();
        LeaveCriticalSection(&s_cs);

        s_listen = socket(AF_INET, SOCK_STREAM, 0);
        if (s_listen == INVALID_SOCKET) {
            fprintf(stderr, "MapBaker serve: socket() failed (error %ld)\n", (long)WSAGetLastError());
            return false;
        }
        int on = 1;
        setsockopt(s_listen, SOL_SOCKET, SO_REUSEADDR, (const char*)&on, sizeof(on));
        SOCKADDR_IN addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons((u_short)config.port);
        if (bind(s_listen, (SOCKADDR*)&addr, sizeof(addr)) == SOCKET_ERROR ||
            listen(s_listen, 16) == SOCKET_ERROR) {
            fprintf(stderr, "MapBaker serve: bind 127.0.0.1:%d failed (error %ld)\n", config.port,
                    (long)WSAGetLastError());
            closesocket(s_listen);
            s_listen = INVALID_SOCKET;
            return false;
        }
        fprintf(stderr, "MapBaker serve: listening on 127.0.0.1:%d\n", config.port);

        s_run = true;
        s_thread = std::thread(ServerThread);
        fprintf(stderr, "MapBaker serve: server thread started\n");
        EventRegister(EVENT_ID_IDLE, ServeIdleCallback);
        fprintf(stderr, "MapBaker serve: idle callback registered, initial job queued (%d tiles)\n",
                (int)s_tiles.size());

        fprintf(stderr, "MapBaker: serve mode: open http://127.0.0.1:%d  (out: %s, %dx%d tiles %dx%d)\n",
                config.port, s_outDir.c_str(), s_w, s_h, (int)s_tiles.size(), s_w * s_h);
        return true;
    }

    void Stop() {
        if (s_dumpFile) {
            fclose(s_dumpFile);
            s_dumpFile = nullptr;
        }
        s_run = false;
        if (s_listen != INVALID_SOCKET) {
            shutdown(s_listen, SD_BOTH);
            closesocket(s_listen);
            s_listen = INVALID_SOCKET;
        }
        if (s_thread.joinable()) {
            s_thread.join();
        }
        DeleteCriticalSection(&s_cs);
    }

}  // namespace MapBakerServe
