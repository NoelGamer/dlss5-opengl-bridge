# DLSS 5 OpenGL Bridge

A ReShade add-on that lets a DLSS 5 Neural Rendering add-on — which only hooks
DirectX 12 — run inside a game that renders with OpenGL.

An OpenGL port of [NIGos' DLSS 5 DX11 Bridge][dx11] and, through it, of
[AlanBacker's Vulkan port][vk]. The D3D12 and NGX half is theirs, near enough
verbatim.

[dx11]: https://github.com/NIGos/dlss5-dx11-bridge
[vk]: https://github.com/AlanBacker/dlss5-vk-bridge

> **Status: young.** Everything below the OpenGL edge is the DX11 bridge's proven
> code; everything at that edge is new. 1.0.0 crashed MX Bikes on launch — see
> [Why the add-on pins itself](#why-the-add-on-pins-itself) — and 1.0.1 fixes
> that, but no title has yet been confirmed bridging end to end. Logs from anyone
> who tries it are the most useful thing you can send.

## Version history

**1.0.3** — two fixes found on the first title to bridge end to end.
`GL_FRAMEBUFFER_ATTACHMENT_DEPTH_SIZE` was written as `0x8213`, which is
`GREEN_SIZE`: it answers zero on a depth attachment, so the default framebuffer
reported no depth and DLSS ran blind on every game whose scene target is the back
buffer. And the fixed-function matrix hook is now opt-in (`mv = 1`) instead of
part of `mv = auto` — it rewrites the first bytes of `glDrawArrays`,
`glDrawElements` and `glBegin` once a frame, which is safe only while the game
draws from a single thread, and MX Bikes crashed on loading a track. It also
retires itself after 300 frames of finding nothing.

**1.0.2** — finds the NGX dispatcher. `_nvngx.dll` lives in the driver store,
which is on nobody's DLL search path, so 1.0.1's bare `LoadLibrary` could not
reach it and every session ended in "no module carries the complete NGX D3D12
export set". It is now located the way NVIDIA's own loader does it, through
`HKLM\SOFTWARE\NVIDIA Corporation\Global\NGXCore` → `FullPath`, with a
driver-store search by pattern as a fallback. This is a problem only an OpenGL
bridge has: the DX11 and Vulkan bridges are woken up by the game's own NGX call,
so the dispatcher is already in the process by the time they look for it.

**1.0.1** — the add-on pins itself in memory before installing anything. ReShade
loads and unloads add-ons several times during startup, and 1.0.0 left a running
thread and a set of handed-out function pointers inside a module that was then
unmapped, which crashed the game a few seconds after launch. Also: with the
default settings `wglGetProcAddress` is no longer hooked at all, the first-draw
hook is not armed until the GL context has been identified, `mode = 3` now
installs no hooks whatsoever, and the swap forward no longer holds a lock across
ReShade's whole present.

**1.0.0** — first build.

## Read this first: OpenGL is not the same problem

The DX11 and Vulkan bridges work by **mirroring**. The game already drives DLSS
through NGX on its own API, so those bridges intercept the game's own
`EvaluateFeature`, forward it untouched, and reproduce the same contract on a
private D3D12 NGX session — the call the DLSS 5 add-on detours. Every size,
format and scalar is read out of the parameter block the game passed.

**None of that is possible in OpenGL, because NGX has no OpenGL API.** NVIDIA
ships NGX for D3D11, D3D12 and Vulkan and never shipped an OpenGL interface. An
OpenGL game has therefore never made an NGX call of any kind, there is no
`NVSDK_NGX_OPENGL_EvaluateFeature` to hook, and there is no parameter block to
read.

So this bridge does not mirror a contract. It **builds** one:

| | DX11 / Vulkan bridges | this bridge |
| --- | --- | --- |
| Color | the game's texture, from the parameter block | the scene target, found in the GL frame |
| Depth | the game's texture | that target's depth, converted to `R32_FLOAT` |
| MotionVectors | the game's texture | **reprojected** from depth and the frame-to-frame view-projection |
| Jitter | mirrored from the game | **zero** — the game does not jitter |
| create flags | copied from the game | stated outright, from buffers this bridge made |
| geometry | whatever the game asked for | **1:1 — DLAA**, always |

The DX11 bridge's own source anticipates this exactly, in a note left for
whoever adds a second source of contract: a synthetic contract *"is a legitimate
second source, but it has to arrive already complete: every key set explicitly by
whatever builds it."* That is the rule this bridge follows. Nothing here is left
unset in the hope that NGX has a sensible default, because an unset parameter is
only ever right when a real caller left it unset on purpose.

**If your OpenGL game already has DLSS through a mod**, that mod is reaching NGX
on D3D11 or D3D12 through interop — and the [DX11 bridge][dx11] already handles
it, because it hooks every module exporting the NGX D3D11 API regardless of what
the host game renders with. Use that one instead. This bridge is for the ordinary
case: an OpenGL game with no DLSS at all.

## Try DLSS5-Feeder first

[**DLSS5-Feeder**](https://github.com/jlrouzies-fr/DLSS5-Feeder) solves the same
problem this bridge does, and it got there first. It also builds a synthetic DLAA
contract for a game with no DLSS, but it assembles it out of ReShade's own
resources: the back buffer ReShade is already processing, depth from ReShade's
Generic Depth add-on, and motion vectors from an established optical-flow shader
(LumeniteFX, iMMERSE Launchpad, VORT or qUINT).

It is the more mature project, and its motion vectors are better than this
bridge's in the case that matters most:

| | DLSS5-Feeder | this bridge |
| --- | --- | --- |
| motion vectors | optical flow, estimated from the image — sees **things that move on their own** | camera reprojection from real depth and real matrices — exact for the world, blind to moving objects |
| depth | ReShade Generic Depth | read straight out of the game's framebuffer |
| APIs | D3D11, D3D12, Vulkan (+ D3D9 via dgVoodoo2, 32-bit via a helper) | **OpenGL** |
| needs | a `.fx` and an optical-flow provider installed and enabled | nothing but itself |

**OpenGL is not on its list**, which is the reason this bridge exists. But its
add-on does initialise on ReShade's OpenGL runtime, so if you can get
`DLSS5_Feed.fx` and a motion-vector provider compiling there, it is the better
tool and you should use it instead.

**Do not run both at once.** Each opens its own private D3D12 NGX session and
drives the same `NVSDK_NGX_D3D12_EvaluateFeature`; the DLSS 5 add-on would be fed
two unrelated feature streams. Pick one and take the other out of the folder.

## What it does

Per frame, all of it inline on the game's GL context, at the moment the scene is
finished and nothing has read it yet:

```
 blit  scene colour ------------------------------> shared Color   (RGBA16F)
 blit  scene depth  --> depth copy
 draw  depth copy + reprojection ------------------> shared Depth   (R32F)
                                              and -> shared MV      (RG16F)
 glSignalSemaphoreEXT(sem_in, v)  +  glFlush
        │
        │   D3D12 queue:  Wait(fence_in, v)
        │                 NVSDK_NGX_D3D12_EvaluateFeature   ◀── the DLSS 5
        │                 Signal(fence_out, v)                  add-on inserts
        │                                                       its pass here
 glWaitSemaphoreEXT(sem_out, v)
 blit  shared Output ------------------------------> scene colour
```

The shared textures are created on the D3D12 side (`HEAP_FLAG_SHARED` +
`ALLOW_SIMULTANEOUS_ACCESS`) and imported into OpenGL as texture objects backed
by `GL_EXT_memory_object_win32` memory objects. The two shared D3D12 fences are
imported as `GL_EXT_semaphore_win32` semaphores. The private D3D12 device is
chosen by **LUID** so it lands on the same physical GPU as the GL context —
cross-API shared handles require it.

OpenGL has one implicit command stream per context, like D3D11's immediate
context and unlike Vulkan, so the signal and the wait are ordinary calls placed
inline. None of the Vulkan port's worker thread, `VkEvent` sandwich or
submit-granularity handoff is needed here.

The DLSS 5 add-on is not modified or patched. It simply starts receiving the
D3D12 evaluate it was always waiting for.

## The three things you should know before installing

**1. It runs as DLAA, not as an upscaler.** There is no generic way to make an
OpenGL game render smaller than its window — the default framebuffer *is* the
window, and an offscreen scene target is composited at its own size. So the
bridge always runs 1:1. That is the right operating point anyway: the purpose is
to give a neural-rendering add-on a D3D12 evaluate to attach to, and DLAA at
native resolution is exactly what such an add-on wants. It is not going to give
you frames back.

**2. There is no jitter.** DLSS reconstructs sub-pixel detail from a projection
matrix the game offsets slightly every frame. A game with no temporal pass of its
own does not do that, and this bridge does not modify the game's projection to
make it. `Jitter.Offset` is therefore reported as zero, truthfully — declaring a
jitter that was never applied would tell DLSS to undo a shift that is not in the
image, which is worse than declaring none. DLSS still runs and still resolves;
it has less to work with than it would in a game that integrated it properly.

**3. Motion vectors are reprojected, not measured.** Given depth and the
view-projection of this frame and the last, every pixel can be put back where it
was. That is exact for everything that did not move on its own — which is the
camera motion DLSS most needs to follow — and wrong for anything that did. A
moving character reprojects as though it had stood still, and DLSS resolves it as
if the camera had moved past it. Expect that to show on fast-moving objects
against a static background.

Getting the matrices at all needs one of:

* **`mv_vp`**, or **`mv_view`** + **`mv_proj`** — the names of the game's matrix
  uniforms. Exact, and by far the best answer for anything shader-based. You have
  to find the names in the game's shader source or its shader mod.
* **the fixed-function matrix stack** (`mv = 1`) — read at the frame's first
  draw. Correct when the first thing drawn is world geometry submitted with the
  camera matrix alone, which is what a fixed-function engine does and no
  shader-based one does. **Opt-in, and not safe everywhere:** catching the first
  draw means rewriting the first bytes of `glDrawArrays`, `glDrawElements` and
  `glBegin` once a frame, and a game that issues GL calls from a second thread
  will eventually be inside one of them when that happens. MX Bikes crashed on a
  loading screen this way. Use it only on an old single-threaded engine, and set
  `mv = 0` at the first sign of trouble. It retires itself after 300 frames if it
  is finding nothing.
* **nothing** — `mv = 3`. Zero motion vectors: a stable picture that smears when
  the camera moves. Worse in motion, never wrong.

## Requirements

In the game folder, alongside the game executable:

| File | Where from |
| --- | --- |
| `opengl32.dll` — ReShade 6.8+ **with add-on support**, installed for **OpenGL** | reshade.me, full version |
| a DLSS 5 Neural Rendering ReShade add-on | its own author |
| `nvngx_dlssnr.dll` | shipped with that add-on |
| `dlss5-opengl-bridge.addon64` | this package |

The DLSS 5 add-on's own neural-rendering toggle has to be enabled, either in its
ReShade overlay panel or in `ReShade.ini`.

**It has to be the OpenGL ReShade, not a DirectX one.** ReShade is installed as
whichever DLL the game's graphics API loads: `opengl32.dll` for OpenGL,
`dxgi.dll` or `d3d11.dll` for Direct3D. An OpenGL game never loads the DirectX
one, so a DirectX install does nothing at all — no ReShade, no add-ons, no
bridge. Pick OpenGL in the ReShade installer and check that
`opengl32.dll` ends up next to the game executable. `ReShade.log` confirms it:

```
Initializing crosire's ReShade version '6.8.0.…' loaded from 'C:\…\OPENGL32.dll'
```

The private D3D12 device the bridge creates is internal to the bridge and needs
no ReShade of its own.

The bridge itself needs an NVIDIA RTX GPU with a DLSS-capable driver, and an
OpenGL **3.3 or newer** context that offers `GL_EXT_memory_object_win32` and
`GL_EXT_semaphore_win32`. Every NVIDIA driver that can run DLSS offers those, so
if they are missing the context is not on the NVIDIA GPU — on a laptop that means
the game is rendering on the integrated part, and the fix is in the NVIDIA
Control Panel.

Unlike the other two bridges, nothing in an OpenGL process loads NGX on its own,
so the bridge asks for `_nvngx.dll` by name when it opens its D3D12 session. The
driver puts it on the DLL search path; if it will not load, the log says so.

## Install

Drop `dlss5-opengl-bridge.addon64` next to ReShade. On first run it writes
`dlss5-opengl-bridge.cfg` with working defaults; nothing needs configuring to
get a picture.

To remove it, delete the file.

Nothing on disk is patched. The only writes to foreign code are 14 bytes at up to
six function entry points, in memory, restored around every call and put back at
unload.

## Configuration

`dlss5-opengl-bridge.cfg` is re-read while the game runs, so values can be
changed without restarting. Changes that only take effect on a new NGX feature
trigger a rebuild automatically.

| Key | Default | Meaning |
| --- | --- | --- |
| `mode` | 0 | `0` bridge normally. `1` do everything except the D3D12 evaluate — an A/B test: the picture should be unchanged and the cost should vanish. `2` transport self-test: copy the shared Color straight back instead of the DLSS output; an unchanged picture proves the whole OpenGL round trip and narrows any remaining fault to the D3D12 side. `3` fully inert — loaded and logging, with not one byte written to anybody else's code. This is the setting that proves the add-on is not the cause of something. |
| `source` | 0 | `0` the back buffer, immediately before the swap — always works, and puts the interface through the neural pass along with the scene, which is visible on text. `1` find the scene target: the largest framebuffer drawn into this frame that has depth, bridged at the moment the game stops drawing into it. `n` that exact framebuffer name; the log lists the candidates. **Needs a restart** — see below. |
| `mv` | 0 | Where the view-projection comes from. `0` auto — the uniforms below if they are named, nothing otherwise. `2` the uniforms, required. `3` none. `1` reads the **fixed-function matrix stack**, which needs the draw entry points patched once a frame; that is safe only in a single-threaded old engine, so it is never chosen automatically — see below. |
| `mv_vp` | *(empty)* | Name of a combined view-projection matrix uniform. **Needs a restart.** |
| `mv_view` | *(empty)* | Name of a view / modelview matrix uniform, used with `mv_proj`. **Needs a restart.** |
| `mv_proj` | *(empty)* | Name of a projection matrix uniform. **Needs a restart.** |
| `flags` | -1 | `-1` decide the DLSS create flags from the source buffers (recommended), or a literal value to force them. |
| `hdr` | -1 | `-1` set `IsHDR` when the source is a floating-point target. `0`/`1` force. |
| `depth_inverted` | -1 | `-1` no, which is the OpenGL default. Set `1` only for a game using `glClipControl` for reverse-Z. |
| `subrects` | 1 | Pass `DLSS.Enable.Output.Subrects` to the feature. |
| `reset_every` | 0 | `1` discards temporal history every frame. Diagnostic only. |
| `verbose` | 0 | Extra per-frame logging. |

### The four settings that need a restart

`source` and the three `mv_*` names are read once, when the add-on loads. They
decide whether `wglGetProcAddress` is hooked, and a game resolves its OpenGL
entry points during startup — a wrapper handed out after that would never be
called. Everything else in the file is live.

With the defaults (`source = 0`, no matrix uniform named) `wglGetProcAddress` is
**not hooked at all**, and the add-on hands the game nothing. That is the
smallest possible footprint and the state to start from.

### Which `source` to use

`source = 0` is the default because it always works. Its cost is that the
interface is in the image: text and HUD elements have no motion of their own but
sit on depth the camera is moving through, so they get the background's motion
vector and smear when you turn.

`source = 1` is worth trying in any game that renders its world offscreen — most
modern OpenGL engines, and every emulator with an internal-resolution setting. It
finds the scene target and bridges it at the moment the game binds the default
framebuffer to composite, which is before anything reads the result. If it picks
the wrong framebuffer the log lists every candidate with its size and format, and
`source = n` names one directly.

## Log

`dlss5-opengl-bridge.log` is written next to the add-on and records the OpenGL
context and driver, the extensions and entry points that were and were not found,
which `opengl32.dll` the process is actually using, the scene target that was
picked and why, where the view-projection is coming from, the result of every NGX
call, whether anything has detoured the D3D12 evaluate, and a timing line every
600 frames.

Two lines in it answer most questions on their own:

* **`D3D12 EvaluateFeature entry (...): -- not detoured`** means no DLSS 5 add-on
  has attached. The bridge is then running plain driver DLAA with nothing riding
  on it, which is not what you installed it for. Check that the add-on and
  `nvngx_dlssnr.dll` are in the folder and that its neural toggle is on.
* **`no view-projection matrix could be found`** means motion vectors are zero.
  See the `mv_*` settings above.

## Building

Windows SDK and MSVC. No external dependencies: the OpenGL entry points are
resolved at runtime through `wglGetProcAddress`, the NGX interfaces are declared
inline, and the ReShade add-on API is reached through `GetProcAddress`.

From the `src` folder. The `.h` and `.inc` files are pulled in by the `.cpp` and
are not compiled separately.

```
rc /nologo version.rc
cl /nologo /LD /EHsc /O2 /MT dlss5-opengl-bridge.cpp ^
   /link /OUT:dlss5-opengl-bridge.addon64 version.res ^
   kernel32.lib user32.lib gdi32.lib advapi32.lib
```

Or with CMake:

```
cmake -B build -S . -A x64
cmake --build build --config Release
```

The version lives in two places that have to stay in step: `BRIDGE_VERSION` in
the `.cpp`, which is what the log prints, and the numbers in `version.rc`, which
is what ReShade's overlay shows.

## How it hooks

Two techniques, chosen per function by how often it is called.

**Patched entry points** — `wglSwapBuffers`, `SwapBuffers`, `wglGetProcAddress`,
and (only in a compatibility context, only when the fixed-function matrix stack
is wanted) `glDrawArrays`, `glDrawElements` and `glBegin`. A 14-byte absolute
jump, with the original bytes restored around every forwarded call: the same
technique both other bridges use on the NGX exports. The draw entry points would
be ruinous to patch this way — a busy frame calls them thousands of times — so
that hook **takes itself back out on its first call** and is re-armed once per
frame. Its cost is two patch operations and one forwarded call per frame,
whatever the game's draw count.

**Wrapped through `wglGetProcAddress`** — `glBindFramebuffer`, `glUseProgram`,
`glUniformMatrix4fv`, `glGetUniformLocation`. Everything above OpenGL 1.1 reaches
a game through `wglGetProcAddress`, so the game is simply handed a wrapper that
calls the driver's function. One indirect call of overhead, and no patching at
all, which is what makes it affordable on `glUniformMatrix4fv`.

Nothing the bridge itself calls goes through those wrappers: it resolves its own
entry points against the unpatched `wglGetProcAddress`, so the framebuffer
tracker never sees the bridge's own binds and the matrix watcher never mistakes
the bridge's program for the game's.

The host executable is never patched.

### Why the add-on pins itself

ReShade does not load an add-on once. It builds and tears down its add-on list
around every runtime it creates, and a game that makes a dummy OpenGL context
before its real one produces several full load/unload cycles before rendering
starts — four of them in MX Bikes, inside two seconds. `ReShade.log` shows them
plainly:

```
Loading add-on from '...' ...  Registered add-on "..."
Unloading add-on "..." ...     Unregistered add-on "..."      ← FreeLibrary
Loading add-on from '...' ...  Registered add-on "..."
```

An add-on that only listens to events survives that. One that installs hooks does
not, and version 1.0.0 did not: its watch thread was sleeping inside the module
when `FreeLibrary` unmapped it, and the wrappers it had handed the game pointed
into the same hole. Both are access violations in the game's own call stack,
where this add-on's exception handler cannot see them, a few seconds after
launch — which is exactly what it looked like.

So the module raises its own reference count with
`GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN, …)` before it installs
anything. `FreeLibrary` can then never unmap it, and every patch, thread and
handed-out pointer stays valid for the life of the process.

The cost is that ReShade's later reload finds a module whose `DllMain` does not
run again, so the add-on stops appearing in ReShade's registered add-on list
after the first cycle. Nothing here depends on being registered — no ReShade
event is subscribed to and the hooks are already in — so the log is the place to
confirm it is alive, not the overlay.

## Known limits

- **No title confirmed working end to end yet.** The D3D12/NGX half is proven
  code from the DX11 bridge; the OpenGL edge is new, and 1.0.0's first contact
  with a real game found a crash rather than a picture.
- **The add-on stops appearing in ReShade's add-on list** after ReShade's first
  reload cycle, because it pins itself. That is expected; the log is where to
  check that it is alive.
- **DLAA only.** See above — there is no generic way to make an OpenGL game
  render smaller.
- **No jitter**, so DLSS has less sub-pixel information than it would in a game
  that integrated it.
- **Motion vectors are camera-only.** Objects that move independently ghost.
- **One scene target per frame.** A game that renders several views (a split
  screen, a mirror, a security monitor) has one of them bridged.
- **No exposure texture**, so the feature is created with `AutoExposure`.
- **MSAA sources are resolved on the way in.** It works, but turning MSAA off
  costs nothing here and saves the resolve — DLSS is doing the antialiasing now.
- **`glReadBuffer` / `glDrawBuffers` are set and restored around every blit**, so
  a multiple-render-target scene target is safe; per-buffer *colour masks* are
  not saved, which would matter only to a game using indexed colour masks across
  a frame boundary.
- **Single GPU only.** Shared handles require one physical device.

If anything goes wrong the bridge disables itself and the game renders on its
own; it never leaves a broken frame on screen deliberately.

## Credit and license

The private D3D12 NGX session — the version negotiation, the snippet search
paths, the create-failure repairs, the parameter-forwarding discipline, the
crash and robustness handling — is **NIGos' DLSS 5 DX11 Bridge**, by way of
**AlanBacker's Vulkan port**. Every line of it was paid for by somebody's bug
report and there was no reason to rediscover any of it.

> <https://github.com/NIGos/dlss5-dx11-bridge>
> <https://github.com/AlanBacker/dlss5-vk-bridge>

Licensed **MIT** (see [LICENSE](LICENSE)), preserving both original copyrights.

Not affiliated with or endorsed by NVIDIA. DLSS is a trademark of NVIDIA
Corporation. "NGX" symbol names are used only for interoperability.
