package mineswitch.launcher;

import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.util.concurrent.Executor;
import java.util.concurrent.atomic.AtomicBoolean;

/** Reads vanilla 1.21.1 counters on its render thread; never changes game state. */
public final class GameMetrics implements AutoCloseable {
    private final AtomicBoolean pending = new AtomicBoolean();
    private final Thread monitor;
    private volatile boolean running = true;

    private GameMetrics(Thread main) {
        monitor = new Thread(() -> sample(main), "MineSwitch game metrics");
        monitor.setDaemon(true);
        monitor.start();
    }

    public static GameMetrics start(Thread main) { return new GameMetrics(main); }

    private void sample(Thread main) {
        try {
            // Main renames the same thread before constructing Minecraft.
            // Wait until then so diagnostics do not initialize game classes.
            while (running && !"Render thread".equals(main.getName())) Thread.sleep(1000);
            if (!running) return;
            Class<?> client = Class.forName("fgo", false, GameMetrics.class.getClassLoader());
            Method instance = client.getMethod("Q");
            Method fps = client.getMethod("o");
            Method renderTime = client.getMethod("p");
            Field level = client.getField("r"), player = client.getField("s");
            while (running && main.isAlive()) {
                Object game = instance.invoke(null);
                if (game instanceof Executor executor && pending.compareAndSet(false, true)) {
                    executor.execute(() -> {
                        try {
                            if (running) System.out.println("MINESWITCH_GAME_METRICS fps=" + fps.invoke(game)
                                    + " render_ns=" + renderTime.invoke(game)
                                    + " world=" + (level.get(game) != null)
                                    + " player=" + (player.get(game) != null));
                        } catch (ReflectiveOperationException failure) {
                            running = false;
                            System.err.println("MINESWITCH_GAME_METRICS_UNAVAILABLE " + failure);
                        } finally { pending.set(false); }
                    });
                }
                Thread.sleep(10_000);
            }
        } catch (InterruptedException stopped) {
            Thread.currentThread().interrupt();
        } catch (ReflectiveOperationException | RuntimeException unavailable) {
            System.err.println("MINESWITCH_GAME_METRICS_UNAVAILABLE " + unavailable);
        }
    }

    @Override public void close() {
        running = false;
        monitor.interrupt();
    }
}
