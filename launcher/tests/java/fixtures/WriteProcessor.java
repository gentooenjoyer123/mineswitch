package fixture;

import fixture.dependency.Message;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;

/** Small ordinary processor used only by host preparation tests. */
public final class WriteProcessor {
    private static int invocations;

    public static void main(String[] args) throws Exception {
        if (++invocations != 1) throw new IllegalStateException("Processor classloader was reused");
        ClassLoader loader = WriteProcessor.class.getClassLoader();
        if (Thread.currentThread().getContextClassLoader() != loader)
            throw new IllegalStateException("Processor context loader was not installed");
        try {
            Class.forName("mineswitch.launcher.ProcessorEntry", false, loader);
            throw new IllegalStateException("Application classpath leaked into processor loader");
        } catch (ClassNotFoundException expected) { }
        if (Files.exists(Path.of(args[2]))) throw new IllegalStateException("Stale completion marker survived");
        String text = args[1].startsWith("@file:")
                ? Files.readString(Path.of(args[1].substring(6)), StandardCharsets.UTF_8)
                : args[1];
        System.out.println("fixture stdout: " + text);
        System.err.println("fixture stderr: " + text);
        if (text.equals("close-streams")) {
            System.out.close(); System.err.close();
        }
        Files.writeString(Path.of(args[0]), Message.prefix() + text, StandardCharsets.UTF_8);
    }
}
