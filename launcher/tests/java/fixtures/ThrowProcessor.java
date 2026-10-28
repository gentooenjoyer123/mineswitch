package fixture;

import java.io.OutputStream;
import java.io.PrintStream;
import java.util.Arrays;

/** Exercises a large log and restoration after a tool changes global state. */
public final class ThrowProcessor {
    public static void main(String[] args) {
        System.out.println("fixture throwing stdout");
        System.err.println("fixture throwing stderr");
        if (args.length != 0 && args[0].equals("flood")) {
            byte[] bytes = new byte[64 * 1024];
            Arrays.fill(bytes, (byte) 'x');
            for (int i = 0; i < 160; ++i) System.out.write(bytes, 0, bytes.length);
        }
        Thread.currentThread().setContextClassLoader(ClassLoader.getPlatformClassLoader());
        PrintStream discarded = new PrintStream(OutputStream.nullOutputStream());
        System.setOut(discarded); System.setErr(discarded);
        throw new IllegalStateException("fixture failing processor");
    }
}
