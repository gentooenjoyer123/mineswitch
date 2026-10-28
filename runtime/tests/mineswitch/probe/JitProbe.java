package mineswitch.probe;

import java.lang.management.ManagementFactory;
import java.lang.reflect.Method;
import jdk.test.whitebox.WhiteBox;

/** Diagnostic-only: checks actual HotSpot nmethods, then executes their Java calls. */
public final class JitProbe {
    private static final WhiteBox WB = WhiteBox.getWhiteBox();
    private static int checks;
    private static volatile long sink;
    private static volatile Object retained;
    private static Method activeMethod;
    private static volatile boolean invalidateActive;
    private static final class Box {
        volatile long value;
        Box(long value) { this.value = value; }
    }

    private static void check(String name, boolean passed) {
        if (!passed) throw new AssertionError(name);
        ++checks;
        System.out.println("jit.check " + name + " PASS");
    }

    static long checksum(int[] values, int rounds) {
        long result = 7;
        for (int round = 0; round < rounds; ++round)
            for (int i = 0; i < values.length; ++i)
                result = Long.rotateLeft(result ^ (values[i] * 0x9e3779b97f4a7c15L), 11) + i;
        return result;
    }

    static int divide(int numerator, int denominator) {
        try { return numerator / denominator; }
        catch (ArithmeticException expected) { return 0x5a5a; }
    }

    static int arrayRead(int[] values, int index) {
        try { return values[index]; }
        catch (NullPointerException expected) { return -1001; }
        catch (ArrayIndexOutOfBoundsException expected) { return -1002; }
    }

    static long gcSurvivors(long value) {
        Box[] boxes = {new Box(value), new Box(value * 3 + 1)};
        // Publish the object graph so C2 cannot scalar-replace this GC test.
        retained = boxes;
        System.gc();
        return boxes[0].value + boxes[1].value + (retained == boxes ? 0 : Long.MIN_VALUE);
    }

    static void invalidateCaller() {
        if (invalidateActive) {
            invalidateActive = false;
            WB.deoptimizeMethod(activeMethod);
        }
    }

    static int activeDeopt(int seed) {
        Box box = new Box(seed);
        retained = box;
        int before = seed * 31;
        invalidateCaller();
        return before + (int)box.value + 99;
    }

    static int jumpTable(int value) {
        return switch (value) {
            case 0 -> 17; case 1 -> -3; case 2 -> 81; case 3 -> 6;
            case 4 -> 1023; case 5 -> -75; case 6 -> 24; case 7 -> 808;
            case 8 -> 53; case 9 -> -991; case 10 -> 456; case 11 -> 9;
            case 12 -> -240; case 13 -> 671; case 14 -> 31; case 15 -> -18;
            case 16 -> 7102; case 17 -> 5; case 18 -> -61; case 19 -> 79;
            case 20 -> 399; case 21 -> -202; case 22 -> 911; case 23 -> 12;
            default -> -12345;
        };
    }

    private static void compile(Method method, int level) throws InterruptedException {
        WB.testSetDontInlineMethod(method, true);
        check(method.getName() + "_enqueued", WB.enqueueMethodForCompilation(method, level));
        long deadline = System.nanoTime() + 60_000_000_000L;
        while ((!WB.isMethodCompiled(method) || WB.getMethodCompilationLevel(method) != level)
                && System.nanoTime() < deadline) {
            // An existing C1 task may absorb a C2 WhiteBox request. Retry only
            // after it leaves the queue, as in NativeContractProbe.
            if (!WB.isMethodQueuedForCompilation(method)) WB.enqueueMethodForCompilation(method, level);
            Thread.sleep(10);
        }
        check(method.getName() + "_compiled", WB.isMethodCompiled(method));
        check(method.getName() + "_level", WB.getMethodCompilationLevel(method) == level);
        Object[] nmethod = WB.getNMethod(method, false);
        check(method.getName() + "_machine_code", nmethod != null && nmethod.length == 5
                && ((byte[])nmethod[2]).length > 0 && ((Long)nmethod[4]) != 0);
        System.out.println("JIT_NMETHOD method=" + method.getName() + " level=" + nmethod[1]
                + " bytes=" + ((byte[])nmethod[2]).length + " compile_id=" + nmethod[3]
                + " entry=0x" + Long.toHexString((Long)nmethod[4]));
    }

    public static void main(String[] arguments) throws Exception {
        int level = arguments.length == 0 ? 1 : Integer.parseInt(arguments[0]);
        if (level != 1 && level != 4) throw new IllegalArgumentException("Use C1 level 1 or C2 level 4");
        System.out.println("MINESWITCH_JIT_BEGIN requested_level=" + level
                + " vm=" + System.getProperty("java.vm.name")
                + " mode=" + System.getProperty("java.vm.info"));
        check("compiler_present", ManagementFactory.getCompilationMXBean() != null);
        int[] values = new int[1024];
        for (int i = 0; i < values.length; ++i) values[i] = ((i * 37) ^ (i >>> 3)) - 17;
        // Fixed answers computed independently with Python's integer arithmetic,
        // an explicit 64-bit mask and an explicit rotate at each iteration.
        check("baseline_checksum_control", checksum(values, 1) == -2273577871850033378L);
        Method checksum = JitProbe.class.getDeclaredMethod("checksum", int[].class, int.class);
        Method divide = JitProbe.class.getDeclaredMethod("divide", int.class, int.class);
        Method arrayRead = JitProbe.class.getDeclaredMethod("arrayRead", int[].class, int.class);
        Method gc = JitProbe.class.getDeclaredMethod("gcSurvivors", long.class);
        Method jump = JitProbe.class.getDeclaredMethod("jumpTable", int.class);
        int[] tableAnswers = {17, -3, 81, 6, 1023, -75, 24, 808, 53, -991, 456, 9,
                -240, 671, 31, -18, 7102, 5, -61, 79, 399, -202, 911, 12};
        boolean tableControl = jumpTable(-1) == -12345;
        for (int i = 0; i < tableAnswers.length; ++i) tableControl &= jumpTable(i) == tableAnswers[i];
        check("jump_table_control", tableControl);
        // Resolve the allocated classes before requesting C2. Otherwise the
        // first nmethod can legitimately be just an uncommon-trap entry.
        check("gc_control", gcSurvivors(17L) == 69L);
        retained = null;
        compile(checksum, level);
        compile(divide, level);
        compile(arrayRead, level);
        compile(gc, level);
        compile(jump, level);
        check("compiled_checksum_1", checksum(values, 1) == -2273577871850033378L);
        check("compiled_checksum_2", checksum(values, 2) == 198340907627075021L);
        check("compiled_checksum_5", checksum(values, 5) == 3167859038456929521L);
        check("compiled_division", divide(-77, 6) == -12);
        check("compiled_division_overflow", divide(Integer.MIN_VALUE, -1) == Integer.MIN_VALUE);
        check("compiled_arithmetic_exception", divide(12, 0) == 0x5a5a);
        check("compiled_array_read", arrayRead(values, 99) == values[99]);
        check("compiled_null_exception", arrayRead(null, 0) == -1001);
        check("compiled_bounds_exception", arrayRead(values, values.length) == -1002);
        check("compiled_gc_live_references", gcSurvivors(1234567L) == 4938269L);
        check("gc_method_still_compiled", WB.isMethodCompiled(gc)
                && WB.getMethodCompilationLevel(gc) == level);
        boolean tableCorrect = jumpTable(99) == -12345;
        for (int i = 0; i < tableAnswers.length; ++i) tableCorrect &= jumpTable(i) == tableAnswers[i];
        check("compiled_jump_table", tableCorrect);
        check("jump_table_still_compiled", WB.isMethodCompiled(jump)
                && WB.getMethodCompilationLevel(jump) == level);
        retained = null;
        Thread worker = new Thread(() -> sink = checksum(values, 5), "compiled Java worker");
        worker.setDaemon(true);
        worker.start();
        worker.join(30_000);
        check("compiled_other_thread", !worker.isAlive() && sink == 3167859038456929521L);
        WB.deoptimizeMethod(checksum);
        check("deoptimized", !WB.isMethodCompiled(checksum));
        check("deoptimized_correctness", checksum(values, 2) == 198340907627075021L);
        compile(checksum, level);
        check("recompiled_correctness", checksum(values, 5) == 3167859038456929521L);
        activeMethod = JitProbe.class.getDeclaredMethod("activeDeopt", int.class);
        WB.testSetDontInlineMethod(JitProbe.class.getDeclaredMethod("invalidateCaller"), true);
        for (int i=0;i<1000;i++) activeDeopt(i);
        compile(activeMethod, level);
        invalidateActive = true;
        check("active_deopt_preserves_live_frame", activeDeopt(7) == 323);
        check("active_deopt_request_executed", !invalidateActive && !WB.isMethodCompiled(activeMethod));
        retained = null;
        System.out.println("MINESWITCH_JIT_PASS checks=" + checks + " level=" + level);
    }
}
