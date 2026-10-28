package mineswitch.compat;

import com.llamalad7.mixinextras.wrapper.factory.FactoryRedirectWrapperMixinTransformer;
import java.io.IOException;
import java.nio.channels.FileChannel;
import java.nio.channels.FileLock;
import java.nio.file.FileAlreadyExistsException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardOpenOption;
import java.util.Comparator;
import java.util.List;
import org.objectweb.asm.ClassReader;
import org.objectweb.asm.tree.AnnotationNode;
import org.objectweb.asm.tree.ClassNode;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Redirect;

public final class CompatibilityTests {
    private static int checks;
    private static void check(boolean value, String message) {
        if (!value) throw new AssertionError(message);
        ++checks;
    }
    private interface Action { void run() throws IOException; }
    private static void fails(Action action, String message) {
        try { action.run(); }
        catch (IOException expected) { ++checks; return; }
        throw new AssertionError(message);
    }
    private static class StrictMoves implements RecoverableConfigMove.FileOps {
        @Override public void move(Path source, Path target) throws IOException {
            if (Files.exists(target)) throw new FileAlreadyExistsException(target.toString());
            Files.move(source, target);
        }
    }
    private static final class Interrupted extends Error {
        private static final long serialVersionUID = 1L;
    }
    private static RecoverableConfigMove.FileOps crashAfter(int step) {
        return new StrictMoves() {
            private int moves;
            @Override public void move(Path source, Path target) throws IOException {
                super.move(source, target);
                if (++moves == step) throw new Interrupted();
            }
        };
    }
    private static RecoverableConfigMove.FileOps failBefore(int step) {
        return new StrictMoves() {
            private int moves;
            @Override public void move(Path source, Path target) throws IOException {
                if (++moves == step) throw new IOException("Ordinary interrupted IO fixture");
                super.move(source, target);
            }
        };
    }
    private static Path sibling(Path target, String ending) {
        return target.resolveSibling(target.getFileName() + RecoverableConfigMove.SUFFIX + ending);
    }
    private static void cleanJournal(Path target) {
        for (String ending : new String[]{".stage", ".backup", ".journal", ".journal.tmp"})
            check(!Files.exists(sibling(target, ending)), "journal sibling cleaned: " + ending);
    }
    private static void save(Path root, Path target, String text, RecoverableConfigMove.FileOps moves) throws IOException {
        Path source = target.resolveSibling(target.getFileName() + ".tmp");
        Files.writeString(source, text);
        RecoverableConfigMove.replace(root, source, target, moves);
    }
    private static Object annotationValue(AnnotationNode annotation, String name) {
        if (annotation.values != null) {
            for (int index = 0; index < annotation.values.size(); index += 2) {
                if (name.equals(annotation.values.get(index))) return annotation.values.get(index + 1);
            }
        }
        return null;
    }
    private static void compiledRedirectMatchesRuntime() throws Exception {
        ClassNode mixin = new ClassNode();
        String resource = "/" + CompatibilityGate.MIXIN.replace('.', '/') + ".class";
        try (var bytes = CompatibilityTests.class.getResourceAsStream(resource)) {
            check(bytes != null, "compiled companion mixin is available for annotation verification");
            new ClassReader(bytes).accept(mixin, ClassReader.SKIP_CODE | ClassReader.SKIP_DEBUG | ClassReader.SKIP_FRAMES);
        }
        AnnotationNode redirect = null;
        int redirects = 0;
        for (var method : mixin.methods) {
            if (method.visibleAnnotations == null) continue;
            for (var annotation : method.visibleAnnotations) {
                if (!"Lorg/spongepowered/asm/mixin/injection/Redirect;".equals(annotation.desc)) continue;
                redirect = annotation;
                ++redirects;
            }
        }
        check(redirects == 1, "compiled companion has exactly one Redirect");
        Object atValue = annotationValue(redirect, "at");
        // MixinExtras 0.5.4 casts this value to AnnotationNode even for INVOKE.
        // Compiling against Sponge 0.17.4 silently emits a List<AnnotationNode>.
        check(atValue instanceof AnnotationNode, "compiled Redirect.at must be one AnnotationNode for Sponge 0.17.3 and MixinExtras 0.5.4");
        check(Redirect.class.getDeclaredMethod("at").getReturnType() == At.class,
              "pinned runtime Redirect API takes a single At");
        AnnotationNode at = (AnnotationNode) atValue;
        check("Lorg/spongepowered/asm/mixin/injection/At;".equals(at.desc), "compiled injection point is At");
        check("INVOKE".equals(annotationValue(at, "value")), "compiled injection point selects an invocation");
        check(("Ljava/nio/file/Files;move" + CompatibilityGate.MOVE_DESCRIPTOR).equals(annotationValue(at, "target")),
              "compiled injection point selects the exact Files.move overload");
        check(List.of(CompatibilityGate.METHOD + CompatibilityGate.METHOD_DESCRIPTOR).equals(annotationValue(redirect, "method")),
              "compiled redirect selects exactly the checked Sodium method");
        check(Boolean.FALSE.equals(annotationValue(at, "remap")) && Boolean.FALSE.equals(annotationValue(redirect, "remap")),
              "compiled Java and Sodium names are not remapped");
        check(Integer.valueOf(1).equals(annotationValue(redirect, "require")) && Integer.valueOf(1).equals(annotationValue(redirect, "allow")),
              "compiled redirect requires exactly one match");
        new FactoryRedirectWrapperMixinTransformer().transform(null, mixin);
        ++checks;
    }
    public static void main(String[] args) throws Exception {
        check(WorldDirectoryNames.storageName("Новый мир").equals("World"), "Russian world storage name");
        check(WorldDirectoryNames.storageName("Новый мир (1)").equals("World"), "Vanilla will resolve duplicate world folders");
        check(WorldDirectoryNames.storageName("世界 🌍").equals("World"), "Other Unicode world names");
        check(WorldDirectoryNames.storageName("My World (2)").equals("My World (2)"), "ASCII world names unchanged");
        compiledRedirectMatchesRuntime();
        Path root = Files.createTempDirectory("mineswitch-compat-");
        try {
            Path config = Files.createDirectory(root.resolve("config"));
            Path target = config.resolve("sodium-options.json");
            StrictMoves moves = new StrictMoves();
            save(root, target, "first config", moves);
            check(Files.readString(target).equals("first config"), "first config created");
            save(root, target, "updated config", moves);
            check(Files.readString(target).equals("updated config"), "existing config replaced with non-replacing moves");
            cleanJournal(target);

            for (int step : new int[]{3, 4}) {
                Files.writeString(target, "old settings");
                fails(() -> save(root, target, "new settings", failBefore(step)), "expected real move failure");
                check(Files.readString(target).equals("old settings"), "old config restored after failed move");
                check(Files.readString(target.resolveSibling(target.getFileName() + ".tmp")).equals("new settings"), "failed payload retained for retry");
                cleanJournal(target);
            }
            for (int step = 1; step <= 4; ++step) {
                Files.writeString(target, "before restart");
                try { save(root, target, "saved before restart", crashAfter(step)); throw new AssertionError("expected interruption"); }
                catch (Interrupted expected) { ++checks; }
                RecoverableConfigMove.recoverBeforeLoad(root, target, moves);
                String expected = step == 4 ? "saved before restart" : "before restart";
                check(Files.readString(target).equals(expected), "prelaunch recovers the correct committed config before loading defaults");
                cleanJournal(target);
                save(root, target, "next normal save", moves);
                check(Files.readString(target).equals("next normal save"), "normal save after interrupted transaction");
            }

            Files.writeString(target, "protected old config");
            Files.writeString(sibling(target, ".backup"), "unowned backup");
            fails(() -> save(root, target, "new config", moves), "unowned backup must be retained");
            check(Files.readString(target).equals("protected old config") && Files.readString(sibling(target, ".backup")).equals("unowned backup"), "ambiguous backup and config remain untouched");
            Files.delete(sibling(target, ".backup"));
            Files.writeString(sibling(target, ".journal.tmp"), "unowned partial journal");
            fails(() -> RecoverableConfigMove.recoverBeforeLoad(root, target, moves), "partial unowned journal must be retained");
            check(Files.readString(sibling(target, ".journal.tmp")).equals("unowned partial journal"), "invalid journal retained");
            Files.delete(sibling(target, ".journal.tmp"));

            try { save(root, target, "interrupted pending save", crashAfter(3)); throw new AssertionError("expected interruption"); }
            catch (Interrupted expected) { ++checks; }
            fails(() -> save(root, target, "possibly stale defaults", moves), "late gap recovery requires reload");
            check(Files.readString(target).equals("protected old config"), "late recovery cannot overwrite old settings with stale defaults");
            cleanJournal(target);

            try { save(root, target, "pending", crashAfter(1)); throw new AssertionError("expected interruption"); }
            catch (Interrupted expected) { ++checks; }
            try (FileChannel channel = FileChannel.open(sibling(target, ".journal"), StandardOpenOption.READ, StandardOpenOption.WRITE);
                 FileLock lock = channel.lock()) {
                check(lock.isValid(), "normal competing journal lock");
                fails(() -> RecoverableConfigMove.recoverBeforeLoad(root, target, moves), "active journal cannot be recovered concurrently");
                check(Files.readString(target).equals("protected old config"), "active transaction leaves config intact");
            }
            RecoverableConfigMove.recoverBeforeLoad(root, target, moves);
            cleanJournal(target);

            var cleanupFailure = new StrictMoves() {
                private boolean failed;
                @Override public void delete(Path path) throws IOException {
                    if (!failed && path.equals(sibling(target, ".backup"))) { failed = true; throw new IOException("Temporary cleanup failure"); }
                    Files.delete(path);
                }
            };
            save(root, target, "committed despite deferred cleanup", cleanupFailure);
            check(Files.readString(target).equals("committed despite deferred cleanup") && Files.exists(sibling(target, ".journal")), "committed target survives cleanup failure");
            RecoverableConfigMove.recoverBeforeLoad(root, target, moves);
            cleanJournal(target);

            Path other = Files.createDirectory(root.resolve("other"));
            Path outside = other.resolve("outside.json");
            Files.writeString(outside, "outside config");
            Files.writeString(other.resolve("outside.json.tmp"), "incoming outside config");
            fails(() -> RecoverableConfigMove.replace(config, other.resolve("outside.json.tmp"), outside, moves), "out-of-game destination rejected");
            check(Files.readString(outside).equals("outside config"), "out-of-game data unchanged");

            check(CompatibilityGate.versionsMatch("Horizon", "1.21.1", "0.6.13+mc1.21.1"), "exact supported platform and version");
            check(!CompatibilityGate.versionsMatch("Linux", "1.21.1", "0.6.13+mc1.21.1"), "ordinary platform skipped");
            check(!CompatibilityGate.versionsMatch("Horizon", "1.21.1", "0.6.14+mc1.21.1"), "other Sodium version skipped");
            check(!CompatibilityGate.versionsMatch("Horizon", "1.21.2", "0.6.13+mc1.21.1"), "other game version skipped");
            check(CompatibilityGate.expectedBytecode(Files.readAllBytes(Path.of(args[0]))), "exact ordinary FileUtil callsite recognized");
            try (var bytes = CompatibilityTests.class.getResourceAsStream("CompatibilityTests.class")) {
                check(bytes != null && !CompatibilityGate.expectedBytecode(bytes.readAllBytes()), "unrelated class is never targeted");
            }
            CompatibilityPlugin plugin = new CompatibilityPlugin();
            plugin.onLoad("mineswitch.compat.mixin");
            check(!plugin.shouldApplyMixin(CompatibilityGate.TARGET, CompatibilityGate.MIXIN), "plugin is a no-op on the host platform");
            System.out.println("COMPAT_TEST_PASS checks=" + checks);
        } finally {
            java.util.List<Path> paths;
            try (var stream = Files.walk(root)) { paths = stream.sorted(Comparator.reverseOrder()).toList(); }
            for (Path path : paths) Files.delete(path);
        }
    }

}
