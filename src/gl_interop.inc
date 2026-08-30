// The OpenGL half of the bridge: resolving the entry points, importing the
// D3D12 shared textures and fences onto the game's context, and the state
// discipline that lets all of it run in the middle of somebody else's frame.
//
// Two things make this simpler than the Vulkan port and about as simple as the
// D3D11 original. GL has one implicit command stream per context, so
// glSignalSemaphoreEXT and glWaitSemaphoreEXT are ordinary calls placed inline
// rather than a submit-granularity handoff needing a worker thread. And the
// shared textures are created by the bridge rather than aliased from the game,
// so their formats are chosen once -- RGBA16F, R32F, RG16F -- and the entire
// format-negotiation surface both other bridges carry does not exist here.
//
// The one thing that is harder: GL state is global and the game owns it. Every
// pass below runs inside GlStateGuard, which puts back everything it touched.
// A bridge that leaks one binding produces a corrupted frame that looks like a
// game bug, so the guard is not optional and nothing draws outside one.

// ---------------------------------------------------------------------------
// entry-point resolution
// ---------------------------------------------------------------------------
static HMODULE g_opengl32;
typedef PROC (WINAPI *PFN_wglGetProcAddress)(LPCSTR);
static PFN_wglGetProcAddress g_wglGetProcAddress;

// Defined in gl_capture.inc: wglGetProcAddress with the bridge's own patch
// lifted, so what comes back is always the driver's function and never one of
// the bridge's observation wrappers.
static PROC WINAPI RealGetProcAddress(LPCSTR name);

static void *GlGet(const char *name)
{
    // wglGetProcAddress answers only for functions the current context has, and
    // several drivers return 1, 2, 3 or -1 rather than null for a missing one.
    PROC p = RealGetProcAddress(name);
    const uintptr_t v = reinterpret_cast<uintptr_t>(p);
    if (v > 3 && v != static_cast<uintptr_t>(-1)) return reinterpret_cast<void *>(p);
    if (g_opengl32) return reinterpret_cast<void *>(GetProcAddress(g_opengl32, name));
    return nullptr;
}

#define GL_LOAD(field, name) *reinterpret_cast<void **>(&GL.field) = GlGet(name)

// Empties the GL error queue without reporting anything, for the places that
// probe state and expect some of the probes to be refused. GL records errors in
// a queue and glGetError removes one at a time, so sampling it once leaves the
// rest behind for whichever call site asks next -- which is how a fault gets
// blamed on the code after the one that caused it.
static void GlDrainErrors()
{
    if (GL.GetError == nullptr) return;
    for (int i = 0; i < 16 && GL.GetError() != GL_NO_ERROR; ++i) {}
}

static bool GlHasExtension(const char *want)
{
    // Core profiles removed the single GL_EXTENSIONS string; ask both ways.
    if (GL.GetStringi && GL.GetIntegerv)
    {
        GLint n = 0;
        GL.GetIntegerv(GL_NUM_EXTENSIONS, &n);
        for (GLint i = 0; i < n; ++i)
        {
            const GLubyte *e = GL.GetStringi(GL_EXTENSIONS, static_cast<GLuint>(i));
            if (e && strcmp(reinterpret_cast<const char *>(e), want) == 0) return true;
        }
        if (n > 0) return false;
    }
    if (GL.GetString)
    {
        const GLubyte *all = GL.GetString(GL_EXTENSIONS);
        if (all) return strstr(reinterpret_cast<const char *>(all), want) != nullptr;
    }
    return false;
}

// Resolves everything and states plainly what is missing. Called once per GL
// context, from the swap hook, with that context current.
static bool GlResolve()
{
    if (g_opengl32 == nullptr) g_opengl32 = GetModuleHandleW(L"opengl32.dll");
    if (g_opengl32 == nullptr) { Log("[gl] opengl32.dll is not in the process"); return false; }
    if (g_wglGetProcAddress == nullptr)
        g_wglGetProcAddress = reinterpret_cast<PFN_wglGetProcAddress>(
            GetProcAddress(g_opengl32, "wglGetProcAddress"));

    memset(&GL, 0, sizeof(GL));

    GL_LOAD(GetString,               "glGetString");
    GL_LOAD(GetError,                "glGetError");
    GL_LOAD(GetIntegerv,             "glGetIntegerv");
    GL_LOAD(GetFloatv,               "glGetFloatv");
    GL_LOAD(GetBooleanv,             "glGetBooleanv");
    GL_LOAD(Enable,                  "glEnable");
    GL_LOAD(Disable,                 "glDisable");
    GL_LOAD(IsEnabled,               "glIsEnabled");
    GL_LOAD(Viewport,                "glViewport");
    GL_LOAD(BindTexture,             "glBindTexture");
    GL_LOAD(GenTextures,             "glGenTextures");
    GL_LOAD(DeleteTextures,          "glDeleteTextures");
    GL_LOAD(TexImage2D,              "glTexImage2D");
    GL_LOAD(TexParameteri,           "glTexParameteri");
    GL_LOAD(GetTexLevelParameteriv,  "glGetTexLevelParameteriv");
    GL_LOAD(DrawArrays,              "glDrawArrays");
    GL_LOAD(Flush,                   "glFlush");
    GL_LOAD(Finish,                  "glFinish");
    GL_LOAD(PixelStorei,             "glPixelStorei");
    GL_LOAD(ColorMask,               "glColorMask");
    GL_LOAD(DepthMask,               "glDepthMask");
    GL_LOAD(ReadPixels,              "glReadPixels");
    GL_LOAD(ReadBuffer,              "glReadBuffer");
    GL_LOAD(DrawBuffer,              "glDrawBuffer");

    GL_LOAD(GetStringi,              "glGetStringi");
    GL_LOAD(ActiveTexture,           "glActiveTexture");
    GL_LOAD(CreateShader,            "glCreateShader");
    GL_LOAD(ShaderSource,            "glShaderSource");
    GL_LOAD(CompileShader,           "glCompileShader");
    GL_LOAD(GetShaderiv,             "glGetShaderiv");
    GL_LOAD(GetShaderInfoLog,        "glGetShaderInfoLog");
    GL_LOAD(DeleteShader,            "glDeleteShader");
    GL_LOAD(CreateProgram,           "glCreateProgram");
    GL_LOAD(AttachShader,            "glAttachShader");
    GL_LOAD(LinkProgram,             "glLinkProgram");
    GL_LOAD(GetProgramiv,            "glGetProgramiv");
    GL_LOAD(GetProgramInfoLog,       "glGetProgramInfoLog");
    GL_LOAD(DeleteProgram,           "glDeleteProgram");
    GL_LOAD(UseProgram,              "glUseProgram");
    GL_LOAD(GetUniformLocation,      "glGetUniformLocation");
    GL_LOAD(Uniform1i,               "glUniform1i");
    GL_LOAD(Uniform1f,               "glUniform1f");
    GL_LOAD(Uniform2f,               "glUniform2f");
    GL_LOAD(UniformMatrix4fv,        "glUniformMatrix4fv");

    GL_LOAD(GenFramebuffers,         "glGenFramebuffers");
    GL_LOAD(DeleteFramebuffers,      "glDeleteFramebuffers");
    GL_LOAD(BindFramebuffer,         "glBindFramebuffer");
    GL_LOAD(FramebufferTexture2D,    "glFramebufferTexture2D");
    GL_LOAD(CheckFramebufferStatus,  "glCheckFramebufferStatus");
    GL_LOAD(BlitFramebuffer,         "glBlitFramebuffer");
    GL_LOAD(GetFramebufferAttachmentParameteriv, "glGetFramebufferAttachmentParameteriv");
    GL_LOAD(DrawBuffers,             "glDrawBuffers");
    GL_LOAD(BindRenderbuffer,        "glBindRenderbuffer");
    GL_LOAD(GetRenderbufferParameteriv, "glGetRenderbufferParameteriv");

    GL_LOAD(GenVertexArrays,         "glGenVertexArrays");
    GL_LOAD(DeleteVertexArrays,      "glDeleteVertexArrays");
    GL_LOAD(BindVertexArray,         "glBindVertexArray");

    GL_LOAD(GetUnsignedBytevEXT,          "glGetUnsignedBytevEXT");
    GL_LOAD(CreateMemoryObjectsEXT,       "glCreateMemoryObjectsEXT");
    GL_LOAD(DeleteMemoryObjectsEXT,       "glDeleteMemoryObjectsEXT");
    GL_LOAD(MemoryObjectParameterivEXT,   "glMemoryObjectParameterivEXT");
    GL_LOAD(ImportMemoryWin32HandleEXT,   "glImportMemoryWin32HandleEXT");
    GL_LOAD(TexStorageMem2DEXT,           "glTexStorageMem2DEXT");

    GL_LOAD(GenSemaphoresEXT,             "glGenSemaphoresEXT");
    GL_LOAD(DeleteSemaphoresEXT,          "glDeleteSemaphoresEXT");
    GL_LOAD(ImportSemaphoreWin32HandleEXT,"glImportSemaphoreWin32HandleEXT");
    GL_LOAD(SemaphoreParameterui64vEXT,   "glSemaphoreParameterui64vEXT");
    GL_LOAD(WaitSemaphoreEXT,             "glWaitSemaphoreEXT");
    GL_LOAD(SignalSemaphoreEXT,           "glSignalSemaphoreEXT");

    if (GL.GetString == nullptr || GL.GetIntegerv == nullptr)
    { Log("[gl] opengl32.dll exports nothing usable"); return false; }

    const char *vendor  = reinterpret_cast<const char *>(GL.GetString(GL_VENDOR));
    const char *rend    = reinterpret_cast<const char *>(GL.GetString(GL_RENDERER));
    const char *version = reinterpret_cast<const char *>(GL.GetString(GL_VERSION));
    Log("[gl] %s | %s | %s", vendor ? vendor : "?", rend ? rend : "?", version ? version : "?");

    g_bridge.gl_major = g_bridge.gl_minor = 0;
    GL.GetIntegerv(GL_MAJOR_VERSION, &g_bridge.gl_major);
    GL.GetIntegerv(GL_MINOR_VERSION, &g_bridge.gl_minor);
    if (g_bridge.gl_major == 0 && version) sscanf_s(version, "%d.%d", &g_bridge.gl_major, &g_bridge.gl_minor);
    GlDrainErrors();   // GL_MAJOR_VERSION is an error before 3.0

    GLint mask = 0;
    GL.GetIntegerv(GL_CONTEXT_PROFILE_MASK, &mask);
    GlDrainErrors();
    g_bridge.core_profile = (mask & GL_CONTEXT_CORE_PROFILE_BIT) != 0;
    Log("[gl] context reports %d.%d, %s profile", g_bridge.gl_major, g_bridge.gl_minor,
        g_bridge.core_profile ? "core" : "compatibility");

    // The shaders are #version 330. Everything below that is a context this
    // bridge cannot render in, and saying so is more use than failing later
    // inside a compile.
    if (g_bridge.gl_major < 3 || (g_bridge.gl_major == 3 && g_bridge.gl_minor < 3))
    {
        Warn("this game's OpenGL context is %d.%d. The bridge needs 3.3 or newer and has "
             "stopped. The game is unaffected.", g_bridge.gl_major, g_bridge.gl_minor);
        return false;
    }

    // The two extensions the whole transport rests on. Both are NVIDIA-supported
    // on every driver that can run DLSS at all, so their absence means the
    // context is not on the NVIDIA GPU -- an Optimus laptop rendering on the
    // integrated part is the case that produces it.
    struct { const char *name; bool present; } need[] = {
        { "GL_EXT_memory_object",        GlHasExtension("GL_EXT_memory_object")        },
        { "GL_EXT_memory_object_win32",  GlHasExtension("GL_EXT_memory_object_win32")  },
        { "GL_EXT_semaphore",            GlHasExtension("GL_EXT_semaphore")            },
        { "GL_EXT_semaphore_win32",      GlHasExtension("GL_EXT_semaphore_win32")      },
    };
    bool all = true;
    for (const auto &e : need) { if (!e.present) { Log("[gl] missing %s", e.name); all = false; } }
    if (!all)
    {
        Warn("this OpenGL context does not offer the D3D12 interop extensions "
             "(GL_EXT_memory_object_win32 / GL_EXT_semaphore_win32). On a laptop this "
             "usually means the game is rendering on the integrated GPU: force it onto "
             "the NVIDIA one in the NVIDIA Control Panel. The game is unaffected.");
        return false;
    }

    const void *required[] = {
        (void *)GL.BindFramebuffer, (void *)GL.GenFramebuffers, (void *)GL.FramebufferTexture2D,
        (void *)GL.BlitFramebuffer, (void *)GL.CheckFramebufferStatus, (void *)GL.DrawBuffers,
        (void *)GL.GetFramebufferAttachmentParameteriv,
        (void *)GL.CreateShader, (void *)GL.CreateProgram, (void *)GL.UseProgram,
        (void *)GL.GetUniformLocation, (void *)GL.UniformMatrix4fv, (void *)GL.ActiveTexture,
        (void *)GL.Uniform1i, (void *)GL.Uniform1f, (void *)GL.Uniform2f,
        (void *)GL.GenVertexArrays, (void *)GL.BindVertexArray,
        (void *)GL.CreateMemoryObjectsEXT, (void *)GL.ImportMemoryWin32HandleEXT,
        (void *)GL.MemoryObjectParameterivEXT, (void *)GL.TexStorageMem2DEXT,
        (void *)GL.GenSemaphoresEXT, (void *)GL.ImportSemaphoreWin32HandleEXT,
        (void *)GL.SemaphoreParameterui64vEXT, (void *)GL.WaitSemaphoreEXT,
        (void *)GL.SignalSemaphoreEXT,
    };
    static const char *const required_names[] = {
        "glBindFramebuffer", "glGenFramebuffers", "glFramebufferTexture2D",
        "glBlitFramebuffer", "glCheckFramebufferStatus", "glDrawBuffers",
        "glGetFramebufferAttachmentParameteriv",
        "glCreateShader", "glCreateProgram", "glUseProgram",
        "glGetUniformLocation", "glUniformMatrix4fv", "glActiveTexture",
        "glUniform1i", "glUniform1f", "glUniform2f",
        "glGenVertexArrays", "glBindVertexArray",
        "glCreateMemoryObjectsEXT", "glImportMemoryWin32HandleEXT",
        "glMemoryObjectParameterivEXT", "glTexStorageMem2DEXT",
        "glGenSemaphoresEXT", "glImportSemaphoreWin32HandleEXT",
        "glSemaphoreParameterui64vEXT", "glWaitSemaphoreEXT",
        "glSignalSemaphoreEXT",
    };
    bool complete = true;
    for (size_t i = 0; i < _countof(required); ++i)
        if (required[i] == nullptr)
        { Log("[gl] entry point not resolved: %s", required_names[i]); complete = false; }
    if (!complete) { Log("[gl] the context is missing entry points the bridge needs"); return false; }

    // The LUID picks the DXGI adapter, so the D3D12 device lands on the GPU the
    // GL context is actually on. Shared NT handles require that they match.
    g_bridge.luid_valid = false;
    if (GL.GetUnsignedBytevEXT)
    {
        GLubyte luid[GL_LUID_SIZE_EXT] = {};
        GL.GetUnsignedBytevEXT(GL_DEVICE_LUID_EXT, luid);
        if (GL.GetError == nullptr || GL.GetError() == GL_NO_ERROR)
        {
            memcpy(&g_bridge.gl_luid, luid, sizeof(LUID));
            g_bridge.luid_valid = true;
            Log("[gl] device LUID %08lX-%08lX",
                (unsigned long)g_bridge.gl_luid.HighPart, (unsigned long)g_bridge.gl_luid.LowPart);
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// state guard
//
// Everything the bridge draws happens between a save and a restore. The list is
// exactly what the passes below touch and nothing more -- a guard that saves the
// world is slower and no safer, and one that saves too little leaves the game
// rendering through the bridge's bindings.
// ---------------------------------------------------------------------------
struct GlStateGuard
{
    GLint  draw_fbo, read_fbo, program, vao, active_tex, tex0, tex1;
    GLint  viewport[4];
    GLboolean depth_test, blend, cull, scissor, stencil, srgb, depth_mask;
    GLboolean color_mask[4];
    bool   ok;

    GlStateGuard()
    {
        // Every function the guard itself calls, checked once. A guard that
        // half-saves is worse than no guard: it would restore some of the game's
        // state and leave the rest of it holding the bridge's.
        ok = GL.GetIntegerv && GL.GetBooleanv && GL.IsEnabled && GL.Enable && GL.Disable &&
             GL.ActiveTexture && GL.BindTexture && GL.BindVertexArray && GL.UseProgram &&
             GL.BindFramebuffer && GL.Viewport && GL.DepthMask && GL.ColorMask;
        if (!ok) return;
        GL.GetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw_fbo);
        GL.GetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_fbo);
        GL.GetIntegerv(GL_CURRENT_PROGRAM, &program);
        GL.GetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
        GL.GetIntegerv(GL_ACTIVE_TEXTURE, &active_tex);
        GL.GetIntegerv(GL_VIEWPORT, viewport);

        GL.ActiveTexture(GL_TEXTURE0);
        GL.GetIntegerv(GL_TEXTURE_BINDING_2D, &tex0);
        GL.ActiveTexture(GL_TEXTURE0 + 1);
        GL.GetIntegerv(GL_TEXTURE_BINDING_2D, &tex1);

        depth_test = GL.IsEnabled(GL_DEPTH_TEST);
        blend      = GL.IsEnabled(GL_BLEND);
        cull       = GL.IsEnabled(GL_CULL_FACE);
        scissor    = GL.IsEnabled(GL_SCISSOR_TEST);
        stencil    = GL.IsEnabled(GL_STENCIL_TEST);
        srgb       = GL.IsEnabled(GL_FRAMEBUFFER_SRGB);
        GL.GetBooleanv(GL_DEPTH_WRITEMASK, &depth_mask);
        GL.GetBooleanv(GL_COLOR_WRITEMASK, color_mask);
        GlDrainErrors();   // swallow anything the queries raised

        // The state the passes want: no test may reject a fullscreen triangle,
        // no blend may mix it with what was there, and no sRGB re-encode may
        // change a value on its way through the transport.
        GL.Disable(GL_DEPTH_TEST);
        GL.Disable(GL_BLEND);
        GL.Disable(GL_CULL_FACE);
        GL.Disable(GL_SCISSOR_TEST);
        GL.Disable(GL_STENCIL_TEST);
        GL.Disable(GL_FRAMEBUFFER_SRGB);
        GL.DepthMask(GL_FALSE);
        GL.ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    }

    ~GlStateGuard()
    {
        if (!ok) return;
        GL.ActiveTexture(GL_TEXTURE0 + 1);
        GL.BindTexture(GL_TEXTURE_2D, (GLuint)tex1);
        GL.ActiveTexture(GL_TEXTURE0);
        GL.BindTexture(GL_TEXTURE_2D, (GLuint)tex0);
        GL.ActiveTexture((GLenum)active_tex);

        GL.BindVertexArray((GLuint)vao);
        GL.UseProgram((GLuint)program);
        GL.BindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)draw_fbo);
        GL.BindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)read_fbo);
        GL.Viewport(viewport[0], viewport[1], viewport[2], viewport[3]);

        depth_test ? GL.Enable(GL_DEPTH_TEST)      : GL.Disable(GL_DEPTH_TEST);
        blend      ? GL.Enable(GL_BLEND)           : GL.Disable(GL_BLEND);
        cull       ? GL.Enable(GL_CULL_FACE)       : GL.Disable(GL_CULL_FACE);
        scissor    ? GL.Enable(GL_SCISSOR_TEST)    : GL.Disable(GL_SCISSOR_TEST);
        stencil    ? GL.Enable(GL_STENCIL_TEST)    : GL.Disable(GL_STENCIL_TEST);
        srgb       ? GL.Enable(GL_FRAMEBUFFER_SRGB): GL.Disable(GL_FRAMEBUFFER_SRGB);
        GL.DepthMask(depth_mask);
        GL.ColorMask(color_mask[0], color_mask[1], color_mask[2], color_mask[3]);
        GlDrainErrors();
    }
};

// A named GL error, reported once per site so a per-frame fault cannot bury the
// log the way an unguarded per-frame message would.
//
// The queue is drained rather than sampled. GL records errors in a queue and
// glGetError removes one; leaving the rest behind would hand them to whichever
// call site asked next, which is how a fault gets blamed on the code after the
// one that caused it.
static bool GlCheck(const char *where)
{
    if (GL.GetError == nullptr) return true;
    GLenum first = GL.GetError();
    if (first == GL_NO_ERROR) return true;
    for (int i = 0; i < 16 && GL.GetError() != GL_NO_ERROR; ++i) {}

    static const char *seen[24];
    static int count;
    for (int i = 0; i < count; ++i) if (strcmp(seen[i], where) == 0) return false;
    if (count < (int)_countof(seen)) seen[count++] = where;
    Log("[gl] %s raised GL error 0x%04X (reported once)", where, first);
    return false;
}

// ---------------------------------------------------------------------------
// importing the D3D12 side
// ---------------------------------------------------------------------------

// A texture the bridge owns: NEAREST, clamped, and explicitly not a comparison
// sampler, so an ordinary sampler2D reads exactly the value that was written.
static void GlSetPlainSampling(GLuint tex)
{
    GL.BindTexture(GL_TEXTURE_2D, tex);
    GL.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    GL.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    GL.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    GL.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    GL.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE);
}

// Wraps one shared D3D12 resource in a GL texture aliasing the same memory, and
// gives it an FBO so it can be blitted to and from.
//
// GL_DEDICATED_MEMORY_OBJECT_EXT is set before the import and is not optional: a
// committed D3D12 resource is a dedicated allocation, and importing it without
// saying so is rejected by the driver rather than silently mishandled.
static bool GlImportSharedTex(int slot)
{
    SharedTex &t = g_bridge.tex[slot];

    GL.CreateMemoryObjectsEXT(1, &t.mem);
    const GLint dedicated = GL_TRUE;
    GL.MemoryObjectParameterivEXT(t.mem, GL_DEDICATED_MEMORY_OBJECT_EXT, &dedicated);
    GL.ImportMemoryWin32HandleEXT(t.mem, t.bytes, GL_HANDLE_TYPE_D3D12_RESOURCE_EXT, t.nt_handle);
    if (!GlCheck("glImportMemoryWin32HandleEXT"))
    { Log("[gl] %s: importing the D3D12 resource failed", kSlotName[slot]); return false; }

    // Importing does not transfer ownership -- the driver has taken its own
    // reference to the payload -- so the extension asks the application to close
    // its handle once it no longer needs it. Doing that here also keeps teardown
    // from double-closing.
    //
    // A consequence worth knowing when reading the log: the kernel reuses the
    // numeric value straight away, so the four slots frequently report the same
    // handle. That is handle recycling, not four textures sharing one.
    CloseHandle(t.nt_handle);
    t.nt_handle = nullptr;

    GL.GenTextures(1, &t.tex);
    GL.BindTexture(GL_TEXTURE_2D, t.tex);
    GL.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_TILING_EXT, GL_OPTIMAL_TILING_EXT);
    GL.TexStorageMem2DEXT(GL_TEXTURE_2D, 1, t.fmtgl, (GLsizei)t.w, (GLsizei)t.h, t.mem, 0);
    if (!GlCheck("glTexStorageMem2DEXT"))
    { Log("[gl] %s: backing a texture with the imported memory failed", kSlotName[slot]); return false; }
    GlSetPlainSampling(t.tex);

    GL.GenFramebuffers(1, &t.fbo);
    GL.BindFramebuffer(GL_DRAW_FRAMEBUFFER, t.fbo);
    GL.FramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t.tex, 0);
    const GLenum status = GL.CheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);
    GL.BindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    if (status != GL_FRAMEBUFFER_COMPLETE)
    { Log("[gl] %s: framebuffer incomplete (0x%04X)", kSlotName[slot], status); return false; }

    Log("[gl] %-6s imported as texture %u (%ux%u, internal 0x%04X)",
        kSlotName[slot], t.tex, t.w, t.h, t.fmtgl);
    return true;
}

static bool GlImportSemaphore(GLuint *out, HANDLE h, const char *name)
{
    GL.GenSemaphoresEXT(1, out);
    GL.ImportSemaphoreWin32HandleEXT(*out, GL_HANDLE_TYPE_D3D12_FENCE_EXT, h);
    if (!GlCheck("glImportSemaphoreWin32HandleEXT"))
    { Log("[gl] %s: importing the shared D3D12 fence failed", name); return false; }
    Log("[gl] %s imported as semaphore %u", name, *out);
    return true;
}

// A D3D12 fence semaphore carries its value as a parameter rather than as an
// argument, so it is set immediately before each operation. The texture list
// with GL_LAYOUT_GENERAL_EXT is what tells the driver which images the ordering
// applies to; the shared textures are ALLOW_SIMULTANEOUS_ACCESS on the D3D12
// side, so GENERAL is accurate on both halves and no layout ever changes.
static void GlSignalFence(GLuint sem, UINT64 value, const GLuint *texs, GLuint n)
{
    if (sem == 0 || n > SLOT_COUNT) return;
    GLenum layouts[SLOT_COUNT];
    for (GLuint i = 0; i < n; ++i) layouts[i] = GL_LAYOUT_GENERAL_EXT;
    const GLuint64 v = value;
    GL.SemaphoreParameterui64vEXT(sem, GL_D3D12_FENCE_VALUE_EXT, &v);
    GL.SignalSemaphoreEXT(sem, 0, nullptr, n, texs, layouts);
}

static void GlWaitFence(GLuint sem, UINT64 value, const GLuint *texs, GLuint n)
{
    if (sem == 0 || n > SLOT_COUNT) return;
    GLenum layouts[SLOT_COUNT];
    for (GLuint i = 0; i < n; ++i) layouts[i] = GL_LAYOUT_GENERAL_EXT;
    const GLuint64 v = value;
    GL.SemaphoreParameterui64vEXT(sem, GL_D3D12_FENCE_VALUE_EXT, &v);
    GL.WaitSemaphoreEXT(sem, 0, nullptr, n, texs, layouts);
}

// ---------------------------------------------------------------------------
// the conversion pass
//
// One fullscreen triangle writes both derived inputs at once: raw device depth
// into the shared R32F texture, and reprojected motion vectors into the shared
// RG16F one. They read the same depth texel, so splitting them into two passes
// would read it twice for nothing.
//
// A fragment shader rather than a compute shader on purpose: this is two texels
// of work per pixel and the fragment path needs only GL 3.3, which some of the
// oldest OpenGL titles are the reason to care about.
// ---------------------------------------------------------------------------
static const char kVertSrc[] =
    "#version 330 core\n"
    "out vec2 vUV;\n"
    "void main()\n"
    "{\n"
    // One triangle covering the viewport, positions from gl_VertexID so no
    // vertex buffer is needed and none of the game's is disturbed.
    "    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
    "    vUV = p;\n"
    "    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";

static const char kFragSrc[] =
    "#version 330 core\n"
    "in vec2 vUV;\n"
    "layout(location = 0) out float oDepth;\n"
    "layout(location = 1) out vec2  oMV;\n"
    "uniform sampler2D uDepth;\n"
    "uniform mat4  uReproj;\n"   // prev_viewproj * inverse(curr_viewproj)
    "uniform vec2  uSize;\n"     // render size, so MVs come out in pixels
    "uniform int   uHaveMV;\n"
    "uniform float uDepthScale;\n"
    "void main()\n"
    "{\n"
    "    float d = texture(uDepth, vUV).r;\n"
    // DLSS wants the game's own device depth, not a linearised one: the create
    // flags describe the convention and NGX does the rest. uDepthScale exists
    // only for the missing-depth case, where it is zero.
    "    oDepth = d * uDepthScale;\n"
    "    vec2 mv = vec2(0.0);\n"
    "    if (uHaveMV != 0)\n"
    "    {\n"
    // Window depth is [0,1]; GL clip space is [-1,1] in z, so the NDC the
    // reprojection matrix expects needs the remap. Reconstructing through the
    // combined matrix rather than inverting in the shader keeps the inverse on
    // the CPU, where it is done once per frame in double precision.
    "        vec4 ndc  = vec4(vUV * 2.0 - 1.0, d * 2.0 - 1.0, 1.0);\n"
    "        vec4 prev = uReproj * ndc;\n"
    "        if (abs(prev.w) > 1e-9)\n"
    "        {\n"
    "            vec2 prev_uv = (prev.xy / prev.w) * 0.5 + 0.5;\n"
    // DLSS reads a motion vector as \"where this pixel was\", so the difference
    // points backwards in time. Pixels, because MV.Scale is then 1,1 and there
    // is no scale convention left to get wrong.
    "            mv = (prev_uv - vUV) * uSize;\n"
    "        }\n"
    "    }\n"
    "    oMV = mv;\n"
    "}\n";

static GLuint GlCompile(GLenum stage, const char *src, const char *what)
{
    GLuint s = GL.CreateShader(stage);
    const GLchar *p = src;
    GL.ShaderSource(s, 1, &p, nullptr);
    GL.CompileShader(s);
    GLint ok = 0;
    GL.GetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok)
    {
        char log[1024] = {};
        if (GL.GetShaderInfoLog) GL.GetShaderInfoLog(s, sizeof(log) - 1, nullptr, log);
        Log("[gl] %s shader failed to compile: %s", what, log);
        GL.DeleteShader(s);
        return 0;
    }
    return s;
}

static bool GlMakeConvertProgram()
{
    if (g_bridge.prog_convert) return true;

    GLuint vs = GlCompile(GL_VERTEX_SHADER, kVertSrc, "vertex");
    if (vs == 0) return false;
    GLuint fs = GlCompile(GL_FRAGMENT_SHADER, kFragSrc, "fragment");
    if (fs == 0) { GL.DeleteShader(vs); return false; }

    GLuint p = GL.CreateProgram();
    GL.AttachShader(p, vs);
    GL.AttachShader(p, fs);
    GL.LinkProgram(p);
    GL.DeleteShader(vs);
    GL.DeleteShader(fs);

    GLint ok = 0;
    GL.GetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok)
    {
        char log[1024] = {};
        if (GL.GetProgramInfoLog) GL.GetProgramInfoLog(p, sizeof(log) - 1, nullptr, log);
        Log("[gl] depth/MV program failed to link: %s", log);
        GL.DeleteProgram(p);
        return false;
    }

    g_bridge.prog_convert  = p;
    g_bridge.u_depth       = GL.GetUniformLocation(p, "uDepth");
    g_bridge.u_reproj      = GL.GetUniformLocation(p, "uReproj");
    g_bridge.u_size        = GL.GetUniformLocation(p, "uSize");
    g_bridge.u_have_mv     = GL.GetUniformLocation(p, "uHaveMV");
    g_bridge.u_depth_scale = GL.GetUniformLocation(p, "uDepthScale");
    Log("[gl] depth/MV program ready (program %u)", p);
    return true;
}

// ---------------------------------------------------------------------------
// blits
// ---------------------------------------------------------------------------

// A colour blit reads the read framebuffer's read buffer and writes every draw
// buffer the draw framebuffer has enabled. Neither of those is state the bridge
// may assume: a deferred renderer's scene target is a multiple-render-target
// G-buffer, and blitting into it without saying which attachment is meant would
// write the neural result over the normals and the material buffer as well as
// over the colour. So both are set explicitly, and both are put back -- they are
// per-framebuffer state, which the state guard does not and cannot cover.
static const int kMaxDrawBuffers = 8;

struct GlBufferSelect
{
    GLuint src_fbo, dst_fbo;
    GLint  prev_read;
    GLint  prev_draw[kMaxDrawBuffers];
    GLint  draw_count;

    void Apply(GLuint src, GLuint dst)
    {
        src_fbo = src; dst_fbo = dst; draw_count = 0; prev_read = 0;

        GL.BindFramebuffer(GL_READ_FRAMEBUFFER, src);
        GL.GetIntegerv(GL_READ_BUFFER, &prev_read);
        const GLenum want_read = src ? GL_COLOR_ATTACHMENT0 : GL_BACK;
        if (GL.ReadBuffer && (GLenum)prev_read != want_read) GL.ReadBuffer(want_read);

        GL.BindFramebuffer(GL_DRAW_FRAMEBUFFER, dst);
        if (dst == 0)
        {
            GL.GetIntegerv(GL_DRAW_BUFFER, &prev_draw[0]);
            draw_count = 1;
            if (GL.DrawBuffer && (GLenum)prev_draw[0] != GL_BACK) GL.DrawBuffer(GL_BACK);
        }
        else
        {
            GLint max_bufs = 1;
            GL.GetIntegerv(GL_MAX_DRAW_BUFFERS, &max_bufs);
            if (max_bufs > kMaxDrawBuffers) max_bufs = kMaxDrawBuffers;
            if (max_bufs < 1) max_bufs = 1;
            for (GLint i = 0; i < max_bufs; ++i)
                GL.GetIntegerv(GL_DRAW_BUFFER0 + i, &prev_draw[i]);
            draw_count = max_bufs;
            const GLenum one[] = { GL_COLOR_ATTACHMENT0 };
            GL.DrawBuffers(1, one);
        }
        GlDrainErrors();
    }

    void Restore()
    {
        if (draw_count <= 0) return;
        GL.BindFramebuffer(GL_DRAW_FRAMEBUFFER, dst_fbo);
        if (dst_fbo == 0)
        {
            if (GL.DrawBuffer) GL.DrawBuffer((GLenum)prev_draw[0]);
        }
        else
        {
            GLenum bufs[kMaxDrawBuffers];
            for (GLint i = 0; i < draw_count; ++i) bufs[i] = (GLenum)prev_draw[i];
            GL.DrawBuffers(draw_count, bufs);
        }
        GL.BindFramebuffer(GL_READ_FRAMEBUFFER, src_fbo);
        if (GL.ReadBuffer) GL.ReadBuffer((GLenum)prev_read);
        GlDrainErrors();
        draw_count = 0;
    }
};

// LINEAR where the sizes differ and the format can filter, NEAREST otherwise --
// and always NEAREST out of a multisampled source, which the spec allows no
// other way.
static void GlBlitColor(GLuint src_fbo, UINT sw, UINT sh,
                        GLuint dst_fbo, UINT dw, UINT dh, bool src_multisample)
{
    const bool scaled = (sw != dw || sh != dh);
    const GLenum filter = (scaled && !src_multisample) ? GL_LINEAR : GL_NEAREST;

    GlBufferSelect sel;
    sel.Apply(src_fbo, dst_fbo);
    GL.BlitFramebuffer(0, 0, (GLint)sw, (GLint)sh, 0, 0, (GLint)dw, (GLint)dh,
                       GL_COLOR_BUFFER_BIT, filter);
    sel.Restore();
}
