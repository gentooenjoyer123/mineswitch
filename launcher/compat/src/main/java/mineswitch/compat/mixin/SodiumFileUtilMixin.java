package mineswitch.compat.mixin;

import java.io.IOException;
import java.nio.file.CopyOption;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardCopyOption;
import mineswitch.compat.CompatibilityGate;
import mineswitch.compat.RecoverableConfigMove;
import net.fabricmc.loader.api.FabricLoader;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Pseudo;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Redirect;

@Pseudo
@Mixin(targets = CompatibilityGate.TARGET, remap = false)
public abstract class SodiumFileUtilMixin {
    @Redirect(method = "writeTextRobustly(Ljava/lang/String;Ljava/nio/file/Path;)V",
              at = @At(value = "INVOKE", target = "Ljava/nio/file/Files;move(Ljava/nio/file/Path;Ljava/nio/file/Path;[Ljava/nio/file/CopyOption;)Ljava/nio/file/Path;", remap = false),
              require = 1, allow = 1, remap = false)
    private static Path mineswitch$recoverableConfigMove(Path source, Path target, CopyOption[] options) throws IOException {
        boolean atomic = false, replacing = false;
        for (CopyOption option : options) {
            if (option == StandardCopyOption.ATOMIC_MOVE) atomic = true;
            else if (option == StandardCopyOption.REPLACE_EXISTING) replacing = true;
        }
        if (!CompatibilityGate.isHorizon() || options.length != 2 || !atomic || !replacing)
            return Files.move(source, target, options);
        return RecoverableConfigMove.replace(FabricLoader.getInstance().getGameDir(), source, target);
    }
}
