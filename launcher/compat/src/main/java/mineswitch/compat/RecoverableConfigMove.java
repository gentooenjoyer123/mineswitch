package mineswitch.compat;

import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.io.OutputStreamWriter;
import java.nio.ByteBuffer;
import java.nio.channels.FileChannel;
import java.nio.channels.FileLock;
import java.nio.channels.OverlappingFileLockException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.LinkOption;
import java.nio.file.NoSuchFileException;
import java.nio.file.Path;
import java.nio.file.StandardOpenOption;
import java.nio.file.attribute.BasicFileAttributes;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.HexFormat;
import java.util.Properties;
import java.util.Set;
import java.util.UUID;

/**
 * Sodium-specific save policy for Horizon's non-replacing rename operation.
 * This protocol is recoverable; it does not claim cross-process atomicity or
 * power-loss guarantees. Every data move uses an absent destination.
 */
public final class RecoverableConfigMove {
    static final String SUFFIX = ".mineswitch-compat";
    private static final String OWNER = "mineswitch_compat:sodium-config:1";
    private static final int MAX_JOURNAL = 16 * 1024;
    private static final long MAX_CONFIG = 8L * 1024 * 1024;
    private static final LinkOption[] NOFOLLOW = {LinkOption.NOFOLLOW_LINKS};
    private static final Set<String> KEYS = Set.of("schema", "owner", "target", "nonce", "new.bytes", "new.sha256",
                                                  "old.present", "old.bytes", "old.sha256");
    interface FileOps {
        void move(Path source, Path target) throws IOException;
        default void delete(Path path) throws IOException { Files.delete(path); }
    }
    static final FileOps REAL = (source, target) -> Files.move(source, target);
    private record Fingerprint(long bytes, String sha256) {}
    private record Paths(Path root, Path source, Path target, Path stage, Path backup, Path journal, Path journalTemp) {}
    private record Transaction(Paths paths, Fingerprint next, Fingerprint previous, byte[] journalBytes) {}
    private record Recovery(boolean restoredMissingTarget) {}

    private RecoverableConfigMove() {}

    public static synchronized Path replace(Path gameRoot, Path source, Path target) throws IOException {
        return replace(gameRoot, source, target, REAL);
    }

    static synchronized Path replace(Path gameRoot, Path source, Path target, FileOps ops) throws IOException {
        Paths paths = paths(gameRoot, source, target);
        Recovery recovery = recover(paths, ops);
        // Prelaunch normally handles this before Sodium reads its config. If
        // recovery was first reached here, its in-memory model could be defaults
        // read while the target was absent. Never silently overwrite recovery.
        if (recovery.restoredMissingTarget())
            throw new IOException("Recovered an interrupted Sodium configuration; reload it before saving again");
        ensureUnownedSiblingsAbsent(paths);
        Fingerprint next = fingerprint(paths.source());
        Fingerprint previous = regularExists(paths.target()) ? fingerprint(paths.target()) : null;
        force(paths.source());
        byte[] journal = encode(paths, next, previous);
        writeNew(paths.journalTemp(), journal);
        moveChecked(paths.journalTemp(), paths.journal(), fingerprint(journal), ops);
        Transaction transaction = new Transaction(paths, next, previous, journal);
        boolean committed = false;
        try {
            try (JournalLock ignored = lock(paths.journal())) {
                requireJournal(transaction);
                moveChecked(paths.source(), paths.stage(), next, ops);
                if (previous != null) moveChecked(paths.target(), paths.backup(), previous, ops);
                else requireAbsent(paths.target());
                moveChecked(paths.stage(), paths.target(), next, ops);
                committed = true;
            }
        } catch (IOException failure) {
            if (!committed) {
                try { recover(paths, ops); }
                catch (IOException rollback) { failure.addSuppressed(rollback); }
                throw failure;
            }
            // The new target has already been checked. Cleanup can be retried
            // from its immutable journal without misreporting the saved data.
            System.err.println("MineSwitch compatibility: saved config; journal close deferred");
            return target;
        }
        try { recover(paths, ops); }
        catch (IOException cleanup) {
            System.err.println("MineSwitch compatibility: saved config; journal cleanup deferred");
        }
        return target;
    }

    /** Called before Sodium reads settings; only this adapter's journal is recovered. */
    public static synchronized void recoverBeforeLoad(Path gameRoot, Path target) throws IOException {
        recoverBeforeLoad(gameRoot, target, REAL);
    }

    static synchronized void recoverBeforeLoad(Path gameRoot, Path target, FileOps ops) throws IOException {
        Path source = target.resolveSibling(target.getFileName() + ".tmp");
        recover(paths(gameRoot, source, target), ops);
    }

    private static Recovery recover(Paths paths, FileOps ops) throws IOException {
        boolean journal = regularExists(paths.journal());
        boolean temporary = regularExists(paths.journalTemp());
        if (!journal) {
            if (regularExists(paths.stage()) || regularExists(paths.backup()))
                throw new IOException("Unowned Sodium configuration backup/staging file retained");
            if (temporary) {
                // No data move is allowed before journal publication. A valid
                // uncommitted journal can be retired without touching config.
                Transaction pending;
                try (JournalLock ignored = lock(paths.journalTemp())) {
                    pending = decode(paths, read(paths.journalTemp(), MAX_JOURNAL));
                }
                deleteChecked(paths.journalTemp(), fingerprint(pending.journalBytes()), ops);
            }
            return new Recovery(false);
        }
        Transaction transaction;
        boolean restored = false;
        try (JournalLock ignored = lock(paths.journal())) {
            transaction = decode(paths, read(paths.journal(), MAX_JOURNAL));
            if (temporary) deleteChecked(paths.journalTemp(), fingerprint(transaction.journalBytes()), ops);
            Fingerprint next = transaction.next(), previous = transaction.previous();
            boolean stage = regularExists(paths.stage()), backup = regularExists(paths.backup());
            if (stage) requireFingerprint(paths.stage(), next);
            if (backup) {
                if (previous == null) throw new IOException("Unexpected configuration backup retained");
                requireFingerprint(paths.backup(), previous);
            }
            boolean target = regularExists(paths.target());
            if (target && matches(paths.target(), next)) {
                // A promoted, verified target is committed even if cleanup was
                // interrupted. Never replace it with the older backup.
                if (stage) deleteChecked(paths.stage(), next, ops);
                if (backup) deleteChecked(paths.backup(), previous, ops);
            } else {
                if (target) {
                    if (previous == null || !matches(paths.target(), previous))
                        throw new IOException("Configuration changed outside its journal; all recovery files retained");
                } else if (previous != null) {
                    if (!backup) throw new IOException("Previous configuration backup is unavailable; journal retained");
                    moveChecked(paths.backup(), paths.target(), previous, ops);
                    backup = false;
                    restored = true;
                }
                if (backup) deleteChecked(paths.backup(), previous, ops);
                if (stage) {
                    // Preserve an unsuccessful payload if no newer Sodium temp
                    // exists. Never replace a new call's incoming temp file.
                    if (!regularExists(paths.source())) moveChecked(paths.stage(), paths.source(), next, ops);
                    else deleteChecked(paths.stage(), next, ops);
                }
            }
        }
        deleteChecked(paths.journal(), fingerprint(transaction.journalBytes()), ops);
        return new Recovery(restored);
    }

    private static Paths paths(Path gameRoot, Path source, Path target) throws IOException {
        Path root = gameRoot.toAbsolutePath().normalize();
        Path to = target.toAbsolutePath().normalize();
        Path from = source.toAbsolutePath().normalize();
        checkPath(root, true, false);
        if (to.equals(root) || !to.startsWith(root) || !from.startsWith(root) || !from.getParent().equals(to.getParent()))
            throw new IOException("Sodium configuration is outside the owned game directory");
        String name = to.getFileName().toString();
        if (!from.getFileName().toString().equals(name + ".tmp") || name.contains(SUFFIX) || name.length() > 180)
            throw new IOException("Unexpected Sodium configuration save paths");
        checkPath(to.getParent(), true, false);
        checkPath(from, false, true); checkPath(to, false, true);
        Path stage = to.resolveSibling(name + SUFFIX + ".stage");
        Path backup = to.resolveSibling(name + SUFFIX + ".backup");
        Path journal = to.resolveSibling(name + SUFFIX + ".journal");
        Path journalTemp = to.resolveSibling(name + SUFFIX + ".journal.tmp");
        for (Path path : new Path[]{stage, backup, journal, journalTemp}) checkPath(path, false, true);
        return new Paths(root, from, to, stage, backup, journal, journalTemp);
    }

    private static void checkPath(Path path, boolean directory, boolean allowMissing) throws IOException {
        Path current = path.getRoot();
        for (Path part : path) {
            String name = part.toString();
            if (name.isEmpty() || name.endsWith(".") || name.endsWith(" ") || name.indexOf('\\') >= 0 || name.indexOf(':') >= 0)
                throw new IOException("Ambiguous configuration path");
            for (int i = 0; i < name.length(); ++i) if (Character.isISOControl(name.charAt(i))) throw new IOException("Invalid configuration path");
            current = current.resolve(part);
            BasicFileAttributes attributes;
            try { attributes = Files.readAttributes(current, BasicFileAttributes.class, NOFOLLOW); }
            catch (NoSuchFileException missing) {
                if (allowMissing && current.equals(path)) return;
                throw missing;
            }
            if (attributes.isSymbolicLink()) throw new IOException("Configuration paths cannot follow symbolic links");
            if (!current.equals(path) || directory) {
                if (!attributes.isDirectory()) throw new IOException("Expected configuration directory");
            } else if (!attributes.isRegularFile()) throw new IOException("Expected regular configuration file");
        }
    }

    private static boolean regularExists(Path path) throws IOException {
        checkPath(path, false, true);
        return Files.exists(path, NOFOLLOW);
    }

    private static void requireAbsent(Path path) throws IOException {
        if (regularExists(path)) throw new IOException("Configuration destination already exists: " + path.getFileName());
    }

    private static void ensureUnownedSiblingsAbsent(Paths paths) throws IOException {
        for (Path path : new Path[]{paths.stage(), paths.backup(), paths.journal(), paths.journalTemp()}) requireAbsent(path);
    }

    private static void moveChecked(Path source, Path target, Fingerprint expected, FileOps ops) throws IOException {
        requireFingerprint(source, expected); requireAbsent(target);
        ops.move(source, target);
        if (regularExists(source)) throw new IOException("Configuration move did not consume its source");
        requireFingerprint(target, expected);
    }

    private static void deleteChecked(Path path, Fingerprint expected, FileOps ops) throws IOException {
        if (!regularExists(path)) return;
        requireFingerprint(path, expected);
        ops.delete(path);
        if (regularExists(path)) throw new IOException("Configuration recovery file was not removed");
    }

    private static void requireJournal(Transaction transaction) throws IOException {
        requireFingerprint(transaction.paths().journal(), fingerprint(transaction.journalBytes()));
    }

    private static boolean matches(Path path, Fingerprint expected) throws IOException {
        return fingerprint(path).equals(expected);
    }

    private static void requireFingerprint(Path path, Fingerprint expected) throws IOException {
        if (!matches(path, expected)) throw new IOException("Configuration recovery file differs from its journal: " + path.getFileName());
    }

    private static Fingerprint fingerprint(Path path) throws IOException {
        checkPath(path, false, false);
        return fingerprint(read(path, MAX_CONFIG));
    }

    private static Fingerprint fingerprint(byte[] bytes) {
        try {
            return new Fingerprint(bytes.length, HexFormat.of().formatHex(MessageDigest.getInstance("SHA-256").digest(bytes)));
        } catch (NoSuchAlgorithmException impossible) { throw new AssertionError(impossible); }
    }

    private static byte[] read(Path path, long maximum) throws IOException {
        if (Files.size(path) > maximum) throw new IOException("Configuration metadata exceeds its size limit");
        try (InputStream input = Files.newInputStream(path, LinkOption.NOFOLLOW_LINKS)) {
            byte[] bytes = input.readNBytes((int)maximum + 1);
            if (bytes.length > maximum) throw new IOException("Configuration metadata exceeds its size limit");
            return bytes;
        }
    }

    private static void force(Path path) throws IOException {
        try (FileChannel channel = FileChannel.open(path, StandardOpenOption.WRITE, LinkOption.NOFOLLOW_LINKS)) {
            channel.force(true);
        }
    }

    private static void writeNew(Path path, byte[] bytes) throws IOException {
        requireAbsent(path);
        try (FileChannel channel = FileChannel.open(path, StandardOpenOption.CREATE_NEW, StandardOpenOption.WRITE)) {
            ByteBuffer data = ByteBuffer.wrap(bytes);
            while (data.hasRemaining()) channel.write(data);
            channel.force(true);
        }
    }

    private static byte[] encode(Paths paths, Fingerprint next, Fingerprint previous) throws IOException {
        Properties properties = new Properties();
        properties.setProperty("schema", "1"); properties.setProperty("owner", OWNER);
        properties.setProperty("target", paths.root().relativize(paths.target()).toString());
        properties.setProperty("nonce", UUID.randomUUID().toString().replace("-", ""));
        properties.setProperty("new.bytes", Long.toString(next.bytes())); properties.setProperty("new.sha256", next.sha256());
        properties.setProperty("old.present", Boolean.toString(previous != null));
        properties.setProperty("old.bytes", previous == null ? "0" : Long.toString(previous.bytes()));
        properties.setProperty("old.sha256", previous == null ? "" : previous.sha256());
        ByteArrayOutputStream output = new ByteArrayOutputStream();
        try (var writer = new OutputStreamWriter(output, StandardCharsets.UTF_8)) { properties.store(writer, OWNER); }
        byte[] bytes = output.toByteArray();
        if (bytes.length > MAX_JOURNAL) throw new IOException("Configuration journal exceeds its size limit");
        return bytes;
    }

    private static final class UniqueProperties extends Properties {
        private static final long serialVersionUID = 1L;
        @Override public synchronized Object put(Object key, Object value) {
            if (containsKey(key)) throw new IllegalArgumentException("Duplicate journal field");
            return super.put(key, value);
        }
    }

    private static Transaction decode(Paths paths, byte[] bytes) throws IOException {
        Properties properties = new UniqueProperties();
        try (var reader = new InputStreamReader(new ByteArrayInputStream(bytes), StandardCharsets.UTF_8)) {
            properties.load(reader);
        } catch (IllegalArgumentException invalid) { throw new IOException("Ambiguous configuration journal retained", invalid); }
        if (!properties.stringPropertyNames().equals(KEYS) || !"1".equals(properties.getProperty("schema")) ||
            !OWNER.equals(properties.getProperty("owner")) ||
            !paths.root().relativize(paths.target()).toString().equals(properties.getProperty("target")) ||
            !properties.getProperty("nonce", "").matches("[0-9a-f]{32}"))
            throw new IOException("Unowned or invalid configuration journal retained");
        Fingerprint next = journalFingerprint(properties, "new");
        String hadOld = properties.getProperty("old.present");
        Fingerprint previous;
        if (hadOld.equals("true")) previous = journalFingerprint(properties, "old");
        else if (hadOld.equals("false") && properties.getProperty("old.bytes").equals("0") && properties.getProperty("old.sha256").isEmpty()) previous = null;
        else throw new IOException("Invalid previous configuration state; journal retained");
        return new Transaction(paths, next, previous, bytes);
    }

    private static Fingerprint journalFingerprint(Properties properties, String prefix) throws IOException {
        String length = properties.getProperty(prefix + ".bytes"), hash = properties.getProperty(prefix + ".sha256");
        if (!length.matches("[0-9]{1,9}") || !hash.matches("[0-9a-f]{64}")) throw new IOException("Invalid journal fingerprint");
        long bytes = Long.parseLong(length);
        if (bytes > MAX_CONFIG) throw new IOException("Journal file exceeds its size limit");
        return new Fingerprint(bytes, hash);
    }

    private static JournalLock lock(Path path) throws IOException {
        FileChannel channel = FileChannel.open(path, StandardOpenOption.READ, StandardOpenOption.WRITE, LinkOption.NOFOLLOW_LINKS);
        try {
            FileLock lock = channel.tryLock();
            if (lock == null) throw new IOException("Configuration journal is active in another process");
            return new JournalLock(channel, lock);
        } catch (IOException | OverlappingFileLockException error) {
            channel.close();
            throw new IOException("Configuration journal is busy or unavailable", error);
        }
    }

    private record JournalLock(FileChannel channel, FileLock lock) implements AutoCloseable {
        @Override public void close() throws IOException {
            try { lock.release(); } finally { channel.close(); }
        }
    }
}
