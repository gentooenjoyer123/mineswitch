#pragma once
#include "catalog.hpp"
#include "http.hpp"
#include "storage.hpp"
#include <atomic>
#include <string>
#include <vector>

namespace mineswitch {
// Everything here operates only below the owned instance root returned by
// instanceRoot(); callers pass names from the corresponding list function.
std::string instanceRoot(const Storage&, const Instance&);
const char* contentFolder(ContentKind); // mods, resourcepacks or shaderpacks
LocalFile digestFile(const std::string& path, const std::atomic_bool* cancelled = nullptr);

struct PackEntry {
    std::string filename, name, description, problem;
    std::uint64_t bytes = 0;
    bool enabled = true, directory = false;
};
std::vector<PackEntry> listPacks(const Storage&, const Instance&, ContentKind);
void setPackEnabled(const Storage&, const Instance&, ContentKind, const std::string& filename, bool enabled);
// Removes one mod, resource pack or shader pack file/folder.
void removeContent(const Storage&, const Instance&, ContentKind, const std::string& filename);
// Downloads a verified resource or shader pack ZIP into its folder.
void installPack(Storage&, HttpClient&, const Instance&, ContentKind, const CatalogVersion&,
                 ProgressCallback = {}, const std::atomic_bool* cancelled = nullptr);

struct WorldInfo {
    std::string folder, name, version, problem;
    std::int64_t last_played = 0; // Unix milliseconds, as stored in level.dat.
    int game_type = 0;
    bool hardcore = false, cheats = false, icon = false;
};
std::vector<WorldInfo> listWorlds(const Storage&, const Instance&);
void deleteWorld(const Storage&, const Instance&, const std::string& folder);
void renameWorld(const Storage&, const Instance&, const std::string& folder, const std::string& name);
std::string copyWorld(const Storage&, const Instance&, const std::string& folder,
                      ProgressCallback = {}, const std::atomic_bool* cancelled = nullptr);
// ZIP below game/backups, like Minecraft's own world backups. Returns its path.
std::string backupWorld(const Storage&, const Instance&, const std::string& folder,
                        ProgressCallback = {}, const std::atomic_bool* cancelled = nullptr);

struct ServerEntry {
    std::string name, address;
    int original = -1; // Index in servers.dat; keeps fields this launcher ignores.
};
std::vector<ServerEntry> listServers(const Storage&, const Instance&);
void saveServers(const Storage&, const Instance&, const std::vector<ServerEntry>&);

struct MediaFile {
    std::string name, path;
    std::uint64_t bytes = 0;
    std::int64_t modified = 0;
};
std::vector<MediaFile> listScreenshots(const Storage&, const Instance&);
void deleteScreenshot(const Storage&, const Instance&, const std::string& name);
// latest.log, archived logs and crash reports, newest first.
std::vector<MediaFile> listLogs(const Storage&, const Instance&);
// Tail of a log as lines; .gz archives are decompressed.
std::vector<std::string> readLog(const Storage&, const Instance&, const std::string& path,
                                 std::size_t maximum_bytes = 2 * 1024 * 1024);

// Whole-instance operations. Copies omit generated launch plans.
Instance duplicateInstance(Storage&, const Instance&, const std::string& name,
                           ProgressCallback = {}, const std::atomic_bool* cancelled = nullptr);
void deleteInstance(Storage&, const Instance&);
// MultiMC/PolyMC-compatible ZIP in <root>/exports. Returns its path.
std::string exportInstance(Storage&, const Instance&, ProgressCallback = {},
                           const std::atomic_bool* cancelled = nullptr);
} // namespace mineswitch
