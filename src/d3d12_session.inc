// The private D3D12 NGX session.
//
// This half is almost unchanged from the Vulkan port, which took it almost
// unchanged from the DLSS 5 DX11 Bridge: the D3D12/NGX API is the same whichever
// graphics API the game uses, and it is the call a DLSS 5 add-on detours. All
// the hard-won behaviour here -- the version negotiation, the snippet search
// paths, the create-failure repairs -- is NIGos' work and is kept verbatim
// wherever it can be, because every line of it was paid for by a report.
//
// What differs at the edges: the D3D12 device is chosen by LUID so it sits on
// the same physical GPU as the game's OpenGL context (cross-API shared handles
// require it), the shared textures export NT handles for GL to import, and the
// allocation size is recorded because glImportMemoryWin32HandleEXT asks for it
// where Vulkan reads it back from the image.

// ---- SEH-guarded NGX calls ------------------------------------------------
// Undocumented exports reached with signatures recovered from the D3D11 bridge;
// a wrong guess must land in the log, not take the game down. Each guard is its
// own function with no C++ objects so __try is legal.
#define NGX_EXCEPTION_MARKER 0x7FFFFFFF

static NVSDK_NGX_Result SafeInitExt(PFN_NGX_D3D12_Init_Ext fn, unsigned long long app,
                                    const wchar_t *path, ID3D12Device *dev, int ver,
                                    const void *info, DWORD *code)
{
    *code = 0;
    __try { return fn(app, path, dev, ver, info); }
    __except (EXCEPTION_EXECUTE_HANDLER) { *code = GetExceptionCode(); return NGX_EXCEPTION_MARKER; }
}

static NVSDK_NGX_Result SafeInitProjectID(PFN_NGX_D3D12_Init_ProjectID fn, const char *proj,
                                          const wchar_t *path, ID3D12Device *dev, int ver,
                                          const void *info, DWORD *code)
{
    *code = 0;
    __try { return fn(proj, 0 /*ENGINE_CUSTOM*/, "1.0", path, dev, ver, info); }
    __except (EXCEPTION_EXECUTE_HANDLER) { *code = GetExceptionCode(); return NGX_EXCEPTION_MARKER; }
}

static NVSDK_NGX_Result SafeAllocParams(PFN_AllocateParameters fn, NVSDK_NGX_Parameter **out, DWORD *code)
{
    *code = 0;
    __try { return fn(out); }
    __except (EXCEPTION_EXECUTE_HANDLER) { *code = GetExceptionCode(); return NGX_EXCEPTION_MARKER; }
}

static NVSDK_NGX_Result SafeCreateFeature(PFN_D3D12CreateFeature fn, ID3D12GraphicsCommandList *list,
                                          NVSDK_NGX_Parameter *p, NVSDK_NGX_Handle **out, DWORD *code)
{
    *code = 0;
    __try { return fn(list, 1 /*SuperSampling*/, p, out); }
    __except (EXCEPTION_EXECUTE_HANDLER) { *code = GetExceptionCode(); return NGX_EXCEPTION_MARKER; }
}

static NVSDK_NGX_Result SafeEvaluate(PFN_D3D12EvaluateFeature fn, ID3D12GraphicsCommandList *list,
                                     const NVSDK_NGX_Handle *feature, const NVSDK_NGX_Parameter *p,
                                     DWORD *code)
{
    *code = 0;
    __try { return fn(list, feature, p, nullptr); }
    __except (EXCEPTION_EXECUTE_HANDLER) { *code = GetExceptionCode(); return NGX_EXCEPTION_MARKER; }
}

// NGX answers in hex; names taken from nvsdk_ngx_defs.h so a report does not
// need the header to be read.
static const char *NgxResultName(NVSDK_NGX_Result r)
{
    if (r == NGX_SUCCESS) return "Success";
    if ((static_cast<unsigned>(r) & 0xFFF00000u) != 0xBAD00000u) return "not an NGX result";
    static const char *k[] = {
        "Fail", "FeatureNotSupported", "PlatformError", "FeatureAlreadyExists",
        "FeatureNotFound", "InvalidParameter", "ScratchBufferTooSmall",
        "NotInitialized", "UnsupportedInputFormat", "RWFlagMissing", "MissingInput",
        "UnableToInitializeFeature", "OutOfDate", "OutOfGPUMemory",
        "UnsupportedFormat", "UnableToWriteToAppDataPath", "UnsupportedParameter",
        "Denied", "NotImplemented",
    };
    const unsigned n = static_cast<unsigned>(r) & 0xFFFFFu;
    return n < _countof(k) ? k[n] : "unknown";
}

// ---- NGX snippet search paths ---------------------------------------------
// The dispatcher looks for feature snippets (nvngx_dlss.dll & co.) in the
// session's data path and the OTA cache keyed by its app id. A private session
// under a made-up app id finds neither -- CreateFeature then fails with
// FeatureNotFound. NGX's official escape is the FeatureCommonInfo PathListInfo
// handed to Init: point it at the directories of the NGX modules already in the
// process, which is exactly where the working snippets live.
//
// An OpenGL game loads no NGX module of its own, so unlike the other two
// bridges the only directories that will be found here are the ones the DLSS 5
// add-on brought with it and the game folder. That is enough -- the add-on ships
// nvngx_dlssnr.dll and sits next to ReShade -- but it is why the game directory
// is added unconditionally rather than as an afterthought.
struct NgxPathListInfo { const wchar_t *const *Path; unsigned int Length; };
struct NgxLoggingInfo  { void *Callback; int MinimumLoggingLevel; bool DisableOtherLoggingSinks; };
struct NgxFeatureCommonInfo
{
    NgxPathListInfo PathListInfo;
    void           *InternalData;
    NgxLoggingInfo  LoggingInfo;
};

static HMODULE g_ngx12_module;              // module the D3D12 entry points came from
static NgxFeatureCommonInfo g_ngx_common;   // static: NGX may keep the pointers
static wchar_t        g_ngx_paths[8][MAX_PATH];
static const wchar_t *g_ngx_path_ptrs[8];

static void NgxAddModuleDir(HMODULE m, unsigned int *n)
{
    wchar_t buf[MAX_PATH];
    if (m == nullptr || *n >= 8 || GetModuleFileNameW(m, buf, MAX_PATH) == 0) return;
    wchar_t *s = wcsrchr(buf, L'\\');
    if (s == nullptr) return;
    *s = L'\0';
    for (unsigned int i = 0; i < *n; ++i) if (_wcsicmp(g_ngx_paths[i], buf) == 0) return;
    wcscpy_s(g_ngx_paths[*n], buf);
    g_ngx_path_ptrs[*n] = g_ngx_paths[*n];
    ++*n;
}

static const NgxFeatureCommonInfo *NgxBuildCommonInfo()
{
    unsigned int n = 0;
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    auto enum_modules = reinterpret_cast<BOOL (WINAPI *)(HANDLE, HMODULE *, DWORD, LPDWORD)>(
        GetProcAddress(k32, "K32EnumProcessModules"));
    HMODULE mods[1024]; DWORD needed = 0;
    if (enum_modules && enum_modules(GetCurrentProcess(), mods, sizeof(mods), &needed))
        for (DWORD i = 0; i < needed / sizeof(HMODULE); ++i)
            if (GetProcAddress(mods[i], "NVSDK_NGX_D3D12_EvaluateFeature") ||
                GetProcAddress(mods[i], "NVSDK_NGX_D3D11_EvaluateFeature") ||
                GetProcAddress(mods[i], "NVSDK_NGX_VULKAN_EvaluateFeature"))
                NgxAddModuleDir(mods[i], &n);
    NgxAddModuleDir(g_self, &n);                      // next to this add-on
    NgxAddModuleDir(GetModuleHandleW(nullptr), &n);   // the game directory

    for (unsigned int i = 0; i < n; ++i) Log("[bridge] NGX search path: %ls", g_ngx_paths[i]);
    g_ngx_common.PathListInfo.Path   = g_ngx_path_ptrs;
    g_ngx_common.PathListInfo.Length = n;
    g_ngx_common.InternalData        = nullptr;
    g_ngx_common.LoggingInfo         = {};
    return &g_ngx_common;
}

// Logs the SuperSampling capability block; returns Available (-1 if unknowable).
static int D3D12QueryNgxCaps()
{
    if (g_ngx12_module == nullptr) return -1;
    auto get_caps = reinterpret_cast<PFN_AllocateParameters>(
        GetProcAddress(g_ngx12_module, "NVSDK_NGX_D3D12_GetCapabilityParameters"));
    if (get_caps == nullptr) { Log("[bridge] no GetCapabilityParameters export to probe"); return -1; }

    DWORD code = 0;
    NVSDK_NGX_Parameter *cap = nullptr;
    NVSDK_NGX_Result rc = SafeAllocParams(get_caps, &cap, &code);
    if (code || rc != NGX_SUCCESS || cap == nullptr)
    { Log("[bridge] GetCapabilityParameters gave nothing (result 0x%08X, code 0x%08X)", rc, code); return -1; }

    int avail = -1, init_res = 0, needs_drv = -1, maj = 0, minor = 0;
    cap->Get("SuperSampling.Available", &avail);
    cap->Get("SuperSampling.FeatureInitResult", &init_res);
    cap->Get("SuperSampling.NeedsUpdatedDriver", &needs_drv);
    cap->Get("SuperSampling.MinDriverVersionMajor", &maj);
    cap->Get("SuperSampling.MinDriverVersionMinor", &minor);
    Log("[bridge] NGX caps: SuperSampling.Available=%d FeatureInitResult=0x%08X "
        "NeedsUpdatedDriver=%d MinDriver=%d.%d", avail, init_res, needs_drv, maj, minor);

    typedef NVSDK_NGX_Result (*PFN_DestroyParameters)(NVSDK_NGX_Parameter *);
    auto destroy = reinterpret_cast<PFN_DestroyParameters>(
        GetProcAddress(g_ngx12_module, "NVSDK_NGX_D3D12_DestroyParameters"));
    if (destroy) destroy(cap);
    return avail;
}

// ---- memory reporting ------------------------------------------------------
// The bridge holds four shared textures the size of the screen, and the DLSS
// feature behind them holds a good deal more. At 5120x1440 that is not a rounding
// error, and a game that then fails to load its own content has a claim worth
// answering with numbers rather than with an opinion. Local video memory is what
// matters; the process working set is logged beside it so system memory can be
// ruled in or out at the same time.
static void LogVideoMemory(const char *when)
{
    if (g_bridge.adapter3 == nullptr) return;

    DXGI_QUERY_VIDEO_MEMORY_INFO local = {}, nonlocal = {};
    const bool have_local = SUCCEEDED(
        g_bridge.adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local));
    const bool have_nonlocal = SUCCEEDED(
        g_bridge.adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &nonlocal));

    const double mb = 1024.0 * 1024.0;
    if (have_local)
        Log("[mem] %s: video memory %.0f / %.0f MB used of budget%s%s", when,
            (double)local.CurrentUsage / mb, (double)local.Budget / mb,
            local.CurrentUsage > local.Budget ? "  -- OVER BUDGET, the driver is "
                                                "evicting to system memory" : "",
            have_nonlocal ? "" : "");
    if (have_nonlocal && nonlocal.CurrentUsage > 0)
        Log("[mem] %s: %.0f MB has spilled into system memory (budget %.0f MB)", when,
            (double)nonlocal.CurrentUsage / mb, (double)nonlocal.Budget / mb);

    typedef struct { DWORD cb; DWORD PageFaultCount; SIZE_T PeakWorkingSetSize, WorkingSetSize,
                     QuotaPeakPagedPoolUsage, QuotaPagedPoolUsage, QuotaPeakNonPagedPoolUsage,
                     QuotaNonPagedPoolUsage, PagefileUsage, PeakPagefileUsage; } PMC;
    typedef BOOL (WINAPI *PFN_GetProcessMemoryInfo)(HANDLE, PMC *, DWORD);
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    auto pmi = reinterpret_cast<PFN_GetProcessMemoryInfo>(
        GetProcAddress(k32, "K32GetProcessMemoryInfo"));
    if (pmi)
    {
        PMC pmc = {}; pmc.cb = sizeof(pmc);
        if (pmi(GetCurrentProcess(), &pmc, sizeof(pmc)))
            Log("[mem] %s: process working set %.0f MB (peak %.0f MB)", when,
                (double)pmc.WorkingSetSize / mb, (double)pmc.PeakWorkingSetSize / mb);
    }
}

static void BridgeReleaseResources();

static void BridgeDisable(const char *why)
{
    if (g_bridge.disabled) return;
    g_bridge.disabled = true;
    Warn("stopped: %s. The game renders normally. See dlss5-opengl-bridge.log for detail.", why);
    BridgeReleaseResources();
}

static void BridgeFail(const char *what)
{
    g_bridge.frame_ready = false;
    if (++g_bridge.consecutive_fails >= 3)
    {
        Log("[bridge] disabled after repeated failures (%s). Game rendering is untouched.", what);
        BridgeDisable(what);
    }
}

// ---- adapter selection by LUID --------------------------------------------
// The D3D12 device must be on the same physical GPU as the game's GL context,
// or the shared NT handles cannot be opened across the two APIs. The GL device
// LUID comes from GL_EXT_memory_object (gl_interop.inc); here it picks the DXGI
// adapter that matches.
static IDXGIAdapter1 *PickAdapterByLuid(const LUID &luid, bool luid_valid)
{
    HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
    auto create = dxgi ? reinterpret_cast<HRESULT (WINAPI *)(REFIID, void **)>(
                             GetProcAddress(dxgi, "CreateDXGIFactory1")) : nullptr;
    if (create == nullptr) return nullptr;

    IDXGIFactory1 *factory = nullptr;
    if (FAILED(create(__uuidof(IDXGIFactory1), reinterpret_cast<void **>(&factory))) || !factory)
        return nullptr;

    IDXGIAdapter1 *chosen = nullptr;
    IDXGIAdapter1 *ad = nullptr;
    for (UINT i = 0; factory->EnumAdapters1(i, &ad) == S_OK; ++i)
    {
        DXGI_ADAPTER_DESC1 d = {};
        ad->GetDesc1(&d);
        if (luid_valid && memcmp(&d.AdapterLuid, &luid, sizeof(LUID)) == 0)
        {
            if (chosen) chosen->Release();
            chosen = ad;   // exact GPU the game's GL context is on
            Log("[bridge] D3D12 adapter matched by LUID: %ls", d.Description);
            break;
        }
        if (chosen == nullptr && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
        {
            chosen = ad;   // first hardware adapter, as a fallback
            chosen->AddRef();
        }
        ad->Release();
    }
    if (chosen && !luid_valid)
        Log("[bridge] no GL device LUID; using the first hardware adapter. If the "
            "machine has more than one GPU this may be the wrong one.");
    factory->Release();
    return chosen;
}

// ---- command submission ----------------------------------------------------
static bool D3D12BeginCommands()
{
    const int slot = g_bridge.frame_slot;
    const UINT64 retire = g_bridge.alloc_fence[slot];
    if (retire != 0 && g_bridge.fence_gpu->GetCompletedValue() < retire)
    {
        g_bridge.fence_gpu->SetEventOnCompletion(retire, g_bridge.fence_event);
        if (WaitForSingleObject(g_bridge.fence_event, 2000) != WAIT_OBJECT_0 ||
            g_bridge.fence_gpu->GetCompletedValue() < retire)
        {
            Log("[bridge] the GPU did not retire allocator slot %d within 2 s.", slot);
            BridgeDisable("the GPU stopped completing work");
            return false;
        }
    }
    if (g_bridge.alloc[slot] == nullptr) return false;
    if (FAILED(g_bridge.alloc[slot]->Reset())) return false;
    return SUCCEEDED(g_bridge.list->Reset(g_bridge.alloc[slot], nullptr));
}

// Submits, records which fence value retires the allocator, advances the ring.
static UINT64 D3D12EndCommands()
{
    g_bridge.list->Close();
    ID3D12CommandList *lists[] = { g_bridge.list };
    g_bridge.queue->ExecuteCommandLists(1, lists);

    const UINT64 v = ++g_bridge.gpu_value;
    g_bridge.queue->Signal(g_bridge.fence_gpu, v);
    g_bridge.alloc_fence[g_bridge.frame_slot] = v;
    g_bridge.frame_slot = (g_bridge.frame_slot + 1) % Bridge::kFrames;
    return v;
}

static void D3D12DrainGpu()
{
    if (g_bridge.queue == nullptr || g_bridge.fence_gpu == nullptr) return;
    const UINT64 v = ++g_bridge.gpu_value;
    g_bridge.queue->Signal(g_bridge.fence_gpu, v);
    if (g_bridge.fence_gpu->GetCompletedValue() < v && g_bridge.fence_event)
    {
        g_bridge.fence_gpu->SetEventOnCompletion(v, g_bridge.fence_event);
        if (WaitForSingleObject(g_bridge.fence_event, 5000) != WAIT_OBJECT_0)
            Log("[bridge] timed out draining the D3D12 queue before teardown");
    }
    for (int i = 0; i < Bridge::kFrames; ++i) g_bridge.alloc_fence[i] = 0;
}

static void Barrier(ID3D12GraphicsCommandList *list, ID3D12Resource *res,
                    D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
{
    if (res == nullptr) return;
    D3D12_RESOURCE_BARRIER b = {};
    b.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource   = res;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter  = to;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    list->ResourceBarrier(1, &b);
}

// ---- shared resource creation (D3D12 side) --------------------------------
// Created with HEAP_FLAG_SHARED and ALLOW_SIMULTANEOUS_ACCESS -- the latter so
// the two APIs need no cross-API state transitions, which is what lets the whole
// synchronisation contract be the two fences and nothing else. Depth and Output
// carry a UAV; Output because DLSS writes it through one.
//
// The allocation size is recorded here rather than derived later:
// glImportMemoryWin32HandleEXT takes it as an argument, and a size that does not
// match the resource is how a driver is made to reject an otherwise valid
// import.
static bool D3D12MakeSharedTexture(int slot, UINT w, UINT h, DXGI_FORMAT fmt, bool uav)
{
    SharedTex &t = g_bridge.tex[slot];

    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width            = w;
    rd.Height           = h;
    rd.DepthOrArraySize = 1;
    rd.MipLevels        = 1;
    rd.Format           = fmt;
    rd.SampleDesc.Count = 1;
    rd.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    rd.Flags            = D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS |
                          (uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
                               : D3D12_RESOURCE_FLAG_NONE);

    HRESULT hr = g_bridge.dev12->CreateCommittedResource(
        &hp, D3D12_HEAP_FLAG_SHARED, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr,
        __uuidof(ID3D12Resource), reinterpret_cast<void **>(&t.tex12));
    if (FAILED(hr)) { Log("[bridge] %s: CreateCommittedResource failed 0x%08X", kSlotName[slot], hr); return false; }

    hr = g_bridge.dev12->CreateSharedHandle(t.tex12, nullptr, GENERIC_ALL, nullptr, &t.nt_handle);
    if (FAILED(hr)) { Log("[bridge] %s: CreateSharedHandle failed 0x%08X", kSlotName[slot], hr); return false; }

    const D3D12_RESOURCE_ALLOCATION_INFO ai = g_bridge.dev12->GetResourceAllocationInfo(0, 1, &rd);
    t.bytes = ai.SizeInBytes;

    {
        wchar_t n[64];
        _snwprintf_s(n, _TRUNCATE, L"dlss5-opengl-bridge %hs", kSlotName[slot]);
        t.tex12->SetName(n);
    }
    t.w = w; t.h = h; t.fmt12 = fmt;
    Log("[bridge] %-6s %ux%u dxgi=%u %llu bytes  D3D12 shared texture created (handle=%p)",
        kSlotName[slot], w, h, fmt, (unsigned long long)t.bytes, t.nt_handle);
    return true;
}

static bool D3D12CreateSharedFence(ID3D12Fence **out, HANDLE *handle)
{
    HRESULT hr = g_bridge.dev12->CreateFence(0, D3D12_FENCE_FLAG_SHARED,
                                             __uuidof(ID3D12Fence), reinterpret_cast<void **>(out));
    if (FAILED(hr)) { Log("[bridge] CreateFence(SHARED) failed 0x%08X", hr); return false; }
    hr = g_bridge.dev12->CreateSharedHandle(*out, nullptr, GENERIC_ALL, nullptr, handle);
    if (FAILED(hr)) { Log("[bridge] CreateSharedHandle(fence) failed 0x%08X", hr); return false; }
    return true;
}

// The D3D12 entry points must all come from the NGX dispatcher (_nvngx.dll from
// the driver store). Feature snippets (nvngx_dlss*.dll and the driver-downloaded
// modules) also export the per-feature Init/Create/Evaluate names, so a
// two-name fingerprint can land on a snippet that lacks the dispatcher-only
// services (AllocateParameters). Demand the complete set.
static const char *const kNgxD3D12Names[] = {
    "NVSDK_NGX_D3D12_AllocateParameters",
    "NVSDK_NGX_D3D12_CreateFeature",
    "NVSDK_NGX_D3D12_EvaluateFeature",
    "NVSDK_NGX_D3D12_ReleaseFeature",
};

static bool NgxHasFullD3D12Set(HMODULE m)
{
    for (const char *name : kNgxD3D12Names)
        if (GetProcAddress(m, name) == nullptr) return false;
    return GetProcAddress(m, "NVSDK_NGX_D3D12_Init_Ext") != nullptr ||
           GetProcAddress(m, "NVSDK_NGX_D3D12_Init_ProjectID") != nullptr;
}

static const wchar_t *NgxModuleName(HMODULE m)
{
    static wchar_t buf[MAX_PATH];
    GetModuleFileNameW(m, buf, MAX_PATH);
    const wchar_t *s = wcsrchr(buf, L'\\');
    return s ? s + 1 : buf;
}

// ---- finding the NGX dispatcher -------------------------------------------
// Unlike the DX11 and Vulkan bridges, nothing in an OpenGL game has caused NGX
// to load. The other two are woken up by the game's own NGX call, so the
// dispatcher is already in the process by the time they look; here nothing has
// ever asked for it, and a DLSS 5 add-on brings only the feature snippets
// (nvngx_dlss*.dll) with it -- those export the per-feature names but not the
// dispatcher-only services, which is why nvngx_dlssnr.dll is rejected below for
// lacking AllocateParameters.
//
// So this bridge has to load the dispatcher itself, and a bare
// LoadLibraryW(L"_nvngx.dll") does not find it: it lives in the driver store,
// which is not on any search path. NVIDIA's own static loader looks it up in the
// registry, and so does this.

static HMODULE NgxLoadDispatcherFrom(const wchar_t *dir, const char *how)
{
    wchar_t path[MAX_PATH];
    if (_snwprintf_s(path, _TRUNCATE, L"%s\\_nvngx.dll", dir) < 0) return nullptr;
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES)
    { Log("[bridge] no _nvngx.dll %s (%ls)", how, path); return nullptr; }

    // LOAD_WITH_ALTERED_SEARCH_PATH so the dispatcher's own dependencies resolve
    // out of the driver store next to it rather than out of the game folder.
    HMODULE m = LoadLibraryExW(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (m == nullptr)
    { Log("[bridge] _nvngx.dll %s would not load (0x%08X): %ls", how, GetLastError(), path); return nullptr; }
    if (!NgxHasFullD3D12Set(m))
    { Log("[bridge] _nvngx.dll %s loaded but lacks the full D3D12 export set: %ls", how, path); return nullptr; }
    Log("[bridge] NGX dispatcher loaded %s: %ls", how, path);
    return m;
}

// HKLM\SOFTWARE\NVIDIA Corporation\Global\NGXCore, value FullPath. This is the
// documented location and the one NVIDIA's own nvsdk_ngx_s loader reads.
static bool NgxCoreDirFromRegistry(wchar_t *out, DWORD cch)
{
    typedef LONG (WINAPI *PFN_RegGetValueW)(HKEY, LPCWSTR, LPCWSTR, DWORD, LPDWORD, PVOID, LPDWORD);
    HMODULE adv = LoadLibraryW(L"advapi32.dll");
    auto get = adv ? reinterpret_cast<PFN_RegGetValueW>(GetProcAddress(adv, "RegGetValueW")) : nullptr;
    if (get == nullptr) return false;

    DWORD bytes = cch * sizeof(wchar_t);
    const DWORD RRF_RT_REG_SZ_ = 0x00000002;
    const LONG r = get(HKEY_LOCAL_MACHINE, L"SOFTWARE\\NVIDIA Corporation\\Global\\NGXCore",
                       L"FullPath", RRF_RT_REG_SZ_, nullptr, out, &bytes);
    if (r != ERROR_SUCCESS)
    { Log("[bridge] NGXCore\\FullPath is not readable (%ld)", r); return false; }
    Log("[bridge] NGXCore\\FullPath = %ls", out);
    return out[0] != L'\0';
}

// Last resort if the registry key is missing: the driver store lays the package
// down under a name that varies only by a hash, so it can be found by pattern.
static HMODULE NgxLoadDispatcherFromDriverStore()
{
    wchar_t sys[MAX_PATH];
    if (GetSystemDirectoryW(sys, MAX_PATH) == 0) return nullptr;

    wchar_t pattern[MAX_PATH];
    if (_snwprintf_s(pattern, _TRUNCATE, L"%s\\DriverStore\\FileRepository\\nv*.inf_amd64_*", sys) < 0)
        return nullptr;

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return nullptr;
    HMODULE found = nullptr;
    do
    {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        wchar_t dir[MAX_PATH];
        if (_snwprintf_s(dir, _TRUNCATE, L"%s\\DriverStore\\FileRepository\\%s", sys, fd.cFileName) < 0)
            continue;
        found = NgxLoadDispatcherFrom(dir, "from the driver store");
    } while (found == nullptr && FindNextFileW(h, &fd));
    FindClose(h);
    return found;
}

static HMODULE FindNgxD3D12Loader(bool allow_load)
{
    if (HMODULE m = GetModuleHandleW(L"_nvngx.dll"))
        if (NgxHasFullD3D12Set(m))
        { Log("[bridge] NGX D3D12 entry points from %ls", NgxModuleName(m)); return m; }

    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    auto enum_modules = reinterpret_cast<BOOL (WINAPI *)(HANDLE, HMODULE *, DWORD, LPDWORD)>(
        GetProcAddress(k32, "K32EnumProcessModules"));
    HMODULE mods[1024]; DWORD needed = 0;
    if (enum_modules && enum_modules(GetCurrentProcess(), mods, sizeof(mods), &needed))
    {
        const DWORD n = needed / sizeof(HMODULE);
        for (DWORD i = 0; i < n; ++i)
            if (NgxHasFullD3D12Set(mods[i]))
            { Log("[bridge] NGX D3D12 entry points from %ls", NgxModuleName(mods[i])); return mods[i]; }

        // No module carries the complete surface: name the near misses so the
        // log pinpoints the missing export on whatever driver produced it.
        for (DWORD i = 0; i < n; ++i)
        {
            if (GetProcAddress(mods[i], "NVSDK_NGX_D3D12_CreateFeature") == nullptr &&
                GetProcAddress(mods[i], "NVSDK_NGX_D3D12_Init_Ext") == nullptr) continue;
            char missing[256] = "";
            for (const char *name : kNgxD3D12Names)
                if (GetProcAddress(mods[i], name) == nullptr)
                {
                    if (missing[0]) strcat_s(missing, ", ");
                    strcat_s(missing, name);
                }
            Log("[bridge] %ls has an NGX D3D12 surface but lacks: %s",
                NgxModuleName(mods[i]), missing[0] ? missing : "(an init entry point)");
        }
    }

    if (!allow_load) return nullptr;

    Log("[bridge] no NGX dispatcher is loaded yet; looking for _nvngx.dll");

    // 1. The bare name, in case something has already put the driver store on
    //    the search path -- cheapest, and correct where it works.
    if (HMODULE m = LoadLibraryW(L"_nvngx.dll"))
    {
        if (NgxHasFullD3D12Set(m))
        { Log("[bridge] NGX dispatcher loaded by name: %ls", NgxModuleName(m)); return m; }
        Log("[bridge] a module named _nvngx.dll loaded but lacks the full D3D12 export set");
    }
    else
        Log("[bridge] _nvngx.dll is not on the DLL search path (0x%08X); asking the "
            "registry where the driver put it", GetLastError());

    // 2. Where NVIDIA says it is. This is the lookup NVIDIA's own static NGX
    //    loader performs, and on a normal driver install it is the one that
    //    works -- the dispatcher lives in the driver store, which is on nobody's
    //    search path.
    wchar_t dir[MAX_PATH] = {};
    if (NgxCoreDirFromRegistry(dir, MAX_PATH))
        if (HMODULE m = NgxLoadDispatcherFrom(dir, "from NGXCore\\FullPath")) return m;

    // 3. The driver store by pattern, for an install whose registry entry is
    //    missing or stale.
    if (HMODULE m = NgxLoadDispatcherFromDriverStore()) return m;

    Warn("the NGX dispatcher (_nvngx.dll) could not be found. It ships with the NVIDIA "
         "driver and lives in the driver store, located through "
         "HKLM\\SOFTWARE\\NVIDIA Corporation\\Global\\NGXCore. A driver that is very old, "
         "or installed without the DLSS component, is the usual reason it is absent.");
    return nullptr;
}

// ---- evaluate detour probe -------------------------------------------------
// The whole point of this bridge is that a DLSS 5 Neural Rendering add-on
// detours NVSDK_NGX_D3D12_EvaluateFeature and inserts its pass there. The
// bridge itself never patches that function, so if its first bytes are (or
// become) a jump, an external party has attached -- and if they never do, the
// bridge's evaluate is plain driver DLAA with nothing riding on it. Logged at
// init and re-checked at the frame heartbeats.
static BYTE g_eval12_baseline[16];
static bool g_eval12_baseline_ok;
static bool g_eval12_detour_reported;

static bool LooksDetoured(const BYTE *p)
{
    if (p[0] == 0xE9) return true;                      // jmp rel32
    if (p[0] == 0xFF && p[1] == 0x25) return true;      // jmp [rip+disp32]
    if (p[0] == 0x48 && p[1] == 0xB8 &&                 // mov rax, imm64
        p[10] == 0xFF && p[11] == 0xE0) return true;    // jmp rax
    return false;
}

static void D3D12ProbeEvalDetour(const char *when)
{
    const BYTE *p = reinterpret_cast<const BYTE *>(g_bridge.eval_feature);
    if (p == nullptr) return;
    BYTE now[16];
    __try { memcpy(now, p, sizeof(now)); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return; }

    if (!g_eval12_baseline_ok)
    {
        memcpy(g_eval12_baseline, now, sizeof(now));
        g_eval12_baseline_ok = true;
        Log("[bridge] D3D12 EvaluateFeature entry (%s): %02X %02X %02X %02X -- %s", when,
            now[0], now[1], now[2], now[3],
            LooksDetoured(now) ? "DETOURED: an add-on is attached to the D3D12 evaluate"
                               : "not detoured: no add-on attached (yet)");
        return;
    }
    if (!g_eval12_detour_reported && memcmp(now, g_eval12_baseline, sizeof(now)) != 0)
    {
        g_eval12_detour_reported = true;
        Log("[bridge] D3D12 EvaluateFeature entry CHANGED (%s): %02X %02X %02X %02X -> %02X %02X %02X %02X%s",
            when, g_eval12_baseline[0], g_eval12_baseline[1], g_eval12_baseline[2], g_eval12_baseline[3],
            now[0], now[1], now[2], now[3],
            LooksDetoured(now) ? " -- an add-on has hooked the D3D12 evaluate" : "");
    }
}

// Opens the D3D12 device (LUID-matched), initialises a second NGX session on it,
// and creates the queue, allocators, retire fence and the two shared cross-API
// fences.
static HANDLE g_shared_fence_in;
static HANDLE g_shared_fence_out;

static bool D3D12InitSession()
{
    Log("");
    Log("################ bridge: opening D3D12 session ################");

    IDXGIAdapter1 *adapter = PickAdapterByLuid(g_bridge.gl_luid, g_bridge.luid_valid);
    if (adapter == nullptr) { Log("[bridge] no usable DXGI adapter"); goto fail; }

    {
        HMODULE d3d12 = LoadLibraryW(L"d3d12.dll");
        auto create_device = d3d12 ? reinterpret_cast<HRESULT (WINAPI *)(
            IUnknown *, D3D_FEATURE_LEVEL, REFIID, void **)>(
                GetProcAddress(d3d12, "D3D12CreateDevice")) : nullptr;
        if (create_device == nullptr) { Log("[bridge] no D3D12CreateDevice"); goto fail; }

        HRESULT hr = create_device(adapter, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device),
                                   reinterpret_cast<void **>(&g_bridge.dev12));
        if (FAILED(hr) || g_bridge.dev12 == nullptr)
        { Log("[bridge] D3D12CreateDevice failed 0x%08X", hr); goto fail; }

        adapter->QueryInterface(__uuidof(IDXGIAdapter3), reinterpret_cast<void **>(&g_bridge.adapter3));

        HMODULE ngx = FindNgxD3D12Loader(true);
        if (ngx == nullptr) { Log("[bridge] no module carries the complete NGX D3D12 export set"); goto fail; }
        g_ngx12_module = ngx;

        auto init_ext = reinterpret_cast<PFN_NGX_D3D12_Init_Ext>(GetProcAddress(ngx, "NVSDK_NGX_D3D12_Init_Ext"));
        auto init_proj = reinterpret_cast<PFN_NGX_D3D12_Init_ProjectID>(GetProcAddress(ngx, "NVSDK_NGX_D3D12_Init_ProjectID"));
        g_bridge.alloc_params    = reinterpret_cast<PFN_AllocateParameters>(GetProcAddress(ngx, "NVSDK_NGX_D3D12_AllocateParameters"));
        g_bridge.create_feature  = reinterpret_cast<PFN_D3D12CreateFeature>(GetProcAddress(ngx, "NVSDK_NGX_D3D12_CreateFeature"));
        g_bridge.eval_feature    = reinterpret_cast<PFN_D3D12EvaluateFeature>(GetProcAddress(ngx, "NVSDK_NGX_D3D12_EvaluateFeature"));
        g_bridge.release_feature = reinterpret_cast<PFN_D3D12ReleaseFeature>(GetProcAddress(ngx, "NVSDK_NGX_D3D12_ReleaseFeature"));
        if (!g_bridge.alloc_params || !g_bridge.create_feature || !g_bridge.eval_feature)
        { Log("[bridge] required NGX D3D12 exports missing"); goto fail; }
        D3D12ProbeEvalDetour("at export resolve");

        // Data path stays the bridge's own directory: certain to be writable
        // (the log lives there). Snippet discovery is PathListInfo's job.
        wchar_t data_path[MAX_PATH] = {};
        GetModuleFileNameW(g_self, data_path, MAX_PATH);
        if (wchar_t *s = wcsrchr(data_path, L'\\')) *(s + 1) = L'\0';
        Log("[bridge] NGX data path: %ls", data_path);

        const NgxFeatureCommonInfo *common = NgxBuildCommonInfo();

        // The SDK version constant is undocumented and has moved between NGX
        // releases, so it is negotiated. A fault (not a refusal) means NGX died
        // inside itself; stop at the first one rather than call back in.
        DWORD code = 0; bool inited = false, faulted = false;
        for (int ver = 0x13; ver <= 0x16 && !inited && !faulted; ++ver)
        {
            if (init_ext)
            {
                NVSDK_NGX_Result r = SafeInitExt(init_ext, 0x1000000ULL, data_path, g_bridge.dev12, ver, common, &code);
                if (code) { Log("[bridge] D3D12 Init_Ext(0x%02X) raised 0x%08X", ver, code); faulted = true; }
                else { Log("[bridge] D3D12 Init_Ext(0x%02X) -> %s", ver, r == NGX_SUCCESS ? "ok" : "refused"); if (r == NGX_SUCCESS) inited = true; }
            }
            if (!inited && !faulted && init_proj)
            {
                NVSDK_NGX_Result r = SafeInitProjectID(init_proj,
                    "a0f57b54-1daf-4934-90ae-c4035c19df04", data_path, g_bridge.dev12, ver, common, &code);
                if (code) { Log("[bridge] D3D12 Init_ProjectID(0x%02X) raised 0x%08X", ver, code); faulted = true; }
                else { Log("[bridge] D3D12 Init_ProjectID(0x%02X) -> %s", ver, r == NGX_SUCCESS ? "ok" : "refused"); if (r == NGX_SUCCESS) inited = true; }
            }
        }
        if (faulted) { Log("[bridge] NGX faulted opening its D3D12 session; stopping."); goto fail; }
        if (!inited) { Log("[bridge] no NGX D3D12 init entry point was accepted by this driver"); goto fail; }
        Log("[bridge] NGX D3D12 session initialised");
        D3D12ProbeEvalDetour("after D3D12 init");

        // A session that cannot host SuperSampling will refuse every
        // CreateFeature shape; learn that now instead of after building the
        // queue, textures and fences.
        if (D3D12QueryNgxCaps() == 0)
        { Log("[bridge] SuperSampling unavailable to this session despite explicit search paths"); goto fail; }

        D3D12_COMMAND_QUEUE_DESC qd = {};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        g_bridge.dev12->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), reinterpret_cast<void **>(&g_bridge.queue));
        for (int i = 0; i < Bridge::kFrames; ++i)
            g_bridge.dev12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                __uuidof(ID3D12CommandAllocator), reinterpret_cast<void **>(&g_bridge.alloc[i]));
        if (g_bridge.alloc[0])
            g_bridge.dev12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_bridge.alloc[0], nullptr,
                __uuidof(ID3D12GraphicsCommandList), reinterpret_cast<void **>(&g_bridge.list));
        if (g_bridge.list) g_bridge.list->Close();
        g_bridge.fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!g_bridge.queue || !g_bridge.list || !g_bridge.fence_event)
        { Log("[bridge] queue / command list / event creation failed"); goto fail; }

        hr = g_bridge.dev12->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence),
                                         reinterpret_cast<void **>(&g_bridge.fence_gpu));
        if (FAILED(hr)) { Log("[bridge] retire fence failed 0x%08X", hr); goto fail; }

        if (!D3D12CreateSharedFence(&g_bridge.fence_in,  &g_shared_fence_in))  goto fail;
        if (!D3D12CreateSharedFence(&g_bridge.fence_out, &g_shared_fence_out)) goto fail;

        NVSDK_NGX_Result rp = SafeAllocParams(g_bridge.alloc_params, &g_bridge.params, &code);
        if (code || rp != NGX_SUCCESS || g_bridge.params == nullptr)
        { Log("[bridge] AllocateParameters failed 0x%08X, %s", rp, NgxResultName(rp)); goto fail; }

        g_bridge.queue->SetName(L"dlss5-opengl-bridge queue");
        g_bridge.list->SetName(L"dlss5-opengl-bridge list");

        Log("[bridge] session ready: queue=%p list=%p fence_in=%p fence_out=%p (%d frames)",
            (void *)g_bridge.queue, (void *)g_bridge.list, (void *)g_bridge.fence_in,
            (void *)g_bridge.fence_out, Bridge::kFrames);
        Log("############# bridge: session open #############\n");
        adapter->Release();
        g_bridge.session_ready = true;
        return true;
    }

fail:
    if (adapter) adapter->Release();
    BridgeDisable("the D3D12 session failed to start");
    Log("############# bridge: session failed #############");
    return false;
}

// ---- feature creation ------------------------------------------------------
// Every value here is stated rather than copied. The DX11 and Vulkan bridges
// copy the game's create flags because the game is the only party that knows
// how its own buffers are laid out; here the bridge laid them out itself, so
// copying would mean copying from nobody. See BuildCreateFlags in gl_bridge.inc
// for what each bit is answering.
static void D3D12LogCreateFailureDetail()
{
    HRESULT drr = g_bridge.dev12 ? g_bridge.dev12->GetDeviceRemovedReason() : S_OK;
    if (drr != S_OK) { Log("[bridge] D3D12 device removed: 0x%08X", drr); return; }
    D3D12QueryNgxCaps();
}

static bool D3D12CreateDlssFeature()
{
    if (g_bridge.feature && g_bridge.release_feature)
    { g_bridge.release_feature(g_bridge.feature); g_bridge.feature = nullptr; }

    unsigned int quality  = g_bridge.quality;
    unsigned int flags    = g_bridge.create_flags;
    unsigned int subrects = (unsigned int)(g_cfg.subrects ? 1 : 0);

    g_bridge.params->Set("Width",     g_bridge.render_w);
    g_bridge.params->Set("Height",    g_bridge.render_h);
    g_bridge.params->Set("OutWidth",  g_bridge.out_w);
    g_bridge.params->Set("OutHeight", g_bridge.out_h);
    g_bridge.params->Set("PerfQualityValue", quality);
    g_bridge.params->Set("DLSS.Feature.Create.Flags", flags);
    g_bridge.params->Set("DLSS.Enable.Output.Subrects", subrects);
    g_bridge.params->Set("CreationNodeMask", 1u);
    g_bridge.params->Set("VisibilityNodeMask", 1u);
    g_bridge.params->Set("RTXValue", 0);

    Log("[bridge] create params: %ux%u -> %ux%u quality=%u flags=0x%X subrects=%u",
        g_bridge.render_w, g_bridge.render_h, g_bridge.out_w, g_bridge.out_h,
        quality, flags, subrects);
    LogCreateFlags("create flags", flags);

    // NGX refuses some parameter shapes outright (UnableToInitializeFeature)
    // without naming the offender. Retry the known repairs before giving up:
    // 1:1 geometry is only valid as DLAA, and output subrects are not accepted
    // everywhere.
    bool tried_dlaa = false, tried_nosub = false;
    for (;;)
    {
        g_bridge.feature = nullptr;
        if (!D3D12BeginCommands()) { Log("[bridge] could not start a command list for create"); return false; }

        DWORD code = 0;
        NVSDK_NGX_Result rf = SafeCreateFeature(g_bridge.create_feature, g_bridge.list,
                                                g_bridge.params, &g_bridge.feature, &code);
        const UINT64 v = D3D12EndCommands();

        if (g_bridge.fence_gpu->GetCompletedValue() < v)
        {
            g_bridge.fence_gpu->SetEventOnCompletion(v, g_bridge.fence_event);
            if (WaitForSingleObject(g_bridge.fence_event, 4000) != WAIT_OBJECT_0)
            { Log("[bridge] feature creation did not complete within 4 s"); BridgeDisable("creating the DLSS feature stalled"); return false; }
        }
        if (code)      { Warn("creating the DLSS feature crashed (0x%08X). A hand-copied nvngx_*.dll "
                              "version mismatch is the usual cause.", code); BridgeDisable("the DLSS feature crashed"); return false; }
        if (rf == NGX_SUCCESS && g_bridge.feature != nullptr) break;
        Log("[bridge] CreateFeature failed 0x%08X, %s", rf, NgxResultName(rf));

        if (!tried_dlaa && quality != NGX_QUALITY_DLAA &&
            g_bridge.render_w == g_bridge.out_w && g_bridge.render_h == g_bridge.out_h)
        {
            tried_dlaa = true; quality = NGX_QUALITY_DLAA;
            g_bridge.params->Set("PerfQualityValue", quality);
            Log("[bridge] retrying: PerfQualityValue=5 (DLAA) for 1:1 geometry");
            continue;
        }
        if (!tried_nosub && subrects != 0)
        {
            tried_nosub = true; subrects = 0;
            g_bridge.params->Set("DLSS.Enable.Output.Subrects", 0u);
            Log("[bridge] retrying: output subrects disabled");
            continue;
        }
        D3D12LogCreateFailureDetail();
        return false;
    }

    g_bridge.quality = quality;
    Log("[bridge] feature ready: render %ux%u -> output %ux%u handle=%p",
        g_bridge.render_w, g_bridge.render_h, g_bridge.out_w, g_bridge.out_h,
        (void *)g_bridge.feature);
    LogVideoMemory("after the DLSS feature was created");
    g_bridge.need_reset = true;
    return true;
}

// ---- the D3D12 half of one frame -------------------------------------------
// Wait for the GL inputs, run the evaluate (where the DLSS 5 add-on inserts its
// pass), signal that the output is ready. The texture params are already bound;
// the per-frame scalars are set by the caller.
//
// This runs on the game's own thread, inside the swap-buffers hook, between the
// GL signal and the GL wait. fence_out is signalled on EVERY path: the GL stream
// is about to wait on that value and skipping it would park the game's context
// with nobody left to release it.
static bool D3D12RunEvaluate(UINT64 v)
{
    g_bridge.queue->Wait(g_bridge.fence_in, v);
    if (!D3D12BeginCommands())
    {
        g_bridge.queue->Signal(g_bridge.fence_out, v);
        BridgeFail("d3d12 command list");
        return false;
    }

    Barrier(g_bridge.list, g_bridge.tex[SLOT_COLOR].tex12,  D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(g_bridge.list, g_bridge.tex[SLOT_DEPTH].tex12,  D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(g_bridge.list, g_bridge.tex[SLOT_MV].tex12,     D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(g_bridge.list, g_bridge.tex[SLOT_OUTPUT].tex12, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    DWORD code = 0;
    NVSDK_NGX_Result re = NGX_SUCCESS;
    static const char kMark[] = "ngx evaluate";
    g_bridge.list->BeginEvent(1, kMark, sizeof(kMark));
    if (g_cfg.mode != 1)
        re = SafeEvaluate(g_bridge.eval_feature, g_bridge.list, g_bridge.feature, g_bridge.params, &code);
    g_bridge.list->EndEvent();

    Barrier(g_bridge.list, g_bridge.tex[SLOT_COLOR].tex12,  D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    Barrier(g_bridge.list, g_bridge.tex[SLOT_DEPTH].tex12,  D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    Barrier(g_bridge.list, g_bridge.tex[SLOT_MV].tex12,     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    Barrier(g_bridge.list, g_bridge.tex[SLOT_OUTPUT].tex12, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);

    D3D12EndCommands();
    g_bridge.queue->Signal(g_bridge.fence_out, v);   // output ready for GL

    if (code)
    {
        Log("[bridge] evaluate raised 0x%08X -- disabling to protect the game", code);
        BridgeDisable("NGX raised an exception in the D3D12 evaluate");
        return false;
    }
    if (re != NGX_SUCCESS)
    {
        Log("[bridge] evaluate failed 0x%08X, %s", re, NgxResultName(re));
        BridgeFail("evaluate");
        return false;
    }
    g_bridge.consecutive_fails = 0;
    return true;
}

static void D3D12ReleaseSharedTex(int slot)
{
    SharedTex &t = g_bridge.tex[slot];
    if (t.nt_handle) { CloseHandle(t.nt_handle); t.nt_handle = nullptr; }
    if (t.tex12)     { t.tex12->Release();       t.tex12 = nullptr; }
    t.bytes = 0; t.w = t.h = 0;
}

static void D3D12ReleaseSession()
{
    D3D12DrainGpu();

    if (g_bridge.feature && g_bridge.release_feature)
    { g_bridge.release_feature(g_bridge.feature); g_bridge.feature = nullptr; }

    for (int i = 0; i < SLOT_COUNT; ++i) D3D12ReleaseSharedTex(i);

    if (g_shared_fence_in)  { CloseHandle(g_shared_fence_in);  g_shared_fence_in  = nullptr; }
    if (g_shared_fence_out) { CloseHandle(g_shared_fence_out); g_shared_fence_out = nullptr; }

    #define REL(x) if (g_bridge.x) { g_bridge.x->Release(); g_bridge.x = nullptr; }
    REL(fence_in); REL(fence_out); REL(fence_gpu);
    REL(list);
    for (int i = 0; i < Bridge::kFrames; ++i) REL(alloc[i]);
    REL(queue); REL(adapter3); REL(dev12);
    #undef REL
    if (g_bridge.fence_event) { CloseHandle(g_bridge.fence_event); g_bridge.fence_event = nullptr; }
    for (int i = 0; i < Bridge::kFrames; ++i) g_bridge.alloc_fence[i] = 0;

    g_bridge.session_ready = false;
    g_bridge.frame_ready   = false;
}
