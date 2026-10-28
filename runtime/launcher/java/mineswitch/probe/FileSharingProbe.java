package mineswitch.probe;

import java.io.InputStream;
import java.nio.ByteBuffer;
import java.nio.channels.FileChannel;
import java.nio.channels.FileLock;
import java.nio.channels.OverlappingFileLockException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardOpenOption;
import java.util.Comparator;
import java.util.List;
import java.util.Properties;
import java.util.concurrent.atomic.AtomicReference;

/** Ordinary config/channel integration checks; never loads Minecraft or mods. */
public final class FileSharingProbe {
    private static int checks;

    private static void check(boolean condition, String label) {
        if (!condition) throw new IllegalStateException(label);
        ++checks;
        System.out.println("FILE_SHARING_CHECK " + label + " PASS");
    }

    public static void main(String[] args) throws Exception {
        if (args.length > 1) throw new IllegalArgumentException("FileSharingProbe [OWNED_TEST_PARENT]");
        Path parent = args.length == 0 ? Path.of("/switch/mineswitch/tests") : Path.of(args[0]);
        Files.createDirectories(parent);
        Path root = Files.createTempDirectory(parent, "file-sharing-");
        System.out.println("FILE_SHARING_JAVA_BEGIN java=" + System.getProperty("java.version") +
                           " os=" + System.getProperty("os.name") + " root=" + root);
        try {
            configUpdate(root);
            nestedLock(root);
            positions(root);
            appends(root);
        } finally {
            List<Path> paths;
            try (var entries = Files.walk(root)) { paths = entries.sorted(Comparator.reverseOrder()).toList(); }
            for (Path path : paths) Files.delete(path);
        }
        check(!Files.exists(root), "owned_cleanup");
        System.out.println("FILE_SHARING_JAVA_PASS checks=" + checks);
    }

    private static void configUpdate(Path root) throws Exception {
        Path path = root.resolve("ordinary.properties");
        Files.writeString(path, "enabled=false\n", StandardCharsets.UTF_8);
        try (InputStream heldInput = Files.newInputStream(path)) {
            Properties before = new Properties();
            before.load(heldInput);
            check("false".equals(before.getProperty("enabled")), "configuration_read");
            // A library may retain its input stream while rewriting settings.
            Files.write(path, List.of("# updated settings", "enabled=true"), StandardCharsets.UTF_8);
            byte[] updated = Files.readAllBytes(path);
            check(new String(updated, StandardCharsets.UTF_8).contains("enabled=true"), "held_input_then_files_write");
            check(heldInput.read() == Byte.toUnsignedInt(updated["enabled=false\n".length()]), "original_input_keeps_its_position");
            check(Files.size(path) > 0, "metadata_after_writer_closed");
        }
    }

    private static void nestedLock(Path root) throws Exception {
        Path path = root.resolve("config-lock.tmp");
        try (FileChannel outer = FileChannel.open(path, StandardOpenOption.CREATE, StandardOpenOption.WRITE);
             FileLock lock = outer.lock()) {
            check(lock.isValid(), "outer_whole_file_lock");
            boolean overlap = false;
            try (FileChannel inner = FileChannel.open(path, StandardOpenOption.CREATE, StandardOpenOption.WRITE)) {
                check(inner.isOpen(), "nested_writer_open");
                try (FileLock unexpected = inner.lock()) {
                    throw new IllegalStateException("Nested channel unexpectedly acquired " + unexpected);
                } catch (OverlappingFileLockException expected) {
                    overlap = true;
                }
            }
            check(overlap, "nested_lock_reports_java_overlap");
            check(lock.isValid() && outer.isOpen(), "inner_close_preserves_outer_lock");
            outer.write(ByteBuffer.wrap(new byte[] {1, 2, 3}));
            outer.force(true);
        }
        try (FileChannel later = FileChannel.open(path, StandardOpenOption.WRITE);
             FileLock lock = later.tryLock()) {
            check(lock != null && lock.isValid(), "lock_reacquired_after_close");
        }
    }

    private static void positions(Path root) throws Exception {
        Path path = root.resolve("positions.bin");
        Files.writeString(path, "0123456789", StandardCharsets.UTF_8);
        try (FileChannel first = FileChannel.open(path, StandardOpenOption.READ, StandardOpenOption.WRITE);
             FileChannel second = FileChannel.open(path, StandardOpenOption.READ, StandardOpenOption.WRITE)) {
            first.position(2); second.position(7);
            ByteBuffer bytes = ByteBuffer.allocate(3);
            check(first.read(bytes, 4) == 3 && new String(bytes.array(), StandardCharsets.UTF_8).equals("456"), "positioned_read_content");
            check(first.position() == 2 && second.position() == 7, "independent_positions_after_read");
            check(second.write(ByteBuffer.wrap(new byte[] {'a', 'b'}), 1) == 2, "positioned_write_content");
            check(first.position() == 2 && second.position() == 7, "independent_positions_after_write");
            first.truncate(4);
            check(second.position() == 7 && second.read(ByteBuffer.allocate(1)) == -1, "other_position_survives_truncate");
            first.force(true);
        }
        check(Files.readString(path).equals("0ab3"), "final_file_contents");
        FileChannel writer = FileChannel.open(path, StandardOpenOption.WRITE);
        try (FileChannel reader = FileChannel.open(path, StandardOpenOption.READ)) {
            writer.close();
            check(reader.read(ByteBuffer.allocate(4)) == 4, "reader_survives_writer_close");
        } finally { writer.close(); }
    }

    private static void appends(Path root) throws Exception {
        Path path = root.resolve("append.txt");
        Files.createFile(path);
        AtomicReference<Throwable> failure = new AtomicReference<>();
        try (FileChannel first = FileChannel.open(path, StandardOpenOption.WRITE, StandardOpenOption.APPEND);
             FileChannel second = FileChannel.open(path, StandardOpenOption.WRITE, StandardOpenOption.APPEND)) {
            Thread a = new Thread(() -> append(first, "A|line\n", failure));
            Thread b = new Thread(() -> append(second, "B|line\n", failure));
            a.start(); b.start(); a.join(); b.join();
            if (failure.get() != null) throw new IllegalStateException("Append worker failed", failure.get());
            first.force(true);
        }
        List<String> lines = Files.readAllLines(path);
        check(lines.size() == 100, "concurrent_append_count");
        check(lines.stream().filter("A|line"::equals).count() == 50 && lines.stream().filter("B|line"::equals).count() == 50,
              "concurrent_append_records");
    }

    private static void append(FileChannel channel, String line, AtomicReference<Throwable> failure) {
        try {
            for (int i = 0; i < 50; ++i) {
                ByteBuffer value = ByteBuffer.wrap(line.getBytes(StandardCharsets.UTF_8));
                while (value.hasRemaining()) channel.write(value);
            }
        } catch (Throwable error) { failure.compareAndSet(null, error); }
    }
}
