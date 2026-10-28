#include "mineswitch/import.hpp"
#include <jansson.h>
#include <minizip/unzip.h>
#include <mbedtls/md.h>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <map>
#include <memory>
#include <set>
#include <sys/stat.h>
#include <unistd.h>

namespace mineswitch {
namespace {
constexpr std::size_t chunk_size = 128 * 1024;
void cancel(const std::atomic_bool* flag) {
    if (flag && flag->load(std::memory_order_relaxed)) throw Error("cancelled", "Import cancelled");
}
std::uint64_t sum(std::uint64_t a, std::uint64_t b) {
    if (b > UINT64_MAX - a) throw Error("size_overflow", "Archive size overflow");
    return a + b;
}
void ioFailure(const std::string& action) { throw Error("io", action + ": " + std::strerror(errno)); }
struct Fd {
    int value = -1;
    explicit Fd(int v) : value(v) {}
    ~Fd() { if (value >= 0) ::close(value); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
};
using Json = std::unique_ptr<json_t, decltype(&json_decref)>;
std::string hex(const unsigned char* bytes, std::size_t count) {
    static const char digits[] = "0123456789abcdef";
    std::string out(count * 2, '0');
    for (std::size_t i = 0; i < count; ++i) { out[2*i] = digits[bytes[i] >> 4]; out[2*i+1] = digits[bytes[i] & 15]; }
    return out;
}
std::string lowerAscii(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return s;
}
void rememberSpelling(std::map<std::string, std::string>& names, const std::string& path) {
    std::size_t end = 0;
    do {
        end = path.find('/', end);
        const std::string part = path.substr(0, end);
        const auto found = names.emplace(lowerAscii(part), part);
        if (!found.second && found.first->second != part)
            throw Error("duplicate_path", "Pack contains case-aliased directory or file names");
        if (end == std::string::npos) break;
        ++end;
    } while (end < path.size());
}
struct Digest {
    mbedtls_md_context_t context;
    explicit Digest(mbedtls_md_type_t kind) {
        mbedtls_md_init(&context);
        const auto* info = mbedtls_md_info_from_type(kind);
        if (!info || mbedtls_md_setup(&context, info, 0) || mbedtls_md_starts(&context)) {
            mbedtls_md_free(&context);
            throw Error("hash", "Cannot initialize digest");
        }
    }
    ~Digest() { mbedtls_md_free(&context); }
    void update(const void* data, std::size_t bytes) {
        if (mbedtls_md_update(&context, static_cast<const unsigned char*>(data), bytes))
            throw Error("hash", "Cannot update digest");
    }
    std::string finish() {
        unsigned char out[MBEDTLS_MD_MAX_SIZE];
        if (mbedtls_md_finish(&context, out)) throw Error("hash", "Cannot finish digest");
        return hex(out, mbedtls_md_get_size(context.md_info));
    }
};
struct HashCheck {
    Digest sha256{MBEDTLS_MD_SHA256};
    std::vector<std::pair<std::string, std::unique_ptr<Digest>>> other;
    std::string wanted256;
    explicit HashCheck(const Hashes& h) : wanted256(h.sha256) {
        add(h.sha1, MBEDTLS_MD_SHA1, 40); add(h.sha512, MBEDTLS_MD_SHA512, 128); add(h.md5, MBEDTLS_MD_MD5, 32);
        validate(wanted256, 64);
    }
    static void validate(const std::string& value, std::size_t length) {
        if (value.empty()) return;
        if (value.size() != length || value.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos)
            throw Error("hash_format", "Invalid expected digest");
    }
    void add(const std::string& value, mbedtls_md_type_t kind, std::size_t length) {
        validate(value, length);
        if (!value.empty()) other.emplace_back(lowerAscii(value), std::make_unique<Digest>(kind));
    }
    void update(const void* data, std::size_t size) {
        sha256.update(data, size); for (auto& item : other) item.second->update(data, size);
    }
    std::string finish() {
        auto actual = sha256.finish();
        if (!wanted256.empty() && actual != lowerAscii(wanted256)) throw Error("hash_mismatch", "SHA-256 mismatch");
        for (auto& item : other) if (item.first != item.second->finish()) throw Error("hash_mismatch", "Provider digest mismatch");
        return actual;
    }
};
std::uint64_t regularSize(const std::string& path) {
    struct stat st{};
    if (::lstat(path.c_str(), &st)) ioFailure("Cannot inspect file");
    if (!S_ISREG(st.st_mode) || st.st_size < 0) throw Error("file_type", "Expected a regular file");
    return static_cast<std::uint64_t>(st.st_size);
}
void writeAll(int fd, const void* data, std::size_t bytes) {
    auto* p = static_cast<const char*>(data);
    while (bytes) {
        ssize_t wrote = ::write(fd, p, bytes);
        if (wrote < 0 && errno == EINTR) continue;
        if (wrote <= 0) ioFailure("Cannot write imported file");
        p += wrote; bytes -= static_cast<std::size_t>(wrote);
    }
}
struct Entry {
    std::string name;
    std::uint64_t bytes;
    bool directory;
    unz64_file_pos position;
};
struct Zip {
    unzFile handle = nullptr;
    std::vector<Entry> entries;
    const ImportLimits& limits;
    Zip(const std::string& path, const ImportLimits& l, const std::atomic_bool* stopped) : limits(l) {
        requireSafePath(path);
        if (regularSize(path) > l.archive_bytes) throw Error("archive_limit", "Archive is too large");
        handle = unzOpen64(path.c_str());
        if (!handle) throw Error("zip", "Cannot open ZIP archive");
        try { scan(stopped); } catch (...) { unzClose(handle); handle = nullptr; throw; }
    }
    ~Zip() { if (handle) unzClose(handle); }
    void scan(const std::atomic_bool* stopped) {
        unz_global_info64 global{};
        if (unzGetGlobalInfo64(handle, &global) != UNZ_OK) throw Error("zip", "Invalid ZIP directory");
        if (!global.number_entry || global.number_entry > limits.entries) throw Error("entry_limit", "Too many ZIP entries or empty archive");
        std::map<std::string, bool> seen;
        std::map<std::string, std::string> spelling;
        std::uint64_t expanded = 0;
        int result = unzGoToFirstFile(handle);
        for (ZPOS64_T i = 0; i < global.number_entry; ++i) {
            cancel(stopped);
            if (result != UNZ_OK) throw Error("zip", "Truncated ZIP directory");
            unz_file_info64 info{};
            if (unzGetCurrentFileInfo64(handle, &info, nullptr, 0, nullptr, 0, nullptr, 0) != UNZ_OK)
                throw Error("zip", "Cannot read ZIP entry");
            if (!info.size_filename || info.size_filename > static_cast<std::uint64_t>(limits.path_bytes) + 1)
                throw Error("path_limit", "ZIP path is too long or empty");
            std::vector<char> name(info.size_filename + 1, 0);
            if (unzGetCurrentFileInfo64(handle, &info, name.data(), static_cast<uLong>(name.size()), nullptr, 0, nullptr, 0) != UNZ_OK)
                throw Error("zip", "Cannot read ZIP name");
            std::string path(name.data(), info.size_filename);
            const bool directory = path.back() == '/';
            if (directory) path.pop_back();
            if (path.size() > limits.path_bytes) throw Error("path_limit", "ZIP path is too long");
            if (!validRelativePath(path)) throw Error("unsafe_path", "Archive contains an invalid relative path");
            // Metadata formats are UTF-8. Legacy non-UTF8 ZIP filenames need an
            // explicit converter instead of silently changing their spelling.
            if (!(info.flag & 0x800) && std::any_of(path.begin(), path.end(), [](unsigned char c) { return c >= 128; }))
                throw Error("zip_encoding", "Non-UTF8 ZIP filenames are unsupported");
            const unsigned type = (info.external_fa >> 16) & 0170000;
            if (type && type != 0100000 && type != 0040000) throw Error("zip_link", "Links and special ZIP entries are unsupported");
            if ((type == 0040000 && !directory) || (type == 0100000 && directory) || (directory && info.uncompressed_size))
                throw Error("zip_type", "Inconsistent ZIP entry type");
            if ((info.flag & 0x41) || info.disk_num_start) throw Error("zip_encrypted", "Encrypted or multi-disk archives are unsupported");
            if (info.compression_method != 0 && info.compression_method != Z_DEFLATED)
                throw Error("zip_compression", "Only stored and deflated ZIP files are supported");
            if (info.uncompressed_size > limits.file_bytes) throw Error("file_limit", "ZIP entry exceeds the file limit");
            expanded = sum(expanded, info.uncompressed_size);
            if (expanded > limits.expanded_bytes) throw Error("expanded_limit", "Archive exceeds the expanded-size budget");
            std::string key = lowerAscii(path);
            rememberSpelling(spelling, path);
            if (!seen.emplace(key, directory).second) throw Error("duplicate_path", "ZIP contains duplicate or case-aliased paths");
            unz64_file_pos position{};
            if (unzGetFilePos64(handle, &position) != UNZ_OK) throw Error("zip", "Cannot save ZIP position");
            entries.push_back({path, info.uncompressed_size, directory, position});
            result = unzGoToNextFile(handle);
        }
        if (result != UNZ_END_OF_LIST_OF_FILE) throw Error("zip", "ZIP entry count mismatch");
        for (const auto& e : entries) {
            for (std::size_t slash = e.name.find('/'); slash != std::string::npos; slash = e.name.find('/', slash+1)) {
                auto p = seen.find(lowerAscii(e.name.substr(0, slash)));
                if (p != seen.end() && !p->second) throw Error("path_conflict", "ZIP file is also used as a directory");
            }
        }
    }
    const Entry* find(const std::string& name) const {
        auto it = std::find_if(entries.begin(), entries.end(), [&](const Entry& e) { return e.name == name && !e.directory; });
        return it == entries.end() ? nullptr : &*it;
    }
    template<class Sink> void read(const Entry& entry, Sink sink, const std::atomic_bool* stopped) {
        auto pos = entry.position;
        if (unzGoToFilePos64(handle, &pos) != UNZ_OK || unzOpenCurrentFile(handle) != UNZ_OK)
            throw Error("zip", "Cannot open ZIP member");
        bool open = true;
        try {
            std::vector<unsigned char> buffer(chunk_size);
            std::uint64_t total = 0;
            for (;;) {
                cancel(stopped);
                int got = unzReadCurrentFile(handle, buffer.data(), static_cast<unsigned>(buffer.size()));
                if (got < 0) throw Error("zip_data", "Cannot decompress ZIP member");
                if (!got) break;
                total = sum(total, static_cast<unsigned>(got));
                if (total > entry.bytes) throw Error("zip_size", "ZIP member exceeds its declared size");
                sink(buffer.data(), static_cast<std::size_t>(got));
            }
            int result = unzCloseCurrentFile(handle); open = false;
            if (result != UNZ_OK) throw Error("zip_crc", "ZIP member CRC failed");
            if (total != entry.bytes) throw Error("zip_size", "ZIP member size mismatch");
        } catch (...) { if (open) unzCloseCurrentFile(handle); throw; }
    }
    std::string text(const Entry* entry, const std::atomic_bool* stopped) {
        if (!entry) throw Error("manifest_missing", "Pack manifest is missing");
        if (entry->bytes > limits.metadata_bytes) throw Error("metadata_limit", "Pack manifest is too large");
        std::string out; out.reserve(static_cast<std::size_t>(entry->bytes));
        read(*entry, [&](const void* p, std::size_t n) { out.append(static_cast<const char*>(p), n); }, stopped);
        return out;
    }
};
struct Inspection { PackPlan plan; std::string prefix, manifest, cfg; };
Inspection inspect(Zip& zip, ManifestParser parser, const std::string& path, const std::atomic_bool* stopped) {
    Inspection out;
    int candidates = 0;
    if (zip.find("modrinth.index.json")) { out.plan.kind = PackKind::Modrinth; out.manifest = "modrinth.index.json"; ++candidates; }
    if (zip.find("manifest.json")) { out.plan.kind = PackKind::CurseForge; out.manifest = "manifest.json"; ++candidates; }
    for (const auto& e : zip.entries) {
        if (e.directory || (e.name != "instance.cfg" && (e.name.size() < 13 || e.name.substr(e.name.size()-13) != "/instance.cfg"))) continue;
        std::string prefix = e.name.substr(0, e.name.size()-12);
        if (zip.find(prefix + "mmc-pack.json")) {
            out.prefix = prefix; out.manifest = prefix + "mmc-pack.json"; out.cfg = e.name;
            out.plan.kind = PackKind::MultiMC; ++candidates;
        }
    }
    if (candidates != 1) throw Error("pack_format", candidates ? "Archive contains multiple pack roots" : "ZIP is not a supported pack export");
    const auto kind = out.plan.kind;
    out.plan = parser(kind, zip.text(zip.find(out.manifest), stopped), out.cfg.empty() ? "" : zip.text(zip.find(out.cfg), stopped));
    out.plan.kind = kind;
    out.plan.source_sha256 = verifyFile(path, {}, zip.limits.archive_bytes, stopped);
    out.plan.expanded_bytes = 0;
    for (const auto& e : zip.entries) out.plan.expanded_bytes = sum(out.plan.expanded_bytes, e.bytes);
    if (out.plan.game.java_major > 21 || out.plan.game.loader.kind == LoaderKind::Unknown) {
        out.plan.state = InstanceState::Unsupported;
        out.plan.notices.push_back("This pack requires an unsupported Java version or loader");
    }
    out.plan.required_free_bytes = sum(out.plan.expanded_bytes, zip.limits.free_margin_bytes);
    for (const auto& file : out.plan.downloads) out.plan.required_free_bytes = sum(out.plan.required_free_bytes, file.bytes);
    return out;
}
struct FileTarget { const Entry* entry = nullptr; const Download* download = nullptr; int layer = 0; };
std::map<std::string, FileTarget> selectFiles(Zip& zip, const Inspection& inspected, const ImportOptions& options) {
    std::map<std::string, FileTarget> files;
    std::map<std::string, std::string> spelling;
    std::map<std::string, std::string> all_components;
    auto add = [&](const std::string& path, FileTarget target) {
        if (!validRelativePath(path) || path.size() > zip.limits.path_bytes)
            throw Error("unsafe_path", "Pack manifest contains an invalid destination");
        auto key = lowerAscii(path);
        rememberSpelling(all_components, path);
        auto named = spelling.emplace(key, path);
        if (!named.second && named.first->second != path) throw Error("duplicate_path", "Pack destinations differ only by case");
        auto previous = files.find(path);
        if (previous != files.end() && previous->second.layer == target.layer)
            throw Error("duplicate_path", "Duplicate destination within a pack layer");
        files[path] = target;
    };
    for (const auto& d : inspected.plan.downloads) {
        if (d.optional && !options.include_optional) continue;
        if (d.path.empty() && d.manual) continue;
        if (d.path.empty() && !d.project_id.empty()) continue; // unresolved CF identifier, retained as missing
        add(d.path, {nullptr, &d, 0});
    }
    int layer = 0, mmc_roots = 0;
    for (const auto& root : inspected.plan.override_roots) {
        if (!validRelativePath(root)) throw Error("unsafe_path", "Invalid overrides root");
        ++layer;
        const std::string prefix = inspected.prefix + root + '/';
        bool used = false;
        for (const auto& e : zip.entries) {
            if (e.name.compare(0, prefix.size(), prefix) || e.directory) continue;
            used = true; add(e.name.substr(prefix.size()), {&e, nullptr, layer});
        }
        if (used && inspected.plan.kind == PackKind::MultiMC && ++mmc_roots > 1)
            throw Error("pack_roots", "MultiMC export contains both minecraft and .minecraft data");
    }
    // Detect file/parent conflicts introduced by combining archive layers and downloads.
    for (const auto& item : files) {
        for (std::size_t slash = item.first.find('/'); slash != std::string::npos; slash = item.first.find('/', slash+1))
            if (spelling.count(lowerAscii(item.first.substr(0, slash))))
                throw Error("path_conflict", "A pack file is also used as a directory");
    }
    return files;
}
void record(Json& manifest, const std::string& path, std::uint64_t bytes, const std::string& digest) {
    Json item(json_pack("{s:s,s:I,s:s}", "path", path.c_str(), "bytes", static_cast<json_int_t>(bytes), "sha256", digest.c_str()), json_decref);
    if (!item || json_array_append(manifest.get(), item.get())) throw Error("memory", "Cannot record imported file");
}
}

std::string verifyFile(const std::string& path, const Hashes& expected, std::uint64_t max_bytes, const std::atomic_bool* stopped) {
    requireSafePath(path);
    const auto size = regularSize(path);
    if (size > max_bytes) throw Error("file_limit", "File exceeds its size budget");
    Fd file(::open(path.c_str(), O_RDONLY));
    if (file.value < 0) ioFailure("Cannot open file for verification");
    HashCheck digest(expected);
    std::vector<unsigned char> buffer(chunk_size);
    std::uint64_t total = 0;
    for (;;) {
        cancel(stopped);
        ssize_t got = ::read(file.value, buffer.data(), buffer.size());
        if (got < 0 && errno == EINTR) continue;
        if (got < 0) ioFailure("Cannot read file for verification");
        if (!got) break;
        total = sum(total, static_cast<std::uint64_t>(got));
        if (total > max_bytes || total > size) throw Error("file_changed", "File changed while being verified");
        digest.update(buffer.data(), static_cast<std::size_t>(got));
    }
    if (total != size) throw Error("file_changed", "File changed while being verified");
    return digest.finish();
}

ArchiveImporter::ArchiveImporter(Storage& storage, HttpClient& http, ManifestParser parser, PlanResolver resolver, ImportLimits limits)
    : storage_(storage), http_(http), parser_(std::move(parser)), resolver_(std::move(resolver)), limits_(limits) {
    if (!parser_) throw Error("configuration", "A pack manifest parser is required");
}
PackPlan ArchiveImporter::inspectArchive(const std::string& path, const std::atomic_bool* stopped) {
    Zip zip(path, limits_, stopped);
    auto result = inspect(zip, parser_, path, stopped);
    (void)selectFiles(zip, result, {"", true, false});
    return result.plan;
}
Instance ArchiveImporter::importArchive(const std::string& path, const ImportOptions& options, ProgressCallback progress, const std::atomic_bool* stopped) {
    cancel(stopped); storage_.initialize();
    if (progress) progress({"inspect", "Checking archive", 0, 0, 0, 0});
    Zip zip(path, limits_, stopped);
    auto inspected = inspect(zip, parser_, path, stopped);
    if (resolver_ && options.download_missing && inspected.plan.state != InstanceState::Unsupported) resolver_(inspected.plan, stopped);
    cancel(stopped);
    auto files = selectFiles(zip, inspected, options);
    auto needsDownload = [&](const FileTarget& target) {
        return !target.entry && !target.download->manual && !target.download->urls.empty() &&
               options.download_missing && inspected.plan.state != InstanceState::Unsupported;
    };
    std::uint64_t budget = limits_.free_margin_bytes;
    for (const auto& file : files) {
        if (!file.second.entry && !needsDownload(file.second)) continue;
        const auto bytes = file.second.entry ? file.second.entry->bytes : file.second.download->bytes;
        if (bytes > limits_.file_bytes) throw Error("file_limit", "Pack file exceeds its size budget");
        budget = sum(budget, bytes);
    }
    if (budget - limits_.free_margin_bytes > limits_.expanded_bytes) throw Error("expanded_limit", "Pack exceeds the expanded-size budget");
    if (storage_.availableBytes() < budget) throw Error("disk_space", "Insufficient free space for the staged pack");
    const std::string id = newId(), transaction = joinPath(storage_.root(), "transactions/" + newId());
    Storage staged(transaction);
    const std::string stage_root = joinPath(transaction, "instances/" + id);
    const std::string destination = joinPath(storage_.root(), "instances/" + id);
    bool published = false, transaction_owned = false;
    try {
        requireSafePath(transaction, true);
        if (::mkdir(transaction.c_str(), 0700)) ioFailure("Cannot create private import transaction");
        transaction_owned = true;
        staged.initialize();
        createDirectories(joinPath(stage_root, "game"));
        createDirectories(joinPath(stage_root, "provenance"));
        Json journal(json_pack("{s:i,s:s,s:s,s:s}", "schema", 1, "instance_id", id.c_str(), "state", "staging", "source_sha256", inspected.plan.source_sha256.c_str()), json_decref);
        atomicWriteJson(joinPath(transaction, "transaction.json"), journal.get());
        Json managed(json_array(), json_decref), missing(json_array(), json_decref);
        if (!managed || !missing) throw Error("memory", "Cannot allocate import manifest");
        std::uint64_t total = 0; std::uint32_t done = 0;
        auto missingFile = [&](const Download& d) {
            Json item(json_pack("{s:s,s:s,s:s,s:s,s:I,s:s,s:s,s:s,s:s}", "path", d.path.c_str(),
                "project_id", d.project_id.c_str(), "file_id", d.file_id.c_str(), "manual_url", d.manual_url.c_str(),
                "bytes", static_cast<json_int_t>(d.bytes), "sha1", d.hashes.sha1.c_str(),
                "sha256", d.hashes.sha256.c_str(), "sha512", d.hashes.sha512.c_str(), "md5", d.hashes.md5.c_str()), json_decref);
            if (!item || json_array_append(missing.get(), item.get())) throw Error("memory", "Cannot record missing file");
        };
        for (const auto& d : inspected.plan.downloads)
            if ((!d.optional || options.include_optional) && d.path.empty()) missingFile(d);
        for (const auto& file : files) {
            cancel(stopped);
            const auto& relative = file.first;
            const auto& target = file.second;
            const std::string final_path = joinPath(stage_root, "game/" + relative);
            if (progress) progress({"import", relative, total, budget-limits_.free_margin_bytes, done, static_cast<std::uint32_t>(files.size())});
            if (!target.entry && !needsDownload(target)) {
                missingFile(*target.download); ++done; continue;
            }
            createDirectories(final_path.substr(0, final_path.rfind('/')));
            if (target.entry) {
                if (sum(total, target.entry->bytes) > limits_.expanded_bytes)
                    throw Error("expanded_limit", "Imported pack exceeds its byte budget");
                Fd output(openExclusiveFile(final_path));
                HashCheck digest({});
                zip.read(*target.entry, [&](const void* p, std::size_t n) {
                    const auto next_total = sum(total, n);
                    if (next_total > limits_.expanded_bytes)
                        throw Error("expanded_limit", "Imported pack exceeds its byte budget");
                    writeAll(output.value, p, n); digest.update(p, n);
                    total = next_total;
                }, stopped);
                int fd = output.value; output.value = -1; syncAndCloseFile(fd, final_path);
                record(managed, relative, target.entry->bytes, digest.finish());
            } else {
                const auto& download = *target.download;
                if (download.hashes.empty()) throw Error("missing_hash", "A downloaded pack file has no integrity digest");
                Download bounded = download;
                bounded.max_bytes = std::min({download.max_bytes, limits_.file_bytes,
                                              limits_.expanded_bytes - total});
                const auto free_bytes = storage_.availableBytes();
                if (free_bytes < limits_.free_margin_bytes)
                    throw Error("disk_space", "Insufficient free space for the staged pack");
                bounded.max_bytes = std::min(bounded.max_bytes, free_bytes - limits_.free_margin_bytes);
                if (download.bytes) {
                    if (download.bytes > bounded.max_bytes)
                        throw Error("file_limit", "Download exceeds its remaining byte budget");
                    bounded.max_bytes = download.bytes;
                }
                const std::string part = joinPath(transaction, "download-" + std::to_string(done) + ".part");
                http_.download(bounded, part, progress, stopped);
                cancel(stopped);
                const auto bytes = regularSize(part);
                if (download.bytes && download.bytes != bytes) throw Error("download_size", "Downloaded file size mismatch");
                const auto digest = verifyFile(part, download.hashes, bounded.max_bytes, stopped);
                if (sum(total, bytes) > limits_.expanded_bytes) throw Error("expanded_limit", "Downloaded pack exceeds its byte budget");
                // Close/flush a writable handle before rename: Horizon excludes
                // readers and administrative operations while a writer is open.
                Fd flush(::open(part.c_str(), O_WRONLY));
                if (flush.value < 0) ioFailure("Cannot flush downloaded file");
                int fd = flush.value; flush.value = -1; syncAndCloseFile(fd, part);
                if (pathExists(final_path)) throw Error("duplicate_path", "Import destination already exists");
                if (::rename(part.c_str(), final_path.c_str())) ioFailure("Cannot publish downloaded file");
                record(managed, relative, bytes, digest); total = sum(total, bytes);
            }
            ++done;
        }
        cancel(stopped);
        if (verifyFile(path, {}, limits_.archive_bytes, stopped) != inspected.plan.source_sha256)
            throw Error("archive_changed", "Source archive changed during import");
        atomicWriteText(joinPath(stage_root, "provenance/manifest.json"), zip.text(zip.find(inspected.manifest), stopped));
        if (!inspected.cfg.empty()) atomicWriteText(joinPath(stage_root, "provenance/instance.cfg"), zip.text(zip.find(inspected.cfg), stopped));
        // Serialization is bounded before the instance directory is published.
        atomicWriteJson(joinPath(stage_root, "managed-files.json"), managed.get(), kInstanceManifestMaxBytes);
        atomicWriteJson(joinPath(stage_root, "missing-files.json"), missing.get(), kInstanceManifestMaxBytes);
        Instance instance;
        instance.packwiz_url = inspected.plan.packwiz_url;
        if (!inspected.plan.icon.empty()) {
            // A MultiMC export carries its custom icon beside instance.cfg.
            const auto* icon = zip.find(inspected.prefix + inspected.plan.icon + ".png");
            if (icon && icon->bytes <= 4 * 1024 * 1024) {
                std::string png;
                zip.read(*icon, [&](const void* p, std::size_t n) { png.append(static_cast<const char*>(p), n); }, stopped);
                atomicWriteText(joinPath(stage_root, "icon.png"), png);
                instance.icon = "file";
            }
        }
        instance.id = id; instance.name = options.name.empty() ? inspected.plan.name : options.name;
        instance.root = destination; instance.game = inspected.plan.game; instance.installed_bytes = total;
        instance.state = inspected.plan.state;
        if (instance.state != InstanceState::Unsupported) {
            instance.state = json_array_size(missing.get()) ? InstanceState::NeedsFiles : InstanceState::NeedsPreparation;
            instance.reason = instance.state == InstanceState::NeedsFiles ? "Required pack files are missing" : "Pack files are installed; preparation is needed before the first launch";
        } else instance.reason = inspected.plan.notices.empty() ? "Unsupported game or loader requirement" : inspected.plan.notices.front();
        staged.saveInstance(instance);
        Json ready(json_pack("{s:i,s:s,s:s}", "schema", 1, "source_sha256", inspected.plan.source_sha256.c_str(), "state", "complete"), json_decref);
        atomicWriteJson(joinPath(stage_root, "import-complete.json"), ready.get());
        cancel(stopped);
        if (pathExists(destination)) throw Error("instance_exists", "Generated instance ID already exists");
        if (::rename(stage_root.c_str(), destination.c_str())) ioFailure("Cannot publish instance");
        published = true;
        try { removeOwnedTree(transaction); } catch (...) { /* Valid published instance survives cleanup failure. */ }
        // Publication is the commit point. A UI notification cannot roll it back
        // or turn a successful import into an apparent failure.
        if (progress) {
            try { progress({"complete", instance.name, total, total, done, static_cast<std::uint32_t>(files.size())}); }
            catch (...) { }
        }
        return instance;
    } catch (...) {
        if (!published && transaction_owned) {
            try { removeOwnedTree(transaction); } catch (...) { /* Startup cleanup can retry. */ }
        }
        throw;
    }
}
}
