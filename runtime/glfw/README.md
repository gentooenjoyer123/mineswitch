# GLFW for the Horizon Minecraft port

This is the real devkitPro Switch backend at the commit in `SOURCE.json`.
The native version remains **3.3.4**; the patch does not claim a complete GLFW 3.4
implementation or a Linux/Windows window system.

`build.sh` uses devkitA64/newlib, the Switch platform, static EGL/Mesa, and no
Vulkan loader. Vulkan capability queries report unavailable through the normal
GLFW error path; no fake Vulkan driver functions are supplied.

LWJGL 3.3.3's GLFW bindings require three later entry points that this SDK port
lacked. The port backports:

- `glfwInitAllocator`: real configured allocation, reallocation and deallocation
  callbacks, with library-owned allocation sites routed through them. Configuration
  applies to the next initialization, while existing allocations retain their
  active allocator.
- `glfwGetPlatform`: returns the private Horizon platform value `0x00060006`
  after initialization.
- `glfwPlatformSupported`: reports Horizon support and rejects or returns false
  for other platforms; it does not pretend an X11, Cocoa or Win32 backend exists.

The allocator behavior is adapted from upstream GLFW 3.4 `src/init.c` under the
same GLFW license. The full source patch is `glfw-horizon.patch`.
Actual Minecraft bytecode uses `glfwGetPlatform` for a diagnostic name and window
icon selection; its unknown-platform branch logs a warning and skips the desktop
icon operation. The other two entry points are required by LWJGL's initial symbol
table. Evidence is recorded in `minecraft-api-audit.json`.

All required GLFW core binding symbols exist in the rebuilt archive. Java/GLFW
callback and rendering behavior must additionally pass the device probe.
The unused Java Vulkan wrapper is not a supported module in the 1.21.1 port.

```sh
python3 runtime/glfw/bootstrap.py
bash runtime/glfw/build.sh
```

Output: `runtime/glfw/build/libglfw3.a`. The SDK archive is left separate.

The controller backend now enumerates actual Horizon Npad sources, reports
hotplug and style changes, and supplies usable GUIDs and standard GLFW gamepad
mappings. Mapping updates no longer return success without applying data.
See [CONTROLLERS.md](CONTROLLERS.md) for layouts, mod compatibility boundaries
and the separate Java integration probe. The changes cross-compile and pass a
sanitized host replay; actual controller input on the device remains to be tested.
