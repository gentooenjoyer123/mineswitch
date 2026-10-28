#include "mineswitch/content.hpp"
#include "mineswitch/import.hpp"
#include "mineswitch/nbt.hpp"
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <functional>
#include <jansson.h>
#include <mbedtls/md.h>
#include <memory>
#include <minizip/unzip.h>
#include <minizip/zip.h>
#include <mutex>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/syscall.h>
#endif

namespace mineswitch {
namespace {
std::mutex content_mutex;

std::string lower(std::string s) {
    for (char &c : s)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    return s;
}
bool ends(const std::string &s, const std::string &suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}
void cancelled(const std::atomic_bool *stop) {
    if (stop && stop->load())
        throw Error("cancelled", "Операция отменена");
}
// A single directory entry name returned by one of the list functions.
std::string entryName(const std::string &name) {
    if (name.empty() || name == "." || name == ".." || name.find('/') != std::string::npos || !validRelativePath(name))
        throw Error("content.name", "Некорректное имя файла");
    return name;
}
struct Stat {
    bool exists = false, directory = false, regular = false;
    std::uint64_t bytes = 0;
    std::int64_t modified = 0;
};
Stat inspect(const std::string &path) {
    Stat result;
    struct stat st {};
    if (::lstat(path.c_str(), &st) != 0)
        return result;
    result.exists = true;
    result.directory = S_ISDIR(st.st_mode);
    result.regular = S_ISREG(st.st_mode);
    result.bytes = st.st_size > 0 ? static_cast<std::uint64_t>(st.st_size) : 0;
    result.modified = static_cast<std::int64_t>(st.st_mtime);
    return result;
}
std::vector<std::string> names(const std::string &folder) {
    std::vector<std::string> result;
    if (!pathExists(folder))
        return result;
    requireSafePath(folder);
    DIR *raw = ::opendir(folder.c_str());
    if (!raw)
        throw Error("content.list", "Не удалось открыть папку");
    std::unique_ptr<DIR, int (*)(DIR *)> directory(raw, ::closedir);
    while (auto *entry = ::readdir(directory.get())) {
        std::string name = entry->d_name;
        if (name == "." || name == ".." || !validRelativePath(name) || name.find('/') != std::string::npos)
            continue;
        result.push_back(name);
        if (result.size() >= 8192)
            break;
    }
    return result;
}
void publish(const std::string &from, const std::string &to) {
    requireSafePath(from);
    requireSafePath(to, true);
#ifdef __linux__
    if (syscall(SYS_renameat2, AT_FDCWD, from.c_str(), AT_FDCWD, to.c_str(), 1u) != 0)
#else
    // Horizon's filesystem rename fails when the destination already exists.
    if (pathExists(to) || std::rename(from.c_str(), to.c_str()) != 0)
#endif
        throw Error("content.rename", "Не удалось переместить: " + std::string(std::strerror(errno)));
}
std::string readFile(const std::string &path, std::size_t maximum) { return readTextFile(path, maximum); }

// Recursively visits regular files below root, relative paths with '/'.
void walk(const std::string &root, const std::string &relative,
          const std::function<bool(const std::string &, bool)> &keep,
          const std::function<void(const std::string &, const Stat &)> &file, unsigned depth = 0) {
    if (depth > 64)
        throw Error("content.depth", "Слишком глубокая вложенность папок");
    auto folder = relative.empty() ? root : joinPath(root, relative);
    for (const auto &name : names(folder)) {
        auto child = relative.empty() ? name : relative + "/" + name;
        auto info = inspect(joinPath(root, child));
        if (!info.directory && !info.regular)
            continue;
        if (!keep(child, info.directory))
            continue;
        if (info.directory)
            walk(root, child, keep, file, depth + 1);
        else
            file(child, info);
    }
}
std::uint64_t treeBytes(const std::string &root, const std::function<bool(const std::string &, bool)> &keep) {
    std::uint64_t total = 0;
    walk(root, {}, keep, [&](const std::string &, const Stat &info) { total += info.bytes; });
    return total;
}
struct Copier {
    ProgressCallback progress;
    const std::atomic_bool *stop;
    std::string stage;
    std::uint64_t done = 0, total = 0;
    std::uint32_t files = 0;
    std::vector<char> buffer = std::vector<char>(1024 * 1024);

    void report(const std::string &detail) {
        if (progress)
            progress({stage, detail, done, total, files, 0});
    }
    void stream(const std::string &from, const std::function<void(const char *, std::size_t)> &sink) {
        int fd = ::open(from.c_str(), O_RDONLY);
        if (fd < 0)
            throw Error("content.read", "Не удалось открыть " + from);
        std::unique_ptr<int, void (*)(int *)> guard(&fd, [](int *value) { ::close(*value); });
        while (true) {
            cancelled(stop);
            auto amount = ::read(fd, buffer.data(), buffer.size());
            if (amount < 0 && errno == EINTR)
                continue;
            if (amount < 0)
                throw Error("content.read", "Ошибка чтения " + from);
            if (amount == 0)
                break;
            sink(buffer.data(), static_cast<std::size_t>(amount));
            done += static_cast<std::uint64_t>(amount);
        }
    }
    void copy(const std::string &from, const std::string &to) {
        int out = openExclusiveFile(to);
        try {
            stream(from, [&](const char *data, std::size_t size) {
                std::size_t offset = 0;
                while (offset < size) {
                    auto written = ::write(out, data + offset, size - offset);
                    if (written < 0 && errno == EINTR)
                        continue;
                    if (written <= 0)
                        throw Error("disk_space", "Не удалось записать файл");
                    offset += static_cast<std::size_t>(written);
                }
            });
        } catch (...) {
            ::close(out);
            throw;
        }
        syncAndCloseFile(out, to);
        ++files;
        report(from.substr(from.find_last_of('/') + 1));
    }
    void tree(const std::string &from, const std::string &to, const std::function<bool(const std::string &, bool)> &keep) {
        createDirectories(to);
        walk(from, {}, keep, [&](const std::string &relative, const Stat &) {
            auto target = joinPath(to, relative);
            createDirectories(target.substr(0, target.find_last_of('/')));
            copy(joinPath(from, relative), target);
        });
    }
};

class ZipWriter {
    zipFile file_ = nullptr;
    std::string path_;

  public:
    explicit ZipWriter(const std::string &path) : path_(path) {
        requireSafePath(path, true);
        if (pathExists(path))
            throw Error("content.exists", "Файл уже существует");
        file_ = zipOpen64(path.c_str(), APPEND_STATUS_CREATE);
        if (!file_)
            throw Error("content.zip", "Не удалось создать ZIP");
    }
    ~ZipWriter() {
        if (file_)
            zipClose(file_, nullptr);
    }
    void add(const std::string &name, Copier &copier, const std::string &source, std::int64_t modified) {
        zip_fileinfo info{};
        std::time_t when = static_cast<std::time_t>(modified);
        if (auto *local = std::localtime(&when)) {
            info.tmz_date.tm_sec = local->tm_sec;
            info.tmz_date.tm_min = local->tm_min;
            info.tmz_date.tm_hour = local->tm_hour;
            info.tmz_date.tm_mday = local->tm_mday;
            info.tmz_date.tm_mon = local->tm_mon;
            info.tmz_date.tm_year = local->tm_year + 1900;
        }
        if (zipOpenNewFileInZip64(file_, name.c_str(), &info, nullptr, 0, nullptr, 0, nullptr, Z_DEFLATED, 6, 1) != ZIP_OK)
            throw Error("content.zip", "Не удалось добавить файл в ZIP");
        copier.stream(source, [&](const char *data, std::size_t size) {
            if (zipWriteInFileInZip(file_, data, static_cast<unsigned>(size)) != ZIP_OK)
                throw Error("disk_space", "Не удалось записать ZIP");
        });
        if (zipCloseFileInZip(file_) != ZIP_OK)
            throw Error("content.zip", "Не удалось завершить запись в ZIP");
        ++copier.files;
        copier.report(name);
    }
    void text(const std::string &name, const std::string &data) {
        zip_fileinfo info{};
        if (zipOpenNewFileInZip64(file_, name.c_str(), &info, nullptr, 0, nullptr, 0, nullptr, Z_DEFLATED, 6, 0) != ZIP_OK ||
            zipWriteInFileInZip(file_, data.data(), static_cast<unsigned>(data.size())) != ZIP_OK ||
            zipCloseFileInZip(file_) != ZIP_OK)
            throw Error("content.zip", "Не удалось записать ZIP");
    }
    void finish() {
        auto result = zipClose(file_, nullptr);
        file_ = nullptr;
        if (result != ZIP_OK)
            throw Error("content.zip", "Не удалось завершить ZIP");
    }
};
std::string stamp() {
    std::time_t now = std::time(nullptr);
    char value[32] = "0";
    if (auto *local = std::localtime(&now))
        std::strftime(value, sizeof(value), "%Y-%m-%d_%H-%M-%S", local);
    return value;
}
// FAT/exFAT-safe ASCII file name; Horizon does not list some non-ASCII names.
std::string safeName(const std::string &name, const std::string &fallback) {
    std::string out;
    for (unsigned char c : name) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_')
            out += static_cast<char>(c);
        else if ((c == ' ' || c == '.') && !out.empty() && out.back() != '_')
            out += '_';
        if (out.size() >= 48)
            break;
    }
    while (!out.empty() && out.back() == '_')
        out.pop_back();
    return out.empty() ? fallback : out;
}

std::string textComponent(json_t *value, unsigned depth = 0) {
    if (depth > 16 || !value)
        return {};
    if (json_is_string(value))
        return json_string_value(value);
    std::string out;
    if (json_is_array(value)) {
        std::size_t i;
        json_t *item;
        json_array_foreach(value, i, item) out += textComponent(item, depth + 1);
    } else if (json_is_object(value)) {
        out += textComponent(json_object_get(value, "text"), depth + 1);
        out += textComponent(json_object_get(value, "extra"), depth + 1);
    }
    return out;
}
std::string stripFormatting(const std::string &text) {
    std::string out;
    for (std::size_t i = 0; i < text.size(); ++i) {
        // § is C2 A7 in UTF-8 and is followed by one formatting code.
        if (static_cast<unsigned char>(text[i]) == 0xc2 && i + 1 < text.size() &&
            static_cast<unsigned char>(text[i + 1]) == 0xa7) {
            i += 2;
            continue;
        }
        out += text[i] == '\n' ? ' ' : text[i];
    }
    return out;
}
std::string zipMember(const std::string &path, const char *member, std::size_t maximum) {
    unzFile zip = unzOpen64(path.c_str());
    if (!zip)
        throw Error("content.archive", "Файл не является ZIP-архивом");
    std::unique_ptr<void, int (*)(unzFile)> close(zip, unzClose);
    if (unzLocateFile(zip, member, 1) != UNZ_OK)
        return {};
    unz_file_info64 info{};
    if (unzGetCurrentFileInfo64(zip, &info, nullptr, 0, nullptr, 0, nullptr, 0) != UNZ_OK ||
        info.uncompressed_size > maximum || unzOpenCurrentFile(zip) != UNZ_OK)
        return {};
    std::string data(static_cast<std::size_t>(info.uncompressed_size), '\0');
    int count = data.empty() ? 0 : unzReadCurrentFile(zip, &data[0], static_cast<unsigned>(data.size()));
    unzCloseCurrentFile(zip);
    if (count != static_cast<int>(data.size()))
        return {};
    return data;
}
std::string packDescription(const std::string &mcmeta) {
    if (mcmeta.empty())
        return {};
    json_error_t error{};
    std::unique_ptr<json_t, void (*)(json_t *)> root(json_loadb(mcmeta.data(), mcmeta.size(), 0, &error),
                                                     [](json_t *value) { json_decref(value); });
    if (!root)
        return {};
    auto pack = json_object_get(root.get(), "pack");
    return stripFormatting(textComponent(json_object_get(pack, "description")));
}
std::string worldsFolder(const Storage &storage, const Instance &instance) {
    return joinPath(instanceRoot(storage, instance), "game/saves");
}
Tag readLevel(const std::string &world) {
    return parseNbt(readFile(joinPath(world, "level.dat"), 16 * 1024 * 1024));
}
} // namespace

std::string instanceRoot(const Storage &storage, const Instance &instance) {
    if (instance.id.empty() || instance.id.size() > 64 ||
        instance.id.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-") !=
            std::string::npos)
        throw Error("content.instance", "Некорректная сборка");
    auto root = joinPath(storage.root(), "instances/" + instance.id);
    if (!instance.root.empty() && instance.root != root)
        throw Error("content.instance", "Сборка находится вне библиотеки");
    requireSafePath(root);
    return root;
}
const char *contentFolder(ContentKind kind) {
    switch (kind) {
    case ContentKind::ResourcePack: return "resourcepacks";
    case ContentKind::ShaderPack: return "shaderpacks";
    default: return "mods";
    }
}

LocalFile digestFile(const std::string &path, const std::atomic_bool *stop) {
    requireSafePath(path);
    Copier reader{{}, stop, {}};
    mbedtls_md_context_t context;
    mbedtls_md_init(&context);
    std::unique_ptr<mbedtls_md_context_t, void (*)(mbedtls_md_context_t *)> guard(&context, mbedtls_md_free);
    if (mbedtls_md_setup(&context, mbedtls_md_info_from_type(MBEDTLS_MD_SHA1), 0) || mbedtls_md_starts(&context))
        throw Error("content.hash", "Не удалось вычислить SHA-1");
    // CurseForge fingerprints are MurmurHash2 (seed 1) of the file without
    // tab, LF, CR and space bytes, so the filtered length is needed first.
    auto ignored = [](unsigned char c) { return c == 9 || c == 10 || c == 13 || c == 32; };
    std::uint32_t length = 0;
    reader.stream(path, [&](const char *data, std::size_t size) {
        mbedtls_md_update(&context, reinterpret_cast<const unsigned char *>(data), size);
        for (std::size_t i = 0; i < size; ++i)
            length += !ignored(static_cast<unsigned char>(data[i]));
    });
    unsigned char digest[20];
    mbedtls_md_finish(&context, digest);
    LocalFile result;
    static constexpr char hex[] = "0123456789abcdef";
    for (auto byte : digest) {
        result.sha1 += hex[byte >> 4];
        result.sha1 += hex[byte & 15];
    }
    constexpr std::uint32_t m = 0x5bd1e995;
    std::uint32_t h = 1 ^ length, word = 0;
    unsigned have = 0;
    reader.stream(path, [&](const char *data, std::size_t size) {
        for (std::size_t i = 0; i < size; ++i) {
            auto c = static_cast<unsigned char>(data[i]);
            if (ignored(c))
                continue;
            word |= static_cast<std::uint32_t>(c) << (8 * have);
            if (++have == 4) {
                word *= m;
                word ^= word >> 24;
                word *= m;
                h = h * m ^ word;
                word = 0;
                have = 0;
            }
        }
    });
    if (have) {
        h ^= word;
        h *= m;
    }
    h ^= h >> 13;
    h *= m;
    h ^= h >> 15;
    result.fingerprint = h;
    return result;
}

std::vector<PackEntry> listPacks(const Storage &storage, const Instance &instance, ContentKind kind) {
    std::lock_guard<std::mutex> lock(content_mutex);
    auto folder = joinPath(instanceRoot(storage, instance), std::string("game/") + contentFolder(kind));
    std::vector<PackEntry> result;
    for (const auto &name : names(folder)) {
        auto path = joinPath(folder, name);
        auto info = inspect(path);
        auto key = lower(name);
        PackEntry entry;
        entry.filename = name;
        entry.enabled = !ends(key, ".disabled");
        if (!entry.enabled)
            key.resize(key.size() - 9);
        entry.directory = info.directory;
        entry.bytes = info.bytes;
        if (!info.directory && !(info.regular && ends(key, ".zip")))
            continue;
        entry.name = entry.enabled ? name : name.substr(0, name.size() - 9);
        if (!entry.directory && ends(lower(entry.name), ".zip"))
            entry.name.resize(entry.name.size() - 4);
        try {
            if (kind == ContentKind::ResourcePack)
                entry.description = packDescription(
                    entry.directory ? (pathExists(joinPath(path, "pack.mcmeta")) ? readFile(joinPath(path, "pack.mcmeta"), 65536) : "")
                                    : zipMember(path, "pack.mcmeta", 65536));
        } catch (const Error &error) {
            entry.problem = error.what();
        }
        result.push_back(std::move(entry));
    }
    std::sort(result.begin(), result.end(), [](const auto &a, const auto &b) { return lower(a.name) < lower(b.name); });
    return result;
}

void setPackEnabled(const Storage &storage, const Instance &instance, ContentKind kind, const std::string &filename,
                    bool enabled) {
    std::lock_guard<std::mutex> lock(content_mutex);
    auto name = entryName(filename);
    bool current = !ends(lower(name), ".disabled");
    if (current == enabled)
        return;
    auto folder = joinPath(instanceRoot(storage, instance), std::string("game/") + contentFolder(kind));
    auto target = enabled ? name.substr(0, name.size() - 9) : name + ".disabled";
    if (!inspect(joinPath(folder, name)).exists)
        throw Error("content.missing", "Файл больше не найден");
    if (inspect(joinPath(folder, target)).exists)
        throw Error("content.exists", "Уже есть файл с именем " + target);
    publish(joinPath(folder, name), joinPath(folder, target));
}

void removeContent(const Storage &storage, const Instance &instance, ContentKind kind, const std::string &filename) {
    std::lock_guard<std::mutex> lock(content_mutex);
    auto path = joinPath(joinPath(instanceRoot(storage, instance), std::string("game/") + contentFolder(kind)),
                         entryName(filename));
    requireSafePath(path);
    auto info = inspect(path);
    if (!info.exists)
        throw Error("content.missing", "Файл больше не найден");
    if (info.directory)
        removeOwnedTree(path);
    else if (::unlink(path.c_str()) != 0)
        throw Error("content.remove", "Не удалось удалить файл: " + std::string(std::strerror(errno)));
}

void installPack(Storage &storage, HttpClient &http, const Instance &instance, ContentKind kind,
                 const CatalogVersion &version, ProgressCallback progress, const std::atomic_bool *stop) {
    if (kind != ContentKind::ResourcePack && kind != ContentKind::ShaderPack)
        throw Error("content.kind", "Неподдерживаемый тип пакета");
    auto file = version.archive;
    if (!ends(lower(file.file_name), ".zip") || file.hashes.empty())
        throw Error("content.download", "Каталог не предоставил проверяемый ZIP");
    if (file.manual || file.urls.empty())
        throw Error("mods.manual_download", "Автор разрешает только ручную загрузку: " + file.manual_url);
    auto name = entryName(file.file_name);
    auto root = instanceRoot(storage, instance);
    auto available = storage.availableBytes();
    constexpr std::uint64_t margin = 32 * 1024 * 1024;
    if (available <= margin || file.bytes > available - margin)
        throw Error("disk_space", "Недостаточно места на SD-карте");
    file.max_bytes = std::min<std::uint64_t>({file.max_bytes, 2ULL * 1024 * 1024 * 1024, available - margin});
    auto stage = joinPath(storage.root(), "transactions/" + newId());
    createDirectories(stage);
    struct Clean {
        std::string path;
        ~Clean() {
            try {
                removeOwnedTree(path);
            } catch (...) {
            }
        }
    } clean{stage};
    auto temporary = joinPath(stage, "download.zip");
    http.download(file, temporary, progress, stop);
    cancelled(stop);
    verifyFile(temporary, file.hashes, file.max_bytes, stop);
    unzFile zip = unzOpen64(temporary.c_str());
    if (!zip)
        throw Error("content.archive", "Скачанный файл не является ZIP-архивом");
    unzClose(zip);
    std::lock_guard<std::mutex> lock(content_mutex);
    auto folder = joinPath(root, std::string("game/") + contentFolder(kind));
    createDirectories(folder);
    for (const auto &existing : names(folder)) {
        auto key = lower(existing);
        if (ends(key, ".disabled"))
            key.resize(key.size() - 9);
        if (key == lower(name))
            throw Error("content.exists", "Файл уже установлен: " + existing);
    }
    publish(temporary, joinPath(folder, name));
    if (progress) {
        try {
            progress({"installed", name, file.bytes, file.bytes, 1, 1});
        } catch (...) {
        }
    }
}

std::vector<WorldInfo> listWorlds(const Storage &storage, const Instance &instance) {
    std::lock_guard<std::mutex> lock(content_mutex);
    auto folder = worldsFolder(storage, instance);
    std::vector<WorldInfo> result;
    for (const auto &name : names(folder)) {
        auto path = joinPath(folder, name);
        if (!inspect(path).directory || !inspect(joinPath(path, "level.dat")).regular)
            continue;
        WorldInfo world;
        world.folder = name;
        world.name = name;
        world.icon = inspect(joinPath(path, "icon.png")).regular;
        try {
            auto root = readLevel(path);
            auto *data = root.find("Data");
            if (!data || data->type != TagType::Compound)
                throw Error("nbt.invalid", "В level.dat нет данных мира");
            auto level = data->string("LevelName");
            if (!level.empty())
                world.name = level;
            world.last_played = data->number("LastPlayed");
            world.game_type = static_cast<int>(data->number("GameType"));
            world.hardcore = data->number("hardcore") != 0;
            world.cheats = data->number("allowCommands") != 0;
            if (auto *version = data->find("Version"))
                world.version = version->string("Name");
        } catch (const Error &error) {
            world.problem = error.what();
        }
        result.push_back(std::move(world));
    }
    std::sort(result.begin(), result.end(), [](const auto &a, const auto &b) { return a.last_played > b.last_played; });
    return result;
}

void deleteWorld(const Storage &storage, const Instance &instance, const std::string &folder) {
    std::lock_guard<std::mutex> lock(content_mutex);
    auto path = joinPath(worldsFolder(storage, instance), entryName(folder));
    if (!inspect(path).directory)
        throw Error("content.missing", "Мир больше не найден");
    removeOwnedTree(path);
}

void renameWorld(const Storage &storage, const Instance &instance, const std::string &folder, const std::string &name) {
    if (name.empty() || name.size() > 128)
        throw Error("content.name", "Название мира: от 1 до 128 байт");
    std::lock_guard<std::mutex> lock(content_mutex);
    auto path = joinPath(worldsFolder(storage, instance), entryName(folder));
    auto root = readLevel(path);
    auto *data = root.find("Data");
    if (!data || data->type != TagType::Compound)
        throw Error("nbt.invalid", "В level.dat нет данных мира");
    data->set("LevelName", TagType::String).text = name;
    atomicWriteText(joinPath(path, "level.dat"), gzipData(writeNbt(root)));
}

std::string copyWorld(const Storage &storage, const Instance &instance, const std::string &folder,
                      ProgressCallback progress, const std::atomic_bool *stop) {
    auto saves = worldsFolder(storage, instance);
    auto source = joinPath(saves, entryName(folder));
    if (!inspect(source).directory)
        throw Error("content.missing", "Мир больше не найден");
    std::string target;
    for (int n = 1; n < 1000 && target.empty(); ++n) {
        auto candidate = safeName(folder, "World") + "-copy" + (n > 1 ? "-" + std::to_string(n) : "");
        if (!inspect(joinPath(saves, candidate)).exists)
            target = candidate;
    }
    if (target.empty())
        throw Error("content.exists", "Слишком много копий этого мира");
    auto skip = [](const std::string &relative, bool) { return relative != "session.lock"; };
    Copier copier{progress, stop, "copy"};
    copier.total = treeBytes(source, skip);
    auto stage = joinPath(storage.root(), "transactions/" + newId());
    try {
        copier.tree(source, stage, skip);
        publish(stage, joinPath(saves, target));
    } catch (...) {
        try {
            removeOwnedTree(stage);
        } catch (...) {
        }
        throw;
    }
    try {
        auto root = readLevel(joinPath(saves, target));
        if (auto *data = root.find("Data")) {
            auto name = data->string("LevelName");
            renameWorld(storage, instance, target, (name.empty() ? folder : name) + " (копия)");
        }
    } catch (const Error &) {
        // The copy is complete even if its display name cannot be changed.
    }
    return target;
}

std::string backupWorld(const Storage &storage, const Instance &instance, const std::string &folder,
                        ProgressCallback progress, const std::atomic_bool *stop) {
    auto source = joinPath(worldsFolder(storage, instance), entryName(folder));
    if (!inspect(source).directory)
        throw Error("content.missing", "Мир больше не найден");
    auto backups = joinPath(instanceRoot(storage, instance), "game/backups");
    createDirectories(backups);
    auto target = joinPath(backups, stamp() + "_" + safeName(folder, "World") + ".zip");
    auto part = target + ".part";
    auto skip = [](const std::string &relative, bool) { return relative != "session.lock"; };
    Copier copier{progress, stop, "backup"};
    copier.total = treeBytes(source, skip);
    try {
        ZipWriter zip(part);
        walk(source, {}, skip, [&](const std::string &relative, const Stat &info) {
            zip.add(folder + "/" + relative, copier, joinPath(source, relative), info.modified);
        });
        zip.finish();
        publish(part, target);
    } catch (...) {
        ::unlink(part.c_str());
        throw;
    }
    return target;
}

std::vector<ServerEntry> listServers(const Storage &storage, const Instance &instance) {
    std::lock_guard<std::mutex> lock(content_mutex);
    auto path = joinPath(instanceRoot(storage, instance), "game/servers.dat");
    std::vector<ServerEntry> result;
    if (!pathExists(path))
        return result;
    auto root = parseNbt(readFile(path, 8 * 1024 * 1024));
    auto *servers = root.find("servers");
    if (!servers || servers->type != TagType::List)
        return result;
    for (std::size_t i = 0; i < servers->children.size(); ++i) {
        const auto &item = servers->children[i];
        if (item.type != TagType::Compound)
            continue;
        result.push_back({item.string("name"), item.string("ip"), static_cast<int>(i)});
    }
    return result;
}

void saveServers(const Storage &storage, const Instance &instance, const std::vector<ServerEntry> &entries) {
    std::lock_guard<std::mutex> lock(content_mutex);
    auto game = joinPath(instanceRoot(storage, instance), "game");
    createDirectories(game);
    auto path = joinPath(game, "servers.dat");
    Tag root;
    root.type = TagType::Compound;
    if (pathExists(path)) {
        try {
            root = parseNbt(readFile(path, 8 * 1024 * 1024));
        } catch (const Error &) {
            root = Tag{};
            root.type = TagType::Compound;
        }
    }
    std::vector<Tag> previous;
    if (auto *servers = root.find("servers"); servers && servers->type == TagType::List)
        previous = servers->children;
    auto &list = root.set("servers", TagType::List);
    list.children.clear();
    list.list_type = TagType::Compound;
    for (const auto &entry : entries) {
        if (entry.address.empty() || entry.address.size() > 255 || entry.name.size() > 255)
            throw Error("content.server", "Адрес сервера: от 1 до 255 символов");
        Tag item;
        if (entry.original >= 0 && static_cast<std::size_t>(entry.original) < previous.size() &&
            previous[entry.original].type == TagType::Compound)
            item = previous[entry.original];
        item.type = TagType::Compound;
        item.name.clear();
        item.set("name", TagType::String).text = entry.name.empty() ? "Minecraft Server" : entry.name;
        item.set("ip", TagType::String).text = entry.address;
        list.children.push_back(std::move(item));
    }
    atomicWriteText(path, writeNbt(root));
}

std::vector<MediaFile> listScreenshots(const Storage &storage, const Instance &instance) {
    auto folder = joinPath(instanceRoot(storage, instance), "game/screenshots");
    std::vector<MediaFile> result;
    for (const auto &name : names(folder)) {
        auto key = lower(name);
        if (!ends(key, ".png") && !ends(key, ".jpg") && !ends(key, ".jpeg"))
            continue;
        auto path = joinPath(folder, name);
        auto info = inspect(path);
        if (info.regular)
            result.push_back({name, path, info.bytes, info.modified});
    }
    std::sort(result.begin(), result.end(), [](const auto &a, const auto &b) {
        return a.modified != b.modified ? a.modified > b.modified : a.name > b.name;
    });
    return result;
}

void deleteScreenshot(const Storage &storage, const Instance &instance, const std::string &name) {
    auto path = joinPath(joinPath(instanceRoot(storage, instance), "game/screenshots"), entryName(name));
    if (!inspect(path).regular)
        throw Error("content.missing", "Скриншот больше не найден");
    if (::unlink(path.c_str()) != 0)
        throw Error("content.remove", "Не удалось удалить скриншот");
}

std::vector<MediaFile> listLogs(const Storage &storage, const Instance &instance) {
    auto root = instanceRoot(storage, instance);
    std::vector<MediaFile> result;
    auto add = [&](const std::string &folder, const std::string &label, auto accept) {
        for (const auto &name : names(joinPath(root, folder))) {
            if (!accept(lower(name)))
                continue;
            auto path = joinPath(root, folder + "/" + name);
            auto info = inspect(path);
            if (info.regular)
                result.push_back({label + name, path, info.bytes, info.modified});
        }
    };
    add("game/logs", "", [](const std::string &n) { return ends(n, ".log") || ends(n, ".log.gz"); });
    add("game/crash-reports", "Краш · ", [](const std::string &n) { return ends(n, ".txt"); });
    add("preparation", "Подготовка · ", [](const std::string &n) { return ends(n, ".log"); });
    std::sort(result.begin(), result.end(), [](const auto &a, const auto &b) {
        bool al = a.name == "latest.log", bl = b.name == "latest.log";
        if (al != bl)
            return al;
        return a.modified != b.modified ? a.modified > b.modified : a.name > b.name;
    });
    return result;
}

std::vector<std::string> readLog(const Storage &storage, const Instance &instance, const std::string &path,
                                 std::size_t maximum) {
    auto root = instanceRoot(storage, instance) + "/";
    if (path.compare(0, root.size(), root) != 0)
        throw Error("content.path", "Журнал вне папки сборки");
    requireSafePath(path);
    auto info = inspect(path);
    if (!info.regular)
        throw Error("content.missing", "Журнал больше не найден");
    std::string data;
    if (ends(lower(path), ".gz")) {
        data = inflateData(readFile(path, 16 * 1024 * 1024), 64 * 1024 * 1024);
    } else {
        int fd = ::open(path.c_str(), O_RDONLY);
        if (fd < 0)
            throw Error("content.read", "Не удалось открыть журнал");
        std::unique_ptr<int, void (*)(int *)> guard(&fd, [](int *value) { ::close(*value); });
        off_t start = info.bytes > maximum ? static_cast<off_t>(info.bytes - maximum) : 0;
        if (::lseek(fd, start, SEEK_SET) < 0)
            throw Error("content.read", "Не удалось прочитать журнал");
        data.resize(static_cast<std::size_t>(info.bytes - static_cast<std::uint64_t>(start)));
        std::size_t offset = 0;
        while (offset < data.size()) {
            auto amount = ::read(fd, &data[offset], data.size() - offset);
            if (amount < 0 && errno == EINTR)
                continue;
            if (amount <= 0)
                break;
            offset += static_cast<std::size_t>(amount);
        }
        data.resize(offset);
    }
    if (data.size() > maximum)
        data.erase(0, data.size() - maximum);
    std::vector<std::string> lines;
    std::string line;
    for (char c : data) {
        if (c == '\n') {
            lines.push_back(std::move(line));
            line.clear();
        } else if (c == '\t')
            line += "    ";
        else if (c != '\r')
            line += c;
    }
    if (!line.empty())
        lines.push_back(std::move(line));
    return lines;
}

Instance duplicateInstance(Storage &storage, const Instance &instance, const std::string &name,
                           ProgressCallback progress, const std::atomic_bool *stop) {
    auto source = instanceRoot(storage, instance);
    if (name.empty() || name.size() > 120)
        throw Error("content.name", "Название сборки: от 1 до 120 байт");
    // Generated state refers to the original paths and is rebuilt on preparation.
    auto keep = [](const std::string &relative, bool) {
        return relative != "instance.json" && relative != "launch-plan.json" && relative != "tmp" &&
               relative != "natives" && relative != "preparation" && relative != "game/session.lock" &&
               relative.compare(0, 9, "game/logs") != 0;
    };
    Copier copier{progress, stop, "copy"};
    copier.total = treeBytes(source, keep);
    auto available = storage.availableBytes();
    if (copier.total + 64 * 1024 * 1024 > available)
        throw Error("disk_space", "Недостаточно места на SD-карте для копии");
    auto stage = joinPath(storage.root(), "transactions/" + newId());
    Instance copy = instance;
    copy.id = newId();
    copy.name = name;
    copy.root = joinPath(storage.root(), "instances/" + copy.id);
    copy.last_played = 0;
    if (copy.state != InstanceState::NeedsFiles && copy.state != InstanceState::Unsupported) {
        copy.state = InstanceState::NeedsPreparation;
        copy.reason = "Копия: файлы игры будут проверены перед запуском";
    }
    try {
        copier.tree(source, stage, keep);
        publish(stage, copy.root);
    } catch (...) {
        try {
            removeOwnedTree(stage);
        } catch (...) {
        }
        throw;
    }
    storage.saveInstance(copy);
    return copy;
}

void deleteInstance(Storage &storage, const Instance &instance) {
    auto root = instanceRoot(storage, instance);
    if (!inspect(root).directory)
        throw Error("content.missing", "Сборка больше не найдена");
    // An interrupted removal is finished by the startup transaction sweep.
    auto trash = joinPath(storage.root(), "transactions/" + newId());
    publish(root, trash);
    removeOwnedTree(trash);
}

std::string exportInstance(Storage &storage, const Instance &instance, ProgressCallback progress,
                           const std::atomic_bool *stop) {
    auto root = instanceRoot(storage, instance);
    auto game = joinPath(root, "game");
    auto exports = joinPath(storage.root(), "exports");
    createDirectories(exports);
    auto target = joinPath(exports, safeName(instance.name, "MineSwitch") + "_" + stamp() + ".zip");
    auto part = target + ".part";
    auto keep = [](const std::string &relative, bool) {
        return relative != "session.lock" && relative != "logs" && relative != "crash-reports" &&
               relative.find("/session.lock") == std::string::npos;
    };
    std::string uid, loader_version = instance.game.loader.version;
    switch (instance.game.loader.kind) {
    case LoaderKind::Fabric: uid = "net.fabricmc.fabric-loader"; break;
    case LoaderKind::Quilt: uid = "org.quiltmc.quilt-loader"; break;
    case LoaderKind::Forge: uid = "net.minecraftforge"; break;
    case LoaderKind::NeoForge: uid = "net.neoforged"; break;
    default: break;
    }
    std::unique_ptr<json_t, void (*)(json_t *)> pack(json_object(), [](json_t *value) { json_decref(value); });
    auto components = json_array();
    json_array_append_new(components, json_pack("{s:s,s:s,s:b}", "uid", "net.minecraft", "version",
                                                instance.game.minecraft_version.c_str(), "important", 1));
    if (!uid.empty()) {
        if (instance.game.loader.kind == LoaderKind::Fabric || instance.game.loader.kind == LoaderKind::Quilt)
            json_array_append_new(components, json_pack("{s:s,s:s,s:b}", "uid", "net.fabricmc.intermediary", "version",
                                                        instance.game.minecraft_version.c_str(), "dependencyOnly", 1));
        json_array_append_new(components, json_pack("{s:s,s:s}", "uid", uid.c_str(), "version", loader_version.c_str()));
    }
    json_object_set_new(pack.get(), "components", components);
    json_object_set_new(pack.get(), "formatVersion", json_integer(1));
    char *pack_text = json_dumps(pack.get(), JSON_INDENT(2));
    if (!pack_text)
        throw Error("content.export", "Не удалось подготовить mmc-pack.json");
    std::string mmc_pack(pack_text);
    std::free(pack_text);
    std::string cfg = "InstanceType=OneSix\nname=" + instance.name + "\n";
    if (!instance.notes.empty()) {
        std::string notes;
        for (char c : instance.notes)
            notes += c == '\n' ? std::string("\\n") : std::string(1, c);
        cfg += "notes=" + notes + "\n";
    }
    if (instance.heap_mib)
        cfg += "OverrideMemory=true\nMaxMemAlloc=" + std::to_string(instance.heap_mib) + "\n";
    Copier copier{progress, stop, "export"};
    copier.total = treeBytes(game, keep);
    try {
        ZipWriter zip(part);
        zip.text("instance.cfg", cfg);
        zip.text("mmc-pack.json", mmc_pack);
        walk(game, {}, keep, [&](const std::string &relative, const Stat &info) {
            zip.add(".minecraft/" + relative, copier, joinPath(game, relative), info.modified);
        });
        zip.finish();
        publish(part, target);
    } catch (...) {
        ::unlink(part.c_str());
        throw;
    }
    return target;
}
} // namespace mineswitch
