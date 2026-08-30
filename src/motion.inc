// Motion vectors.
//
// This file has no counterpart in the DX11 or Vulkan bridges, and it is the
// honest cost of there being no NGX OpenGL API. Those two mirror a game that is
// already running DLSS, so the game hands them a motion-vector texture it
// produced for its own temporal pass. An OpenGL game with no TAA has never
// computed one and has nowhere to get it from.
//
// So the bridge derives them. Given the depth buffer and the view-projection
// matrix of this frame and the last, every pixel can be put back where it was:
// unproject through the current matrix, project through the previous one, and
// the difference is the motion vector. This is exact for everything that did
// not move on its own, which is the camera motion DLSS most needs to follow, and
// wrong for anything that did -- a moving character reprojects as though it had
// stood still, and DLSS resolves it as if the camera had moved past it.
//
// That is a real limitation and it is stated in the README rather than hidden
// here. It is also why mv = none is a supported setting and not a failure mode:
// zero motion vectors are a worse picture in motion but never a wrong one.
//
// Getting the matrices is the part that varies by game:
//
//   legacy   the fixed-function matrix stack, read with glGetFloatv. Correct
//            when the scene's first draw is world geometry submitted with the
//            camera matrix alone, which is what fixed-function engines do.
//   uniform  a named matrix uniform the game uploads. Exact, and the right
//            answer for anything shader-based -- but it needs the name, which
//            only the game (or its shader mod) can tell you.
//   none     no matrices, zero motion vectors.

// ---------------------------------------------------------------------------
// 4x4 matrix arithmetic, column-major throughout: m[c * 4 + r], the layout both
// glGetFloatv and glUniformMatrix4fv use, so nothing captured needs transposing
// on the way in and a point transforms as clip = M * world.
// ---------------------------------------------------------------------------
static Mat4 Mat4Identity()
{
    Mat4 r = {};
    r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
    return r;
}

static Mat4 Mat4Mul(const Mat4 &a, const Mat4 &b)   // a * b
{
    Mat4 r;
    for (int c = 0; c < 4; ++c)
        for (int row = 0; row < 4; ++row)
        {
            double s = 0.0;
            for (int k = 0; k < 4; ++k) s += (double)a.m[k * 4 + row] * (double)b.m[c * 4 + k];
            r.m[c * 4 + row] = (float)s;
        }
    return r;
}

static Mat4 Mat4Transpose(const Mat4 &a)
{
    Mat4 r;
    for (int c = 0; c < 4; ++c)
        for (int row = 0; row < 4; ++row) r.m[c * 4 + row] = a.m[row * 4 + c];
    return r;
}

// General inverse by cofactor expansion, in double. A projection matrix is
// nearly singular in its far row and a float inverse of one loses enough
// precision to bend distant motion vectors visibly, which shows up as the sky
// shimmering while the ground is stable.
static bool Mat4Inverse(const Mat4 &src, Mat4 *out)
{
    double m[16], inv[16];
    for (int i = 0; i < 16; ++i) m[i] = (double)src.m[i];

    inv[0]  =  m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15] + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
    inv[4]  = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15] - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
    inv[8]  =  m[4]*m[9]*m[15]  - m[4]*m[11]*m[13] - m[8]*m[5]*m[15] + m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
    inv[12] = -m[4]*m[9]*m[14]  + m[4]*m[10]*m[13] + m[8]*m[5]*m[14] - m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];
    inv[1]  = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15] - m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
    inv[5]  =  m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15] + m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
    inv[9]  = -m[0]*m[9]*m[15]  + m[0]*m[11]*m[13] + m[8]*m[1]*m[15] - m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
    inv[13] =  m[0]*m[9]*m[14]  - m[0]*m[10]*m[13] - m[8]*m[1]*m[14] + m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];
    inv[2]  =  m[1]*m[6]*m[15]  - m[1]*m[7]*m[14]  - m[5]*m[2]*m[15] + m[5]*m[3]*m[14] + m[13]*m[2]*m[7]  - m[13]*m[3]*m[6];
    inv[6]  = -m[0]*m[6]*m[15]  + m[0]*m[7]*m[14]  + m[4]*m[2]*m[15] - m[4]*m[3]*m[14] - m[12]*m[2]*m[7]  + m[12]*m[3]*m[6];
    inv[10] =  m[0]*m[5]*m[15]  - m[0]*m[7]*m[13]  - m[4]*m[1]*m[15] + m[4]*m[3]*m[13] + m[12]*m[1]*m[7]  - m[12]*m[3]*m[5];
    inv[14] = -m[0]*m[5]*m[14]  + m[0]*m[6]*m[13]  + m[4]*m[1]*m[14] - m[4]*m[2]*m[13] - m[12]*m[1]*m[6]  + m[12]*m[2]*m[5];
    inv[3]  = -m[1]*m[6]*m[11]  + m[1]*m[7]*m[10]  + m[5]*m[2]*m[11] - m[5]*m[3]*m[10] - m[9]*m[2]*m[7]   + m[9]*m[3]*m[6];
    inv[7]  =  m[0]*m[6]*m[11]  - m[0]*m[7]*m[10]  - m[4]*m[2]*m[11] + m[4]*m[3]*m[10] + m[8]*m[2]*m[7]   - m[8]*m[3]*m[6];
    inv[11] = -m[0]*m[5]*m[11]  + m[0]*m[7]*m[9]   + m[4]*m[1]*m[11] - m[4]*m[3]*m[9]  - m[8]*m[1]*m[7]   + m[8]*m[3]*m[5];
    inv[15] =  m[0]*m[5]*m[10]  - m[0]*m[6]*m[9]   - m[4]*m[1]*m[10] + m[4]*m[2]*m[9]  + m[8]*m[1]*m[6]   - m[8]*m[2]*m[5];

    double det = m[0]*inv[0] + m[1]*inv[4] + m[2]*inv[8] + m[3]*inv[12];
    if (det > -1e-12 && det < 1e-12) return false;
    det = 1.0 / det;
    for (int i = 0; i < 16; ++i) out->m[i] = (float)(inv[i] * det);
    return true;
}

static bool Mat4Sane(const Mat4 &a)
{
    for (int i = 0; i < 16; ++i)
    {
        const float v = a.m[i];
        if (!(v == v)) return false;                    // NaN
        if (v > 1e30f || v < -1e30f) return false;      // infinity, or nonsense
    }
    // An all-zero matrix is what an unwritten uniform reads back as, and it
    // inverts to nothing useful.
    for (int i = 0; i < 16; ++i) if (a.m[i] != 0.0f) return true;
    return false;
}

// Identity is a perfectly legal reprojection -- it is what a camera that did not
// move produces -- but it is never a view-projection. A shader-based game in a
// compatibility context still has a matrix stack, and that stack is identity;
// accepting it would look like a successful capture, silence the "no matrices
// found" warning, and hand DLSS zero motion vectors while claiming otherwise.
static bool Mat4LooksLikeViewProj(const Mat4 &a)
{
    if (!Mat4Sane(a)) return false;
    const Mat4 id = Mat4Identity();
    for (int i = 0; i < 16; ++i)
    {
        const float d = a.m[i] - id.m[i];
        if (d > 1e-6f || d < -1e-6f) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// capture
// ---------------------------------------------------------------------------
enum MvSource { MV_AUTO = 0, MV_LEGACY, MV_UNIFORM, MV_NONE };

static int      g_mv_source_resolved = -1;   // one of MvSource, once decided
static bool     g_mv_said_source;
static Mat4     g_mv_pending;                // captured this frame, not yet promoted
static bool     g_mv_pending_valid;

// -- the uniform route -------------------------------------------------------
// glGetUniformLocation is hooked so the bridge learns which location a named
// matrix lives at, and glUseProgram so the current program is known without a
// query on every upload. glUniformMatrix4fv then snapshots the value whenever
// it lands on one of those (program, location) pairs.
struct MatSlot
{
    GLuint program;
    GLint  location;
    bool   have;
    Mat4   value;
};

static MatSlot g_slot_vp[8];      // a combined view-projection uniform
static MatSlot g_slot_view[8];    // or view and projection separately
static MatSlot g_slot_proj[8];
static int     g_slot_vp_n, g_slot_view_n, g_slot_proj_n;
static GLuint  g_current_program;

static void MotionOnUseProgram(GLuint program) { g_current_program = program; }

static void MotionRememberLocation(MatSlot *set, int *n, GLuint program, GLint loc)
{
    if (loc < 0 || *n >= 8) return;
    for (int i = 0; i < *n; ++i)
        if (set[i].program == program && set[i].location == loc) return;
    set[*n].program  = program;
    set[*n].location = loc;
    set[*n].have     = false;
    ++*n;
}

// Called from the glGetUniformLocation hook: the game has just asked where a
// uniform lives, and if its name is one we were configured to watch, that
// answer is the only way to recognise the upload later.
static void MotionOnGetUniformLocation(GLuint program, const char *name, GLint loc)
{
    if (name == nullptr || loc < 0) return;
    if (g_cfg.mv_vp[0]   && _stricmp(name, g_cfg.mv_vp)   == 0)
    { MotionRememberLocation(g_slot_vp,   &g_slot_vp_n,   program, loc);
      Log("[mv] watching uniform '%s' at location %d in program %u", name, loc, program); }
    if (g_cfg.mv_view[0] && _stricmp(name, g_cfg.mv_view) == 0)
    { MotionRememberLocation(g_slot_view, &g_slot_view_n, program, loc);
      Log("[mv] watching uniform '%s' at location %d in program %u", name, loc, program); }
    if (g_cfg.mv_proj[0] && _stricmp(name, g_cfg.mv_proj) == 0)
    { MotionRememberLocation(g_slot_proj, &g_slot_proj_n, program, loc);
      Log("[mv] watching uniform '%s' at location %d in program %u", name, loc, program); }
}

static bool MotionStore(MatSlot *set, int n, GLint loc, const GLfloat *v, bool transpose)
{
    for (int i = 0; i < n; ++i)
        if (set[i].location == loc && set[i].program == g_current_program)
        {
            Mat4 m;
            memcpy(m.m, v, sizeof(m.m));
            if (transpose) m = Mat4Transpose(m);
            set[i].value = m;
            set[i].have  = true;
            return true;
        }
    return false;
}

// Called from the glUniformMatrix4fv hook. Deliberately cheap on the miss path:
// three short scans of integers, and nothing else, on a call a game can make
// thousands of times a frame.
static void MotionOnUniformMatrix4fv(GLint loc, GLsizei count, GLboolean transpose, const GLfloat *value)
{
    if (loc < 0 || count < 1 || value == nullptr) return;
    if (g_slot_vp_n)   MotionStore(g_slot_vp,   g_slot_vp_n,   loc, value, transpose != GL_FALSE);
    if (g_slot_view_n) MotionStore(g_slot_view, g_slot_view_n, loc, value, transpose != GL_FALSE);
    if (g_slot_proj_n) MotionStore(g_slot_proj, g_slot_proj_n, loc, value, transpose != GL_FALSE);
}

static bool MotionFromUniforms(Mat4 *out)
{
    for (int i = 0; i < g_slot_vp_n; ++i)
        if (g_slot_vp[i].have && Mat4LooksLikeViewProj(g_slot_vp[i].value))
        { *out = g_slot_vp[i].value; return true; }

    // Separate matrices: projection * view, in that order, because a point goes
    // clip = P * (V * world).
    for (int p = 0; p < g_slot_proj_n; ++p)
    {
        if (!g_slot_proj[p].have || !Mat4LooksLikeViewProj(g_slot_proj[p].value)) continue;
        for (int v = 0; v < g_slot_view_n; ++v)
        {
            if (!g_slot_view[v].have || !Mat4Sane(g_slot_view[v].value)) continue;
            *out = Mat4Mul(g_slot_proj[p].value, g_slot_view[v].value);
            return true;
        }
    }
    return false;
}

// -- the legacy route --------------------------------------------------------
// Read at the first scene draw of the frame rather than at the swap, because by
// swap time the stack holds whatever the UI pass left on it.
//
// GL_MODELVIEW_MATRIX is model * view, not view, so this is exact only while the
// first thing drawn carries no model transform of its own -- world geometry
// submitted in world space, which is what fixed-function engines do and what
// makes this route worth having at all. Where it is wrong it is wrong the same
// way every frame, so the frame-to-frame difference still tracks the camera.
static bool MotionFromLegacyStack(Mat4 *out)
{
    if (g_bridge.core_profile || GL.GetFloatv == nullptr) return false;
    Mat4 proj, mv;
    GL.GetFloatv(GL_PROJECTION_MATRIX, proj.m);
    GL.GetFloatv(GL_MODELVIEW_MATRIX,  mv.m);
    if (GL.GetError && GL.GetError() != GL_NO_ERROR) return false;
    if (!Mat4LooksLikeViewProj(proj) || !Mat4Sane(mv)) return false;
    *out = Mat4Mul(proj, mv);
    return Mat4LooksLikeViewProj(*out);
}

// -- the frame hooks ---------------------------------------------------------

// Whether the first-draw hook is worth arming at all. A core-profile context has
// no matrix stack to read, and a game whose matrices are already arriving
// through the uniform route has nothing to gain from a second, weaker source --
// so in both cases the draw entry points are left unpatched entirely.
// Whether to patch the draw entry points at all.
//
// This is deliberately not part of mv = auto, and the reason is a crash rather
// than a preference. Reading the fixed-function matrix stack means catching the
// frame's first draw, which means patching glDrawArrays, glDrawElements and
// glBegin -- and because the hook takes itself out on its first call, it has to
// be put back every frame. Overwriting the first fourteen bytes of three of the
// hottest functions in the process, once a frame, is safe only while no other
// thread is inside them. A game that streams on a second GL thread will
// eventually be, and the result is an access violation in the game's own stack
// with nothing in any log. MX Bikes did exactly that on loading a track.
//
// There is no way to make per-draw interception safe here without a real
// trampoline, and a length disassembler is a bigger and riskier thing than the
// feature is worth. So this is opt-in: mv = 1, chosen by somebody who knows
// their game is fixed-function and single-threaded. Auto means uniforms if they
// are configured and nothing otherwise, which costs no patches at all.
static bool MotionWantsFirstDraw()
{
    if (g_cfg.mv != MV_LEGACY) return false;
    if (!g_bridge.gl_ready) return false;       // core_profile is not known before this
    if (g_bridge.core_profile) return false;    // no matrix stack to read
    if (g_mv_source_resolved == MV_UNIFORM) return false;
    return true;
}

// Called once per frame at the first draw into the scene target (gl_capture.inc
// arms and disarms the hook that gets here). This instant is the whole point of
// the legacy route: the matrix stack holds the camera here, and by the time the
// frame reaches a swap it holds whatever the interface pass left on it.
static void MotionOnSceneDraw()
{
    if (g_mv_pending_valid) return;
    Mat4 m;
    if (MotionFromLegacyStack(&m)) { g_mv_pending = m; g_mv_pending_valid = true; }
}

static const char *MvSourceName(int s)
{
    switch (s)
    {
    case MV_LEGACY:  return "the fixed-function matrix stack";
    case MV_UNIFORM: return "the configured matrix uniforms";
    case MV_NONE:    return "nothing -- motion vectors are zero";
    default:         return "undecided";
    }
}

// Settles this frame's view-projection. Called from the frame path, after the
// scene has been drawn and before the conversion pass reads uReproj.
static void MotionEndFrame()
{
    g_bridge.vp_prev       = g_bridge.vp;
    g_bridge.vp_prev_valid = g_bridge.vp_valid;
    g_bridge.vp_valid      = false;

    if (g_cfg.mv == MV_NONE) { g_mv_pending_valid = false; return; }

    Mat4 m = Mat4Identity();
    bool got = false;
    int  from = MV_NONE;

    if (g_cfg.mv == MV_UNIFORM || g_cfg.mv == MV_AUTO)
        if (MotionFromUniforms(&m)) { got = true; from = MV_UNIFORM; }

    // Legacy only when it was asked for by name. See MotionWantsFirstDraw.
    if (!got && g_cfg.mv == MV_LEGACY && g_mv_pending_valid)
    { m = g_mv_pending; got = true; from = MV_LEGACY; }

    g_mv_pending_valid = false;

    if (!got)
    {
        // Not on the first frame. The legacy route needs its draw hook armed,
        // and that does not happen until the context has been resolved -- which
        // is during this very frame. Warning here would fire on every session
        // before the source it is complaining about has had a chance to work.
        static UINT64 quiet_frames;
        if (!g_mv_said_source && ++quiet_frames > 60)
        {
            g_mv_said_source = true;
            g_mv_source_resolved = MV_NONE;
            Warn("no view-projection matrix is available, so motion vectors are zero. "
                 "DLSS still runs and the picture is stable when you are still, but it "
                 "cannot follow the camera and will smear when you move.");
            Log("  To fix it, name this game's matrix uniform in the config -- mv_vp for "
                "a combined view-projection, or mv_view and mv_proj for a separate pair "
                "-- and restart the game. The names come from the game's shader source "
                "or its shader mod.");
            Log("  mv = 1 reads the fixed-function matrix stack instead, which only an "
                "old engine still uses. It patches the draw entry points every frame and "
                "is not safe in a game that issues GL calls from more than one thread, "
                "so it is off unless asked for by name.");
        }
        return;
    }

    if (!g_mv_said_source || g_mv_source_resolved != from)
    {
        g_mv_said_source = true;
        g_mv_source_resolved = from;
        Log("[mv] view-projection is coming from %s", MvSourceName(from));
    }

    g_bridge.vp       = m;
    g_bridge.vp_valid = true;
}

// The reprojection matrix the shader consumes: previous view-projection times
// the inverse of the current one, so an NDC position goes straight to where it
// was. Also decides whether this frame is a cut, which is the one thing DLSS
// cannot work out for itself and the one thing Reset exists for.
static bool MotionReprojection(Mat4 *out, bool *is_cut)
{
    *is_cut = false;
    if (!g_bridge.vp_valid || !g_bridge.vp_prev_valid)
    {
        // Having no matrices at all is a steady state, not a discontinuity: with
        // mv = none the bridge is in it on every frame of the session, and
        // resetting there would throw away the temporal accumulation that is
        // most of what DLSS does. Only the transition is a cut -- tracking that
        // has just started, or has just been lost.
        *is_cut = g_bridge.vp_valid != g_bridge.vp_prev_valid;
        return false;
    }

    Mat4 inv;
    if (!Mat4Inverse(g_bridge.vp, &inv)) { *is_cut = true; return false; }
    *out = Mat4Mul(g_bridge.vp_prev, inv);
    if (!Mat4Sane(*out)) { *is_cut = true; return false; }

    // How far the middle of the screen moved, at mid-depth. A camera that
    // teleported puts it most of a screen away, and carrying temporal history
    // across that is exactly the smear Reset exists to prevent. A camera that
    // merely turned quickly does not, so this does not fire on ordinary play.
    const float ndc[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    float p[4];
    for (int r = 0; r < 4; ++r)
    {
        double s = 0.0;
        for (int k = 0; k < 4; ++k) s += (double)out->m[k * 4 + r] * (double)ndc[k];
        p[r] = (float)s;
    }
    if (p[3] > 1e-9f || p[3] < -1e-9f)
    {
        const float dx = p[0] / p[3], dy = p[1] / p[3];
        if (dx * dx + dy * dy > 1.0f) *is_cut = true;   // over half a screen in NDC
    }
    return true;
}

static void MotionReset()
{
    g_bridge.vp_valid = g_bridge.vp_prev_valid = false;
    g_mv_pending_valid = false;
    for (int i = 0; i < g_slot_vp_n; ++i)   g_slot_vp[i].have = false;
    for (int i = 0; i < g_slot_view_n; ++i) g_slot_view[i].have = false;
    for (int i = 0; i < g_slot_proj_n; ++i) g_slot_proj[i].have = false;
}
