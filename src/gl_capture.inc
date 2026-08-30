// Where the frame comes from, and how the bridge gets to see it.
//
// The DX11 and Vulkan bridges are handed everything: the game calls NGX with a
// parameter block naming its Color, Depth, MotionVectors and Output, and reading
// that block is the whole of their capture problem. Nothing calls anything here,
// so this file has to find the frame.
//
// Two interception techniques, chosen per function by how often it is called:
//
//   wglSwapBuffers / SwapBuffers / wglGetProcAddress -- patched at the entry
//     point with a 14-byte absolute jump, the same technique both other bridges
//     use on the NGX exports. The original bytes go back around every forwarded
//     call. These are called once a frame (or once a session), so the cost of
//     restoring and reinstalling around each one does not matter.
//
//   glBindFramebuffer / glUseProgram / glUniformMatrix4fv / glGetUniformLocation
//     -- not patched at all. Everything above OpenGL 1.1 reaches a game through
//     wglGetProcAddress, so the hook goes there: the game asks for the function
//     and is handed one of ours, which calls the driver's. That is one indirect
//     call of overhead instead of a restore/reinstall pair, which is what makes
//     it affordable on glUniformMatrix4fv -- a function a busy frame calls
//     thousands of times, and where entry-point patching would cost more than
//     the neural pass it is here to enable.
//
// Nothing the bridge itself calls goes through those wrappers: GlGet resolves
// against the unpatched wglGetProcAddress, so the tracker never sees the
// bridge's own binds and never mistakes the bridge's program for the game's.

// ---------------------------------------------------------------------------
// entry-point patching
// ---------------------------------------------------------------------------
struct Hook
{
    void         *target;
    void         *detour;
    unsigned char orig[14];
    bool          installed;
};

static CRITICAL_SECTION g_hook_cs;

static void WriteCode(void *dst, const void *src, size_t n)
{
    DWORD old = 0;
    if (VirtualProtect(dst, n, PAGE_EXECUTE_READWRITE, &old))
    {
        memcpy(dst, src, n);
        VirtualProtect(dst, n, old, &old);
        FlushInstructionCache(GetCurrentProcess(), dst, n);
    }
}

static void BuildJump(unsigned char code[14], void *dst)
{
    code[0] = 0xFF; code[1] = 0x25;               // jmp qword ptr [rip+0]
    code[2] = code[3] = code[4] = code[5] = 0x00;
    memcpy(code + 6, &dst, 8);
}

static bool HookInstall(Hook *h, void *target, void *detour)
{
    if (target == nullptr || reinterpret_cast<uintptr_t>(target) < 0x10000) return false;
    h->target = target;
    h->detour = detour;
    memcpy(h->orig, target, 14);
    unsigned char jmp[14];
    BuildJump(jmp, detour);
    WriteCode(target, jmp, 14);
    h->installed = true;
    return true;
}

static void HookRestore(Hook *h)   { if (h->installed) WriteCode(h->target, h->orig, 14); }
static void HookReinstall(Hook *h) { if (h->installed) { unsigned char j[14]; BuildJump(j, h->detour); WriteCode(h->target, j, 14); } }

static Hook g_hook_swap;        // wglSwapBuffers, in opengl32.dll
static Hook g_hook_gdi_swap;    // SwapBuffers, in gdi32.dll
static Hook g_hook_getproc;     // wglGetProcAddress, in opengl32.dll

// Defined in gl_bridge.inc: the frame trigger, which has to be reachable from
// the framebuffer wrapper below because in scene mode that is where the frame
// ends -- see the comment on the trigger itself.
static void BridgeOnFramebufferBind(GLenum target, GLuint prev_fbo, GLuint new_fbo);

// ---------------------------------------------------------------------------
// the wglGetProcAddress hook
//
// The bridge resolves its own entry points through this, with the patch lifted,
// so it always reaches the driver rather than one of the wrappers below. Without
// that, the bridge's own glBindFramebuffer calls would land in its own tracker
// and its own glUseProgram would be mistaken for the game's.
// ---------------------------------------------------------------------------
static PROC WINAPI RealGetProcAddress(LPCSTR name)
{
    if (!g_hook_getproc.installed)
        return g_wglGetProcAddress ? g_wglGetProcAddress(name) : nullptr;

    EnterCriticalSection(&g_hook_cs);
    HookRestore(&g_hook_getproc);
    PROC p = reinterpret_cast<PFN_wglGetProcAddress>(g_hook_getproc.target)(name);
    HookReinstall(&g_hook_getproc);
    LeaveCriticalSection(&g_hook_cs);
    return p;
}

// The driver's real functions, behind the wrappers the game is handed.
static void  (__stdcall *g_real_BindFramebuffer)(GLenum, GLuint);
static void  (__stdcall *g_real_UseProgram)(GLuint);
static void  (__stdcall *g_real_UniformMatrix4fv)(GLint, GLsizei, GLboolean, const GLfloat *);
static GLint (__stdcall *g_real_GetUniformLocation)(GLuint, const GLchar *);

static void GlNoteFramebufferBind(GLenum target, GLuint fbo);   // below

// The wrappers are handed out unconditionally rather than only when the config
// asks for them. A game resolves its entry points once, at load, long before
// anyone edits a config file -- gating them on a setting would mean the setting
// could never be turned on again without restarting the game. The cost of an
// unwanted wrapper is one indirect call and one comparison against an empty
// list, which is small enough that the alternative is not worth its rigidity.
static GLuint g_last_bound_fbo;

static void __stdcall Wrap_BindFramebuffer(GLenum target, GLuint fbo)
{
    const GLuint prev = g_last_bound_fbo;
    g_real_BindFramebuffer(target, fbo);
    GlNoteFramebufferBind(target, fbo);
    BridgeOnFramebufferBind(target, prev, fbo);
}

static void __stdcall Wrap_UseProgram(GLuint program)
{
    g_real_UseProgram(program);
    MotionOnUseProgram(program);
}

static void __stdcall Wrap_UniformMatrix4fv(GLint loc, GLsizei count, GLboolean transpose, const GLfloat *value)
{
    g_real_UniformMatrix4fv(loc, count, transpose, value);
    MotionOnUniformMatrix4fv(loc, count, transpose, value);
}

static GLint __stdcall Wrap_GetUniformLocation(GLuint program, const GLchar *name)
{
    const GLint loc = g_real_GetUniformLocation(program, name);
    MotionOnGetUniformLocation(program, name, loc);
    return loc;
}

static PROC WINAPI Detour_wglGetProcAddress(LPCSTR name)
{
    PROC real = RealGetProcAddress(name);
    if (real == nullptr || name == nullptr) return real;

    #define WRAP(n, slot, fn) \
        if (strcmp(name, n) == 0) { \
            if (slot == nullptr) *reinterpret_cast<void **>(&slot) = reinterpret_cast<void *>(real); \
            return reinterpret_cast<PROC>(fn); }

    WRAP("glBindFramebuffer",    g_real_BindFramebuffer,    Wrap_BindFramebuffer)
    WRAP("glBindFramebufferEXT", g_real_BindFramebuffer,    Wrap_BindFramebuffer)
    WRAP("glUseProgram",         g_real_UseProgram,         Wrap_UseProgram)
    WRAP("glUniformMatrix4fv",   g_real_UniformMatrix4fv,   Wrap_UniformMatrix4fv)
    WRAP("glGetUniformLocation", g_real_GetUniformLocation, Wrap_GetUniformLocation)
    #undef WRAP
    return real;
}

// ---------------------------------------------------------------------------
// the first-draw hook
//
// The fixed-function matrix stack has to be read at the instant the camera is on
// it, which is the frame's first world draw. By the swap it holds whatever the
// interface pass left, and at a framebuffer bind it holds nothing in particular.
//
// glDrawArrays, glDrawElements and glBegin are OpenGL 1.1, so they are real
// opengl32.dll exports and reachable only by patching the entry point -- the
// wglGetProcAddress route does not see them. Patching a function a busy frame
// calls ten thousand times would be ruinous, so the hook takes itself out: the
// first call captures the matrices, puts the original bytes back for good, and
// forwards. It is re-armed once per frame. The cost is two patch operations and
// one forwarded call per frame, whatever the game's draw count.
//
// It is armed only when there is something to gain -- a compatibility context,
// with no matrix uniform already supplying a better answer. A modern game pays
// nothing at all.
// ---------------------------------------------------------------------------
static Hook g_hook_draw_arrays;
static Hook g_hook_draw_elements;
static Hook g_hook_begin;
static bool g_draw_armed;

typedef void (WINAPI *PFN_glDrawArrays)(GLenum, GLint, GLsizei);
typedef void (WINAPI *PFN_glDrawElements)(GLenum, GLsizei, GLenum, const void *);
typedef void (WINAPI *PFN_glBegin)(GLenum);

static void DisarmFirstDraw()
{
    if (!g_draw_armed) return;
    g_draw_armed = false;
    HookRestore(&g_hook_draw_arrays);
    HookRestore(&g_hook_draw_elements);
    HookRestore(&g_hook_begin);
}

// The capture, then the disarm, then the forward -- in that order, because the
// forward has to run with the patch already gone or it would come straight back
// through the detour.
static void FirstDrawSeen()
{
    EnterCriticalSection(&g_hook_cs);
    if (g_draw_armed) { MotionOnSceneDraw(); DisarmFirstDraw(); }
    LeaveCriticalSection(&g_hook_cs);
}

static void WINAPI Detour_glDrawArrays(GLenum mode, GLint first, GLsizei count)
{
    void *target = g_hook_draw_arrays.target;
    FirstDrawSeen();
    reinterpret_cast<PFN_glDrawArrays>(target)(mode, first, count);
}

static void WINAPI Detour_glDrawElements(GLenum mode, GLsizei count, GLenum type, const void *indices)
{
    void *target = g_hook_draw_elements.target;
    FirstDrawSeen();
    reinterpret_cast<PFN_glDrawElements>(target)(mode, count, type, indices);
}

static void WINAPI Detour_glBegin(GLenum mode)
{
    void *target = g_hook_begin.target;
    FirstDrawSeen();
    reinterpret_cast<PFN_glBegin>(target)(mode);
}

// Once this has produced nothing for long enough, the patching stops for good.
// A game whose matrix stack is identity -- which is every shader-based engine --
// will never produce anything, and going on rewriting three hot functions once a
// frame for the rest of the session buys nothing and risks everything.
static bool  g_draw_hook_retired;
static UINT64 g_draw_arm_count;

static void RetireFirstDraw(const char *why)
{
    if (g_draw_hook_retired) return;
    g_draw_hook_retired = true;
    EnterCriticalSection(&g_hook_cs);
    DisarmFirstDraw();
    g_hook_draw_arrays.installed = g_hook_draw_elements.installed = g_hook_begin.installed = false;
    LeaveCriticalSection(&g_hook_cs);
    Log("[hook] first-draw watch retired: %s. The draw entry points are back to the "
        "driver's own and will not be patched again this session.", why);
}

static void ArmFirstDraw()
{
    if (g_draw_armed || g_draw_hook_retired || g_opengl32 == nullptr) return;
    if (!MotionWantsFirstDraw()) return;

    if (++g_draw_arm_count > 300)
    { RetireFirstDraw("300 frames with no usable matrix on the fixed-function stack"); return; }

    EnterCriticalSection(&g_hook_cs);
    if (g_hook_draw_arrays.target == nullptr)
    {
        void *a = reinterpret_cast<void *>(GetProcAddress(g_opengl32, "glDrawArrays"));
        void *e = reinterpret_cast<void *>(GetProcAddress(g_opengl32, "glDrawElements"));
        void *b = reinterpret_cast<void *>(GetProcAddress(g_opengl32, "glBegin"));
        bool any = false;
        if (a && HookInstall(&g_hook_draw_arrays,   a, reinterpret_cast<void *>(Detour_glDrawArrays)))   any = true;
        if (e && HookInstall(&g_hook_draw_elements, e, reinterpret_cast<void *>(Detour_glDrawElements))) any = true;
        if (b && HookInstall(&g_hook_begin,         b, reinterpret_cast<void *>(Detour_glBegin)))        any = true;
        if (any)
        {
            Log("[hook] mv=1: watching the first draw of each frame for the "
                "fixed-function matrix stack (glDrawArrays %p, glDrawElements %p, "
                "glBegin %p)", a, e, b);
            Warn("mv = 1 rewrites the first bytes of glDrawArrays, glDrawElements and "
                 "glBegin once per frame. That is only safe while the game issues GL "
                 "calls from one thread. If it crashes on a loading screen, set mv = 0.");
        }
    }
    else
    {
        HookReinstall(&g_hook_draw_arrays);
        HookReinstall(&g_hook_draw_elements);
        HookReinstall(&g_hook_begin);
    }
    g_draw_armed = g_hook_draw_arrays.installed || g_hook_draw_elements.installed ||
                   g_hook_begin.installed;
    LeaveCriticalSection(&g_hook_cs);
}

// ---------------------------------------------------------------------------
// framebuffer tracking
//
// Every distinct framebuffer the game bound for drawing this frame, in bind
// order. At the swap the set is inspected once and the scene target picked out
// of it; the decision is then cached, because a game that renders the same way
// every frame does not need answering again every frame.
// ---------------------------------------------------------------------------
static const int kMaxTracked = 32;
static GLuint    g_seen_fbo[kMaxTracked];
static int       g_seen_count;

static void GlNoteFramebufferBind(GLenum target, GLuint fbo)
{
    if (target != GL_FRAMEBUFFER && target != GL_DRAW_FRAMEBUFFER) return;
    g_last_bound_fbo = fbo;
    if (fbo == 0) return;
    for (int i = 0; i < g_seen_count; ++i) if (g_seen_fbo[i] == fbo) return;
    if (g_seen_count < kMaxTracked) g_seen_fbo[g_seen_count++] = fbo;
}

static void GlResetFrameTracking() { g_seen_count = 0; }

// What one framebuffer looks like from the outside: the size and format of its
// first colour attachment, and whether it carries depth at all.
struct FboInfo
{
    bool   valid;
    UINT   w, h;
    GLenum color_fmt;
    bool   has_color, has_depth;
    GLenum depth_fmt;       // the attachment's own sized internal format
    GLint  samples;
};

static bool AttachmentSize(GLenum fbo_target, GLenum attachment, UINT *w, UINT *h,
                           GLenum *fmt, GLint *samples)
{
    GLint type = GL_NONE, name = 0;
    GL.GetFramebufferAttachmentParameteriv(fbo_target, attachment,
                                           GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &type);
    if (type == GL_NONE) return false;
    GL.GetFramebufferAttachmentParameteriv(fbo_target, attachment,
                                           GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &name);

    GLint gw = 0, gh = 0, gf = 0, gs = 0;
    if (type == GL_TEXTURE && GL.GetTexLevelParameteriv)
    {
        // Try the plain 2D target, then the multisample one. A game that renders
        // into an MSAA texture rather than an MSAA renderbuffer answers nothing
        // on GL_TEXTURE_2D, and reading that as "no colour attachment" would
        // quietly drop the one framebuffer worth bridging.
        static const GLenum targets[] = { GL_TEXTURE_2D, GL_TEXTURE_2D_MULTISAMPLE };
        static const GLenum bindings[] = { GL_TEXTURE_BINDING_2D, GL_TEXTURE_BINDING_2D_MULTISAMPLE };
        for (int t = 0; t < 2 && gw <= 0; ++t)
        {
            GLint prev = 0;
            GL.GetIntegerv(bindings[t], &prev);
            GlDrainErrors();
            GL.BindTexture(targets[t], (GLuint)name);
            GL.GetTexLevelParameteriv(targets[t], 0, GL_TEXTURE_WIDTH, &gw);
            GL.GetTexLevelParameteriv(targets[t], 0, GL_TEXTURE_HEIGHT, &gh);
            GL.GetTexLevelParameteriv(targets[t], 0, GL_TEXTURE_INTERNAL_FORMAT, &gf);
            if (t == 1) GL.GetTexLevelParameteriv(targets[t], 0, GL_TEXTURE_SAMPLES, &gs);
            GL.BindTexture(targets[t], (GLuint)prev);
            GlDrainErrors();
        }
    }
    else if (type == GL_RENDERBUFFER && GL.BindRenderbuffer && GL.GetRenderbufferParameteriv)
    {
        GL.BindRenderbuffer(GL_RENDERBUFFER, (GLuint)name);
        GL.GetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_WIDTH, &gw);
        GL.GetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_HEIGHT, &gh);
        GL.GetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_INTERNAL_FORMAT, &gf);
        GL.GetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_SAMPLES, &gs);
        GL.BindRenderbuffer(GL_RENDERBUFFER, 0);
    }
    GlDrainErrors();
    if (gw <= 0 || gh <= 0) return false;
    if (w) *w = (UINT)gw;
    if (h) *h = (UINT)gh;
    if (fmt) *fmt = (GLenum)gf;
    if (samples) *samples = gs;
    return true;
}

static bool DepthFormatIsKnown(GLenum f)
{
    return f == GL_DEPTH_COMPONENT16 || f == GL_DEPTH_COMPONENT24 ||
           f == GL_DEPTH_COMPONENT32 || f == GL_DEPTH_COMPONENT32F ||
           f == GL_DEPTH24_STENCIL8  || f == GL_DEPTH32F_STENCIL8;
}

// The default framebuffer has no attachment object to ask for a format, only
// component sizes. Depth and stencil sizes together name the format the driver
// gave the pixel format, which is what the copy has to match.
static GLenum DepthFormatFromSizes(GLint depth_bits, GLint stencil_bits)
{
    if (depth_bits <= 0) return 0;
    if (stencil_bits > 0) return (depth_bits > 24) ? GL_DEPTH32F_STENCIL8 : GL_DEPTH24_STENCIL8;
    if (depth_bits <= 16) return GL_DEPTH_COMPONENT16;
    if (depth_bits <= 24) return GL_DEPTH_COMPONENT24;
    return GL_DEPTH_COMPONENT32F;
}

// Describes one framebuffer. Leaves the read binding on `fbo`; the caller is
// inside a GlStateGuard, which puts the game's bindings back.
static FboInfo InspectFbo(GLuint fbo)
{
    FboInfo r = {};
    GL.BindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    if (GL.GetError && GL.GetError() != GL_NO_ERROR) return r;

    if (fbo == 0)
    {
        // The default framebuffer has no attachment objects to query, so its
        // size comes from the window and its depth from the pixel format.
        RECT rc = {};
        HWND wnd = g_bridge.hdc ? WindowFromDC(g_bridge.hdc) : nullptr;
        if (wnd && GetClientRect(wnd, &rc) && rc.right > 0 && rc.bottom > 0)
        { r.w = (UINT)rc.right; r.h = (UINT)rc.bottom; }
        else
        {
            GLint vp[4] = {};
            GL.GetIntegerv(GL_VIEWPORT, vp);
            if (vp[2] <= 0 || vp[3] <= 0) return r;
            r.w = (UINT)vp[2]; r.h = (UINT)vp[3];
        }
        r.has_color = true;
        r.color_fmt = 0;              // unknowable, and it does not need to be:
                                      // the shared Color texture is RGBA16F
                                      // whatever the back buffer turns out to be
        GLint depth_bits = 0, stencil_bits = 0;
        GL.GetFramebufferAttachmentParameteriv(GL_READ_FRAMEBUFFER, GL_DEPTH,
                                               GL_FRAMEBUFFER_ATTACHMENT_DEPTH_SIZE, &depth_bits);
        GlDrainErrors();
        GL.GetFramebufferAttachmentParameteriv(GL_READ_FRAMEBUFFER, GL_STENCIL,
                                               GL_FRAMEBUFFER_ATTACHMENT_STENCIL_SIZE, &stencil_bits);
        GlDrainErrors();
        r.depth_fmt = DepthFormatFromSizes(depth_bits, stencil_bits);
        r.has_depth = r.depth_fmt != 0;
        GLint samples = 0;
        GL.GetIntegerv(GL_SAMPLES, &samples);
        GlDrainErrors();
        r.samples = samples;
        r.valid   = true;
        return r;
    }

    GLenum cfmt = 0;
    GLint  csamples = 0;
    r.has_color = AttachmentSize(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, &r.w, &r.h, &cfmt, &csamples);
    r.color_fmt = cfmt;
    r.samples   = csamples;
    if (!r.has_color) return r;

    GLenum dfmt = 0;
    UINT dw = 0, dh = 0;
    const bool has = AttachmentSize(GL_READ_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, &dw, &dh, &dfmt, nullptr) ||
                     AttachmentSize(GL_READ_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, &dw, &dh, &dfmt, nullptr);
    // A format this build has no name for is exactly the one a report needs to
    // identify, so it is carried through as-is rather than mapped onto a guess:
    // the copy is created with the same value, and if the driver will not accept
    // it the depth blit fails loudly instead of silently copying the wrong bits.
    r.has_depth = has && dfmt != 0;
    r.depth_fmt = dfmt;
    if (r.has_depth && !DepthFormatIsKnown(dfmt))
    {
        static GLenum said;
        if (said != dfmt) { said = dfmt; Log("[gl] depth attachment is internal format "
                                             "0x%04X, which this build has no name for; "
                                             "using it verbatim for the copy", dfmt); }
    }
    r.valid = true;
    return r;
}

static bool ColorFormatIsFloat(GLenum f)
{
    return f == GL_RGBA16F || f == GL_RGB16F || f == GL_RGBA32F || f == GL_R11F_G11F_B10F;
}

// ---------------------------------------------------------------------------
// picking the scene target
//
// source = 0 (backbuffer): the default framebuffer, whatever is about to be
//   presented. Always available, needs no tracking, and includes the UI -- see
//   the README on what that costs.
//
// source = 1 (scene): the largest framebuffer the game drew into this frame
//   that carries depth. In an engine that renders the world offscreen and then
//   composites the interface over it, that is the world, and taking it instead
//   of the back buffer is what keeps the interface out of the neural pass. In an
//   emulator it is the internal-resolution target, which is also the one worth
//   upscaling.
//
// source >= 2: that exact framebuffer name, for when the heuristic picks wrong
//   and the log has already said which names were on offer.
// ---------------------------------------------------------------------------
static GLuint    g_scene_fbo;
static ULONGLONG g_scene_decided_at;
static bool      g_scene_reported;

static bool ChooseSource(GlSource *out)
{
    // Inspecting a framebuffer means binding it, and binding a texture and a
    // renderbuffer to ask their size. All three are the game's state, and this
    // runs in the middle of the game's frame -- so it carries its own guard
    // rather than relying on the caller to have one. BridgeRunFrame makes a
    // second guard of its own; two sequential guards cost two rounds of queries
    // and cannot leave anything of the bridge's behind, which is the trade the
    // right way round.
    GlStateGuard guard;
    if (!guard.ok) return false;

    const bool rescan = g_scene_fbo == 0 || g_bridge.frames_done < 2 ||
                        (g_bridge.frames_done % 120) == 0;

    if (g_cfg.source >= 2)
        g_scene_fbo = (GLuint)g_cfg.source;
    else if (g_cfg.source == 0)
        g_scene_fbo = 0;
    else if (rescan)
    {
        GLuint best = 0;
        UINT   best_area = 0;
        for (int i = 0; i < g_seen_count; ++i)
        {
            const FboInfo f = InspectFbo(g_seen_fbo[i]);
            if (!f.valid || !f.has_color || !f.has_depth) continue;
            const UINT area = f.w * f.h;
            if (area >= best_area) { best_area = area; best = g_seen_fbo[i]; }
        }
        if (best == 0)
        {
            static bool said = false;
            if (!said)
            {
                said = true;
                Log("[gl] source=1 was asked for but no framebuffer with both colour and "
                    "depth was drawn into this frame; using the back buffer. The %d "
                    "framebuffers the game did bind are listed below.", g_seen_count);
                for (int i = 0; i < g_seen_count; ++i)
                {
                    const FboInfo f = InspectFbo(g_seen_fbo[i]);
                    Log("  fbo %-4u %ux%u colour=%d depth=%d fmt=0x%04X",
                        g_seen_fbo[i], f.w, f.h, (int)f.has_color, (int)f.has_depth, f.color_fmt);
                }
            }
        }
        if (best != g_scene_fbo)
        {
            g_scene_fbo = best;
            g_scene_decided_at = GetTickCount64();
            g_scene_reported = false;
        }
    }

    FboInfo f = InspectFbo(g_scene_fbo);
    if (!f.valid || !f.has_color || f.w == 0 || f.h == 0)
    {
        // A named framebuffer that turns out not to exist, or one the game
        // deleted between frames. Fall back to the back buffer, which is always
        // there -- and do it here rather than by calling back into this
        // function, which with source >= 2 would re-choose the same dead name.
        if (g_scene_fbo != 0)
        {
            static GLuint said_for;
            if (said_for != g_scene_fbo)
            {
                said_for = g_scene_fbo;
                Log("[gl] framebuffer %u is not usable as a scene target; falling back "
                    "to the back buffer", g_scene_fbo);
            }
            g_scene_fbo = 0;
            g_scene_reported = false;
            f = InspectFbo(0);
        }
        if (!f.valid || !f.has_color || f.w == 0 || f.h == 0) return false;
    }

    out->fbo            = g_scene_fbo;
    out->w              = f.w;
    out->h              = f.h;
    out->color_fmt      = f.color_fmt;
    out->color_is_float = ColorFormatIsFloat(f.color_fmt);
    out->has_depth      = f.has_depth;
    out->depth_fmt      = f.depth_fmt;
    out->samples        = f.samples;

    if (!g_scene_reported)
    {
        g_scene_reported = true;
        Log("[gl] scene target: framebuffer %u, %ux%u, colour format 0x%04X (%s), "
            "depth %s (format 0x%04X), %d sample%s",
            out->fbo, out->w, out->h, out->color_fmt,
            out->color_is_float ? "floating point" : "fixed point or unknown",
            out->has_depth ? "present" : "ABSENT", out->depth_fmt,
            out->samples ? out->samples : 1, (out->samples > 1) ? "s" : "");
        if (!out->has_depth)
            Warn("the scene target has no depth buffer, so DLSS runs without depth. It "
                 "will hold on to the previous frame where it should not -- expect "
                 "ghosting. source=1 in the config looks for a target that has one.");
        if (out->samples > 1)
            Log("[gl] the source is multisampled; it is resolved on the way into the "
                "bridge. Turning MSAA off in the game costs nothing here and saves the "
                "resolve -- DLSS is doing the antialiasing now.");
    }
    return true;
}

// ---------------------------------------------------------------------------
// the depth copy
//
// The game's depth is never sampled where it lies. A depth texture the game left
// in comparison-sampling mode reads back undefined through an ordinary
// sampler2D, and the only fix would be to write to the game's texture state.
// Blitting into a texture the bridge owns costs one blit and cannot corrupt
// anything. It also handles the two cases where there is nothing to sample at
// all: a renderbuffer attachment, and the default framebuffer's own depth.
//
// The blit is depth-to-depth, so the copy must carry the source's exact format
// -- glBlitFramebuffer refuses a depth blit between different ones.
// ---------------------------------------------------------------------------
static bool DepthFormatIsPacked(GLenum f)
{
    return f == GL_DEPTH24_STENCIL8 || f == GL_DEPTH32F_STENCIL8;
}

static void GlReleaseDepthCopy()
{
    if (g_bridge.depth_copy_fbo) { GL.DeleteFramebuffers(1, &g_bridge.depth_copy_fbo); g_bridge.depth_copy_fbo = 0; }
    if (g_bridge.depth_copy_tex) { GL.DeleteTextures(1, &g_bridge.depth_copy_tex);     g_bridge.depth_copy_tex = 0; }
    g_bridge.depth_copy_w = g_bridge.depth_copy_h = 0;
    g_bridge.depth_copy_fmt = 0;
}

static bool GlEnsureDepthCopy(const GlSource &src)
{
    if (!src.has_depth || src.depth_fmt == 0) { GlReleaseDepthCopy(); return false; }

    const GLenum want = src.depth_fmt;
    if (g_bridge.depth_copy_tex && g_bridge.depth_copy_w == src.w &&
        g_bridge.depth_copy_h == src.h && g_bridge.depth_copy_fmt == want) return true;

    GlReleaseDepthCopy();

    GL.GenTextures(1, &g_bridge.depth_copy_tex);
    GL.BindTexture(GL_TEXTURE_2D, g_bridge.depth_copy_tex);
    const bool packed = DepthFormatIsPacked(want);
    GL.TexImage2D(GL_TEXTURE_2D, 0, (GLint)want, (GLsizei)src.w, (GLsizei)src.h, 0,
                  packed ? GL_DEPTH_STENCIL : GL_DEPTH_COMPONENT,
                  packed ? GL_UNSIGNED_INT_24_8 : GL_FLOAT, nullptr);
    if (!GlCheck("depth copy glTexImage2D")) { GlReleaseDepthCopy(); return false; }
    GlSetPlainSampling(g_bridge.depth_copy_tex);

    GL.GenFramebuffers(1, &g_bridge.depth_copy_fbo);
    GL.BindFramebuffer(GL_DRAW_FRAMEBUFFER, g_bridge.depth_copy_fbo);
    GL.FramebufferTexture2D(GL_DRAW_FRAMEBUFFER,
                            packed ? GL_DEPTH_STENCIL_ATTACHMENT : GL_DEPTH_ATTACHMENT,
                            GL_TEXTURE_2D, g_bridge.depth_copy_tex, 0);
    const GLenum none[] = { GL_NONE };
    GL.DrawBuffers(1, none);
    const GLenum status = GL.CheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE)
    {
        Log("[gl] depth copy framebuffer incomplete (0x%04X) for format 0x%04X", status, want);
        GlReleaseDepthCopy();
        return false;
    }

    g_bridge.depth_copy_w   = src.w;
    g_bridge.depth_copy_h   = src.h;
    g_bridge.depth_copy_fmt = want;
    Log("[gl] depth copy ready: %ux%u, internal format 0x%04X", src.w, src.h, want);
    return true;
}
