package mineswitch.probe;

import java.nio.ByteBuffer;
import java.nio.FloatBuffer;
import java.util.Arrays;
import org.lwjgl.glfw.GLFWErrorCallback;
import org.lwjgl.glfw.GLFWGamepadState;
import org.lwjgl.glfw.GLFWJoystickCallback;
import org.lwjgl.system.MemoryStack;
import static org.lwjgl.glfw.GLFW.*;

/** Real GLFW/LWJGL controller exercise; it never injects controller input. */
public final class ControllerProbe {
    private static int samples, connections, disconnections, errors;
    private static boolean sawController, sawStick, sawButton;
    private static final String[] identities = new String[GLFW_JOYSTICK_LAST + 1];
    private static final long[] lastButtons = new long[GLFW_JOYSTICK_LAST + 1];
    private static final long[] lastReports = new long[GLFW_JOYSTICK_LAST + 1];

    private static void require(boolean result, String message) {
        if (!result) throw new AssertionError(message);
    }

    private static void verifyMappingUpdate(GLFWGamepadState state) {
        for (int jid = GLFW_JOYSTICK_1; jid <= GLFW_JOYSTICK_LAST; ++jid) {
            String guid = glfwGetJoystickGUID(jid);
            if (guid == null) continue;
            try (MemoryStack stack = MemoryStack.stackPush()) {
                require(glfwUpdateGamepadMappings(stack.ASCII(guid + ",MineSwitch mapping probe," +
                    "a:b0,leftx:a0,platform:Switch,")), "Mapping parser rejected valid data");
            }
            // This deliberately changes the real mapping, after input sampling.
            // Termination below discards it; no other application is running here.
            require("MineSwitch mapping probe".equals(glfwGetGamepadName(jid)),
                "Mapping update was ignored");
            require(glfwGetGamepadState(jid, state), "Updated mapping is unusable");
            System.out.println("CONTROLLER_MAPPING_UPDATE_PASS jid=" + jid);
            return;
        }
        System.out.println("CONTROLLER_MAPPING_UPDATE_SKIP no_connected_device");
    }

    private static void sample(int jid, GLFWGamepadState gamepad, long now) {
        if (!glfwJoystickPresent(jid)) {
            identities[jid] = null;
            return;
        }
        String name = glfwGetJoystickName(jid), guid = glfwGetJoystickGUID(jid);
        FloatBuffer axes = glfwGetJoystickAxes(jid);
        ByteBuffer buttons = glfwGetJoystickButtons(jid), hats = glfwGetJoystickHats(jid);
        // A physical disconnect may happen between separate public API calls.
        if (name == null || guid == null || axes == null || buttons == null || hats == null)
            return;
        require(guid.matches("[0-9a-f]{32}"), "Missing/invalid GUID: " + guid);
        require(axes.remaining() == 2 || axes.remaining() == 4, "Unexpected raw axis count");
        require(buttons.remaining() == 16 && hats.remaining() == 1, "Unexpected raw input counts");
        if (!glfwJoystickIsGamepad(jid)) {
            if (!glfwJoystickPresent(jid)) return;
            throw new AssertionError("No built-in gamepad mapping: " + guid);
        }
        if (!glfwGetGamepadState(jid, gamepad)) return;
        sawController = true;
        ++samples;

        String identity = name + " / " + guid;
        if (!identity.equals(identities[jid])) {
            identities[jid] = identity;
            System.out.println("CONTROLLER_DEVICE jid=" + jid + " name=" + name +
                " guid=" + guid + " raw_axes=" + axes.remaining() +
                " raw_buttons=" + buttons.remaining() + " hats=" + hats.remaining() +
                " gamepad_name=" + glfwGetGamepadName(jid));
        }

        long pressed = 0;
        for (int i = 0; i < buttons.remaining(); ++i) {
            byte value = buttons.get(i);
            require(value == GLFW_PRESS || value == GLFW_RELEASE, "Invalid raw button state");
            if (value == GLFW_PRESS) pressed |= 1L << i;
        }
        require((hats.get(0) & ~15) == 0, "Invalid raw hat state");
        sawButton |= pressed != 0 || hats.get(0) != 0;
        for (int i = 0; i < axes.remaining(); ++i) {
            float value = axes.get(i);
            require(Float.isFinite(value) && value >= -1 && value <= 1, "Invalid raw stick value");
            sawStick |= Math.abs(value) > 0.25f;
        }
        for (int i = 0; i <= GLFW_GAMEPAD_BUTTON_LAST; ++i) {
            byte value = gamepad.buttons(i);
            require(value == GLFW_PRESS || value == GLFW_RELEASE, "Invalid mapped button state");
        }
        float[] mapped = new float[GLFW_GAMEPAD_AXIS_LAST + 1];
        for (int i = 0; i < mapped.length; ++i) {
            mapped[i] = gamepad.axes(i);
            require(Float.isFinite(mapped[i]) && mapped[i] >= -1 && mapped[i] <= 1,
                "Invalid mapped axis value");
        }
        require(mapped[GLFW_GAMEPAD_AXIS_LEFT_TRIGGER] == -1 ||
            mapped[GLFW_GAMEPAD_AXIS_LEFT_TRIGGER] == 1, "ZL/L is a digital trigger");
        require(mapped[GLFW_GAMEPAD_AXIS_RIGHT_TRIGGER] == -1 ||
            mapped[GLFW_GAMEPAD_AXIS_RIGHT_TRIGGER] == 1, "ZR/ZL is a digital trigger");
        if (pressed != lastButtons[jid] || now - lastReports[jid] >= 1000) {
            lastButtons[jid] = pressed;
            lastReports[jid] = now;
            System.out.println("CONTROLLER_STATE jid=" + jid + " buttons=0x" +
                Long.toHexString(pressed) + " hat=" + hats.get(0) +
                " mapped_axes=" + Arrays.toString(mapped));
        }
    }

    public static void main(String[] args) throws Exception {
        int seconds = 30;
        boolean requireController = false, requireInput = false;
        for (String arg : args) {
            if (arg.startsWith("--seconds=")) seconds = Integer.parseInt(arg.substring(10));
            else if (arg.equals("--require-controller")) requireController = true;
            else if (arg.equals("--require-input")) requireInput = requireController = true;
            else throw new IllegalArgumentException(arg);
        }
        require(seconds >= 1 && seconds <= 300, "Probe duration must be 1..300 seconds");
        System.out.println("MINESWITCH_CONTROLLER_BEGIN seconds=" + seconds);
        try (GLFWErrorCallback error = GLFWErrorCallback.create((code, message) -> {
                 ++errors;
                 System.out.println("CONTROLLER_ERROR code=0x" + Integer.toHexString(code) +
                     " message=" + GLFWErrorCallback.getDescription(message));
             });
             GLFWJoystickCallback joystick = GLFWJoystickCallback.create((jid, event) -> {
                 if (event == GLFW_CONNECTED) ++connections;
                 else if (event == GLFW_DISCONNECTED) ++disconnections;
                 System.out.println("CONTROLLER_EVENT jid=" + jid + " event=" +
                     (event == GLFW_CONNECTED ? "connected" : "disconnected"));
             })) {
            glfwSetErrorCallback(error);
            // Test the real raw button count separately from GLFW's hat-as-buttons option.
            glfwInitHint(GLFW_JOYSTICK_HAT_BUTTONS, GLFW_FALSE);
            require(glfwInit(), "GLFW initialization");
            try (GLFWGamepadState state = GLFWGamepadState.calloc()) {
                glfwSetJoystickCallback(joystick);
                long start = System.nanoTime();
                while ((System.nanoTime() - start) / 1_000_000L < seconds * 1000L) {
                    // Intentionally no window: discovery must work before a game creates one.
                    glfwPollEvents();
                    long now = (System.nanoTime() - start) / 1_000_000L;
                    for (int jid = GLFW_JOYSTICK_1; jid <= GLFW_JOYSTICK_LAST; ++jid)
                        sample(jid, state, now);
                    Thread.sleep(20);
                }
                require(errors == 0, "GLFW reported errors");
                require(!requireController || sawController, "No physical controller was observed");
                require(!requireInput || (sawStick && sawButton),
                    "Required real stick movement and button press were not observed");
                verifyMappingUpdate(state);
                System.out.println("MINESWITCH_CONTROLLER_PASS samples=" + samples +
                    " saw_controller=" + sawController + " saw_stick=" + sawStick +
                    " saw_button=" + sawButton + " connections=" + connections +
                    " disconnections=" + disconnections);
            } finally {
                glfwSetJoystickCallback(null);
                glfwTerminate();
                glfwSetErrorCallback(null);
            }
        }
    }
}
