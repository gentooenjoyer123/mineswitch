import java.io.InputStream;
import java.io.OutputStream;
import java.nio.file.Path;
import java.util.jar.JarFile;

/** Read and verify every signed entry of the untouched official 1.21.1 client. */
public final class Sha5OfficialJarProbe {
    public static void main(String[] args) throws Exception {
        if (args.length != 1) throw new IllegalArgumentException("official-client.jar");
        long started = System.nanoTime();
        long signedBytes = 0;
        int signedEntries = 0;
        try (JarFile jar = new JarFile(Path.of(args[0]).toFile(), true)) {
            var entries = jar.entries();
            while (entries.hasMoreElements()) {
                var entry = entries.nextElement();
                if (entry.isDirectory()) continue;
                long bytes;
                try (InputStream input = jar.getInputStream(entry)) {
                    bytes = input.transferTo(OutputStream.nullOutputStream());
                }
                if (!entry.getName().startsWith("META-INF/")) {
                    if (entry.getCodeSigners() == null || entry.getCodeSigners().length == 0) {
                        throw new AssertionError("Missing signature: " + entry.getName());
                    }
                    signedEntries++;
                    signedBytes += bytes;
                }
            }
        }
        if (signedEntries != 23516 || signedBytes != 41812158L) {
            throw new AssertionError("Unexpected official 1.21.1 content: "
                    + signedEntries + " entries, " + signedBytes + " bytes");
        }
        System.out.println("SHA5_OFFICIAL_JAR_PASS signed_entries=" + signedEntries
                + " signed_bytes=" + signedBytes
                + " elapsed_ms=" + (System.nanoTime() - started) / 1_000_000);
    }
}
