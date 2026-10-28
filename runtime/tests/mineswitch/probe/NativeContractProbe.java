package mineswitch.probe;

import java.lang.management.ManagementFactory;
import java.lang.reflect.Method;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import jdk.test.whitebox.WhiteBox;

/** Exercises native HotSpot FP, stack guards and compiled-loop safepoint contracts. */
public final class NativeContractProbe {
    private static final WhiteBox WB = WhiteBox.getWhiteBox();
    private static volatile boolean stop;
    private static volatile long progress;
    private static volatile double[] spinResult;
    private static volatile Throwable workerFailure;
    private static int checks;
    private static final class Token { int marker = 0x13579; }
    private interface Step { int apply(int value); }
    private static final class First implements Step {
        public int apply(int value) { return value * 3 + 1; }
    }
    private static final class Second implements Step {
        public int apply(int value) { return value * 5 - 1; }
    }

    private static void check(String name, boolean success) {
        if (!success) throw new AssertionError(name);
        ++checks;
        System.out.println("native.contract.check " + name + " PASS");
    }
    private static void compile(String name, int level, Class<?>... parameters) throws Exception {
        Method method = NativeContractProbe.class.getDeclaredMethod(name, parameters);
        WB.testSetDontInlineMethod(method, true);
        long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(60);
        int requests = 0;
        while ((!WB.isMethodCompiled(method) || WB.getMethodCompilationLevel(method) != level)
                && System.nanoTime() < deadline) {
            // HotSpot has one queued bit per method, not one per tier. A WB
            // request can join an already queued C1 task without scheduling C2.
            // Match the upstream IR framework: retry only after that task drains.
            if (!WB.isMethodQueuedForCompilation(method)) {
                if (!WB.isMethodCompilable(method, level))
                    throw new AssertionError(name + " is not compilable at level " + level);
                if (WB.enqueueMethodForCompilation(method, level)) ++requests;
            }
            Thread.sleep(10);
        }
        if (!WB.isMethodCompiled(method) || WB.getMethodCompilationLevel(method) != level) {
            System.err.println("NATIVE_CONTRACT_COMPILE_TIMEOUT method=" + name
                    + " requested=" + level + " actual=" + WB.getMethodCompilationLevel(method)
                    + " queued=" + WB.isMethodQueuedForCompilation(method) + " requests=" + requests);
        }
        check(name + "_compiled_level_" + level, WB.isMethodCompiled(method)
                && WB.getMethodCompilationLevel(method) == level);
    }
    private static void remainsCompiled(String label, String name, int level, Class<?>... parameters) throws Exception {
        Method method = NativeContractProbe.class.getDeclaredMethod(name, parameters);
        check(label, WB.isMethodCompiled(method) && WB.getMethodCompilationLevel(method) == level);
    }
    static double divide(double a, double b) { return a / b; }
    static int nested(int depth, int limit) {
        if (depth >= limit) return 0;
        return 1 + nested(depth + 1, limit);
    }
    static int dispatch(Step step, int value) { return step.apply(value); }
    static void spin(CountDownLatch entered, Token token, long limit) {
        double a = 0.5, b = 1.25;
        long iterations = 0;
        entered.countDown();
        while (!stop && iterations < limit) {
            a += 0.25;
            b += 0.5;
            ++iterations;
            if ((iterations & 65535) == 0) progress = iterations;
        }
        spinResult = new double[] {a, b, iterations, token.marker, divide(Double.MIN_NORMAL, 2)};
    }
    private static long collections() {
        return ManagementFactory.getGarbageCollectorMXBeans().stream()
                .mapToLong(bean -> Math.max(0, bean.getCollectionCount())).sum();
    }
    private static void safepointAndStackWalk(int level) throws Exception {
        // Resolve the actual referenced classes/calls and exercise both exits
        // before enqueueing C2. Otherwise an unresolved reference or unseen
        // branch can produce a trap-only nmethod that immediately deoptimizes.
        stop = false;
        spin(new CountDownLatch(1), new Token(), 10000);
        stop = true;
        spin(new CountDownLatch(0), new Token(), 10000);
        stop = false;
        progress = 0;
        spinResult = null;
        workerFailure = null;
        compile("spin", level, CountDownLatch.class, Token.class, long.class);
        CountDownLatch entered = new CountDownLatch(1);
        Thread worker = new Thread(() -> {
            try { spin(entered, new Token(), Long.MAX_VALUE); }
            catch (Throwable error) { workerFailure = error; entered.countDown(); }
        }, "native-contract compiled loop");
        worker.setDaemon(true);
        worker.start();
        try {
            check("worker_entered", entered.await(20, TimeUnit.SECONDS));
            long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(20);
            while (progress == 0 && workerFailure == null && System.nanoTime() < deadline) Thread.sleep(5);
            check("compiled_loop_running", progress != 0 && workerFailure == null);
            remainsCompiled("live_loop_compiled_before_gc", "spin", level,
                    CountDownLatch.class, Token.class, long.class);
            long before = collections();
            for (int cycle = 0; cycle < 3; ++cycle) System.gc();
            check("gc_completed_during_compiled_loop", collections() > before);
            remainsCompiled("live_loop_compiled_after_gc", "spin", level,
                    CountDownLatch.class, Token.class, long.class);
            var information = ManagementFactory.getThreadMXBean().getThreadInfo(worker.threadId(), 16);
            boolean found = false;
            if (information != null) {
                for (var frame : information.getStackTrace()) {
                    System.out.println("native.contract.frame " + frame);
                    if (frame.getClassName().equals(NativeContractProbe.class.getName())
                            && frame.getMethodName().equals("spin")) found = true;
                }
            }
            check("compiled_loop_management_stack", found);
            remainsCompiled("live_loop_compiled_after_stackwalk", "spin", level,
                    CountDownLatch.class, Token.class, long.class);
        } finally {
            stop = true;
            worker.join(20_000);
        }
        check("compiled_loop_join", !worker.isAlive() && workerFailure == null);
        double[] value = spinResult;
        check("live_fp_values_across_safepoints", value != null && value[2] > 0
                && value[0] == 0.5 + value[2] * 0.25
                && value[1] == 1.25 + value[2] * 0.5);
        check("live_oop_across_safepoints", value != null && value[3] == 0x13579);
        check("worker_fp_subnormal", value != null && value[4] == 0x0.8p-1022);
    }
    public static void main(String[] arguments) throws Exception {
        int level = arguments.length == 0 ? 1 : Integer.parseInt(arguments[0]);
        if (level != 1 && level != 4) throw new IllegalArgumentException("Use level 1 or 4");
        System.out.println("MINESWITCH_NATIVE_CONTRACT_BEGIN level=" + level);
        if (divide(3, 2) != 1.5) throw new AssertionError("FP baseline");
        compile("divide", level, double.class, double.class);
        check("fp_signed_zero", divide(1, -0.0) == Double.NEGATIVE_INFINITY);
        check("fp_nan", Double.isNaN(divide(0, 0)));
        check("fp_gradual_underflow", divide(Double.MIN_NORMAL, 2) == 0x0.8p-1022);
        check("fp_smallest_subnormal", divide(Double.MIN_VALUE, 1) == Double.MIN_VALUE);
        remainsCompiled("fp_remained_compiled", "divide", level, double.class, double.class);
        if (nested(0, 8) != 8) throw new AssertionError("recursion baseline");
        compile("nested", level, int.class, int.class);
        check("recursion_control", nested(0, 8) == 8);
        for (int attempt = 1; attempt <= 2; ++attempt) {
            boolean overflow = false;
            try { nested(0, Integer.MAX_VALUE); }
            catch (StackOverflowError expected) { overflow = true; }
            check("stack_overflow_" + attempt, overflow);
            check("stack_reguarded_" + attempt, nested(0, 8) == 8);
            remainsCompiled("recursion_remained_compiled_" + attempt, "nested", level, int.class, int.class);
        }
        Step first = new First();
        for (int i = 0; i < 5000; ++i) {
            if (dispatch(first, i) != i * 3 + 1) throw new AssertionError("dispatch warmup");
        }
        compile("dispatch", level, Step.class, int.class);
        check("monomorphic_compiled_dispatch", dispatch(first, 42) == 127);
        remainsCompiled("monomorphic_dispatch_remained_compiled", "dispatch", level, Step.class, int.class);
        Step second = new Second();
        check("new_implementation_dispatch", dispatch(second, 42) == 209);
        check("original_implementation_still_valid", dispatch(first, 42) == 127);
        safepointAndStackWalk(level);
        System.out.println("MINESWITCH_NATIVE_CONTRACT_PASS checks=" + checks + " level=" + level);
    }
}
