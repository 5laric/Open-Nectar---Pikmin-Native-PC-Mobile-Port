# Pikmin 1 on macOS: rendering and audio map

Scope: Pikmin 1 in `games/pikmin1/`. Pikmin 2 has the same layer layout but was not reviewed.

Date and environment: 2026-10-12, M3 Max, macOS 27 (Darwin 27.0.0). Local SDL2 is `sdl2-compat` 2.32.74, which implements the SDL2 API on top of SDL3. Official SDL2 was not tested. The GL and audio results come from macOS itself, so they should carry over.

Not run in-game. The game needs a user-supplied GameCube image, which is not in this repo.

## Summary

| Area | macOS status | Evidence |
|---|---|---|
| Audio | Device opens at the game's exact spec. No macOS code needed. Output not listened to. | `audio_probe.c` |
| Rendering | Works on GL 4.1 core. The title screen renders and the game is playable. | Full-screen screenshot, GL error checks, `gl_probe.c` |

Status (2026-10-12): option A is working. The original game runs natively on macOS with OpenGL 4.1 core: the title screen renders, the game is playable, and audio plays. Changes are committed on branch `macos-port`. See "Applied" below.

Recommendation:

1. Run the existing renderer on a GL 4.1 core context on macOS (SDL2, no Metal). This is a small change set. It also gives a reference frame for any later Metal port.
2. Decide on Metal after step 1. It means rewriting the GL layer (see Options).

## Applied

Committed on branch `macos-port`. Defaults are unchanged on every platform. Non-Apple builds take their original code paths.

Renderer and window (`games/pikmin1/pc_port/`):

- `gl/pc_opengl.h`: Apple branch includes `<OpenGL/gl3.h>` and `<OpenGL/glext.h>`, defines `GL_SILENCE_DEPRECATION`, and defines `GL_STACK_OVERFLOW` and `GL_STACK_UNDERFLOW` (0x0503 and 0x0504, from SDL's `SDL_opengl.h`).
- `pc_window.cpp`, `applyGlAttrs`: on macOS, request GL 4.1 core, forward-compatible. The 3.3 compatibility request is skipped there.
- `pc_window.cpp`, context fallback: on macOS, no retry with GL 2.1. That context cannot run the renderer.
- `gl/pc_gfx.cpp`, streaming VAO: `pc_gfx_init` creates a named VAO for the streaming ring before recording its attributes. Core profiles have no default VAO. The other passes restore that VAO instead of 0, and the batch flush binds it before drawing.
- `gl/pc_gfx.cpp`, present: `pc_window_swap_buffers` binds the default framebuffer around `SDL_GL_SwapWindow` and then restores the renderer's binding. This is the fix for the black window. On macOS the swap shows the frame only when the default framebuffer is bound.
- `gl/pc_texpack.cpp`: extension queries use `glGetStringi` when it loads, because core profiles reject `glGetString(GL_EXTENSIONS)`.
- `gl/pc_gfx_shadows.inc`: the shadow mask pass restores the streaming VAO.

Build and launcher:

- `CMakeLists.txt`: X11 is linked on Linux only. The Clang relaxation flags also match `AppleClang`. The disc-image library links `iconv` on Darwin.
- `pc_port/dolphin_stubs/audio_stubs.cpp`: `Jac_NoteDemoSkipped` is a no-op in the default build. The JAudio engine that uses it is not built there.
- `pc_port/launcher/gamecube_image.cpp`: on macOS, disc file names that are not UTF-8 (Shift-JIS on this disc) are converted to UTF-8 with iconv, as Windows already does. APFS rejects the raw bytes.
- `pc_port/launcher/launcher_platform_posix.cpp`: on macOS, `executablePath()` uses `_NSGetExecutablePath` and `realpath`. Before this, it returned nothing and the launcher used the working directory, so the packaging step looked for the game in the wrong place.

Verified on this Mac (M3 Max, macOS 27):

- Full disc extraction from the USA Rev 1 ISO: 3,495 files. The launcher's packaging step passes from the root directory, and the installed copy runs by itself.
- The GL context is 4.1 core (Metal), GLSL 4.10. The first batched draw reports no GL error.
- The title screen renders and the game is playable.
- The whole window averages about 76 out of 255 across samples, and the full-screen screenshot shows the title.

Not verified: Linux and Windows, which were not built or run. The window opens at the desktop's native size because that is the original default. Fixing that would change behaviour, so it is left alone.

## Verified

Probe sources are in [`macos-probes/`](macos-probes/). Build and run:

```bash
clang -o gl_probe gl_probe.c $(sdl2-config --cflags --libs) -framework OpenGL
./gl_probe
```

```bash
clang -o audio_probe audio_probe.c $(sdl2-config --cflags --libs)
./audio_probe
```

GL context results (`gl_probe.c`):

| Request | Result |
|---|---|
| 3.3 compatibility (`pc_window.cpp:791-793`, desktop default) | Fails: "Failed creating OpenGL context at version requested" |
| 2.1 default (fallback, `pc_window.cpp:847-849`) | Works, GL 2.1, GLSL 1.20. Too old for the renderer (below). |
| 3.2 core, forward compatible | Works, GL 4.1, GLSL 4.10 |
| 4.1 core, forward compatible | Works, GL 4.1, GLSL 4.10 |

Audio results (`audio_probe.c`): the device opens at 32000 Hz, S16, 2 channels, 1024 samples. The obtained spec matches the request exactly.

Header test: `pc_opengl.h` does not compile on macOS. Line 52 includes `<GL/gl.h>`. On the case-insensitive filesystem that resolves to the project's own `pc_port/gl/gl.h`, which includes `pc_opengl.h` back. Line 53 includes `<GL/glext.h>`, which resolves to the project's `pc_port/gl/glext.h` shim. The shim does not define the GL base types, so the compile fails. Apple's SDK headers `<OpenGL/gl3.h>` and `<OpenGL/glext.h>` define the needed types. Tested with `PFNGLBINDVERTEXARRAYPROC`, `PFNGLMAPBUFFERRANGEPROC`, and `PFNGLFENCESYNCPROC`. Applied; see "Applied".

Inferred, not run: the 2.1 fallback cannot run the renderer. It uses VAOs (GL 3.0+) and `#version 140` shaders (`pc_gfx.cpp:1917`, `:2014`).

Stale comment to check: the comment above `applyGlAttrs` in `pc_window.cpp` (lines 778-781) says a core profile fails because of GLSL 1.20 `attribute`/`varying` syntax and `GL_QUADS`. A search of `pc_port/gl/` found neither. There are no `attribute` or `varying` qualifiers, no `GL_QUADS`, and no immediate-mode calls. The comment may predate current code. Only a run on a core context settles it.

## Rendering pipeline

Flow: game code, then GX calls, then `pc_gfx` state and batching, then GL draws, then present to the SDL window.

| Stage | Where | GL / SDL dependency | macOS plan |
|---|---|---|---|
| GX entry points | `dolphin_stubs/gx_stubs.cpp` (69 calls into `pc_gfx_*`) | None | Portable |
| GX FIFO replay | `pc_gfx.cpp` `pc_gfx_call_display_list_impl` (~8465), `handle_bp_reg` (~8118), `handle_xf_regs` (~8263) | None | Portable |
| Vertex submit and batching | `pc_gfx_begin/position/color/texcoord` (5850-5909), `append_primitive` (~6937), `pc_gfx_flush_batch` (~7092) | None | Portable |
| Vertex upload | VBO ring: `glMapBufferRange` + `glFenceSync` (`pc_gfx.cpp` 7013-7092), fallback `glBufferSubData` | GL 3.2 core | Core: keep. Metal: `MTLBuffer` ring gated on command-buffer completion. |
| Per-draw state | `apply_draw_state` (~7304): blend, depth, cull, alpha compare, colour mask, scissor, viewport | `glBlendFunc`, `glDepthMask`, `glColorMask`, `glScissor`, `glViewport`, and others | Core: keep. Metal: pipeline state and depth-stencil state, set per draw. |
| Logic op | `pc_gfx.cpp` ~4743-4751 | `glLogicOp` (desktop only) | Core: keep. Metal: no equivalent. The GLES path already falls back to COPY. Reuse that. |
| Shader selection | `build_tev_shader_key` (~6419), `compile_specialised_program` (~6466), `use_program_for_current_state` (~6542) | GLSL built at runtime | Needs a decision (see Options). |
| Shader sources | `gl/pc_tev_shader.cpp` (468 lines), `vertex_shader_source` (~2002), `fragment_shader_source` (~2255) | GLSL 140 desktop, 300 es on GLES | Core: keep. Metal: translate or re-emit. |
| Uniforms | `cached_uniform*` (358-423), `query_program_locations` (~2432) | `glUniform*` | Metal: argument buffers or `setBytes`. |
| Program binary cache | `program_binary_*` (~2596-2690) | `glProgramBinary`, optional, null-checked | Metal: `MTLBinaryArchive`, or drop. |
| Textures | `pc_texpack.cpp` (669 lines, 6 GL calls), `pc_gfx_init_tex_obj` (~5364), `upload_ci_texture` (~5617), wrap modes `gxWrapToGl` (~1106) | `glTexImage2D`, `glGenerateMipmap`, anisotropy by extension | Metal: `MTLTexture`, `MTLSamplerState` (`maxAnisotropy`). |
| Render target | `depth_tex_image` (~2716): RGBA8 colour, `DEPTH24_STENCIL8` | FBO | Metal: RGBA8 texture, `Depth32Float` (see gotchas). |
| Post-process | `pc_postprocess.cpp` (655 lines): bloom, SSAO, shadows, DOF, blur, FXAA-style pass. `pc_gfx_shadows.inc` (110 GL calls). | GLSL 330 core / 300 es | Metal: fullscreen render passes. |
| Native resolution blit | `glBlitFramebuffer` with `GL_LINEAR` (`pc_gfx.cpp` 1755-1787) | GL 3.0 | Metal: fullscreen sample pass. A plain blit does not scale. |
| Depth readback | `glReadPixels` (`pc_gfx.cpp:3443`) | GL | Metal: blit to a shared buffer, read after completion. |
| GPU timers | queries (`pc_gfx.cpp` 3027-3090) | `GL_TIME_ELAPSED`, optional | Metal: `MTLCommandBuffer` GPU start/end times. |
| Present | `pc_gfx_present` (~4354), `SDL_GL_SwapWindow` (`pc_window.cpp:1475`) | SDL GL swap | Metal: `CAMetalLayer` drawable. |
| Context and window | `applyGlAttrs` (`pc_window.cpp` ~786-797), context create (~843), swap interval 0 (~869) | SDL GL attributes | Metal: `SDL_WINDOW_METAL` and no GL context. |
| Function loading | `load_gl_functions` (`pc_gfx.cpp` 159-240) | `SDL_GL_GetProcAddress` | Metal: none. |

Size of the GL surface: 111 distinct `gl*` names in `pc_port/gl/` and `pc_window.cpp`, and 990 call sites outside the launcher.

GL 4.x entry points are already optional. `glInvalidateFramebuffer` (4.3) is null-checked at `pc_gfx.cpp:4411`. `glProgramBinary` is null-checked at `:2606`. On a 4.1 core context these are skipped or fall back.

## Metal gotchas

- Clip depth range. GL clip z runs over [-w, w]. Metal expects [0, w]. The projection matrices are GL-style (`C_MTXPerspective` form, `pc_gfx_set_projection` at `pc_gfx.cpp:4454`), so a z remap is needed in the vertex path.
- Shadow lookup. `pc_postprocess.cpp:120` does `lc.xyz / lc.w * 0.5 + 0.5`, which assumes GL's z range. Under Metal the `* 0.5 + 0.5` on z must go.
- Origin. Metal's framebuffer and texture origin is top-left. GL's is bottom-left. `pc_gfx.cpp` tracks GL's origin in comments (`:4570`). Every y-flip site must be listed when porting.
- Depth format. The render target uses `DEPTH24_STENCIL8` or `DEPTH_COMPONENT24` (`pc_gfx.cpp:2714-2720`), and the shadow map uses `DEPTH_COMPONENT24` (`pc_gfx_shadows.inc:192`). Apple GPUs do not support `Depth24`. Use `Depth32Float`, plus stencil if `depth_has_stencil()` (`:2706`) is true. Confirm on the M3 against the Metal feature tables.
- Sync. The `glFenceSync` VBO ring (`:7042-7074`) becomes command-buffer completion handlers.

## Audio pipeline

Default build (`PIKMIN_NATIVE_JAUDIO=OFF`, CMake `:107`):

- `pc_port/audio/pc_audio.cpp` opens one SDL device with a callback (`:505-519`): 32000 Hz, S16, stereo, 1024 frames.
- The callback `audio_callback` (`:292`) mixes stream voices, sample voices, the JAudio DMA queue, FX and Dolby delay, and a limiter. Mixing is plain C++. SDL is the only platform dependency.
- Game calls arrive through `dolphin_stubs/audio_stubs.cpp`: AI DMA wrappers (`:259-264`), sequence tracks, events, STX playback.
- Supporting modules: `pc_jam`, `pc_envelope`, `pc_instrument_bank`, `pc_wave_bank`, `pc_sequence_archive`, `pc_aram`, `pc_dsp_host`.

Alternate build (`PIKMIN_NATIVE_JAUDIO=ON`, CMake `:354`, `:415`):

- `jaudio_host.cpp` and `jaudio_sink.cpp` push PCM with `SDL_QueueAudio`. Device open runs on a worker thread and retries (`jaudio_host.cpp` ~1590-1595).
- Also SDL-only. No macOS change expected.

Not compiled: `dolphin_stubs/pc_audio.cpp` defines a second `pc_audio_init`, but CMake does not list it. Likely dead code. Check before deleting.

Platform audio code: none specific to Linux or Windows. Nothing to port.

Risks to test on hardware:

- Changing output device mid-game (AirPods, HDMI). Verify whether SDL2 reopens the device.
- Latency: the callback buffer is 1024 frames at 32 kHz, about 32 ms. Resampling to the hardware rate happens inside SDL.

Headless check: the binary accepts `--audio-self-test` (README `:585`). It reports whether each scene produces audio. Run it first, before listening.

## Options

| Option | What changes | Effort | Notes |
|---|---|---|---|
| A. GL 4.1 core via SDL2 | Apple branch in `pc_opengl.h`. Core request in `applyGlAttrs`. Shader and state fixes under core. | Small | Apple deprecated OpenGL in macOS 10.14 and adds no features. Driver bugs are possible. |
| B. Metal direct | New backend under `pc_gfx`. Rewrite the 111 GL entry points. Translate or re-emit shaders. | Large | No deprecation risk. Full control of pipeline and sync. |
| C. SDL3 GPU API | Port SDL2 calls to SDL3. SDL_GPU uses Metal underneath. Shaders via SDL_shadercross. | Large | Keeps SDL. SDL3 3.4.18 is installed here. |
| D. ANGLE (GLES over Metal) | Reuse the `PIKI_USE_GLES` path. Bundle ANGLE. | Medium | Not tested. Needs SDL2 to load EGL from ANGLE on macOS. |

Shader decision for B and C: the TEV shaders are built at runtime (`compile_specialised_program`, `:6466`). Either the translator ships in the app (glslang and SPIRV-Cross), or the generator emits Metal Shading Language directly.

## Step plan for option A

1. Done: `pc_opengl.h` Apple branch and the `GL_STACK_*` tokens.
2. Done: `applyGlAttrs` requests GL 4.1 core on macOS, and the GL 2.1 retry is skipped there.
3. Done: the full project builds with CMake (`pikmin_pc` and `pikmin_launcher`). Runtime is verified on this Mac. The renderer runs on the 4.1 core context with no GL errors on the first batched draw.
4. Not done: compare frames against a Linux build.
5. Not done: `--audio-self-test`. Audio plays, which the port's own log confirms.

The launcher's GUI requests an ES 3.0 context (`launcher_gui.cpp:1806-1810`), which SDL2 on macOS does not provide without EGL. The command-line install (`--rom`, `--install-dir`, `--extract-only`) works. The GUI is not run.

## Open questions

- Does the port build and run on Linux and Windows with these changes? Not tested.
- Official SDL2 behavior versus `sdl2-compat` is untested.
- Should the default window size change on macOS? Left as the original (native size) at the user's request.
