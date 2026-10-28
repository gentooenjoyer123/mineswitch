/** Host proof of vanilla Minecraft's optional-profiler selection only. */
package mineswitch.tools;

public final class GameModulePolicyProbe {
    public static void main(String[] args) throws Exception {
        if (ModuleLayer.boot().findModule("jdk.jfr").isPresent())
            throw new AssertionError("jdk.jfr must be absent from the Minecraft boot layer");
        Class<?> profilerInterface = Class.forName("bnl");
        Object profiler = profilerInterface.getField("f").get(null);
        if (!profiler.getClass().getName().equals("bnl$a"))
            throw new AssertionError("Vanilla did not select its NoOp profiler: " + profiler.getClass());
        if ((Boolean) profilerInterface.getMethod("d").invoke(profiler))
            throw new AssertionError("NoOp profiler incorrectly reports available");
        System.out.println("GAME_MODULE_POLICY_PASS jfr_boot_layer=absent vanilla_profiler=" + profiler.getClass().getName());
    }
}
