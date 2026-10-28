/* Build-time helper: reads target descriptors without executing target classes. */
import java.lang.module.ModuleDescriptor;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayDeque;
import java.util.Arrays;
import java.util.TreeSet;
class ModuleClosure {
    public static void main(String[] args) throws Exception {
        Path modules = Path.of(args[0]);
        var selected = new TreeSet<String>();
        var pending = new ArrayDeque<String>(Arrays.asList(args[1].split(",")));
        while (!pending.isEmpty()) {
            String name = pending.removeFirst();
            if (!selected.add(name)) continue;
            try (var input = Files.newInputStream(modules.resolve(name).resolve("module-info.class"))) {
                var descriptor = ModuleDescriptor.read(input);
                if (!descriptor.name().equals(name)) throw new IllegalStateException("Unexpected module " + descriptor.name());
                for (var dependency : descriptor.requires()) {
                    if (!dependency.modifiers().contains(ModuleDescriptor.Requires.Modifier.STATIC))
                        pending.addLast(dependency.name());
                }
            }
        }
        System.out.println(String.join(",", selected));
    }
}
