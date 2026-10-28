import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.RandomAccessFile;
import java.io.OutputStream;
import java.lang.management.ManagementFactory;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.net.ServerSocket;
import java.net.Socket;
import java.nio.ByteBuffer;
import java.nio.MappedByteBuffer;
import java.nio.channels.AsynchronousCloseException;
import java.nio.channels.ClosedChannelException;
import java.nio.channels.FileChannel;
import java.nio.channels.Selector;
import java.nio.channels.ServerSocketChannel;
import java.nio.channels.SocketChannel;
import java.nio.charset.StandardCharsets;
import java.nio.file.DirectoryStream;
import java.nio.file.FileStore;
import java.nio.file.FileSystems;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.nio.file.StandardOpenOption;
import java.nio.file.WatchService;
import java.nio.file.attribute.BasicFileAttributes;
import java.security.SecureRandom;
import java.util.Arrays;
import java.util.concurrent.atomic.AtomicReference;
import java.util.concurrent.atomic.AtomicLong;
import java.util.zip.CRC32;
import java.util.zip.Deflater;
import java.util.zip.Inflater;

/** Java 8 bootstrap probe; runs inside the actual Horizon OpenJDK 8 VM.
 *  Covers the VM, libjava, libzip, libverify, libmanagement, and (Horizon
 *  overlay classes in lib/horizon.jar) libnio/libnet: NIO files, mapping,
 *  sockets, selector. */
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
        final AtomicLong answer = new AtomicLong();
        // A lambda (invokedynamic) keeps the probe a single class file: the
        // launcher defines only HelloHorizon itself from romfs:/tests.
        Thread worker = new Thread(() -> answer.set(nativeRoundTrip(42)), "horizon-java-worker");
        worker.setDaemon(true);
        worker.start();
        worker.join(10000);
        check("thread_join", !worker.isAlive());
        check("jni_from_java_worker", answer.get() == 133);
    }

    private static void classLibrary() throws Exception {
        InputStream input = Object.class.getResourceAsStream("Object.class");
        try {
            check("rt_jar_resource_open", input != null);
            byte[] header = new byte[8];
            int n = 0;
            while (n < 8) { int r = input.read(header, n, 8 - n); if (r < 0) break; n += r; }
            check("class_magic", n == 8 && header[0] == (byte)0xca && header[1] == (byte)0xfe
                    && header[2] == (byte)0xba && header[3] == (byte)0xbe);
            check("java8_class_version", header[6] == 0 && header[7] == 52);
        } finally {
            if (input != null) input.close();
        }
        byte[] data = new byte[65536];
        for (int i = 0; i < data.length; ++i) data[i] = (byte)(i * 31 + (i >> 7));
        CRC32 crc = new CRC32();
        crc.update(data, 0, data.length);
        check("zip_crc32", crc.getValue() != 0);
        Deflater deflater = new Deflater();
        deflater.setInput(data);
        deflater.finish();
        byte[] packed = new byte[data.length + 64];
        int packedLength = deflater.deflate(packed);
        deflater.end();
        Inflater inflater = new Inflater();
        inflater.setInput(packed, 0, packedLength);
        byte[] unpacked = new byte[data.length];
        int unpackedLength = inflater.inflate(unpacked);
        inflater.end();
        check("zip_deflate_inflate", unpackedLength == data.length && Arrays.equals(data, unpacked));
        check("strictmath_fdlibm", Math.abs(StrictMath.sin(1.0) - 0.8414709848078965) < 1e-15
                && StrictMath.log(Math.E) == 1.0);
        check("string_format", String.format("%05.2f|%x", 3.14159, 255).equals("03.14|ff"));
    }

    private static void files() throws Exception {
        File root = new File("/switch/mineswitch/tmp");
        root.mkdirs();
        File directory = new File(root, "java8-" + System.nanoTime());
        check("io_mkdir", directory.mkdir() && directory.isDirectory());
        File file = new File(directory, "roundtrip.bin");
        byte[] expected = new byte[8192];
        for (int i = 0; i < expected.length; ++i) expected[i] = (byte)(i * 37 + 13);
        try {
            FileOutputStream out = new FileOutputStream(file);
            try { out.write(expected); out.getFD().sync(); } finally { out.close(); }
            check("io_length", file.length() == expected.length);
            byte[] read = new byte[expected.length];
            FileInputStream in = new FileInputStream(file);
            try {
                int n = 0;
                while (n < read.length) { int r = in.read(read, n, read.length - n); if (r < 0) break; n += r; }
                check("io_roundtrip", n == read.length && Arrays.equals(expected, read));
            } finally { in.close(); }
            RandomAccessFile raf = new RandomAccessFile(file, "rw");
            try {
                raf.seek(123);
                check("io_random_access", raf.read() == (expected[123] & 0xff) && raf.getFilePointer() == 124);
                raf.setLength(100);
                check("io_truncate", raf.length() == 100);
            } finally { raf.close(); }
            String[] names = directory.list();
            check("io_list", names != null && names.length == 1 && "roundtrip.bin".equals(names[0]));
            check("io_canonical", directory.getCanonicalPath().startsWith("/switch/mineswitch/tmp"));
        } finally {
            file.delete();
            directory.delete();
        }
        check("io_owned_files_removed", !directory.exists());
    }

    private static void nioFiles() throws Exception {
        Path directory = Paths.get("/switch/mineswitch/tmp/java8-nio");
        Files.createDirectories(directory);
        Path file = directory.resolve("nio.bin");
        byte[] data = new byte[20000];
        for (int i = 0; i < data.length; ++i) data[i] = (byte)(i * 13);
        Files.write(file, data);
        check("nio_files_roundtrip", Arrays.equals(Files.readAllBytes(file), data) && Files.size(file) == data.length);
        BasicFileAttributes attributes = Files.readAttributes(directory, BasicFileAttributes.class);
        check("nio_basic_attributes", attributes.isDirectory() && !attributes.isRegularFile());
        int entries = 0;
        try (DirectoryStream<Path> stream = Files.newDirectoryStream(directory)) {
            for (Path ignored : stream) ++entries;
        }
        check("nio_directory_stream", entries == 1);
        try (FileChannel channel = FileChannel.open(file, StandardOpenOption.READ, StandardOpenOption.WRITE)) {
            ByteBuffer buffer = ByteBuffer.allocateDirect(4);
            buffer.putInt(0x12345678).flip();
            channel.write(buffer, 100);
            buffer.clear();
            channel.read(buffer, 100);
            buffer.flip();
            check("nio_filechannel_positioned", buffer.getInt() == 0x12345678 && channel.size() == data.length);
            MappedByteBuffer mapped = channel.map(FileChannel.MapMode.READ_ONLY, 4096, 8192);
            check("nio_map_readonly", mapped.get(0) == data[4096] && mapped.get(8191) == data[4096 + 8191]);
            channel.truncate(1000);
            check("nio_truncate", channel.size() == 1000);
        }
        FileStore store = Files.getFileStore(directory);
        check("nio_filestore", store.getTotalSpace() > 0 && store.getUsableSpace() >= 0);
        try (WatchService watch = FileSystems.getDefault().newWatchService()) {
            check("nio_watch_service", watch != null);
        }
        Files.delete(file);
        Files.delete(directory);
        check("nio_delete", !Files.exists(directory));
    }

    private static void platformServices() throws Exception {
        boolean rejected = false;
        try {
            new ProcessBuilder("probe").start();
        } catch (IOException expected) {
            rejected = expected.getMessage().contains("not supported on Horizon");
        }
        check("process_rejected", rejected);
        byte[] seed = new SecureRandom().generateSeed(32);
        check("secure_random_seed", seed.length == 32);
        // Minecraft <= 1.17 snooper / crash reports read these MXBeans.
        check("management_input_arguments",
              ManagementFactory.getRuntimeMXBean().getInputArguments().contains("-Xrs"));
        check("management_heap_usage",
              ManagementFactory.getMemoryMXBean().getHeapMemoryUsage().getUsed() > 0);
        check("management_os_bean",
              ManagementFactory.getOperatingSystemMXBean().getAvailableProcessors() > 0);
        check("management_threads", ManagementFactory.getThreadMXBean().getThreadCount() > 0);
    }

    private static void sockets() throws Exception {
        InetAddress loopback = InetAddress.getByName("127.0.0.1");
        try (ServerSocket server = new ServerSocket(0, 1, loopback);
             Socket client = new Socket(loopback, server.getLocalPort());
             Socket accepted = server.accept()) {
            OutputStream out = client.getOutputStream();
            out.write("ping-8".getBytes(StandardCharsets.UTF_8));
            out.flush();
            byte[] buffer = new byte[6];
            InputStream in = accepted.getInputStream();
            int n = 0;
            while (n < buffer.length) {
                int r = in.read(buffer, n, buffer.length - n);
                if (r < 0) break;
                n += r;
            }
            check("net_socket_loopback", "ping-8".equals(new String(buffer, 0, n, StandardCharsets.UTF_8)));
        }
        try (ServerSocketChannel server = ServerSocketChannel.open()) {
            server.bind(new InetSocketAddress(loopback, 0));
            try (SocketChannel client = SocketChannel.open(server.getLocalAddress());
                 SocketChannel accepted = server.accept()) {
                client.write(ByteBuffer.wrap(new byte[] {1, 2, 3, 4}));
                ByteBuffer buffer = ByteBuffer.allocate(4);
                while (buffer.hasRemaining() && accepted.read(buffer) >= 0) { }
                check("nio_socketchannel_loopback", buffer.get(0) == 1 && buffer.get(3) == 4);
                final SocketChannel blocked = accepted;
                final AtomicReference<Throwable> outcome = new AtomicReference<>();
                Thread reader = new Thread(() -> {
                    try {
                        blocked.read(ByteBuffer.allocate(1));
                    } catch (Throwable error) {
                        outcome.set(error);
                    }
                }, "blocked-reader");
                reader.start();
                Thread.sleep(200);
                blocked.close();
                reader.join(5000);
                check("nio_close_releases_reader", !reader.isAlive()
                        && (outcome.get() instanceof AsynchronousCloseException
                            || outcome.get() instanceof ClosedChannelException));
            }
        }
        try (Selector selector = Selector.open()) {
            selector.wakeup();
            long start = System.nanoTime();
            selector.select(5000);
            check("nio_selector_wakeup", System.nanoTime() - start < 2_000_000_000L);
        }
    }

    public static void main(String[] arguments) throws Exception {
        System.out.println("MINESWITCH_JAVA_BEGIN class_version=52");
        System.out.println("java.version=" + System.getProperty("java.version"));
        System.out.println("java.vm.name=" + System.getProperty("java.vm.name"));
        System.out.println("os.name=" + System.getProperty("os.name"));
        System.out.println("java.home=" + System.getProperty("java.home"));
        check("runtime_java8", System.getProperty("java.specification.version").equals("1.8"));
        check("platform_horizon", "Horizon".equals(System.getProperty("os.name")));
        check("vm_server", System.getProperty("java.vm.name").contains("Server VM"));
        check("string_unicode", "Java Нинтендо".length() == 13);
        check("long_arithmetic", (0x100000002L * 3 + 7) == 12884901901L);
        try {
            throw new IOException("expected-java-exception");
        } catch (IOException expected) {
            check("exception", "expected-java-exception".equals(expected.getMessage()));
        }
        check("jni_callback_small", nativeRoundTrip(123) == 376);
        check("jni_callback_64bit", nativeRoundTrip(0x100000002L) == 12884901901L);
        allocationAndThreads();
        classLibrary();
        files();
        nioFiles();
        platformServices();
        sockets();
        System.out.println("MINESWITCH_JAVA_PASS checks=" + checks);
    }
}
