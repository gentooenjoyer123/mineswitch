package mineswitch.compat.mixin;

import mineswitch.compat.WorldDirectoryNames;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.ModifyArg;

@Mixin(targets = "net.minecraft.class_8100", remap = false)
public abstract class WorldDirectoryMixin {
    @ModifyArg(method = "method_49704(Ljava/lang/String;)Ljava/lang/String;",
               at = @At(value = "INVOKE", target = "Lnet/minecraft/class_4239;method_19773(Ljava/nio/file/Path;Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;", remap = false),
               index = 1, require = 2, allow = 2, remap = false)
    private String mineswitch$worldStorageName(String name) {
        return WorldDirectoryNames.storageName(name);
    }
}
