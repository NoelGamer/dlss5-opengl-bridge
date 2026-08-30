# DLSS 5 OpenGL Bridge

A ReShade add-on that lets a DLSS 5 Neural Rendering add-on — which only hooks
DirectX 12 — run inside a game that renders with OpenGL.

An OpenGL port of [NIGos' DLSS 5 DX11 Bridge][dx11] and, through it, of
[AlanBacker's Vulkan port][vk]. The D3D12 and NGX half is theirs, near enough
verbatim.

[dx11]: https://github.com/NIGos/dlss5-dx11-bridge
[vk]: https://github.com/AlanBacker/dlss5-vk-bridge

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

## Known limits

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
