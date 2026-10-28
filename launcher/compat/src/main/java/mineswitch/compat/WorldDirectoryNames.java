package mineswitch.compat;

/** Only the storage folder changes; Minecraft retains the user's world title. */
public final class WorldDirectoryNames {
    private WorldDirectoryNames() {}

    public static String storageName(String title) {
        // Horizon SD enumeration omits these names on the tested console.
        // Vanilla findAvailableName still allocates World, World (1), etc.,
        // including its exclusive directory-create collision checks.
        return title.chars().anyMatch(c -> c >= 128) ? "World" : title;
    }
}
