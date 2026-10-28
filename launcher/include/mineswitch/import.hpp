#pragma once
#include "http.hpp"
#include "storage.hpp"
#include <functional>

namespace mineswitch {
struct ImportLimits {
    std::uint64_t archive_bytes = 4ULL * 1024 * 1024 * 1024 - 1;
    std::uint64_t expanded_bytes = 8ULL * 1024 * 1024 * 1024;
    std::uint64_t file_bytes = 4ULL * 1024 * 1024 * 1024 - 1;
    std::uint64_t metadata_bytes = 8ULL * 1024 * 1024;
    std::uint64_t free_margin_bytes = 32ULL * 1024 * 1024;
    std::uint32_t entries = 65536;
    std::uint32_t path_bytes = 512;
};
struct ImportOptions {
    std::string name;
    bool include_optional = false;
    bool download_missing = true;
};
using ManifestParser = std::function<PackPlan(PackKind, const std::string&, const std::string&)>;
using PlanResolver = std::function<void(PackPlan&, const std::atomic_bool*)>;

// Manifest parsing is pure. Resolution may access the two supported providers;
// only importArchive invokes it, after the complete local ZIP has been checked.
class ArchiveImporter {
public:
    ArchiveImporter(Storage&, HttpClient&, ManifestParser, PlanResolver = {}, ImportLimits = {});
    PackPlan inspectArchive(const std::string&, const std::atomic_bool* cancelled = nullptr);
    Instance importArchive(const std::string&, const ImportOptions& = {},
                           ProgressCallback = {}, const std::atomic_bool* cancelled = nullptr);
private:
    Storage& storage_;
    HttpClient& http_;
    ManifestParser parser_;
    PlanResolver resolver_;
    ImportLimits limits_;
};

// Streaming verification is also useful to preparation code. Always computes
// SHA-256; supplied provider digests are all checked, never silently ignored.
std::string verifyFile(const std::string& path, const Hashes& expected = {},
                       std::uint64_t max_bytes = UINT64_MAX,
                       const std::atomic_bool* cancelled = nullptr);
}
