package mineswitch.launcher;

import java.io.IOException;
import java.io.Reader;
import java.lang.reflect.InvocationTargetException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.FileAlreadyExistsException;
import java.nio.file.Path;
import java.nio.file.StandardOpenOption;
import java.util.ArrayList;
import java.util.List;
import java.util.Properties;
import java.util.UUID;

/** Launches the original Minecraft entry point with a local development profile. */
public final class ClientEntry {
    private static String required(Properties profile, String name) {
        String value = profile.getProperty(name);
        if (value == null || value.isBlank()) throw new IllegalArgumentException("Missing profile field: " + name);
        return value;
    }

    private static void defaults(Path game) throws IOException {
        Files.createDirectories(game);
        Path options = game.resolve("options.txt");
        if (Files.notExists(options)) {
            // Initial resource limits only. Existing user settings are preserved.
            String text = "renderDistance:4\nsimulationDistance:5\nmaxFps:30\n"
                    + "graphicsMode:0\nmipmapLevels:0\ncloudStatus:0\n"
                    + "fullscreen:false\npauseOnLostFocus:false\n";
            Files.writeString(options, text, StandardCharsets.UTF_8,
                    StandardOpenOption.CREATE_NEW, StandardOpenOption.WRITE);
        } else {
            // Correct the invalid value written by the first MVP launcher.
            // Minecraft 1.21.1 accepts simulation distances from 5 through 33.
            String text = Files.readString(options, StandardCharsets.UTF_8);
            String corrected = text.replaceAll("(?m)^simulationDistance:4(?=\\r?$)", "simulationDistance:5");
            if (!corrected.equals(text)) {
                Files.writeString(options, corrected, StandardCharsets.UTF_8,
                        StandardOpenOption.TRUNCATE_EXISTING, StandardOpenOption.WRITE);
            }
        }
    }

    public static void main(String[] extraArguments) throws Throwable {
        Path profileFile = Path.of(System.getProperty("mineswitch.profile",
                "/switch/mineswitch/profile.properties"));
        if (Files.notExists(profileFile)) {
            Files.createDirectories(profileFile.toAbsolutePath().getParent());
            try {
                Files.copy(Path.of("/switch/mineswitch/runtime/mineswitch/offline.properties"), profileFile);
            } catch (FileAlreadyExistsException alreadyCreated) {
                // Preserve an existing or concurrently created owner profile.
            }
        }
        Properties profile = new Properties();
        try (Reader input = Files.newBufferedReader(profileFile, StandardCharsets.UTF_8)) {
            profile.load(input);
        }
        if (!"offline".equals(required(profile, "mode"))) {
            throw new IllegalArgumentException("This MVP launcher currently supports the local profile only");
        }
        String username = required(profile, "username");
        if (!username.matches("[A-Za-z0-9_]{1,16}")) throw new IllegalArgumentException("Invalid local username");
        String uuid = UUID.nameUUIDFromBytes(("OfflinePlayer:" + username)
                .getBytes(StandardCharsets.UTF_8)).toString();
        Path game = Path.of(required(profile, "gameDir"));
        defaults(game);
        System.setProperty("org.lwjgl.system.allocator", "system");
        System.setProperty("io.netty.transport.noNative", "true");
        System.setProperty("java.awt.headless", "true");

        List<String> arguments = new ArrayList<>(List.of(
                "--username", username,
                "--uuid", uuid,
                "--accessToken", "0",
                "--userType", "legacy",
                "--version", required(profile, "version"),
                "--versionType", "release",
                "--gameDir", game.toString(),
                "--assetsDir", required(profile, "assetsDir"),
                "--assetIndex", required(profile, "assetIndex"),
                "--width", required(profile, "width"),
                "--height", required(profile, "height")));
        String server = profile.getProperty("quickPlayServer", "").trim();
        if (!server.isEmpty()) {
            arguments.add("--quickPlayMultiplayer");
            arguments.add(server);
        }
        arguments.addAll(List.of(extraArguments));
        System.out.println("MINESWITCH_MINECRAFT_BOOTSTRAP version=" + required(profile, "version")
                + " mode=offline username=" + username + " heap_max=" + Runtime.getRuntime().maxMemory());
        long started = System.nanoTime();
        try (var sampler = Boolean.getBoolean("mineswitch.progress")
                ? ProgressSampler.start(Thread.currentThread()) : null;
             var metrics = Boolean.getBoolean("mineswitch.metrics")
                ? GameMetrics.start(Thread.currentThread()) : null) {
            System.out.println("MINESWITCH_MINECRAFT_MAIN_LOAD_BEGIN");
            Class<?> entry = Class.forName("net.minecraft.client.main.Main");
            System.out.println("MINESWITCH_MINECRAFT_MAIN_CLASS_READY elapsed_ms="
                    + (System.nanoTime() - started) / 1_000_000);
            var main = entry.getMethod("main", String[].class);
            System.out.println("MINESWITCH_MINECRAFT_MAIN_INVOKE elapsed_ms="
                    + (System.nanoTime() - started) / 1_000_000);
            main.invoke(null, (Object)arguments.toArray(String[]::new));
            System.out.println("MINESWITCH_MINECRAFT_MAIN_RETURN");
        } catch (InvocationTargetException error) {
            Throwable cause = error.getCause();
            System.err.println("MINESWITCH_MINECRAFT_MAIN_FAILED " + cause);
            throw cause;
        }
    }
}
