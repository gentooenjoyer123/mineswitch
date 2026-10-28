package mineswitch.compat;

import java.io.InputStream;
import java.nio.file.Files;
import net.fabricmc.loader.api.FabricLoader;
import org.objectweb.asm.ClassReader;
import org.objectweb.asm.Opcodes;
import org.objectweb.asm.tree.ClassNode;
import org.objectweb.asm.tree.MethodInsnNode;

public final class CompatibilityGate {
    public static final String TARGET = "net.caffeinemc.mods.sodium.client.util.FileUtil";
    public static final String METHOD = "writeTextRobustly";
    public static final String METHOD_DESCRIPTOR = "(Ljava/lang/String;Ljava/nio/file/Path;)V";
    public static final String MOVE_DESCRIPTOR = "(Ljava/nio/file/Path;Ljava/nio/file/Path;[Ljava/nio/file/CopyOption;)Ljava/nio/file/Path;";
    public static final String MIXIN = "mineswitch.compat.mixin.SodiumFileUtilMixin";

    private CompatibilityGate() {}

    public static boolean isHorizon() { return "Horizon".equalsIgnoreCase(System.getProperty("os.name", "")); }

    static boolean versionsMatch(String os, String minecraft, String sodium) {
        return "Horizon".equalsIgnoreCase(os) && "1.21.1".equals(minecraft) && "0.6.13+mc1.21.1".equals(sodium);
    }

    public static boolean enabled() {
        if (!isHorizon()) return false;
        try {
            var loader = FabricLoader.getInstance();
            var sodium = loader.getModContainer("sodium");
            var minecraft = loader.getModContainer("minecraft");
            if (sodium.isEmpty() || minecraft.isEmpty() || !versionsMatch(System.getProperty("os.name", ""),
                    minecraft.get().getMetadata().getVersion().getFriendlyString(),
                    sodium.get().getMetadata().getVersion().getFriendlyString())) return false;
            var path = sodium.get().findPath(TARGET.replace('.', '/') + ".class");
            if (path.isEmpty()) return false;
            try (InputStream input = Files.newInputStream(path.get())) {
                byte[] bytes = input.readNBytes(65537);
                return bytes.length <= 65536 && expectedBytecode(bytes);
            }
        } catch (Exception | LinkageError unavailable) {
            System.err.println("MineSwitch compatibility: Sodium target could not be verified; adapter skipped");
            return false;
        }
    }

    static boolean expectedBytecode(byte[] bytes) {
        try {
            ClassNode node = new ClassNode();
            new ClassReader(bytes).accept(node, ClassReader.SKIP_DEBUG | ClassReader.SKIP_FRAMES);
            if (!TARGET.replace('.', '/').equals(node.name)) return false;
            int methods = 0, moves = 0;
            for (var method : node.methods) {
                if (!METHOD.equals(method.name) || !METHOD_DESCRIPTOR.equals(method.desc)) continue;
                if ((method.access & (Opcodes.ACC_PUBLIC | Opcodes.ACC_STATIC)) != (Opcodes.ACC_PUBLIC | Opcodes.ACC_STATIC)) return false;
                ++methods;
                for (var instruction = method.instructions.getFirst(); instruction != null; instruction = instruction.getNext()) {
                    if (instruction instanceof MethodInsnNode call && call.getOpcode() == Opcodes.INVOKESTATIC &&
                        "java/nio/file/Files".equals(call.owner) && "move".equals(call.name) && MOVE_DESCRIPTOR.equals(call.desc)) ++moves;
                }
            }
            return methods == 1 && moves == 1;
        } catch (RuntimeException invalidClass) { return false; }
    }

}
