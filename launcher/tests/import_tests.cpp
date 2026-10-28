#include "mineswitch/catalog.hpp"
#include "mineswitch/import.hpp"
#include <mbedtls/md.h>
#include <minizip/zip.h>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <dirent.h>
#include <functional>
#include <iostream>
#include <map>
#include <stdexcept>
#include <unistd.h>

using namespace mineswitch;
namespace {
int checks = 0;
void check(bool condition, const std::string& label) {
    if (!condition) throw std::runtime_error("FAIL: " + label);
    ++checks;
}
void rejects(const std::string& code, const std::function<void()>& action) {
    try { action(); }
    catch (const Error& error) {
        check(error.code == code, "expected " + code + ", received " + error.code);
        return;
    }
    throw std::runtime_error("FAIL: expected " + code);
}
struct Temporary {
    std::string path;
    Temporary() {
        char pattern[] = "/tmp/mineswitch-import-XXXXXX";
        const char* value = ::mkdtemp(pattern);
        if (!value) throw std::runtime_error("Cannot create test directory");
        path = value;
    }
    ~Temporary() { try { removeOwnedTree(path); } catch (...) {} }
};
std::string digest(const std::string& text, mbedtls_md_type_t type) {
    const auto* info = mbedtls_md_info_from_type(type);
    unsigned char bytes[MBEDTLS_MD_MAX_SIZE] {};
    if (!info || mbedtls_md(info, reinterpret_cast<const unsigned char*>(text.data()), text.size(), bytes))
        throw std::runtime_error("Cannot hash test data");
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    for (std::size_t i = 0; i < mbedtls_md_get_size(info); ++i) {
        result += digits[bytes[i] >> 4]; result += digits[bytes[i] & 15];
    }
    return result;
}
void writeZip(const std::string& path, const std::vector<std::pair<std::string, std::string>>& files) {
    zipFile file = zipOpen64(path.c_str(), APPEND_STATUS_CREATE);
    if (!file) throw std::runtime_error("Cannot create fixture ZIP");
    try {
        for (const auto& entry : files) {
            if (zipOpenNewFileInZip64(file, entry.first.c_str(), nullptr, nullptr, 0, nullptr, 0,
                                     nullptr, Z_DEFLATED, Z_DEFAULT_COMPRESSION, 0) != ZIP_OK)
                throw std::runtime_error("Cannot create fixture ZIP entry");
            if (zipWriteInFileInZip(file, entry.second.data(), static_cast<unsigned>(entry.second.size())) != ZIP_OK ||
                zipCloseFileInZip(file) != ZIP_OK) throw std::runtime_error("Cannot write fixture ZIP entry");
        }
    } catch (...) { zipClose(file, nullptr); throw; }
    if (zipClose(file, nullptr) != ZIP_OK) throw std::runtime_error("Cannot close fixture ZIP");
}
std::size_t childCount(const std::string& path) {
    DIR* directory = ::opendir(path.c_str());
    if (!directory) throw std::runtime_error("Cannot read test directory");
    std::size_t count = 0;
    while (auto* entry = ::readdir(directory))
        if (std::string(entry->d_name) != "." && std::string(entry->d_name) != "..") ++count;
    ::closedir(directory);
    return count;
}
std::string mrFile(const std::string& path, const std::string& body, std::uint64_t bytes, bool optional = false) {
    return "{\"path\":\"" + path + "\",\"fileSize\":" + std::to_string(bytes) +
           ",\"hashes\":{\"sha1\":\"" + digest(body, MBEDTLS_MD_SHA1) +
           "\",\"sha512\":\"" + digest(body, MBEDTLS_MD_SHA512) +
           "\"},\"downloads\":[\"https://cdn.modrinth.com/data/fixture.jar\"]" +
           (optional ? ",\"env\":{\"client\":\"optional\",\"server\":\"optional\"}" : "") + "}";
}
std::string mrPack(const std::string& files = "") {
    return "{\"formatVersion\":1,\"game\":\"minecraft\",\"name\":\"Local fixture\",\"versionId\":\"1\","
           "\"dependencies\":{\"minecraft\":\"1.21.1\",\"fabric-loader\":\"0.16.10\"},\"files\":[" + files + "]}";
}
class FakeHttp final : public HttpClient {
public:
    std::map<std::string, std::string> bodies;
    std::vector<Download> requests;
    std::uint64_t last_written = 0;
    HttpResponse request(const HttpRequest&, const std::atomic_bool*) override {
        throw std::runtime_error("Unexpected network metadata request in local importer test");
    }
    void download(const Download& request, const std::string& path, ProgressCallback progress,
                  const std::atomic_bool* stopped) override {
        requests.push_back(request);
        const auto found = bodies.find(request.path);
        if (found == bodies.end()) throw std::runtime_error("Missing fake download body");
        int fd = openExclusiveFile(path);
        last_written = 0;
        try {
            for (std::size_t offset = 0; offset < found->second.size();) {
                if (stopped && stopped->load()) throw Error("cancelled", "Local transfer cancelled");
                const auto amount = std::min<std::size_t>(64, found->second.size() - offset);
                if (amount > request.max_bytes - last_written)
                    throw Error("download_limit", "Local transfer reached the requested bound");
                const auto written = ::write(fd, found->second.data() + offset, amount);
                if (written != static_cast<ssize_t>(amount)) throw std::runtime_error("Cannot write fake transfer");
                offset += amount; last_written += amount;
                if (progress) progress({"download", request.path, last_written, request.bytes, 0, 0});
            }
            const int closing = fd; fd = -1; syncAndCloseFile(closing, path);
        } catch (...) { if (fd >= 0) ::close(fd); throw; }
    }
};
void unchanged(Storage& storage, std::size_t count) {
    check(storage.listInstances().size() == count, "failed import does not publish an instance");
    check(childCount(joinPath(storage.root(), "transactions")) == 0, "failed import cleans staging");
}
}

int main() {
    try {
        Temporary temporary;
        Storage storage(joinPath(temporary.path, "library"));
        storage.initialize();
        FakeHttp http;
        ArchiveImporter importer(storage, http, parse_pack_manifest);
        const auto mr = joinPath(temporary.path, "normal.mrpack");
        const std::string required = "A local mod fixture", optional = "Optional local mod";
        http.bodies["mods/example.jar"] = required;
        http.bodies["mods/optional.jar"] = optional;
        const auto manifest = mrPack(mrFile("mods/example.jar", required, required.size()) + "," +
                                     mrFile("mods/optional.jar", optional, optional.size(), true));
        writeZip(mr, {{"modrinth.index.json", manifest}, {"overrides/config/options.txt", "shared"},
                     {"client-overrides/config/options.txt", "client"}, {"overrides/saves/World/level.dat", "save"}});
        const auto plan = importer.inspectArchive(mr);
        check(plan.kind == PackKind::Modrinth && plan.downloads.size() == 2, "normal Modrinth inspection");
        check(http.requests.empty(), "inspection performs no transfers");
        bool observed_staging = false;
        const auto imported = importer.importArchive(mr, {}, [&](const Progress& progress) {
            if (progress.stage == "import") {
                check(storage.listInstances().empty(), "staging is invisible before publication");
                observed_staging = true;
            }
        });
        check(observed_staging && imported.state == InstanceState::NeedsPreparation, "Modrinth staging and publication");
        check(readTextFile(joinPath(imported.root, "game/config/options.txt")) == "client", "client override takes precedence");
        check(readTextFile(joinPath(imported.root, "game/mods/example.jar")) == required, "required download copied");
        check(!pathExists(joinPath(imported.root, "game/mods/optional.jar")), "optional file excluded by default");
        check(http.requests.size() == 1 && http.requests.front().max_bytes == required.size(), "known download is bounded exactly");
        check(imported.installed_bytes == required.size() + 6 + 4, "installed bytes count final files once");
        check(storage.listInstances().front().root == imported.root, "published instance root is derived correctly");
        check(pathExists(joinPath(imported.root, "import-complete.json")), "publication includes completion marker");
        check(childCount(joinPath(storage.root(), "transactions")) == 0, "successful transaction cleaned");

        ImportOptions include_optional; include_optional.include_optional = true;
        const auto included = importer.importArchive(mr, include_optional, [](const Progress& p) {
            if (p.stage == "complete") throw std::runtime_error("Notification consumer closed");
        });
        check(readTextFile(joinPath(included.root, "game/mods/optional.jar")) == optional, "optional file selection");
        check(storage.listInstances().size() == 2, "completion callback cannot fail a committed import");

        const auto cf = joinPath(temporary.path, "normal-cf.zip");
        writeZip(cf, {{"manifest.json", "{\"manifestType\":\"minecraftModpack\",\"manifestVersion\":1,\"name\":\"CF local\","
                   "\"version\":\"1\",\"minecraft\":{\"version\":\"1.21.1\",\"modLoaders\":[]},\"files\":["
                   "{\"projectID\":123,\"fileID\":456,\"required\":true}],\"overrides\":\"overrides\"}"},
                   {"overrides/options.txt", "CF settings"}});
        const auto missing = importer.importArchive(cf);
        check(missing.state == InstanceState::NeedsFiles, "unresolved CurseForge file stays missing");
        check(readTextFile(joinPath(missing.root, "missing-files.json")).find("456") != std::string::npos,
              "missing CurseForge identifier persisted");
        const std::string cf_body = "Resolved local CF mod";
        http.bodies["mods/cf.jar"] = cf_body;
        bool resolved = false;
        ArchiveImporter cf_importer(storage, http, parse_pack_manifest, [&](PackPlan& p, const std::atomic_bool*) {
            check(p.kind == PackKind::CurseForge, "resolver gets CurseForge plan");
            resolved = true;
            auto& file = p.downloads.at(0);
            file.path = "mods/cf.jar"; file.bytes = cf_body.size();
            file.urls = {"https://edge.forgecdn.net/files/fixture.jar"};
            file.hashes.sha256 = digest(cf_body, MBEDTLS_MD_SHA256);
        });
        const auto resolved_instance = cf_importer.importArchive(cf);
        check(resolved && resolved_instance.state == InstanceState::NeedsPreparation, "resolved CurseForge import");
        check(readTextFile(joinPath(resolved_instance.root, "game/mods/cf.jar")) == cf_body, "resolved file installed");

        const auto mmc = joinPath(temporary.path, "normal-mmc.zip");
        writeZip(mmc, {{"Friendly/instance.cfg", "name=Friendly local instance\nJavaPath=/usr/bin/java\n"},
                     {"Friendly/mmc-pack.json", "{\"formatVersion\":1,\"components\":[{\"uid\":\"net.minecraft\",\"version\":\"1.21.1\"}]}"},
                     {"Friendly/.minecraft/options.txt", "MultiMC settings"}, {"Friendly/notes.txt", "export note"}});
        const auto mmc_instance = importer.importArchive(mmc);
        check(mmc_instance.name == "Friendly local instance", "MultiMC name from instance.cfg");
        check(readTextFile(joinPath(mmc_instance.root, "game/options.txt")) == "MultiMC settings", "MultiMC game directory imported");
        check(!pathExists(joinPath(mmc_instance.root, "game/notes.txt")), "MultiMC export notes are not game files");

        const auto count = storage.listInstances().size();
        std::atomic_bool cancelled {false};
        rejects("cancelled", [&] {
            importer.importArchive(mr, {}, [&](const Progress& p) {
                if (p.stage == "import") cancelled = true;
            }, &cancelled);
        });
        unchanged(storage, count);
        cancelled = false;
        rejects("cancelled", [&] {
            importer.importArchive(mr, {}, [&](const Progress& p) {
                if (p.stage == "download") cancelled = true;
            }, &cancelled);
        });
        unchanged(storage, count);

        // Ordinary files with deliberately small caller budgets exercise the
        // streaming contract without special ZIP headers or external servers.
        const std::string transfer_body(1500, 'x');
        const auto bounded_zip = joinPath(temporary.path, "bounded.mrpack");
        const auto bounded_manifest = mrPack(mrFile("a-download.bin", transfer_body, 0));
        const std::string archived_body(500, 'z');
        writeZip(bounded_zip, {{"modrinth.index.json", bounded_manifest}, {"overrides/z-options.txt", archived_body}});
        http.bodies["a-download.bin"] = transfer_body;
        cancelled = false;
        rejects("cancelled", [&] {
            importer.importArchive(bounded_zip, {}, [&](const Progress& p) {
                if (p.stage == "download") cancelled = true;
            }, &cancelled);
        });
        check(http.last_written == 64, "cancellation stops a transfer between chunks");
        unchanged(storage, count);
        ImportLimits limits;
        limits.expanded_bytes = 1800;
        ArchiveImporter aggregate(storage, http, parse_pack_manifest, {}, limits);
        rejects("expanded_limit", [&] { aggregate.importArchive(bounded_zip); });
        check(http.requests.back().max_bytes == 1800, "unknown transfer gets remaining aggregate bound");
        unchanged(storage, count);

        const auto extraction_first = joinPath(temporary.path, "extraction-first.mrpack");
        writeZip(extraction_first, {{"modrinth.index.json", mrPack(mrFile("z-download.bin", transfer_body, 0))},
                                    {"overrides/a-options.txt", archived_body}});
        http.bodies["z-download.bin"] = transfer_body;
        rejects("download_limit", [&] { aggregate.importArchive(extraction_first); });
        check(http.requests.back().max_bytes == 1300 && http.last_written <= 1300,
              "extracted bytes reduce the next transfer's streaming budget");
        unchanged(storage, count);

        limits.expanded_bytes = 8192;
        limits.file_bytes = 1024;
        ArchiveImporter per_file(storage, http, parse_pack_manifest, {}, limits);
        rejects("download_limit", [&] { per_file.importArchive(bounded_zip); });
        check(http.last_written <= 1024 && http.requests.back().max_bytes == 1024, "transfer stops before its file budget");
        unchanged(storage, count);

        limits.file_bytes = 8192;
        limits.expanded_bytes = 1200;
        ArchiveImporter during_transfer(storage, http, parse_pack_manifest, {}, limits);
        rejects("download_limit", [&] { during_transfer.importArchive(bounded_zip); });
        check(http.last_written <= 1200, "aggregate transfer budget is enforced while writing");
        unchanged(storage, count);

        const auto name_zip = joinPath(temporary.path, "filename-limit.mrpack");
        writeZip(name_zip, {{"modrinth.index.json", mrPack()}, {"overrides/options.txt", "ordinary text"}});
        limits = ImportLimits{}; limits.path_bytes = 20;
        ArchiveImporter short_names(storage, http, parse_pack_manifest, {}, limits);
        rejects("path_limit", [&] { short_names.inspectArchive(name_zip); });
        check(storage.listInstances().size() == count, "inspection bound never changes storage");
        std::cout << "Importer tests passed: " << checks << " checks\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
