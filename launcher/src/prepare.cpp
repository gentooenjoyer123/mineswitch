#include "mineswitch/prepare.hpp"
#include "mineswitch/heap.hpp"
#include "mineswitch/import.hpp"
#include <jansson.h>
#include <mbedtls/md.h>
#include <minizip/unzip.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __SWITCH__
#include <switch.h>
#endif

namespace mineswitch {
namespace {
constexpr std::uint64_t metadata_limit = 8 * 1024 * 1024;
constexpr std::uint64_t artifact_limit = 512 * 1024 * 1024;
constexpr std::uint64_t free_margin = 32 * 1024 * 1024;
constexpr const char* manifest_url = "https://piston-meta.mojang.com/mc/game/version_manifest_v2.json";
constexpr const char* lwjgl_version = "3.3.3";
constexpr const char* boot_modules = "java.base,java.compiler,java.desktop,java.management,java.naming,java.net.http,java.rmi,java.scripting,java.security.jgss,java.sql,jdk.crypto.ec,jdk.management,jdk.naming.dns,jdk.unsupported,jdk.zipfs";
std::mutex preparation_mutex;
struct JsonDelete { void operator()(json_t* value) const { json_decref(value); } };
using Json = std::unique_ptr<json_t, JsonDelete>;

[[noreturn]] void bad(const std::string& message) { throw Error("prepare_metadata", message); }
void cancel(const std::atomic_bool* token) {
    if (token && token->load()) throw Error("cancelled", "Preparation cancelled");
}
const json_t* member(const json_t* value, const char* name) { return json_object_get(value, name); }
const json_t* object(const json_t* value, const char* name) {
    if (!json_is_object(value)) bad(std::string(name) + " must be an object");
    return value;
}
const json_t* array(const json_t* value, const char* name) {
    if (!json_is_array(value) || json_array_size(value) > 100000) bad(std::string(name) + " must be a bounded array");
    return value;
}
std::string string(const json_t* value, const char* name, bool required = true) {
    const auto* field = member(value, name);
    if ((!field || json_is_null(field)) && !required) return {};
    if (!json_is_string(field)) bad(std::string(name) + " must be a string");
    std::string result(json_string_value(field), json_string_length(field));
    if ((required && result.empty()) || result.find('\0') != std::string::npos || result.find_first_of("\r\n") != std::string::npos)
        bad(std::string(name) + " contains an invalid value");
    return result;
}
std::uint64_t integer(const json_t* value, const char* name, bool required = true) {
    auto field = member(value, name);
    if (!field && !required) return 0;
    if (!json_is_integer(field) || json_integer_value(field) < 0) bad(std::string(name) + " must be a nonnegative integer");
    return static_cast<std::uint64_t>(json_integer_value(field));
}
Json parse(const std::string& input) {
    if (input.empty() || input.size() > metadata_limit) bad("Metadata exceeds its size limit");
    json_error_t error{};
    Json result(json_loadb(input.data(), input.size(), JSON_REJECT_DUPLICATES, &error));
    if (!result) bad("Invalid metadata JSON at line " + std::to_string(error.line));
    object(result.get(), "metadata");
    return result;
}
std::string java_path(const std::string& path) {
    return path.compare(0, 6, "sdmc:/") == 0 ? path.substr(5) : path;
}
std::string parent(const std::string& path) { return path.substr(0, path.find_last_of('/')); }
std::string identifier(const std::string& value, const char* label) {
    if (value.empty() || value.size() > 128 || !validRelativePath(value) || value.find('/') != std::string::npos)
        bad(std::string("Invalid ") + label);
    for (unsigned char c : value)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.' || c == '+'))
            bad(std::string("Invalid ") + label);
    return value;
}
bool companion_loader(const std::string& version) {
    std::array<unsigned, 3> parts{};
    std::size_t start = 0;
    for (unsigned i = 0; i < parts.size(); ++i) {
        auto end = version.find('.', start);
        if ((i < 2) != (end != std::string::npos)) return false;
        auto value = version.substr(start, end == std::string::npos ? end : end - start);
        if (value.empty() || value.size() > 6 || value.find_first_not_of("0123456789") != std::string::npos) return false;
        for (char c : value) parts[i] = parts[i] * 10 + static_cast<unsigned>(c - '0');
        start = end == std::string::npos ? version.size() : end + 1;
    }
    return parts[0] > 0 || parts[1] >= 16;
}
std::string https(const std::string& url) {
    if (url.compare(0, 8, "https://") != 0 || url.size() > 8192)
        bad("Official artifact metadata requires an HTTPS URL");
    auto end = url.find_first_of("/?#", 8);
    auto authority = url.substr(8, end == std::string::npos ? end : end - 8);
    if (authority.empty() ||
        authority.find('@') != std::string::npos || url.find('#') != std::string::npos || url.find('\\') != std::string::npos)
        bad("Official artifact metadata requires an HTTPS URL");
    for (unsigned char c : url) if (c <= 32 || c == 127) bad("Invalid artifact URL");
    return url;
}
std::string hex(std::string value, std::size_t length) {
    if (value.empty()) return {};
    if (value.size() != length) bad("Invalid artifact checksum length");
    for (auto& c : value) {
        if (c >= 'A' && c <= 'F') c = static_cast<char>(c - 'A' + 'a');
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) bad("Invalid artifact checksum");
    }
    return value;
}
std::string digest(const std::string& input, mbedtls_md_type_t type) {
    auto* info = mbedtls_md_info_from_type(type);
    std::array<unsigned char, MBEDTLS_MD_MAX_SIZE> output{};
    if (!info || mbedtls_md(info, reinterpret_cast<const unsigned char*>(input.data()), input.size(), output.data()))
        throw Error("prepare_hash", "Cannot hash preparation metadata");
    static constexpr char alphabet[] = "0123456789abcdef";
    std::string result;
    for (unsigned i = 0; i < mbedtls_md_get_size(info); ++i) {
        result += alphabet[output[i] >> 4]; result += alphabet[output[i] & 15];
    }
    return result;
}
std::string offline_uuid(const std::string& username) {
    auto value = digest("OfflinePlayer:" + username, MBEDTLS_MD_MD5);
    value[12] = '3';
    unsigned nibble = value[16] <= '9' ? value[16] - '0' : value[16] - 'a' + 10;
    value[16] = "0123456789abcdef"[(nibble & 3) | 8];
    return value.substr(0, 8) + "-" + value.substr(8, 4) + "-" + value.substr(12, 4) + "-" + value.substr(16, 4) + "-" + value.substr(20);
}
std::uint64_t regular_size(const std::string& path) {
    requireSafePath(path);
    struct stat info{};
    if (::lstat(path.c_str(), &info) || !S_ISREG(info.st_mode) || info.st_size < 0)
        throw Error("prepare_file", "Expected a regular prepared artifact");
    return static_cast<std::uint64_t>(info.st_size);
}
void unlink_owned(const std::string& path) {
    requireSafePath(path, true);
    if (::unlink(path.c_str()) && errno != ENOENT) throw Error("prepare_cache", "Cannot remove a preparation journal file");
}
void commit_binary(const std::string& part, const std::string& target) {
    requireSafePath(part); requireSafePath(target, true);
    auto backup = target + ".old";
    if (pathExists(backup)) unlink_owned(backup);
    bool previous = pathExists(target);
    if (previous && ::rename(target.c_str(), backup.c_str())) throw Error("prepare_cache", "Cannot preserve cached artifact");
    if (::rename(part.c_str(), target.c_str())) {
        if (previous) ::rename(backup.c_str(), target.c_str());
        throw Error("prepare_cache", "Cannot publish verified cached artifact");
    }
    if (previous) unlink_owned(backup);
}
void copy_verified_input(const std::string& source, const std::string& target) {
    requireSafePath(source);
    int input = ::open(source.c_str(), O_RDONLY);
    if (input < 0) throw Error("prepare_file", "Cannot read verified shared input");
    int output = -1;
    try {
        output = openExclusiveFile(target);
        std::array<char, 65536> buffer{};
        for (;;) {
            auto count = ::read(input, buffer.data(), buffer.size());
            if (count < 0 && errno == EINTR) continue;
            if (count < 0) throw Error("prepare_file", "Cannot copy verified shared input");
            if (!count) break;
            std::size_t offset = 0;
            while (offset < static_cast<std::size_t>(count)) {
                auto wrote = ::write(output, buffer.data() + offset, static_cast<std::size_t>(count) - offset);
                if (wrote < 0 && errno == EINTR) continue;
                if (wrote <= 0) throw Error("prepare_file", "Cannot stage verified shared input");
                offset += static_cast<std::size_t>(wrote);
            }
        }
        ::close(input); input = -1;
        auto closing = output; output = -1; syncAndCloseFile(closing, target);
    } catch (...) {
        if (input >= 0) ::close(input);
        if (output >= 0) ::close(output);
        throw;
    }
}
using Features = std::map<std::string, bool>;
bool allowed(const json_t* rules, const Features& features) {
    if (!rules) return true;
    array(rules, "rules");
    bool result = false;
    std::size_t i; json_t* rule;
    json_array_foreach(rules, i, rule) {
        object(rule, "rule");
        bool matches = true;
        auto os = member(rule, "os");
        if (os) {
            object(os, "os rule");
            auto name = string(os, "name", false), arch = string(os, "arch", false);
            matches = (name.empty() || name == "horizon") && (arch.empty() || arch == "aarch64" || arch == "arm64");
            if (member(os, "version")) matches = false; // No foreign OS/version impersonation.
        }
        auto required = member(rule, "features");
        if (required) {
            object(required, "feature rule");
            const char* key; json_t* value;
            json_object_foreach(const_cast<json_t*>(required), key, value) {
                if (!json_is_boolean(value)) bad("Feature rule must be boolean");
                auto found = features.find(key);
                bool actual = found != features.end() && found->second;
                matches = matches && actual == static_cast<bool>(json_is_true(value));
            }
        }
        auto action = string(rule, "action");
        if (action != "allow" && action != "disallow") bad("Unknown artifact rule action");
        if (matches) result = action == "allow";
    }
    return result;
}
std::string expand(std::string value, const std::map<std::string, std::string>& variables) {
    std::size_t pos = 0;
    while ((pos = value.find("${", pos)) != std::string::npos) {
        auto end = value.find('}', pos + 2);
        if (end == std::string::npos) bad("Unterminated launch argument placeholder");
        auto key = value.substr(pos + 2, end - pos - 2);
        auto found = variables.find(key);
        if (found == variables.end()) throw Error("prepare_argument", "Unsupported launch argument placeholder: " + key);
        value.replace(pos, end - pos + 1, found->second);
        pos += found->second.size();
    }
    if (value.find('\0') != std::string::npos || value.find_first_of("\r\n") != std::string::npos)
        bad("Invalid control character in launch argument");
    return value;
}
void arguments(const json_t* values, const Features& features,
               const std::map<std::string, std::string>& variables, std::vector<std::string>& target) {
    if (!values) return;
    array(values, "launch arguments");
    auto add = [&](json_t* value) {
        if (!json_is_string(value)) bad("Launch argument must be a string");
        target.push_back(expand(std::string(json_string_value(value), json_string_length(value)), variables));
    };
    std::size_t i; json_t* item;
    json_array_foreach(values, i, item) {
        if (json_is_string(item)) { add(item); continue; }
        object(item, "conditional argument");
        if (!allowed(member(item, "rules"), features)) continue;
        auto value = member(item, "value");
        if (json_is_array(value)) {
            std::size_t j; json_t* arg;
            json_array_foreach(value, j, arg) add(arg);
        } else add(const_cast<json_t*>(value));
    }
}
struct Coordinate {
    std::string group, artifact, version, classifier, path, key, extension = "jar";
};
Coordinate coordinate(const std::string& value, bool classpath = true) {
    auto input = value.substr(0, value.find('@'));
    if (input.empty() || input.back() == ':') bad("Invalid Maven coordinate");
    std::vector<std::string> fields;
    std::istringstream stream(input);
    std::string part;
    while (std::getline(stream, part, ':')) fields.push_back(identifier(part, "Maven coordinate"));
    if (fields.size() < 3 || fields.size() > 4) bad("Invalid Maven coordinate");
    Coordinate out;
    if (value.find('@') != std::string::npos) out.extension = identifier(value.substr(value.find('@') + 1), "artifact extension");
    if (out.extension.find('.') != std::string::npos || (classpath && out.extension != "jar")) bad("Unsupported classpath artifact extension");
    out.group = fields[0]; out.artifact = fields[1]; out.version = fields[2];
    if (fields.size() == 4) out.classifier = fields[3];
    auto group_path = out.group;
    std::replace(group_path.begin(), group_path.end(), '.', '/');
    out.path = group_path + "/" + out.artifact + "/" + out.version + "/" + out.artifact + "-" + out.version
        + (out.classifier.empty() ? "" : "-" + out.classifier) + "." + out.extension;
    if (!validRelativePath(out.path)) bad("Invalid Maven artifact path");
    out.key = out.group + ":" + out.artifact + ":" + out.classifier + "@" + out.extension;
    return out;
}
Hashes published_hashes(const json_t* value) {
    Hashes out;
    out.sha1 = hex(string(value, "sha1", false), 40);
    out.sha256 = hex(string(value, "sha256", false), 64);
    out.sha512 = hex(string(value, "sha512", false), 128);
    out.md5 = hex(string(value, "md5", false), 32);
    return out;
}
void set_string(json_t* value, const char* name, const std::string& text) {
    if (json_object_set_new(value, name, json_stringn(text.data(), text.size()))) throw Error("prepare_memory", "Cannot create launch metadata");
}
void set_strings(json_t* value, const char* name, const std::vector<std::string>& strings) {
    Json list(json_array());
    if (!list) throw Error("prepare_memory", "Cannot create launch metadata");
    for (const auto& text : strings)
        if (json_array_append_new(list.get(), json_stringn(text.data(), text.size()))) throw Error("prepare_memory", "Cannot create launch metadata");
    if (json_object_set(value, name, list.get())) throw Error("prepare_memory", "Cannot create launch metadata");
}

class ArtifactVerifier {
public:
    struct Stamp {
        bool exists = false;
        std::uint64_t size = 0;
        std::int64_t mtime = 0, mtime_ns = 0, ctime = 0, ctime_ns = 0;
        std::string identity;
        bool reliable() const { return mtime > 0 && mtime_ns >= 0 && mtime_ns < 1000000000 && ctime >= 0 && ctime_ns >= 0 && ctime_ns < 1000000000; }
        bool operator==(const Stamp& rhs) const {
            return exists == rhs.exists && size == rhs.size && mtime == rhs.mtime && mtime_ns == rhs.mtime_ns &&
                   ctime == rhs.ctime && ctime_ns == rhs.ctime_ns && identity == rhs.identity;
        }
    };
    ArtifactVerifier(const std::string& root, bool force, const std::atomic_bool* stopped)
        : root_(root), path_(joinPath(root, "cache/verified-artifacts.json")), force_(force), stopped_(stopped) {
        directory(joinPath(root, "cache"), true);
        try {
            Json stored(readStoredJson(path_, kInstanceManifestMaxBytes));
            const auto now = std::time(nullptr);
            if (stored && json_is_object(stored.get()) && integer(stored.get(), "schema") == 1 && now > 0 &&
                integer(stored.get(), "saved_at") > 0 &&
                integer(stored.get(), "saved_at") <= static_cast<std::uint64_t>(now) &&
                json_is_object(member(stored.get(), "files")) && json_object_size(member(stored.get(), "files")) <= 100000)
                cached_.reset(json_incref(const_cast<json_t*>(member(stored.get(), "files"))));
        } catch (const Error& error) {
            if (error.code != "storage_json" && error.code != "storage_size" && error.code != "prepare_metadata") throw;
        }
    }
    void directory(const std::string& path, bool create = false) {
        confined(path, true);
        std::string current = path.compare(0, 6, "sdmc:/") == 0 ? "sdmc:/" : "/";
        std::size_t start = current.size();
        while (start < path.size()) {
            auto slash = path.find('/', start);
            auto part = path.substr(start, slash == std::string::npos ? slash : slash - start);
            if (current.back() != '/') current += '/';
            current += part;
#ifdef __SWITCH__
            // The SD FAT backend cannot contain symlinks. Cache only directories
            // validated during this preparation; never cache host confinement.
            if (!directories_.count(current))
#endif
            {
                struct stat st{}; ++metadata_checks_;
                if (::lstat(current.c_str(), &st)) {
                    if (!create || errno != ENOENT || (::mkdir(current.c_str(), 0700) && errno != EEXIST))
                        throw Error("prepare_file", "Cannot inspect or create preparation directory");
                    ++metadata_checks_;
                    if (::lstat(current.c_str(), &st)) throw Error("prepare_file", "Cannot inspect created directory");
                }
                if (!S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode)) throw Error("storage_path", "Preparation paths cannot traverse symbolic links or files");
                directories_.insert(current);
            }
            if (slash == std::string::npos) break;
            start = slash + 1;
        }
    }
    Stamp inspect(const std::string& path) {
        confined(path); directory(parent(path));
        struct stat st{}; ++metadata_checks_;
        Stamp result;
        if (::lstat(path.c_str(), &st)) {
            if (errno == ENOENT) { seen_[path] = result; return result; }
            throw Error("prepare_file", "Cannot inspect prepared artifact");
        }
        if (S_ISLNK(st.st_mode)) throw Error("storage_path", "Prepared artifacts cannot be symbolic links");
        if (!S_ISREG(st.st_mode) || st.st_size < 0) throw Error("prepare_file", "Expected a regular prepared artifact");
        result.exists = true; result.size = static_cast<std::uint64_t>(st.st_size);
        result.mtime = st.st_mtim.tv_sec; result.mtime_ns = st.st_mtim.tv_nsec;
        result.ctime = st.st_ctim.tv_sec; result.ctime_ns = st.st_ctim.tv_nsec;
#ifndef __SWITCH__
        result.identity = std::to_string(static_cast<std::uint64_t>(st.st_dev)) + ":" + std::to_string(static_cast<std::uint64_t>(st.st_ino));
#endif
        auto previous = seen_.find(path);
        if (previous != seen_.end() && !(previous->second == result)) digests_.erase(path);
        seen_[path] = result; return result;
    }
    bool check(const std::string& path, const Download& file, Stamp& stamp, bool allow_receipt = true) {
        cancel(stopped_); ++files_checked_;
        digests_.erase(path);
        if (!stamp.exists || (file.bytes && stamp.size != file.bytes) || stamp.size > file.max_bytes) return false;
        const auto proof = expectation(file.hashes, file.bytes);
        if (allow_receipt && !force_ && !file.hashes.empty() && stamp.reliable()) {
            const auto* entry = member(cached_.get(), path.c_str());
            try {
                auto wanted = hex(string(entry, "sha256"), 64);
                auto checked = integer(entry, "checked_at");
                auto now = std::time(nullptr);
                bool old_enough = now > 0 && checked > 0 && checked <= static_cast<std::uint64_t>(now);
#ifdef __SWITCH__
                // FAT timestamps have coarse resolution. Never reuse a receipt
                // within the same timestamp quantum as its full verification.
                old_enough = old_enough && static_cast<std::uint64_t>(now) - checked >= 2;
#endif
                if (!wanted.empty() && old_enough && string(entry, "expected") == proof && matches(entry, stamp)) {
                    ++receipt_hits_; digests_[path] = wanted;
                    json_object_set(next_.get(), path.c_str(), const_cast<json_t*>(entry));
                    return true;
                }
            } catch (const Error& error) { if (error.code != "prepare_metadata") throw; }
        }
        try {
            auto actual = hash_file(path, file.hashes, file.max_bytes, stamp);
            remember(path, file, stamp, actual); return true;
        } catch (const Error& error) {
            if (error.code == "hash_mismatch" || error.code == "download_hash" || error.code == "file_changed" || error.code == "file_limit") return false;
            throw;
        }
    }
    std::string hash_file(const std::string& path, const Hashes& expected = {}, std::uint64_t limit = artifact_limit) {
        ++files_checked_;
        auto stamp = inspect(path);
        return hash_file(path, expected, limit, stamp);
    }
    std::string fingerprint(const std::string& path) {
        auto known = seen_.find(path); auto hash = digests_.find(path);
        if (known != seen_.end() && hash != digests_.end()) {
            const auto before = known->second; const auto actual = hash->second;
            if (inspect(path) == before) return actual;
        }
        return hash_file(path);
    }
    void promoted(const std::string& source, const std::string& target, const Download& file) {
        const auto before = seen_.at(source); const auto hash = digests_.at(source);
        auto after = inspect(target);
        if (before.size != after.size || before.mtime != after.mtime || before.mtime_ns != after.mtime_ns || before.identity != after.identity) {
            if (!check(target, file, after, false)) throw Error("prepare_hash", "Published artifact changed after verification");
        } else remember(target, file, after, hash);
    }
    std::uint64_t size(const std::string& path) {
        auto found = seen_.find(path);
        auto value = found == seen_.end() ? inspect(path) : found->second;
        if (!value.exists) throw Error("prepare_file", "Prepared artifact is missing");
        return value.size;
    }
    void finish(LaunchPlan& plan, json_t* stored) {
        cancel(stopped_);
        Json cache(json_object());
        json_object_set_new(cache.get(), "schema", json_integer(1));
        json_object_set_new(cache.get(), "saved_at", json_integer(std::max<std::time_t>(0, std::time(nullptr))));
        json_object_set(cache.get(), "files", next_.get());
        atomicWriteJson(path_, cache.get(), kInstanceManifestMaxBytes);
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started_).count();
        Json stats(json_object());
        for (const auto& pair : std::map<std::string, std::uint64_t>{{"elapsed_ms", static_cast<std::uint64_t>(elapsed)},
                {"bytes_hashed", bytes_hashed_}, {"hash_reads", hash_reads_}, {"receipt_hits", receipt_hits_},
                {"metadata_checks", metadata_checks_}, {"files_checked", files_checked_}})
            json_object_set_new(stats.get(), pair.first.c_str(), json_integer(static_cast<json_int_t>(pair.second)));
        json_object_set_new(stats.get(), "force_validation", json_boolean(force_));
        json_object_set(stored, "preparation_stats", stats.get());
        plan.preparation_summary = "PREPARATION_STATS elapsed_ms=" + std::to_string(elapsed) + " files_checked=" + std::to_string(files_checked_)
            + " hash_reads=" + std::to_string(hash_reads_) + " bytes_hashed=" + std::to_string(bytes_hashed_) + " receipt_hits=" + std::to_string(receipt_hits_)
            + " metadata_checks=" + std::to_string(metadata_checks_) + " force_validation=" + (force_ ? "1" : "0");
        set_string(stored, "preparation_summary", plan.preparation_summary);
    }
private:
    std::string root_, path_;
    bool force_;
    const std::atomic_bool* stopped_;
    Json cached_{json_object()}, next_{json_object()};
    std::set<std::string> directories_;
    std::map<std::string, Stamp> seen_;
    std::map<std::string, std::string> digests_;
    std::chrono::steady_clock::time_point started_ = std::chrono::steady_clock::now();
    std::uint64_t bytes_hashed_ = 0, hash_reads_ = 0, receipt_hits_ = 0, metadata_checks_ = 0, files_checked_ = 0;
    void confined(const std::string& path, bool directory = false) const {
        if (directory && path == root_) return;
        if (path.compare(0, root_.size() + 1, root_ + "/") != 0 || !validRelativePath(path.substr(root_.size() + 1)))
            throw Error("storage_path", "Prepared artifact leaves owned storage");
    }
    static std::string expectation(const Hashes& hashes, std::uint64_t bytes) {
        return hex(hashes.sha1, 40) + ":" + hex(hashes.sha256, 64) + ":" + hex(hashes.sha512, 128) + ":" + hex(hashes.md5, 32) + ":" + std::to_string(bytes);
    }
    static bool matches(const json_t* entry, const Stamp& s) {
        return integer(entry, "size") == s.size && integer(entry, "mtime") == static_cast<std::uint64_t>(s.mtime) &&
            integer(entry, "mtime_ns") == static_cast<std::uint64_t>(s.mtime_ns) && integer(entry, "ctime") == static_cast<std::uint64_t>(s.ctime) &&
            integer(entry, "ctime_ns") == static_cast<std::uint64_t>(s.ctime_ns) && string(entry, "identity", false) == s.identity;
    }
    void remember(const std::string& path, const Download& file, const Stamp& stamp, const std::string& actual) {
        seen_[path] = stamp; digests_[path] = actual;
        if (file.hashes.empty() || !stamp.reliable() || std::time(nullptr) <= 0) return;
        Json item(json_object());
        set_string(item.get(), "expected", expectation(file.hashes, file.bytes)); set_string(item.get(), "sha256", actual);
        set_string(item.get(), "identity", stamp.identity);
        for (const auto& pair : std::map<std::string, std::uint64_t>{{"size", stamp.size}, {"mtime", static_cast<std::uint64_t>(stamp.mtime)},
                {"mtime_ns", static_cast<std::uint64_t>(stamp.mtime_ns)}, {"ctime", static_cast<std::uint64_t>(stamp.ctime)},
                {"ctime_ns", static_cast<std::uint64_t>(stamp.ctime_ns)}, {"checked_at", static_cast<std::uint64_t>(std::time(nullptr))}})
            json_object_set_new(item.get(), pair.first.c_str(), json_integer(static_cast<json_int_t>(pair.second)));
        json_object_set(next_.get(), path.c_str(), item.get());
    }
    std::string hash_file(const std::string& path, const Hashes& expected, std::uint64_t limit, Stamp& stamp) {
        if (!stamp.exists) throw Error("prepare_file", "Expected a prepared artifact");
        if (stamp.size > limit) throw Error("file_limit", "Artifact exceeds its verification limit");
        struct Digest {
            mbedtls_md_context_t context{};
            std::string expected;
            Digest(mbedtls_md_type_t type, const std::string& wanted, std::size_t length) : expected(hex(wanted, length)) {
                mbedtls_md_init(&context);
                if (mbedtls_md_setup(&context, mbedtls_md_info_from_type(type), 0) || mbedtls_md_starts(&context)) {
                    mbedtls_md_free(&context); throw Error("prepare_hash", "Cannot initialize artifact verification");
                }
            }
            ~Digest() { mbedtls_md_free(&context); }
            std::string finish() {
                unsigned char bytes[MBEDTLS_MD_MAX_SIZE];
                if (mbedtls_md_finish(&context, bytes)) throw Error("prepare_hash", "Cannot finish artifact verification");
                std::string actual;
                for (unsigned i = 0; i < mbedtls_md_get_size(context.md_info); ++i) { actual += "0123456789abcdef"[bytes[i] >> 4]; actual += "0123456789abcdef"[bytes[i] & 15]; }
                if (!expected.empty() && expected != actual) throw Error("hash_mismatch", "Published artifact digest mismatch");
                return actual;
            }
        };
        std::vector<std::unique_ptr<Digest>> hashes;
        hashes.emplace_back(new Digest(MBEDTLS_MD_SHA256, expected.sha256, 64));
        if (!expected.sha1.empty()) hashes.emplace_back(new Digest(MBEDTLS_MD_SHA1, expected.sha1, 40));
        if (!expected.sha512.empty()) hashes.emplace_back(new Digest(MBEDTLS_MD_SHA512, expected.sha512, 128));
        if (!expected.md5.empty()) hashes.emplace_back(new Digest(MBEDTLS_MD_MD5, expected.md5, 32));
        int flags = O_RDONLY;
#ifdef O_NOFOLLOW
        flags |= O_NOFOLLOW;
#endif
        int fd = ::open(path.c_str(), flags);
        if (fd < 0) throw Error("prepare_file", "Cannot open artifact for verification");
        ++hash_reads_;
        std::uint64_t count = 0;
        try {
            std::array<unsigned char, 65536> buffer{};
            for (;;) {
                cancel(stopped_);
                auto read = ::read(fd, buffer.data(), buffer.size());
                if (read < 0 && errno == EINTR) continue;
                if (read < 0) throw Error("prepare_file", "Cannot read artifact for verification");
                if (!read) break;
                count += static_cast<std::uint64_t>(read); bytes_hashed_ += static_cast<std::uint64_t>(read);
                if (count > stamp.size || count > limit) throw Error("file_changed", "Artifact changed while being verified");
                for (auto& hash : hashes) if (mbedtls_md_update(&hash->context, buffer.data(), static_cast<std::size_t>(read)))
                    throw Error("prepare_hash", "Cannot update artifact verification");
            }
            ::close(fd); fd = -1;
            if (count != stamp.size || !(inspect(path) == stamp)) throw Error("file_changed", "Artifact metadata changed while being verified");
            std::string actual;
            for (auto& hash : hashes) { auto value = hash->finish(); if (actual.empty()) actual = value; }
            digests_[path] = actual; return actual;
        } catch (...) { if (fd >= 0) ::close(fd); throw; }
    }
};

class InstallerZip {
public:
    explicit InstallerZip(const std::string& path) {
        requireSafePath(path);
        zip_ = unzOpen64(path.c_str());
        if (!zip_) throw Error("prepare_installer_zip", "Cannot open verified installer archive");
    }
    ~InstallerZip() { if (zip_) unzClose(zip_); }
    InstallerZip(const InstallerZip&) = delete;
    bool has(const std::string& entry) { return unzLocateFile(zip_, entry.c_str(), 1) == UNZ_OK; }
    std::string read(const std::string& entry, std::uint64_t maximum = metadata_limit) {
        if (!validRelativePath(entry) || unzLocateFile(zip_, entry.c_str(), 1) != UNZ_OK)
            throw Error("prepare_installer_entry", "Required installer resource is missing");
        unz_file_info64 info{};
        if (unzGetCurrentFileInfo64(zip_, &info, nullptr, 0, nullptr, 0, nullptr, 0) != UNZ_OK ||
            info.uncompressed_size > maximum || (info.flag & 1) || ((info.external_fa >> 16) & 0170000) == 0120000)
            throw Error("prepare_installer_entry", "Unsupported or oversized installer resource");
        if (unzOpenCurrentFile(zip_) != UNZ_OK) throw Error("prepare_installer_entry", "Cannot read installer resource");
        std::string result;
        std::array<char, 32768> buffer{};
        try {
            int count;
            while ((count = unzReadCurrentFile(zip_, buffer.data(), buffer.size())) > 0) {
                if (static_cast<std::uint64_t>(count) > maximum - result.size()) throw Error("prepare_installer_entry", "Installer resource exceeds its limit");
                result.append(buffer.data(), static_cast<std::size_t>(count));
            }
            if (count < 0 || result.size() != info.uncompressed_size) throw Error("prepare_installer_entry", "Installer resource is truncated");
            int closed = unzCloseCurrentFile(zip_);
            if (closed != UNZ_OK) throw Error("prepare_installer_entry", "Installer resource failed CRC verification");
        } catch (...) { unzCloseCurrentFile(zip_); throw; }
        return result;
    }
private:
    unzFile zip_ = nullptr;
};

// The JNI invocation API accepts one option per string. Preserve real module
// names from verified jars when extending the Horizon JFR exclusion boundary.
std::string module_name(const std::string& path) {
    InstallerZip jar(path);
    std::map<std::string, std::string> attributes;
    if (jar.has("META-INF/MANIFEST.MF")) {
        std::istringstream stream(jar.read("META-INF/MANIFEST.MF", 65536));
        std::string line, key;
        while (std::getline(stream, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) break;
            if (line.front() == ' ') {
                if (key.empty()) bad("Malformed jar manifest continuation");
                attributes[key] += line.substr(1); continue;
            }
            const auto colon = line.find(": ");
            if (colon == std::string::npos) bad("Malformed jar manifest attribute");
            key = line.substr(0, colon);
            for (char& c : key) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
            if (!attributes.emplace(key, line.substr(colon + 2)).second) bad("Duplicate jar manifest attribute");
        }
    }
    auto entry = std::string("module-info.class");
    if (attributes["multi-release"] == "true") {
        for (int version = 21; version >= 9; --version) {
            auto candidate = "META-INF/versions/" + std::to_string(version) + "/module-info.class";
            if (jar.has(candidate)) { entry = std::move(candidate); break; }
        }
    }
    std::string name;
    if (jar.has(entry)) {
        auto bytes = jar.read(entry, 1024 * 1024);
        std::size_t at = 0;
        auto take = [&](unsigned count) -> std::uint32_t {
            if (count > bytes.size() - at) bad("Truncated module descriptor");
            std::uint32_t value = 0;
            while (count--) value = (value << 8) | static_cast<unsigned char>(bytes[at++]);
            return value;
        };
        auto skip = [&](std::uint32_t count) {
            if (count > bytes.size() - at) bad("Truncated module descriptor attribute");
            at += count;
        };
        if (take(4) != 0xcafebabe) bad("Invalid module descriptor classfile");
        skip(4);
        const auto size = take(2);
        std::vector<std::string> strings(size);
        std::vector<std::uint32_t> modules(size);
        for (std::uint32_t n = 1; n < size; ++n) {
            auto tag = take(1);
            switch (tag) {
                case 1: {
                    auto length = take(2); if (length > bytes.size() - at) bad("Truncated module descriptor string");
                    strings[n] = bytes.substr(at, length); skip(length); break;
                }
                case 3: case 4: case 9: case 10: case 11: case 12: case 17: case 18: skip(4); break;
                case 5: case 6: skip(8); if (++n >= size) bad("Invalid module constant pool"); break;
                case 7: case 8: case 16: case 20: skip(2); break;
                case 15: skip(3); break;
                case 19: modules[n] = take(2); break;
                default: bad("Unsupported module descriptor constant pool tag");
            }
        }
        skip(6); skip(take(2) * 2);
        for (unsigned group = 0; group < 2; ++group) {
            auto members = take(2);
            for (std::uint32_t n = 0; n < members; ++n) {
                skip(6); auto count = take(2);
                for (std::uint32_t j = 0; j < count; ++j) { skip(2); skip(take(4)); }
            }
        }
        auto count = take(2);
        for (std::uint32_t n = 0; n < count; ++n) {
            auto key = take(2); auto length = take(4);
            if (key >= strings.size() || length > bytes.size() - at) bad("Invalid module attribute");
            if (strings[key] == "Module") {
                if (length < 2) bad("Truncated module name");
                auto module = take(2);
                if (module >= modules.size() || !modules[module] || modules[module] >= strings.size()) bad("Invalid module name reference");
                name = strings[modules[module]]; skip(length - 2);
            } else skip(length);
        }
    } else name = attributes["automatic-module-name"];
    if (name.empty() || name.size() > 256 || name.front() == '.' || name.back() == '.' || name.find("..") != std::string::npos ||
        name.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_.$") != std::string::npos)
        throw Error("prepare_module_unsupported", "A module-path jar lacks an explicit supported module name");
    if (name == "jdk.jfr" || name == "java.instrument")
        throw Error("prepare_module_unsupported", "The selected loader requires an unavailable Horizon module");
    return name;
}

std::string property_escape(const std::string& value) {
    std::string out;
    for (unsigned char c : value) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '=': case ':': case '#': case '!': case ' ': out += '\\'; out += static_cast<char>(c); break;
            default: if (c < 32 || c == 127) bad("Unsupported control byte in processor plan"); out += static_cast<char>(c);
        }
    }
    return out;
}
std::map<std::string, std::string> properties(const std::string& input) {
    if (input.size() > 1024 * 1024) bad("Processor completion exceeds its size limit");
    auto unescape = [](const std::string& raw) {
        std::string out;
        for (std::size_t i = 0; i < raw.size(); ++i) {
            if (raw[i] != '\\') { out += raw[i]; continue; }
            if (++i == raw.size()) bad("Truncated processor completion escape");
            char c = raw[i];
            if (c == 'n') out += '\n'; else if (c == 'r') out += '\r'; else if (c == 't') out += '\t';
            else if (c == 'f') out += '\f';
            else if (c == 'u') {
                if (i + 4 >= raw.size()) bad("Truncated Unicode property escape");
                unsigned code = 0;
                for (int n = 0; n < 4; ++n) {
                    char h = raw[++i];
                    unsigned digit = h >= '0' && h <= '9' ? h - '0' : h >= 'a' && h <= 'f' ? h - 'a' + 10 : h >= 'A' && h <= 'F' ? h - 'A' + 10 : 16;
                    if (digit == 16) bad("Invalid Unicode property escape");
                    code = (code << 4) | digit;
                }
                if (!code || (code >= 0xd800 && code <= 0xdfff)) bad("Unsupported Unicode property value");
                if (code < 0x80) out += static_cast<char>(code);
                else if (code < 0x800) { out += static_cast<char>(0xc0 | (code >> 6)); out += static_cast<char>(0x80 | (code & 63)); }
                else { out += static_cast<char>(0xe0 | (code >> 12)); out += static_cast<char>(0x80 | ((code >> 6) & 63)); out += static_cast<char>(0x80 | (code & 63)); }
            } else out += c;
        }
        return out;
    };
    std::map<std::string, std::string> out;
    std::istringstream stream(input);
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        auto begin = line.find_first_not_of(" \t\f");
        if (begin == std::string::npos || line[begin] == '#' || line[begin] == '!') continue;
        bool escaped = false; std::size_t at = begin;
        for (; at < line.size(); ++at) {
            char c = line[at];
            if (!escaped && (c == '=' || c == ':' || c == ' ' || c == '\t')) break;
            escaped = !escaped && c == '\\';
            if (c != '\\') escaped = false;
        }
        auto key = unescape(line.substr(begin, at - begin));
        auto value = line.find_first_not_of(" \t=:" , at);
        if (!out.emplace(key, value == std::string::npos ? "" : unescape(line.substr(value))).second)
            bad("Duplicate processor completion field");
    }
    return out;
}
std::string owned_native_path(const std::string& root, const std::string& java) {
    const auto base = java_path(root);
    if (java.compare(0, base.size() + 1, base + "/") != 0 || !validRelativePath(java.substr(base.size() + 1)))
        throw Error("prepare_processor_path", "Processor path leaves owned storage");
    return joinPath(root, java.substr(base.size() + 1));
}
struct ProcessorOutput { std::string path, sha1; };
struct ProcessorStep {
    std::string jar;
    std::vector<std::string> classpath, args;
    std::vector<ProcessorOutput> outputs;
};
struct ForgeResult {
    Json version;
    bool complete = false;
    LaunchPlan helper;
};

class Preparation {
public:
    Preparation(Storage& storage, HttpClient& http, ProgressCallback report, const std::atomic_bool* stopped, bool force = false)
        : storage_(storage), http_(http), progress_(std::move(report)), stopped_(stopped), verifier_(storage.root(), force, stopped) {}
    bool verified(const std::string& path, const Download& file, const std::atomic_bool*, bool allow_receipt = true) {
        auto stamp = verifier_.inspect(path);
        return verifier_.check(path, file, stamp, allow_receipt);
    }
    std::string hash_file(const std::string& path, const Hashes& expected = {}) { return verifier_.hash_file(path, expected); }
    void finish(LaunchPlan& plan, json_t* stored) { verifier_.finish(plan, stored); }
    void status(const std::string& stage, const std::string& detail, std::uint64_t done = 0, std::uint64_t total = 0) {
        cancel(stopped_);
        if (progress_) progress_({stage, detail, done, total, 0, 0});
    }
    std::string get(const std::string& url, std::uint64_t max = metadata_limit) {
        status("metadata", "Reading official version metadata");
        HttpRequest request;
        request.url = https(url); request.max_bytes = max;
        request.headers["Accept"] = "application/json, text/plain";
        auto result = http_.request(request, stopped_);
        cancel(stopped_);
        if (result.status != 200) throw Error("prepare_http", "Official metadata returned HTTP " + std::to_string(result.status));
        if (result.body.size() > max) bad("Metadata exceeds its declared budget");
        return result.body;
    }
    Json cached_json(const std::string& path, const std::string& url, const std::string& sha1 = {}, bool refresh = false) {
        createDirectories(parent(path));
        if (!refresh && pathExists(path)) {
            auto contents = readTextFile(path, metadata_limit);
            if (sha1.empty() || digest(contents, MBEDTLS_MD_SHA1) == sha1) return parse(contents);
        }
        auto contents = get(url);
        if (!sha1.empty() && digest(contents, MBEDTLS_MD_SHA1) != sha1) throw Error("prepare_hash", "Official version metadata failed SHA1 verification");
        auto json = parse(contents);
        atomicWriteText(path, contents);
        return json;
    }
    Json minecraft(const std::string& version) {
        // An immutable seed can be installed with the runtime's own SHA256
        // manifest, allowing the already-installed base game to prepare offline.
        auto seed = joinPath(storage_.root(), "runtime/mineswitch/metadata/" + version + ".json");
        if (directory_exists(parent(seed)) && pathExists(seed)) {
            auto value = parse(readTextFile(seed, metadata_limit));
            if (string(value.get(), "id") != version) bad("Bundled Minecraft metadata names another version");
            return value;
        }
        auto index_path = joinPath(storage_.root(), "cache/metadata/minecraft-manifest.json");
        auto index = cached_json(index_path, manifest_url);
        for (int attempt = 0; attempt < 2; ++attempt) {
            auto versions = array(member(index.get(), "versions"), "version manifest");
            std::size_t i; json_t* entry;
            json_array_foreach(versions, i, entry) {
                if (string(object(entry, "version"), "id") != version) continue;
                auto hash = hex(string(entry, "sha1"), 40);
                auto path = joinPath(storage_.root(), "cache/metadata/versions/" + version + ".json");
                auto value = cached_json(path, string(entry, "url"), hash);
                if (string(value.get(), "id") != version) bad("Minecraft metadata names another version");
                return value;
            }
            if (!attempt) index = cached_json(index_path, manifest_url, {}, true);
        }
        throw Error("prepare_version_missing", "Requested Minecraft version is not in the official manifest");
    }
    Json loader(const GameSpec& game) {
        if (game.loader.kind == LoaderKind::Vanilla) return Json();
        std::string type, base;
        if (game.loader.kind == LoaderKind::Fabric) { type = "fabric"; base = "https://meta.fabricmc.net/v2/versions/loader/"; }
        else if (game.loader.kind == LoaderKind::Quilt) { type = "quilt"; base = "https://meta.quiltmc.org/v3/versions/loader/"; }
        else throw Error("prepare_loader_unsupported", "This loader still requires its real installer/processor implementation");
        auto version = identifier(game.loader.version, "loader version");
        auto path = joinPath(storage_.root(), "cache/metadata/loaders/" + type + "/" + game.minecraft_version + "/" + version + ".json");
        auto result = cached_json(path, base + game.minecraft_version + "/" + version + "/profile/json");
        if (string(result.get(), "inheritsFrom") != game.minecraft_version) bad("Loader profile targets another Minecraft version");
        return result;
    }
    std::string artifact(Download file, const std::string& target, const std::string& legacy = {}, bool canonical = false,
                         const ProgressCallback& observer = {}) {
        auto file_status = [&](const char* stage) {
            if (observer) {
                cancel(stopped_);
                observer({stage, file.file_name, 0, 0, 0, 0});
            } else status(stage, file.file_name);
        };
        file_status("verify");
        if (file.hashes.empty()) throw Error("prepare_hash", "Artifact has no published checksum");
        file.max_bytes = std::min(file.max_bytes, artifact_limit);
        if (file.bytes > file.max_bytes) throw Error("prepare_size", "Artifact exceeds the preparation budget");
        verifier_.directory(parent(target), true);
        auto backup = target + ".old";
        auto target_stamp = verifier_.inspect(target);
        auto backup_stamp = verifier_.inspect(backup);
        if (!target_stamp.exists && backup_stamp.exists) {
            if (::rename(backup.c_str(), target.c_str())) throw Error("prepare_cache", "Cannot restore cached artifact backup");
            target_stamp = verifier_.inspect(target); backup_stamp.exists = false;
        }
        if (verifier_.check(target, file, target_stamp)) {
            if (backup_stamp.exists) unlink_owned(backup);
            record(target, file); return target;
        }
        bool reusable = !legacy.empty() && directory_exists(parent(legacy)) && verified(legacy, file, stopped_);
        if (reusable && !canonical) { record(legacy, file); return legacy; }
        auto free = storage_.availableBytes();
        if (free <= free_margin || (file.bytes && file.bytes > free - free_margin))
            throw Error("prepare_space", "Insufficient SD space to prepare game artifacts");
        file.max_bytes = std::min(file.max_bytes, free - free_margin);
        auto part = joinPath(storage_.root(), "transactions/prepare-" + newId() + ".part");
        try {
            file_status("download");
            if (reusable) copy_verified_input(legacy, part);
            else http_.download(file, part, observer ? observer : progress_, stopped_);
            cancel(stopped_);
            if (!verified(part, file, stopped_, false)) throw Error("prepare_hash", "Downloaded artifact failed verification");
            int fd = ::open(part.c_str(), O_WRONLY);
            if (fd < 0) throw Error("prepare_file", "Cannot flush prepared artifact");
            syncAndCloseFile(fd, part);
            commit_binary(part, target);
            verifier_.promoted(part, target, file);
        } catch (...) { if (pathExists(part)) unlink_owned(part); throw; }
        record(target, file);
        return target;
    }
    std::string checksum(const std::string& artifact_url) {
        auto checksum_url = artifact_url + ".sha1";
        auto checksum_path = joinPath(storage_.root(), "cache/metadata/checksums/" + digest(checksum_url, MBEDTLS_MD_SHA256) + ".sha1");
        createDirectories(parent(checksum_path));
        auto value = pathExists(checksum_path) ? readTextFile(checksum_path, 1024) : get(checksum_url, 1024);
        auto begin = value.find_first_not_of(" \r\n\t");
        if (begin == std::string::npos) bad("Missing Maven checksum");
        auto end = value.find_first_of(" \r\n\t", begin);
        auto result = hex(value.substr(begin, end == std::string::npos ? end : end - begin), 40);
        if (!pathExists(checksum_path)) atomicWriteText(checksum_path, result + "\n");
        return result;
    }
    std::string library(const json_t* item, const Coordinate& coordinate, bool from_loader, bool canonical = false) {
        auto downloads = member(item, "downloads");
        auto details = json_is_object(downloads) ? member(downloads, "artifact") : nullptr;
        Download file;
        file.path = coordinate.path;
        file.file_name = coordinate.artifact + "-" + coordinate.version + ".jar";
        if (details) {
            object(details, "library artifact");
            auto path = string(details, "path", false);
            if (!path.empty() && path != coordinate.path) bad("Library artifact path does not match its Maven identity");
            file.bytes = integer(details, "size"); file.hashes = published_hashes(details);
            auto url = string(details, "url", false);
            if (url.empty()) {
                auto output = joinPath(storage_.root(), "cache/libraries/" + coordinate.path);
                if (!generated_.count(output) || !verified(output, file, stopped_, false))
                    throw Error("prepare_processor_output", "Required generated loader artifact is not verified");
                record(output, file); return output;
            }
            file.urls = {https(url)};
        } else {
            if (!from_loader) bad("A Minecraft library has no artifact metadata");
            auto repository = https(string(item, "url"));
            if (repository.back() != '/') repository += '/';
            file.urls = {repository + coordinate.path};
            file.bytes = integer(item, "size", false); file.hashes = published_hashes(item);
            if (file.hashes.empty()) {
                file.hashes.sha1 = checksum(file.urls.front());
            }
        }
        return artifact(file, joinPath(storage_.root(), "cache/libraries/" + coordinate.path),
                        joinPath(storage_.root(), "minecraft/libraries/" + coordinate.path), canonical);
    }
    std::string assets(const json_t* index_info) {
        object(index_info, "asset index");
        auto id = identifier(string(index_info, "id"), "asset index id");
        // Reuse the complete existing content-addressed asset tree. New versions
        // add missing hashes; no existing game-data bundle is retransmitted.
        auto root = joinPath(storage_.root(), "minecraft/assets");
        verifier_.directory(joinPath(root, "indexes"), true); verifier_.directory(joinPath(root, "objects"), true);
        Download index;
        index.file_name = id + ".json"; index.bytes = integer(index_info, "size");
        index.hashes.sha1 = hex(string(index_info, "sha1"), 40);
        index.urls = {https(string(index_info, "url"))}; index.max_bytes = metadata_limit;
        auto index_path = artifact(index, joinPath(root, "indexes/" + id + ".json"));
        auto parsed = parse(readTextFile(index_path, metadata_limit));
        if (json_is_true(member(parsed.get(), "virtual")) || json_is_true(member(parsed.get(), "map_to_resources")))
            throw Error("prepare_asset_layout", "Legacy virtual/resource-mapped assets are not supported by this Java21 profile");
        auto objects = object(member(parsed.get(), "objects"), "asset objects");
        if (json_object_size(objects) > 100000) bad("Too many asset objects");
        struct Asset { std::uint64_t bytes; std::string name; };
        std::map<std::string, Asset> expected;
        const char* key; json_t* value;
        json_object_foreach(const_cast<json_t*>(objects), key, value) {
            auto hash = hex(string(object(value, "asset"), "hash"), 40);
            auto size = integer(value, "size");
            auto previous = expected.emplace(hash, Asset{size, key});
            if (!previous.second && previous.first->second.bytes != size) bad("One asset hash has conflicting sizes");
        }
        std::uint64_t done = 0, total = expected.size();
        status("assets", "Verifying Minecraft resources", done, total);
        for (const auto& entry : expected) {
            Download file;
            auto path = entry.first.substr(0, 2) + "/" + entry.first;
            file.file_name = entry.first; file.bytes = entry.second.bytes; file.hashes.sha1 = entry.first;
            file.urls = {"https://resources.download.minecraft.net/" + path};
            ProgressCallback resource_progress;
            if (progress_) {
                resource_progress = [&, name = entry.second.name](const Progress& update) {
                    const char* detail = update.stage == "download" ? "Downloading Minecraft resources: " : "Verifying Minecraft resources: ";
                    // A transfer's byte/file counters are local to that file.
                    // Keep the outer count at fully verified resource objects.
                    status("assets", detail + name, done, total);
                };
            }
            artifact(file, joinPath(root, "objects/" + path), {}, false, resource_progress);
            ++done;
            status("assets", "Verified Minecraft resources", done, total);
        }
        status("assets", "Minecraft resources verified", done, total);
        return root;
    }
    void record(const std::string& path, const Download& file) {
        Json value(json_object());
        set_string(value.get(), "path", path);
        set_string(value.get(), "sha1", file.hashes.sha1); set_string(value.get(), "sha256", file.hashes.sha256);
        set_string(value.get(), "sha512", file.hashes.sha512); set_string(value.get(), "md5", file.hashes.md5);
        json_object_set_new(value.get(), "bytes", json_integer(static_cast<json_int_t>(verifier_.size(path))));
        if (json_array_append(receipt_.get(), value.get())) throw Error("prepare_memory", "Cannot record artifact verification");
    }
    json_t* receipt() const { return receipt_.get(); }
    ForgeResult forge(const Instance&, const Settings&, const std::string& client_path);
    static bool directory_exists(const std::string& path) {
        struct stat info{};
        if (::lstat(path.c_str(), &info)) {
            if (errno == ENOENT) return false;
            throw Error("prepare_file", "Cannot inspect preparation directory");
        }
        requireSafePath(path);
        return S_ISDIR(info.st_mode);
    }
private:
    Storage& storage_; HttpClient& http_; ProgressCallback progress_; const std::atomic_bool* stopped_;
    Json receipt_{json_array()};
    std::set<std::string> generated_;
    ArtifactVerifier verifier_;
};

ForgeResult Preparation::forge(const Instance& instance, const Settings& settings, const std::string& client_path) {
    const bool neo = instance.game.loader.kind == LoaderKind::NeoForge;
    const auto kind = std::string(neo ? "neoforge" : "forge");
    const auto mc = identifier(instance.game.minecraft_version, "Minecraft version");
    const auto version = identifier(instance.game.loader.version, "loader version");
    const auto release = neo ? version : mc + "-" + version;
    const auto installer_coordinate = coordinate((neo ? "net.neoforged:neoforge:" : "net.minecraftforge:forge:") + release + ":installer");
    const auto url = std::string(neo ? "https://maven.neoforged.net/releases/" : "https://maven.minecraftforge.net/") + installer_coordinate.path;
    const auto cache = joinPath(storage_.root(), "cache/installers/" + kind + "/" + mc + "/" + version);
    const auto preparation = joinPath(instance.root, "preparation");
    const auto libraries_root = joinPath(storage_.root(), "cache/libraries");
    createDirectories(preparation); createDirectories(libraries_root);
    const auto plan_path = joinPath(preparation, "processor-plan.properties");
    const auto completion_path = joinPath(preparation, "completion.properties");
    const auto tools_path = joinPath(storage_.root(), "runtime/mineswitch/launcher-tools.jar");
    if (!directory_exists(parent(tools_path)) || !pathExists(tools_path) || !regular_size(tools_path))
        throw Error("prepare_runtime_missing", "The installed runtime lacks the Java21 installer processor runner");
    Download installer;
    installer.file_name = kind + "-" + version + "-installer.jar";
    installer.urls = {url}; installer.hashes.sha1 = checksum(url);
    auto installer_path = artifact(installer, joinPath(cache, "installer.jar"));
    InstallerZip zip(installer_path);
    auto profile = parse(zip.read("install_profile.json"));
    if (integer(profile.get(), "spec") != 1 || string(profile.get(), "minecraft") != mc)
        bad("Installer profile schema or Minecraft version does not match the selected instance");
    const auto expected_id = neo ? "neoforge-" + version : mc + "-forge-" + version;
    if (string(profile.get(), "version") != expected_id) bad("Installer identifies a different loader release");
    auto version_entry = string(profile.get(), "json");
    if (version_entry.front() == '/') version_entry.erase(0, 1);
    ForgeResult result;
    result.version = parse(zip.read(version_entry));
    if (string(result.version.get(), "inheritsFrom") != mc || string(result.version.get(), "id") != string(profile.get(), "version"))
        bad("Installer launch profile does not match its parent Minecraft version");

    std::map<std::string, const json_t*> described;
    for (const auto* owner : {profile.get(), result.version.get()}) {
        auto list = array(member(owner, "libraries"), "installer libraries");
        std::size_t i; json_t* entry;
        json_array_foreach(list, i, entry) {
            object(entry, "installer library");
            auto coord = coordinate(string(entry, "name"), false);
            auto found = described.emplace(coord.path, entry);
            if (!found.second) {
                const auto* before = member(member(found.first->second, "downloads"), "artifact");
                const auto* after = member(member(entry, "downloads"), "artifact");
                if (!before || !after || string(before, "sha1", false) != string(after, "sha1", false) ||
                    integer(before, "size", false) != integer(after, "size", false))
                    bad("Installer libraries disagree about a Maven artifact");
            }
        }
    }

    std::map<std::string, std::string> protected_inputs;
    auto protect = [&](const std::string& path) {
        owned_native_path(storage_.root(), java_path(path));
        if (!protected_inputs.count(path)) protected_inputs.emplace(path, verifier_.fingerprint(path));
    };
    protect(client_path); protect(installer_path); protect(tools_path);
    std::map<std::string, Coordinate> references;
    std::map<std::string, std::string> values{
        {"SIDE", "client"}, {"ROOT", java_path(joinPath(preparation, "work"))},
        {"LIBRARY_DIR", java_path(libraries_root)}, {"MINECRAFT_JAR", java_path(client_path)},
        {"MINECRAFT_VERSION", mc}, {"INSTALLER", java_path(installer_path)}
    };
    std::set<std::string> resolving;
    const auto* data = object(member(profile.get(), "data"), "installer data");
    std::function<std::string(const std::string&, unsigned)> resolve;
    std::function<std::string(std::string, unsigned)> substitute;
    substitute = [&](std::string value, unsigned depth) -> std::string {
        if (depth > 32 || value.size() > 16384) bad("Installer variable expansion exceeds its limit");
        if (value.size() >= 2 && value.front() == '\'' && value.back() == '\'') return value.substr(1, value.size() - 2);
        if (value.size() >= 2 && value.front() == '[' && value.back() == ']') {
            auto coord = coordinate(value.substr(1, value.size() - 2), false);
            references.emplace(coord.path, coord);
            return java_path(joinPath(libraries_root, coord.path));
        }
        std::size_t pos = 0;
        while ((pos = value.find('{', pos)) != std::string::npos) {
            auto end = value.find('}', pos + 1);
            if (end == std::string::npos) bad("Unterminated installer variable");
            auto replacement = resolve(value.substr(pos + 1, end - pos - 1), depth + 1);
            value.replace(pos, end - pos + 1, replacement);
            pos += replacement.size();
            if (value.size() > 16384) bad("Installer argument exceeds its limit");
        }
        if (value.find('\0') != std::string::npos || value.find_first_of("\r\n") != std::string::npos)
            bad("Invalid control byte in installer argument");
        return value;
    };
    resolve = [&](const std::string& name, unsigned depth) -> std::string {
        auto known = values.find(name);
        if (known != values.end()) return known->second;
        if (depth > 32 || !resolving.insert(name).second) bad("Cyclic installer data variable");
        const auto* definition = object(member(data, name.c_str()), "installer variable");
        auto value = string(definition, "client");
        if (!value.empty() && value.front() == '/') {
            auto entry = value.substr(1);
            if (!validRelativePath(entry)) bad("Invalid embedded installer path");
            auto contents = zip.read(entry, 64 * 1024 * 1024);
            auto path = joinPath(cache, "resources/" + entry);
            createDirectories(parent(path));
            Hashes expected; expected.sha256 = digest(contents, MBEDTLS_MD_SHA256);
            Download embedded; embedded.hashes = expected; embedded.bytes = contents.size(); embedded.max_bytes = 64 * 1024 * 1024;
            if (!verified(path, embedded, stopped_)) atomicWriteText(path, contents);
            verifier_.inspect(path);
            record(path, embedded); protect(path); value = java_path(path);
        } else value = substitute(value, depth + 1);
        resolving.erase(name); values.emplace(name, value); return value;
    };

    auto processor_list = array(member(profile.get(), "processors"), "installer processors");
    if (json_array_size(processor_list) > 128) bad("Too many installer processors");
    std::vector<ProcessorStep> steps;
    std::map<std::string, ProcessorOutput> final_outputs;
    std::map<std::string, std::string> stage_paths;
    std::set<std::string> output_identities;
    auto lowercase = [](std::string text) {
        for (char& c : text) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        return text;
    };
    auto output = [&](const std::string& path, const std::string& hash) -> ProcessorOutput {
        auto native = owned_native_path(storage_.root(), path);
        const auto prefix = java_path(libraries_root) + "/";
        if (path.compare(0, prefix.size(), prefix) != 0 || !validRelativePath(path.substr(prefix.size())))
            throw Error("prepare_processor_path", "Generated artifacts must use separate Maven library paths");
        auto staged = joinPath(preparation, "generated/" + path.substr(prefix.size()));
        auto found = stage_paths.emplace(path, java_path(staged));
        if (found.second && !output_identities.insert(lowercase(path)).second)
            throw Error("prepare_processor_path", "Installer outputs collide on the SD filesystem");
        (void)native;
        return {path, hex(hash, 40)};
    };
    auto fetch_coordinate = [&](const std::string& name) {
        auto coord = coordinate(name, false);
        auto found = described.find(coord.path);
        if (found == described.end()) bad("Processor dependency lacks published artifact metadata");
        auto path = library(found->second, coord, true, true);
        protect(path); return java_path(path);
    };
    std::size_t i; json_t* item;
    json_array_foreach(processor_list, i, item) {
        object(item, "processor");
        if (const auto* sides = member(item, "sides")) {
            array(sides, "processor sides"); bool client = false;
            std::size_t j; json_t* side;
            json_array_foreach(sides, j, side) {
                if (!json_is_string(side)) bad("Installer side must be a string");
                if (!std::strcmp(json_string_value(side), "client")) client = true;
                else if (std::strcmp(json_string_value(side), "server")) bad("Unknown installer side");
            }
            if (!client) continue;
        }
        ProcessorStep step;
        step.jar = fetch_coordinate(string(item, "jar"));
        auto cp = array(member(item, "classpath"), "processor classpath");
        if (json_array_size(cp) > 512) bad("Processor classpath exceeds its limit");
        std::size_t j; json_t* entry;
        json_array_foreach(cp, j, entry) {
            if (!json_is_string(entry)) bad("Processor classpath item must be a Maven coordinate");
            step.classpath.push_back(fetch_coordinate(json_string_value(entry)));
        }
        auto args = array(member(item, "args"), "processor arguments");
        if (json_array_size(args) > 512) bad("Processor argument count exceeds its limit");
        json_array_foreach(args, j, entry) {
            if (!json_is_string(entry)) bad("Processor argument must be a string");
            step.args.push_back(substitute(std::string(json_string_value(entry), json_string_length(entry)), 0));
        }
        std::map<std::string, ProcessorOutput> outputs;
        if (const auto* published = member(item, "outputs")) {
            object(published, "processor outputs");
            const char* path; json_t* hash;
            json_object_foreach(const_cast<json_t*>(published), path, hash) {
                if (!json_is_string(hash)) bad("Processor output hash must be a string");
                auto declared = output(substitute(path, 0), substitute(json_string_value(hash), 0));
                if (declared.sha1.empty() || !outputs.emplace(declared.path, declared).second) bad("Invalid declared processor output");
            }
        }
        // Modern NeoForge omits the optional outputs map. These are the file
        // output switches used by its published client processor pipeline.
        for (std::size_t j = 0; j < step.args.size(); ++j) {
            if (step.args[j] != "--output" && step.args[j] != "--slim" && step.args[j] != "--extra") continue;
            if (++j >= step.args.size()) bad("Processor output flag lacks its path");
            auto inferred = output(step.args[j], {});
            outputs.emplace(inferred.path, inferred);
        }
        if (outputs.empty() || outputs.size() > 256) throw Error("prepare_processor_unsupported", "Installer processor has no supported bounded file outputs");
        for (const auto& pair : outputs) { step.outputs.push_back(pair.second); final_outputs[pair.first] = pair.second; }
        steps.push_back(std::move(step));
    }
    if (steps.empty() || final_outputs.empty() || final_outputs.size() > 4096) bad("Installer has no valid client processor pipeline");
    for (const auto* required : {"PATCHED", "MC_SRG", "MC_EXTRA"}) {
        if (!neo && std::strcmp(required, "PATCHED")) continue;
        if (!final_outputs.count(resolve(required, 0)))
            bad("Installer pipeline does not produce a required client loader artifact");
    }
    for (const auto& pair : references) {
        auto path = java_path(joinPath(libraries_root, pair.first));
        if (final_outputs.count(path)) continue;
        auto found = described.find(pair.first);
        if (found == described.end()) bad("Installer input lacks verified download metadata");
        protect(library(found->second, pair.second, true, true));
    }
    if (protected_inputs.size() > 64) bad("Installer immutable input count exceeds its limit");
    for (const auto& generated : final_outputs) {
        const auto key = lowercase(generated.first);
        for (const auto& immutable : protected_inputs) {
            const auto input_key = lowercase(java_path(immutable.first));
            if (key == input_key || key.compare(0, input_key.size() + 1, input_key + "/") == 0 ||
                input_key.compare(0, key.size() + 1, key + "/") == 0)
                throw Error("prepare_processor_path", "Generated artifact overlaps a verified input");
        }
    }
    std::ostringstream plan;
    auto field = [&](const std::string& key, const std::string& value) { plan << key << '=' << property_escape(value) << '\n'; };
    field("schema", "1"); field("storage_root", java_path(storage_.root()));
    field("completion_file", java_path(completion_path)); field("input.count", std::to_string(protected_inputs.size()));
    std::size_t input_number = 0;
    for (const auto& immutable : protected_inputs) {
        field("input." + std::to_string(input_number++) + ".path", java_path(immutable.first));
        plan << "# input-sha256=" << immutable.second << '\n';
    }
    field("processor.count", std::to_string(steps.size()));
    for (std::size_t n = 0; n < steps.size(); ++n) {
        auto& step = steps[n]; const auto prefix = "processor." + std::to_string(n) + ".";
        field(prefix + "jar", step.jar); field(prefix + "classpath.count", std::to_string(step.classpath.size()));
        for (std::size_t j = 0; j < step.classpath.size(); ++j) field(prefix + "classpath." + std::to_string(j), step.classpath[j]);
        field(prefix + "args.count", std::to_string(step.args.size()));
        for (std::size_t j = 0; j < step.args.size(); ++j) {
            auto found = stage_paths.find(step.args[j]);
            field(prefix + "args." + std::to_string(j), found == stage_paths.end() ? step.args[j] : found->second);
        }
        field(prefix + "outputs.count", std::to_string(step.outputs.size()));
        for (std::size_t j = 0; j < step.outputs.size(); ++j) {
            auto key = prefix + "outputs." + std::to_string(j) + ".";
            field(key + "path", stage_paths.at(step.outputs[j].path));
            if (!step.outputs[j].sha1.empty()) field(key + "sha1", step.outputs[j].sha1);
        }
    }
    auto text = plan.str();
    if (text.size() > 1024 * 1024) bad("Processor plan exceeds its size limit");
    if (!pathExists(plan_path) || readTextFile(plan_path, 1024 * 1024) != text) atomicWriteText(plan_path, text);

    std::map<std::string, Hashes> completed;
    if (pathExists(completion_path)) {
        try {
            const auto marker = properties(readTextFile(completion_path, 1024 * 1024));
            auto get_field = [&](const std::string& key) {
                auto found = marker.find(key);
                if (found == marker.end()) throw Error("prepare_processor_output", "Processor completion is incomplete");
                return found->second;
            };
            if (get_field("schema") != "1" || get_field("state") != "complete" || get_field("plan_file") != java_path(plan_path) ||
                get_field("plan_sha256") != digest(text, MBEDTLS_MD_SHA256) ||
                get_field("processor.count") != std::to_string(steps.size()) || get_field("output.count") != std::to_string(final_outputs.size()))
                throw Error("prepare_processor_output", "Processor completion belongs to a different preparation plan");
            std::map<std::string, ProcessorOutput> expected;
            for (const auto& pair : final_outputs) expected.emplace(stage_paths.at(pair.first), pair.second);
            for (std::size_t j = 0; j < expected.size(); ++j) {
                const auto prefix = "output." + std::to_string(j) + ".";
                auto path = get_field(prefix + "path");
                auto found = expected.find(path);
                if (found == expected.end()) throw Error("prepare_processor_output", "Unexpected completed processor output");
                Hashes hashes; hashes.sha256 = hex(get_field(prefix + "sha256"), 64); hashes.sha1 = found->second.sha1;
                if (hashes.sha256.empty() || (!hashes.sha1.empty() && get_field(prefix + "sha1") != hashes.sha1))
                    throw Error("prepare_processor_output", "Processor completion lacks the expected output checksums");
                verifier_.hash_file(owned_native_path(storage_.root(), path), hashes);
                if (!completed.emplace(found->second.path, hashes).second) throw Error("prepare_processor_output", "Duplicate completed processor output");
            }
        } catch (const Error& error) {
            if (error.code == "cancelled") throw;
            completed.clear();
        }
    }
    if (completed.size() == final_outputs.size()) {
        for (const auto& pair : completed) {
            Download file; file.hashes = pair.second; file.file_name = pair.first.substr(pair.first.find_last_of('/') + 1);
            auto staged = owned_native_path(storage_.root(), stage_paths.at(pair.first)); file.bytes = regular_size(staged);
            const auto canonical = owned_native_path(storage_.root(), pair.first);
            artifact(file, canonical, staged, true); generated_.insert(canonical);
        }
        result.complete = true;
        return result;
    }
    // Failed/interrupted processors only touch this instance's staging files.
    // Existing canonical libraries remain valid until all output hashes pass.
    const auto generated_root = joinPath(preparation, "generated");
    if (pathExists(generated_root)) removeOwnedTree(generated_root);
    for (const auto& pair : stage_paths) {
        auto path = owned_native_path(storage_.root(), pair.second); createDirectories(parent(path));
    }
    for (const auto& path : {completion_path, completion_path + ".tmp"}) if (pathExists(path)) unlink_owned(path);
    createDirectories(joinPath(preparation, "work")); createDirectories(joinPath(preparation, "tmp"));
    auto& helper = result.helper;
    helper.preparation_only = true; helper.main_class = "mineswitch.launcher.ProcessorEntry";
    helper.working_directory = joinPath(preparation, "work"); helper.game_arguments = {java_path(plan_path)};
    helper.jvm_options = {"-Xms128m", "-Xmx" + std::to_string(settings.heap_mib) + "m",
        "-Djava.class.path=" + java_path(tools_path), "-Djava.home=" + java_path(joinPath(storage_.root(), "runtime")),
        "-Duser.dir=" + java_path(helper.working_directory), "-Djava.io.tmpdir=" + java_path(joinPath(preparation, "tmp")),
        "-Djava.awt.headless=true", "-Djava.net.preferIPv4Stack=true", "-XX:+UseSerialGC", "-XX:CICompilerCount=2",
        "-XX:ReservedCodeCacheSize=96m", "-XX:InitialCodeCacheSize=8m", "-XX:-SegmentedCodeCache",
        std::string("--limit-modules=") + boot_modules, std::string("--add-modules=") + boot_modules};
    return result;
}

void defaults(const std::string& game) {
    createDirectories(game);
    auto options = joinPath(game, "options.txt");
    if (!pathExists(options)) atomicWriteText(options,
        "renderDistance:4\nsimulationDistance:5\nmaxFps:30\ngraphicsMode:0\nmipmapLevels:0\ncloudStatus:0\nfullscreen:false\npauseOnLostFocus:false\n");
}
} // namespace

namespace {
// Version of the stored launch plan layout; older plans always rebuild.
constexpr json_int_t kPlanFormat = 2;
int processor_count() {
#ifdef __SWITCH__
    u64 mask = 0;
    if (R_SUCCEEDED(svcGetInfo(&mask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0)) && mask) return __builtin_popcountll(mask);
#endif
    return 3; // Stock application core budget; host plans model it.
}
// Reuses a Ready instance's verified plan when every input that shapes it is
// unchanged, skipping thousands of SD metadata checks. Full validation and any
// changed setting take the normal path.
std::optional<LaunchPlan> reuse_plan(const std::string& root, const Instance& instance, const Settings& settings,
                                     const Profile& profile) {
    try {
        Json stored(readStoredJson(joinPath(root, "launch-plan.json"), kInstanceManifestMaxBytes));
        auto value = stored.get();
        auto text = [&](const char* key) {
            auto field = json_object_get(value, key);
            return json_is_string(field) ? std::string(json_string_value(field), json_string_length(field)) : std::string("\x01");
        };
        auto number = [&](const char* key) {
            auto field = json_object_get(value, key);
            return json_is_integer(field) ? json_integer_value(field) : -1;
        };
        auto list = [&](const char* key, std::vector<std::string>& out) {
            auto field = json_object_get(value, key);
            if (!json_is_array(field)) return false;
            std::size_t i; json_t* item;
            json_array_foreach(field, i, item) {
                if (!json_is_string(item)) return false;
                out.emplace_back(json_string_value(item), json_string_length(item));
            }
            return true;
        };
        if (!value || !json_is_object(value) || number("plan_format") != kPlanFormat ||
            !json_is_false(json_object_get(value, "preparation_only")) || text("profile_id") != profile.id ||
            text("username") != profile.username || text("minecraft_version") != instance.game.minecraft_version ||
            text("loader") != instance.game.loader.version || number("heap_mib") != settings.heap_mib ||
            number("full_initial_heap") != (settings.full_initial_heap ? 1 : 0) || text("jvm_args") != settings.jvm_args ||
            number("cpu_count") != processor_count())
            return std::nullopt;
        LaunchPlan plan;
        plan.main_class = text("main_class");
        plan.working_directory = text("working_directory");
        if (plan.working_directory != joinPath(root, "game") || plan.main_class.empty() ||
            !list("jvm_options", plan.jvm_options) || !list("game_arguments", plan.game_arguments) || plan.jvm_options.empty())
            return std::nullopt;
        if (!pathExists(plan.working_directory)) return std::nullopt;
        return plan;
    } catch (const Error&) {
        return std::nullopt;
    }
}
} // namespace

std::vector<std::string> parseJvmArguments(const std::string& text) {
    std::vector<std::string> result;
    std::istringstream input(text);
    std::string token;
    while (input >> token) {
        for (unsigned char c : token)
            if (c < 32 || c == 127) throw Error("jvm_args", "JVM arguments contain control characters");
        bool allowed = token.size() > 2 && token.size() <= 512 &&
            (token.compare(0, 2, "-D") == 0 || token.compare(0, 4, "-XX:") == 0 || token.compare(0, 4, "-Xss") == 0);
        if (!allowed) throw Error("jvm_args", "Only -D…, -XX:… and -Xss… are allowed: " + token.substr(0, 64));
        if (token.find("CodeCache") != std::string::npos && token.compare(0, 4, "-XX:") == 0 &&
            token.find("Reserved") != std::string::npos)
            throw Error("jvm_args", "The launcher sets the JIT cache size");
        if (token.compare(0, 4, "-XX:") == 0 && token.find("GC") != std::string::npos &&
            token.find("Use") != std::string::npos && token != "-XX:+UseSerialGC")
            throw Error("jvm_args", "Only SerialGC is available in this Java");
        result.push_back(token);
        if (result.size() > 64) throw Error("jvm_args", "Too many JVM arguments");
    }
    return result;
}

GamePreparer::GamePreparer(Storage& storage, HttpClient& http) : storage_(storage), http_(http) {}

LaunchPlan GamePreparer::prepare(Instance& instance, const Settings& settings, const Profile& profile,
                                 ProgressCallback progress, const std::atomic_bool* stopped, bool force_validation,
                                 bool reuse_ready_plan) {
    std::lock_guard<std::mutex> lock(preparation_mutex);
    cancel(stopped);
    if (profile.username.empty() || profile.username.size() > 16 ||
        profile.username.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_") != std::string::npos)
        throw Error("prepare_profile", "Offline username must contain 1..16 letters, digits or underscores");
    const auto heap_budget = currentHeapBudget();
    if (settings.heap_mib < 256 || settings.heap_mib > heap_budget.max_mib)
        throw Error("prepare_heap", "Java maximum in this mode: " + std::to_string(heap_budget.max_mib) + " MB");
    if (instance.id.empty() || instance.id.size() > 64 || instance.id.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-") != std::string::npos)
        throw Error("prepare_instance", "Invalid instance identifier");
    auto expected_root = joinPath(storage_.root(), "instances/" + instance.id);
    if (!instance.root.empty() && instance.root != expected_root) throw Error("prepare_instance", "Instance is outside owned storage");
    instance.root = expected_root;
    if (instance.state == InstanceState::NeedsFiles) throw Error("prepare_missing_files", "Required pack files must be supplied before preparation");
    if (reuse_ready_plan && !force_validation && instance.state == InstanceState::Ready) {
        const auto started = std::chrono::steady_clock::now();
        if (auto plan = reuse_plan(expected_root, instance, settings, profile)) {
            plan->preparation_summary = "PREPARATION_STATS fast_path=1 elapsed_ms=" + std::to_string(
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count());
            if (progress) { try { progress({"ready", instance.name, 1, 1, 1, 1}); } catch (...) {} }
            return *plan;
        }
    }
    if (instance.state == InstanceState::Unsupported || instance.game.loader.kind == LoaderKind::Unknown)
        throw Error("prepare_unsupported", "Instance has unsupported requirements");
    const bool installer_loader = instance.game.loader.kind == LoaderKind::Forge || instance.game.loader.kind == LoaderKind::NeoForge;
    auto mc_version = identifier(instance.game.minecraft_version, "Minecraft version");
    storage_.initialize(); createDirectories(instance.root);
    auto missing = joinPath(instance.root, "missing-files.json");
    Json missing_files(readStoredJson(missing, kInstanceManifestMaxBytes));
    if (missing_files) {
        auto& list = missing_files;
        if (!json_is_array(list.get()) || json_array_size(list.get())) {
            instance.state = InstanceState::NeedsFiles; instance.reason = "Required pack files are unresolved";
            storage_.saveInstance(instance);
            throw Error("prepare_missing_files", instance.reason);
        }
    }
    instance.state = InstanceState::NeedsPreparation;
    instance.reason = "Preparing verified game runtime and loader";
    storage_.saveInstance(instance);
    try {
        Preparation work(storage_, http_, progress, stopped, force_validation);
        auto metadata = work.minecraft(mc_version);
        auto java = object(member(metadata.get(), "javaVersion"), "javaVersion");
        if (integer(java, "majorVersion") != 21 || instance.game.java_major != 21)
            throw Error("prepare_java_unsupported", "This launcher uses Java21; the selected Minecraft metadata requires another runtime");
        Features features{{"has_custom_resolution", true}, {"is_demo_user", false}};
        auto base_libraries = array(member(metadata.get(), "libraries"), "libraries");
        std::size_t library_index; json_t* base_library;
        json_array_foreach(base_libraries, library_index, base_library) {
            object(base_library, "library");
            if (!allowed(member(base_library, "rules"), features)) continue;
            auto dependency = coordinate(string(base_library, "name"));
            if (dependency.group == "org.lwjgl" && dependency.version != lwjgl_version)
                throw Error("prepare_lwjgl_unsupported", "Selected Minecraft needs another native LWJGL ABI");
        }
        auto runtime = joinPath(storage_.root(), "runtime");
        if (!Preparation::directory_exists(parent(joinPath(runtime, "lib/modules"))))
            throw Error("prepare_runtime_missing", "Install the bundled Java21 Horizon runtime before preparation");
        regular_size(joinPath(runtime, "lib/modules"));
        auto game = joinPath(instance.root, "game");
        defaults(game); createDirectories(joinPath(instance.root, "natives")); createDirectories(joinPath(instance.root, "tmp"));
        auto publish = [&](LaunchPlan& plan) {
            cancel(stopped);
            Json stored(json_object());
            json_object_set_new(stored.get(), "schema", json_integer(1));
            json_object_set_new(stored.get(), "preparation_only", json_boolean(plan.preparation_only));
            set_string(stored.get(), "main_class", plan.main_class); set_string(stored.get(), "working_directory", plan.working_directory);
            set_string(stored.get(), "profile_id", profile.id); set_string(stored.get(), "minecraft_version", mc_version);
            set_strings(stored.get(), "jvm_options", plan.jvm_options); set_strings(stored.get(), "game_arguments", plan.game_arguments);
            // Inputs checked by reuse_plan(); profile_id and minecraft_version are stored above.
            json_object_set_new(stored.get(), "plan_format", json_integer(kPlanFormat));
            set_string(stored.get(), "username", profile.username);
            set_string(stored.get(), "loader", instance.game.loader.version);
            json_object_set_new(stored.get(), "heap_mib", json_integer(settings.heap_mib));
            json_object_set_new(stored.get(), "full_initial_heap", json_integer(settings.full_initial_heap ? 1 : 0));
            set_string(stored.get(), "jvm_args", settings.jvm_args);
            json_object_set_new(stored.get(), "cpu_count", json_integer(processor_count()));
            json_object_set(stored.get(), "verified_artifacts", work.receipt());
            work.finish(plan, stored.get());
            atomicWriteJson(joinPath(instance.root, "launch-plan.json"), stored.get());
            instance.state = plan.preparation_only ? InstanceState::NeedsPreparation : InstanceState::Ready;
            instance.reason = plan.preparation_only ? "Run the verified loader processors, then reopen the launcher" : "";
            instance.game.java_major = 21; storage_.saveInstance(instance);
        };
        auto client_info = object(member(object(member(metadata.get(), "downloads"), "downloads"), "client"), "client");
        Download client;
        client.file_name = "client.jar"; client.bytes = integer(client_info, "size");
        client.hashes.sha1 = hex(string(client_info, "sha1"), 40); client.urls = {https(string(client_info, "url"))};
        auto client_path = work.artifact(client, joinPath(storage_.root(), "cache/versions/" + mc_version + "/client.jar"),
                                         joinPath(storage_.root(), "minecraft/client.jar"));
        Json loader;
        if (installer_loader) {
            auto prepared = work.forge(instance, settings, client_path);
            if (!prepared.complete) { publish(prepared.helper); return prepared.helper; }
            loader = std::move(prepared.version);
        } else loader = work.loader(instance.game);
        LaunchPlan plan;
        plan.working_directory = game;
        plan.main_class = string(loader ? loader.get() : metadata.get(), "mainClass");
        if (plan.main_class.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_.$") != std::string::npos)
            bad("Invalid Java main class name");
        std::vector<std::string> classpath;
        for (const auto* name : {"lwjgl", "lwjgl-opengl", "lwjgl-openal"}) {
            auto patched = joinPath(runtime, std::string("mineswitch/libraries/") + name + "-" + lwjgl_version + "-horizon.jar");
            if (!Preparation::directory_exists(parent(patched)) || !pathExists(patched) || !regular_size(patched))
                throw Error("prepare_runtime_missing", "Required Horizon LWJGL overlay is not installed");
            Download file; file.max_bytes = artifact_limit;
            file.hashes.sha256 = work.hash_file(patched);
            work.record(patched, file); classpath.push_back(java_path(patched));
        }
        std::set<std::string> selected;
        auto libraries = [&](const json_t* owner, bool from_loader) {
            auto list = array(member(owner, "libraries"), "libraries");
            std::size_t i; json_t* entry;
            json_array_foreach(list, i, entry) {
                object(entry, "library");
                if (!allowed(member(entry, "rules"), features)) continue;
                auto artifact = coordinate(string(entry, "name"));
                if (!artifact.classifier.empty() && artifact.classifier.compare(0, 7, "natives") == 0) continue;
                if (artifact.group == "org.lwjgl") {
                    if (artifact.version != lwjgl_version) throw Error("prepare_lwjgl_unsupported", "Selected Minecraft needs another native LWJGL ABI");
                    if (artifact.artifact == "lwjgl" || artifact.artifact == "lwjgl-opengl" || artifact.artifact == "lwjgl-openal") continue;
                }
                if (!selected.insert(artifact.key).second) continue;
                classpath.push_back(java_path(work.library(entry, artifact, from_loader, installer_loader)));
            }
        };
        if (loader) libraries(loader.get(), true);
        libraries(metadata.get(), false);
        // Forge's generated client is a version library. NeoForge's FML loads
        // its generated SRG/extra/patched jars from libraryDirectory itself.
        if (!installer_loader) classpath.push_back(java_path(client_path));
        auto index = object(member(metadata.get(), "assetIndex"), "assetIndex");
        auto assets = work.assets(index);
        std::string cp;
        for (const auto& path : classpath) { if (!cp.empty()) cp += ';'; cp += path; }
        std::map<std::string, std::string> variables{
            {"auth_player_name", profile.username}, {"auth_uuid", offline_uuid(profile.username)},
            {"auth_access_token", "0"}, {"auth_session", "0"}, {"user_type", "legacy"},
            {"user_properties", "{}"}, {"clientid", ""}, {"auth_xuid", ""},
            {"version_name", mc_version}, {"version_type", string(metadata.get(), "type", false)},
            {"game_directory", java_path(game)}, {"assets_root", java_path(assets)},
            {"assets_index_name", string(index, "id")}, {"game_assets", java_path(assets)},
            {"natives_directory", java_path(joinPath(instance.root, "natives"))},
            {"library_directory", java_path(joinPath(storage_.root(), "cache/libraries"))},
            {"classpath", cp}, {"classpath_separator", ";"},
            {"launcher_name", "MineSwitch"}, {"launcher_version", "0.2.0"},
            {"resolution_width", "1280"}, {"resolution_height", "720"}
        };
        std::vector<std::string> jvm;
        auto vanilla_args = object(member(metadata.get(), "arguments"), "arguments");
        arguments(member(vanilla_args, "jvm"), features, variables, jvm);
        arguments(member(vanilla_args, "game"), features, variables, plan.game_arguments);
        if (loader) {
            auto args = member(loader.get(), "arguments");
            if (args) {
                object(args, "loader arguments");
                arguments(member(args, "jvm"), features, variables, jvm);
                arguments(member(args, "game"), features, variables, plan.game_arguments);
            }
        }
        std::set<std::string> module_names;
        for (std::size_t i = 0; i < jvm.size(); ++i) {
            auto option = jvm[i];
            if (option == "-cp" || option == "-classpath" || option == "--class-path") {
                if (++i >= jvm.size() || jvm[i] != cp) bad("Unrecognized classpath argument");
                continue;
            }
            if (option.compare(0, 11, "-javaagent:") == 0 || option.compare(0, 9, "-agentlib") == 0 || option.compare(0, 10, "-agentpath") == 0)
                throw Error("prepare_agent_unsupported", "Selected profile requires unavailable JVM instrumentation");
            if (option == "-p" || option == "--module-path" || option == "--add-modules" || option == "--add-opens" ||
                option == "--add-exports" || option == "--add-reads") {
                if (++i >= jvm.size() || jvm[i].empty()) bad("Missing JVM module option value");
                option = (option == "-p" ? "--module-path" : option) + "=" + jvm[i];
            }
            if (option.compare(0, 16, "--limit-modules=") == 0 || option == "--limit-modules")
                throw Error("prepare_module_unsupported", "Loader tries to replace the Horizon runtime module boundary");
            if (option.compare(0, 14, "--module-path=") == 0) {
                std::istringstream paths(option.substr(14)); std::string path;
                while (std::getline(paths, path, ';')) {
                    auto native = owned_native_path(storage_.root(), path);
                    regular_size(native); module_names.insert(module_name(native));
                }
                if (module_names.empty()) bad("Loader module path is empty");
            }
            plan.jvm_options.push_back(option);
        }
        if (instance.game.loader.kind == LoaderKind::Fabric && mc_version == "1.21.1" && companion_loader(instance.game.loader.version)) {
            auto companion = joinPath(runtime, "mineswitch/mineswitch-compat.jar");
            if (pathExists(companion)) {
                Download file; file.hashes.sha256 = work.hash_file(companion);
                work.record(companion, file);
                const std::string prefix = "-Dfabric.addMods=", path = java_path(companion);
                bool configured = false;
                for (auto& option : plan.jvm_options) if (option.compare(0, prefix.size(), prefix) == 0) {
                    configured = true;
                    const auto entries = ";" + option.substr(prefix.size()) + ";";
                    if (entries.find(";" + path + ";") == std::string::npos)
                        option += (option.size() == prefix.size() ? "" : ";") + path;
                }
                if (!configured) plan.jvm_options.push_back(prefix + path);
            }
        }
        auto logging = member(metadata.get(), "logging");
        auto log_client = logging ? member(logging, "client") : nullptr;
        if (log_client) {
            auto info = object(member(log_client, "file"), "logging file");
            auto name = identifier(string(info, "id"), "logging file id");
            Download config;
            config.file_name = name; config.bytes = integer(info, "size");
            config.hashes.sha1 = hex(string(info, "sha1"), 40); config.urls = {https(string(info, "url"))};
            auto path = work.artifact(config, joinPath(storage_.root(), "cache/logging/" + name), joinPath(storage_.root(), "minecraft/logging/" + name));
            auto log_variables = variables; log_variables["path"] = java_path(path);
            plan.jvm_options.push_back(expand(string(log_client, "argument"), log_variables));
        }
        std::string limited_modules = boot_modules;
        for (const auto& name : module_names) limited_modules += "," + name;
        for (const auto& option : std::vector<std::string>{
                "-Xms" + std::string(settings.full_initial_heap ? std::to_string(settings.heap_mib) : "256") + "m",
                "-Xmx" + std::to_string(settings.heap_mib) + "m", "-Djava.class.path=" + cp,
                "-Djava.home=" + java_path(runtime), "-Duser.dir=" + java_path(game),
                "-Djava.io.tmpdir=" + java_path(joinPath(instance.root, "tmp")),
                "-Djava.awt.headless=true", "-Djava.net.preferIPv4Stack=true", "-Dorg.lwjgl.system.allocator=system",
                "-Dio.netty.transport.noNative=true", "-XX:+UseSerialGC", "-XX:ReservedCodeCacheSize=96m",
                "-XX:InitialCodeCacheSize=8m", "-XX:-SegmentedCodeCache",
                // A fourth core (forwarder NPDM) gets a third compiler thread.
                std::string("-XX:CICompilerCount=") + (processor_count() >= 4 ? "3" : "2"),
                "-Dmineswitch.progress=true", "-Xlog:codecache=info",
                "--limit-modules=" + limited_modules, std::string("--add-modules=") + boot_modules})
            plan.jvm_options.push_back(option);
        for (auto& option : parseJvmArguments(settings.jvm_args)) plan.jvm_options.push_back(std::move(option));
        publish(plan);
        if (progress) { try { progress({"ready", instance.name, 1, 1, 1, 1}); } catch (...) {} }
        return plan;
    } catch (const Error& error) {
        if (error.code == "prepare_java_unsupported" || error.code == "prepare_lwjgl_unsupported" ||
            error.code == "prepare_agent_unsupported" || error.code == "prepare_loader_unsupported") instance.state = InstanceState::Unsupported;
        else instance.state = InstanceState::NeedsPreparation;
        instance.reason = error.what();
        try { storage_.saveInstance(instance); } catch (...) {}
        throw;
    }
}
} // namespace mineswitch
