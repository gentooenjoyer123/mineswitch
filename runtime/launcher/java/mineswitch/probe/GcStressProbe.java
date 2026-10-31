package mineswitch.probe;

import java.lang.management.GarbageCollectorMXBean;
import java.lang.management.ManagementFactory;
import java.lang.ref.WeakReference;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.ThreadLocalRandom;
import java.util.concurrent.atomic.AtomicLong;
import java.util.concurrent.atomic.AtomicReferenceArray;

/**
 * Collector stress for the Horizon port: a large linked live set that keeps
 * being rewritten by several threads (young and old collections, promotion,
 * cross-generation references), explicit full collections, then a complete
 * integrity check of every reachable object.
 */
public final class GcStressProbe {
    // Nodes are Object[]{int[] payload, Object[] next}: the probe host loads
    // only this class from RomFS, so there are no helper classes.
    static Object[] node(long key, Object[] next) {
        int[] payload = new int[6 + (int) (key & 15)];
        payload[0] = (int) key;
        payload[1] = (int) (key >>> 32);
        for (int i = 2; i < payload.length; i++) payload[i] = (int) (key * 31 + i);
        return new Object[] {payload, next};
    }

    static long key(Object[] node) {
        int[] p = (int[]) node[0];
        return (p[0] & 0xffffffffL) | ((long) p[1] << 32);
    }

    static boolean valid(Object[] node) {
        int[] p = (int[]) node[0];
        long key = key(node);
        if (p.length != 6 + (int) (key & 15)) return false;
        for (int i = 2; i < p.length; i++) if (p[i] != (int) (key * 31 + i)) return false;
        return true;
    }

    static Object[] chain(long base, int length) {
        Object[] head = null;
        for (int i = length - 1; i >= 0; i--) head = node(base + i, head);
        return head;
    }

    static long verify(Object[] head, long base, int length) {
        long seen = 0;
        for (Object[] n = head; n != null; n = (Object[]) n[1], seen++) {
            if (key(n) != base + seen || !valid(n)) throw new IllegalStateException("corrupt node " + key(n) + " expected " + (base + seen));
        }
        if (seen != length) throw new IllegalStateException("chain length " + seen + " expected " + length);
        return seen;
    }

    public static void main(String[] args) throws Exception {
        int slots = args.length > 0 ? Integer.parseInt(args[0]) : 256;
        int length = args.length > 1 ? Integer.parseInt(args[1]) : 12_000;
        int seconds = args.length > 2 ? Integer.parseInt(args[2]) : 30;
        int threads = Math.max(2, Runtime.getRuntime().availableProcessors());
        System.out.println("GC_STRESS_BEGIN slots=" + slots + " length=" + length + " seconds=" + seconds +
                " threads=" + threads + " max_heap_mib=" + (Runtime.getRuntime().maxMemory() >> 20));
        for (GarbageCollectorMXBean bean : ManagementFactory.getGarbageCollectorMXBeans())
            System.out.println("GC_STRESS_COLLECTOR " + bean.getName());

        AtomicReferenceArray<Object[]> live = new AtomicReferenceArray<>(slots);
        long[] bases = new long[slots];
        for (int i = 0; i < slots; i++) {
            bases[i] = (long) i << 32;
            live.set(i, chain(bases[i], length));
        }
        System.out.println("GC_STRESS_LIVE_SET built used_mib=" + used());

        long deadline = System.nanoTime() + seconds * 1_000_000_000L;
        AtomicLong replaced = new AtomicLong(), garbage = new AtomicLong();
        long[] generation = new long[slots];
        Object lock = new Object();
        List<Thread> workers = new ArrayList<>();
        Throwable[] failure = new Throwable[1];
        for (int t = 0; t < threads; t++) {
            final int id = t;
            Thread worker = new Thread(() -> {
                ThreadLocalRandom random = ThreadLocalRandom.current();
                try {
                    while (System.nanoTime() < deadline) {
                        // Short-lived garbage of mixed sizes, including some large arrays.
                        Object[] scratch = new Object[64];
                        for (int i = 0; i < scratch.length; i++)
                            scratch[i] = random.nextInt(100) == 0 ? new long[16_384 + random.nextInt(65_536)] : node(random.nextLong(), null);
                        garbage.addAndGet(scratch.length);
                        // Replace a live chain: old-generation garbage and fresh promotion.
                        if (random.nextInt(8) == 0) {
                            int slot = random.nextInt(slots);
                            synchronized (lock) {
                                long base = ((long) slot << 32) + (++generation[slot] << 20);
                                bases[slot] = base;
                                live.set(slot, chain(base, length));
                            }
                            replaced.incrementAndGet();
                        }
                        // Old objects pointing at young ones (card marking).
                        int slot = random.nextInt(slots);
                        Object[] head = live.get(slot);
                        if (head != null && head[1] != null) {
                            synchronized (lock) {
                                Object[] current = live.get(slot);
                                Object[] second = (Object[]) current[1];
                                current[1] = node(key(second), (Object[]) second[1]);
                            }
                        }
                    }
                } catch (Throwable e) {
                    synchronized (failure) { if (failure[0] == null) failure[0] = e; }
                }
            }, "gc-stress-" + id);
            workers.add(worker);
            worker.start();
        }
        long lastReport = System.nanoTime();
        while (workers.stream().anyMatch(Thread::isAlive)) {
            Thread.sleep(250);
            if (System.nanoTime() - lastReport > 5_000_000_000L) {
                lastReport = System.nanoTime();
                System.out.println("GC_STRESS_PROGRESS replaced=" + replaced.get() + " garbage=" + garbage.get() + " used_mib=" + used() + gcSummary());
            }
        }
        for (Thread worker : workers) worker.join();
        if (failure[0] != null) throw new IllegalStateException("worker failed", failure[0]);

        WeakReference<Object> weak = new WeakReference<>(new byte[1 << 20]);
        for (int i = 0; i < 3; i++) {
            long start = System.nanoTime();
            System.gc();
            System.out.println("GC_STRESS_FULL_GC " + i + " ms=" + (System.nanoTime() - start) / 1_000_000 + " used_mib=" + used());
        }
        if (weak.get() != null) throw new IllegalStateException("weak reference survived full GCs");

        long nodes = 0;
        for (int i = 0; i < slots; i++) nodes += verify(live.get(i), bases[i], length);
        System.out.println("GC_STRESS_VERIFIED nodes=" + nodes + " replaced=" + replaced.get() + " garbage=" + garbage.get() + gcSummary());
        System.out.println("GC_STRESS_PASS");
    }

    static long used() {
        Runtime r = Runtime.getRuntime();
        return (r.totalMemory() - r.freeMemory()) >> 20;
    }

    static String gcSummary() {
        StringBuilder out = new StringBuilder();
        for (GarbageCollectorMXBean bean : ManagementFactory.getGarbageCollectorMXBeans())
            out.append(' ').append(bean.getName().replace(' ', '_')).append('=').append(bean.getCollectionCount())
               .append('/').append(bean.getCollectionTime()).append("ms");
        return out.toString();
    }
}
