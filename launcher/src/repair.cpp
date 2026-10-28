#include "mineswitch/repair.hpp"
#include "mineswitch/import.hpp"
#include <jansson.h>
#include <memory>
#include <map>
#include <sys/stat.h>

namespace mineswitch {
namespace {
using Json = std::unique_ptr<json_t, decltype(&json_decref)>;
Json arrayFile(const std::string& path, bool allow_missing = false) {
    Json value(readStoredJson(path, kInstanceManifestMaxBytes), json_decref);
    if (!value && allow_missing) value.reset(json_array());
    if (!json_is_array(value.get()) || json_array_size(value.get()) > 65536)
        throw Error("manual_metadata", "Invalid missing-file metadata");
    return value;
}
bool gameFileExists(const std::string& instance_root, const std::string& relative) {
    auto current = joinPath(instance_root, "game");
    if (!pathExists(current)) return false;
    std::size_t begin = 0;
    while (begin < relative.size()) {
        const auto slash = relative.find('/', begin);
        current = joinPath(current, relative.substr(begin, slash == std::string::npos ? slash : slash - begin));
        // Every call's parent has already been checked. An absent directory is
        // an unresolved file, while existing symlinks are still rejected.
        if (!pathExists(current)) return false;
        if (slash == std::string::npos) return true;
        begin = slash + 1;
    }
    return false;
}
std::string string(json_t* item, const char* name) {
    auto* value = json_object_get(item, name);
    if (!value) return {};
    if (!json_is_string(value) || json_string_length(value) > 8192)
        throw Error("manual_metadata", "Invalid missing-file field");
    return std::string(json_string_value(value), json_string_length(value));
}
Download entry(json_t* item) {
    if (!json_is_object(item)) throw Error("manual_metadata", "Invalid missing-file entry");
    Download d;
    d.path = string(item, "path");
    if (!d.path.empty() && !validRelativePath(d.path)) throw Error("manual_metadata", "Invalid missing-file path");
    d.project_id = string(item, "project_id"); d.file_id = string(item, "file_id");
    d.manual = true; d.manual_url = string(item, "manual_url");
    d.hashes = {string(item,"sha1"), string(item,"sha256"), string(item,"sha512"), string(item,"md5")};
    auto* bytes = json_object_get(item, "bytes");
    if (bytes) {
        if (!json_is_integer(bytes) || json_integer_value(bytes) < 0 ||
            static_cast<std::uint64_t>(json_integer_value(bytes)) > d.max_bytes)
            throw Error("manual_metadata", "Invalid missing-file length");
        d.bytes = static_cast<std::uint64_t>(json_integer_value(bytes));
    }
    return d;
}
}
std::vector<Download> missingFiles(const Instance& instance) {
    auto list = arrayFile(joinPath(instance.root, "missing-files.json"));
    std::vector<Download> result;
    for (std::size_t i = 0; i < json_array_size(list.get()); ++i)
        result.push_back(entry(json_array_get(list.get(), i)));
    return result;
}
Instance verifyManualFiles(Storage& storage, Instance instance, ProgressCallback progress,
                           const std::atomic_bool* cancelled) {
    if (instance.id.empty() || instance.id.size() > 64 ||
        instance.id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") != std::string::npos ||
        instance.root != joinPath(storage.root(), "instances/" + instance.id))
        throw Error("manual_instance", "Instance is outside owned storage");
    if (instance.state != InstanceState::NeedsFiles)
        throw Error("manual_state", "Instance does not need manual files");
    auto stop = [&] { if (cancelled && cancelled->load()) throw Error("cancelled", "File verification cancelled"); };
    stop();
    auto path = joinPath(instance.root, "missing-files.json");
    auto previous = arrayFile(path);
    Json remaining(json_array(), json_decref);
    if (!remaining) throw Error("memory", "Cannot track missing files");
    auto managed_path = joinPath(instance.root, "managed-files.json");
    auto managed = arrayFile(managed_path, true);
    if (!managed) throw Error("memory", "Cannot track verified files");
    for (std::size_t i = 0; i < json_array_size(previous.get()); ++i) {
        stop();
        auto* original = json_array_get(previous.get(), i);
        auto file = entry(original); bool valid = false;
        if (progress) progress({"verify", file.path.empty() ? "Unresolved provider file" : file.path,
                                0, 0, static_cast<std::uint32_t>(i), static_cast<std::uint32_t>(json_array_size(previous.get()))});
        if (!file.path.empty() && !file.hashes.empty()) {
            auto destination = joinPath(instance.root, "game/" + file.path);
            if (gameFileExists(instance.root, file.path)) {
                requireSafePath(destination);
                struct stat info{};
                if (::lstat(destination.c_str(), &info) == 0 && S_ISREG(info.st_mode) && info.st_size >= 0 &&
                    (!file.bytes || static_cast<std::uint64_t>(info.st_size) == file.bytes)) {
                    try {
                        auto sha256 = verifyFile(destination, file.hashes, file.max_bytes, cancelled);
                        Json record(json_pack("{s:s,s:I,s:s}", "path", file.path.c_str(), "bytes",
                                              static_cast<json_int_t>(info.st_size), "sha256", sha256.c_str()), json_decref);
                        if (!record) throw Error("memory", "Cannot track verified file");
                        bool replaced = false;
                        for (std::size_t j = 0; j < json_array_size(managed.get()); ++j)
                            if (string(json_array_get(managed.get(), j), "path") == file.path) {
                                if (json_array_set(managed.get(), j, record.get())) throw Error("memory", "Cannot update file receipt");
                                replaced = true; break;
                            }
                        if (!replaced && json_array_append(managed.get(), record.get())) throw Error("memory", "Cannot track verified file");
                        valid = true;
                    } catch (const Error& error) {
                        if (error.code != "hash_mismatch") throw;
                    }
                }
            }
        }
        if (!valid && json_array_append(remaining.get(), original)) throw Error("memory", "Cannot retain missing file");
    }
    stop();
    // Receipts are idempotent. Commit the authoritative missing list afterwards,
    // so an interrupted metadata update never skips file verification on retry.
    std::map<std::string, std::uint64_t> sizes;
    for (std::size_t i = 0; i < json_array_size(managed.get()); ++i) {
        auto file = entry(json_array_get(managed.get(), i));
        sizes[file.path] = file.bytes;
    }
    std::uint64_t installed_bytes = 0;
    for (const auto& size : sizes) {
        if (size.second > INT64_MAX - installed_bytes) throw Error("manual_metadata", "File receipt size overflow");
        installed_bytes += size.second;
    }
    atomicWriteJson(managed_path, managed.get(), kInstanceManifestMaxBytes);
    atomicWriteJson(path, remaining.get(), kInstanceManifestMaxBytes);
    auto count = json_array_size(remaining.get());
    instance.state = count ? InstanceState::NeedsFiles : InstanceState::NeedsPreparation;
    instance.reason = count ? "Files remaining: " + std::to_string(count) + ". Check the names and checksums." :
                              "All pack files are verified. Minecraft can be prepared now.";
    instance.installed_bytes = installed_bytes;
    storage.saveInstance(instance);
    return instance;
}
}
