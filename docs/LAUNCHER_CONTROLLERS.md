# Controller mods for Minecraft 1.21.1

Source and release metadata checked on 2026-10-06. **MidnightControls is the first
candidate to test with this port's GLFW backend.** This is a source-level
recommendation, not a claim that any of these mods has run successfully on
Horizon. No mod binary was downloaded or executed for this review.

| Mod and inspected version | Actual input backend | MineSwitch implication |
| --- | --- | --- |
| MidnightControls **1.10.0.1**, Fabric/Quilt or NeoForge for 1.21/1.21.1 | Direct LWJGL GLFW calls: `glfwJoystickPresent`, `glfwJoystickIsGamepad`, `glfwGetGamepadState`, names, GUIDs and mappings. | Closest match to the existing Horizon GLFW implementation. Use the release for the selected loader and its declared dependencies; gameplay and loader compatibility remain unverified. |
| Controlify **3.0.1+lts**, Fabric or NeoForge for 1.21/1.21.1 | Prefers SDL3 through `libsdl4j`/JNA. `Controlify.initializeControlify()` selects `GLFWControllerManager` when SDL native loading returns false. Separate `hid4java` discovery starts earlier. | A real GLFW fallback exists, so calling this mod “SDL-only” would be incorrect. Its JNA/HID initialization and error handling still need Horizon validation before recommending it. Desktop Linux AArch64 SDL binaries are not Horizon binaries. |
| Controllable **0.25.4**, source tag `v1.21.1-0.25.4`, Fabric/NeoForge targets | Defaults to SDL2 through `controllable-sdl`/JNA. Its client `inputLibrary` setting explicitly selects either `SDL2ControllerManager` or `GLFWControllerManager`. | The `GLFW` setting is a concrete candidate path; changing it requires a game restart. The default SDL2 path is not supplied by our GLFW backend. The complete mod and its Framework dependency remain unverified on Horizon. |

MidnightControls and Controllable read SDL GameControllerDB **mapping data** for
GLFW. That text format does not itself require SDL native code. Conversely,
linking SDL2 into the NRO for another subsystem does not make arbitrary Java
SDL/JNA bindings usable by a mod.

Use one controller mod at a time during initial validation. Keep the port's
legacy controller-to-keyboard synthesis disabled to avoid duplicate actions.
The GLFW path provides buttons, sticks, hats, mappings and connection events;
it does not establish support for rumble, gyro, adaptive triggers or a mod's
desktop HID extensions. A single horizontal Joy-Con also cannot supply the
two-stick layout normally expected by these mods. See the
[Horizon controller contract](../runtime/glfw/CONTROLLERS.md) for the implemented
layouts and the distinction between host logic checks and device validation.

References are pinned where source identity matters:

- MidnightControls release metadata: [Fabric/Quilt 1.10.0.1](https://modrinth.com/mod/midnightcontrols/version/mZMBB8jx), [NeoForge 1.10.0.1](https://modrinth.com/mod/midnightcontrols/version/oz41N6Dp). Matching source revision `e022fd1`: [version properties](https://github.com/TeamMidnightDust/MidnightControls/blob/e022fd1827da0805a0e0a72ccabb3ef4371d04bd/gradle.properties) and [Controller.java](https://github.com/TeamMidnightDust/MidnightControls/blob/e022fd1827da0805a0e0a72ccabb3ef4371d04bd/common/src/main/java/eu/midnightdust/midnightcontrols/client/controller/Controller.java).
- Controlify release metadata: [Fabric 3.0.1+lts](https://modrinth.com/mod/controlify/version/y9bu5RxH), [NeoForge 3.0.1+lts](https://modrinth.com/mod/controlify/version/RqsNKKLK). LTS revision `fd81677`: [version/dependencies](https://github.com/isXander/Controlify/blob/fd816773a67cc41d4fa242911eae4cc6e36bca12/gradle.properties), [backend selection](https://github.com/isXander/Controlify/blob/fd816773a67cc41d4fa242911eae4cc6e36bca12/src/main/java/dev/isxander/controlify/Controlify.java#L250), [SDL native loader](https://github.com/isXander/Controlify/blob/fd816773a67cc41d4fa242911eae4cc6e36bca12/src/main/java/dev/isxander/controlify/driver/sdl/SDLNativesLoader.java), and [separate HID service](https://github.com/isXander/Controlify/blob/fd816773a67cc41d4fa242911eae4cc6e36bca12/src/main/java/dev/isxander/controlify/hid/ControllerHIDService.java).
- Controllable [1.21.1-0.25.4 source tag](https://github.com/MrCrayfish/Controllable/tree/v1.21.1-0.25.4), revision `95a8a81`: [version/dependencies](https://github.com/MrCrayfish/Controllable/blob/95a8a81597afe8218670607efd2ac7da088083f7/gradle.properties), [inputLibrary setting and SDL2 default](https://github.com/MrCrayfish/Controllable/blob/95a8a81597afe8218670607efd2ac7da088083f7/common/src/main/java/com/mrcrayfish/controllable/Config.java#L33), [backend selection](https://github.com/MrCrayfish/Controllable/blob/95a8a81597afe8218670607efd2ac7da088083f7/common/src/main/java/com/mrcrayfish/controllable/Controllable.java#L110), [GLFW input](https://github.com/MrCrayfish/Controllable/blob/95a8a81597afe8218670607efd2ac7da088083f7/common/src/main/java/com/mrcrayfish/controllable/client/input/glfw/GLFWController.java), and [SDL2/JNA path](https://github.com/MrCrayfish/Controllable/blob/95a8a81597afe8218670607efd2ac7da088083f7/common/src/main/java/com/mrcrayfish/controllable/client/input/sdl2/SDL2ControllerManager.java).
