# Native Horizon OpenGL probe

`mineswitch-gl.nro` requests a **desktop OpenGL 3.2 core** context through EGL
on the default Switch window at 1280×720. Mesa may return a newer compatible
core context; the actual version and profile are logged.

The probe logs EGL/OpenGL vendor and version, renderer, GLSL version and maximum
texture size. It compiles GLSL 150 vertex/fragment shaders, links a program,
uploads a VAO/VBO and draws an interpolated RGB triangle. Before presentation,
it reads three pixels from the real back buffer:

| Location | Expected RGBA, ±3/channel | Purpose |
| --- | --- | --- |
| Triangle center (640, 360) | 64, 64, 128, 255 | Shader, buffer, draw and interpolation check |
| Background (8, 8) | 8, 16, 32, 255 | Clear and framebuffer readback check |
| Far corner (1279, 719) | 8, 16, 32, 255 | Verify readback reaches the requested buffer extent |

Native window dimensions must be exactly 1280×720. EGL dimensions are queried
before and after the first draw, and any nonzero mismatch is a failure. Mesa's
Switch EGL backend leaves its generic `_EGLSurface.Width/Height` fields zero:
it instead sizes actual buffers from `nwindowGetDimensions` via
`fb->template.width0/height0` in `switch_create_window_surface`. Consequently,
`eglQuerySurface` reporting 0×0 does not demonstrate a zero-sized framebuffer.
For that case, this probe requires the correct native dimensions **and** real
center, background, and far-corner pixel readbacks. An out-of-bounds read leaves
the zero-initialized pixel sentinel unchanged and cannot pass the color check.

The original 0.1.0 probe stopped too early on this EGL metadata quirk; 0.1.1
performs the actual rendering checks instead. The official Switchbrew triangle
example does not rely on an EGL surface-size query.

It presents frames with vertical synchronization for 20 seconds, or until **+**
is pressed, then releases its resources and returns to the homebrew launcher.
Frame counts are a liveness check, not an estimate of Minecraft performance.

Process memory budget/commitment and allocator usage are reported before EGL,
after scene initialization, after the first draw, and after EGL cleanup.
The process committed-memory number includes the heap reserved by libnx.
`mallinfo` measures the C allocator and does not account for every GPU/driver
allocation. Neither is a measurement of Java heap capacity.

## Build and launch

Required devkitPro packages: `devkitA64`, `libnx`, `switch-tools`, `switch-mesa`,
`switch-glad` and their dependencies (`switch-libdrm_nouveau`, etc.). GLM is not
used. Link libraries match the official Switchbrew OpenGL triangle example:
`-lglad -lEGL -lglapi -ldrm_nouveau -lnx`.

With `DEVKITPRO`, `DEVKITA64` and their tool directories set in `PATH`:

```sh
make -C graphics
nxlink -s -a SWITCH_IP graphics/mineswitch-gl.nro
```

Use application mode for development of a future Minecraft runtime. Applet mode
has a much smaller memory budget. Network logging uses its own `FILE *`; the
program does not redirect standard streams or initialize the libnx console,
which cannot share the window with EGL. The network stream closes before
`socketExit` to avoid using a removed socket device during newlib exit cleanup.

The shader/draw and pixel checks establish that this minimal rendering path
works. They do not establish full LWJGL, Minecraft renderer, or JVM compatibility.

References:

- https://github.com/switchbrew/switch-examples/tree/master/graphics/opengl/simple_triangle
- https://github.com/switchbrew/switch-examples/blob/master/graphics/opengl/README.md
- Mesa source inspected at commit `05fc4b1d449b8fe12cf1a47b46c4a1c1f4e4e3a6`:
  https://github.com/devkitPro/mesa/blob/05fc4b1d449b8fe12cf1a47b46c4a1c1f4e4e3a6/src/egl/drivers/switch/egl_switch.c
