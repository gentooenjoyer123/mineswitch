package mineswitch.compat;

import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.LinkOption;
import net.fabricmc.loader.api.FabricLoader;
import net.fabricmc.loader.api.entrypoint.PreLaunchEntrypoint;

/** Recover before Sodium can interpret a briefly absent config as defaults. */
public final class CompatibilityPreLaunch implements PreLaunchEntrypoint {
    @Override public void onPreLaunch() {
        if (!CompatibilityGate.enabled()) return;
        var loader = FabricLoader.getInstance();
        var config = loader.getConfigDir();
        if (!Files.exists(config, LinkOption.NOFOLLOW_LINKS)) return;
        try {
            RecoverableConfigMove.recoverBeforeLoad(loader.getGameDir(), config.resolve("sodium-options.json"));
        } catch (IOException unresolved) {
            // Unknown recovery files are preserved. The save redirect also
            // refuses them, rather than letting stale defaults replace data.
            System.err.println("MineSwitch compatibility: Sodium configuration recovery retained its files: " + unresolved.getMessage());
        }
    }
}
