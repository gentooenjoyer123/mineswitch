package mineswitch.diagnostics;

import java.lang.reflect.Method;

/** Measures unmodified 1.21.1 Blocks.rebuildCache; no renderer or network. */
public final class ShapeCacheTiming {
    public static void main(String[] arguments) throws Exception {
        long began = System.nanoTime();
        System.out.println("SHAPE_TIMING mode=" + System.getProperty("java.vm.info"));
        Class.forName("ab").getMethod("a").invoke(null); // SharedConstants.tryDetectVersion
        Class.forName("akt").getMethod("a").invoke(null); // Bootstrap.bootStrap
        System.out.println("SHAPE_TIMING bootstrap_ms=" + (System.nanoTime() - began) / 1_000_000);
        Class<?> blocks = Class.forName("dga");
        Method rebuild = blocks.getMethod("a"); // Blocks.rebuildCache
        Iterable<?> states = (Iterable<?>) Class.forName("dfy").getField("q").get(null);
        int count = 0;
        for (Object state : states) ++count;
        System.out.println("SHAPE_TIMING states=" + count);
        for (int attempt = 1; attempt <= 3; ++attempt) {
            began = System.nanoTime();
            rebuild.invoke(null);
            System.out.println("SHAPE_TIMING rebuild=" + attempt + " elapsed_ms="
                    + (System.nanoTime() - began) / 1_000_000);
        }
        System.out.println("SHAPE_TIMING_COMPLETE host_only=1 unchanged_default_tags=1");
        // Vanilla bootstrap starts worker threads; this standalone measurement is done.
        System.exit(0);
    }
}
