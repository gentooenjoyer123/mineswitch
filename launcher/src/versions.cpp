#include "mineswitch/versions.hpp"
#include <algorithm>
#include <jansson.h>
#include <memory>

namespace mineswitch {
namespace {
constexpr const char *manifest_url = "https://piston-meta.mojang.com/mc/game/version_manifest_v2.json";
struct JsonDelete {
    void operator()(json_t *value) const { json_decref(value); }
};
using Json = std::unique_ptr<json_t, JsonDelete>;

Json parse(const std::string &text) {
    json_error_t error{};
    Json value(json_loadb(text.data(), text.size(), 0, &error));
    if (!value)
        throw Error("versions.invalid", "Invalid response from the version server");
    return value;
}
std::string field(json_t *object, const char *key) {
    auto value = json_object_get(object, key);
    return json_is_string(value) ? std::string(json_string_value(value), json_string_length(value)) : std::string();
}
bool safeVersion(const std::string &value) {
    return !value.empty() && value.size() <= 80 &&
           value.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-_+ ") ==
               std::string::npos;
}
std::string fetch(HttpClient &http, const std::string &url, std::uint64_t limit, const std::atomic_bool *stop) {
    HttpRequest request;
    request.url = url;
    request.max_bytes = limit;
    auto response = http.request(request, stop);
    if (stop && stop->load())
        throw Error("cancelled", "Operation cancelled");
    if (response.status == 404)
        return {};
    if (response.status != 200)
        throw Error("versions.http", "The version server returned HTTP " + std::to_string(response.status));
    return response.body;
}
} // namespace

int estimatedJava(const std::string &id, const std::string &released) {
    // Year-numbered releases (26.1+) moved to Java 25.
    if (!id.empty() && id[0] >= '2' && id[0] <= '9' && id.size() > 2 && id[2] == '.')
        return 25;
    if (released >= "2024-04-23")
        return 21; // 1.20.5
    if (released >= "2021-06-08")
        return 17; // 1.17 needs 16; 17 runs it.
    return 8;
}

std::vector<GameVersion> gameVersions(Storage &storage, HttpClient &http, const std::atomic_bool *stop) {
    auto cache = joinPath(storage.root(), "cache/metadata/minecraft-manifest.json");
    std::string text;
    try {
        text = fetch(http, manifest_url, 16 * 1024 * 1024, stop);
        if (!text.empty()) {
            createDirectories(joinPath(storage.root(), "cache/metadata"));
            atomicWriteText(cache, text);
        }
    } catch (const Error &error) {
        if (error.code == "cancelled" || !pathExists(cache))
            throw;
        text = readTextFile(cache, 16 * 1024 * 1024);
    }
    auto root = parse(text);
    std::vector<GameVersion> result;
    auto list = json_object_get(root.get(), "versions");
    std::size_t i;
    json_t *item;
    json_array_foreach(list, i, item) {
        GameVersion version{field(item, "id"), field(item, "type"), field(item, "releaseTime")};
        if (!safeVersion(version.id))
            continue;
        version.java_major = estimatedJava(version.id, version.released);
        result.push_back(std::move(version));
    }
    return result;
}

std::vector<LoaderVersion> loaderVersions(HttpClient &http, LoaderKind kind, const std::string &mc,
                                          const std::atomic_bool *stop) {
    if (!safeVersion(mc))
        throw Error("versions.minecraft", "Invalid Minecraft version");
    std::vector<LoaderVersion> result;
    if (kind == LoaderKind::Fabric || kind == LoaderKind::Quilt) {
        auto url = std::string(kind == LoaderKind::Fabric ? "https://meta.fabricmc.net/v2/versions/loader/"
                                                          : "https://meta.quiltmc.org/v3/versions/loader/") + mc;
        auto text = fetch(http, url, 8 * 1024 * 1024, stop);
        if (text.empty())
            return result;
        auto root = parse(text);
        std::size_t i;
        json_t *item;
        json_array_foreach(root.get(), i, item) {
            auto loader = json_object_get(item, "loader");
            LoaderVersion version{field(loader, "version"), true};
            auto stable = json_object_get(loader, "stable");
            if (json_is_boolean(stable))
                version.stable = json_is_true(stable);
            else
                version.stable = version.version.find('-') == std::string::npos;
            if (safeVersion(version.version))
                result.push_back(std::move(version));
        }
    } else if (kind == LoaderKind::Forge) {
        auto text = fetch(http, "https://maven.minecraftforge.net/net/minecraftforge/forge/maven-metadata.xml",
                          4 * 1024 * 1024, stop);
        const std::string open = "<version>", close = "</version>", prefix = mc + "-";
        for (std::size_t at = text.find(open); at != std::string::npos; at = text.find(open, at)) {
            at += open.size();
            auto end = text.find(close, at);
            if (end == std::string::npos)
                break;
            auto value = text.substr(at, end - at);
            if (value.compare(0, prefix.size(), prefix) != 0)
                continue;
            auto version = value.substr(prefix.size());
            // Some old entries carry a trailing "-<mc>" branch suffix that the
            // installer coordinate does not use.
            if (safeVersion(version) && version.find('-') == std::string::npos)
                result.push_back({version, true});
        }
        std::reverse(result.begin(), result.end());
    } else if (kind == LoaderKind::NeoForge) {
        auto text = fetch(http, "https://maven.neoforged.net/api/maven/versions/releases/net/neoforged/neoforge",
                          4 * 1024 * 1024, stop);
        auto root = parse(text);
        // NeoForge numbers releases after the Minecraft version: 1.21.1 -> 21.1.x.
        std::string prefix;
        if (mc.compare(0, 2, "1.") == 0) {
            auto rest = mc.substr(2);
            prefix = rest.find('.') == std::string::npos ? rest + ".0." : rest + ".";
        } else
            prefix = mc + ".";
        std::size_t i;
        json_t *item;
        json_array_foreach(json_object_get(root.get(), "versions"), i, item) {
            if (!json_is_string(item))
                continue;
            std::string value = json_string_value(item);
            if (value.compare(0, prefix.size(), prefix) == 0 && safeVersion(value))
                result.push_back({value, value.find("beta") == std::string::npos});
        }
        std::reverse(result.begin(), result.end());
    }
    return result;
}
} // namespace mineswitch
