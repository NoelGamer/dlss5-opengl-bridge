// NGX declarations, inlined so the project has no dependency on NVIDIA's NGX SDK
// headers. The Parameter vtable deliberately mirrors the declaration order in
// NVIDIA's nvsdk_ngx.h: MSVC emits same-name virtual overloads in reverse
// declaration order, which is what places the resource Get/Set on the vtable
// slots the shipping NGX consumers actually call. Keeping the order identical
// reproduces NVIDIA's layout without hardcoding any offset. (Same technique as
// the DLSS 5 DX11 Bridge and the Vulkan port.)
//
// Only the D3D12 half of NGX matters here. NGX has no OpenGL API at all -- that
// absence is the whole reason this bridge has to build the DLSS contract itself
// rather than mirror one, and it is the one structural difference from the DX11
// and Vulkan bridges. See README.md.

#pragma once

typedef int NVSDK_NGX_Result;
static const NVSDK_NGX_Result NGX_SUCCESS = 1;

struct NVSDK_NGX_Handle { unsigned int Id; };

struct ID3D11Resource;   // opaque, only used to keep the vtable shape identical
struct ID3D12Resource;   // opaque here; real pointers are set in d3d12_session.inc

struct NVSDK_NGX_Parameter
{
    virtual void Set(const char *InName, unsigned long long InValue) = 0;
    virtual void Set(const char *InName, float InValue) = 0;
    virtual void Set(const char *InName, double InValue) = 0;
    virtual void Set(const char *InName, unsigned int InValue) = 0;
    virtual void Set(const char *InName, int InValue) = 0;
    virtual void Set(const char *InName, ID3D11Resource *InValue) = 0;
    virtual void Set(const char *InName, ID3D12Resource *InValue) = 0;
    virtual void Set(const char *InName, void *InValue) = 0;

    virtual NVSDK_NGX_Result Get(const char *InName, unsigned long long *OutValue) const = 0;
    virtual NVSDK_NGX_Result Get(const char *InName, float *OutValue) const = 0;
    virtual NVSDK_NGX_Result Get(const char *InName, double *OutValue) const = 0;
    virtual NVSDK_NGX_Result Get(const char *InName, unsigned int *OutValue) const = 0;
    virtual NVSDK_NGX_Result Get(const char *InName, int *OutValue) const = 0;
    virtual NVSDK_NGX_Result Get(const char *InName, ID3D11Resource **OutValue) const = 0;
    virtual NVSDK_NGX_Result Get(const char *InName, ID3D12Resource **OutValue) const = 0;
    virtual NVSDK_NGX_Result Get(const char *InName, void **OutValue) const = 0;

    virtual void Reset() = 0;
};

typedef void (*PFN_NVSDK_NGX_ProgressCallback)(float, bool *);

// ---------------------------------------------------------------------------
// DLSS feature-creation flags, from nvsdk_ngx_defs.h.
//
// The DX11 and Vulkan bridges copy the game's value because the game is the
// only party that knows how its own buffers are laid out. This bridge builds
// the buffers, so it knows them exactly and states them outright -- see
// BuildCreateFlags in gl_bridge.inc for what each one is answering.
// ---------------------------------------------------------------------------
enum
{
    NGX_DLSS_FLAG_IS_HDR         = 1 << 0,
    NGX_DLSS_FLAG_MV_LOW_RES     = 1 << 1,
    NGX_DLSS_FLAG_MV_JITTERED    = 1 << 2,
    NGX_DLSS_FLAG_DEPTH_INVERTED = 1 << 3,
    NGX_DLSS_FLAG_DO_SHARPENING  = 1 << 5,
    NGX_DLSS_FLAG_AUTO_EXPOSURE  = 1 << 6,
};

// NVSDK_NGX_PerfQuality_Value. 5 is DLAA -- 1:1 geometry, which is what a
// bridge that upscales nothing has to ask for; NGX refuses a 1:1 feature under
// any other quality value.
enum
{
    NGX_QUALITY_MAX_PERF          = 0,
    NGX_QUALITY_BALANCED          = 1,
    NGX_QUALITY_MAX_QUALITY       = 2,
    NGX_QUALITY_ULTRA_PERFORMANCE = 3,
    NGX_QUALITY_ULTRA_QUALITY     = 4,
    NGX_QUALITY_DLAA              = 5,
};
