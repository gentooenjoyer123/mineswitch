#pragma once
#include "model.hpp"
#include <cstddef>
#include <cstdint>
#include <jansson.h>
#include <string>
#include <vector>

namespace mineswitch {

// Shared read/write budget for generated managed-files and missing-files JSON.
inline constexpr std::size_t kInstanceManifestMaxBytes = 64U * 1024U * 1024U;

// Paths are UTF-8, use forward slashes, and cannot contain ambiguous FAT names.
bool validRelativePath(const std::string& path);
std::string joinPath(const std::string& root, const std::string& relative);
void requireSafePath(const std::string& path, bool allow_missing_leaf = false);
bool pathExists(const std::string& path);
void createDirectories(const std::string& path);

// The caller owns a returned descriptor. syncAndCloseFile always closes it,
// including on error. Existing files and symbolic links are never replaced.
int openExclusiveFile(const std::string& path);
void syncAndCloseFile(int fd, const std::string& path);
std::string readTextFile(const std::string& path,
                         std::size_t maximum_bytes = 4 * 1024 * 1024);
// Recovers reserved .tmp/.old journal siblings under the storage lock. Returns
// a caller-owned JSON reference, or nullptr only when no committed file exists.
json_t* readStoredJson(const std::string& path, std::size_t maximum_bytes);

// .tmp and .old siblings are reserved for the recoverable replacement journal.
// This handles interrupted renames; SD power-loss durability is not guaranteed.
void atomicWriteText(const std::string& path, const std::string& text);
void atomicWriteJson(const std::string& path, json_t* value);
// Bounds serialization before opening or replacing the destination file.
void atomicWriteJson(const std::string& path, json_t* value, std::size_t maximum_bytes);
void removeOwnedTree(const std::string& path);
std::string newId();

class Storage {
public:
    explicit Storage(std::string root = "sdmc:/switch/mineswitch");
    const std::string& root() const { return root_; }
    void initialize();
    // Call once at startup before starting import workers. Removes abandoned
    // generated transaction directories; never publishes staged content.
    std::size_t cleanupTransactions();
    Settings loadSettings();
    void saveSettings(const Settings& settings);
    std::vector<Profile> loadProfiles();
    void saveProfiles(const std::vector<Profile>& profiles);
    std::vector<Instance> listInstances();
    void saveInstance(const Instance& instance);
    std::uint64_t availableBytes() const;

private:
    std::string root_;
};
}
