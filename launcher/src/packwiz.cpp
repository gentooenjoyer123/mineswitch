#include "mineswitch/packwiz.hpp"
#include "mineswitch/catalog.hpp"
#include "mineswitch/import.hpp"
#include <jansson.h>
#include <mbedtls/md.h>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <memory>
#include <set>
#include <sys/stat.h>
#include <unistd.h>

namespace mineswitch {
namespace {
using Json = std::unique_ptr<json_t, decltype(&json_decref)>;
constexpr std::uint64_t free_margin = 64ULL * 1024 * 1024;

[[noreturn]] void bad(const std::string& message) { throw Error("packwiz.format", message); }
void cancel(const std::atomic_bool* flag) {
    if (flag && flag->load(std::memory_order_relaxed)) throw Error("cancelled", "Operation cancelled");
}
std::string lower(std::string value) {
    for (char& c : value) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return value;
}
bool ends(const std::string& value, const std::string& tail) {
    return value.size() >= tail.size() && value.compare(value.size() - tail.size(), tail.size(), tail) == 0;
}

// ------------------------------------------------------------------ TOML
struct Toml {
    const std::string& s;
    std::size_t i = 0;
    TomlDocument out;
    std::map<std::string, std::string>* element = nullptr; // current [[array]] entry
    std::string prefix;                                    // current [table] + "."

    explicit Toml(const std::string& text) : s(text) {}
    bool done() const { return i >= s.size(); }
    char peek(std::size_t ahead = 0) const { return i + ahead < s.size() ? s[i + ahead] : '\0'; }
    void spaces() { while (!done() && (s[i] == ' ' || s[i] == '\t')) ++i; }
    void comment() { if (peek() == '#') while (!done() && s[i] != '\n') ++i; }
    // Whitespace, newlines and comments, as allowed between array elements.
    void blank() {
        for (;;) {
            spaces(); comment();
            if (peek() == '\r' || peek() == '\n') { ++i; continue; }
            return;
        }
    }
    void endOfLine() {
        spaces(); comment();
        if (peek() == '\r') ++i;
        if (!done() && s[i] != '\n') bad("Unexpected text after a TOML value");
        if (!done()) ++i;
    }
    static void utf8(std::string& out, unsigned long cp) {
        if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) bad("Invalid TOML unicode escape");
        if (cp < 0x80) out += static_cast<char>(cp);
        else if (cp < 0x800) { out += static_cast<char>(0xC0 | (cp >> 6)); out += static_cast<char>(0x80 | (cp & 63)); }
        else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12)); out += static_cast<char>(0x80 | ((cp >> 6) & 63));
            out += static_cast<char>(0x80 | (cp & 63));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18)); out += static_cast<char>(0x80 | ((cp >> 12) & 63));
            out += static_cast<char>(0x80 | ((cp >> 6) & 63)); out += static_cast<char>(0x80 | (cp & 63));
        }
    }
    std::string basic() {
        bool multi = s.compare(i, 3, "\"\"\"") == 0;
        i += multi ? 3 : 1;
        if (multi && peek() == '\r') ++i;
        if (multi && peek() == '\n') ++i;
        std::string value;
        for (;;) {
            if (done()) bad("Unterminated TOML string");
            char c = s[i];
            if (multi ? s.compare(i, 3, "\"\"\"") == 0 : c == '"') { i += multi ? 3 : 1; return value; }
            if (!multi && (c == '\n' || c == '\r')) bad("Newline in a TOML string");
            if (c != '\\') { value += c; ++i; continue; }
            char e = peek(1);
            i += 2;
            switch (e) {
            case '"': value += '"'; break;
            case '\\': value += '\\'; break;
            case 'n': value += '\n'; break;
            case 't': value += '\t'; break;
            case 'r': value += '\r'; break;
            case 'b': value += '\b'; break;
            case 'f': value += '\f'; break;
            case 'u': case 'U': {
                std::size_t n = e == 'u' ? 4 : 8;
                if (i + n > s.size()) bad("Short TOML unicode escape");
                unsigned long cp = 0;
                for (std::size_t k = 0; k < n; ++k) {
                    char h = s[i + k];
                    int digit = h >= '0' && h <= '9' ? h - '0' : h >= 'a' && h <= 'f' ? h - 'a' + 10 : h >= 'A' && h <= 'F' ? h - 'A' + 10 : -1;
                    if (digit < 0) bad("Invalid TOML unicode escape");
                    cp = cp * 16 + static_cast<unsigned long>(digit);
                }
                i += n;
                utf8(value, cp);
                break;
            }
            default:
                if (multi && (e == '\n' || e == '\r' || e == ' ' || e == '\t')) {
                    // Line-ending backslash trims the following whitespace.
                    i -= 1;
                    while (!done() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) ++i;
                    break;
                }
                bad("Invalid TOML escape");
            }
        }
    }
    std::string literal() {
        bool multi = s.compare(i, 3, "'''") == 0;
        i += multi ? 3 : 1;
        if (multi && peek() == '\r') ++i;
        if (multi && peek() == '\n') ++i;
        auto end = s.find(multi ? "'''" : "'", i);
        if (end == std::string::npos) bad("Unterminated TOML string");
        std::string value = s.substr(i, end - i);
        if (!multi && value.find('\n') != std::string::npos) bad("Newline in a TOML string");
        i = end + (multi ? 3 : 1);
        return value;
    }
    std::string key() {
        std::string full;
        for (;;) {
            spaces();
            std::string part;
            if (peek() == '"') part = basic();
            else if (peek() == '\'') part = literal();
            else {
                while (!done() && (std::isalnum(static_cast<unsigned char>(s[i])) || s[i] == '_' || s[i] == '-')) part += s[i++];
                if (part.empty()) bad("Invalid TOML key");
            }
            full += part;
            spaces();
            if (peek() != '.') return full;
            ++i;
            full += '.';
        }
    }
    void store(const std::string& name, std::string value) {
        auto& target = element ? *element : out.values;
        auto full = element ? name : prefix + name;
        if (!target.emplace(full, std::move(value)).second) bad("Duplicate TOML key: " + full);
    }
    std::string scalar() {
        if (peek() == '"') return basic();
        if (peek() == '\'') return literal();
        std::string value;
        while (!done() && s[i] != ',' && s[i] != ']' && s[i] != '}' && s[i] != '#' &&
               s[i] != '\n' && s[i] != '\r') value += s[i++];
        while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.pop_back();
        if (value.empty()) bad("Missing TOML value");
        return value;
    }
    // Inline table members become "name.member" keys.
    void value(const std::string& name, int depth = 0) {
        if (depth > 16) bad("TOML nesting is too deep");
        spaces();
        if (peek() == '[') {
            ++i;
            std::string joined;
            for (;;) {
                blank();
                if (peek() == ']') { ++i; break; }
                if (peek() == '[' || peek() == '{') skip(depth + 1);
                else { if (!joined.empty()) joined += '\n'; joined += scalar(); }
                blank();
                if (peek() == ',') { ++i; continue; }
                if (peek() == ']') { ++i; break; }
                bad("Invalid TOML array");
            }
            store(name, joined);
        } else if (peek() == '{') {
            ++i;
            spaces();
            if (peek() == '}') { ++i; return; }
            for (;;) {
                auto member = key();
                if (peek() != '=') bad("Expected '=' in a TOML inline table");
                ++i;
                value(name + "." + member, depth + 1);
                spaces();
                if (peek() == ',') { ++i; continue; }
                if (peek() == '}') { ++i; return; }
                bad("Invalid TOML inline table");
            }
        } else store(name, scalar());
    }
    // Nested arrays and inline tables inside arrays are not used by packwiz.
    void skip(int depth) {
        if (depth > 16) bad("TOML nesting is too deep");
        char open = s[i++], close = open == '[' ? ']' : '}';
        for (;;) {
            blank();
            if (done()) bad("Unterminated TOML array");
            if (peek() == close) { ++i; return; }
            if (peek() == '[' || peek() == '{') skip(depth + 1);
            else if (peek() == '"') basic();
            else if (peek() == '\'') literal();
            else ++i;
        }
    }
    TomlDocument parse() {
        if (s.size() > 16 * 1024 * 1024) bad("TOML file is too large");
        for (;;) {
            blank();
            if (done()) return std::move(out);
            if (peek() == '[') {
                bool array = peek(1) == '[';
                i += array ? 2 : 1;
                auto name = key();
                if (s.compare(i, array ? 2 : 1, array ? "]]" : "]") != 0) bad("Invalid TOML table header");
                i += array ? 2 : 1;
                if (array) {
                    auto& list = out.arrays[name];
                    if (list.size() >= 65536) bad("Too many TOML array entries");
                    list.emplace_back();
                    element = &list.back();
                    prefix.clear();
                } else {
                    element = nullptr;
                    prefix = name + ".";
                }
                endOfLine();
                continue;
            }
            auto name = key();
            if (peek() != '=') bad("Expected '=' after a TOML key");
            ++i;
            value(name);
            endOfLine();
        }
    }
};

// ------------------------------------------------------------------ hashes
std::string digest(mbedtls_md_type_t kind, const std::string& data) {
    unsigned char out[MBEDTLS_MD_MAX_SIZE];
    const auto* info = mbedtls_md_info_from_type(kind);
    if (!info || mbedtls_md(info, reinterpret_cast<const unsigned char*>(data.data()), data.size(), out))
        throw Error("hash", "Cannot compute digest");
    static const char digits[] = "0123456789abcdef";
    std::string text;
    for (unsigned k = 0; k < mbedtls_md_get_size(info); ++k) { text += digits[out[k] >> 4]; text += digits[out[k] & 15]; }
    return text;
}
Hashes hashes(const std::string& format, const std::string& value) {
    Hashes out;
    auto f = lower(format), v = lower(value);
    if (f == "sha256") out.sha256 = v;
    else if (f == "sha1") out.sha1 = v;
    else if (f == "sha512") out.sha512 = v;
    else if (f == "md5") out.md5 = v;
    else throw Error("packwiz.hash", "Unsupported packwiz hash format: " + format);
    return out;
}
void checkText(const std::string& data, const std::string& format, const std::string& value, const std::string& what) {
    auto f = lower(format);
    mbedtls_md_type_t kind = f == "sha256" ? MBEDTLS_MD_SHA256 : f == "sha1" ? MBEDTLS_MD_SHA1 :
                             f == "sha512" ? MBEDTLS_MD_SHA512 : f == "md5" ? MBEDTLS_MD_MD5 : MBEDTLS_MD_NONE;
    if (kind == MBEDTLS_MD_NONE) throw Error("packwiz.hash", "Unsupported packwiz hash format: " + format);
    if (digest(kind, data) != lower(value)) throw Error("packwiz.hash", what + " does not match its packwiz hash");
}

// ------------------------------------------------------------------ URLs
std::string encodePath(const std::string& path) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : path) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' || c == '/') out += static_cast<char>(c);
        else { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
    }
    return out;
}
std::string directoryOf(const std::string& url) { return url.substr(0, url.rfind('/') + 1); }
std::string parentOf(const std::string& path) {
    auto slash = path.rfind('/');
    return slash == std::string::npos ? std::string() : path.substr(0, slash);
}
void requireHttps(const std::string& url) {
    if (url.rfind("https://", 0) != 0 || url.size() > 2048 || url.size() < 12 ||
        url.find_first_of(" \t\r\n\"<>\\") != std::string::npos)
        throw Error("packwiz.url", "packwiz pack URL must be HTTPS: " + url);
}
std::string fetch(HttpClient& http, const std::string& url, std::uint64_t limit, const std::atomic_bool* stop) {
    HttpRequest request;
    request.url = url;
    request.max_bytes = limit;
    auto response = http.request(request, stop);
    if (response.status != 200)
        throw Error("packwiz.http", "packwiz server returned HTTP " + std::to_string(response.status) + " for " + url);
    return response.body;
}

// ------------------------------------------------------------------ state
struct Entry { std::string hash, dest; };
struct State {
    std::string url, index_hash, version, versions;
    std::map<std::string, Entry> entries; // keyed by index path
};
std::string statePath(const Instance& instance) { return joinPath(instance.root, "packwiz-state.json"); }
std::string jstr(json_t* object, const char* key) {
    auto* value = json_object_get(object, key);
    return json_is_string(value) ? json_string_value(value) : std::string();
}
State loadState(const Instance& instance) {
    State state;
    Json value(readStoredJson(statePath(instance), 16 * 1024 * 1024), json_decref);
    if (!value || !json_is_object(value.get())) return state;
    state.url = jstr(value.get(), "url");
    state.index_hash = jstr(value.get(), "index_hash");
    state.version = jstr(value.get(), "version");
    state.versions = jstr(value.get(), "versions");
    const char* key; json_t* item;
    json_object_foreach(json_object_get(value.get(), "entries"), key, item) {
        Entry entry{jstr(item, "hash"), jstr(item, "dest")};
        if (validRelativePath(key) && validRelativePath(entry.dest)) state.entries[key] = entry;
    }
    return state;
}
void saveState(const Instance& instance, const State& state) {
    Json value(json_pack("{s:i,s:s,s:s,s:s,s:s}", "schema", 1, "url", state.url.c_str(),
                         "index_hash", state.index_hash.c_str(), "version", state.version.c_str(),
                         "versions", state.versions.c_str()), json_decref);
    Json entries(json_object(), json_decref);
    if (!value || !entries) throw Error("memory", "Cannot allocate packwiz state");
    for (const auto& item : state.entries)
        json_object_set_new(entries.get(), item.first.c_str(),
                            json_pack("{s:s,s:s}", "hash", item.second.hash.c_str(), "dest", item.second.dest.c_str()));
    json_object_set(value.get(), "entries", entries.get());
    atomicWriteJson(statePath(instance), value.get(), 16 * 1024 * 1024);
}

// The pack's [versions] table as a game spec; loader stays as is when absent.
std::string versionsText(const TomlDocument& pack) {
    std::string text = pack.get("versions.minecraft");
    for (const char* loader : {"neoforge", "forge", "fabric", "quilt"}) {
        auto version = pack.get(std::string("versions.") + loader);
        if (!version.empty()) text += std::string(" ") + loader + " " + version;
    }
    return text;
}
bool applyVersions(const TomlDocument& pack, GameSpec& game) {
    auto before = game;
    auto minecraft = pack.get("versions.minecraft");
    if (!minecraft.empty()) game.minecraft_version = minecraft;
    const std::pair<const char*, LoaderKind> loaders[] = {{"neoforge", LoaderKind::NeoForge}, {"forge", LoaderKind::Forge},
                                                          {"fabric", LoaderKind::Fabric}, {"quilt", LoaderKind::Quilt}};
    for (const auto& loader : loaders) {
        auto version = pack.get(std::string("versions.") + loader.first);
        if (version.empty()) continue;
        game.loader.kind = loader.second;
        game.loader.version = version;
        break;
    }
    return game.minecraft_version != before.minecraft_version || game.loader.kind != before.loader.kind ||
           game.loader.version != before.loader.version;
}

struct Target {
    std::string index_path, dest, entry_hash;
    Download download;
    bool preserve = false;
};
bool flag(const std::map<std::string, std::string>& table, const std::string& key) {
    auto found = table.find(key);
    if (found == table.end()) return false;
    if (found->second == "true") return true;
    if (found->second == "false") return false;
    bad("TOML value of " + key + " must be a boolean");
}
std::string field(const std::map<std::string, std::string>& table, const std::string& key) {
    auto found = table.find(key);
    return found == table.end() ? std::string() : found->second;
}
// Present copy of a destination, which may be disabled by the mods tab.
// pathExists() rejects a missing parent directory; here that just means absent.
bool regularFile(const std::string& path) {
    struct stat info {};
    return ::lstat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode);
}
std::string presentCopy(const std::string& path) {
    if (regularFile(path)) return path;
    if (regularFile(path + ".disabled")) return path + ".disabled";
    return {};
}
bool matches(const std::string& path, const Hashes& expected, const std::atomic_bool* stop) {
    try {
        verifyFile(path, expected, UINT64_MAX, stop);
        return true;
    } catch (const Error& error) {
        if (error.code == "cancelled") throw;
        return false;
    }
}
void unlinkFile(const std::string& path) {
    if (::unlink(path.c_str()) && errno != ENOENT) throw Error("io", "Cannot remove " + path + ": " + std::strerror(errno));
}
} // namespace

std::string TomlDocument::get(const std::string& key, const std::string& fallback) const {
    auto found = values.find(key);
    return found == values.end() ? fallback : found->second;
}
TomlDocument parseToml(const std::string& text) { return Toml(text).parse(); }

bool packwizSynced(const Instance& instance) {
    try {
        auto state = loadState(instance);
        return !state.url.empty() && state.url == instance.packwiz_url && !state.index_hash.empty();
    } catch (const Error&) {
        return false;
    }
}

PackwizSync syncPackwiz(Storage& storage, HttpClient& http, const Settings& settings, Instance& instance, bool force,
                        ProgressCallback progress, const std::atomic_bool* stop) {
    PackwizSync result;
    const auto url = instance.packwiz_url;
    requireHttps(url);
    auto report = [&](const std::string& detail, std::uint32_t done, std::uint32_t total) {
        if (progress) progress({"packwiz", detail, done, total, done, total});
    };
    report("Чтение pack.toml", 0, 0);
    auto pack = parseToml(fetch(http, url, 1024 * 1024, stop));
    if (pack.get("pack-format", "packwiz:1.0.0").rfind("packwiz:", 0) != 0) bad("Unsupported pack-format");
    result.version = pack.get("version");
    auto index_file = pack.get("index.file");
    auto index_format = pack.get("index.hash-format");
    auto index_hash = pack.get("index.hash");
    if (!validRelativePath(index_file) || index_format.empty() || index_hash.empty()) bad("pack.toml has no valid [index]");

    auto state = loadState(instance);
    if (state.url != url) state = State{};
    // Versions are applied when the pack changes them, so a manual loader
    // choice in the wizard survives until the next pack release does.
    auto versions = versionsText(pack);
    if (versions != state.versions || state.index_hash.empty()) {
        auto game = instance.game;
        if (applyVersions(pack, game)) {
            instance.game = game;
            storage.saveInstance(instance);
            result.game_changed = true;
        }
    }
    const auto applied = index_format + ":" + lower(index_hash);
    if (!force && state.index_hash == applied) {
        if (state.versions != versions) { state.versions = versions; saveState(instance, state); }
        return result;
    }
    result.changed = true;

    report("Чтение index.toml", 0, 0);
    const auto index_url = directoryOf(url) + encodePath(index_file);
    auto index_text = fetch(http, index_url, 16 * 1024 * 1024, stop);
    checkText(index_text, index_format, index_hash, "index.toml");
    auto index = parseToml(index_text);
    const auto default_format = index.get("hash-format", index_format);
    const auto base = directoryOf(index_url), base_path = parentOf(index_file);
    const auto& files = index.arrays["files"];
    const auto game_root = joinPath(instance.root, "game");

    std::vector<Target> targets;
    std::set<std::string> dests;
    std::unique_ptr<CatalogClient> catalog;
    std::uint32_t step = 0;
    for (const auto& file : files) {
        cancel(stop);
        Target target;
        target.index_path = field(file, "file");
        auto format = field(file, "hash-format");
        if (format.empty()) format = default_format;
        auto hash = lower(field(file, "hash"));
        if (!validRelativePath(target.index_path) || hash.empty()) bad("index.toml has an invalid file entry");
        target.entry_hash = format + ":" + hash;
        target.preserve = flag(file, "preserve");
        auto alias = field(file, "alias");
        if (!alias.empty() && !validRelativePath(alias)) bad("index.toml has an invalid alias");
        bool metafile = flag(file, "metafile") || ends(lower(target.index_path), ".pw.toml");
        const auto file_url = base + encodePath(target.index_path);
        auto known = state.entries.find(target.index_path);
        if (!metafile) {
            target.dest = alias.empty() ? target.index_path : alias;
            target.download.urls = {file_url};
            target.download.hashes = hashes(format, hash);
        } else if (!force && known != state.entries.end() && known->second.hash == target.entry_hash &&
                   !presentCopy(joinPath(game_root, known->second.dest)).empty()) {
            // Unchanged metafile whose file is in place: nothing to fetch.
            target.dest = known->second.dest;
            target.preserve = true;
        } else {
            report("Метаданные: " + target.index_path, step, static_cast<std::uint32_t>(files.size()));
            auto text = fetch(http, file_url, 1024 * 1024, stop);
            checkText(text, format, hash, target.index_path);
            auto meta = parseToml(text);
            auto side = lower(meta.get("side", "both"));
            auto filename = meta.get("filename");
            if (filename.empty() || filename.find('/') != std::string::npos) bad(target.index_path + " has no valid filename");
            if (side == "server") { ++result.skipped; ++step; continue; }
            if (meta.get("option.optional") == "true" && meta.get("option.default") != "true") { ++result.skipped; ++step; continue; }
            auto folder = parentOf(alias.empty() ? target.index_path : alias);
            target.dest = folder.empty() ? filename : folder + "/" + filename;
            auto mode = lower(meta.get("download.mode", "url"));
            if (mode == "metadata:curseforge") {
                if (!catalog) catalog = std::make_unique<CatalogClient>(http, settings);
                auto version = catalog->version(Provider::CurseForge, meta.get("update.curseforge.project-id"),
                                                meta.get("update.curseforge.file-id"), stop, ContentKind::Mod);
                target.download = version.archive;
            } else if (mode == "url" || mode.empty()) {
                auto download = meta.get("download.url");
                requireHttps(download);
                target.download.urls = {download};
                target.download.hashes = hashes(meta.get("download.hash-format"), meta.get("download.hash"));
            } else bad("Unsupported packwiz download mode: " + mode);
        }
        if (!validRelativePath(target.dest)) bad("packwiz destination is invalid: " + target.dest);
        if (!dests.insert(lower(target.dest)).second) bad("packwiz destinations repeat: " + target.dest);
        target.download.file_name = target.dest.substr(target.dest.rfind('/') + 1);
        targets.push_back(std::move(target));
        ++step;
    }

    State next;
    next.url = url;
    next.version = result.version;
    next.versions = versions;
    const auto part = joinPath(instance.root, "packwiz-download.part");
    unlinkFile(part);
    std::uint32_t done = 0;
    const auto total = static_cast<std::uint32_t>(targets.size());
    for (const auto& target : targets) {
        cancel(stop);
        report(target.dest, done, total);
        const auto final_path = joinPath(game_root, target.dest);
        auto present = presentCopy(final_path);
        auto known = state.entries.find(target.index_path);
        // A forced sync rehashes files instead of trusting the last state.
        bool recorded = !force && known != state.entries.end() && known->second.hash == target.entry_hash && known->second.dest == target.dest;
        bool keep = !present.empty() && (target.preserve || recorded || matches(present, target.download.hashes, stop));
        if (keep) {
            ++result.kept;
        } else {
            auto free_bytes = storage.availableBytes();
            if (free_bytes < free_margin + target.download.bytes)
                throw Error("disk_space", "Недостаточно места на SD-карте для файлов сборки");
            Download bounded = target.download;
            bounded.max_bytes = std::min<std::uint64_t>(free_bytes - free_margin, 1ULL << 30);
            if (bounded.bytes > bounded.max_bytes) throw Error("disk_space", "Недостаточно места на SD-карте для файлов сборки");
            http.download(bounded, part, progress, stop);
            createDirectories(parentOf(final_path));
            // Horizon's rename does not replace; an update keeps a disabled mod disabled.
            auto destination = present.empty() ? final_path : present;
            if (!present.empty()) unlinkFile(present);
            if (::rename(part.c_str(), destination.c_str()))
                throw Error("io", "Cannot place " + target.dest + ": " + std::strerror(errno));
            ++result.downloaded;
        }
        next.entries[target.index_path] = {target.entry_hash, target.dest};
        ++done;
    }
    // Remove what an earlier sync placed and the pack no longer lists.
    for (const auto& old : state.entries) {
        if (next.entries.count(old.first) || dests.count(lower(old.second.dest))) continue;
        auto present = presentCopy(joinPath(game_root, old.second.dest));
        if (present.empty()) continue;
        unlinkFile(present);
        ++result.removed;
    }
    next.index_hash = applied;
    saveState(instance, next);
    report("Готово", total, total);
    return result;
}
}
