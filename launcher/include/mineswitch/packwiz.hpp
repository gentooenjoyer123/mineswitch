#pragma once
#include "http.hpp"
#include "storage.hpp"
#include <atomic>
#include <map>
#include <string>
#include <vector>

namespace mineswitch {
// The TOML subset packwiz writes: tables, arrays of tables, strings, integers
// and booleans. Scalars keep their text; arrays are newline-joined scalars.
struct TomlDocument {
    std::map<std::string, std::string> values; // "table.key" -> value
    std::map<std::string, std::vector<std::map<std::string, std::string>>> arrays; // [[name]]
    std::string get(const std::string& key, const std::string& fallback = {}) const;
};
TomlDocument parseToml(const std::string& text);

struct PackwizSync {
    std::string version;
    std::uint32_t downloaded = 0, removed = 0, kept = 0, skipped = 0;
    bool changed = false;     // false when the recorded index was already applied
    bool game_changed = false; // instance.game was updated from [versions]
};
// Brings <root>/game to the pack's index the way packwiz-installer does before
// each launch. Without force an already applied index hash returns at once.
// Files the pack no longer lists are removed only if an earlier sync placed them.
PackwizSync syncPackwiz(Storage&, HttpClient&, const Settings&, Instance&, bool force,
                        ProgressCallback = {}, const std::atomic_bool* cancelled = nullptr);
// True after one complete sync of the instance's current pack URL.
bool packwizSynced(const Instance&);
}
