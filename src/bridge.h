// OpenGL -> D3D12 NGX bridge state.
//
// The DX11 and Vulkan bridges both work the same way: the game already drives
// DLSS through NGX on its own API, that evaluate is intercepted and forwarded
// untouched, and the same contract is then reproduced on a second NGX session
// running on a private D3D12 device -- the call a DLSS 5 Neural Rendering add-on
// detours and inserts its pass into.
//
// OpenGL cannot work that way, and the reason is not a detail: NGX has no
// OpenGL API. There is no NVSDK_NGX_OPENGL_EvaluateFeature to hook, because
// NVIDIA never shipped one -- NGX exists for D3D11, D3D12 and Vulkan only. An
// OpenGL game therefore never makes an NGX call of any kind, and there is no
// contract to mirror.
//
// So this bridge builds the contract instead of copying one. That is the second
// source of contract the DX11 bridge's own notes anticipate, and it comes with
// the obligation stated there: it must arrive complete. Every parameter is set
// explicitly, from something this bridge measured or created, and nothing is
// routed through a "the game did not set this" fallback -- there is no game to
// have set it.
//
//   Color   the scene render target, found in the GL frame (gl_capture.inc)
//   Depth   that target's depth, converted to R32F (gl_bridge.inc)
//   MV      derived from depth and the frame-to-frame view-projection
//           (motion.inc) -- an OpenGL game with no TAA has no motion vectors
//           of its own, so they are reprojected rather than read
//   scalars stated outright: jitter is zero because the game does not jitter,
//           depth is not inverted because GL depth is not, exposure is
//           automatic because there is no exposure texture to hand over
//
// Per frame, all of it inline on the game's GL context at the frame boundary:
//
//   1. blit the scene colour into the shared Color texture
//   2. one fullscreen pass writes shared Depth (R32F) and shared MV (RG16F)
//   3. glSignalSemaphoreEXT on the semaphore aliasing fence_in
//   4. the D3D12 queue waits fence_in, runs the evaluate -- where the DLSS 5
//      add-on inserts its neural pass -- and signals fence_out
//   5. glWaitSemaphoreEXT on the semaphore aliasing fence_out, then blit the
//      shared Output back over the game's own target
//
// GL has an implicit single command stream, like D3D11's immediate context and
// unlike Vulkan, so steps 3 and 5 are ordinary calls in the middle of the frame
// and none of the Vulkan port's worker-thread machinery is needed.
//
// The shared textures are created on the D3D12 side (HEAP_FLAG_SHARED +
// ALLOW_SIMULTANEOUS_ACCESS) and imported into GL as texture objects backed by
// EXT_memory_object_win32 memory objects. The two shared D3D12 fences are
// imported as EXT_semaphore_win32 semaphores.

#pragma once

enum { SLOT_COLOR = 0, SLOT_OUTPUT, SLOT_DEPTH, SLOT_MV, SLOT_COUNT };

static const char *kSlotKey[SLOT_COUNT]  = { "Color", "Output", "Depth", "MotionVectors" };
static const char *kSlotName[SLOT_COUNT] = { "Color", "Output", "Depth", "MV" };

// One imported shared texture: the D3D12 resource, its shared NT handle, and
// the GL objects that alias the same memory on the game's context.
struct SharedTex
{
    ID3D12Resource *tex12;
    HANDLE          nt_handle;   // from ID3D12Device::CreateSharedHandle
    UINT64          bytes;       // allocation size, which the GL import wants
    GLuint          mem;         // GL memory object holding nt_handle
    GLuint          tex;         // GL texture backed by that memory
    GLuint          fbo;         // FBO with tex on COLOR_ATTACHMENT0, for blits
    UINT            w, h;
    DXGI_FORMAT     fmt12;
    GLenum          fmtgl;       // sized internal format of the GL alias
};

// What one frame's capture settled on: the framebuffer holding the scene, its
// size, and what its depth looks like. Filled in gl_capture.inc.
//
// The game's own depth texture is never sampled directly. A depth texture whose
// GL_TEXTURE_COMPARE_MODE the game left set to comparison sampling reads back
// undefined through an ordinary sampler2D, and fixing that would mean writing to
// the game's texture state -- which this bridge does not do. The depth is
// blitted into a texture the bridge owns instead, whose parameters it is allowed
// to set. That is one extra blit and no way to corrupt the game.
struct GlSource
{
    GLuint fbo;             // framebuffer holding the scene (0 = the default one)
    UINT   w, h;            // its colour size
    GLenum color_fmt;       // sized internal format, 0 if unknowable
    bool   color_is_float;  // drives the IsHDR create flag
    bool   has_depth;
    // The depth attachment's exact sized internal format. glBlitFramebuffer
    // refuses a depth blit between different ones, so the bridge's own depth
    // copy is created with this rather than with something inferred from a bit
    // count -- inferring it is how a D24 source and a D24S8 copy end up as an
    // INVALID_OPERATION on the one game that has no stencil.
    GLenum depth_fmt;
    GLint  samples;         // > 1 means the source is multisampled
};

// A 4x4 column-major matrix, the layout glGetFloatv and glUniformMatrix4fv both
// use, so captured matrices never need transposing on the way in.
struct Mat4 { float m[16]; };

struct Bridge
{
    bool disabled;          // set after a hard failure; never retried
    bool session_ready;     // D3D12 device, queue, fences, NGX session
    bool gl_ready;          // GL entry points, shaders, FBOs, VAO
    bool frame_ready;       // shared textures and NGX feature match the source
    int  consecutive_fails;

    // ---- D3D12 side (identical in shape to both other bridges) -----------
    ID3D12Device              *dev12;
    IDXGIAdapter3             *adapter3;
    ID3D12CommandQueue        *queue;
    ID3D12GraphicsCommandList *list;

    // One allocator per frame in flight, each remembering the fence value that
    // retires it, so none is reset while the GPU is still reading it.
    static const int           kFrames = 3;
    ID3D12CommandAllocator    *alloc[kFrames];
    UINT64                     alloc_fence[kFrames];
    int                        frame_slot;

    HANDLE                     fence_event;

    // fence_in : signalled by the GL stream once the input copies are done,
    //            waited on by the D3D12 queue before the evaluate.
    // fence_out: signalled by the D3D12 queue after the evaluate, waited on by
    //            the GL stream before the copy-back.
    ID3D12Fence               *fence_in;
    ID3D12Fence               *fence_out;
    ID3D12Fence               *fence_gpu;   // D3D12-internal retire fence
    UINT64                     gpu_value;
    UINT64                     timeline;    // per-frame value on both shared fences

    GLuint                     sem_in;      // aliases fence_in
    GLuint                     sem_out;     // aliases fence_out

    PFN_D3D12CreateFeature   create_feature;
    PFN_D3D12EvaluateFeature eval_feature;
    PFN_D3D12ReleaseFeature  release_feature;
    PFN_AllocateParameters   alloc_params;

    NVSDK_NGX_Parameter *params;
    NVSDK_NGX_Handle    *feature;

    SharedTex tex[SLOT_COUNT];

    // The contract, as this bridge decided it. render_* is what the game draws,
    // out_* is what DLSS returns. They are equal in the default backbuffer mode
    // -- see the README on why 1:1 (DLAA) is the honest operating point for a
    // bridge that cannot make an OpenGL game render smaller.
    UINT render_w, render_h;
    UINT out_w, out_h;
    UINT create_flags;
    UINT quality;

    bool   need_reset;
    UINT64 frames_done;

    // ---- GL side ---------------------------------------------------------
    HGLRC  ctx;             // the context everything below belongs to
    HDC    hdc;
    int    gl_major, gl_minor;
    bool   core_profile;
    LUID   gl_luid;         // the GL device's LUID, for adapter matching
    bool   luid_valid;

    GLuint vao;             // core profile refuses to draw without one
    GLuint prog_convert;    // depth+MV pass
    GLint  u_depth, u_reproj, u_size, u_have_mv, u_depth_scale;
    GLuint fbo_convert;     // Depth + MV as two colour attachments
    GLuint fbo_scratch;     // used to wrap whatever needs wrapping for a blit

    GLuint depth_copy_tex;  // private depth texture, when the game's depth is a
    GLuint depth_copy_fbo;  //   renderbuffer or the default framebuffer's
    GLenum depth_copy_fmt;
    UINT   depth_copy_w, depth_copy_h;

    GlSource src;           // what this frame's capture settled on

    // The view-projection of this frame and the last, and whether they are
    // trustworthy. Motion vectors are the difference between them.
    Mat4 vp, vp_prev;
    bool vp_valid, vp_prev_valid;

    LONGLONG qpf;
    LONGLONG cpu_ticks;
    LONGLONG span_start;
    UINT64   timed_frames;
    LONGLONG last_entry, iv_min, iv_max;
};

static Bridge g_bridge;
