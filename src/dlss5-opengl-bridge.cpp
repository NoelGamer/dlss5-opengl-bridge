// dlss5-opengl-bridge -- a ReShade add-on that lets a DLSS 5 Neural Rendering
// add-on, which only hooks DirectX 12, run inside a game that renders with
// OpenGL.
//
// The DLSS 5 DX11 Bridge and its Vulkan port both work by mirroring: the game
// already drives DLSS through NGX on its own API, that evaluate is intercepted
// and forwarded untouched, and the same contract is reproduced on a private
// D3D12 NGX session -- the call the add-on detours.
//
// OpenGL has no such call to mirror. NGX exists for D3D11, D3D12 and Vulkan;
// NVIDIA never shipped an OpenGL interface, so no OpenGL game has ever made an
// NGX call and there is no parameter block to read. This bridge therefore builds
// the DLSS contract itself, out of what an OpenGL frame can be made to give up:
// the scene colour and depth at the moment the scene is finished, and motion
// vectors reprojected from that depth and the frame-to-frame view-projection.
//
// Everything downstream of that contract is the original's, near enough
// verbatim: the private D3D12 session, the NGX version negotiation, the snippet
// search paths, the create-failure repairs. That half was paid for by other
// people's bug reports and there is no reason to rediscover it.
//
//   dlss5-dx11-bridge (c) 2026 NIGos, MIT
//   dlss5-vk-bridge   (c) 2026 Alan Z., MIT
//   this OpenGL bridge, MIT, preserving both copyrights.
//
// Building, from the src folder:
//
//   rc /nologo version.rc
//   cl /nologo /LD /EHsc /O2 /MT dlss5-opengl-bridge.cpp ^
//      /link /OUT:dlss5-opengl-bridge.addon64 version.res ^
//      kernel32.lib user32.lib gdi32.lib

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "ngx.h"
#include "gl.h"

// Kept in step with version.rc, which is where ReShade's overlay reads it from.
#define BRIDGE_VERSION "1.0.5"

// D3D12 / NGX function typedefs, declared before bridge.h because the Bridge
// struct holds these pointers.
typedef NVSDK_NGX_Result (*PFN_NGX_D3D12_Init_Ext)(
    unsigned long long, const wchar_t *, ID3D12Device *, int, const void *);
typedef NVSDK_NGX_Result (*PFN_NGX_D3D12_Init_ProjectID)(
    const char *, int, const char *, const wchar_t *, ID3D12Device *, int, const void *);
typedef NVSDK_NGX_Result (*PFN_AllocateParameters)(NVSDK_NGX_Parameter **);
typedef NVSDK_NGX_Result (*PFN_D3D12CreateFeature)(
    ID3D12GraphicsCommandList *, int, NVSDK_NGX_Parameter *, NVSDK_NGX_Handle **);
typedef NVSDK_NGX_Result (*PFN_D3D12EvaluateFeature)(
    ID3D12GraphicsCommandList *, const NVSDK_NGX_Handle *, const NVSDK_NGX_Parameter *,
    PFN_NVSDK_NGX_ProgressCallback);
typedef NVSDK_NGX_Result (*PFN_D3D12ReleaseFeature)(NVSDK_NGX_Handle *);

#include "bridge.h"

// ---------------------------------------------------------------------------
// module-wide state
// ---------------------------------------------------------------------------
static HMODULE          g_self;
static CRITICAL_SECTION g_log_cs;
static wchar_t          g_log_path[MAX_PATH];
static bool             g_log_ready;

typedef void (*PFN_ReShadeLogMessage)(HMODULE, int, const char *);
static PFN_ReShadeLogMessage g_reshade_log;
static HMODULE               g_reshade_module;

// ---------------------------------------------------------------------------
// configuration (dlss5-opengl-bridge.cfg, next to the add-on)
// ---------------------------------------------------------------------------
struct BridgeCfg
{
    int  mode;            // 0 bridge normally, 1 no D3D12 evaluate (A/B test),
                          // 2 transport self-test, 3 fully inert
    int  source;          // 0 back buffer, 1 find the scene target, >=2 that name
    int  mv;              // 0 auto, 1 legacy matrix stack, 2 uniforms, 3 none
    char mv_vp[64];       // name of a combined view-projection uniform
    char mv_view[64];     // or a view/modelview uniform ...
    char mv_proj[64];     // ... and a projection uniform
    int  flags;           // -1 decide from the source, else literal create flags
    int  hdr;             // -1 decide from the source format, 0/1 force
    int  depth_inverted;  // -1 no (OpenGL default), 1 for a reverse-Z game
    int  subrects;
    int  reset_every;     // 1 discards temporal history every frame; diagnostic
    int  verbose;
};

static BridgeCfg g_cfg = { 0, 0, 0, "", "", "", -1, -1, -1, 1, 0, 0 };

// ---------------------------------------------------------------------------
// logging (8 MB cap, matching both other bridges)
// ---------------------------------------------------------------------------
static void LogPath()
{
    GetModuleFileNameW(g_self, g_log_path, MAX_PATH);
    if (wchar_t *s = wcsrchr(g_log_path, L'\\'))
        wcscpy_s(s + 1, MAX_PATH - (s + 1 - g_log_path), L"dlss5-opengl-bridge.log");
    g_log_ready = true;
}

static void LogV(const char *tag, const char *fmt, va_list ap)
{
    if (!g_log_ready) return;
    EnterCriticalSection(&g_log_cs);

    FILE *f = nullptr;
    if (_wfopen_s(&f, g_log_path, L"a") == 0 && f)
    {
        // "a" reports position 0 until the first write; seek to learn the size.
        _fseeki64(f, 0, SEEK_END);
        long long here = _ftelli64(f);
        if (here > 8 * 1024 * 1024)
        {
            fclose(f); f = nullptr;
            if (_wfopen_s(&f, g_log_path, L"w") != 0) f = nullptr;
            here = 0;
        }
        if (f && here == 0) fputs("dlss5-opengl-bridge log\n", f);
    }
    if (f)
    {
        SYSTEMTIME t; GetLocalTime(&t);
        fprintf(f, "[%02d:%02d:%02d.%03d]%s ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, tag);
        vfprintf(f, fmt, ap);
        fputc('\n', f);
        fclose(f);
    }
    LeaveCriticalSection(&g_log_cs);
}

static void Log(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt); LogV("", fmt, ap); va_end(ap);
}

// ReShade unregisters this add-on when it cycles its list, and the module is
// pinned so it outlives that -- which means ReShadeLogMessage can be called with
// a module handle ReShade no longer knows. That is its business to handle, but
// this add-on exists to keep a game running, so it is not going to find out the
// hard way inside somebody's race.
static void ReShadeLogGuarded(const char *tagged)
{
    __try { g_reshade_log(g_reshade_module, 1 /* error */, tagged); }
    __except (EXCEPTION_EXECUTE_HANDLER) { g_reshade_log = nullptr; }
}

// Anything that means "your setup is wrong" also goes into ReShade's own log,
// where its overlay shows it. People reliably post ReShade.log rather than this
// one, so the message has to be in both.
static void Warn(const char *fmt, ...)
{
    char msg[1024];
    va_list ap; va_start(ap, fmt);
    _vsnprintf_s(msg, _TRUNCATE, fmt, ap);
    va_end(ap);

    Log("WARN: %s", msg);

    if (g_reshade_log != nullptr)
    {
        char tagged[1100];
        _snprintf_s(tagged, _TRUNCATE, "[dlss5-opengl-bridge] %s", msg);
        ReShadeLogGuarded(tagged);
    }
}

// Names the bits rather than printing a number, because a create-flag report
// that says 0x41 makes the reader go and find nvsdk_ngx_defs.h.
static void LogCreateFlags(const char *what, unsigned int f)
{
    char line[256] = "";
    struct { unsigned bit; const char *name; } k[] = {
        { NGX_DLSS_FLAG_IS_HDR,         "IsHDR" },
        { NGX_DLSS_FLAG_MV_LOW_RES,     "MVLowRes" },
        { NGX_DLSS_FLAG_MV_JITTERED,    "MVJittered" },
        { NGX_DLSS_FLAG_DEPTH_INVERTED, "DepthInverted" },
        { NGX_DLSS_FLAG_DO_SHARPENING,  "DoSharpening" },
        { NGX_DLSS_FLAG_AUTO_EXPOSURE,  "AutoExposure" },
    };
    for (const auto &e : k)
        if (f & e.bit) { if (line[0]) strcat_s(line, " | "); strcat_s(line, e.name); }
    Log("[bridge] %s 0x%X = %s", what, f, line[0] ? line : "none");
}

// ---------------------------------------------------------------------------
// timing
//
// The point is to separate two very different costs: work this add-on does on
// the CPU, and the pipeline bubble the cross-API fences create. A small CPU
// figure next to a long frame interval means the cost is the synchronisation
// rather than anything the bridge computes.
// ---------------------------------------------------------------------------
static void LogVideoMemory(const char *when);   // d3d12_session.inc

static void TimingTick(LONGLONG entry, LONGLONG exit)
{
    if (g_bridge.qpf == 0)
    {
        LARGE_INTEGER f; QueryPerformanceFrequency(&f);
        g_bridge.qpf = f.QuadPart;
        g_bridge.span_start = entry;
    }
    g_bridge.cpu_ticks += (exit - entry);
    ++g_bridge.timed_frames;

    if (g_bridge.timed_frames < 600) return;

    const double span_ms = (double)(exit - g_bridge.span_start) * 1000.0 / (double)g_bridge.qpf;
    const double cpu_ms  = (double)g_bridge.cpu_ticks * 1000.0 / (double)g_bridge.qpf;
    const double per     = span_ms / (double)g_bridge.timed_frames;
    const double lo      = (double)g_bridge.iv_min * 1000.0 / (double)g_bridge.qpf;
    const double hi      = (double)g_bridge.iv_max * 1000.0 / (double)g_bridge.qpf;

    Log("[bridge] %llu frames: bridge CPU %.2f ms/frame | frame interval %.2f ms (%.1f fps) "
        "| spread %.2f-%.2f ms | bridge is %.0f%% of the frame",
        (unsigned long long)g_bridge.timed_frames,
        cpu_ms / (double)g_bridge.timed_frames, per, per > 0.0 ? 1000.0 / per : 0.0,
        lo, hi, span_ms > 0.0 ? (cpu_ms / span_ms) * 100.0 : 0.0);

    LogVideoMemory("at heartbeat");

    g_bridge.cpu_ticks = 0;
    g_bridge.timed_frames = 0;
    g_bridge.span_start = exit;
    g_bridge.iv_min = g_bridge.iv_max = 0;
}

// ---------------------------------------------------------------------------
// config file
//
// Re-read while the game runs, rate limited on the clock rather than on frames
// so a stalled game does not stop it being reloadable. Changes that only take
// effect on a new NGX feature trigger a rebuild by way of ContractChanged.
// ---------------------------------------------------------------------------
static void CfgPath(wchar_t *out)
{
    GetModuleFileNameW(g_self, out, MAX_PATH);
    if (wchar_t *s = wcsrchr(out, L'\\'))
        wcscpy_s(s + 1, MAX_PATH - (s + 1 - out), L"dlss5-opengl-bridge.cfg");
}

static void CfgWriteDefault()
{
    wchar_t path[MAX_PATH];
    CfgPath(path);
    FILE *f = nullptr;
    if (_wfopen_s(&f, path, L"w") != 0 || f == nullptr) return;
    fputs(
        "# dlss5-opengl-bridge configuration. Re-read while the game runs.\n"
        "#\n"
        "# mode      0 bridge normally -- the DLSS output goes back to the game\n"
        "#           1 everything except the NGX evaluate: the full cross-API round\n"
        "#             trip, then the shared Color is copied back. The picture\n"
        "#             should look exactly like the game's own. If it does, the\n"
        "#             transport is faithful and only the neural pass is left.\n"
        "#           2 the same, without touching D3D12 at all. Proves the OpenGL\n"
        "#             half on its own.\n"
        "#           3 fully inert; the add-on loads, logs, and writes nothing\n"
        "#\n"
        "# source    0 the back buffer, immediately before the swap. Always works,\n"
        "#             and puts the interface through the neural pass with the\n"
        "#             scene, which is visible on text.\n"
        "#           1 find the scene target: the largest framebuffer drawn into\n"
        "#             this frame that has depth. Keeps the interface out of it,\n"
        "#             when the game renders the world offscreen.\n"
        "#           n that exact framebuffer name. The log lists the candidates.\n"
        "#\n"
        "# mv        where the view-projection matrix comes from, for motion\n"
        "#           vectors. 0 auto, 1 the fixed-function matrix stack,\n"
        "#           2 the uniforms named below, 3 none (zero motion vectors:\n"
        "#           a stable picture that smears when the camera moves).\n"
        "# mv_vp     name of a combined view-projection matrix uniform, or\n"
        "# mv_view   name of a view / modelview matrix uniform, together with\n"
        "# mv_proj   name of a projection matrix uniform.\n"
        "#           These are the single most useful thing to set for a\n"
        "#           shader-based game. Find them in the game's shader source or\n"
        "#           its shader mod.\n"
        "#\n"
        "# flags    -1 decide the DLSS create flags from the source buffers\n"
        "#             (recommended), or a literal value to force them\n"
        "# hdr      -1 decide IsHDR from whether the source is a float target\n"
        "# depth_inverted  -1 no, which is the OpenGL default. Set 1 only for a\n"
        "#             game using glClipControl for reverse-Z depth.\n"
        "# subrects  1 pass DLSS.Enable.Output.Subrects to the feature\n"
        "# reset_every  1 discards temporal history every frame; diagnostic only\n"
        "# verbose   1 extra per-frame logging\n"
        "\n"
        "mode = 0\n"
        "source = 0\n"
        "mv = 0\n"
        "mv_vp =\n"
        "mv_view =\n"
        "mv_proj =\n"
        "flags = -1\n"
        "hdr = -1\n"
        "depth_inverted = -1\n"
        "subrects = 1\n"
        "reset_every = 0\n"
        "verbose = 0\n", f);
    fclose(f);
}

static void CfgTrim(char *s)
{
    char *end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n')) --end;
    *end = '\0';
}

static void CfgReload()
{
    static ULONGLONG next_check;
    const ULONGLONG now = GetTickCount64();
    if (now < next_check) return;
    next_check = now + 1000;

    wchar_t path[MAX_PATH];
    CfgPath(path);

    FILE *f = nullptr;
    if (_wfopen_s(&f, path, L"r") != 0 || f == nullptr) { CfgWriteDefault(); return; }

    BridgeCfg c = g_cfg;
    char line[256];
    while (fgets(line, sizeof(line), f))
    {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;
        char key[64] = "", val[128] = "";
        if (sscanf_s(line, " %63[^= ] = %127[^\n]", key, (unsigned)sizeof(key),
                     val, (unsigned)sizeof(val)) < 1) continue;
        CfgTrim(key); CfgTrim(val);

        // The three name keys are legitimately empty -- that is how a matrix
        // uniform is turned off again. The numbers are not: an empty value there
        // is a half-typed edit caught mid-save, and reading it as atoi("") == 0
        // would silently apply a setting nobody asked for.
        if      (_stricmp(key, "mv_vp")   == 0) { strcpy_s(c.mv_vp,   val); continue; }
        else if (_stricmp(key, "mv_view") == 0) { strcpy_s(c.mv_view, val); continue; }
        else if (_stricmp(key, "mv_proj") == 0) { strcpy_s(c.mv_proj, val); continue; }
        if (val[0] == '\0') continue;

        if      (_stricmp(key, "mode")           == 0) c.mode           = atoi(val);
        else if (_stricmp(key, "source")         == 0) c.source         = atoi(val);
        else if (_stricmp(key, "mv")             == 0) c.mv             = atoi(val);
        else if (_stricmp(key, "flags")          == 0) c.flags          = atoi(val);
        else if (_stricmp(key, "hdr")            == 0) c.hdr            = atoi(val);
        else if (_stricmp(key, "depth_inverted") == 0) c.depth_inverted = atoi(val);
        else if (_stricmp(key, "subrects")       == 0) c.subrects       = atoi(val);
        else if (_stricmp(key, "reset_every")    == 0) c.reset_every    = atoi(val);
        else if (_stricmp(key, "verbose")        == 0) c.verbose        = atoi(val);
    }
    fclose(f);

    if (memcmp(&c, &g_cfg, sizeof(c)) != 0)
    {
        g_cfg = c;
        Log("[cfg] mode=%d source=%d mv=%d flags=%d hdr=%d depth_inverted=%d subrects=%d "
            "reset_every=%d verbose=%d mv_vp='%s' mv_view='%s' mv_proj='%s'",
            c.mode, c.source, c.mv, c.flags, c.hdr, c.depth_inverted, c.subrects,
            c.reset_every, c.verbose, c.mv_vp, c.mv_view, c.mv_proj);
    }
}

// ---------------------------------------------------------------------------
// the four halves, in dependency order
// ---------------------------------------------------------------------------
#include "d3d12_session.inc"
#include "gl_interop.inc"
#include "motion.inc"
#include "gl_capture.inc"
#include "gl_bridge.inc"

// ---------------------------------------------------------------------------
// environment report
//
// Written once, to answer the usual questions without a conversation. The DX11
// bridge's report exists because the most common cause of "it does nothing" is a
// missing model DLL or no DLSS 5 add-on at all, and that is no less true here --
// more so, because an OpenGL game brings none of the NGX machinery with it and
// every piece has to come from the add-on beside us.
// ---------------------------------------------------------------------------
static void LogEnvironment()
{
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(GetModuleHandleW(nullptr), exe, MAX_PATH);
    Log("");
    Log("################ dlss5-opengl-bridge " BRIDGE_VERSION " ################");
    Log("built " __DATE__ " " __TIME__);
    Log("host: %ls", exe);

    typedef LONG (WINAPI *PFN_RtlGetVersion)(void *);
    if (HMODULE nt = GetModuleHandleW(L"ntdll.dll"))
        if (auto rtl = reinterpret_cast<PFN_RtlGetVersion>(GetProcAddress(nt, "RtlGetVersion")))
        {
            struct { ULONG size, major, minor, build, platform; WCHAR csd[128]; } v = {};
            v.size = sizeof(v);
            if (rtl(&v) == 0) Log("windows %lu.%lu build %lu", v.major, v.minor, v.build);
        }

    wchar_t dir[MAX_PATH] = {};
    GetModuleFileNameW(g_self, dir, MAX_PATH);
    if (wchar_t *s = wcsrchr(dir, L'\\')) *(s + 1) = L'\0';
    Log("add-on directory: %ls", dir);

    // What is actually next to us. A missing nvngx_dlssnr.dll or no *.addon file
    // at all is the single most common reason nothing happens, and it is
    // answerable from here rather than by asking.
    static const wchar_t *const patterns[] = { L"*nvngx*.dll", L"*.addon*" };
    for (const wchar_t *pat : patterns)
    {
        wchar_t glob[MAX_PATH];
        wcscpy_s(glob, dir);
        wcscat_s(glob, pat);
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW(glob, &fd);
        if (h == INVALID_HANDLE_VALUE) { Log("  %ls: none present", pat); continue; }
        do { Log("  %ls (%llu bytes)", fd.cFileName,
                 ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow); }
        while (FindNextFileW(h, &fd));
        FindClose(h);
    }

    // Which modules in the process carry an NGX surface, and on which API. In
    // the DX11 and Vulkan bridges this list is how a report shows which layer
    // was hooked; here it shows something different and more important, which is
    // whether a DLSS 5 add-on is present at all -- because nothing else in an
    // OpenGL process has any reason to load NGX.
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    auto enum_modules = reinterpret_cast<BOOL (WINAPI *)(HANDLE, HMODULE *, DWORD, LPDWORD)>(
        GetProcAddress(k32, "K32EnumProcessModules"));
    HMODULE mods[1024]; DWORD needed = 0;
    int ngx_modules = 0;
    if (enum_modules && enum_modules(GetCurrentProcess(), mods, sizeof(mods), &needed))
        for (DWORD i = 0; i < needed / sizeof(HMODULE); ++i)
        {
            const bool d3d11 = GetProcAddress(mods[i], "NVSDK_NGX_D3D11_CreateFeature") != nullptr;
            const bool d3d12 = GetProcAddress(mods[i], "NVSDK_NGX_D3D12_CreateFeature") != nullptr;
            const bool vk    = GetProcAddress(mods[i], "NVSDK_NGX_VULKAN_CreateFeature") != nullptr;
            if (!d3d11 && !d3d12 && !vk) continue;
            wchar_t name[MAX_PATH] = {};
            GetModuleFileNameW(mods[i], name, MAX_PATH);
            Log("  NGX module: %ls%s%s%s", name, d3d11 ? " D3D11" : "", d3d12 ? " D3D12" : "",
                vk ? " VULKAN" : "");
            ++ngx_modules;
        }
    if (ngx_modules == 0)
        Log("  no NGX module is loaded yet. That is normal in an OpenGL game: nothing "
            "here loads NGX on its own, so the dispatcher is asked for by name when the "
            "D3D12 session opens.");
    Log("############################################################");
}

// ---------------------------------------------------------------------------
// ReShade add-on registration
//
// Replicates what reshade.hpp's register_addon does, without the SDK: locate the
// module exporting ReShadeRegisterAddon and call it. The API version is
// negotiated downwards because ReShade rejects one newer than its own.
//
// No ReShade event is subscribed to. Being a ReShade add-on is how this file
// gets into the process and how it knows a DLSS 5 add-on is there to detour the
// evaluate; all the graphics work goes through OpenGL directly, which keeps the
// dependency to three exported functions whose signatures have not moved.
// ---------------------------------------------------------------------------
typedef bool (*PFN_ReShadeRegisterAddon)(HMODULE, uint32_t);
typedef void (*PFN_ReShadeUnregisterAddon)(HMODULE);
static PFN_ReShadeUnregisterAddon g_unregister;

// ---------------------------------------------------------------------------
// Pinning
//
// ReShade does not load an add-on once. It loads them, unloads them and loads
// them again around every runtime it builds -- MX Bikes produces four full
// load/unload cycles before its real context exists, because the engine makes a
// dummy context first and ReShade rebuilds its add-on list for each one.
//
// An add-on that only reads events survives that. One that installs hooks does
// not, and the failure is not subtle:
//
//   * the watch thread created in DllMain is sleeping inside this module when
//     FreeLibrary unmaps it, and wakes up executing unmapped memory;
//   * every function pointer handed out through wglGetProcAddress -- which the
//     game has already cached -- becomes a jump into the same hole;
//   * any thread still inside a detour when the unload lands is in the same
//     position.
//
// Each of those is an access violation in the game's own call stack, where this
// add-on's exception handler cannot see it, a few seconds after launch. Pinning
// is the fix for all three at once: the module's reference count is raised so
// FreeLibrary can never unmap it, and every pointer and patch stays valid for
// the life of the process.
//
// The cost is that ReShade's later reload finds a module whose DllMain does not
// run again, so the add-on stops appearing in ReShade's registered list. Nothing
// here depends on being registered -- no ReShade event is subscribed to, and the
// hooks are already installed -- so that is a fair price for not taking the game
// down.
static void PinSelf()
{
    HMODULE pinned = nullptr;
    const BOOL ok = GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
        reinterpret_cast<LPCWSTR>(&PinSelf), &pinned);
    if (ok)
        Log("[bridge] module pinned: ReShade unloads and reloads its add-ons several "
            "times during startup, and the hooks, the watch thread and anything handed "
            "out to the game all have to outlive that.");
    else
        Warn("this add-on could not pin itself in memory (0x%08X). ReShade unloads "
             "add-ons during startup, and hooks installed by a module that then goes "
             "away will crash the game. Stopping instead.", GetLastError());
    if (!ok) BridgeDisable("the module could not be pinned");
}

static bool RegisterWithReShade(HMODULE self)
{
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    if (k32 == nullptr) return false;
    auto enum_modules = reinterpret_cast<BOOL (WINAPI *)(HANDLE, HMODULE *, DWORD, LPDWORD)>(
        GetProcAddress(k32, "K32EnumProcessModules"));
    if (enum_modules == nullptr) return false;

    HMODULE mods[1024];
    DWORD   needed = 0;
    if (!enum_modules(GetCurrentProcess(), mods, sizeof(mods), &needed)) return false;

    const DWORD count = needed / sizeof(HMODULE);
    for (DWORD i = 0; i < count; ++i)
    {
        auto reg = reinterpret_cast<PFN_ReShadeRegisterAddon>(
            GetProcAddress(mods[i], "ReShadeRegisterAddon"));
        if (reg == nullptr) continue;

        for (uint32_t version = 18; version >= 5; --version)
            if (reg(self, version))
            {
                g_unregister = reinterpret_cast<PFN_ReShadeUnregisterAddon>(
                    GetProcAddress(mods[i], "ReShadeUnregisterAddon"));
                g_reshade_log = reinterpret_cast<PFN_ReShadeLogMessage>(
                    GetProcAddress(mods[i], "ReShadeLogMessage"));
                g_reshade_module = self;
                Log("[bridge] registered with ReShade (add-on API %u)", version);
                return true;
            }
    }
    return false;
}

// ---------------------------------------------------------------------------
// startup watch
//
// opengl32.dll may not be loaded when ReShade brings this add-on up, and in an
// OpenGL game ReShade itself is usually the opengl32.dll -- so it is, but a
// game that creates its context late, or a launcher process that never creates
// one at all, both have to be survivable. Scan eagerly for the first minute,
// then keep a slow watch.
//
// The same thread says something once if nothing ever called a swap through us,
// because "hooked but never called" is a different problem from "never hooked"
// and a log that cannot tell them apart cannot be acted on.
// ---------------------------------------------------------------------------
static DWORD WINAPI StartupWatch(void *)
{
    for (int i = 0; i < 240 && !g_bridge.disabled; ++i)
    {
        if (InstallGlHooks()) break;
        Sleep(250);
    }
    if (g_cfg.mode == 3 || g_bridge.disabled) return 0;
    if (!g_hook_swap.installed && !g_hook_gdi_swap.installed)
    {
        Warn("no OpenGL swap entry point could be hooked in 60 seconds. If this process "
             "is a launcher rather than the game, that is expected. If it is the game, "
             "it is not rendering with OpenGL and this add-on has nothing to do.");
        return 0;
    }

    for (int i = 0; i < 240 && !g_bridge.disabled; ++i)
    {
        if (g_bridge.frames_done > 0) return 0;
        Sleep(250);
    }
    if (g_bridge.frames_done == 0 && !g_bridge.disabled)
        Log("[bridge] the swap entry points were hooked but no frame has been bridged in "
            "60 seconds. If the game is rendering, look above for the reason the OpenGL "
            "side or the D3D12 session did not come up.");
    return 0;
}

// ---------------------------------------------------------------------------
BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, void *)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_self = inst;
        DisableThreadLibraryCalls(inst);
        InitializeCriticalSection(&g_log_cs);
        InitializeCriticalSection(&g_hook_cs);
        LogPath();

        // Registration first, and nothing installed before it: if ReShade is not
        // here this returns FALSE, the loader unmaps us again, and there must be
        // no hook, no thread and no pin left behind when it does.
        if (!RegisterWithReShade(inst))
            return FALSE;

        // Then pin, before anything that outlives a single load exists. See
        // PinSelf: everything below this line would be a dangling pointer the
        // moment ReShade cycles its add-on list, which it does four times before
        // this game's real context is created.
        PinSelf();
        if (g_bridge.disabled) return TRUE;   // pinned failed; stay inert but loaded

        CfgReload();
        LogEnvironment();
        InstallGlHooks();
        CreateThread(nullptr, 0, StartupWatch, nullptr, 0, nullptr);
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        // Put the patched entry points back before the process tears down; a
        // jump into a module that has been unmapped is a crash on the way out
        // that gets blamed on the game.
        HookRestore(&g_hook_swap);
        HookRestore(&g_hook_gdi_swap);
        HookRestore(&g_hook_getproc);
        HookRestore(&g_hook_draw_arrays);
        HookRestore(&g_hook_draw_elements);
        HookRestore(&g_hook_begin);
        if (g_unregister) g_unregister(g_self);
    }
    return TRUE;
}
