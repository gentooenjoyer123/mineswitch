# Horizon controller support

The backend reads real libnx `PadState` snapshots through public HID APIs. It
configures the standard Npad styles and independently observes Handheld and
player slots 1–8. It allocates a GLFW joystick only when a supported Npad state
has `HidNpadAttribute_IsConnected`; no default or synthetic controller exists.
Handheld and player 1 are never merged.

Supported logical styles are Pro Controller / FullKey, Handheld, paired Joy-Con,
single left Joy-Con, and single right Joy-Con. A system-assigned JoyDual slot
remains one logical controller when only one half is currently connected; the
missing half contributes no input. A FullKey-compatible third-party controller
has the FullKey layout; the name is a style description, not a manufacturer or
serial-number assertion. The backend does not pair, split or merge devices on
its own. Pairing and player assignment remain Horizon operations.

GUIDs identify our versioned Horizon input layouts, rather than claiming USB or
Bluetooth hardware IDs that the Npad API does not provide:

| Style | GUID | Raw axes |
| --- | --- | --- |
| Pro / FullKey | `6e780000010000000000000100000000` | 4 |
| Handheld | `6e780000010000000000000200000000` | 4 |
| Joy-Con pair | `6e780000010000000000000400000000` | 4 |
| Joy-Con left | `6e780000010000000000000800000000` | 2 |
| Joy-Con right | `6e780000010000000000001000000000` | 2 |

Raw buttons 0–11 follow libnx A, B, X, Y, StickL, StickR, L, R, ZL, ZR, Plus,
Minus. Buttons 12–15 are LeftSL, LeftSR, RightSL, RightSR. There is one real
D-pad hat. Stick direction pseudo-buttons are not exported as additional hats.
GLFW's normal `GLFW_JOYSTICK_HAT_BUTTONS` initialization hint still controls
whether four extra hat-derived buttons appear in the raw button array.

Raw sticks use the libnx calibrated range, normalized and clamped to [-1, 1],
with Y inverted for GLFW. Four-axis layouts are left X/Y, right X/Y; singles
have only their own X/Y. No application deadzone is imposed. Gamepad mappings
use GLFW's positional button convention: on a full controller, Switch B is
GLFW A / south, Switch A is GLFW B / east. ZL/ZR are real digital trigger
buttons, exposed as -1 released / +1 pressed on the gamepad trigger axes.
Single Joy-Con gamepad mappings assume horizontal use; SL/SR are bumpers,
their sole stick is mapped to left X/Y, and absent controls remain neutral.
A single Joy-Con cannot provide the two-stick controls expected by most
Minecraft controller mods.

The upstream `glfwUpdateGamepadMappings` parser and lookup are enabled, including
the `platform:Switch` filter. Mapping changes really replace the lookup entries.
Desktop mappings with unrelated GUIDs do not alter Horizon Npad layouts.
`glfwPollEvents` discovers additions even without a window. Joystick getters
also refresh their device's HID state and detect removal. A style change sends
disconnect followed by connect, as the device's name, GUID and capabilities
may change. Device IDs are GLFW slot assignments, not permanent player IDs.
All these GLFW APIs retain their normal main-thread requirement.

Controller-to-keyboard synthesis defaults off to avoid duplicate actions when
a controller mod consumes gamepad input. The previous arrow/X/Z/S/A/Enter/Escape
compatibility mapping is available with `MINESWITCH_GLFW_LEGACY_KEYS=1`, using
the first connected device (Handheld preferred). Touch remains independent and
maps to the actual window dimensions. The existing Plus+Minus+L+R close request
is retained for that primary controller. There was no physical USB keyboard
backend in the imported Switch fork; this change does not add one.

## Minecraft mod boundary

This supports mods that use LWJGL's **GLFW joystick or gamepad API**, provided
their Java code accepts the Horizon runtime and its GLFW platform. It is not
proof that a particular mod works: mod loaders, native-library selection and
the mod's platform checks must also be compatible.

Mods using SDL2/SDL3 through JNA, hidapi, JInput, or desktop-only native binaries
do not gain support from a GLFW patch. The NRO's existing SDL2 audio dependency
does not expose arbitrary SDL Java bindings to mods. Rumble and gyroscope data
are also outside GLFW's gamepad API, and HOME/capture are owned by the system.
Do not select a mod solely because its description says “controller support”;
check the backend used by the exact Minecraft 1.21.1 mod version.

## Verification

```
python3 runtime/limit-build.py -- bash runtime/glfw/build.sh
python3 runtime/limit-build.py -- python3 runtime/glfw/tests/replay.py
python3 runtime/limit-build.py -- python3 runtime/glfw/prepare-controller-probe.py
```

The host replay compiles the actual backend and actual GLFW mapping/joystick
code, substituting only libnx and unrelated runtime glue. ASan/UBSan cover
empty startup, independent slots, fresh values, real mapping replacement,
callbacks, style replacement, single Joy-Con controls, touch, opt-in key events
and cleanup. It is a logic test, not a device connection claim.

The Java integration artifact is
`runtime/glfw/build/controller-probe/controller-probe.jar`; its main class is
`mineswitch.probe.ControllerProbe`. Use the existing Horizon LWJGL core/GLFW
classpath and the rebuilt static GLFW library. The probe creates no window and
polls for 30 seconds by default. Options:

- `--seconds=60`: longer interactive interval, maximum 300.
- `--require-controller`: fail if no physical controller was observed.
- `--require-input`: additionally require a button/D-pad press and stick movement.

The probe logs identities, raw counts, mapped axes and genuine hotplug events;
it validates ranges, digital triggers and a temporary mapping replacement. The
replacement is discarded at GLFW termination. It injects no device state.
Connect/disconnect events need an actual controller transition during the run;
initial devices are enumerated, not falsely reported as new hotplug events.

Device validation is pending. The root agent owns staging, NRO linking and
console execution. No FPS measurement, overclock or console changes are part
of this work.
