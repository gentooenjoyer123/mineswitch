package mineswitch.probe;

import java.lang.management.ManagementFactory;
import java.lang.management.ThreadInfo;
import java.lang.management.ThreadMXBean;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

/** Regression for Zero's management vframe sender walk, executed in the real VM. */
public final class StackDepthProbe {
    private static final CountDownLatch entered = new CountDownLatch(1);
    private static final CountDownLatch release = new CountDownLatch(1);
    private static volatile Throwable workerFailure;
    private static volatile StackTraceElement[] synchronous;
    private static int checks;

    private static void check(String name, boolean passed) {
        if (!passed) throw new AssertionError(name);
        ++checks;
        System.out.println("stack.check " + name + " PASS");
    }
    private static boolean contains(StackTraceElement[] trace, String name) {
        for (StackTraceElement element : trace) {
            if (element.getClassName().equals(StackDepthProbe.class.getName())
                    && element.getMethodName().equals(name)) return true;
        }
        return false;
    }
    private static void levelOne() throws InterruptedException { levelTwo(); }
    private static void levelTwo() throws InterruptedException { levelThree(); }
    private static void levelThree() throws InterruptedException {
        synchronous = new Throwable("synchronous control").getStackTrace();
        entered.countDown();
        release.await();
    }
    public static void main(String[] arguments) throws Exception {
        ThreadMXBean management = ManagementFactory.getThreadMXBean();
        Thread target = new Thread(() -> {
            try { levelOne(); }
            catch (Throwable error) { workerFailure = error; entered.countDown(); }
        }, "Zero nested stack target");
        target.setDaemon(true);
        target.start();
        try {
            check("worker_entered", entered.await(15, TimeUnit.SECONDS));
            if (workerFailure != null) throw new AssertionError("Worker failed", workerFailure);
            check("synchronous_chain", synchronous != null && contains(synchronous, "levelOne")
                    && contains(synchronous, "levelTwo") && contains(synchronous, "levelThree"));
            ThreadInfo info = management.getThreadInfo(target.threadId(), 16);
            check("target_identity", info != null && info.getThreadId() == target.threadId());
            StackTraceElement[] stack = info.getStackTrace();
            System.out.println("stack.management_depth=" + stack.length);
            for (StackTraceElement frame : stack) System.out.println("stack.frame " + frame);
            check("management_nested_chain", contains(stack, "levelOne")
                    && contains(stack, "levelTwo") && contains(stack, "levelThree"));
            check("management_bound_16", stack.length > 1 && stack.length <= 16);
            check("management_bound_2", management.getThreadInfo(target.threadId(), 2).getStackTrace().length == 2);
            check("management_bound_0", management.getThreadInfo(target.threadId(), 0).getStackTrace().length == 0);
            StackTraceElement[] direct = target.getStackTrace();
            check("direct_thread_dump_chain", contains(direct, "levelOne")
                    && contains(direct, "levelTwo") && contains(direct, "levelThree"));
        } finally {
            release.countDown();
            target.join(15_000);
        }
        check("worker_joined", !target.isAlive() && workerFailure == null);
        System.out.println("MINESWITCH_STACK_PASS checks=" + checks);
    }
}
