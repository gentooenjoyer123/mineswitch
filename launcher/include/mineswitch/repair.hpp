#pragma once
#include "http.hpp"
#include "storage.hpp"
namespace mineswitch {
// Does not download files or trust a name alone. Unresolved IDs and files without
// a published digest remain missing. Only the instance's game tree is inspected.
// Instance manifests recover interrupted metadata renames before being read.
std::vector<Download> missingFiles(const Instance&);
Instance verifyManualFiles(Storage&, Instance, ProgressCallback = {},
                           const std::atomic_bool* cancelled = nullptr);
}
