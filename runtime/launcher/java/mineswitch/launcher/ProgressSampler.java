package mineswitch.launcher;

import java.lang.management.ClassLoadingMXBean;
import java.lang.management.ManagementFactory;
import java.lang.management.CompilationMXBean;
import java.lang.management.MemoryPoolMXBean;
import java.lang.management.MemoryUsage;
import java.lang.management.ThreadInfo;
import java.lang.management.ThreadMXBean;

/** Optional development diagnostics; it never interrupts or replaces game work. */
public final class ProgressSampler implements AutoCloseable {
    private final Thread observed;
    private final long observedId;
    private final Thread sampler;
    private final long started = System.nanoTime();
    private final int workerLimit = Math.max(0, Math.min(4,
            Integer.getInteger("mineswitch.progress.workers", 2)));
    private volatile boolean running = true;

    private ProgressSampler(Thread observed) {
        this.observed = observed;
        this.observedId = observed.threadId();
        sampler = new Thread(this::sample, "MineSwitch progress sampler");
        sampler.setDaemon(true);
        sampler.start();
    }

    public static ProgressSampler start(Thread observed) {
        if (observed == null) throw new NullPointerException("observed thread");
        return new ProgressSampler(observed);
    }

    /** Native launcher entry; leaves ordinary launches without a sampler. */
    public static ProgressSampler startIfEnabled(Thread observed) {
        return Boolean.getBoolean("mineswitch.progress") ? start(observed) : null;
    }

    private void sample() {
        System.out.println("MINESWITCH_SAMPLER_START observed_thread_id=" + observedId
                + " available_processors=" + Runtime.getRuntime().availableProcessors());
        ClassLoadingMXBean classes = null;
        ThreadMXBean threads = null;
        CompilationMXBean compilation = null;
        java.util.List<MemoryPoolMXBean> pools = java.util.List.of();
        try {
            classes = ManagementFactory.getClassLoadingMXBean();
            threads = ManagementFactory.getThreadMXBean();
            compilation = ManagementFactory.getCompilationMXBean();
            pools = ManagementFactory.getMemoryPoolMXBeans();
        } catch (Throwable unavailable) {
            System.out.println("MINESWITCH_SAMPLER_MANAGEMENT_UNAVAILABLE " + unavailable);
        }
        System.out.println("MINESWITCH_SAMPLER_READY elapsed_ms=" + (System.nanoTime() - started) / 1_000_000
                + " bounded_stack_sampling=" + (threads != null));
        // Startup is profiled at a finer interval; gameplay keeps the old 10 s.
        long fastUntilMs = Long.getLong("mineswitch.progress.fast_ms", 150_000);
        long fastInterval = Math.max(250, Long.getLong("mineswitch.progress.fast_interval_ms", 1_000));
        long tick = 0;
        while (running && observed.isAlive()) {
            boolean fast = (System.nanoTime() - started) / 1_000_000 < fastUntilMs;
            try {
                Thread.sleep(fast ? fastInterval : 10_000);
            } catch (InterruptedException stop) {
                if (!running) break;
                continue;
            }
            if (!running) break;
            try {
                long sampleStarted = System.nanoTime();
                Runtime runtime = Runtime.getRuntime();
                ThreadInfo info = threads == null ? null : threads.getThreadInfo(observedId, 16);
                // Avoid the full-stack allocation of Thread.getStackTrace() if
                // bounded management sampling is unavailable on this runtime.
                StackTraceElement[] trace = info == null ? new StackTraceElement[0] : info.getStackTrace();
                int loaded = classes == null ? -1 : classes.getLoadedClassCount();
                long totalLoaded = classes == null ? -1 : classes.getTotalLoadedClassCount();
                long sampleMicros = (System.nanoTime() - sampleStarted) / 1_000;
                System.out.println("MINESWITCH_PROGRESS elapsed_ms=" + (System.nanoTime() - started) / 1_000_000
                        + " snapshot_us=" + sampleMicros
                        + " target_id=" + observedId + " target_name=" + observed.getName()
                        + " state=" + (info == null ? observed.getState() : info.getThreadState())
                        + " loaded_classes=" + loaded
                        + " total_loaded_classes=" + totalLoaded
                        + " heap_used=" + (runtime.totalMemory() - runtime.freeMemory())
                        + " heap_committed=" + runtime.totalMemory() + " heap_max=" + runtime.maxMemory());
                if (compilation != null && compilation.isCompilationTimeMonitoringSupported())
                    System.out.println("MINESWITCH_COMPILATION total_ms=" + compilation.getTotalCompilationTime());
                for (MemoryPoolMXBean pool : pools) {
                    if (!pool.getName().startsWith("Code") && !pool.getName().equals("Metaspace")) continue;
                    MemoryUsage usage = pool.getUsage();
                    if (usage != null) System.out.println("MINESWITCH_MEMORY_POOL name=" + pool.getName()
                            + " used=" + usage.getUsed() + " committed=" + usage.getCommitted()
                            + " max=" + usage.getMax());
                }
                int length = Math.min(trace.length, 16);
                for (int i = 0; i < length; ++i) System.out.println("MINESWITCH_STACK " + i + " " + trace[i]);
                if (!fast || tick % 10 == 0) sampleWorkers(threads);
                ++tick;
            } catch (Throwable failure) {
                System.out.println("MINESWITCH_SAMPLER_FAILED " + failure);
                // A diagnostic must never repeatedly disrupt game execution.
                break;
            }
        }
        System.out.println("MINESWITCH_SAMPLER_END observed_thread_id=" + observedId);
    }

    private void sampleWorkers(ThreadMXBean threads) {
        if (threads == null || workerLimit == 0) return;
        // Resource reload runs in these workers while the main thread renders
        // the loading screen. Bound both thread count and stack depth.
        ThreadInfo[] candidates = threads.getThreadInfo(threads.getAllThreadIds(), 0);
        int sampled = 0;
        for (ThreadInfo candidate : candidates) {
            if (candidate == null || !candidate.getThreadName().startsWith("Worker-Main-")) continue;
            long sampleStarted = System.nanoTime();
            ThreadInfo info = threads.getThreadInfo(candidate.getThreadId(), 16);
            if (info == null) continue;
            long sampleMicros = (System.nanoTime() - sampleStarted) / 1_000;
            System.out.println("MINESWITCH_WORKER elapsed_ms=" + (System.nanoTime() - started) / 1_000_000
                    + " snapshot_us=" + sampleMicros + " target_id=" + info.getThreadId()
                    + " target_name=" + info.getThreadName() + " state=" + info.getThreadState());
            StackTraceElement[] trace = info.getStackTrace();
            for (int i = 0; i < trace.length; ++i)
                System.out.println("MINESWITCH_WORKER_STACK target_id=" + info.getThreadId()
                        + " frame=" + i + " " + trace[i]);
            if (++sampled == workerLimit) break;
        }
    }

    @Override public void close() {
        running = false;
        sampler.interrupt();
    }
}
