package mineswitch.launcher;

import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.BufferedOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.io.OutputStreamWriter;
import java.io.PrintStream;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Modifier;
import java.net.URL;
import java.net.URLClassLoader;
import java.nio.ByteBuffer;
import java.nio.channels.FileChannel;
import java.nio.charset.CodingErrorAction;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.LinkOption;
import java.nio.file.NoSuchFileException;
import java.nio.file.Path;
import java.nio.file.StandardOpenOption;
import java.nio.file.attribute.BasicFileAttributes;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.HexFormat;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Properties;
import java.util.Set;
import java.util.jar.JarFile;
import java.util.jar.Manifest;

/**
 * One-shot entry point for already verified official installer processors.
 * Native preparation resolves all arguments and paths before launching this
 * separate JVM. Classloader isolation is not a filesystem/security sandbox:
 * processor code is trusted and must use separate output paths.
 */
public final class ProcessorEntry {
    private static final int MAX_PLAN_BYTES = 1024 * 1024;
    private static final int MAX_PROCESSORS = 128;
    private static final int MAX_CLASSPATH = 512;
    private static final int MAX_ARGS = 512;
    private static final int MAX_OUTPUTS_PER_PROCESSOR = 256;
    private static final int MAX_OUTPUTS = 4096;
    private static final int MAX_INPUTS = 64;
    private static final long MAX_FILE_BYTES = (1L << 32) - 1;
    private static final int MAX_LOG_BYTES = 8 * 1024 * 1024;
    private static final int LOG_FAILURE_RESERVE = 64 * 1024;
    private static final byte[] LOG_TRUNCATED = (
            "\n[MineSwitch] Verbose output truncated; space reserved for failure diagnostics.\n")
            .getBytes(StandardCharsets.UTF_8);
    private static final LinkOption[] NOFOLLOW = { LinkOption.NOFOLLOW_LINKS };

    private ProcessorEntry() {}

    private record Output(Path path, String sha1) {}
    private record Processor(Path jar, List<Path> classpath, String[] args, List<Output> outputs) {}
    private record Plan(Path file, Path root, Path completion, Path log, String sha256,
                        List<Processor> processors, List<Path> inputs, List<Output> outputs) {}
    private record Digests(String sha256, String sha1) {}

    /** Bounded, buffered tool output with reserved room for a failure stack. */
    private static final class BoundedLog extends OutputStream {
        private final OutputStream output;
        private long written;
        private boolean truncated;
        private boolean diagnostics;
        private IOException failure;

        BoundedLog(Path path) throws IOException {
            checkPath(path, true, false);
            // Recreate instead of truncating an existing inode. readPlan also
            // rejects aliases to protected inputs before any file is removed.
            Files.deleteIfExists(path);
            output = new BufferedOutputStream(Files.newOutputStream(path,
                    StandardOpenOption.CREATE_NEW, StandardOpenOption.WRITE, LinkOption.NOFOLLOW_LINKS), 64 * 1024);
        }

        @Override
        public synchronized void write(int value) throws IOException {
            write(new byte[] { (byte) value }, 0, 1);
        }

        @Override
        public synchronized void write(byte[] bytes, int offset, int length) throws IOException {
            java.util.Objects.checkFromIndexSize(offset, length, bytes.length);
            int limit = diagnostics ? MAX_LOG_BYTES : MAX_LOG_BYTES - LOG_FAILURE_RESERVE - LOG_TRUNCATED.length;
            int keep = (int) Math.min(length, Math.max(0L, limit - written));
            try {
                if (keep != 0) { output.write(bytes, offset, keep); written += keep; }
                if (!diagnostics && keep < length && !truncated) {
                    output.write(LOG_TRUNCATED); written += LOG_TRUNCATED.length; truncated = true;
                }
            } catch (IOException error) { failure = error; throw error; }
        }

        @Override
        public synchronized void flush() throws IOException {
            try { output.flush(); } catch (IOException error) { failure = error; throw error; }
        }

        @Override
        public synchronized void close() throws IOException { output.close(); }

        synchronized void checkFailure() throws IOException {
            flush();
            if (failure != null) throw new IOException("Cannot write processor log", failure);
        }

        PrintStream stream() {
            // A processor may close System.out/err; it must not close the
            // shared log or the caller's console streams.
            OutputStream view = new OutputStream() {
                @Override public void write(int value) throws IOException { BoundedLog.this.write(value); }
                @Override public void write(byte[] bytes, int offset, int length) throws IOException {
                    BoundedLog.this.write(bytes, offset, length);
                }
                @Override public void flush() throws IOException { BoundedLog.this.flush(); }
                @Override public void close() throws IOException { flush(); }
            };
            return new PrintStream(view, false, StandardCharsets.UTF_8);
        }

        void recordFailure(Throwable error) {
            synchronized (this) { diagnostics = true; }
            try (PrintStream stream = stream()) {
                stream.println("\n[MineSwitch] Preparation failed:");
                error.printStackTrace(stream);
                checkFailure();
            } catch (IOException logging) { if (logging != error) error.addSuppressed(logging); }
        }
    }

    private static final class UniqueProperties extends Properties {
        private static final long serialVersionUID = 1L;
        @Override
        public synchronized Object put(Object key, Object value) {
            if (containsKey(key)) throw new IllegalArgumentException("Duplicate plan field: " + key);
            return super.put(key, value);
        }
    }

    private static final class Fields {
        private final Properties properties;
        private final Set<String> used = new HashSet<>();

        Fields(byte[] bytes) throws IOException {
            properties = new UniqueProperties();
            var decoder = StandardCharsets.UTF_8.newDecoder()
                    .onMalformedInput(CodingErrorAction.REPORT)
                    .onUnmappableCharacter(CodingErrorAction.REPORT);
            try (var reader = new InputStreamReader(new ByteArrayInputStream(bytes), decoder)) {
                properties.load(reader);
            }
        }

        String required(String key) throws IOException {
            String value = optional(key);
            if (value == null) throw new IOException("Missing plan field: " + key);
            return value;
        }

        String optional(String key) {
            String value = properties.getProperty(key);
            if (value != null) used.add(key);
            return value;
        }

        int count(String key, int maximum, boolean optional) throws IOException {
            String text = optional ? optional(key) : required(key);
            if (text == null) return 0;
            if (!text.matches("[0-9]{1,9}")) throw new IOException("Invalid plan count: " + key);
            int value = Integer.parseInt(text);
            if (value > maximum) throw new IOException("Plan count exceeds limit: " + key);
            return value;
        }

        void finish() throws IOException {
            if (!used.equals(properties.stringPropertyNames())) throw new IOException("Unrecognized plan fields");
        }
    }

    public static void main(String[] args) throws Exception {
        if (args.length != 1) throw new IllegalArgumentException("Usage: ProcessorEntry PLAN_PROPERTIES");
        Plan plan = readPlan(args[0]);
        Path temporary = inside(plan.root(), plan.completion() + ".tmp");
        // Invalidate stale success before loading any processor class. If a tool
        // calls System.exit, this process ends with no success marker.
        removeMarker(plan.completion());
        removeMarker(temporary);
        PrintStream consoleOut = System.out, consoleErr = System.err;
        ClassLoader context = Thread.currentThread().getContextClassLoader();
        try (BoundedLog log = new BoundedLog(plan.log())) {
            Thread shutdown = new Thread(() -> {
                try { log.flush(); } catch (IOException ignored) { }
            }, "mineswitch-processor-log");
            Runtime.getRuntime().addShutdownHook(shutdown);
            try {
                consoleOut.println("Processor log: " + plan.log());
                execute(plan, temporary, log, consoleOut);
            } catch (Exception | Error error) {
                log.recordFailure(error);
                consoleErr.println("Loader preparation failed. Details: " + plan.log());
                throw error;
            } finally {
                System.setOut(consoleOut); System.setErr(consoleErr);
                Thread.currentThread().setContextClassLoader(context);
                try { Runtime.getRuntime().removeShutdownHook(shutdown); }
                catch (IllegalStateException shuttingDown) { }
            }
        } catch (Exception | Error error) {
            // Includes log flush/close failures after an otherwise successful
            // pipeline: no failure may leave a published success marker.
            for (Path marker : List.of(plan.completion(), temporary)) {
                try { removeMarker(marker); } catch (IOException cleanup) { error.addSuppressed(cleanup); }
            }
            throw error;
        }
    }

    private static void execute(Plan plan, Path temporary, BoundedLog log, PrintStream console) throws Exception {
        Map<Path, String> protectedHashes = new LinkedHashMap<>();
        for (Path input : plan.inputs()) protectedHashes.put(input, digests(input, false).sha256());

        for (int i = 0; i < plan.processors().size(); ++i) {
            Processor processor = plan.processors().get(i);
            String step = "Processor " + (i + 1) + "/" + plan.processors().size();
            console.println(step + ": " + processor.jar().getFileName());
            try (PrintStream notes = log.stream()) { notes.println("\n[MineSwitch] " + step + ": " + processor.jar()); }
            verifyInputs(protectedHashes);
            for (Output output : processor.outputs()) createParents(plan.root(), output.path());
            invokeLogged(processor, log);
            verifyInputs(protectedHashes);
            for (Output output : processor.outputs()) verifyOutput(output);
            log.checkFailure();
            console.println(step + " complete");
        }

        Properties completed = new Properties();
        completed.setProperty("schema", "1");
        completed.setProperty("state", "complete");
        completed.setProperty("plan_file", plan.file().toString());
        completed.setProperty("plan_sha256", plan.sha256());
        completed.setProperty("processor.count", Integer.toString(plan.processors().size()));
        completed.setProperty("output.count", Integer.toString(plan.outputs().size()));
        for (int i = 0; i < plan.outputs().size(); ++i) {
            Output output = plan.outputs().get(i);
            Digests hashes = verifyOutput(output);
            String prefix = "output." + i + ".";
            completed.setProperty(prefix + "path", output.path().toString());
            completed.setProperty(prefix + "sha256", hashes.sha256());
            if (output.sha1() != null) completed.setProperty(prefix + "sha1", hashes.sha1());
        }
        verifyInputs(protectedHashes);
        log.checkFailure();
        publish(plan, temporary, completed);
        console.println("Loader preparation complete. Reopen the launcher.");
    }

    private static void invokeLogged(Processor processor, BoundedLog log) throws Exception {
        PrintStream previousOut = System.out, previousErr = System.err;
        try (PrintStream output = log.stream(); PrintStream error = log.stream()) {
            try {
                System.setOut(output); System.setErr(error);
                invoke(processor);
            } finally {
                System.setOut(previousOut); System.setErr(previousErr);
                output.flush(); error.flush();
            }
        }
        log.checkFailure();
    }

    private static Plan readPlan(String filename) throws Exception {
        Path file = absolute(filename);
        checkPath(file, false, false);
        byte[] bytes;
        try (InputStream stream = Files.newInputStream(file, StandardOpenOption.READ, LinkOption.NOFOLLOW_LINKS)) {
            bytes = boundedRead(stream, MAX_PLAN_BYTES);
        }
        Fields fields = new Fields(bytes);
        if (!fields.required("schema").equals("1")) throw new IOException("Unsupported processor plan schema");
        Path root = absolute(fields.required("storage_root"));
        checkPath(root, false, true);
        inside(root, file.toString());
        Path completion = inside(root, fields.required("completion_file"));
        Path temporary = inside(root, completion + ".tmp");
        Path log = inside(root, file.getParent().resolve("processor.log").toString());
        checkPath(completion, true, false);
        checkPath(temporary, true, false);
        checkPath(log, true, false);
        int count = fields.count("processor.count", MAX_PROCESSORS, false);
        List<Processor> processors = new ArrayList<>();
        List<Path> protectedInputs = new ArrayList<>();
        Set<String> immutable = new HashSet<>();
        List<Path> immutablePaths = new ArrayList<>(List.of(file, completion, temporary, log));
        for (Path path : immutablePaths) immutable.add(identity(path));
        if (immutable.size() != 4) throw new IOException("Plan, completion and log paths must be separate");
        Set<String> reserved = Set.copyOf(immutable);
        int inputCount = fields.count("input.count", MAX_INPUTS, true);
        for (int i = 0; i < inputCount; ++i) {
            Path input = inside(root, fields.required("input." + i + ".path"));
            checkPath(input, false, false);
            if (!immutable.add(identity(input))) throw new IOException("Protected input paths must be distinct");
            protectedInputs.add(input);
            immutablePaths.add(input);
        }
        Map<String, Output> finalOutputs = new LinkedHashMap<>();
        int totalClasspath = 0;
        for (int i = 0; i < count; ++i) {
            String prefix = "processor." + i + ".";
            Path jar = inside(root, fields.required(prefix + "jar"));
            checkPath(jar, false, false);
            if (reserved.contains(identity(jar))) throw new IOException("Processor jar overlaps plan bookkeeping");
            immutable.add(identity(jar));
            immutablePaths.add(jar);
            List<Path> classpath = new ArrayList<>();
            int classCount = fields.count(prefix + "classpath.count", MAX_CLASSPATH, false);
            totalClasspath += classCount;
            if (totalClasspath > 4096) throw new IOException("Total processor classpath exceeds its limit");
            for (int j = 0; j < classCount; ++j) {
                Path dependency = inside(root, fields.required(prefix + "classpath." + j));
                checkPath(dependency, false, false);
                if (reserved.contains(identity(dependency))) throw new IOException("Processor classpath overlaps plan bookkeeping");
                immutable.add(identity(dependency));
                immutablePaths.add(dependency);
                classpath.add(dependency);
            }
            int argumentCount = fields.count(prefix + "args.count", MAX_ARGS, false);
            String[] arguments = new String[argumentCount];
            for (int j = 0; j < argumentCount; ++j) {
                String argument = fields.required(prefix + "args." + j);
                if (argument.length() > 16384 || argument.indexOf('\0') >= 0)
                    throw new IOException("Processor argument exceeds its bounds");
                arguments[j] = argument;
            }
            int outputCount = fields.count(prefix + "outputs.count", MAX_OUTPUTS_PER_PROCESSOR, false);
            List<Output> outputs = new ArrayList<>();
            Set<String> stepOutputs = new HashSet<>();
            for (int j = 0; j < outputCount; ++j) {
                String key = prefix + "outputs." + j + ".";
                Path output = inside(root, fields.required(key + "path"));
                checkPath(output, true, false);
                String sha1 = fields.optional(key + "sha1");
                if (sha1 != null && !sha1.matches("[0-9a-fA-F]{40}")) throw new IOException("Invalid expected output SHA1");
                if (!stepOutputs.add(identity(output))) throw new IOException("Duplicate processor output");
                Output spec = new Output(output, sha1 == null ? null : sha1.toLowerCase(Locale.ROOT));
                outputs.add(spec);
                Output previous = finalOutputs.put(identity(output), spec);
                if (previous != null && !previous.path().equals(output)) throw new IOException("Case-aliased processor outputs");
                if (finalOutputs.size() > MAX_OUTPUTS) throw new IOException("Too many processor outputs");
            }
            processors.add(new Processor(jar, List.copyOf(classpath), arguments, List.copyOf(outputs)));
        }
        fields.finish();
        for (Path path : immutablePaths) if (!path.equals(log)) checkLogSeparation(log, path);
        for (Output output : finalOutputs.values()) {
            checkLogSeparation(log, output.path());
            if (immutable.contains(identity(output.path()))) throw new IOException("Processor output overlaps an immutable input");
            for (String input : immutable) {
                if (input.startsWith(identity(output.path()) + "/") || identity(output.path()).startsWith(input + "/"))
                    throw new IOException("Processor output conflicts with an input path");
            }
        }
        return new Plan(file, root, completion, log, hash(bytes, "SHA-256"), List.copyOf(processors),
                        List.copyOf(protectedInputs), List.copyOf(finalOutputs.values()));
    }

    private static void checkLogSeparation(Path log, Path other) throws IOException {
        String first = identity(log), second = identity(other);
        if (first.equals(second) || first.startsWith(second + "/") || second.startsWith(first + "/") ||
                (Files.exists(log, NOFOLLOW) && Files.exists(other, NOFOLLOW) && Files.isSameFile(log, other)))
            throw new IOException("Processor log overlaps plan data, an input or an output");
    }

    private static void invoke(Processor processor) throws Exception {
        String mainClass;
        try (JarFile jar = new JarFile(processor.jar().toFile(), true)) {
            var entry = jar.getJarEntry("META-INF/MANIFEST.MF");
            if (entry == null) throw new IOException("Processor jar has no manifest");
            try (InputStream stream = jar.getInputStream(entry)) {
                Manifest manifest = new Manifest(new ByteArrayInputStream(boundedRead(stream, 64 * 1024)));
                mainClass = manifest.getMainAttributes().getValue("Main-Class");
            }
        }
        if (mainClass == null || mainClass.length() > 512 ||
            !mainClass.matches("[A-Za-z_$][A-Za-z0-9_$]*(\\.[A-Za-z_$][A-Za-z0-9_$]*)*"))
            throw new IOException("Processor jar has no valid Main-Class");
        if (mainClass.startsWith("net.minecraftforge.installer.") || mainClass.startsWith("net.neoforged.installer."))
            throw new IOException("Installer frontends are not processor tools");
        List<URL> urls = new ArrayList<>();
        urls.add(processor.jar().toUri().toURL());
        for (Path path : processor.classpath()) urls.add(path.toUri().toURL());
        Thread thread = Thread.currentThread();
        ClassLoader previous = thread.getContextClassLoader();
        try (URLClassLoader loader = new URLClassLoader(urls.toArray(URL[]::new), ClassLoader.getPlatformClassLoader())) {
            thread.setContextClassLoader(loader);
            try {
                Class<?> entry = Class.forName(mainClass, true, loader);
                var main = entry.getMethod("main", String[].class);
                if (!Modifier.isStatic(main.getModifiers()) || main.getReturnType() != void.class)
                    throw new IOException("Processor Main-Class has no public static void main");
                main.invoke(null, (Object) processor.args().clone());
            } catch (InvocationTargetException error) {
                Throwable cause = error.getCause();
                if (cause instanceof Exception exception) throw exception;
                if (cause instanceof Error fatal) throw fatal;
                throw new IOException("Processor failed", cause);
            } finally {
                thread.setContextClassLoader(previous);
            }
        }
    }

    private static void verifyInputs(Map<Path, String> inputs) throws Exception {
        for (var input : inputs.entrySet()) {
            if (!digests(input.getKey(), false).sha256().equals(input.getValue()))
                throw new IOException("Processor changed a protected input");
        }
    }

    private static Digests verifyOutput(Output output) throws Exception {
        Digests result = digests(output.path(), output.sha1() != null);
        if (output.sha1() != null && !result.sha1().equals(output.sha1())) throw new IOException("Processor output SHA1 mismatch");
        return result;
    }

    private static Digests digests(Path path, boolean needSha1) throws Exception {
        checkPath(path, false, false);
        MessageDigest sha256 = MessageDigest.getInstance("SHA-256");
        MessageDigest sha1 = needSha1 ? MessageDigest.getInstance("SHA-1") : null;
        long count = 0;
        byte[] buffer = new byte[64 * 1024];
        try (InputStream stream = Files.newInputStream(path, StandardOpenOption.READ, LinkOption.NOFOLLOW_LINKS)) {
            int read;
            while ((read = stream.read(buffer)) != -1) {
                if (read > MAX_FILE_BYTES - count) throw new IOException("Processor file exceeds the SD file-size limit");
                count += read;
                sha256.update(buffer, 0, read);
                if (sha1 != null) sha1.update(buffer, 0, read);
            }
        }
        return new Digests(HexFormat.of().formatHex(sha256.digest()), sha1 == null ? null : HexFormat.of().formatHex(sha1.digest()));
    }

    private static String hash(byte[] bytes, String algorithm) throws Exception {
        return HexFormat.of().formatHex(MessageDigest.getInstance(algorithm).digest(bytes));
    }

    private static byte[] boundedRead(InputStream stream, int maximum) throws IOException {
        ByteArrayOutputStream result = new ByteArrayOutputStream();
        byte[] buffer = new byte[8192];
        int count;
        while ((count = stream.read(buffer)) != -1) {
            if (count > maximum - result.size()) throw new IOException("Processor metadata exceeds its size limit");
            result.write(buffer, 0, count);
        }
        return result.toByteArray();
    }

    private static Path absolute(String text) throws IOException {
        if (text.isEmpty() || text.getBytes(StandardCharsets.UTF_8).length > 4096 || text.indexOf('\\') >= 0 || text.indexOf(':') >= 0)
            throw new IOException("Invalid absolute processor path");
        for (int i = 0; i < text.length(); ++i) if (Character.isISOControl(text.charAt(i))) throw new IOException("Control character in processor path");
        Path path = Path.of(text);
        if (!path.isAbsolute() || !path.equals(path.normalize())) throw new IOException("Processor path must be absolute and normalized");
        for (Path component : path) {
            String name = component.toString();
            if (name.endsWith(".") || name.endsWith(" ") || name.getBytes(StandardCharsets.UTF_8).length > 255)
                throw new IOException("Ambiguous processor path component");
        }
        return path;
    }

    private static Path inside(Path root, String text) throws IOException {
        Path path = absolute(text);
        if (path.equals(root) || !path.startsWith(root)) throw new IOException("Processor path is outside storage_root");
        return path;
    }

    private static String identity(Path path) { return path.toString().toLowerCase(Locale.ROOT); }

    private static void checkPath(Path path, boolean allowMissing, boolean directory) throws IOException {
        Path current = path.getRoot();
        for (Path component : path) {
            current = current.resolve(component);
            BasicFileAttributes attributes;
            try { attributes = Files.readAttributes(current, BasicFileAttributes.class, NOFOLLOW); }
            catch (NoSuchFileException missing) {
                if (allowMissing) return;
                throw missing;
            }
            if (attributes.isSymbolicLink()) throw new IOException("Processor paths cannot follow symbolic links");
            boolean last = current.equals(path);
            if ((!last || directory) && !attributes.isDirectory()) throw new IOException("Expected processor directory");
            if (last && !directory && (!attributes.isRegularFile() || attributes.size() > MAX_FILE_BYTES))
                throw new IOException("Expected a bounded regular processor file");
        }
    }

    private static void createParents(Path root, Path file) throws IOException {
        Path current = root;
        for (Path component : root.relativize(file.getParent())) {
            current = current.resolve(component);
            if (!Files.exists(current, NOFOLLOW)) Files.createDirectory(current);
            checkPath(current, false, true);
        }
    }

    private static void removeMarker(Path path) throws IOException {
        checkPath(path, true, false);
        Files.deleteIfExists(path);
    }

    private static void publish(Plan plan, Path temporary, Properties properties) throws IOException {
        createParents(plan.root(), temporary);
        ByteArrayOutputStream encoded = new ByteArrayOutputStream();
        try (var writer = new OutputStreamWriter(encoded, StandardCharsets.UTF_8)) {
            properties.store(writer, "MineSwitch completed processor plan");
        }
        byte[] bytes = encoded.toByteArray();
        try {
            try (FileChannel channel = FileChannel.open(temporary, StandardOpenOption.CREATE_NEW, StandardOpenOption.WRITE,
                                                        LinkOption.NOFOLLOW_LINKS)) {
                ByteBuffer buffer = ByteBuffer.wrap(bytes);
                while (buffer.hasRemaining()) channel.write(buffer);
                channel.force(true);
            }
            // No replacement required: stale completion was removed before the
            // first processor. An interrupted move cannot imply a partial plan.
            Files.move(temporary, plan.completion());
        } catch (IOException error) {
            try { Files.deleteIfExists(temporary); } catch (IOException cleanup) { error.addSuppressed(cleanup); }
            throw error;
        }
    }
}
