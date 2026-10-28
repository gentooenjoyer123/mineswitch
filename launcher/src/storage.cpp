#include "mineswitch/storage.hpp"
#include "mineswitch/heap.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <limits>
#include <memory>
#include <mutex>
#include <set>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#ifdef __SWITCH__
#include <switch.h>
#else
#include <sys/statvfs.h>
#endif

namespace mineswitch {
namespace {
constexpr std::size_t kMaxPath = 4096;
constexpr std::size_t kMaxSettings = 32 * 1024;
constexpr std::size_t kMaxProfiles = 128 * 1024;
constexpr std::size_t kMaxInstance = 64 * 1024;
constexpr std::size_t kMaxProfilesCount = 256;
constexpr std::size_t kMaxInstancesCount = 4096;
std::mutex journal_mutex;

[[noreturn]] void ioError(const std::string& operation, const std::string& path,
                          int error = errno) {
    throw Error("storage_io", operation + " (" + path + "): " + std::strerror(error));
}
[[noreturn]] void invalidData(const std::string& field) {
    // Only schema field names enter this message, never persisted values or keys.
    throw Error("storage_schema", "Invalid stored field: " + field);
}

bool validUtf8(const std::string& s, bool reject_controls) {
    for (std::size_t i = 0; i < s.size();) {
        const auto first = static_cast<unsigned char>(s[i++]);
        std::uint32_t cp = 0;
        int extra = 0;
        if (first <= 0x7f) cp = first;
        else if (first >= 0xc2 && first <= 0xdf) { cp = first & 0x1f; extra = 1; }
        else if (first >= 0xe0 && first <= 0xef) { cp = first & 0x0f; extra = 2; }
        else if (first >= 0xf0 && first <= 0xf4) { cp = first & 0x07; extra = 3; }
        else return false;
        if (i + static_cast<std::size_t>(extra) > s.size()) return false;
        for (int j = 0; j < extra; ++j) {
            const auto next = static_cast<unsigned char>(s[i++]);
            if ((next & 0xc0) != 0x80) return false;
            cp = (cp << 6) | (next & 0x3f);
        }
        if ((extra == 1 && cp < 0x80) || (extra == 2 && cp < 0x800) ||
            (extra == 3 && cp < 0x10000) || cp > 0x10ffff ||
            (cp >= 0xd800 && cp <= 0xdfff) || cp == 0) return false;
        if (reject_controls && (cp < 0x20 || (cp >= 0x7f && cp <= 0x9f))) return false;
    }
    return true;
}

std::string normalizeAbsolute(const std::string& input) {
    std::string path = input;
    const std::size_t anchor = path.compare(0, 6, "sdmc:/") == 0 ? 6 :
                               (!path.empty() && path[0] == '/' ? 1 : 0);
    if (!anchor) throw Error("storage_path", "Storage path must be absolute or start with sdmc:/");
    while (path.size() > anchor && path.back() == '/') path.pop_back();
    if (path.size() > kMaxPath ||
        (path.size() > anchor && !validRelativePath(path.substr(anchor))))
        throw Error("storage_path", "Invalid storage path");
    return path;
}

std::string parentPath(const std::string& path) {
    const auto slash = path.find_last_of('/');
    if (slash == std::string::npos) throw Error("storage_path", "Storage path has no parent");
    if (slash == 0) return "/";
    if (slash == 5 && path.compare(0, 6, "sdmc:/") == 0) return "sdmc:/";
    return path.substr(0, slash);
}

template<typename Fn> void eachComponent(const std::string& input, Fn&& visit) {
    const auto path = normalizeAbsolute(input);
    const std::size_t anchor = path.compare(0, 6, "sdmc:/") == 0 ? 6 : 1;
    visit(path.substr(0, anchor), path.size() == anchor);
    std::size_t pos = anchor;
    while (pos < path.size()) {
        const auto slash = path.find('/', pos);
        const bool final = slash == std::string::npos;
        visit(path.substr(0, final ? path.size() : slash), final);
        if (final) break;
        pos = slash + 1;
    }
}

bool regularExists(const std::string& path) {
    requireSafePath(path, true);
    struct stat st {};
    if (::lstat(path.c_str(), &st) == 0) {
        if (!S_ISREG(st.st_mode)) throw Error("storage_path", "Expected a regular storage file: " + path);
        return true;
    }
    if (errno == ENOENT) return false;
    ioError("Cannot inspect file", path);
}

void removeFileIfPresent(const std::string& path) {
    if (regularExists(path) && ::unlink(path.c_str()) != 0) ioError("Cannot remove journal file", path);
}

void syncDirectory(const std::string& path) {
#ifndef __SWITCH__
    requireSafePath(path);
    int flags = O_RDONLY;
#ifdef O_DIRECTORY
    flags |= O_DIRECTORY;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    const int fd = ::open(path.c_str(), flags);
    if (fd < 0) ioError("Cannot open directory", path);
    const int result = ::fsync(fd);
    const int error = errno;
    const int close_result = ::close(fd);
    const int close_error = errno;
    // Some SD/FUSE filesystems do not support directory fsync.
    if (result != 0 && error != EINVAL && error != ENOTSUP && error != EROFS)
        ioError("Cannot sync directory", path, error);
    if (close_result != 0) ioError("Cannot close directory", path, close_error);
#else
    (void)path;
    // libnx devoptab does not expose directory descriptors. File data is fsynced
    // before renaming; recovery uses the surviving directory entries on SD.
#endif
}

void recoverJournal(const std::string& path) {
    const auto temporary = path + ".tmp";
    const auto backup = path + ".old";
    const bool current_exists = regularExists(path);
    const bool backup_exists = regularExists(backup);
    // A missing current file means the previous replacement was not committed.
    if (!current_exists && backup_exists) {
        if (::rename(backup.c_str(), path.c_str()) != 0) ioError("Cannot restore storage backup", path);
        syncDirectory(parentPath(path));
    }
    removeFileIfPresent(temporary);
    if (current_exists && backup_exists) removeFileIfPresent(backup);
}

bool validId(const std::string& id) {
    if (id.empty() || id.size() > 64) return false;
    for (const unsigned char c : id)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
    return true;
}
bool validUsername(const std::string& username) {
    if (username.empty() || username.size() > 16) return false;
    for (const unsigned char c : username)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_')) return false;
    return true;
}

struct JsonDelete { void operator()(json_t* value) const { json_decref(value); } };
using Json = std::unique_ptr<json_t, JsonDelete>;
Json checkedJson(json_t* value) {
    if (!value) throw Error("storage_memory", "Cannot allocate storage JSON");
    return Json(value);
}
void setValue(json_t* object, const char* field, json_t* value) {
    if (!value) throw Error("storage_memory", "Cannot allocate storage JSON");
    // The _new API consumes value on both success and failure.
    if (json_object_set_new(object, field, value) != 0)
        throw Error("storage_memory", "Cannot create storage JSON");
}
void setString(json_t* object, const char* field, const std::string& value,
               std::size_t maximum = 4096, bool nonempty = false) {
    if (value.size() > maximum || (nonempty && value.empty()) || !validUtf8(value, false))
        invalidData(field);
    setValue(object, field, json_stringn(value.data(), value.size()));
}
void checkFields(json_t* value, std::initializer_list<const char*> fields,
                  const char* context) {
    if (!json_is_object(value) || json_object_size(value) != fields.size()) invalidData(context);
    for (const auto field : fields)
        if (!json_object_get(value, field)) invalidData(field);
}
// Like checkFields, but later schema additions may be absent in older files.
void checkFields(json_t* value, std::initializer_list<const char*> fields,
                  std::initializer_list<const char*> optional, const char* context) {
    if (!json_is_object(value)) invalidData(context);
    std::size_t present = fields.size();
    for (const auto field : fields)
        if (!json_object_get(value, field)) invalidData(field);
    for (const auto field : optional)
        if (json_object_get(value, field)) ++present;
    if (json_object_size(value) != present) invalidData(context);
}
std::string getString(json_t* object, const char* field, std::size_t maximum = 4096,
                       bool nonempty = false) {
    json_t* value = json_object_get(object, field);
    if (!json_is_string(value)) invalidData(field);
    const auto length = json_string_length(value);
    if (length > maximum || (nonempty && length == 0)) invalidData(field);
    std::string result(json_string_value(value), length);
    if (!validUtf8(result, false)) invalidData(field);
    return result;
}
json_int_t getInteger(json_t* object, const char* field, json_int_t min, json_int_t max) {
    json_t* value = json_object_get(object, field);
    if (!json_is_integer(value)) invalidData(field);
    const json_int_t result = json_integer_value(value);
    if (result < min || result > max) invalidData(field);
    return result;
}
bool getBoolean(json_t* object, const char* field) {
    json_t* value = json_object_get(object, field);
    if (!json_is_boolean(value)) invalidData(field);
    return json_is_true(value);
}
Json readJson(const std::string& path, std::size_t maximum) {
    std::lock_guard<std::mutex> lock(journal_mutex);
    recoverJournal(path);
    if (!regularExists(path)) return Json();
    const auto text = readTextFile(path, maximum);
    json_error_t error {};
    Json result(json_loadb(text.data(), text.size(), JSON_REJECT_DUPLICATES, &error));
    if (!result) throw Error("storage_json", "Invalid storage JSON");
    return result;
}

const char* loaderName(LoaderKind kind) {
    switch (kind) {
        case LoaderKind::Vanilla: return "vanilla";
        case LoaderKind::Fabric: return "fabric";
        case LoaderKind::Quilt: return "quilt";
        case LoaderKind::Forge: return "forge";
        case LoaderKind::NeoForge: return "neoforge";
        case LoaderKind::Unknown: return "unknown";
    }
    invalidData("loader.kind");
}
LoaderKind parseLoader(const std::string& name) {
    if (name == "vanilla") return LoaderKind::Vanilla;
    if (name == "fabric") return LoaderKind::Fabric;
    if (name == "quilt") return LoaderKind::Quilt;
    if (name == "forge") return LoaderKind::Forge;
    if (name == "neoforge") return LoaderKind::NeoForge;
    if (name == "unknown") return LoaderKind::Unknown;
    invalidData("loader.kind");
}
const char* stateName(InstanceState state) {
    switch (state) {
        case InstanceState::Ready: return "ready";
        case InstanceState::NeedsFiles: return "needs_files";
        case InstanceState::NeedsPreparation: return "needs_preparation";
        case InstanceState::Unsupported: return "unsupported";
        case InstanceState::Broken: return "broken";
    }
    invalidData("state");
}
InstanceState parseState(const std::string& state) {
    if (state == "ready") return InstanceState::Ready;
    if (state == "needs_files") return InstanceState::NeedsFiles;
    if (state == "needs_preparation") return InstanceState::NeedsPreparation;
    if (state == "unsupported") return InstanceState::Unsupported;
    if (state == "broken") return InstanceState::Broken;
    invalidData("state");
}
Json gameJson(const GameSpec& game) {
    if (game.java_major < 8 || game.java_major > 99) invalidData("game.java_major");
    auto result = checkedJson(json_object());
    setString(result.get(), "minecraft_version", game.minecraft_version, 128);
    setValue(result.get(), "java_major", json_integer(game.java_major));
    auto loader = checkedJson(json_object());
    setString(loader.get(), "kind", loaderName(game.loader.kind), 16, true);
    setString(loader.get(), "version", game.loader.version, 128);
    setValue(result.get(), "loader", loader.release());
    return result;
}
GameSpec parseGame(json_t* value) {
    checkFields(value, {"minecraft_version", "loader", "java_major"}, "game");
    GameSpec game;
    game.minecraft_version = getString(value, "minecraft_version", 128);
    game.java_major = static_cast<int>(getInteger(value, "java_major", 8, 99));
    auto loader = json_object_get(value, "loader");
    checkFields(loader, {"kind", "version"}, "game.loader");
    game.loader.kind = parseLoader(getString(loader, "kind", 16, true));
    game.loader.version = getString(loader, "version", 128);
    return game;
}

void removeTree(const std::string& path, unsigned depth) {
    if (depth > 128) throw Error("storage_path", "Owned directory tree is too deep");
    struct stat st {};
    if (::lstat(path.c_str(), &st) != 0) {
        if (errno == ENOENT) return;
        ioError("Cannot inspect owned path", path);
    }
    if (!S_ISDIR(st.st_mode)) {
        // unlink removes a symlink itself and never follows its target.
        if (::unlink(path.c_str()) != 0) ioError("Cannot remove owned file", path);
        return;
    }
    DIR* directory = ::opendir(path.c_str());
    if (!directory) ioError("Cannot open owned directory", path);
    try {
        while (true) {
            errno = 0;
            dirent* entry = ::readdir(directory);
            if (!entry) {
                if (errno != 0) ioError("Cannot read owned directory", path);
                break;
            }
            const std::string name = entry->d_name;
            if (name == "." || name == "..") continue;
            removeTree(path + "/" + name, depth + 1);
        }
    } catch (...) {
        ::closedir(directory);
        throw;
    }
    if (::closedir(directory) != 0) ioError("Cannot close owned directory", path);
    if (::rmdir(path.c_str()) != 0) ioError("Cannot remove owned directory", path);
}
}

bool validRelativePath(const std::string& path) {
    if (path.empty() || path.size() > kMaxPath || !validUtf8(path, true)) return false;
    std::size_t start = 0;
    while (start < path.size()) {
        const auto slash = path.find('/', start);
        const auto end = slash == std::string::npos ? path.size() : slash;
        const auto size = end - start;
        if (!size || size > 255 || (size == 1 && path[start] == '.') ||
            (size == 2 && path[start] == '.' && path[start + 1] == '.') ||
            path[end - 1] == '.' || path[end - 1] == ' ') return false;
        for (std::size_t i = start; i < end; ++i)
            if (path[i] == '\\' || path[i] == ':' || path[i] == '*' ||
                path[i] == '?' || path[i] == '"' || path[i] == '<' ||
                path[i] == '>' || path[i] == '|') return false;
        if (slash == std::string::npos) return true;
        start = slash + 1;
    }
    return false;
}

std::string joinPath(const std::string& root, const std::string& relative) {
    if (!validRelativePath(relative)) throw Error("storage_path", "Invalid relative storage path");
    auto result = normalizeAbsolute(root);
    if (result.back() != '/') result += '/';
    result += relative;
    if (result.size() > kMaxPath) throw Error("storage_path", "Storage path is too long");
    return result;
}

void requireSafePath(const std::string& path, bool allow_missing_leaf) {
    eachComponent(path, [&](const std::string& component, bool final) {
        struct stat st {};
        if (::lstat(component.c_str(), &st) != 0) {
            if (errno == ENOENT && final && allow_missing_leaf) return;
            ioError("Cannot inspect storage path", component);
        }
        if (S_ISLNK(st.st_mode) || (!final && !S_ISDIR(st.st_mode)))
            throw Error("storage_path", "Storage paths cannot traverse symbolic links or files");
    });
}

bool pathExists(const std::string& path) {
    requireSafePath(path, true);
    struct stat st {};
    if (::lstat(path.c_str(), &st) == 0) return true;
    if (errno == ENOENT) return false;
    ioError("Cannot inspect storage path", path);
}

void createDirectories(const std::string& path) {
    eachComponent(path, [&](const std::string& component, bool) {
        struct stat st {};
        if (::lstat(component.c_str(), &st) != 0) {
            if (errno != ENOENT) ioError("Cannot inspect storage directory", component);
            if (::mkdir(component.c_str(), 0700) != 0 && errno != EEXIST)
                ioError("Cannot create storage directory", component);
            if (::lstat(component.c_str(), &st) != 0) ioError("Cannot inspect storage directory", component);
        }
        if (!S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode))
            throw Error("storage_path", "Storage directory is not a real directory");
    });
}

int openExclusiveFile(const std::string& path) {
    requireSafePath(path, true);
    int flags = O_WRONLY | O_CREAT | O_EXCL;
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    const int fd = ::open(path.c_str(), flags, 0600);
    if (fd < 0) ioError("Cannot create exclusive storage file", path);
    return fd;
}

void syncAndCloseFile(int fd, const std::string& path) {
    const int result = ::fsync(fd);
    const int error = errno;
    const int close_result = ::close(fd);
    const int close_error = errno;
    if (result != 0) ioError("Cannot sync storage file", path, error);
    if (close_result != 0) ioError("Cannot close storage file", path, close_error);
}

std::string readTextFile(const std::string& path, std::size_t maximum_bytes) {
    requireSafePath(path);
    int flags = O_RDONLY;
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    const int fd = ::open(path.c_str(), flags);
    if (fd < 0) ioError("Cannot open storage file", path);
    std::string result;
    try {
        struct stat st {};
        if (::fstat(fd, &st) != 0) ioError("Cannot inspect storage file", path);
        if (!S_ISREG(st.st_mode) || st.st_size < 0 ||
            static_cast<std::uintmax_t>(st.st_size) > maximum_bytes)
            throw Error("storage_size", "Storage file is not regular or exceeds its size limit");
        result.reserve(static_cast<std::size_t>(st.st_size));
        std::array<char, 8192> buffer {};
        while (true) {
            const auto amount = ::read(fd, buffer.data(), buffer.size());
            if (amount < 0) {
                if (errno == EINTR) continue;
                ioError("Cannot read storage file", path);
            }
            if (!amount) break;
            if (static_cast<std::size_t>(amount) > maximum_bytes - result.size())
                throw Error("storage_size", "Storage file exceeds its size limit");
            result.append(buffer.data(), static_cast<std::size_t>(amount));
        }
    } catch (...) {
        ::close(fd);
        throw;
    }
    if (::close(fd) != 0) ioError("Cannot close storage file", path);
    return result;
}

void atomicWriteText(const std::string& path, const std::string& text) {
    std::lock_guard<std::mutex> lock(journal_mutex);
    requireSafePath(path, true);
    recoverJournal(path);
    const auto temporary = path + ".tmp";
    const auto backup = path + ".old";
    int fd = openExclusiveFile(temporary);
    try {
        std::size_t offset = 0;
        while (offset < text.size()) {
            const auto amount = ::write(fd, text.data() + offset,
                                       std::min<std::size_t>(text.size() - offset, 1024 * 1024));
            if (amount < 0 && errno == EINTR) continue;
            if (amount <= 0) ioError("Cannot write storage file", temporary, amount == 0 ? EIO : errno);
            offset += static_cast<std::size_t>(amount);
        }
        const int closing_fd = fd;
        fd = -1;
        syncAndCloseFile(closing_fd, temporary);
        const bool had_previous = regularExists(path);
        if (had_previous) {
            if (::rename(path.c_str(), backup.c_str()) != 0) ioError("Cannot back up storage file", path);
            syncDirectory(parentPath(path));
        }
        if (::rename(temporary.c_str(), path.c_str()) != 0) {
            const int error = errno;
            if (had_previous) {
                // Keep .old for a later recovery if restoring it fails here.
                if (::rename(backup.c_str(), path.c_str()) == 0) syncDirectory(parentPath(path));
            }
            ioError("Cannot install storage file", path, error);
        }
        syncDirectory(parentPath(path));
        if (had_previous) removeFileIfPresent(backup);
    } catch (...) {
        if (fd >= 0) ::close(fd);
        // .tmp/.old are deliberately retained for the next recovery attempt.
        throw;
    }
}

json_t* readStoredJson(const std::string& path, std::size_t maximum_bytes) {
    return readJson(path, maximum_bytes).release();
}

void atomicWriteJson(const std::string& path, json_t* value) {
    atomicWriteJson(path, value, std::numeric_limits<std::size_t>::max());
}

void atomicWriteJson(const std::string& path, json_t* value, std::size_t maximum_bytes) {
    if (!value) throw Error("storage_schema", "Cannot save missing JSON");
    if (!maximum_bytes) throw Error("storage_size", "Serialized storage metadata exceeds its size limit");
    struct Sink {
        std::string text;
        std::size_t maximum;
        bool limited = false;
    } sink {{}, maximum_bytes - 1}; // Reserve one byte for the final newline.
    auto append = [](const char* data, std::size_t size, void* context) noexcept -> int {
        auto& output = *static_cast<Sink*>(context);
        if (size > output.maximum - output.text.size()) {
            output.limited = true;
            return -1;
        }
        try { output.text.append(data, size); }
        catch (...) { return -1; }
        return 0;
    };
    if (json_dump_callback(value, append, &sink, JSON_INDENT(2) | JSON_SORT_KEYS) != 0) {
        if (sink.limited) throw Error("storage_size", "Serialized storage metadata exceeds its size limit");
        throw Error("storage_memory", "Cannot encode storage JSON");
    }
    sink.text += '\n';
    atomicWriteText(path, sink.text);
}

void removeOwnedTree(const std::string& input) {
    const auto path = normalizeAbsolute(input);
    if (path == "/" || path == "sdmc:/")
        throw Error("storage_path", "Cannot remove a filesystem root");
    requireSafePath(path, true);
    removeTree(path, 0);
}

std::string newId() {
    std::array<unsigned char, 16> random {};
#ifdef __SWITCH__
    if (R_FAILED(csrngInitialize()))
        throw Error("storage_random", "Cannot open the system random source");
    const Result random_result = csrngGetRandomBytes(random.data(), random.size());
    csrngExit();
    if (R_FAILED(random_result))
        throw Error("storage_random", "Cannot obtain a random instance ID");
#else
    const int fd = ::open("/dev/urandom", O_RDONLY);
    if (fd < 0) throw Error("storage_random", "Cannot open the system random source");
    std::size_t offset = 0;
    while (offset < random.size()) {
        const auto amount = ::read(fd, random.data() + offset, random.size() - offset);
        if (amount < 0 && errno == EINTR) continue;
        if (amount <= 0) {
            ::close(fd);
            throw Error("storage_random", "Cannot obtain a random instance ID");
        }
        offset += static_cast<std::size_t>(amount);
    }
    if (::close(fd) != 0) throw Error("storage_random", "Cannot close the system random source");
#endif
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(32);
    for (const auto byte : random) {
        result += hex[byte >> 4];
        result += hex[byte & 15];
    }
    return result;
}

Storage::Storage(std::string root) : root_(normalizeAbsolute(root)) {}

void Storage::initialize() {
    createDirectories(root_);
    createDirectories(joinPath(root_, "instances"));
    createDirectories(joinPath(root_, "transactions"));
    createDirectories(joinPath(root_, "cache"));
}

std::size_t Storage::cleanupTransactions() {
    initialize();
    const auto path = joinPath(root_, "transactions");
    requireSafePath(path);
    DIR* directory = ::opendir(path.c_str());
    if (!directory) ioError("Cannot inspect abandoned transactions", path);
    std::vector<std::string> abandoned;
    try {
        while (true) {
            errno = 0;
            const dirent* entry = ::readdir(directory);
            if (!entry) {
                if (errno) ioError("Cannot read transaction directory", path);
                break;
            }
            const std::string name = entry->d_name;
            if (name.size() != 32 || name.find_first_not_of("0123456789abcdef") != std::string::npos)
                continue;
            const auto candidate = joinPath(path, name);
            struct stat st {};
            if (::lstat(candidate.c_str(), &st) != 0) ioError("Cannot inspect abandoned transaction", candidate);
            if (S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode)) abandoned.push_back(candidate);
            if (abandoned.size() > kMaxInstancesCount)
                throw Error("storage_size", "Too many abandoned transactions");
        }
    } catch (...) {
        ::closedir(directory);
        throw;
    }
    if (::closedir(directory) != 0) ioError("Cannot close transaction directory", path);
    // No journal fields are used as paths. A surviving published instance lives
    // in root_/instances and cannot be reached by this startup-only sweep.
    for (const auto& candidate : abandoned) removeOwnedTree(candidate);
    return abandoned.size();
}

Settings Storage::loadSettings() {
    initialize();
    Settings result;
    result.root = root_;
    auto value = readJson(joinPath(root_, "settings.json"), kMaxSettings);
    if (!value) return result;
    checkFields(value.get(), {"schema", "root", "profile_id", "curseforge_key", "heap_mib",
                            "reduced_motion", "show_incompatible"}, {"jvm_args", "library_sort", "full_initial_heap"}, "settings");
    getInteger(value.get(), "schema", 1, 1);
    // Validate, but do not allow on-disk data to change the selected storage root.
    normalizeAbsolute(getString(value.get(), "root", kMaxPath, true));
    result.profile_id = getString(value.get(), "profile_id", 64);
    if (!result.profile_id.empty() && !validId(result.profile_id)) invalidData("profile_id");
    result.curseforge_key = getString(value.get(), "curseforge_key", 4096);
    // Accept the previous6144MiB schema limit so older expanded-device settings
    // migrate instead of preventing startup. The frontend also applies its
    // device-specific budget before preparing a launch.
    result.heap_mib = std::min(kPortableMaxHeapMiB,
        static_cast<int>(getInteger(value.get(), "heap_mib", 64, 6144)));
    result.reduced_motion = getBoolean(value.get(), "reduced_motion");
    result.show_incompatible = getBoolean(value.get(), "show_incompatible");
    if (json_object_get(value.get(), "jvm_args")) result.jvm_args = getString(value.get(), "jvm_args", 4096);
    if (json_object_get(value.get(), "library_sort"))
        result.library_sort = static_cast<int>(getInteger(value.get(), "library_sort", 0, 2));
    if (json_object_get(value.get(), "full_initial_heap"))
        result.full_initial_heap = getBoolean(value.get(), "full_initial_heap");
    return result;
}

void Storage::saveSettings(const Settings& settings) {
    if ((!settings.profile_id.empty() && !validId(settings.profile_id)) ||
        settings.heap_mib < 64 || settings.heap_mib > kPortableMaxHeapMiB ||
        settings.library_sort < 0 || settings.library_sort > 2) invalidData("settings");
    auto value = checkedJson(json_object());
    setValue(value.get(), "schema", json_integer(1));
    setString(value.get(), "root", root_, kMaxPath, true);
    setString(value.get(), "profile_id", settings.profile_id, 64);
    setString(value.get(), "curseforge_key", settings.curseforge_key, 4096);
    setValue(value.get(), "heap_mib", json_integer(settings.heap_mib));
    setValue(value.get(), "reduced_motion", json_boolean(settings.reduced_motion));
    setValue(value.get(), "show_incompatible", json_boolean(settings.show_incompatible));
    setString(value.get(), "jvm_args", settings.jvm_args, 4096);
    setValue(value.get(), "library_sort", json_integer(settings.library_sort));
    setValue(value.get(), "full_initial_heap", json_boolean(settings.full_initial_heap));
    initialize();
    atomicWriteJson(joinPath(root_, "settings.json"), value.get());
}

std::vector<Profile> Storage::loadProfiles() {
    initialize();
    auto value = readJson(joinPath(root_, "profiles.json"), kMaxProfiles);
    if (!value) return {};
    checkFields(value.get(), {"schema", "profiles"}, "profiles");
    getInteger(value.get(), "schema", 1, 1);
    auto array = json_object_get(value.get(), "profiles");
    if (!json_is_array(array) || json_array_size(array) > kMaxProfilesCount) invalidData("profiles");
    std::vector<Profile> result;
    std::set<std::string> ids;
    for (std::size_t i = 0; i < json_array_size(array); ++i) {
        auto object = json_array_get(array, i);
        checkFields(object, {"id", "username"}, "profile");
        Profile profile {getString(object, "id", 64, true), getString(object, "username", 16, true)};
        if (!validId(profile.id) || !validUsername(profile.username) || !ids.insert(profile.id).second)
            invalidData("profile");
        result.push_back(std::move(profile));
    }
    return result;
}

void Storage::saveProfiles(const std::vector<Profile>& profiles) {
    if (profiles.size() > kMaxProfilesCount) invalidData("profiles");
    auto value = checkedJson(json_object());
    auto array = checkedJson(json_array());
    std::set<std::string> ids;
    for (const auto& profile : profiles) {
        if (!validId(profile.id) || !validUsername(profile.username) || !ids.insert(profile.id).second)
            invalidData("profile");
        auto item = checkedJson(json_object());
        setString(item.get(), "id", profile.id, 64, true);
        setString(item.get(), "username", profile.username, 16, true);
        if (json_array_append_new(array.get(), item.release()) != 0)
            throw Error("storage_memory", "Cannot create profile JSON");
    }
    setValue(value.get(), "schema", json_integer(1));
    setValue(value.get(), "profiles", array.release());
    initialize();
    atomicWriteJson(joinPath(root_, "profiles.json"), value.get());
}

std::vector<Instance> Storage::listInstances() {
    initialize();
    const auto path = joinPath(root_, "instances");
    requireSafePath(path);
    DIR* directory = ::opendir(path.c_str());
    if (!directory) ioError("Cannot list instances", path);
    std::vector<std::string> names;
    try {
        while (true) {
            errno = 0;
            dirent* entry = ::readdir(directory);
            if (!entry) {
                if (errno) ioError("Cannot read instances directory", path);
                break;
            }
            const std::string name = entry->d_name;
            if (!validId(name)) continue;
            const auto instance_path = joinPath(path, name);
            struct stat st {};
            if (::lstat(instance_path.c_str(), &st) != 0) ioError("Cannot inspect instance directory", instance_path);
            if (!S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode)) continue;
            names.push_back(name);
            if (names.size() > kMaxInstancesCount) throw Error("storage_size", "Too many stored instances");
        }
    } catch (...) {
        ::closedir(directory);
        throw;
    }
    if (::closedir(directory) != 0) ioError("Cannot close instances directory", path);
    std::sort(names.begin(), names.end());
    std::vector<Instance> result;
    for (const auto& id : names) {
        Instance instance;
        instance.id = id;
        instance.root = joinPath(path, id);
        instance.name = id;
        try {
            auto value = readJson(joinPath(instance.root, "instance.json"), kMaxInstance);
            if (!value) continue; // Uncommitted/import staging directories are not instances.
            checkFields(value.get(), {"schema", "id", "name", "root", "game", "state", "reason",
                                    "provider_project", "provider_version", "installed_bytes"},
                        {"icon", "notes", "jvm_args", "last_played", "heap_mib", "packwiz_url"}, "instance");
            getInteger(value.get(), "schema", 1, 1);
            if (getString(value.get(), "id", 64, true) != id) invalidData("instance.id");
            // Stored root is descriptive only; derive the real root after a move/import.
            getString(value.get(), "root", kMaxPath, true);
            instance.name = getString(value.get(), "name", 1024, true);
            instance.game = parseGame(json_object_get(value.get(), "game"));
            instance.state = parseState(getString(value.get(), "state", 32, true));
            instance.reason = getString(value.get(), "reason", 8192);
            instance.provider_project = getString(value.get(), "provider_project", 256);
            instance.provider_version = getString(value.get(), "provider_version", 256);
            instance.installed_bytes = static_cast<std::uint64_t>(getInteger(value.get(), "installed_bytes", 0,
                                                  std::numeric_limits<json_int_t>::max()));
            auto has = [&](const char* field) { return json_object_get(value.get(), field) != nullptr; };
            if (has("icon")) instance.icon = getString(value.get(), "icon", 64);
            if (has("notes")) instance.notes = getString(value.get(), "notes", 8192);
            if (has("jvm_args")) instance.jvm_args = getString(value.get(), "jvm_args", 4096);
            if (has("last_played")) instance.last_played = getInteger(value.get(), "last_played", 0,
                                                  std::numeric_limits<json_int_t>::max());
            if (has("heap_mib")) instance.heap_mib = static_cast<int>(getInteger(value.get(), "heap_mib", 0, kPortableMaxHeapMiB));
            if (has("packwiz_url")) instance.packwiz_url = getString(value.get(), "packwiz_url", 2048);
        } catch (const Error& error) {
            instance.state = InstanceState::Broken;
            instance.reason = "Instance metadata could not be loaded (" + error.code + ")";
        }
        result.push_back(std::move(instance));
    }
    return result;
}

void Storage::saveInstance(const Instance& instance) {
    if (!validId(instance.id) ||
        instance.installed_bytes > static_cast<std::uint64_t>(std::numeric_limits<json_int_t>::max()) ||
        instance.last_played < 0 || instance.heap_mib < 0 || instance.heap_mib > kPortableMaxHeapMiB)
        invalidData("instance");
    const auto instance_root = joinPath(root_, "instances/" + instance.id);
    auto value = checkedJson(json_object());
    setValue(value.get(), "schema", json_integer(1));
    setString(value.get(), "id", instance.id, 64, true);
    setString(value.get(), "name", instance.name, 1024, true);
    setString(value.get(), "root", instance_root, kMaxPath, true);
    setValue(value.get(), "game", gameJson(instance.game).release());
    setString(value.get(), "state", stateName(instance.state), 32, true);
    setString(value.get(), "reason", instance.reason, 8192);
    setString(value.get(), "provider_project", instance.provider_project, 256);
    setString(value.get(), "provider_version", instance.provider_version, 256);
    setValue(value.get(), "installed_bytes", json_integer(static_cast<json_int_t>(instance.installed_bytes)));
    setString(value.get(), "icon", instance.icon, 64);
    setString(value.get(), "notes", instance.notes, 8192);
    setString(value.get(), "jvm_args", instance.jvm_args, 4096);
    setValue(value.get(), "last_played", json_integer(static_cast<json_int_t>(instance.last_played)));
    setValue(value.get(), "heap_mib", json_integer(instance.heap_mib));
    setString(value.get(), "packwiz_url", instance.packwiz_url, 2048);
    initialize();
    createDirectories(instance_root);
    atomicWriteJson(joinPath(instance_root, "instance.json"), value.get());
}

std::uint64_t Storage::availableBytes() const {
    requireSafePath(root_);
#ifdef __SWITCH__
    FsFileSystem* filesystem = fsdevGetDeviceFileSystem("sdmc");
    s64 bytes = 0;
    if (!filesystem || R_FAILED(fsFsGetFreeSpace(filesystem, "/", &bytes)) || bytes < 0)
        throw Error("storage_space", "Cannot read SD free space");
    return static_cast<std::uint64_t>(bytes);
#else
    struct statvfs info {};
    if (::statvfs(root_.c_str(), &info) != 0) ioError("Cannot read available space", root_);
    const auto blocks = static_cast<std::uint64_t>(info.f_bavail);
    const auto unit = static_cast<std::uint64_t>(info.f_frsize ? info.f_frsize : info.f_bsize);
    if (unit && blocks > std::numeric_limits<std::uint64_t>::max() / unit)
        return std::numeric_limits<std::uint64_t>::max();
    return blocks * unit;
#endif
}
}
