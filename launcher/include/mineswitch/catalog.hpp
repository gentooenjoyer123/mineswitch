#pragma once
#include "http.hpp"
#include "model.hpp"
#include <atomic>
#include <map>
#include <string>
#include <vector>

namespace mineswitch {

// Pure metadata parsing. The importer owns ZIP traversal, overrides, optional
// file policy, downloads, hash verification and transactional filesystem writes.
// CurseForge downloads initially contain IDs only; resolve_pack fills them.
PackPlan parse_pack_manifest(PackKind kind, const std::string& manifest_json,
                             const std::string& instance_cfg = {});
// Pack URL passed to packwiz-installer in a MultiMC/Prism PreLaunchCommand.
// Empty when the command does not run packwiz-installer.
std::string packwizUrlFromCommand(const std::string& command);

// Digests of an installed file used to recognize it in the catalogs.
struct LocalFile {
    std::string sha1;
    std::uint32_t fingerprint = 0; // CurseForge MurmurHash2; 0 when unknown.
};

class CatalogClient {
public:
    CatalogClient(HttpClient& http, const Settings& settings);
    SearchPage search(Provider provider, const SearchQuery& query,
                      const std::atomic_bool* cancelled = nullptr);
    CatalogProject project(Provider provider, const std::string& project_id,
                           const std::atomic_bool* cancelled = nullptr,
                           ContentKind kind = ContentKind::Modpack);
    std::vector<CatalogVersion> versions(Provider provider, const std::string& project_id,
                                         const std::string& minecraft_version = {},
                                         const std::atomic_bool* cancelled = nullptr,
                                         ContentKind kind = ContentKind::Modpack,
                                         LoaderKind loader = LoaderKind::Unknown);
    CatalogVersion version(Provider provider, const std::string& project_id,
                           const std::string& version_id,
                           const std::atomic_bool* cancelled = nullptr,
                           ContentKind kind = ContentKind::Modpack);
    // Recognizes installed files, Modrinth by SHA-1 first, then CurseForge by
    // fingerprint. Result keys match the input; unknown files are absent.
    // CurseForge entries carry provider, project_id and id only.
    std::map<std::string, CatalogVersion> identify(const std::map<std::string, LocalFile>& files, ContentKind kind,
                                                   const std::atomic_bool* cancelled = nullptr);
    // Newer compatible versions for identified files, keyed like identify().
    std::map<std::string, CatalogVersion> updates(const std::map<std::string, CatalogVersion>& identified,
                                                  const std::map<std::string, LocalFile>& files, ContentKind kind,
                                                  const std::string& minecraft_version, LoaderKind loader,
                                                  const std::atomic_bool* cancelled = nullptr);
    void resolve_pack(PackPlan& plan, const std::atomic_bool* cancelled = nullptr);
    // Automatic public-key resolution is available when no personal key is set.
    bool curseforge_configured() const noexcept { return true; }

private:
    HttpClient& http_;
    std::string curseforge_key_;
    HttpResponse metadata(Provider provider, const std::string& path,
                          const std::string& body = {},
                          const std::atomic_bool* cancelled = nullptr);
    bool modrinth_alternative(Download& file, const std::atomic_bool* cancelled);
};
}
