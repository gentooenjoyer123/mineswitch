package mineswitch.compat;

import java.util.List;
import java.util.Set;
import net.fabricmc.loader.api.FabricLoader;
import org.objectweb.asm.tree.ClassNode;
import org.spongepowered.asm.mixin.extensibility.IMixinConfigPlugin;
import org.spongepowered.asm.mixin.extensibility.IMixinInfo;

public final class CompatibilityPlugin implements IMixinConfigPlugin {
    private boolean enabled;
    private boolean announced;
    private boolean worldDirectories;
    @Override public void onLoad(String mixinPackage) {
        enabled = CompatibilityGate.enabled();
        worldDirectories = CompatibilityGate.isHorizon() && FabricLoader.getInstance()
                .getModContainer("minecraft").map(mod -> "1.21.1".equals(
                        mod.getMetadata().getVersion().getFriendlyString())).orElse(false);
    }
    @Override public String getRefMapperConfig() { return null; }
    @Override public boolean shouldApplyMixin(String target, String mixin) {
        if ("mineswitch.compat.mixin.WorldDirectoryMixin".equals(mixin))
            return worldDirectories && "net.minecraft.class_8100".equals(target);
        return enabled && CompatibilityGate.TARGET.equals(target) && CompatibilityGate.MIXIN.equals(mixin);
    }
    @Override public void acceptTargets(Set<String> mine, Set<String> other) {}
    @Override public List<String> getMixins() { return null; }
    @Override public void preApply(String target, ClassNode node, String mixin, IMixinInfo info) {}
    @Override public void postApply(String target, ClassNode node, String mixin, IMixinInfo info) {
        if (worldDirectories && "mineswitch.compat.mixin.WorldDirectoryMixin".equals(mixin)) {
            System.out.println("MINESWITCH_COMPAT world_directories=ascii display_names=preserved");
            return;
        }
        if (!announced && shouldApplyMixin(target, mixin)) {
            announced = true;
            System.out.println("MINESWITCH_COMPAT sodium_config=enabled version=0.6.13+mc1.21.1");
        }
    }
}
