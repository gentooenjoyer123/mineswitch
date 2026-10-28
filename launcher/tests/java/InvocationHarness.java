package fixture;

import java.io.PrintStream;
import mineswitch.launcher.ProcessorEntry;

public final class InvocationHarness {
    public static void main(String[] args) throws Exception {
        ClassLoader previous = Thread.currentThread().getContextClassLoader();
        PrintStream output = System.out, error = System.err;
        boolean failed = false;
        try {
            ProcessorEntry.main(new String[] { args[0] });
        } catch (Exception | Error failure) {
            failed = true;
            if (args.length != 2 || !args[1].equals("expect-failure")) throw failure;
        } finally {
            if (System.out != output || System.err != error)
                throw new IllegalStateException("Caller streams were not restored");
            if (Thread.currentThread().getContextClassLoader() != previous)
                throw new IllegalStateException("Caller context loader was not restored");
        }
        if (args.length == 2 && !failed) throw new IllegalStateException("Expected processor failure");
        System.out.println("Processor context restored");
        System.out.println("Processor streams restored");
    }
}
