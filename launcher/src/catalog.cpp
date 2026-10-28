#include "mineswitch/catalog.hpp"
#include <jansson.h>
#include <algorithm>
#include <cctype>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <utility>

namespace mineswitch {
namespace {
constexpr std::size_t manifest_limit = 16 * 1024 * 1024;
constexpr std::size_t file_limit = 10000;
constexpr const char* modrinth_api = "https://api.modrinth.com/v2";
constexpr const char* curseforge_api = "https://api.curseforge.com/v1";
struct JsonDelete { void operator()(json_t* value) const { json_decref(value); } };
using Json = std::unique_ptr<json_t, JsonDelete>;

[[noreturn]] void invalid(const std::string& message) { throw Error("pack.invalid_manifest", message); }
void cancellation(const std::atomic_bool* token) {
    if (token && token->load(std::memory_order_relaxed)) throw Error("catalog.cancelled", "Cancelled");
}
Json parse_json(const std::string& input) {
    if (input.empty() || input.size() > manifest_limit) invalid("Metadata is empty or exceeds the size limit");
    json_error_t error{};
    Json value(json_loadb(input.data(), input.size(), JSON_REJECT_DUPLICATES, &error));
    if (!value) invalid("Invalid JSON metadata at line " + std::to_string(error.line));
    return value;
}
const json_t* object(const json_t* value, const char* label) {
    if (!json_is_object(value)) invalid(std::string(label) + " must be an object");
    return value;
}
const json_t* array(const json_t* value, const char* label) {
    if (!json_is_array(value)) invalid(std::string(label) + " must be an array");
    if (json_array_size(value) > file_limit) invalid(std::string(label) + " contains too many entries");
    return value;
}
const json_t* member(const json_t* value, const char* key) { return json_object_get(value, key); }
std::string text(const json_t* value, const char* label, bool required = true) {
    if (!value || json_is_null(value)) {
        if (!required) return {};
        invalid(std::string("Missing ") + label);
    }
    if (!json_is_string(value)) invalid(std::string(label) + " must be a string");
    std::string out(json_string_value(value), json_string_length(value));
    if (out.find('\0') != std::string::npos) invalid(std::string(label) + " contains a null byte");
    if (required && out.empty()) invalid(std::string(label) + " is empty");
    return out;
}
std::string str(const json_t* value, const char* key, bool required = true) {
    return text(member(value, key), key, required);
}
std::uint64_t number(const json_t* value, const char* label, bool required = true) {
    if (!value || json_is_null(value)) {
        if (!required) return 0;
        invalid(std::string("Missing ") + label);
    }
    if (!json_is_integer(value) || json_integer_value(value) < 0)
        invalid(std::string(label) + " must be a nonnegative integer");
    return static_cast<std::uint64_t>(json_integer_value(value));
}
std::uint64_t num(const json_t* value, const char* key, bool required = true) {
    return number(member(value, key), key, required);
}
bool flag(const json_t* value, const char* key, bool fallback) {
    auto field = member(value, key);
    if (!field || json_is_null(field)) return fallback;
    if (!json_is_boolean(field)) invalid(std::string(key) + " must be a boolean");
    return json_is_true(field);
}
std::string lower(std::string value) {
    for (auto& c : value) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    return value;
}
bool ends_with(const std::string& value, const std::string& suffix) {
    return value.size() >= suffix.size() && value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}
std::string relative_path(const std::string& value) {
    if (value.empty() || value.size() > 1024 || value.front() == '/' || value.back() == '/')
        throw Error("pack.unsafe_path", "Invalid relative file path");
    for (unsigned char c : value)
        if (c < 32 || c == 127 || c == '\\' || c == ':')
            throw Error("pack.unsafe_path", "Unsupported character in relative file path");
    std::istringstream parts(value);
    std::string part;
    while (std::getline(parts, part, '/'))
        if (part.empty() || part == "." || part == "..")
            throw Error("pack.unsafe_path", "File path leaves its instance directory");
    return value;
}
std::string file_name(const std::string& value) {
    relative_path(value);
    if (value.find('/') != std::string::npos) throw Error("pack.unsafe_path", "File name contains a directory");
    return value;
}
std::string basename(const std::string& path) {
    auto at = path.find_last_of('/');
    return at == std::string::npos ? path : path.substr(at + 1);
}
bool https_url(const std::string& url) {
    if (url.size() > 8192 || url.compare(0, 8, "https://") != 0) return false;
    auto end = url.find_first_of("/?#", 8);
    auto authority = url.substr(8, end == std::string::npos ? end : end - 8);
    if (authority.empty() || authority.find('@') != std::string::npos) return false;
    for (unsigned char c : url) if (c <= 32 || c == 127 || c == '\\') return false;
    return url.find('#') == std::string::npos;
}
std::string approved_url(const std::string& value) {
    if (!https_url(value)) throw Error("catalog.invalid_url", "Download metadata requires a valid HTTPS URL");
    return value;
}
std::string encode(const std::string& value) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : value) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') out += static_cast<char>(c);
        else { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
    }
    return out;
}
std::string identifier(const std::string& value, Provider provider) {
    if (value.empty() || value.size() > 128) invalid("Invalid catalog identifier");
    for (unsigned char c : value) {
        if (provider == Provider::CurseForge) {
            if (c < '0' || c > '9') invalid("Invalid CurseForge identifier");
        } else if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                     (c >= '0' && c <= '9') || c == '-' || c == '_')) invalid("Invalid Modrinth identifier");
    }
    if (provider == Provider::CurseForge) {
        if (value.size() > 10) invalid("CurseForge identifier exceeds uint32");
        auto parsed = std::stoull(value);
        if (!parsed || parsed > std::numeric_limits<std::uint32_t>::max()) invalid("Invalid CurseForge identifier");
        return std::to_string(parsed);
    }
    return value;
}
std::string cf_id(const json_t* value, const char* key) {
    auto n = num(value, key);
    if (!n || n > std::numeric_limits<std::uint32_t>::max()) invalid(std::string("Invalid ") + key);
    return std::to_string(n);
}
std::string hex_hash(const std::string& value, std::size_t length) {
    if (value.empty()) return {};
    if (value.size() != length) invalid("Invalid checksum length");
    for (unsigned char c : value)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
            invalid("Invalid checksum encoding");
    return lower(value);
}
Hashes hashes(const json_t* value, bool mrpack = false) {
    object(value, "hashes");
    Hashes out;
    out.sha1 = hex_hash(str(value, "sha1", false), 40);
    out.sha512 = hex_hash(str(value, "sha512", false), 128);
    out.sha256 = hex_hash(str(value, "sha256", false), 64);
    out.md5 = hex_hash(str(value, "md5", false), 32);
    if ((mrpack && (out.sha1.empty() || out.sha512.empty())) || out.empty())
        throw Error("pack.hash_missing", mrpack ? "Modrinth index requires SHA1 and SHA512" : "File metadata has no supported checksum");
    return out;
}
Hashes cf_hashes(const json_t* value) {
    Hashes out;
    auto values = array(member(value, "hashes"), "hashes");
    std::size_t i; json_t* item;
    json_array_foreach(values, i, item) {
        object(item, "file hash");
        auto algorithm = num(item, "algo");
        if (algorithm == 1) out.sha1 = hex_hash(str(item, "value"), 40);
        else if (algorithm == 2) out.md5 = hex_hash(str(item, "value"), 32);
    }
    if (out.empty()) throw Error("pack.hash_missing", "CurseForge did not provide a supported file checksum");
    return out;
}
std::string dump(const json_t* value) {
    char* data = json_dumps(value, JSON_COMPACT | JSON_ENCODE_ANY);
    if (!data) throw Error("catalog.memory", "Unable to encode API request");
    std::string out(data);
    free(data);
    return out;
}
std::vector<std::string> strings(const json_t* value, const char* label, bool required = true) {
    if (!value && !required) return {};
    array(value, label);
    std::vector<std::string> out;
    std::size_t i; json_t* item;
    json_array_foreach(value, i, item) out.push_back(text(item, label));
    return out;
}
LoaderKind loader_kind(const std::string& name) {
    auto key = lower(name);
    if (key == "fabric" || key == "fabric-loader" || key == "net.fabricmc.fabric-loader") return LoaderKind::Fabric;
    if (key == "quilt" || key == "quilt-loader" || key == "org.quiltmc.quilt-loader") return LoaderKind::Quilt;
    if (key == "forge" || key == "net.minecraftforge") return LoaderKind::Forge;
    if (key == "neoforge" || key == "net.neoforged.neoforge" || key == "net.neoforged") return LoaderKind::NeoForge;
    if (key == "vanilla") return LoaderKind::Vanilla;
    return LoaderKind::Unknown;
}
const char* mr_type(ContentKind kind) {
    switch (kind) {
    case ContentKind::Mod: return "mod";
    case ContentKind::ResourcePack: return "resourcepack";
    case ContentKind::ShaderPack: return "shader";
    default: return "modpack";
    }
}
unsigned cf_class(ContentKind kind) {
    switch (kind) {
    case ContentKind::Mod: return 6;
    case ContentKind::ResourcePack: return 12;
    case ContentKind::ShaderPack: return 6552;
    default: return 4471;
    }
}
bool wanted_file(ContentKind kind, const std::string& filename) {
    auto name = lower(filename);
    if (kind == ContentKind::Mod) return ends_with(name, ".jar");
    if (kind == ContentKind::Modpack) return ends_with(name, ".mrpack") || ends_with(name, ".zip");
    return ends_with(name, ".zip");
}
Error no_file(ContentKind kind) {
    if (kind == ContentKind::Mod) return Error("catalog.no_mod_jar", "Version does not contain a mod JAR");
    if (kind == ContentKind::Modpack) return Error("catalog.no_pack_archive", "Version does not contain a modpack archive");
    return Error("catalog.no_pack_zip", "Version does not contain a ZIP pack");
}
void select_loader(PackPlan& plan, LoaderKind kind, const std::string& version) {
    if (kind == LoaderKind::Unknown) {
        plan.state = InstanceState::Unsupported;
        plan.notices.push_back("The pack requires an unrecognized loader or component");
        return;
    }
    if (version.empty()) invalid("Loader version is empty");
    if (plan.game.loader.kind != LoaderKind::Vanilla &&
        (plan.game.loader.kind != kind || plan.game.loader.version != version))
        throw Error("pack.conflicting_loaders", "Pack declares conflicting loader versions");
    plan.game.loader = {kind, version};
}
void account(PackPlan& plan) {
    std::set<std::string> paths;
    std::uint64_t total = 0;
    for (const auto& item : plan.downloads) {
        if (!item.path.empty() && !paths.insert(lower(relative_path(item.path))).second)
            throw Error("pack.duplicate_path", "Pack contains colliding destination paths");
        if (item.bytes > std::numeric_limits<std::uint64_t>::max() - total) invalid("Pack size overflows accounting");
        total += item.bytes;
    }
    plan.expanded_bytes = total;
    plan.required_free_bytes = total;
    if (plan.game.minecraft_version.empty()) throw Error("pack.no_minecraft", "Pack has no Minecraft version");
    if (plan.game.minecraft_version.size() > 80) invalid("Minecraft version is too long");
    plan.game.java_major = 21; // Runtime selection is fixed; version compatibility is checked by preparation.
}
Download mr_file(const json_t* value) {
    object(value, "version file");
    Download out;
    out.file_name = file_name(str(value, "filename"));
    out.path = out.file_name;
    out.bytes = num(value, "size");
    out.hashes = hashes(member(value, "hashes"));
    out.urls.push_back(approved_url(str(value, "url")));
    return out;
}
CatalogProject mr_project(const json_t* value, ContentKind kind = ContentKind::Modpack) {
    object(value, "Modrinth project");
    CatalogProject out;
    out.provider = Provider::Modrinth;
    out.id = str(value, member(value, "project_id") ? "project_id" : "id");
    identifier(out.id, Provider::Modrinth);
    out.slug = str(value, "slug", false);
    out.name = str(value, "title");
    out.summary = str(value, "description", false);
    out.author = str(value, "author", false);
    out.icon_url = str(value, "icon_url", false);
    out.downloads = num(value, "downloads", false);
    auto type=str(value,"project_type",false);
    if(!type.empty() && type!=mr_type(kind))throw Error("catalog.project_type","Unexpected project type");
    out.website = std::string("https://modrinth.com/")+mr_type(kind)+"/"+encode(out.slug.empty() ? out.id : out.slug);
    return out;
}
CatalogProject cf_project(const json_t* value, ContentKind kind = ContentKind::Modpack) {
    object(value, "CurseForge project");
    CatalogProject out;
    out.provider = Provider::CurseForge;
    if(member(value,"classId") && num(value,"classId")!=cf_class(kind))
        throw Error("catalog.project_type","Unexpected CurseForge project type");
    out.id = cf_id(value, "id");
    if (member(value, "gameId") && num(value, "gameId") != 432) invalid("CurseForge project belongs to another game");
    out.slug = str(value, "slug", false);
    out.name = str(value, "name");
    out.summary = str(value, "summary", false);
    out.downloads = num(value, "downloadCount", false);
    auto logo = member(value, "logo");
    if (json_is_object(logo)) out.icon_url = str(logo, "thumbnailUrl", false);
    auto links = member(value, "links");
    if (json_is_object(links)) out.website = str(links, "websiteUrl", false);
    auto authors = member(value, "authors");
    if (json_is_array(authors)) {
        std::size_t i; json_t* author;
        json_array_foreach(authors, i, author) {
            if (!out.author.empty()) out.author += ", ";
            out.author += str(object(author, "author"), "name");
        }
    }
    return out;
}
std::string manual_link(const json_t* project, const std::string& file_id) {
    auto links = member(project, "links");
    if (!json_is_object(links)) return {};
    auto website = str(links, "websiteUrl", false);
    if (!https_url(website)) return {};
    while (!website.empty() && website.back() == '/') website.pop_back();
    return website + "/download/" + file_id; // Human-facing official project page, never a fabricated CDN URL.
}
bool distribution_allowed(const json_t* project) {
    return flag(project, "allowModDistribution", true);
}
Download cf_file(const json_t* value, const std::string& expected_project, const json_t* project) {
    object(value, "CurseForge file");
    Download out;
    out.project_id = cf_id(value, "modId");
    out.file_id = cf_id(value, "id");
    if (out.project_id != expected_project) throw Error("catalog.identity_mismatch", "CurseForge returned a file from another project");
    out.file_name = file_name(str(value, "fileName"));
    out.path = out.file_name;
    out.bytes = num(value, "fileLength");
    out.hashes = cf_hashes(value);
    out.manual_url = manual_link(project, out.file_id);
    auto url = str(value, "downloadUrl", false);
    out.manual = !distribution_allowed(project) || !flag(value, "isAvailable", true) || url.empty();
    if (!out.manual) out.urls.push_back(approved_url(url));
    return out;
}
CatalogVersion mr_version(const json_t* value, const std::string& expected_project, ContentKind kind = ContentKind::Modpack) {
    object(value, "Modrinth version");
    CatalogVersion out;
    out.provider = Provider::Modrinth;
    out.id = str(value, "id");
    identifier(out.id, out.provider);
    out.project_id = str(value, "project_id");
    if (!expected_project.empty() && out.project_id != expected_project)
        throw Error("catalog.identity_mismatch", "Modrinth returned a version from another project");
    out.name = str(value, "name", false);
    if (out.name.empty()) out.name = str(value, "version_number");
    out.published = str(value, "date_published", false);
    out.minecraft_versions = strings(member(value, "game_versions"), "game_versions");
    for (const auto& name : strings(member(value, "loaders"), "loaders", false))
        out.loaders.push_back(loader_kind(name));
    auto files = array(member(value, "files"), "version files");
    const json_t* selected = nullptr;
    std::size_t i; json_t* item;
    json_array_foreach(files, i, item) {
        object(item, "version file");
        if (!wanted_file(kind, str(item, "filename"))) continue;
        if (!selected || flag(item, "primary", false)) selected = item;
        if (flag(item, "primary", false)) break;
    }
    if (!selected) throw no_file(kind);
    out.archive = mr_file(selected);
    out.archive.project_id = out.project_id;
    out.archive.file_id = out.id;
    auto dependencies = member(value, "dependencies");
    if (dependencies && !json_is_null(dependencies)) {
        array(dependencies, "dependencies");
        json_array_foreach(dependencies, i, item) {
            object(item, "dependency");
            if (str(item, "dependency_type", false) != "required") continue;
            Dependency dependency{str(item, "project_id", false), str(item, "version_id", false)};
            if (dependency.project_id.empty() && dependency.version_id.empty()) continue;
            if (!dependency.project_id.empty()) identifier(dependency.project_id, Provider::Modrinth);
            if (!dependency.version_id.empty()) identifier(dependency.version_id, Provider::Modrinth);
            out.dependencies.push_back(std::move(dependency));
        }
    }
    return out;
}
CatalogVersion cf_version(const json_t* value, const std::string& expected_project, const json_t* project, ContentKind kind = ContentKind::Modpack) {
    if (flag(value, "isServerPack", false)) throw Error("catalog.server_pack", "A dedicated-server archive is not a client modpack");
    CatalogVersion out;
    out.provider = Provider::CurseForge;
    out.archive = cf_file(value, expected_project, project);
    out.id = out.archive.file_id;
    out.project_id = out.archive.project_id;
    out.name = str(value, "displayName", false);
    if (out.name.empty()) out.name = out.archive.file_name;
    out.published = str(value, "fileDate", false);
    for (const auto& version : strings(member(value, "gameVersions"), "gameVersions", false)) {
        auto loader = loader_kind(version);
        if (loader != LoaderKind::Unknown) out.loaders.push_back(loader);
        else out.minecraft_versions.push_back(version);
    }
    if(!wanted_file(kind,out.archive.file_name))throw no_file(kind);
    auto dependencies=member(value,"dependencies");
    if(dependencies && !json_is_null(dependencies)) {
        array(dependencies,"dependencies");
        std::size_t i; json_t* item;
        json_array_foreach(dependencies,i,item) {
            object(item,"dependency");
            // relationType 3 is RequiredDependency in the CurseForge API.
            if(num(item,"relationType",false)!=3)continue;
            out.dependencies.push_back({cf_id(item,"modId"),{}});
        }
    }
    return out;
}
std::string trim(const std::string& value) {
    auto begin = value.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return {};
    return value.substr(begin, value.find_last_not_of(" \t\r\n") - begin + 1);
}
std::map<std::string, std::string> instance_cfg(const std::string& value) {
    if (value.size() > 1024 * 1024) invalid("Instance configuration exceeds the size limit");
    std::map<std::string, std::string> out;
    std::istringstream input(value);
    std::string line;
    while (std::getline(input, line)) {
        line = trim(line);
        if (line.empty() || line.front() == '#' || line.front() == ';' || line.front() == '[') continue;
        auto split = line.find('=');
        if (split != std::string::npos) out[trim(line.substr(0, split))] = trim(line.substr(split + 1));
    }
    return out;
}
} // namespace

std::string packwizUrlFromCommand(const std::string& command) {
    std::vector<std::string> tokens;
    std::string current;
    bool quoted = false, any = false;
    for (char c : command) {
        if (c == '\\') continue; // QSettings may escape the inner quotes.
        if (c == '"') { quoted = !quoted; any = true; continue; }
        if (!quoted && (c == ' ' || c == '\t')) {
            if (any) tokens.push_back(current);
            current.clear(); any = false;
            continue;
        }
        current += c; any = true;
    }
    if (any) tokens.push_back(current);
    bool installer = false;
    std::string url;
    for (const auto& token : tokens) {
        if (lower(token).find("packwiz-installer") != std::string::npos) installer = true;
        auto key = lower(token);
        if ((key.rfind("https://", 0) == 0 || key.rfind("http://", 0) == 0) && key.size() > 5 && key.compare(key.size() - 5, 5, ".toml") == 0) url = token;
    }
    return installer ? url : std::string();
}

PackPlan parse_pack_manifest(PackKind kind, const std::string& input, const std::string& cfg) {
    auto json = parse_json(input);
    const auto* root = object(json.get(), "manifest");
    PackPlan plan;
    plan.kind = kind;
    if (kind == PackKind::Modrinth) {
        if (num(root, "formatVersion") != 1 || str(root, "game") != "minecraft")
            throw Error("pack.unsupported_format", "Unsupported Modrinth modpack format");
        plan.name = str(root, "name");
        plan.version = str(root, "versionId");
        plan.override_roots = {"overrides", "client-overrides"};
        // Jansson 2.11's read-only iterator API predates const-correct signatures.
        auto* dependencies = const_cast<json_t*>(object(member(root, "dependencies"), "dependencies"));
        const char* key; json_t* value;
        json_object_foreach(dependencies, key, value) {
            auto version = text(value, "dependency version");
            if (std::string(key) == "minecraft") plan.game.minecraft_version = version;
            else {
                auto loader = loader_kind(key);
                select_loader(plan, loader, version);
                if (loader == LoaderKind::Unknown) plan.notices.push_back(std::string("Unsupported dependency: ") + key);
            }
        }
        auto files = array(member(root, "files"), "files");
        std::size_t i; json_t* item;
        json_array_foreach(files, i, item) {
            object(item, "file");
            Download file;
            file.path = relative_path(str(item, "path"));
            file.file_name = basename(file.path);
            auto env = member(item, "env");
            if (env) {
                object(env, "file environment");
                auto side = str(env, "client", false);
                if (side.empty() || side == "unsupported") continue;
                if (side != "required" && side != "optional") invalid("Unknown client environment policy");
                file.optional = side == "optional";
                auto server = str(env, "server", false);
                if (!server.empty() && server != "required" && server != "optional" && server != "unsupported") invalid("Unknown server environment policy");
            }
            file.hashes = hashes(member(item, "hashes"), true);
            file.bytes = num(item, "fileSize");
            for (const auto& url : strings(member(item, "downloads"), "downloads")) file.urls.push_back(approved_url(url));
            if (file.urls.empty() || file.urls.size() > 16) invalid("File must have 1..16 download locations");
            plan.downloads.push_back(std::move(file));
        }
    } else if (kind == PackKind::CurseForge) {
        if (str(root, "manifestType") != "minecraftModpack" || num(root, "manifestVersion") != 1)
            throw Error("pack.unsupported_format", "Unsupported CurseForge modpack format");
        plan.name = str(root, "name", false);
        if (plan.name.empty()) plan.name = "Imported CurseForge pack";
        plan.version = str(root, "version", false);
        auto mc = object(member(root, "minecraft"), "minecraft");
        plan.game.minecraft_version = str(mc, "version");
        auto loaders = member(mc, "modLoaders");
        if (loaders) {
            array(loaders, "modLoaders");
            std::size_t i; json_t* loader;
            json_array_foreach(loaders, i, loader) {
                auto id = str(object(loader, "loader"), "id");
                auto split = id.find('-');
                if (split == std::string::npos) { select_loader(plan, LoaderKind::Unknown, id); continue; }
                select_loader(plan, loader_kind(id.substr(0, split)), id.substr(split + 1));
                flag(loader, "primary", false);
            }
        }
        auto overrides = str(root, "overrides", false);
        plan.override_roots = {relative_path(overrides.empty() ? "overrides" : overrides)};
        auto files = array(member(root, "files"), "files");
        std::set<std::string> ids;
        std::size_t i; json_t* item;
        json_array_foreach(files, i, item) {
            object(item, "file");
            Download file;
            file.project_id = cf_id(item, "projectID");
            file.file_id = cf_id(item, "fileID");
            file.optional = !flag(item, "required", true);
            if (!ids.insert(file.file_id).second) invalid("Duplicate CurseForge file identifier");
            plan.downloads.push_back(std::move(file));
        }
    } else {
        if (num(root, "formatVersion") != 1) throw Error("pack.unsupported_format", "Unsupported MultiMC component format");
        auto settings = instance_cfg(cfg);
        plan.name = settings.count("name") && !settings["name"].empty() ? settings["name"] : "Imported instance";
        plan.override_roots = {".minecraft", "minecraft"};
        auto components = array(member(root, "components"), "components");
        std::size_t i; json_t* component;
        bool minecraft = false;
        json_array_foreach(components, i, component) {
            object(component, "component");
            auto uid = str(component, "uid");
            auto version = str(component, "version");
            if (uid == "net.minecraft") {
                if (minecraft) invalid("Duplicate Minecraft component");
                minecraft = true;
                plan.game.minecraft_version = version;
            } else if (uid == "org.lwjgl3" || uid == "org.lwjgl") {
                plan.notices.push_back("The bundled Horizon LWJGL runtime is retained");
            } else if (uid == "net.fabricmc.intermediary" || uid == "org.quiltmc.hashed") {
                // Mapping components are implied by the Fabric/Quilt loader profile.
            } else {
                auto loader = loader_kind(uid);
                select_loader(plan, loader, version);
                if (loader == LoaderKind::Unknown) plan.notices.push_back("Unsupported component: " + uid);
            }
        }
        auto icon = settings["iconKey"];
        if (!icon.empty() && icon.size() <= 64 &&
            icon.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") == std::string::npos)
            plan.icon = icon;
        auto command = settings["PreLaunchCommand"];
        if (settings["OverrideCommands"] != "false") plan.packwiz_url = packwizUrlFromCommand(command);
        if (!plan.packwiz_url.empty()) {
            if (plan.packwiz_url.rfind("https://", 0) != 0) {
                plan.state = InstanceState::Unsupported;
                plan.notices.push_back("packwiz pack must be served over HTTPS: " + plan.packwiz_url);
            } else
                plan.notices.push_back("packwiz: files are downloaded from " + plan.packwiz_url + " and updated before each launch");
        } else if (!command.empty())
            plan.notices.push_back("Pre-launch command is not run on Switch: " + command);
        plan.notices.push_back("Desktop Java paths are not imported; runtime is Java 21");
    }
    account(plan);
    return plan;
}

CatalogClient::CatalogClient(HttpClient& http, const Settings& settings)
    : http_(http), curseforge_key_(settings.curseforge_key) {
    for (unsigned char c : curseforge_key_)
        if (c < 32 || c == 127) throw Error("catalog.cf_key_invalid", "CurseForge configuration contains an invalid header value");
}

HttpResponse CatalogClient::metadata(Provider provider, const std::string& path,
                                     const std::string& body, const std::atomic_bool* cancelled) {
    cancellation(cancelled);
    HttpRequest request;
    request.url = std::string(provider == Provider::Modrinth ? modrinth_api : curseforge_api) + path;
    request.headers["Accept"] = "application/json";
    if (provider == Provider::CurseForge) {
        if(curseforge_key_.empty()) {
            // Public resolver used by PolyMC. Never log the response/token or
            // send a user's configured key to this service. An explicit key
            // remains the first choice and bypasses the resolver entirely.
            HttpRequest key_request;
            key_request.url="https://cf.polymc.org/api";
            key_request.max_bytes=4096;
            key_request.headers["Accept"]="application/json";
            auto response=http_.request(key_request,cancelled);
            if(response.status!=200)throw Error("catalog.cf_key_unavailable","CurseForge key service is unavailable; an optional personal key can be set in settings");
            auto key=parse_json(response.body);
            if(!flag(key.get(),"ok",false))throw Error("catalog.cf_key_unavailable","CurseForge key service did not provide access");
            auto token=str(key.get(),"token");
            if(token.empty() || token.size()>512)throw Error("catalog.cf_key_invalid","CurseForge key service returned an invalid token");
            for(unsigned char c:token)if(c<33 || c==127)throw Error("catalog.cf_key_invalid","CurseForge key service returned an invalid token");
            curseforge_key_=std::move(token);
        }
        request.headers["x-api-key"] = curseforge_key_;
    }
    if (!body.empty()) {
        request.method = "POST";
        request.body = body;
        request.headers["Content-Type"] = "application/json";
    }
    auto response = http_.request(request, cancelled);
    cancellation(cancelled);
    if (response.body.size() > request.max_bytes) throw Error("catalog.response_too_large", "Catalog response exceeds its size limit");
    if (response.status == 401 || response.status == 403) throw Error("catalog.unauthorized", "Catalog access was not authorized");
    if (response.status == 429) throw Error("catalog.rate_limited", "Catalog rate limit reached; retry after its reset interval");
    if (response.status == 404) throw Error("catalog.not_found", "Catalog item was not found");
    if (response.status == 410) throw Error("catalog.api_retired", "Catalog API version is no longer available");
    if (response.status != 200) throw Error("catalog.http_status", "Catalog returned HTTP " + std::to_string(response.status));
    return response;
}

SearchPage CatalogClient::search(Provider provider, const SearchQuery& query, const std::atomic_bool* cancelled) {
    if (query.text.size() > 512 || query.minecraft_version.size() > 80 || !query.limit || query.limit > 50)
        throw Error("catalog.invalid_query", "Invalid search length or page size");
    std::string path;
    if (provider == Provider::Modrinth) {
        Json facets(json_array());
        Json kind(json_pack("[s]", (std::string("project_type:")+mr_type(query.kind)).c_str()));
        json_array_append(facets.get(), kind.get());
        if(query.kind==ContentKind::Mod && query.loader!=LoaderKind::Unknown) {
            const char* tag=query.loader==LoaderKind::Fabric?"fabric":query.loader==LoaderKind::Quilt?"quilt":
                            query.loader==LoaderKind::Forge?"forge":query.loader==LoaderKind::NeoForge?"neoforge":"vanilla";
            Json loaders(json_pack("[s]",(std::string("categories:")+tag).c_str()));
            if(query.loader==LoaderKind::Quilt)json_array_append_new(loaders.get(),json_string("categories:fabric"));
            json_array_append(facets.get(),loaders.get());
        }
        if (!query.minecraft_version.empty()) {
            Json versions(json_pack("[s]", ("versions:" + query.minecraft_version).c_str()));
            json_array_append(facets.get(), versions.get());
        }
        const char* index = query.sort == SearchSort::Downloads ? "downloads" : query.sort == SearchSort::Updated ? "updated" : "relevance";
        path = "/search?query=" + encode(query.text) + "&offset=" + std::to_string(query.offset)
            + "&limit=" + std::to_string(query.limit) + "&index=" + index + "&facets=" + encode(dump(facets.get()));
    } else {
        if (query.offset > 10000 - query.limit) throw Error("catalog.invalid_query", "CurseForge search limit is 10000 results");
        // CurseForge sort fields: 1 featured, 3 last updated, 6 total downloads.
        const char* sort = query.sort == SearchSort::Downloads ? "6" : query.sort == SearchSort::Updated ? "3" : "2";
        path = "/mods/search?gameId=432&classId="+std::to_string(cf_class(query.kind))+"&searchFilter=" + encode(query.text)
            + "&index=" + std::to_string(query.offset) + "&pageSize=" + std::to_string(query.limit)
            + "&sortField=" + sort + "&sortOrder=desc";
        if (!query.minecraft_version.empty()) path += "&gameVersion=" + encode(query.minecraft_version);
        if(query.kind==ContentKind::Mod && query.loader!=LoaderKind::Unknown) {
            unsigned loader=query.loader==LoaderKind::Forge?1:query.loader==LoaderKind::Fabric?4:
                            query.loader==LoaderKind::Quilt?5:query.loader==LoaderKind::NeoForge?6:0;
            if(loader)path+="&modLoaderType="+std::to_string(loader);
        }
    }
    auto data = parse_json(metadata(provider, path, {}, cancelled).body);
    auto root = object(data.get(), "search response");
    SearchPage page;
    page.offset = query.offset;
    auto items = array(member(root, provider == Provider::Modrinth ? "hits" : "data"), "search results");
    std::size_t i; json_t* item;
    json_array_foreach(items, i, item) page.projects.push_back(provider == Provider::Modrinth ? mr_project(item,query.kind) : cf_project(item,query.kind));
    auto total = provider == Provider::Modrinth ? num(root, "total_hits") : num(object(member(root, "pagination"), "pagination"), "totalCount");
    page.total = static_cast<std::uint32_t>(std::min<std::uint64_t>(total, std::numeric_limits<std::uint32_t>::max()));
    auto end = static_cast<std::uint64_t>(page.offset) + page.projects.size();
    page.has_more = !page.projects.empty() && end < page.total && (provider != Provider::CurseForge || end < 10000);
    return page;
}

CatalogProject CatalogClient::project(Provider provider, const std::string& id, const std::atomic_bool* cancelled, ContentKind kind) {
    auto wanted = identifier(id, provider);
    auto response = parse_json(metadata(provider, provider == Provider::Modrinth ? "/project/" + wanted : "/mods/" + wanted, {}, cancelled).body);
    auto result = provider == Provider::Modrinth ? mr_project(response.get(),kind) : cf_project(member(response.get(), "data"),kind);
    if (provider == Provider::CurseForge && result.id != wanted)
        throw Error("catalog.identity_mismatch", "CurseForge returned another project");
    return result;
}

std::vector<CatalogVersion> CatalogClient::versions(Provider provider, const std::string& id,
                                                   const std::string& minecraft_version,
                                                   const std::atomic_bool* cancelled, ContentKind kind, LoaderKind requested_loader) {
    auto wanted = identifier(id, provider);
    if (minecraft_version.size() > 80) throw Error("catalog.invalid_query", "Minecraft version is too long");
    std::vector<CatalogVersion> result;
    if (provider == Provider::Modrinth) {
        auto project_data = project(provider, wanted, cancelled,kind); // Resolve a mutable slug to stable project ID.
        auto path = "/project/" + project_data.id + "/version";
        if (!minecraft_version.empty()) {
            Json versions(json_pack("[s]", minecraft_version.c_str()));
            path += "?game_versions=" + encode(dump(versions.get()));
        }
        auto data = parse_json(metadata(provider, path, {}, cancelled).body);
        auto list = array(data.get(), "versions");
        std::size_t i; json_t* item;
        json_array_foreach(list, i, item) {
            // Pack projects can publish versions without a usable archive.
            try { result.push_back(mr_version(item, project_data.id,kind)); }
            catch (const Error& error) { if (error.code.compare(0, 14, "catalog.no_mod") && error.code.compare(0, 15, "catalog.no_pack")) throw; }
        }
    } else {
        auto project_data = parse_json(metadata(provider, "/mods/" + wanted, {}, cancelled).body);
        auto details = object(member(project_data.get(), "data"), "project");
        if (cf_id(details, "id") != wanted) throw Error("catalog.identity_mismatch", "CurseForge returned another project");
        for (std::uint32_t offset = 0; offset < 10000; offset += 50) {
            auto path = "/mods/" + wanted + "/files?pageSize=50&index=" + std::to_string(offset);
            if (!minecraft_version.empty()) path += "&gameVersion=" + encode(minecraft_version);
            auto page = parse_json(metadata(provider, path, {}, cancelled).body);
            auto list = array(member(page.get(), "data"), "versions");
            std::size_t i; json_t* item;
            json_array_foreach(list, i, item) {
                if (flag(item, "isServerPack", false)) continue;
                try { result.push_back(cf_version(item, wanted, details,kind)); }
                catch (const Error& error) { if (error.code.compare(0, 14, "catalog.no_mod") && error.code.compare(0, 15, "catalog.no_pack")) throw; }
            }
            auto pagination = member(page.get(), "pagination");
            if (json_array_size(list) < 50 || (json_is_object(pagination) && offset + 50 >= num(pagination, "totalCount"))) break;
        }
    }
    if(kind!=ContentKind::Modpack) result.erase(std::remove_if(result.begin(),result.end(),[&](const auto& version) {
        bool game=minecraft_version.empty() || std::find(version.minecraft_versions.begin(),version.minecraft_versions.end(),minecraft_version)!=version.minecraft_versions.end();
        // Resource and shader packs are not tied to a mod loader.
        bool loader=kind!=ContentKind::Mod || requested_loader==LoaderKind::Unknown || std::find(version.loaders.begin(),version.loaders.end(),requested_loader)!=version.loaders.end();
        if(requested_loader==LoaderKind::Quilt && std::find(version.loaders.begin(),version.loaders.end(),LoaderKind::Fabric)!=version.loaders.end())loader=true;
        return !game || !loader;
    }),result.end());
    std::stable_sort(result.begin(), result.end(), [](const auto& a, const auto& b) { return a.published > b.published; });
    return result;
}

CatalogVersion CatalogClient::version(Provider provider, const std::string& project_id,
                                      const std::string& version_id, const std::atomic_bool* cancelled, ContentKind kind) {
    auto wanted_project = identifier(project_id, provider);
    auto wanted_version = identifier(version_id, provider);
    if (provider == Provider::Modrinth) {
        auto actual_project = project(provider, wanted_project, cancelled,kind);
        auto data = parse_json(metadata(provider, "/version/" + wanted_version, {}, cancelled).body);
        auto result = mr_version(data.get(), actual_project.id,kind);
        if (result.id != wanted_version) throw Error("catalog.identity_mismatch", "Modrinth returned another version");
        return result;
    }
    auto project_data = parse_json(metadata(provider, "/mods/" + wanted_project, {}, cancelled).body);
    auto details = object(member(project_data.get(), "data"), "project");
    if (cf_id(details, "id") != wanted_project) throw Error("catalog.identity_mismatch", "CurseForge returned another project");
    auto data = parse_json(metadata(provider, "/mods/" + wanted_project + "/files/" + wanted_version, {}, cancelled).body);
    auto result = cf_version(member(data.get(), "data"), wanted_project, details,kind);
    if (result.id != wanted_version) throw Error("catalog.identity_mismatch", "CurseForge returned another file");
    return result;
}

std::map<std::string, CatalogVersion> CatalogClient::identify(const std::map<std::string, LocalFile>& files, ContentKind kind,
                                                              const std::atomic_bool* cancelled) {
    std::map<std::string, CatalogVersion> result;
    std::map<std::string, std::vector<std::string>> by_sha1;
    std::map<std::uint32_t, std::vector<std::string>> by_fingerprint;
    for (const auto& file : files) {
        if (file.second.sha1.size() == 40) by_sha1[lower(file.second.sha1)].push_back(file.first);
    }
    for (auto it = by_sha1.begin(); it != by_sha1.end();) {
        Json body(json_object()), hashes(json_array());
        for (unsigned count = 0; count < 100 && it != by_sha1.end(); ++count, ++it)
            json_array_append_new(hashes.get(), json_string(it->first.c_str()));
        json_object_set(body.get(), "hashes", hashes.get());
        json_object_set_new(body.get(), "algorithm", json_string("sha1"));
        auto data = parse_json(metadata(Provider::Modrinth, "/version_files", dump(body.get()), cancelled).body);
        const char* hash; json_t* value;
        json_object_foreach(const_cast<json_t*>(object(data.get(), "version files")), hash, value) {
            auto owners = by_sha1.find(lower(hash));
            if (owners == by_sha1.end()) continue;
            try {
                auto version = mr_version(value, {}, kind);
                for (const auto& key : owners->second) result[key] = version;
            } catch (const Error& error) {
                if (error.code == "catalog.cancelled") throw;
            }
        }
    }
    {
        for (const auto& file : files)
            if (!result.count(file.first) && file.second.fingerprint) by_fingerprint[file.second.fingerprint].push_back(file.first);
        if (!by_fingerprint.empty()) {
            Json body(json_object()), list(json_array());
            for (const auto& item : by_fingerprint) json_array_append_new(list.get(), json_integer(item.first));
            json_object_set(body.get(), "fingerprints", list.get());
            try {
                auto data = parse_json(metadata(Provider::CurseForge, "/fingerprints/432", dump(body.get()), cancelled).body);
                auto matches = array(member(object(member(data.get(), "data"), "fingerprint data"), "exactMatches"), "exactMatches");
                std::size_t i; json_t* item;
                json_array_foreach(matches, i, item) {
                    auto file = object(member(object(item, "match"), "file"), "matched file");
                    auto fingerprint = num(file, "fileFingerprint", false);
                    auto owners = by_fingerprint.find(static_cast<std::uint32_t>(fingerprint));
                    if (owners == by_fingerprint.end()) continue;
                    CatalogVersion version;
                    version.provider = Provider::CurseForge;
                    version.project_id = cf_id(file, "modId");
                    version.id = cf_id(file, "id");
                    version.name = str(file, "displayName", false);
                    version.published = str(file, "fileDate", false);
                    for (const auto& key : owners->second) result[key] = version;
                }
            } catch (const Error& error) {
                // CurseForge recognition is a best-effort complement to Modrinth.
                if (error.code == "catalog.cancelled") throw;
            }
        }
    }
    return result;
}

std::map<std::string, CatalogVersion> CatalogClient::updates(const std::map<std::string, CatalogVersion>& identified,
                                                             const std::map<std::string, LocalFile>& files, ContentKind kind,
                                                             const std::string& minecraft_version, LoaderKind loader,
                                                             const std::atomic_bool* cancelled) {
    std::map<std::string, CatalogVersion> result;
    std::map<std::string, std::vector<std::string>> by_sha1;
    std::map<std::string, std::vector<std::string>> curseforge;
    for (const auto& item : identified) {
        auto file = files.find(item.first);
        if (item.second.provider == Provider::Modrinth && file != files.end() && file->second.sha1.size() == 40)
            by_sha1[lower(file->second.sha1)].push_back(item.first);
        else if (item.second.provider == Provider::CurseForge) curseforge[item.second.project_id].push_back(item.first);
    }
    Json loaders(json_array());
    if (kind == ContentKind::Mod) {
        const char* tag = loader == LoaderKind::Fabric ? "fabric" : loader == LoaderKind::Quilt ? "quilt" :
                          loader == LoaderKind::Forge ? "forge" : loader == LoaderKind::NeoForge ? "neoforge" : nullptr;
        if (tag) json_array_append_new(loaders.get(), json_string(tag));
        if (loader == LoaderKind::Quilt) json_array_append_new(loaders.get(), json_string("fabric"));
    } else if (kind == ContentKind::ResourcePack) json_array_append_new(loaders.get(), json_string("minecraft"));
    else for (const char* tag : {"iris", "optifine", "canvas"}) json_array_append_new(loaders.get(), json_string(tag));
    for (auto it = by_sha1.begin(); it != by_sha1.end();) {
        Json body(json_object()), hashes(json_array()), games(json_pack("[s]", minecraft_version.c_str()));
        for (unsigned count = 0; count < 100 && it != by_sha1.end(); ++count, ++it)
            json_array_append_new(hashes.get(), json_string(it->first.c_str()));
        json_object_set(body.get(), "hashes", hashes.get());
        json_object_set_new(body.get(), "algorithm", json_string("sha1"));
        json_object_set(body.get(), "loaders", loaders.get());
        json_object_set(body.get(), "game_versions", games.get());
        auto data = parse_json(metadata(Provider::Modrinth, "/version_files/update", dump(body.get()), cancelled).body);
        const char* hash; json_t* value;
        json_object_foreach(const_cast<json_t*>(object(data.get(), "update files")), hash, value) {
            auto owners = by_sha1.find(lower(hash));
            if (owners == by_sha1.end()) continue;
            try {
                auto latest = mr_version(value, {}, kind);
                for (const auto& key : owners->second)
                    if (latest.id != identified.at(key).id && latest.archive.hashes.sha1 != lower(hash)) result[key] = latest;
            } catch (const Error& error) {
                if (error.code == "catalog.cancelled") throw;
            }
        }
    }
    for (const auto& project : curseforge) {
        cancellation(cancelled);
        try {
            auto list = versions(Provider::CurseForge, project.first, minecraft_version, cancelled, kind, loader);
            if (list.empty()) continue;
            for (const auto& key : project.second) {
                const auto& current = identified.at(key);
                // Newer means published later than the installed file, not merely different.
                if (list.front().id != current.id && (current.published.empty() || list.front().published > current.published))
                    result[key] = list.front();
            }
        } catch (const Error& error) {
            if (error.code == "catalog.cancelled") throw;
        }
    }
    return result;
}

bool CatalogClient::modrinth_alternative(Download& file, const std::atomic_bool* cancelled) {
    if (file.hashes.sha1.empty()) return false;
    try {
        auto response = parse_json(metadata(Provider::Modrinth, "/version_file/" + file.hashes.sha1 + "?algorithm=sha1", {}, cancelled).body);
        auto files = array(member(object(response.get(), "alternative version"), "files"), "alternative files");
        std::size_t i; json_t* item;
        json_array_foreach(files, i, item) {
            auto candidate = mr_file(item);
            // Match the actual file, not an unrelated primary file in the same version.
            if (candidate.hashes.sha1 != file.hashes.sha1 || candidate.bytes != file.bytes) continue;
            file.urls = std::move(candidate.urls);
            if (!candidate.hashes.sha512.empty()) file.hashes.sha512 = candidate.hashes.sha512;
            file.manual = false;
            return true;
        }
    } catch (const Error& error) {
        if (error.code == "catalog.cancelled") throw;
        // A secondary public source is optional. The original file remains a
        // manual task if the exact file cannot be independently verified there.
    }
    return false;
}

void CatalogClient::resolve_pack(PackPlan& plan, const std::atomic_bool* cancelled) {
    cancellation(cancelled);
    if (plan.kind != PackKind::CurseForge || plan.downloads.empty()) return;
    if (curseforge_key_.empty()) throw Error("catalog.cf_key_missing", "CurseForge access is not configured for this build");
    PackPlan resolved = plan;
    std::map<std::string, Json> projects;
    std::map<std::string, Json> files;
    std::set<std::string> project_ids, file_ids;
    for (const auto& file : plan.downloads) {
        project_ids.insert(identifier(file.project_id, Provider::CurseForge));
        file_ids.insert(identifier(file.file_id, Provider::CurseForge));
    }
    auto fetch = [&](const std::set<std::string>& ids, const char* key, const char* endpoint,
                     std::map<std::string, Json>& output) {
        auto it = ids.begin();
        while (it != ids.end()) {
            Json body(json_object()), list(json_array());
            for (unsigned count = 0; count < 100 && it != ids.end(); ++count, ++it)
                json_array_append_new(list.get(), json_integer(static_cast<json_int_t>(std::stoull(*it))));
            json_object_set(body.get(), key, list.get());
            auto data = parse_json(metadata(Provider::CurseForge, endpoint, dump(body.get()), cancelled).body);
            auto values = array(member(data.get(), "data"), "batch data");
            std::size_t i; json_t* item;
            json_array_foreach(values, i, item) {
                object(item, "batch entry");
                auto id = cf_id(item, "id");
                if (!ids.count(id) || output.count(id)) throw Error("catalog.identity_mismatch", "Catalog returned an unexpected or duplicate identifier");
                output.emplace(id, Json(json_incref(item)));
            }
        }
    };
    fetch(project_ids, "modIds", "/mods", projects);
    fetch(file_ids, "fileIds", "/mods/files", files);
    bool manual = false;
    for (auto& original : resolved.downloads) {
        cancellation(cancelled);
        auto project = projects.find(original.project_id);
        auto found = files.find(original.file_id);
        if (project == projects.end() || found == files.end())
            throw Error("catalog.not_found", "A required CurseForge project/file was not returned by the API");
        auto file = cf_file(found->second.get(), original.project_id, project->second.get());
        file.optional = original.optional;
        auto class_id = num(project->second.get(), "classId", false);
        std::string directory;
        if (class_id == 6 || !class_id) directory = ends_with(lower(file.file_name), ".zip") ? "resourcepacks" : "mods";
        else if (class_id == 12) directory = "resourcepacks";
        else if (class_id == 6552) directory = "shaderpacks";
        else {
            resolved.state = InstanceState::Unsupported;
            resolved.notices.push_back("Unsupported CurseForge file class: " + std::to_string(class_id));
            directory = "mods";
            file.manual = true;
            file.urls.clear();
        }
        file.path = relative_path(directory + "/" + file.file_name);
        if (file.manual && flag(found->second.get(), "isAvailable", true) &&
            (class_id == 6 || class_id == 12 || class_id == 6552 || !class_id) &&
            modrinth_alternative(file, cancelled))
            resolved.notices.push_back("Identical public Modrinth file selected for " + file.file_name);
        if (file.manual) {
            manual = true;
            resolved.notices.push_back("Manual download required: " + file.file_name);
        }
        original = std::move(file);
    }
    if (manual && resolved.state != InstanceState::Unsupported) resolved.state = InstanceState::NeedsFiles;
    account(resolved);
    plan = std::move(resolved);
}
} // namespace mineswitch
