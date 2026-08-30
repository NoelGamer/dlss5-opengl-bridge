// One frame, end to end.
//
// The whole sequence runs inline on the game's GL context, in the middle of the
// game's frame, at the point where the scene is finished and nothing has read it
// yet. That placement is the same lesson the Vulkan port paid for: a copy-back
// submitted after the game has already consumed its own image lands on pixels
// nobody will ever look at again. Here it is enforced by construction -- the
// bridge runs at the moment the game stops drawing into the scene target (or,
// in back-buffer mode, immediately before the swap), so the copy-back is
// always ahead of whatever reads next.
//
//   blit  scene colour ---------------------------> shared Color   (RGBA16F)
//   blit  scene depth  --> depth copy
//   draw  depth copy + reprojection ---------------> shared Depth   (R32F)
//                                              and > shared MV      (RG16F)
//   glSignalSemaphoreEXT(sem_in, v) + glFlush
//        D3D12 queue: Wait(fence_in, v) -> evaluate -> Signal(fence_out, v)
//              a DLSS 5 add-on inserts its neural pass in that evaluate
//   glWaitSemaphoreEXT(sem_out, v)
//   blit  shared Output --------------------------> scene colour

// ---------------------------------------------------------------------------
// the contract
//
// The DX11 and Vulkan bridges read every one of these out of the game's own NGX
// parameter block, because the game is the only party that knows how its buffers
// are laid out. Here the bridge laid them out, so it states them. Each line
// below is answering a question about a buffer this file created.
// ---------------------------------------------------------------------------
static UINT BuildCreateFlags(const GlSource &src)
{
    if (g_cfg.flags >= 0) return (UINT)g_cfg.flags;

    UINT f = 0;

    // AutoExposure, always. The exposure input is the one texture the bridge
    // has no way to produce: an OpenGL game that is not doing its own temporal
    // pass has no exposure buffer, and inventing one is worse than letting NGX
    // measure it. This is also the flag Bannerlord's report in the DX11 bridge
    // showed the cost of getting wrong -- DLSS accumulating without knowing the
    // exposure is ghosting.
    f |= NGX_DLSS_FLAG_AUTO_EXPOSURE;

    // IsHDR describes the colour buffer, and the colour buffer is whatever the
    // game rendered into. A floating-point scene target is carrying values
    // outside [0,1]; an 8-bit back buffer is not.
    if (g_cfg.hdr > 0 || (g_cfg.hdr < 0 && src.color_is_float)) f |= NGX_DLSS_FLAG_IS_HDR;

    // MVLowRes says the motion vectors are at render resolution rather than
    // display resolution. The bridge writes them at render resolution -- but
    // the two are the same size here, so the bit describes nothing and is left
    // off rather than set to a value that happens not to matter yet.
    //
    // MVJittered says the vectors already carry the jitter offset. They do not:
    // there is no jitter (see below), so there is nothing in them to carry.
    //
    // DepthInverted says near is 1 and far is 0. OpenGL's default depth range is
    // the other way round and this bridge does not change it, so the bit stays
    // off. A game using glClipControl to get reverse-Z would need it on, which
    // is what depth_inverted in the config is for.
    if (g_cfg.depth_inverted > 0) f |= NGX_DLSS_FLAG_DEPTH_INVERTED;

    return f;
}

// ---------------------------------------------------------------------------
// resource bring-up
// ---------------------------------------------------------------------------
static void GlReleaseSharedTex(int slot)
{
    SharedTex &t = g_bridge.tex[slot];
    if (GL.DeleteFramebuffers && t.fbo) { GL.DeleteFramebuffers(1, &t.fbo); t.fbo = 0; }
    if (GL.DeleteTextures     && t.tex) { GL.DeleteTextures(1, &t.tex);     t.tex = 0; }
    if (GL.DeleteMemoryObjectsEXT && t.mem) { GL.DeleteMemoryObjectsEXT(1, &t.mem); t.mem = 0; }
    D3D12ReleaseSharedTex(slot);
    t.fmtgl = 0;
}

static bool ContractChanged(const GlSource &s)
{
    return !g_bridge.frame_ready ||
           g_bridge.render_w != s.w || g_bridge.render_h != s.h ||
           g_bridge.create_flags != BuildCreateFlags(s);
}

// (Re)builds everything sized to the source: the four shared textures, their GL
// aliases, the framebuffer the conversion pass draws into, and the NGX feature.
static bool BridgeRebuild(const GlSource &src)
{
    Log("[bridge] (re)building for %ux%u", src.w, src.h);
    D3D12DrainGpu();
    g_bridge.frame_ready = false;

    if (g_bridge.fbo_convert) { GL.DeleteFramebuffers(1, &g_bridge.fbo_convert); g_bridge.fbo_convert = 0; }
    for (int i = 0; i < SLOT_COUNT; ++i) GlReleaseSharedTex(i);

    g_bridge.render_w = g_bridge.out_w = src.w;
    g_bridge.render_h = g_bridge.out_h = src.h;
    g_bridge.create_flags = BuildCreateFlags(src);
    g_bridge.quality      = NGX_QUALITY_DLAA;   // 1:1 geometry; see the README

    // Formats are chosen rather than negotiated, because the bridge owns both
    // ends of every one of these. RGBA16F holds an 8-bit back buffer exactly and
    // an HDR scene target without clipping; R32F is the depth format DLSS wants;
    // RG16F is the motion-vector format it recommends.
    struct Plan { int slot; DXGI_FORMAT dxgi; GLenum gl; bool uav; } plan[SLOT_COUNT] = {
        { SLOT_COLOR,  DXGI_FORMAT_R16G16B16A16_FLOAT, GL_RGBA16F, false },
        { SLOT_OUTPUT, DXGI_FORMAT_R16G16B16A16_FLOAT, GL_RGBA16F, true  },
        { SLOT_DEPTH,  DXGI_FORMAT_R32_FLOAT,          GL_R32F,    true  },
        { SLOT_MV,     DXGI_FORMAT_R16G16_FLOAT,       GL_RG16F,   false },
    };

    for (const Plan &p : plan)
    {
        if (!D3D12MakeSharedTexture(p.slot, src.w, src.h, p.dxgi, p.uav))
        { BridgeFail("shared texture"); return false; }
        g_bridge.tex[p.slot].fmtgl = p.gl;
        if (!GlImportSharedTex(p.slot)) { BridgeFail("import texture"); return false; }
    }

    // Depth and MV are written together by one pass, so they share a framebuffer.
    GL.GenFramebuffers(1, &g_bridge.fbo_convert);
    GL.BindFramebuffer(GL_DRAW_FRAMEBUFFER, g_bridge.fbo_convert);
    GL.FramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_bridge.tex[SLOT_DEPTH].tex, 0);
    GL.FramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, g_bridge.tex[SLOT_MV].tex, 0);
    const GLenum bufs[2] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1 };
    GL.DrawBuffers(2, bufs);
    const GLenum status = GL.CheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);
    GL.BindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    if (status != GL_FRAMEBUFFER_COMPLETE)
    { Log("[gl] conversion framebuffer incomplete (0x%04X)", status); BridgeFail("convert fbo"); return false; }

    // Warmed up here so the first bridged frame does not also pay for it. A
    // source with no depth is not a failure -- DLSS runs without it, badly, and
    // the log has already said so -- and neither is a depth format the driver
    // will not give us a texture of. Both are answered per frame below.
    GlEnsureDepthCopy(src);

    for (int i = 0; i < SLOT_COUNT; ++i)
        g_bridge.params->Set(kSlotKey[i], g_bridge.tex[i].tex12);

    if (!D3D12CreateDlssFeature()) { BridgeFail("create feature"); return false; }

    MotionReset();
    g_bridge.frame_ready       = true;
    g_bridge.consecutive_fails = 0;
    return true;
}

// ---------------------------------------------------------------------------
// per-frame parameters
//
// Every one is set explicitly. Nothing here reads a game's value, because there
// is no game value to read, and nothing is left unset on the grounds that NGX
// has a default -- an unset parameter is only ever the right answer when a real
// caller left it unset on purpose, which is the DX11 bridge's situation and not
// this one.
// ---------------------------------------------------------------------------
static void BridgeSetFrameParams(bool is_cut)
{
    NVSDK_NGX_Parameter *p = g_bridge.params;

    p->Set("Width",  g_bridge.render_w);
    p->Set("Height", g_bridge.render_h);
    p->Set("DLSS.Render.Subrect.Dimensions.Width",  g_bridge.render_w);
    p->Set("DLSS.Render.Subrect.Dimensions.Height", g_bridge.render_h);

    p->Set("DLSS.Input.Color.Subrect.Base.X", 0u);
    p->Set("DLSS.Input.Color.Subrect.Base.Y", 0u);
    p->Set("DLSS.Input.Depth.Subrect.Base.X", 0u);
    p->Set("DLSS.Input.Depth.Subrect.Base.Y", 0u);
    p->Set("DLSS.Input.MV.Subrect.Base.X",    0u);
    p->Set("DLSS.Input.MV.Subrect.Base.Y",    0u);
    p->Set("DLSS.Output.Subrect.Base.X",      0u);
    p->Set("DLSS.Output.Subrect.Base.Y",      0u);

    // The motion vectors are written in pixels of the render resolution, so
    // there is no scale left to apply. Writing them that way rather than in
    // texture coordinates is deliberate: MV.Scale is the parameter the DX11
    // bridge's Trails report showed is easiest to get catastrophically wrong,
    // and 1,1 cannot be.
    p->Set("MV.Scale.X", 1.0f);
    p->Set("MV.Scale.Y", 1.0f);

    // Zero, and truthfully so. DLSS reconstructs detail from a projection matrix
    // the game jitters by a sub-pixel offset each frame; an OpenGL game with no
    // temporal pass of its own does not jitter, and this bridge does not modify
    // the game's projection to make it. Declaring a jitter that was never
    // applied would tell DLSS to undo a shift that is not in the image, which
    // is worse than declaring none. See the README on what this costs.
    p->Set("Jitter.Offset.X", 0.0f);
    p->Set("Jitter.Offset.Y", 0.0f);

    p->Set("Sharpness", 0.0f);

    // No exposure texture exists, so the feature was created with AutoExposure
    // and these two stay at the neutral the official helper sends.
    p->Set("DLSS.Pre.Exposure",   1.0f);
    p->Set("DLSS.Exposure.Scale", 1.0f);
    p->Set("Pre.Exposure",        1.0f);
    p->Set("Exposure.Scale",      1.0f);

    // Reset discards the temporal history. It is set on the first frame of a new
    // feature, on a camera cut the reprojection could not follow, and on every
    // frame when reset_every is on for diagnosis.
    const unsigned int reset = (g_bridge.need_reset || is_cut || g_cfg.reset_every) ? 1u : 0u;
    p->Set("Reset", reset);
    g_bridge.need_reset = false;

    // Frame time drives the internal pacing. Measured rather than assumed,
    // because a locked 60 is not something a bridge gets to promise.
    static LONGLONG last;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    float dt = 16.6f;
    if (last && g_bridge.qpf) dt = (float)((double)(now.QuadPart - last) * 1000.0 / (double)g_bridge.qpf);
    last = now.QuadPart;
    if (dt < 0.1f || dt > 1000.0f) dt = 16.6f;
    p->Set("FrameTimeDeltaInMsec", dt);
}

// ---------------------------------------------------------------------------
// the frame
// ---------------------------------------------------------------------------
static bool g_frame_done;        // this swap interval has already been bridged

static void BridgeRunFrame(const GlSource &src)
{
    if (g_bridge.disabled || !g_bridge.gl_ready) return;

    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    GlStateGuard guard;
    if (!guard.ok) return;

    if (ContractChanged(src) && !BridgeRebuild(src)) return;
    if (!g_bridge.frame_ready) return;

    // Settle this frame's view-projection before anything reads it. Doing it
    // here rather than at the swap is what keeps it aligned with the depth about
    // to be sampled: both describe the same instant in the game's frame.
    MotionEndFrame();
    Mat4 reproj = Mat4Identity();
    bool is_cut = false;
    const bool have_mv = MotionReprojection(&reproj, &is_cut);

    // 1. the scene colour into the shared Color texture.
    GlBlitColor(src.fbo, src.w, src.h, g_bridge.tex[SLOT_COLOR].fbo,
                g_bridge.render_w, g_bridge.render_h, src.samples > 1);

    // 2. the scene depth into a texture the bridge is allowed to sample. The
    //    copy is re-checked here rather than only at a rebuild: a game can swap
    //    its depth attachment for one of another format without changing any
    //    size, and a copy of the wrong format is a depth blit the driver
    //    refuses rather than one that silently copies the wrong bits.
    bool have_depth = false;
    if (src.has_depth && GlEnsureDepthCopy(src))
    {
        GL.BindFramebuffer(GL_READ_FRAMEBUFFER, src.fbo);
        GL.BindFramebuffer(GL_DRAW_FRAMEBUFFER, g_bridge.depth_copy_fbo);
        // A depth blit cannot scale and cannot convert, so it is 1:1 by
        // construction -- the copy was created with the source's own format.
        GL.BlitFramebuffer(0, 0, (GLint)src.w, (GLint)src.h, 0, 0, (GLint)src.w, (GLint)src.h,
                           GL_DEPTH_BUFFER_BIT, GL_NEAREST);
        have_depth = GlCheck("depth blit");
    }

    // 3. one pass writes shared Depth and shared MV together.
    GL.BindFramebuffer(GL_DRAW_FRAMEBUFFER, g_bridge.fbo_convert);
    GL.Viewport(0, 0, (GLint)g_bridge.render_w, (GLint)g_bridge.render_h);
    GL.UseProgram(g_bridge.prog_convert);
    GL.BindVertexArray(g_bridge.vao);
    GL.ActiveTexture(GL_TEXTURE0);
    GL.BindTexture(GL_TEXTURE_2D, have_depth ? g_bridge.depth_copy_tex : 0);
    if (g_bridge.u_depth       >= 0) GL.Uniform1i(g_bridge.u_depth, 0);
    if (g_bridge.u_reproj      >= 0) GL.UniformMatrix4fv(g_bridge.u_reproj, 1, GL_FALSE, reproj.m);
    if (g_bridge.u_size        >= 0) GL.Uniform2f(g_bridge.u_size, (float)g_bridge.render_w, (float)g_bridge.render_h);
    if (g_bridge.u_have_mv     >= 0) GL.Uniform1i(g_bridge.u_have_mv, (have_mv && have_depth) ? 1 : 0);
    // With no depth there is nothing to write, and writing the sampler's zero is
    // the same as writing a far plane -- which is what DLSS should see for a
    // frame it must treat as flat.
    if (g_bridge.u_depth_scale >= 0) GL.Uniform1f(g_bridge.u_depth_scale, have_depth ? 1.0f : 0.0f);
    GL.DrawArrays(GL_TRIANGLES, 0, 3);
    GlCheck("conversion pass");

    // mode 2 never crosses to D3D12: it writes the shared Color and reads it
    // back, both from the GL stream, to prove the transport on its own. The
    // fences must be skipped along with the evaluate -- signalling fence_in and
    // then waiting fence_out for a value nothing will ever signal is a GPU wait
    // with no timeout, which is a hung context rather than a failed self-test.
    const bool crosses = (g_cfg.mode != 2);
    bool evaluated = true;
    const UINT64 v = ++g_bridge.timeline;

    if (crosses)
    {
        // 4. publish the inputs to the D3D12 queue. The flush matters for the
        //    same reason the D3D11 bridge flushes its immediate context: a
        //    signal still sitting in the client's command buffer is a wait the
        //    other queue never satisfies.
        GLuint inputs[3] = { g_bridge.tex[SLOT_COLOR].tex, g_bridge.tex[SLOT_DEPTH].tex,
                             g_bridge.tex[SLOT_MV].tex };
        GlSignalFence(g_bridge.sem_in, v, inputs, 3);
        if (GL.Flush) GL.Flush();
        GlCheck("glSignalSemaphoreEXT");

        // 5. the D3D12 evaluate -- the call a DLSS 5 add-on detours.
        BridgeSetFrameParams(is_cut);
        evaluated = D3D12RunEvaluate(v);

        // 6. take the result back. Every exit path from the evaluate signalled
        //    fence_out, so this wait always completes; what changes is only what
        //    it is waiting for. A teardown reached from inside the evaluate has
        //    already deleted the semaphore, and there is then nothing to wait on.
        if (!g_bridge.disabled)
        {
            GLuint outputs[1] = { g_bridge.tex[SLOT_OUTPUT].tex };
            GlWaitFence(g_bridge.sem_out, v, outputs, 1);
            GlCheck("glWaitSemaphoreEXT");
        }
    }

    if (evaluated && !g_bridge.disabled)
    {
        // Only mode 0 has anything in the Output texture. Modes 1 and 2 skip the
        // evaluate, so Output holds whatever it was created with -- copying that
        // back is a black screen, which is what mode 1 did before this and is
        // useless as an A/B test. They copy the shared Color back instead: the
        // picture should be indistinguishable from the game's own, which is
        // exactly what proves the transport is faithful. Mode 1 goes the whole
        // way round through D3D12 to do it, mode 2 never leaves OpenGL, so the
        // pair brackets the cross-API half.
        const int slot = (g_cfg.mode == 0) ? SLOT_OUTPUT : SLOT_COLOR;
        GlBlitColor(g_bridge.tex[slot].fbo, g_bridge.out_w, g_bridge.out_h,
                    src.fbo, src.w, src.h, false);
        GlCheck("copy-back");

        const UINT64 n = ++g_bridge.frames_done;
        if (n == 1)
            Log("[bridge] first frame bridged end to end: colour and depth in, "
                "D3D12 evaluate done, result copied back over the game's own target");
        if (n == 100 || n % 1000 == 0 || (g_cfg.verbose && (n <= 10 || n % 60 == 0)))
            Log("[bridge] %llu frames bridged (timeline %llu)",
                (unsigned long long)n, (unsigned long long)v);
        if (n == 100 || n % 1000 == 0) D3D12ProbeEvalDetour("at heartbeat");
    }

    QueryPerformanceCounter(&t1);
    TimingTick(t0.QuadPart, t1.QuadPart);
    g_frame_done = true;
}

// ---------------------------------------------------------------------------
// GL-side bring-up, once per context
// ---------------------------------------------------------------------------
static bool BridgeEnsureGl()
{
    if (g_bridge.gl_ready) return true;
    if (g_bridge.disabled) return false;

    if (!GlResolve()) { BridgeDisable("this OpenGL context cannot host the bridge"); return false; }
    if (!g_bridge.session_ready && !D3D12InitSession()) return false;   // disables on failure

    GlStateGuard guard;
    if (!guard.ok) return false;

    if (!GlImportSemaphore(&g_bridge.sem_in,  g_shared_fence_in,  "fence_in") ||
        !GlImportSemaphore(&g_bridge.sem_out, g_shared_fence_out, "fence_out"))
    { BridgeDisable("the shared D3D12 fences could not be imported into OpenGL"); return false; }

    // Core profiles refuse to draw with no vertex array bound, even for a
    // triangle whose vertices come out of gl_VertexID and touch no buffer.
    GL.GenVertexArrays(1, &g_bridge.vao);
    if (!GlMakeConvertProgram()) { BridgeDisable("the depth/motion shader would not build"); return false; }

    g_bridge.gl_ready = true;
    Log("[bridge] OpenGL side ready on context %p", (void *)g_bridge.ctx);
    return true;
}

// ---------------------------------------------------------------------------
// teardown
//
// Reached from BridgeDisable at any stage of bring-up, so every handle may be
// null and nothing may assume a previous step ran. The GL objects are released
// only if their context is still the current one; if it is not, they belong to a
// context that is going away anyway and touching them would be worse than
// leaving them.
// ---------------------------------------------------------------------------
static void BridgeReleaseResources()
{
    typedef HGLRC (WINAPI *PFN_wglGetCurrentContext)(void);
    static PFN_wglGetCurrentContext get_ctx;
    if (get_ctx == nullptr && g_opengl32)
        get_ctx = reinterpret_cast<PFN_wglGetCurrentContext>(GetProcAddress(g_opengl32, "wglGetCurrentContext"));

    const bool gl_usable = g_bridge.gl_ready && get_ctx && get_ctx() == g_bridge.ctx &&
                           g_bridge.ctx != nullptr && GL.DeleteFramebuffers != nullptr;
    if (gl_usable)
    {
        if (GL.Finish) GL.Finish();   // the shared textures are about to go
        for (int i = 0; i < SLOT_COUNT; ++i) GlReleaseSharedTex(i);
        if (g_bridge.fbo_convert) { GL.DeleteFramebuffers(1, &g_bridge.fbo_convert); g_bridge.fbo_convert = 0; }
        GlReleaseDepthCopy();
        if (g_bridge.prog_convert && GL.DeleteProgram) { GL.DeleteProgram(g_bridge.prog_convert); g_bridge.prog_convert = 0; }
        if (g_bridge.vao && GL.DeleteVertexArrays)     { GL.DeleteVertexArrays(1, &g_bridge.vao); g_bridge.vao = 0; }
        if (GL.DeleteSemaphoresEXT)
        {
            if (g_bridge.sem_in)  { GL.DeleteSemaphoresEXT(1, &g_bridge.sem_in);  g_bridge.sem_in = 0; }
            if (g_bridge.sem_out) { GL.DeleteSemaphoresEXT(1, &g_bridge.sem_out); g_bridge.sem_out = 0; }
        }
    }
    else
    {
        // Not our context any more. The D3D12 side still has to go, and the GL
        // names die with the context that owns them.
        for (int i = 0; i < SLOT_COUNT; ++i)
        {
            SharedTex &t = g_bridge.tex[i];
            t.fbo = t.tex = t.mem = 0;
        }
        g_bridge.fbo_convert = g_bridge.prog_convert = g_bridge.vao = 0;
        g_bridge.depth_copy_fbo = g_bridge.depth_copy_tex = 0;
        g_bridge.sem_in = g_bridge.sem_out = 0;
    }

    D3D12ReleaseSession();
    g_bridge.gl_ready = false;
}

// ---------------------------------------------------------------------------
// the frame trigger
//
// Where the bridge runs decides whether anything it writes is ever seen. The
// Vulkan port learned this the expensive way: two thousand frames bridged
// cleanly with no visible change, because the copy-back was landing after the
// game had already consumed its own image.
//
// source = 0: immediately before the swap. The back buffer is the last thing
//   written and the swap is the only thing that reads it, so there is no way to
//   be late.
//
// source = 1 or a name: the moment the game stops drawing into the scene target
//   and binds the default framebuffer, which is where an engine composites its
//   interface over the finished world. Running there puts the neural result into
//   the scene target before the composite reads it. Running at the swap instead
//   would write it afterwards, onto pixels the composite has already copied out
//   of.
//
// If the guess is wrong the frame is simply bridged at the swap instead, which
// is the back-buffer behaviour -- a worse picture for one setting, never a
// broken one.
// ---------------------------------------------------------------------------
static void BridgeOnFramebufferBind(GLenum target, GLuint prev_fbo, GLuint new_fbo)
{
    if (target != GL_FRAMEBUFFER && target != GL_DRAW_FRAMEBUFFER) return;
    if (g_bridge.disabled || g_cfg.mode == 3) return;

    // Binding the scene target re-arms the first-draw watch, so the matrix the
    // legacy route captures is the one in force for the world rather than for
    // whatever the game happened to draw before it.
    if (g_cfg.source != 0 && new_fbo != 0 && new_fbo == g_scene_fbo) ArmFirstDraw();

    if (g_cfg.source == 0 || g_frame_done) return;
    if (new_fbo != 0 || prev_fbo == 0 || prev_fbo != g_scene_fbo) return;
    if (!BridgeEnsureGl()) return;

    GlSource src = {};
    if (ChooseSource(&src) && src.fbo == g_scene_fbo) BridgeRunFrame(src);
}

// ---------------------------------------------------------------------------
// the swap hooks
//
// Two entry points reach the same swap: wglSwapBuffers in opengl32.dll, and
// SwapBuffers in gdi32.dll, which forwards into it when a GL context is
// current. Both are patched, and a thread-local depth means only the outermost
// does any work -- otherwise a game calling the GDI one would bridge its frame
// twice.
// ---------------------------------------------------------------------------
typedef BOOL (WINAPI *PFN_SwapBuffers)(HDC);
static __declspec(thread) int g_swap_nest;

// The lock is held only across the two patch edits, never across the forwarded
// call. That call is ReShade's whole present -- its effect passes, its overlay,
// and the real swap -- and ReShade waits on its own shader-compile threads in
// there. Holding a lock those threads can want, for the length of a frame, is a
// deadlock waiting for the right timing. While the patch is lifted another
// thread calling swap simply reaches the real function unbridged, which is the
// right thing to happen and not worth a lock.
static BOOL ForwardSwap(Hook *h, HDC hdc)
{
    if (!h->installed) return TRUE;
    void *target = h->target;

    EnterCriticalSection(&g_hook_cs);
    HookRestore(h);
    LeaveCriticalSection(&g_hook_cs);

    BOOL r = reinterpret_cast<PFN_SwapBuffers>(target)(hdc);

    EnterCriticalSection(&g_hook_cs);
    HookReinstall(h);
    LeaveCriticalSection(&g_hook_cs);
    return r;
}

// Notices that the GL context has changed under us. The shared textures, the
// shader and the imported semaphores all belong to the context they were made
// on and mean nothing on another one, so the whole GL side is rebuilt. The old
// names are not deleted: they died with their context, and asking the new one
// about them would be an error at best.
static void BridgeCheckContext(HDC hdc)
{
    typedef HGLRC (WINAPI *PFN_wglGetCurrentContext)(void);
    static PFN_wglGetCurrentContext get_ctx;
    if (get_ctx == nullptr && g_opengl32)
        get_ctx = reinterpret_cast<PFN_wglGetCurrentContext>(GetProcAddress(g_opengl32, "wglGetCurrentContext"));
    const HGLRC now = get_ctx ? get_ctx() : nullptr;

    g_bridge.hdc = hdc;
    if (now == g_bridge.ctx) return;

    if (g_bridge.ctx != nullptr)
    {
        Log("[gl] the current context changed (%p -> %p); rebuilding the OpenGL side",
            (void *)g_bridge.ctx, (void *)now);
        for (int i = 0; i < SLOT_COUNT; ++i)
        {
            SharedTex &t = g_bridge.tex[i];
            t.fbo = t.tex = t.mem = 0;
            D3D12ReleaseSharedTex(i);
        }
        g_bridge.fbo_convert = g_bridge.prog_convert = g_bridge.vao = 0;
        g_bridge.depth_copy_fbo = g_bridge.depth_copy_tex = 0;
        g_bridge.depth_copy_w = g_bridge.depth_copy_h = 0;
        g_bridge.sem_in = g_bridge.sem_out = 0;
        g_bridge.gl_ready = g_bridge.frame_ready = false;
        g_scene_fbo = 0;
        g_scene_reported = false;
        MotionReset();
    }
    g_bridge.ctx = now;
}

static void BridgeAtSwapInner(HDC hdc)
{
    // Said once, unconditionally, before anything can go wrong. "The hooks
    // landed but no swap ever arrived" and "a swap arrived and something in here
    // went wrong" are different faults with different causes, and a log that
    // stops after the hook lines cannot tell them apart.
    static bool said_first_swap = false;
    if (!said_first_swap)
    {
        said_first_swap = true;
        Log("[bridge] first swap seen (hdc %p)", (void *)hdc);
    }

    BridgeCheckContext(hdc);
    if (g_bridge.ctx == nullptr)
    {
        static bool said = false;
        if (!said) { said = true;
            Log("[bridge] this swap has no current GL context; standing aside. If every "
                "swap says this, the game is presenting from a thread other than the one "
                "holding its context."); }
        return;
    }
    CfgReload();
    if (g_cfg.mode == 3) { g_frame_done = true; return; }   // fully inert

    if (!BridgeEnsureGl()) return;
    if (g_frame_done) return;              // scene mode already bridged this frame

    GlSource src = {};
    if (ChooseSource(&src)) BridgeRunFrame(src);
}

// The bridge is a guest in somebody else's frame, so an exception raised inside
// it has to end the bridge and not the game.
static void BridgeAtSwap(HDC hdc)
{
    if (g_bridge.disabled) return;
    DWORD code = 0;
    __try { BridgeAtSwapInner(hdc); }
    __except ((code = GetExceptionCode()), EXCEPTION_EXECUTE_HANDLER)
    {
        Log("[bridge] the frame path faulted 0x%08X; disabling to protect the game", code);
        BridgeDisable("the bridge frame path raised an exception");
    }
}

static void BridgeAfterSwap()
{
    g_frame_done = false;
    GlResetFrameTracking();
    if (!g_bridge.disabled)
        ArmFirstDraw();   // for the next frame; it takes itself back out on the
                          // first draw that follows

    // Frame-to-frame pacing, measured at the one place in the process that is
    // guaranteed to happen once per displayed frame.
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (g_bridge.last_entry != 0)
    {
        const LONGLONG iv = now.QuadPart - g_bridge.last_entry;
        if (g_bridge.iv_min == 0 || iv < g_bridge.iv_min) g_bridge.iv_min = iv;
        if (iv > g_bridge.iv_max) g_bridge.iv_max = iv;
    }
    g_bridge.last_entry = now.QuadPart;
}

static BOOL WINAPI Detour_wglSwapBuffers(HDC hdc)
{
    const bool outer = (++g_swap_nest == 1);
    if (outer) BridgeAtSwap(hdc);
    BOOL r = ForwardSwap(&g_hook_swap, hdc);
    if (outer) BridgeAfterSwap();
    --g_swap_nest;
    return r;
}

static BOOL WINAPI Detour_gdiSwapBuffers(HDC hdc)
{
    const bool outer = (++g_swap_nest == 1);
    if (outer) BridgeAtSwap(hdc);
    BOOL r = ForwardSwap(&g_hook_gdi_swap, hdc);
    if (outer) BridgeAfterSwap();
    --g_swap_nest;
    return r;
}

// ---------------------------------------------------------------------------
// installation
//
// opengl32.dll is always present in a GL game, but not necessarily before this
// add-on registers; the watch thread in the main file comes back for it. The
// host executable is never patched -- a game that checks its own code would see
// it, and there is nothing there to patch anyway.
// ---------------------------------------------------------------------------
static bool InstallGlHooks()
{
    if (g_hook_swap.installed || g_hook_gdi_swap.installed) return true;
    if (g_bridge.disabled) return false;
    if (g_cfg.mode == 3)
    {
        // Fully inert means fully inert: loaded, logging, and not a single byte
        // written to anybody else's code. This is the state to put the add-on in
        // to prove it is not the cause of something.
        static bool said = false;
        if (!said) { said = true; Log("[hook] mode=3: no hooks installed at all."); }
        return true;
    }

    HMODULE gl = GetModuleHandleW(L"opengl32.dll");
    if (gl == nullptr) return false;
    g_opengl32 = gl;

    // Which opengl32.dll this is matters and belongs in the log. With ReShade
    // installed for OpenGL, the module under that name is ReShade's own proxy,
    // and patching it puts the bridge ahead of ReShade's effects -- the neural
    // result goes into the back buffer and ReShade then works on it. If the name
    // resolves to the one in System32 instead, the bridge runs after ReShade
    // instead of before it. Both work; only one of them is worth knowing about
    // when a report says the effects look wrong.
    {
        wchar_t path[MAX_PATH] = {};
        GetModuleFileNameW(gl, path, MAX_PATH);
        Log("[hook] opengl32.dll in this process is %ls", path);
    }

    EnterCriticalSection(&g_hook_cs);
    void *swap    = reinterpret_cast<void *>(GetProcAddress(gl, "wglSwapBuffers"));
    void *getproc = reinterpret_cast<void *>(GetProcAddress(gl, "wglGetProcAddress"));
    if (g_wglGetProcAddress == nullptr)
        g_wglGetProcAddress = reinterpret_cast<PFN_wglGetProcAddress>(getproc);

    // wglGetProcAddress is patched only when a setting actually needs what it
    // buys, and the default settings do not. Hooking it means handing the game
    // pointers into this module for glBindFramebuffer and three others, which the
    // game caches for the whole session -- a much larger commitment than a patch
    // that can be lifted, and one worth making only for the features that
    // require it: finding the scene target (source), and watching a matrix
    // uniform (mv_vp / mv_view / mv_proj).
    //
    // It is read once, here, because the game resolves its entry points during
    // startup and a wrapper handed out later would never be called. Changing
    // those settings needs the game restarted; the rest are live.
    const bool want_getproc = g_cfg.source != 0 || g_cfg.mv_vp[0] || g_cfg.mv_view[0] ||
                              g_cfg.mv_proj[0];
    if (want_getproc && getproc && !g_hook_getproc.installed)
    {
        HookInstall(&g_hook_getproc, getproc, reinterpret_cast<void *>(Detour_wglGetProcAddress));
        Log("[hook] patched wglGetProcAddress @ %p (source=%d, matrix uniforms %s)",
            getproc, g_cfg.source,
            (g_cfg.mv_vp[0] || g_cfg.mv_view[0] || g_cfg.mv_proj[0]) ? "configured" : "not set");
    }
    else if (!want_getproc)
        Log("[hook] wglGetProcAddress left alone: source=0 and no matrix uniform is "
            "configured, so nothing needs to observe the game's GL calls.");

    if (swap && HookInstall(&g_hook_swap, swap, reinterpret_cast<void *>(Detour_wglSwapBuffers)))
        Log("[hook] patched wglSwapBuffers @ %p", swap);

    if (HMODULE gdi = GetModuleHandleW(L"gdi32.dll"))
    {
        void *gswap = reinterpret_cast<void *>(GetProcAddress(gdi, "SwapBuffers"));
        if (gswap && !g_hook_gdi_swap.installed &&
            HookInstall(&g_hook_gdi_swap, gswap, reinterpret_cast<void *>(Detour_gdiSwapBuffers)))
            Log("[hook] patched SwapBuffers @ %p in gdi32.dll", gswap);
    }
    LeaveCriticalSection(&g_hook_cs);
    return g_hook_swap.installed || g_hook_gdi_swap.installed;
}
