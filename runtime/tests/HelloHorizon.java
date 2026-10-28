import java.io.IOException;
import java.io.InputStream;
import java.lang.management.ManagementFactory;
import java.nio.ByteBuffer;
import java.nio.channels.FileChannel;
import java.nio.channels.FileLock;
import java.nio.channels.OverlappingFileLockException;
import java.nio.file.AccessDeniedException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardOpenOption;
import java.nio.file.LinkOption;
import java.nio.file.attribute.BasicFileAttributes;
import java.security.SecureRandom;
import java.util.Arrays;
import java.util.concurrent.atomic.AtomicLong;
import jdk.jfr.FlightRecorder;

/** Runs inside the actual Horizon OpenJDK VM, not a host-side substitute. */
public final class HelloHorizon {
    private static int checks;

    private static native long nativeRoundTrip(long value);

    private static long nativeCallback(long value) {
        return value * 3;
    }

    private static void check(String name, boolean success) {
        if (!success) throw new AssertionError(name);
        ++checks;
        System.out.println("java.check " + name + " PASS");
    }

    private static void allocationAndThreads() throws Exception {
        String[] live = new String[128];
        for (int i = 0; i < live.length; ++i) live[i] = "live-" + i;
        byte[] last = null;
        // Exceeds the launcher's 64 MiB heap and exercises actual GC relocation.
        for (int i = 0; i < 4096; ++i) {
            last = new byte[32768];
            last[0] = (byte)i;
            last[last.length - 1] = (byte)(i >> 8);
        }
        check("allocation_data", last[0] == (byte)4095 && last[32767] == 15);
        check("live_references_after_gc", "live-42".equals(live[42]));

        AtomicLong answer = new AtomicLong();
        Thread worker = new Thread(() -> answer.set(nativeRoundTrip(42)), "horizon-java-worker");
        worker.setDaemon(true);
        worker.start();
        worker.join(10_000);
        check("thread_join", !worker.isAlive());
        check("jni_from_java_worker", answer.get() == 133);
    }

    private static void imageAndFiles() throws Exception {
        try (InputStream input = Object.class.getResourceAsStream("Object.class")) {
            check("jrt_resource_open", input != null);
            byte[] header = input.readNBytes(8);
            check("jrt_class_magic", header.length == 8
                    && header[0] == (byte)0xca && header[1] == (byte)0xfe
                    && header[2] == (byte)0xba && header[3] == (byte)0xbe);
            check("java21_class_version", header[6] == 0 && header[7] == 65);
        }

        Path temporaryRoot = Path.of("/switch/mineswitch/tmp");
        Files.createDirectories(temporaryRoot);
        Path directory = Files.createTempDirectory(temporaryRoot, "java21-");
        Path file = directory.resolve("roundtrip.bin");
        byte[] expected = new byte[8192];
        for (int i = 0; i < expected.length; ++i) expected[i] = (byte)(i * 37 + 13);
        try {
            Files.write(file, expected, StandardOpenOption.CREATE_NEW, StandardOpenOption.WRITE);
            check("nio_size", Files.size(file) == expected.length);
            check("nio_file_roundtrip", Arrays.equals(expected, Files.readAllBytes(file)));
            try (FileChannel channel = FileChannel.open(file, StandardOpenOption.READ,
                                                       StandardOpenOption.WRITE)) {
                channel.position(7);
                ByteBuffer sample = ByteBuffer.allocate(8);
                check("nio_positioned_read", channel.read(sample, 123) == 8);
                check("nio_cursor_unchanged", channel.position() == 7);
                check("nio_positioned_data", sample.array()[0] == expected[123]);
                channel.force(true);
            }
        } finally {
            Files.deleteIfExists(file);
            Files.delete(directory);
        }
        check("nio_owned_files_removed", !Files.exists(directory));
    }

    private static void writerMetadata() throws Exception {
        Path directory = Files.createTempDirectory(Path.of("/switch/mineswitch/tmp"), "writer-metadata-");
        Path file = directory.resolve("LogCase.json");
        Path alias = directory.resolve("logcase.JSON");
        try {
            // Match Minecraft's cache journal: the writer remains open while
            // Files.walkFileTree reads attributes for the containing directory.
            try (FileChannel writer = FileChannel.open(file, StandardOpenOption.CREATE_NEW,
                                                       StandardOpenOption.WRITE)) {
                ByteBuffer first = ByteBuffer.wrap("{\"hello\":1}\n".getBytes(java.nio.charset.StandardCharsets.UTF_8));
                while (first.hasRemaining()) writer.write(first);
                writer.force(true);
                long length = writer.size();
                long position = writer.position();
                BasicFileAttributes before = Files.readAttributes(file, BasicFileAttributes.class);
                check("writer_metadata_size", before.isRegularFile() && before.size() == length);
                BasicFileAttributes aliased = Files.readAttributes(alias, BasicFileAttributes.class,
                                                                   LinkOption.NOFOLLOW_LINKS);
                check("writer_metadata_alias", aliased.size() == length &&
                        aliased.lastModifiedTime().equals(before.lastModifiedTime()));
                check("writer_metadata_last_modified", Files.getLastModifiedTime(file).equals(before.lastModifiedTime()));
                check("writer_metadata_java_io", file.toFile().isFile() && file.toFile().length() == length &&
                        file.toFile().lastModified() == before.lastModifiedTime().toMillis());
                // Files.walk uses the same FileTreeWalker.getAttributes path
                // that failed in Minecraft's Files.walkFileTree cache vacuum.
                try (var paths = Files.walk(directory)) {
                    check("writer_metadata_tree_walk", paths.filter(path ->
                            path.getFileName().equals(file.getFileName())).count() == 1);
                }
                check("writer_metadata_cursor_preserved", writer.position() == position);
                ByteBuffer tail = ByteBuffer.wrap(new byte[] {1, 2, 3, 4});
                while (tail.hasRemaining()) writer.write(tail);
                writer.force(true);
                check("writer_metadata_after_write", Files.size(alias) == length + 4);
                writer.truncate(3);
                writer.force(true);
                check("writer_metadata_after_truncate", Files.readAttributes(file, BasicFileAttributes.class).size() == 3);
                try (FileChannel ignored = FileChannel.open(alias, StandardOpenOption.READ)) {
                    throw new AssertionError("metadata lookup weakened native writer exclusion");
                } catch (AccessDeniedException expected) {
                    check("writer_metadata_exclusion_preserved", true);
                }
            }
            check("writer_metadata_closed_path", Files.size(file) == 3);
        } finally {
            Files.deleteIfExists(file);
            Files.delete(directory);
        }
    }

    private static void locks() throws Exception {
        Path directory = Files.createTempDirectory(Path.of("/switch/mineswitch/tmp"), "java-lock-");
        Path file = directory.resolve("session.lock");
        FileLock liveAtClose = null;
        try {
            // Minecraft opens its session lock write-only.
            try (FileChannel owner = FileChannel.open(file, StandardOpenOption.CREATE_NEW,
                                                     StandardOpenOption.WRITE)) {
                FileLock lock = owner.tryLock();
                check("file_lock_acquire", lock != null && lock.isValid() && !lock.isShared());
                try {
                    owner.tryLock();
                    throw new AssertionError("overlapping lock accepted");
                } catch (OverlappingFileLockException expected) {
                    check("file_lock_overlap", true);
                }
                lock.release();
                check("file_lock_release", !lock.isValid());
                // Native Horizon writable opens remain exclusive until close.
                try (FileChannel ignored = FileChannel.open(file, StandardOpenOption.WRITE)) {
                    throw new AssertionError("another writer opened a held native file");
                } catch (AccessDeniedException expected) {
                    check("file_lock_native_open_exclusion", true);
                }
                try {
                    owner.tryLock(0, 1, false);
                    throw new AssertionError("unsupported region lock accepted");
                } catch (IOException expected) {
                    check("file_lock_unsupported_region", true);
                }
                liveAtClose = owner.tryLock();
                check("file_lock_reacquire_after_failure", liveAtClose != null && liveAtClose.isValid());
            }
            check("file_lock_close_invalidates", liveAtClose != null && !liveAtClose.isValid());
            try (FileChannel next = FileChannel.open(file, StandardOpenOption.WRITE);
                 FileLock lock = next.tryLock()) {
                check("file_lock_close_reopen", lock != null && lock.isValid());
            }
        } finally {
            Files.deleteIfExists(file);
            Files.delete(directory);
        }
    }

    private static void management() {
        var runtime = ManagementFactory.getRuntimeMXBean();
        check("management_runtime_name", runtime.getVmName().contains("Zero"));
        check("management_runtime_arguments", runtime.getInputArguments().contains("-Xmx64m"));
        check("management_uptime", runtime.getUptime() >= 0);
        var memory = ManagementFactory.getMemoryMXBean().getHeapMemoryUsage();
        check("management_heap", memory.getUsed() > 0 && memory.getMax() == 64L * 1024 * 1024);
        var threads = ManagementFactory.getThreadMXBean();
        check("management_thread_count", threads.getThreadCount() > 0);
        check("management_current_thread", threads.getThreadInfo(Thread.currentThread().threadId()) != null);
        check("management_gc", !ManagementFactory.getGarbageCollectorMXBeans().isEmpty());
        var operatingSystem = ManagementFactory.getOperatingSystemMXBean();
        check("management_os", "Horizon".equals(operatingSystem.getName()));
        check("management_processors", operatingSystem.getAvailableProcessors() > 0);
        if (operatingSystem instanceof com.sun.management.UnixOperatingSystemMXBean extended) {
            check("management_memory_budget", extended.getTotalMemorySize() >= memory.getMax());
            check("management_free_memory", extended.getFreeMemorySize() >= 0
                    && extended.getFreeMemorySize() <= extended.getTotalMemorySize());
            check("management_descriptor_limit", extended.getMaxFileDescriptorCount() == 1024);
            check("management_open_descriptors", extended.getOpenFileDescriptorCount() >= 3);
            check("management_no_swap", extended.getTotalSwapSpaceSize() == 0);
        } else {
            throw new AssertionError("Horizon operating-system management provider missing");
        }
        check("jfr_unavailable_supported_fallback", !FlightRecorder.isAvailable());
    }

    public static void main(String[] arguments) throws Exception {
        if (arguments.length == 1 && "--writer-metadata-only".equals(arguments[0])) {
            System.out.println("MINESWITCH_WRITER_METADATA_BEGIN");
            writerMetadata();
            System.out.println("MINESWITCH_WRITER_METADATA_PASS checks=" + checks);
            return;
        }
        System.out.println("MINESWITCH_JAVA_BEGIN class_version=65");
        System.out.println("java.version=" + System.getProperty("java.version"));
        System.out.println("java.vm.name=" + System.getProperty("java.vm.name"));
        System.out.println("os.name=" + System.getProperty("os.name"));
        System.out.println("java.home=" + System.getProperty("java.home"));
        check("runtime_feature_21", Runtime.version().feature() == 21);
        check("platform_horizon", "Horizon".equals(System.getProperty("os.name")));
        check("string_unicode", "Java \u041d\u0438\u043d\u0442\u0435\u043d\u0434\u043e".length() == 13);
        check("long_arithmetic", (0x100000002L * 3 + 7) == 12884901901L);
        try {
            throw new IOException("expected-java-exception");
        } catch (IOException expected) {
            check("exception", "expected-java-exception".equals(expected.getMessage()));
        }
        check("jni_callback_small", nativeRoundTrip(123) == 376);
        check("jni_callback_64bit", nativeRoundTrip(0x100000002L) == 12884901901L);
        allocationAndThreads();

        SecureRandom random = new SecureRandom();
        byte[] first = new byte[32];
        byte[] second = new byte[32];
        random.nextBytes(first);
        random.nextBytes(second);
        check("csrng_provider_available", !Arrays.equals(first, second));
        // This checks the wired provider and does not claim a statistical RNG audit.
        imageAndFiles();
        writerMetadata();
        locks();
        management();
        System.out.println("MINESWITCH_JAVA_PASS checks=" + checks);
    }
}
