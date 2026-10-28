import java.io.IOException;
import java.nio.channels.spi.AsynchronousChannelProvider;
import java.nio.file.DirectoryStream;
import java.nio.file.FileSystems;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.WatchService;
import java.nio.file.attribute.BasicFileAttributes;
import java.nio.file.attribute.PosixFileAttributeView;
import java.util.Arrays;

/**
 * Host check of the Java 8 Horizon overlay (lib/horizon.jar) on a Linux JDK 8:
 *   java -Xbootclasspath/p:horizon.jar -Dos.name=Horizon OverlayHostCheck <dir>
 * Exercises the os.name dispatch and the Horizon file-system provider over the
 * host's own Unix natives. Overlay classes with Horizon-only natives
 * (SocketDispatcher, FileKey, NativeSeedGenerator) are covered by the device probe.
 */
public final class OverlayHostCheck {
    private static int checks;

    private static void check(String name, boolean ok) {
        if (!ok) throw new AssertionError(name);
        ++checks;
        System.out.println("overlay.check " + name + " PASS");
    }

    public static void main(String[] args) throws Exception {
        check("os_name", "Horizon".equals(System.getProperty("os.name")));
        check("provider_horizon", FileSystems.getDefault().provider().getClass().getName()
                .equals("sun.nio.fs.HorizonFileSystemProvider"));
        Path dir = FileSystems.getDefault().getPath(args[0], "overlay-check");
        Files.createDirectories(dir);
        Path file = dir.resolve("a.bin");
        byte[] data = new byte[5000];
        for (int i = 0; i < data.length; ++i) data[i] = (byte)(i * 7);
        Files.write(file, data);
        check("files_roundtrip", Arrays.equals(Files.readAllBytes(file), data));
        BasicFileAttributes attrs = Files.readAttributes(file, BasicFileAttributes.class);
        check("basic_attributes", attrs.isRegularFile() && attrs.size() == data.length);
        check("posix_view_absent", Files.getFileAttributeView(file, PosixFileAttributeView.class) == null);
        boolean rejected = false;
        try {
            Files.readAttributes(file, "posix:permissions");
        } catch (UnsupportedOperationException | IllegalArgumentException expected) {
            rejected = true;
        }
        check("posix_attributes_rejected", rejected);
        check("supported_views", FileSystems.getDefault().supportedFileAttributeViews().size() == 1);
        int n = 0;
        try (DirectoryStream<Path> stream = Files.newDirectoryStream(dir)) {
            for (Path ignored : stream) ++n;
        }
        check("directory_stream", n == 1);
        check("filestore", Files.getFileStore(dir).getTotalSpace() > 0
                && "horizon".equals(Files.getFileStore(dir).type()));
        try (WatchService watch = FileSystems.getDefault().newWatchService()) {
            check("polling_watch_service", watch.getClass().getName().equals("sun.nio.fs.PollingWatchService"));
        }
        Files.move(file, dir.resolve("b.bin"));
        check("move", Files.exists(dir.resolve("b.bin")) && !Files.exists(file));
        Files.delete(dir.resolve("b.bin"));
        Files.delete(dir);
        check("delete", !Files.exists(dir));
        boolean noProcess = false;
        try {
            new ProcessBuilder("true").start();
        } catch (IOException expected) {
            noProcess = expected.getMessage().contains("not supported on Horizon");
        }
        check("process_rejected", noProcess);
        boolean noAsync = false;
        try {
            AsynchronousChannelProvider.provider().openAsynchronousSocketChannel(null);
        } catch (UnsupportedOperationException expected) {
            noAsync = true;
        }
        check("async_rejected", noAsync);
        Class<?> portConfig = Class.forName("sun.net.PortConfig");
        java.lang.reflect.Method lower = portConfig.getDeclaredMethod("getLower");
        lower.setAccessible(true);
        check("portconfig_horizon", ((Integer)lower.invoke(null)) > 0);
        System.out.println("OVERLAY_HOST_PASS checks=" + checks);
    }
}
