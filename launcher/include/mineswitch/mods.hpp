#pragma once
#include "catalog.hpp"
#include "http.hpp"
#include "storage.hpp"

namespace mineswitch {
struct InstalledMod {
    std::string filename,name,version,id,problem;
    std::uint64_t bytes=0;
    bool enabled=true;
};
class ModManager {
public:
    ModManager(Storage& storage,HttpClient& http):storage_(storage),http_(http) {}
    std::vector<InstalledMod> list(const Instance&);
    void setEnabled(const Instance&,const std::string& filename,bool enabled);
    void install(const Instance&,const CatalogVersion&,ProgressCallback={},
                 const std::atomic_bool* cancelled=nullptr);
    void remove(const Instance&,const std::string& filename);
    // Replaces an installed file with another version, keeping it disabled if it was.
    void update(const Instance&,const std::string& filename,const CatalogVersion& newer,ProgressCallback={},
                const std::atomic_bool* cancelled=nullptr);
    // Installs the version and its missing required dependencies from the same
    // catalog. Returns display names of everything that was installed.
    std::vector<std::string> installWithDependencies(const Instance&,CatalogClient&,const CatalogVersion&,
                                                     ProgressCallback={},const std::atomic_bool* cancelled=nullptr);
private:
    Storage& storage_;
    HttpClient& http_;
};
}
