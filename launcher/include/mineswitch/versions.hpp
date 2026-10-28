#pragma once
#include "http.hpp"
#include "storage.hpp"
#include <atomic>
#include <string>
#include <vector>

namespace mineswitch {
struct GameVersion {
    std::string id, type, released; // type: release, snapshot, old_beta, old_alpha
    int java_major = 8;               // Estimated from the release date; preparation checks the real value.
};
struct LoaderVersion {
    std::string version;
    bool stable = true;
};
// Official Mojang manifest, newest first. Falls back to the cached copy offline.
std::vector<GameVersion> gameVersions(Storage&, HttpClient&, const std::atomic_bool* cancelled = nullptr);
// Loader versions for one Minecraft version, newest first. Forge versions omit
// the Minecraft prefix, matching the instance loader.version format.
std::vector<LoaderVersion> loaderVersions(HttpClient&, LoaderKind, const std::string& minecraft_version,
                                          const std::atomic_bool* cancelled = nullptr);
int estimatedJava(const std::string& id, const std::string& released);
} // namespace mineswitch
