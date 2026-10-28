#include "mineswitch/prepare.hpp"
#include "mineswitch/import.hpp"
#include <jansson.h>
#include <mbedtls/md.h>
#include <minizip/zip.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#ifdef MINESWITCH_VERIFY_READ_TEST
static std::string counted_path;
static std::uint64_t counted_reads = 0, counted_bytes = 0;
extern "C" ssize_t __real_read(int fd, void* data, size_t size);
extern "C" ssize_t __wrap_read(int fd, void* data, size_t size) {
    bool counted = false;
    if (!counted_path.empty()) {
        const int saved_errno = errno;
        char descriptor[64], path[4096];
        std::snprintf(descriptor, sizeof(descriptor), "/proc/self/fd/%d", fd);
        const auto length = ::readlink(descriptor, path, sizeof(path));
        counted = length >= 0 && static_cast<std::size_t>(length) == counted_path.size() &&
                  std::memcmp(path, counted_path.data(), counted_path.size()) == 0;
        errno = saved_errno;
    }
    const auto result = __real_read(fd, data, size);
    if (counted && result > 0) { ++counted_reads; counted_bytes += static_cast<std::uint64_t>(result); }
    return result;
}
#endif

using namespace mineswitch;
namespace {
int checks = 0;
void check(bool condition, const std::string& label) {
    if (!condition) throw std::runtime_error("FAIL: " + label);
    ++checks;
}
template<class F> void rejects(const std::string& code, F action) {
    try { action(); } catch (const Error& error) {
        check(error.code == code, "expected " + code + " got " + error.code + ": " + error.what()); return;
    }
    throw std::runtime_error("FAIL: expected " + code);
}
std::string sha(const std::string& bytes) {
    std::array<unsigned char, MBEDTLS_MD_MAX_SIZE> value{};
    auto* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA1);
    if (mbedtls_md(info, reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size(), value.data())) throw std::runtime_error("hash");
    std::string out;
    for (unsigned i = 0; i < 20; ++i) { out += "0123456789abcdef"[value[i] >> 4]; out += "0123456789abcdef"[value[i] & 15]; }
    return out;
}
std::string quoted(const std::string& value) {
    json_t* j = json_stringn(value.data(), value.size());
    char* p = json_dumps(j, JSON_ENCODE_ANY); std::string out(p); free(p); json_decref(j); return out;
}
std::string artifact(const std::string& url, const std::string& bytes, const std::string& path = {}) {
    return "{\"url\":" + quoted(url) + ",\"sha1\":" + quoted(sha(bytes)) + ",\"size\":" + std::to_string(bytes.size())
        + (path.empty() ? "" : ",\"path\":" + quoted(path)) + "}";
}
void write_file(const std::string& path, const std::string& data) {
    createDirectories(path.substr(0, path.find_last_of('/')));
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    if (!out) throw std::runtime_error("fixture write failed");
}
bool has(const std::vector<std::string>& values, const std::string& value) {
    return std::find(values.begin(), values.end(), value) != values.end();
}
std::string option(const LaunchPlan& plan, const std::string& prefix) {
    for (auto it = plan.jvm_options.rbegin(); it != plan.jvm_options.rend(); ++it)
        if (it->compare(0, prefix.size(), prefix) == 0) return it->substr(prefix.size());
    throw std::runtime_error("missing option: " + prefix);
}
std::string argument(const LaunchPlan& plan, const std::string& flag) {
    auto it = std::find(plan.game_arguments.begin(), plan.game_arguments.end(), flag);
    if (it == plan.game_arguments.end() || ++it == plan.game_arguments.end()) throw std::runtime_error("missing argument " + flag);
    return *it;
}
class FakeHttp final : public HttpClient {
public:
    std::map<std::string, std::string> responses, files;
    int requests = 0, downloads = 0;
    bool network_forbidden = false, corrupt_download = false;
    HttpResponse request(const HttpRequest& request, const std::atomic_bool* token) override {
        check(!network_forbidden, "cached preparation performs no metadata network request");
        check(!token || !token->load(), "request respects cancellation");
        check(request.url.compare(0, 8, "https://") == 0, "metadata HTTPS");
        ++requests;
        auto found = responses.find(request.url);
        if (found == responses.end()) throw std::runtime_error("unexpected metadata request " + request.url);
        return {200, found->second, {}};
    }
    void download(const Download& file, const std::string& path, ProgressCallback progress,
                  const std::atomic_bool* token) override {
        check(!network_forbidden, "cached preparation performs no artifact download");
        check(!token || !token->load(), "download respects cancellation");
        check(!file.hashes.empty() && file.max_bytes != 0, "download checksum and byte budget");
        check(!pathExists(path), "exclusive temporary destination");
        auto found = files.find(file.urls.at(0));
        if (found == files.end()) throw std::runtime_error("unexpected artifact download " + file.urls.at(0));
        write_file(path, corrupt_download ? "bad" : found->second);
        ++downloads;
        if (progress) {
            const auto bytes = static_cast<std::uint64_t>(found->second.size());
            progress({"download", file.file_name, 0, bytes, 0, 1});
            progress({"download", file.file_name, bytes / 2, bytes, 0, 1});
            progress({"download", file.file_name, bytes, bytes, 1, 1});
        }
    }
};
struct Fixture {
    std::string root;
    Storage storage;
    FakeHttp http;
    Settings settings;
    Profile profile{"local", "MineSwitch"};
    Instance instance;
    std::string client = "fixture client jar bytes", library = "fixture Java library bytes";
    std::string resource = "fixture resource bytes", log = "fixture logging configuration";
    std::string index, metadata;
    static std::string temp() {
        char name[] = "/tmp/mineswitch-prepare-XXXXXX";
        auto* path = ::mkdtemp(name);
        if (!path) throw std::runtime_error("mkdtemp");
        return path;
    }
    explicit Fixture(std::string base = {}) : root(base.empty() ? temp() : std::move(base)), storage(root) {
        storage.initialize(); settings.root = root;
        instance.id = "fixture"; instance.name = "Fixture";
        instance.root = joinPath(root, "instances/fixture");
        instance.game.minecraft_version = "1.21.1";
        instance.state = InstanceState::NeedsPreparation;
        write_file(joinPath(root, "runtime/lib/modules"), "installed runtime fixture");
        for (const auto* name : {"lwjgl", "lwjgl-opengl", "lwjgl-openal"})
            write_file(joinPath(root, std::string("runtime/mineswitch/libraries/") + name + "-3.3.3-horizon.jar"), std::string("overlay ") + name);
        index = "{\"objects\":{\"fixture/a\":{\"hash\":" + quoted(sha(resource)) + ",\"size\":" + std::to_string(resource.size()) +
            "},\"fixture/same-data\":{\"hash\":" + quoted(sha(resource)) + ",\"size\":" + std::to_string(resource.size()) + "}}}";
        metadata = "{\"id\":\"1.21.1\",\"type\":\"release\",\"javaVersion\":{\"majorVersion\":21},\"mainClass\":\"net.minecraft.client.main.Main\","
            "\"downloads\":{\"client\":" + artifact("https://piston-data.mojang.com/client.jar", client) + "},"
            "\"assetIndex\":{\"id\":\"17\",\"url\":\"https://piston-meta.mojang.com/index.json\",\"sha1\":" + quoted(sha(index)) + ",\"size\":" + std::to_string(index.size()) + "},"
            "\"libraries\":[{\"name\":\"org.example:base:1.0\",\"downloads\":{\"artifact\":" + artifact("https://libraries.minecraft.net/base.jar", library, "org/example/base/1.0/base-1.0.jar") + "}},"
            "{\"name\":\"org.lwjgl:lwjgl:3.3.3\"},{\"name\":\"org.lwjgl:lwjgl:3.3.3:natives-linux\",\"rules\":[{\"action\":\"allow\",\"os\":{\"name\":\"linux\"}}]},"
            "{\"name\":\"org.example:windows-only:1\",\"rules\":[{\"action\":\"allow\",\"os\":{\"name\":\"windows\"}}]}],"
            "\"arguments\":{\"game\":[\"--username\",\"${auth_player_name}\",\"--uuid\",\"${auth_uuid}\",\"--gameDir\",\"${game_directory}\",\"--assetsDir\",\"${assets_root}\",\"--assetIndex\",\"${assets_index_name}\",\"--accessToken\",\"${auth_access_token}\","
            "{\"rules\":[{\"action\":\"allow\",\"features\":{\"is_demo_user\":true}}],\"value\":\"--demo\"},"
            "{\"rules\":[{\"action\":\"allow\",\"features\":{\"has_custom_resolution\":true}}],\"value\":[\"--width\",\"${resolution_width}\",\"--height\",\"${resolution_height}\"]}],"
            "\"jvm\":[\"-Djava.library.path=${natives_directory}\",\"-cp\",\"${classpath}\",{\"rules\":[{\"action\":\"allow\",\"os\":{\"name\":\"osx\"}}],\"value\":\"-XstartOnFirstThread\"}]},"
            "\"logging\":{\"client\":{\"argument\":\"-Dlog4j.configurationFile=${path}\",\"file\":{\"id\":\"client.xml\",\"url\":\"https://piston-data.mojang.com/log.xml\",\"sha1\":" + quoted(sha(log)) + ",\"size\":" + std::to_string(log.size()) + "}}}}";
        seed();
        http.files["https://piston-data.mojang.com/client.jar"] = client;
        http.files["https://libraries.minecraft.net/base.jar"] = library;
        http.files["https://piston-meta.mojang.com/index.json"] = index;
        http.files["https://resources.download.minecraft.net/" + sha(resource).substr(0, 2) + "/" + sha(resource)] = resource;
        http.files["https://piston-data.mojang.com/log.xml"] = log;
    }
    ~Fixture() { try { removeOwnedTree(root); } catch (...) {} }
    void seed() { write_file(joinPath(root, "runtime/mineswitch/metadata/1.21.1.json"), metadata); }
    void add_resource(const std::string& name, const std::string& bytes) {
        auto encode = [](json_t* value) {
            char* data = json_dumps(value, JSON_COMPACT);
            if (!data) throw std::runtime_error("Cannot encode resource fixture");
            std::string result(data); std::free(data); return result;
        };
        json_t* parsed = json_loads(index.c_str(), 0, nullptr);
        json_t* entry = json_object();
        json_object_set_new(entry, "hash", json_string(sha(bytes).c_str()));
        json_object_set_new(entry, "size", json_integer(static_cast<json_int_t>(bytes.size())));
        json_object_set_new(json_object_get(parsed, "objects"), name.c_str(), entry);
        index = encode(parsed); json_decref(parsed);
        parsed = json_loads(metadata.c_str(), 0, nullptr);
        auto* info = json_object_get(parsed, "assetIndex");
        json_object_set_new(info, "sha1", json_string(sha(index).c_str()));
        json_object_set_new(info, "size", json_integer(static_cast<json_int_t>(index.size())));
        metadata = encode(parsed); json_decref(parsed); seed();
        http.files["https://piston-meta.mojang.com/index.json"] = index;
        http.files["https://resources.download.minecraft.net/" + sha(bytes).substr(0, 2) + "/" + sha(bytes)] = bytes;
    }
    void legacy() {
        write_file(joinPath(root, "minecraft/client.jar"), client);
        write_file(joinPath(root, "minecraft/libraries/org/example/base/1.0/base-1.0.jar"), library);
        write_file(joinPath(root, "minecraft/assets/indexes/17.json"), index);
        write_file(joinPath(root, "minecraft/assets/objects/" + sha(resource).substr(0, 2) + "/" + sha(resource)), resource);
        write_file(joinPath(root, "minecraft/logging/client.xml"), log);
    }
};
void vanilla_test() {
    Fixture f; GamePreparer preparer(f.storage, f.http);
    auto plan = preparer.prepare(f.instance, f.settings, f.profile);
    check(f.instance.state == InstanceState::Ready && !plan.preparation_only, "vanilla Ready after verified preparation");
    check(plan.main_class == "net.minecraft.client.main.Main", "original vanilla main");
    check(f.http.requests == 0 && f.http.downloads == 5, "immutable seed and deduplicated asset downloads");
    check(argument(plan, "--username") == "MineSwitch" && argument(plan, "--uuid") == "0bda9dc7-8f1c-3639-8e34-b19c92a314a2", "actual Java-compatible offline MD5 UUID");
    check(argument(plan, "--accessToken") == "0" && !has(plan.game_arguments, "--demo"), "local profile arguments");
    check(argument(plan, "--width") == "1280" && argument(plan, "--height") == "720", "feature-selected dimensions");
    check(!has(plan.jvm_options, "-Xint") && !has(plan.jvm_options, "-XstartOnFirstThread") && !has(plan.jvm_options, "-cp"), "JNI options and real target OS rules");
    auto cp = option(plan, "-Djava.class.path=");
    check(cp.find("lwjgl-3.3.3-horizon.jar") != std::string::npos && cp.find("natives-linux") == std::string::npos, "patched LWJGL override and no foreign native jars");
    check(cp.find(';') != std::string::npos && option(plan, "--limit-modules=").find("jdk.jfr") == std::string::npos, "Horizon separator and native JFR exclusion");
    check(option(plan, "--add-modules=").find("jdk.zipfs") != std::string::npos, "ZIP filesystem retained");
    check(option(plan, "--add-modules=").find("java.net.http") != std::string::npos, "genuine Java21 HTTP module retained for mods");
    check(pathExists(joinPath(f.instance.root, "launch-plan.json")), "persistent launch plan");
    check(readTextFile(joinPath(f.instance.root, "game/options.txt")).find("simulationDistance:5") != std::string::npos, "initial game defaults");
    write_file(joinPath(f.instance.root, "game/options.txt"), "maxFps:47\n");
    f.http.network_forbidden = true;
    preparer.prepare(f.instance, f.settings, f.profile);
    check(readTextFile(joinPath(f.instance.root, "game/options.txt")) == "maxFps:47\n", "existing user options preserved");
    check(f.http.downloads == 5, "second prepare verifies and reuses shared cache");
    f.settings.heap_mib = 1900;
    auto expanded = preparer.prepare(f.instance, f.settings, f.profile);
    check(has(expanded.jvm_options, "-Xmx1900m") && has(expanded.jvm_options,"-Xms256m"),
          "standard-device maximum and256MiB initial heap reach JVM options");
    check(expanded.preparation_summary.find("fast_path=1") == std::string::npos, "a changed heap rebuilds the plan");
    f.settings.jvm_args = "-Dmineswitch.fixture=1";
    auto custom = preparer.prepare(f.instance, f.settings, f.profile, {}, nullptr, false, true);
    check(has(custom.jvm_options, "-Dmineswitch.fixture=1") && custom.preparation_summary.find("fast_path=1") == std::string::npos,
          "changed JVM options rebuild the plan and reach the JVM");
    auto again = preparer.prepare(f.instance, f.settings, f.profile, {}, nullptr, false, true);
    check(again.preparation_summary.find("fast_path=1") != std::string::npos && again.jvm_options == custom.jvm_options &&
          again.game_arguments == custom.game_arguments, "the rebuilt plan is reused unchanged on the next launch");
    f.settings.jvm_args.clear();
    f.settings.heap_mib = 1901;
    rejects("prepare_heap", [&] { preparer.prepare(f.instance, f.settings, f.profile); });
}
std::vector<Progress> asset_events(const std::vector<Progress>& events, std::uint64_t total) {
    std::vector<Progress> assets;
    bool left_phase = false;
    for (const auto& event : events) {
        if (event.stage != "assets") {
            if (!assets.empty()) left_phase = true;
            continue;
        }
        check(!left_phase, "nested file updates never replace the aggregate resource phase");
        check(event.total == total && event.completed <= total, "asset counts stay within the unique resource total");
        check(event.files_done == 0 && event.files_total == 0, "per-download file counters do not escape into resource progress");
        if (!assets.empty())
            check(event.completed >= assets.back().completed && event.completed <= assets.back().completed + 1,
                  "resource completion count is monotonic and advances only one verified object at a time");
        assets.push_back(event);
    }
    check(!assets.empty() && assets.front().completed == 0, "resource phase starts with an honest zero completed count");
    return assets;
}
void asset_progress_test() {
    Fixture f;
    const std::string second = "second resource fixture bytes", third = "third resource fixture bytes";
    f.add_resource("minecraft/sounds/fixture-two.ogg", second);
    f.add_resource("minecraft/lang/fixture-three.json", third);
    f.legacy();
    GamePreparer preparer(f.storage, f.http);
    std::vector<Progress> events;
    auto report = [&](const Progress& event) { events.push_back(event); };
    preparer.prepare(f.instance, f.settings, f.profile, report);
    auto assets = asset_events(events, 3);
    check(assets.back().completed == 3 && assets.back().detail == "Minecraft resources verified",
          "resource phase explicitly reports verified completion including its last object");
    check(f.http.downloads == 2, "mixed cached and missing resources retain existing cache/download behavior");
    bool downloading = false, logical_name = false;
    for (const auto& event : assets) {
        downloading |= event.detail.find("Downloading Minecraft resources:") == 0;
        logical_name |= event.detail.find("minecraft/sounds/fixture-two.ogg") != std::string::npos;
        check(event.detail.find(sha(f.resource)) == std::string::npos && event.detail.find(sha(second)) == std::string::npos &&
              event.detail.find(sha(third)) == std::string::npos, "asset details show readable names rather than opaque hashes");
    }
    check(downloading && logical_name, "nested transfer updates keep a readable resource action and logical index name");
    f.http.network_forbidden = true; events.clear();
    auto reused = preparer.prepare(f.instance, f.settings, f.profile, report, nullptr, false, true);
    bool resource_events = false;
    for (const auto& event : events) resource_events |= event.stage == "assets";
    check(reused.preparation_summary.find("fast_path=1") != std::string::npos && !resource_events,
          "an unchanged Ready instance reuses its verified launch plan without rescanning resources");
    events.clear();
    preparer.prepare(f.instance, f.settings, f.profile, report, nullptr, true);
    assets = asset_events(events, 3);
    check(assets.back().completed == 3 && f.http.downloads == 2, "fully cached resource verification reaches the same completion count offline");
}
void asset_progress_cancellation_test(bool after_completion) {
    Fixture f;
    f.add_resource("minecraft/sounds/fixture-two.ogg", "second resource fixture bytes");
    f.add_resource("minecraft/lang/fixture-three.json", "third resource fixture bytes");
    f.legacy();
    // All resource objects must download, while client/index/libraries/logging
    // remain cached. This makes the second object's nested updates observable.
    auto cached = joinPath(f.root, "minecraft/assets/objects/" + sha(f.resource).substr(0, 2) + "/" + sha(f.resource));
    check(::unlink(cached.c_str()) == 0, "remove only the fixture's cached resource");
    std::atomic_bool stopped{false};
    std::vector<Progress> events;
    unsigned nested_updates = 0;
    auto report = [&](const Progress& event) {
        events.push_back(event);
        if (event.stage != "assets") return;
        if (after_completion) {
            if (event.completed == 3) stopped = true;
        } else if (event.completed == 1 && event.detail.find("Downloading Minecraft resources:") == 0) {
            if (++nested_updates == 2) stopped = true; // Inside HttpClient::download, after its announcement.
        }
    };
    rejects("cancelled", [&] { GamePreparer(f.storage, f.http).prepare(f.instance, f.settings, f.profile, report, &stopped); });
    auto assets = asset_events(events, 3);
    check(stopped.load() && assets.back().completed == (after_completion ? 3u : 1u),
          "cancellation preserves the actual count of verified resource objects");
    check(f.instance.state == InstanceState::NeedsPreparation && !pathExists(joinPath(f.instance.root, "launch-plan.json")),
          "resource progress does not claim whole preparation is Ready after cancellation");
    check(f.http.downloads == (after_completion ? 3 : 2), "cancellation does not start later resource downloads");
}
void legacy_and_sd_test() {
    {
        Fixture f; f.legacy(); f.http.network_forbidden = true;
        auto plan = GamePreparer(f.storage, f.http).prepare(f.instance, f.settings, f.profile);
        check(f.http.downloads == 0, "already-installed game data never retransmitted");
        check(option(plan, "-Djava.class.path=").find("minecraft/client.jar") != std::string::npos, "existing verified client reused in place");
    }
    auto cwd = std::array<char, 4096>{};
    if (!::getcwd(cwd.data(), cwd.size())) throw std::runtime_error("getcwd");
    auto temporary = Fixture::temp();
    if (::chdir(temporary.c_str())) throw std::runtime_error("chdir");
    try {
        { Fixture f("sdmc:/switch/mineswitch"); f.legacy(); f.http.network_forbidden = true;
          auto plan = GamePreparer(f.storage, f.http).prepare(f.instance, f.settings, f.profile);
          check(plan.working_directory == "sdmc:/switch/mineswitch/instances/fixture/game", "native working directory keeps SD device prefix");
          check(argument(plan, "--gameDir") == "/switch/mineswitch/instances/fixture/game", "Java game path strips sdmc prefix");
          check(option(plan, "-Duser.dir=") == "/switch/mineswitch/instances/fixture/game", "Java working directory matches instance");
          check(option(plan, "-Djava.class.path=").find("sdmc:") == std::string::npos, "all Java classpath entries strip device prefix"); }
    } catch (...) {
        if (::chdir(cwd.data())) std::terminate();
        removeOwnedTree(temporary); throw;
    }
    if (::chdir(cwd.data())) throw std::runtime_error("restore cwd");
    removeOwnedTree(temporary);
}
void loader_test(LoaderKind kind, const std::string& version = "0.16.10", bool companion = false, bool expect_companion = false) {
    Fixture f; f.legacy();
    f.instance.game.loader = {kind, version};
    auto fabric = kind == LoaderKind::Fabric;
    auto main = fabric ? "net.fabricmc.loader.impl.launch.knot.KnotClient" : "org.quiltmc.loader.impl.launch.knot.KnotClient";
    auto endpoint = std::string(fabric ? "https://meta.fabricmc.net/v2/versions/loader/" : "https://meta.quiltmc.org/v3/versions/loader/") + "1.21.1/" + version + "/profile/json";
    auto bytes = std::string("official loader fixture bytes");
    const std::string existing_mods = "/existing-one.jar;/existing-two.jar";
    if (companion) write_file(joinPath(f.root, "runtime/mineswitch/mineswitch-compat.jar"), "bundled companion fixture");
    auto profile = "{\"id\":\"loader-fixture\",\"inheritsFrom\":\"1.21.1\",\"mainClass\":" + quoted(main)
        + ",\"arguments\":{\"jvm\":[\"-Dloader.fixture=true\"" + (companion ? "," + quoted("-Dfabric.addMods=" + existing_mods) : "")
        + "],\"game\":[]},\"libraries\":[{\"name\":" + quoted("org.example:loader:" + version) + ",\"url\":\"https://maven.fabricmc.net/\"}]}";
    f.http.responses[endpoint] = profile;
    auto url = "https://maven.fabricmc.net/org/example/loader/" + version + "/loader-" + version + ".jar";
    f.http.responses[std::string(url) + ".sha1"] = sha(bytes) + "\n";
    f.http.files[url] = bytes;
    auto plan = GamePreparer(f.storage, f.http).prepare(f.instance, f.settings, f.profile);
    check(plan.main_class == main && f.instance.state == InstanceState::Ready, "official loader main and Ready");
    check(has(plan.jvm_options, "-Dloader.fixture=true"), "loader JVM arguments retained");
    check(f.http.downloads == 1 && f.http.requests == 2, "loader metadata and published Maven checksum used");
    check(option(plan, "-Djava.class.path=").find("loader-" + version + ".jar") != std::string::npos, "loader classpath installed");
    if (companion) check(option(plan, "-Dfabric.addMods=") == existing_mods +
        (expect_companion ? ";" + joinPath(f.root, "runtime/mineswitch/mineswitch-compat.jar") : ""),
        "companion respects loader gate and preserves existing fabric.addMods entries");
}

std::uint64_t preparation_stat(const Fixture& f, const char* key) {
    json_t* plan = readStoredJson(joinPath(f.instance.root, "launch-plan.json"), kInstanceManifestMaxBytes);
    auto* field = json_object_get(json_object_get(plan, "preparation_stats"), key);
    check(json_is_integer(field) && json_integer_value(field) >= 0, "preparation statistics field is present");
    auto value = static_cast<std::uint64_t>(json_integer_value(field)); json_decref(plan); return value;
}
void receipt_cache_test() {
    Fixture f; f.legacy(); f.http.network_forbidden = true;
    const auto resource = joinPath(f.root, "minecraft/assets/objects/" + sha(f.resource).substr(0, 2) + "/" + sha(f.resource));
#ifdef MINESWITCH_VERIFY_READ_TEST
    counted_path = resource; counted_reads = counted_bytes = 0;
#endif
    auto first = GamePreparer(f.storage, f.http).prepare(f.instance, f.settings, f.profile);
    check(preparation_stat(f, "receipt_hits") == 0 && preparation_stat(f, "hash_reads") >= 8, "first preparation fully verifies published artifacts");
    const auto cold_bytes = preparation_stat(f, "bytes_hashed");
    check(pathExists(joinPath(f.root, "cache/verified-artifacts.json")), "verification receipts persisted");
#ifdef MINESWITCH_VERIFY_READ_TEST
    check(counted_reads > 0 && counted_bytes == f.resource.size(), "cold asset verification really reads file content");
    counted_reads = counted_bytes = 0;
#endif
    auto warm = GamePreparer(f.storage, f.http).prepare(f.instance, f.settings, f.profile);
    check(warm.main_class == first.main_class && warm.jvm_options == first.jvm_options && warm.game_arguments == first.game_arguments,
          "verification receipts do not change the generated launch plan");
    check(preparation_stat(f, "receipt_hits") >= 5 && preparation_stat(f, "bytes_hashed") < cold_bytes, "warm preparation reuses stat-bound published-digest receipts");
    check(preparation_stat(f, "hash_reads") == 3 && preparation_stat(f, "metadata_checks") > 0, "only unpinned runtime overlays are rehashed on unchanged warm launch");
    check(warm.preparation_summary.find("PREPARATION_STATS ") == 0 && warm.preparation_summary.find("force_validation=0") != std::string::npos,
          "launch plan returns the persisted diagnostic summary");
#ifdef MINESWITCH_VERIFY_READ_TEST
    check(counted_reads == 0 && counted_bytes == 0, "unchanged asset receipt performs zero content reads");
#endif
    auto forced = GamePreparer(f.storage, f.http).prepare(f.instance, f.settings, f.profile, {}, nullptr, true);
    check(preparation_stat(f, "receipt_hits") == 0 && preparation_stat(f, "bytes_hashed") == cold_bytes &&
          forced.preparation_summary.find("force_validation=1") != std::string::npos, "forced validation bypasses persisted receipts");
#ifdef MINESWITCH_VERIFY_READ_TEST
    check(counted_reads > 0 && counted_bytes == f.resource.size(), "forced asset validation reads content again");
    counted_reads = counted_bytes = 0;
#endif
    struct stat before{}; check(::stat(resource.c_str(), &before) == 0, "read resource timestamps");
    struct timespec times[2] = {before.st_atim, before.st_mtim}; times[1].tv_sec -= 100;
    check(::utimensat(AT_FDCWD, resource.c_str(), times, 0) == 0, "move fixture modification time backwards");
    GamePreparer(f.storage, f.http).prepare(f.instance, f.settings, f.profile);
    check(preparation_stat(f, "hash_reads") == 4, "backwards mtime invalidates the receipt");
#ifdef MINESWITCH_VERIFY_READ_TEST
    check(counted_reads > 0, "changed timestamp causes actual verification reads");
#endif
    times[1] = {0, 0}; check(::utimensat(AT_FDCWD, resource.c_str(), times, 0) == 0, "set unavailable resource timestamp");
    for (int i = 0; i < 2; ++i) {
        GamePreparer(f.storage, f.http).prepare(f.instance, f.settings, f.profile);
        check(preparation_stat(f, "hash_reads") == 4, "zero timestamps never yield reusable receipts");
    }
    auto cache = joinPath(f.root, "cache/verified-artifacts.json");
    json_t* receipt = readStoredJson(cache, kInstanceManifestMaxBytes);
    json_object_set_new(receipt, "saved_at", json_integer(static_cast<json_int_t>(std::time(nullptr)) + 86400));
    atomicWriteJson(cache, receipt); json_decref(receipt);
    GamePreparer(f.storage, f.http).prepare(f.instance, f.settings, f.profile);
    check(preparation_stat(f, "receipt_hits") == 0, "clock rollback invalidates persisted verification generation");
    check(::unlink(cache.c_str()) == 0, "remove receipt fixture");
    GamePreparer(f.storage, f.http).prepare(f.instance, f.settings, f.profile);
    check(preparation_stat(f, "receipt_hits") == 0, "missing receipts force full validation");
    write_file(cache, "invalid receipt JSON");
    GamePreparer(f.storage, f.http).prepare(f.instance, f.settings, f.profile);
    check(preparation_stat(f, "receipt_hits") == 0, "corrupt receipts are rebuilt after full validation");
#ifdef MINESWITCH_VERIFY_READ_TEST
    counted_path.clear();
#endif
}
void receipt_mutation_test() {
    Fixture f; f.legacy();
    GamePreparer preparer(f.storage, f.http);
    preparer.prepare(f.instance, f.settings, f.profile);
    const auto resource = joinPath(f.root, "minecraft/assets/objects/" + sha(f.resource).substr(0, 2) + "/" + sha(f.resource));
    check(::unlink(resource.c_str()) == 0, "delete cached artifact fixture");
    preparer.prepare(f.instance, f.settings, f.profile);
    check(f.http.downloads == 1 && readTextFile(resource) == f.resource, "missing file cannot reuse receipt and is verified on download");
    write_file(resource, std::string(f.resource.size(), 'x'));
    preparer.prepare(f.instance, f.settings, f.profile);
    check(f.http.downloads == 2 && readTextFile(resource) == f.resource, "same-size changed content invalidates metadata and is repaired");
    const auto changed = std::string(f.library.size(), 'n');
    json_t* metadata = json_loads(f.metadata.c_str(), 0, nullptr);
    auto* artifact_info = json_object_get(json_object_get(json_array_get(json_object_get(metadata, "libraries"), 0), "downloads"), "artifact");
    json_object_set_new(artifact_info, "sha1", json_string(sha(changed).c_str()));
    char* encoded = json_dumps(metadata, JSON_COMPACT); f.metadata = encoded; std::free(encoded); json_decref(metadata); f.seed();
    f.http.files["https://libraries.minecraft.net/base.jar"] = changed;
    auto plan = preparer.prepare(f.instance, f.settings, f.profile);
    check(f.http.downloads == 3 && option(plan, "-Djava.class.path=").find("cache/libraries/org/example/base") != std::string::npos,
          "changed published digest cannot reuse an old receipt even with unchanged old file metadata");
}
void receipt_symlink_test(bool ancestor) {
    Fixture f; f.legacy();
    GamePreparer(f.storage, f.http).prepare(f.instance, f.settings, f.profile);
    auto original = joinPath(f.root, "minecraft/assets/objects/" + sha(f.resource).substr(0, 2));
    if (!ancestor) original += "/" + sha(f.resource);
    const auto moved = original + "-moved";
    check(::rename(original.c_str(), moved.c_str()) == 0 && ::symlink(moved.c_str(), original.c_str()) == 0, "replace cached artifact path with symlink fixture");
    rejects("storage_path", [&] { GamePreparer(f.storage, f.http).prepare(f.instance, f.settings, f.profile); });
}
std::string zip_bytes(Fixture& fixture, const std::vector<std::pair<std::string, std::string>>& entries) {
    auto path = joinPath(fixture.root, "fixture-" + newId() + ".jar");
    zipFile archive = zipOpen64(path.c_str(), APPEND_STATUS_CREATE);
    if (!archive) throw std::runtime_error("Cannot create fixture archive");
    try {
        for (const auto& entry : entries) {
            if (zipOpenNewFileInZip64(archive, entry.first.c_str(), nullptr, nullptr, 0, nullptr, 0,
                                     nullptr, Z_DEFLATED, Z_DEFAULT_COMPRESSION, 0) != ZIP_OK ||
                zipWriteInFileInZip(archive, entry.second.data(), static_cast<unsigned>(entry.second.size())) != ZIP_OK ||
                zipCloseFileInZip(archive) != ZIP_OK) throw std::runtime_error("Cannot write fixture archive");
        }
    } catch (...) { zipClose(archive, nullptr); throw; }
    if (zipClose(archive, nullptr) != ZIP_OK) throw std::runtime_error("Cannot close fixture archive");
    return readTextFile(path);
}
std::string module_fixture() {
    // Real Java 21 output from: javac --release 21 module-info.java
    // Source: module fixture.bootstrap {}
    const std::string encoded =
        "cafebabe00000041000b07000201000b6d6f64756c652d696e666f01000a536f7572636546696c65"
        "0100106d6f64756c652d696e666f2e6a6176610100064d6f64756c65130007010011666978747572"
        "652e626f6f7473747261701300090100096a6176612e6261736501000732312e302e313180000001"
        "000000000000000000020003000000020004000500000016000600000000000100088000000a"
        "0000000000000000";
    std::string bytes;
    for (std::size_t i = 0; i < encoded.size(); i += 2)
        bytes += static_cast<char>(std::stoul(encoded.substr(i, 2), nullptr, 16));
    return bytes;
}
std::map<std::string, std::string> plan_fields(const std::string& text) {
    std::map<std::string, std::string> fields;
    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) {
        if (line.empty() || line[0] == '#' || line[0] == '!') continue;
        auto equal = line.find('=');
        if (equal == std::string::npos) throw std::runtime_error("Invalid fixture properties line");
        std::string value;
        for (std::size_t i = equal + 1; i < line.size(); ++i) {
            char c = line[i];
            if (c == '\\' && i + 1 < line.size()) {
                c = line[++i];
                if (c == 'n') c = '\n';
                else if (c == 'r') c = '\r';
                else if (c == 't') c = '\t';
            }
            value += c;
        }
        fields.emplace(line.substr(0, equal), std::move(value));
    }
    return fields;
}
struct InstallerFixture {
    std::string patched_path, srg_path, extra_path;
    std::string generated = "verified generated fixture";
    std::string main, profile_id;
};
InstallerFixture seed_installer(Fixture& f, LoaderKind kind) {
    const bool neo = kind == LoaderKind::NeoForge;
    InstallerFixture result;
    auto version = std::string(neo ? "21.1.256" : "52.1.16");
    auto release = neo ? version : "1.21.1-" + version;
    f.instance.game.loader = {kind, version};
    result.main = neo ? "cpw.mods.bootstraplauncher.BootstrapLauncher" : "net.minecraftforge.bootstrap.ForgeBootstrap";
    result.profile_id = neo ? "neoforge-" + version : "1.21.1-forge-" + version;
    const auto group = std::string(neo ? "net.neoforged" : "net.minecraftforge");
    const auto loader_artifact = std::string(neo ? "neoforge" : "forge");
    const auto group_path = std::string(neo ? "net/neoforged" : "net/minecraftforge");
    const auto repository = std::string(neo ? "https://maven.neoforged.net/releases/" : "https://maven.minecraftforge.net/");
    const auto patched_relative = group_path + "/" + loader_artifact + "/" + release + "/" + loader_artifact + "-" + release + "-client.jar";
    const std::string minecraft_release = "1.21.1-20240808.144430";
    const auto minecraft_relative = "net/minecraft/client/" + minecraft_release + "/client-" + minecraft_release;
    result.patched_path = joinPath(f.root, "cache/libraries/" + patched_relative);
    result.srg_path = joinPath(f.root, "cache/libraries/" + minecraft_relative + "-srg.jar");
    result.extra_path = joinPath(f.root, "cache/libraries/" + minecraft_relative + "-extra.jar");
    write_file(joinPath(f.root, "runtime/mineswitch/launcher-tools.jar"), "local Java processor helper fixture");
    const auto tool = zip_bytes(f, {{"META-INF/MANIFEST.MF", "Manifest-Version: 1.0\nMain-Class: fixture.Tool\n\n"}});
    const auto dependency = zip_bytes(f, {{"META-INF/MANIFEST.MF", "Manifest-Version: 1.0\n\n"}});
    const auto bootstrap = zip_bytes(f, {{"META-INF/MANIFEST.MF", "Manifest-Version: 1.0\nAutomatic-Module-Name: wrong.manifest.fallback\n\n"},
                                          {"module-info.class", module_fixture()}});
    const auto jarjar = zip_bytes(f, {{"META-INF/MANIFEST.MF", "Manifest-Version: 1.0\nAutomatic-Module-Name: FixtureFileSystems\n\n"}});
    auto library = [&](const std::string& name, const std::string& path, const std::string& bytes) {
        auto url = repository + path;
        f.http.files[url] = bytes;
        return "{\"name\":" + quoted(name) + ",\"downloads\":{\"artifact\":" + artifact(url, bytes, path) + "}}";
    };
    const auto tool_library = library("org.fixture:tool:1", "org/fixture/tool/1/tool-1.jar", tool);
    const auto dependency_library = library("org.fixture:dependency:1", "org/fixture/dependency/1/dependency-1.jar", dependency);
    const std::string bootstrap_path = "cpw/mods/bootstraplauncher/2.0.2/bootstraplauncher-2.0.2.jar";
    const std::string filesystem_path = "net/neoforged/JarJarFileSystems/0.4.1/JarJarFileSystems-0.4.1.jar";
    auto launch_libraries = library("cpw.mods:bootstraplauncher:2.0.2", bootstrap_path, bootstrap);
    if (neo) launch_libraries += "," + library("net.neoforged:JarJarFileSystems:0.4.1", filesystem_path, jarjar);
    else launch_libraries += ",{\"name\":" + quoted(group + ":" + loader_artifact + ":" + release + ":client") +
                               ",\"downloads\":{\"artifact\":" + artifact("", result.generated, patched_relative) + "}}";
    std::string jvm = "\"-Djava.net.preferIPv6Addresses=system\"";
    if (neo) {
        const auto module_path = "${library_directory}/" + bootstrap_path + "${classpath_separator}${library_directory}/" + filesystem_path;
        jvm += ",\"-DignoreList=client-extra,${version_name}.jar\",\"-DlibraryDirectory=${library_directory}\",\"-p\"," + quoted(module_path) +
               ",\"--add-modules\",\"ALL-MODULE-PATH\",\"--add-opens\",\"java.base/java.util.jar=fixture.bootstrap\"";
    }
    const auto launch = "{\"id\":" + quoted(result.profile_id) + ",\"inheritsFrom\":\"1.21.1\",\"mainClass\":" + quoted(result.main) +
                        ",\"arguments\":{\"game\":[\"--launchTarget\",\"forgeclient\"],\"jvm\":[" + jvm + "]},\"libraries\":[" + launch_libraries + "]}";
    std::string data = "{\"BINPATCH\":{\"client\":\"/data/client.lzma\"},\"PATCHED\":{\"client\":" +
                       quoted("[" + group + ":" + loader_artifact + ":" + release + ":client]") + "}";
    if (neo) data += ",\"MC_SRG\":{\"client\":" + quoted("[net.minecraft:client:" + minecraft_release + ":srg]") +
                     "},\"MC_EXTRA\":{\"client\":" + quoted("[net.minecraft:client:" + minecraft_release + ":extra]") + "}";
    data += "}";
    std::string processors;
    if (neo) processors = "{\"sides\":[\"client\"],\"jar\":\"org.fixture:tool:1\",\"classpath\":[\"org.fixture:dependency:1\"],"
                          "\"args\":[\"--input\",\"{MINECRAFT_JAR}\",\"--slim\",\"{MC_SRG}\",\"--extra\",\"{MC_EXTRA}\"]},";
    processors += "{\"sides\":[\"client\"],\"jar\":\"org.fixture:tool:1\",\"classpath\":[\"org.fixture:dependency:1\"],"
                  "\"args\":[\"--input\"," + quoted(neo ? "{MC_SRG}" : "{MINECRAFT_JAR}") + ",\"--apply\",\"{BINPATCH}\",\"--output\",\"{PATCHED}\"]";
    if (!neo) processors += ",\"outputs\":{\"{PATCHED}\":" + quoted(sha(result.generated)) + "}";
    processors += "},{\"sides\":[\"server\"],\"jar\":\"org.fixture:server-only:1\",\"classpath\":[],\"args\":[]}";
    const auto profile = "{\"spec\":1,\"minecraft\":\"1.21.1\",\"json\":\"/version.json\",\"version\":" + quoted(result.profile_id) +
                         ",\"data\":" + data + ",\"processors\":[" + processors + "],\"libraries\":[" + tool_library + "," + dependency_library + "]}";
    const auto installer = zip_bytes(f, {{"install_profile.json", profile}, {"version.json", launch}, {"data/client.lzma", "local binpatch fixture"}});
    const auto installer_url = repository + group_path + "/" + loader_artifact + "/" + release + "/" + loader_artifact + "-" + release + "-installer.jar";
    f.http.files[installer_url] = installer;
    f.http.responses[installer_url + ".sha1"] = sha(installer) + "\n";
    return result;
}
std::map<std::string, std::string> complete_fixture_plan(const std::string& filename, const std::string& contents) {
    const auto fields = plan_fields(readTextFile(filename));
    std::map<std::string, std::string> outputs;
    const int processors = std::stoi(fields.at("processor.count"));
    for (int i = 0; i < processors; ++i) {
        const auto prefix = "processor." + std::to_string(i) + ".outputs.";
        for (int j = 0; j < std::stoi(fields.at(prefix + "count")); ++j) {
            const auto key = prefix + std::to_string(j) + ".";
            auto wanted = fields.find(key + "sha1");
            outputs[fields.at(key + "path")] = wanted == fields.end() ? "" : wanted->second;
        }
    }
    std::ostringstream marker;
    marker << "schema=1\nstate=complete\nplan_file=" << filename << "\nplan_sha256=" << verifyFile(filename)
           << "\nprocessor.count=" << processors << "\noutput.count=" << outputs.size() << '\n';
    int index = 0;
    for (const auto& output : outputs) {
        write_file(output.first, contents);
        const auto key = "output." + std::to_string(index++) + ".";
        marker << key << "path=" << output.first << '\n' << key << "sha256=" << verifyFile(output.first) << '\n';
        if (!output.second.empty()) marker << key << "sha1=" << output.second << '\n';
    }
    write_file(fields.at("completion_file"), marker.str());
    return outputs;
}
void installer_test(LoaderKind kind) {
    Fixture f; f.legacy();
    const auto seed = seed_installer(f, kind);
    GamePreparer preparer(f.storage, f.http);
    // A previous good library must survive interrupted preparation of a new plan.
    write_file(seed.patched_path, "previous verified client output");
    const auto original_path = joinPath(f.root, "minecraft/client.jar");
    const auto original_hash = verifyFile(original_path);
    auto helper = preparer.prepare(f.instance, f.settings, f.profile);
    check(helper.preparation_only && helper.main_class == "mineswitch.launcher.ProcessorEntry" &&
          f.instance.state == InstanceState::NeedsPreparation, "installer preparation returns separate helper JVM plan");
    check(helper.game_arguments.size() == 1 && helper.game_arguments[0] == joinPath(f.instance.root, "preparation/processor-plan.properties"),
          "helper receives explicit agreed processor plan");
    const auto plan_path = helper.game_arguments.front();
    auto fields = plan_fields(readTextFile(plan_path));
    const auto marker = fields.at("completion_file");
    const bool neo = kind == LoaderKind::NeoForge;
    check(fields.at("processor.count") == (neo ? "2" : "1"), "server-only processor excluded before dependency fetch");
    std::set<std::string> inputs;
    for (int i = 0; i < std::stoi(fields.at("input.count")); ++i) inputs.insert(fields.at("input." + std::to_string(i) + ".path"));
    check(inputs.count(original_path) == 1, "original client is an explicit protected input");
    for (const auto& field : fields) {
        if (field.first.find(".outputs.") != std::string::npos && field.first.size() >= 5 && field.first.substr(field.first.size() - 5) == ".path")
            check(field.second.find(joinPath(f.instance.root, "preparation/generated") + "/") == 0, "processor output stays in private generated staging");
    }
    if (neo) check(fields.at("processor.1.args.1").find("preparation/generated/") != std::string::npos,
                   "later processors consume staged earlier outputs");
    check(readTextFile(seed.patched_path) == "previous verified client output", "helper planning preserves previous canonical output");
    write_file(marker, "schema=1\nstate=complete\nplan_sha256=stale\n");
    f.http.network_forbidden = true;
    auto retried = preparer.prepare(f.instance, f.settings, f.profile);
    check(retried.preparation_only && !pathExists(marker), "stale completion schedules helper and clears stale marker");
    check(readTextFile(seed.patched_path) == "previous verified client output", "stale completion cannot replace canonical output");
    f.http.network_forbidden = false;
    auto staged = complete_fixture_plan(plan_path, seed.generated);
    auto launch = preparer.prepare(f.instance, f.settings, f.profile);
    check(!launch.preparation_only && launch.main_class == seed.main && f.instance.state == InstanceState::Ready,
          "verified completion becomes normal loader launch");
    check(readTextFile(seed.patched_path) == seed.generated && verifyFile(original_path) == original_hash,
          "completed output published separately from original client");
    const auto cp = option(launch, "-Djava.class.path=");
    check(cp.find(original_path) == std::string::npos, "original unpatched client excluded from loader bootstrap classpath");
    if (neo) {
        check(readTextFile(seed.srg_path) == seed.generated && readTextFile(seed.extra_path) == seed.generated,
              "Neo SRG and extra artifacts published into canonical libraries");
        check(cp.find(seed.patched_path) == std::string::npos && cp.find(seed.srg_path) == std::string::npos && cp.find(seed.extra_path) == std::string::npos,
              "Neo generated game jars are located by FML rather than bootstrap classpath");
        check(option(launch, "-DlibraryDirectory=") == joinPath(f.root, "cache/libraries"), "FML receives canonical library directory");
        check(option(launch, "--module-path=").find(';') != std::string::npos && !has(launch.jvm_options, "-p"), "module-path normalized for JNI and Horizon separator");
        check(has(launch.jvm_options, "--add-modules=ALL-MODULE-PATH") &&
              has(launch.jvm_options, "--add-opens=java.base/java.util.jar=fixture.bootstrap"), "module option pairs normalized without loss");
        auto modules = option(launch, "--limit-modules=");
        check(modules.find("fixture.bootstrap") != std::string::npos && modules.find("FixtureFileSystems") != std::string::npos &&
              modules.find("wrong.manifest.fallback") == std::string::npos && modules.find("jdk.jfr") == std::string::npos,
              "explicit and automatic module names retained while JFR stays excluded");
    } else check(cp.find(seed.patched_path) != std::string::npos, "Forge generated client remains a declared version library");
    f.http.network_forbidden = true;
    const auto requests = f.http.requests, downloads = f.http.downloads;
    auto cached = preparer.prepare(f.instance, f.settings, f.profile);
    check(!cached.preparation_only && requests == f.http.requests && downloads == f.http.downloads, "verified installer pipeline is reusable offline");
    // A changed staged file invalidates its completion receipt, but the last
    // good canonical libraries remain intact until another complete run.
    write_file(staged.begin()->first, "partial retry output");
    auto retry = preparer.prepare(f.instance, f.settings, f.profile);
    check(retry.preparation_only && f.instance.state == InstanceState::NeedsPreparation && !pathExists(marker),
          "changed generated output invalidates completion");
    check(readTextFile(seed.patched_path) == seed.generated && verifyFile(original_path) == original_hash,
          "failed retry preserves canonical output and original client");
}
void rejection_tests() {
    {
        Fixture f; f.instance.state = InstanceState::NeedsFiles;
        rejects("prepare_missing_files", [&] { GamePreparer(f.storage, f.http).prepare(f.instance, f.settings, f.profile); });
        check(f.http.requests == 0 && f.http.downloads == 0, "missing-file gate before network");
    }
    {
        Fixture f; f.metadata.replace(f.metadata.find("\"majorVersion\":21"), 17, "\"majorVersion\":25"); f.seed();
        rejects("prepare_java_unsupported", [&] { GamePreparer(f.storage, f.http).prepare(f.instance, f.settings, f.profile); });
        check(f.instance.state == InstanceState::Unsupported && !pathExists(joinPath(f.instance.root, "launch-plan.json")), "wrong Java cannot become Ready");
    }
    {
        Fixture f; auto at = f.metadata.find("org.lwjgl:lwjgl:3.3.3"); f.metadata.replace(at, 21, "org.lwjgl:lwjgl:3.3.4"); f.seed();
        rejects("prepare_lwjgl_unsupported", [&] { GamePreparer(f.storage, f.http).prepare(f.instance, f.settings, f.profile); });
        check(f.instance.state == InstanceState::Unsupported, "unknown native ABI does not silently reuse old bindings");
    }
    for (auto kind : {LoaderKind::Forge, LoaderKind::NeoForge}) {
        Fixture f; f.instance.game.loader = {kind, kind == LoaderKind::Forge ? "52.1.16" : "21.1.256"};
        rejects("prepare_runtime_missing", [&] { GamePreparer(f.storage, f.http).prepare(f.instance, f.settings, f.profile); });
        check(f.instance.state == InstanceState::NeedsPreparation, "missing helper runtime stays recoverable and not Ready");
    }
    {
        Fixture f; f.http.corrupt_download = true;
        rejects("prepare_hash", [&] { GamePreparer(f.storage, f.http).prepare(f.instance, f.settings, f.profile); });
        check(f.instance.state != InstanceState::Ready && !pathExists(joinPath(f.instance.root, "launch-plan.json")), "corrupted download never publishes launch readiness");
    }
    {
        Fixture f; std::atomic_bool cancelled{true};
        rejects("cancelled", [&] { GamePreparer(f.storage, f.http).prepare(f.instance, f.settings, f.profile, {}, &cancelled); });
        check(f.http.requests == 0 && f.http.downloads == 0, "cancelled preparation performs no network work");
    }
}
}
int main() {
    try {
        vanilla_test(); asset_progress_test(); asset_progress_cancellation_test(false); asset_progress_cancellation_test(true);
        legacy_and_sd_test(); receipt_cache_test(); receipt_mutation_test(); receipt_symlink_test(false); receipt_symlink_test(true);
        loader_test(LoaderKind::Fabric); loader_test(LoaderKind::Quilt);
        loader_test(LoaderKind::Fabric, "0.19.3", true, true);
        loader_test(LoaderKind::Fabric, "0.15.11", true, false);
        loader_test(LoaderKind::Fabric, "0.19.3-beta.1", true, false);
        loader_test(LoaderKind::Quilt, "0.16.10", true, false);
        installer_test(LoaderKind::Forge); installer_test(LoaderKind::NeoForge); rejection_tests();
        std::cout << "PREPARE_TEST_PASS checks=" << checks << '\n';
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
