#pragma once
#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace mineswitch {
enum class Provider { Modrinth, CurseForge };
enum class PackKind { Modrinth, CurseForge, MultiMC };
// Catalog project class. Modpacks create instances; the rest install into one.
enum class ContentKind { Modpack, Mod, ResourcePack, ShaderPack };
enum class LoaderKind { Vanilla, Fabric, Quilt, Forge, NeoForge, Unknown };
enum class InstanceState { Ready, NeedsFiles, NeedsPreparation, Unsupported, Broken };

struct Error : std::runtime_error {
    std::string code;
    Error(std::string code_, std::string message) : std::runtime_error(std::move(message)), code(std::move(code_)) {}
};
struct Hashes {
    std::string sha1, sha256, sha512, md5;
    bool empty() const { return sha1.empty() && sha256.empty() && sha512.empty() && md5.empty(); }
};
struct LoaderSpec {
    LoaderKind kind = LoaderKind::Vanilla;
    std::string version;
};
struct GameSpec {
    std::string minecraft_version;
    LoaderSpec loader;
    int java_major = 21;
};
struct Download {
    std::string path; // Normalized path relative to the instance game directory.
    std::string file_name;
    std::vector<std::string> urls;
    std::uint64_t bytes = 0;
    // Independent hard streaming limit, including when bytes is not known.
    std::uint64_t max_bytes = 4ULL * 1024 * 1024 * 1024 - 1;
    Hashes hashes;
    bool optional = false;
    bool manual = false;
    std::string manual_url;
    std::string project_id, file_id;
};
struct PackPlan {
    PackKind kind = PackKind::Modrinth;
    std::string name, version, source_sha256;
    GameSpec game;
    std::vector<Download> downloads;
    std::vector<std::string> override_roots;
    std::vector<std::string> notices;
    std::string packwiz_url; // MultiMC PreLaunchCommand running packwiz-installer.
    std::string icon;        // MultiMC iconKey; <icon>.png beside instance.cfg.
    InstanceState state = InstanceState::NeedsPreparation;
    std::uint64_t expanded_bytes = 0, required_free_bytes = 0;
};
struct CatalogProject {
    Provider provider = Provider::Modrinth;
    std::string id, slug, name, summary, author, icon_url, website;
    std::uint64_t downloads = 0;
};
struct Dependency {
    std::string project_id, version_id; // Either may be empty, never both.
};
struct CatalogVersion {
    Provider provider = Provider::Modrinth;
    std::string id, project_id, name, published;
    std::vector<std::string> minecraft_versions;
    std::vector<LoaderKind> loaders;
    std::vector<Dependency> dependencies; // Required dependencies only.
    Download archive;
};
enum class SearchSort { Relevance, Downloads, Updated };
struct SearchQuery {
    std::string text, minecraft_version;
    std::uint32_t offset = 0, limit = 20;
    ContentKind kind = ContentKind::Modpack;
    LoaderKind loader = LoaderKind::Unknown;
    SearchSort sort = SearchSort::Relevance;
};
struct SearchPage {
    std::vector<CatalogProject> projects;
    std::uint32_t offset = 0, total = 0;
    bool has_more = false;
};
struct Instance {
    std::string id, name, root; // root is an owned absolute SD path.
    GameSpec game;
    InstanceState state = InstanceState::NeedsPreparation;
    std::string reason;
    std::string provider_project, provider_version;
    std::uint64_t installed_bytes = 0;
    // Launcher-only presentation and overrides; absent in older instance.json.
    std::string icon, notes, jvm_args;
    std::int64_t last_played = 0; // Unix seconds of the last launch handoff.
    int heap_mib = 0;             // 0 uses the global setting.
    std::string packwiz_url;      // pack.toml synced before each launch.
};
struct Profile {
    std::string id, username;
};
struct Settings {
    std::string root = "sdmc:/switch/mineswitch";
    std::string profile_id;
    std::string curseforge_key;
    int heap_mib = 1024;
    bool reduced_motion = false;
    bool show_incompatible = false;
    std::string jvm_args; // Extra validated JVM options for every instance.
    int library_sort = 0; // 0 name, 1 last played, 2 Minecraft version.
    bool full_initial_heap = false; // -Xms equal to -Xmx instead of 256 MiB.
    std::string language = "en"; // UI language: "en" or "ru".
};
struct Progress {
    std::string stage, detail;
    std::uint64_t completed = 0, total = 0;
    std::uint32_t files_done = 0, files_total = 0;
};
}
