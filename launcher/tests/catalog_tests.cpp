#include "mineswitch/catalog.hpp"
#include <jansson.h>
#include <atomic>
#include <cstdlib>
#include <deque>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace mineswitch;
namespace {
int checks = 0;
const std::string sha1(40, 'a'), sha512(128, 'b'), md5(32, 'c');
void check(bool ok, const std::string& label) {
    if (!ok) throw std::runtime_error("FAIL: " + label);
    ++checks;
}
void rejects(const std::string& code, const std::function<void()>& action) {
    try { action(); } catch (const Error& error) {
        check(error.code == code, "error code: " + code + " received " + error.code);
        return;
    }
    throw std::runtime_error("FAIL: expected " + code);
}
std::string quote(const std::string& value) {
    json_t* string = json_stringn(value.data(), value.size());
    char* data = json_dumps(string, JSON_ENCODE_ANY | JSON_COMPACT);
    std::string out(data);
    free(data);
    json_decref(string);
    return out;
}
std::string hashes() { return "{\"sha1\":" + quote(sha1) + ",\"sha512\":" + quote(sha512) + "}"; }
std::string mr_file(const std::string& path = "mods/example.jar", const std::string& env = {}) {
    return "{\"path\":" + quote(path) + ",\"hashes\":" + hashes()
        + ",\"downloads\":[\"https://cdn.modrinth.com/data/example.jar\"],\"fileSize\":42"
        + (env.empty() ? "" : ",\"env\":" + env) + "}";
}
std::string mr_pack(const std::string& files, const std::string& dependencies = "{\"minecraft\":\"1.21.1\",\"fabric-loader\":\"0.16.10\"}") {
    return "{\"formatVersion\":1,\"game\":\"minecraft\",\"versionId\":\"release-1\",\"name\":\"Fixture pack\",\"files\":["
        + files + "],\"dependencies\":" + dependencies + "}";
}
std::string cf_pack(bool required = true) {
    return "{\"manifestType\":\"minecraftModpack\",\"manifestVersion\":1,\"name\":\"CF fixture\",\"version\":\"1\","
        "\"minecraft\":{\"version\":\"1.21.1\",\"modLoaders\":[{\"id\":\"neoforge-21.1.100\",\"primary\":true}]},"
        "\"files\":[{\"projectID\":10,\"fileID\":20,\"required\":" + std::string(required ? "true" : "false") + "}],\"overrides\":\"overrides\"}";
}
std::string project(bool allowed = true, bool mod = true) {
    return "{\"id\":10,\"gameId\":432,\"classId\":"+std::to_string(mod?6:4471)+",\"name\":\"Example\",\"slug\":\"example\",\"allowModDistribution\":"
        + std::string(allowed ? "true" : "false") + ",\"links\":{\"websiteUrl\":\"https://www.curseforge.com/minecraft/mc-mods/example\"}}";
}
std::string cf_file(bool downloadable, const std::string& filename = "example.jar") {
    return "{\"id\":20,\"modId\":10,\"fileName\":" + quote(filename)
        + ",\"fileLength\":42,\"isAvailable\":true,\"hashes\":[{\"algo\":1,\"value\":" + quote(sha1)
        + "},{\"algo\":2,\"value\":" + quote(md5) + "}],\"downloadUrl\":"
        + (downloadable ? "\"https://edge.forgecdn.net/files/example.jar\"" : "null")
        + ",\"displayName\":\"Version One\",\"gameVersions\":[\"1.21.1\",\"NeoForge\"],\"fileDate\":\"2026-01-01T00:00:00Z\"}";
}
std::string mr_version_file(const std::string& name, bool primary, const std::string& hash = sha1) {
    return "{\"filename\":" + quote(name) + ",\"size\":42,\"primary\":" + (primary ? "true" : "false")
        + ",\"url\":\"https://cdn.modrinth.com/data/" + name + "\",\"hashes\":{\"sha1\":" + quote(hash)
        + ",\"sha512\":" + quote(sha512) + "}}";
}
std::string mr_version(const std::string& files) {
    return "{\"id\":\"version1\",\"project_id\":\"project1\",\"name\":\"Version One\",\"version_number\":\"1\","
        "\"date_published\":\"2026-01-01T00:00:00Z\",\"game_versions\":[\"1.21.1\"],\"loaders\":[\"fabric\"],\"files\":[" + files + "]}";
}
struct Step {
    std::string method, url_contains, body;
    int status = 200;
    std::function<void(const HttpRequest&)> inspect;
};
class FakeHttp final : public HttpClient {
public:
    std::deque<Step> steps;
    int requests = 0;
    HttpResponse request(const HttpRequest& request, const std::atomic_bool*) override {
        check(!steps.empty(), "no unexpected HTTP request");
        auto step = std::move(steps.front()); steps.pop_front();
        check(request.method == step.method, "HTTP method");
        check(request.url.find(step.url_contains) != std::string::npos, "HTTP endpoint " + request.url);
        check(request.max_bytes <= 8 * 1024 * 1024, "bounded API body");
        if (step.inspect) step.inspect(request);
        ++requests;
        return {step.status, step.body, {}};
    }
    void download(const Download&, const std::string&, ProgressCallback, const std::atomic_bool*) override {
        throw std::runtime_error("Catalog layer must not download or write files");
    }
};
void authenticated(const HttpRequest& request) {
    check(request.headers.at("x-api-key") == "test-only-key", "own configured app key");
}
void anonymous(const HttpRequest& request) {
    check(request.headers.count("x-api-key") == 0, "no CF credential on Modrinth request");
    check(request.headers.count("Authorization") == 0, "no private-account token required");
}
void parser_tests() {
    auto plan = parse_pack_manifest(PackKind::Modrinth, mr_pack(mr_file()));
    check(plan.name == "Fixture pack" && plan.version == "release-1", "MR pack identity");
    check(plan.game.minecraft_version == "1.21.1" && plan.game.java_major == 21, "Java21 policy metadata");
    check(plan.game.loader.kind == LoaderKind::Fabric && plan.game.loader.version == "0.16.10", "MR loader exact version");
    check(plan.downloads.size() == 1 && plan.downloads[0].path == "mods/example.jar", "MR destination");
    check(plan.downloads[0].hashes.sha512 == sha512 && plan.downloads[0].hashes.sha1 == sha1, "MR both required hashes");
    check(plan.override_roots == std::vector<std::string>({"overrides", "client-overrides"}), "client override ordering");
    check(plan.expanded_bytes == 42 && plan.required_free_bytes == 42, "file byte accounting");

    auto optional = mr_file("mods/optional.jar", "{\"client\":\"optional\",\"server\":\"unsupported\"}");
    auto server = mr_file("mods/server.jar", "{\"client\":\"unsupported\",\"server\":\"required\"}");
    plan = parse_pack_manifest(PackKind::Modrinth, mr_pack(optional + "," + server));
    check(plan.downloads.size() == 1 && plan.downloads[0].optional, "optional preserved, server-only omitted");
    check(plan.downloads[0].path == "mods/optional.jar", "importer owns optional activation policy");
    rejects("pack.unsafe_path", [] { parse_pack_manifest(PackKind::Modrinth, mr_pack(mr_file("../outside.jar"))); });
    rejects("pack.unsafe_path", [] { parse_pack_manifest(PackKind::Modrinth, mr_pack(mr_file("sdmc:/outside.jar"))); });
    rejects("pack.unsafe_path", [] { parse_pack_manifest(PackKind::Modrinth, mr_pack(mr_file("mods\\outside.jar"))); });
    rejects("pack.duplicate_path", [] { parse_pack_manifest(PackKind::Modrinth, mr_pack(mr_file("mods/A.jar") + "," + mr_file("Mods/a.jar"))); });
    rejects("pack.invalid_manifest", [] { parse_pack_manifest(PackKind::Modrinth, "{\"formatVersion\":1,\"formatVersion\":2}"); });
    rejects("pack.conflicting_loaders", [] { parse_pack_manifest(PackKind::Modrinth, mr_pack("", "{\"minecraft\":\"1.21.1\",\"forge\":\"1\",\"fabric-loader\":\"2\"}")); });
    rejects("pack.no_minecraft", [] { parse_pack_manifest(PackKind::Modrinth, mr_pack("", "{\"fabric-loader\":\"1\"}")); });
    auto bad_hash = mr_pack(mr_file());
    bad_hash.replace(bad_hash.find(sha512), sha512.size(), "abc");
    rejects("pack.invalid_manifest", [&] { parse_pack_manifest(PackKind::Modrinth, bad_hash); });
    auto bad_url = mr_pack(mr_file());
    bad_url.replace(bad_url.find("https://"), 8, "http://");
    rejects("catalog.invalid_url", [&] { parse_pack_manifest(PackKind::Modrinth, bad_url); });
    plan = parse_pack_manifest(PackKind::Modrinth, mr_pack("", "{\"minecraft\":\"1.21.1\",\"future-loader\":\"1\"}"));
    check(plan.state == InstanceState::Unsupported && !plan.notices.empty(), "unknown dependency is explicit");

    plan = parse_pack_manifest(PackKind::CurseForge, cf_pack(false));
    check(plan.game.loader.kind == LoaderKind::NeoForge && plan.game.loader.version == "21.1.100", "CF loader");
    check(plan.downloads[0].project_id == "10" && plan.downloads[0].file_id == "20", "CF exact identifier pins");
    check(plan.downloads[0].path.empty() && plan.downloads[0].urls.empty() && plan.downloads[0].hashes.empty(), "CF resolution required");
    check(plan.downloads[0].optional, "CF required=false");
    const auto components = "{\"formatVersion\":1,\"components\":[{\"uid\":\"net.minecraft\",\"version\":\"1.21.1\"},{\"uid\":\"net.fabricmc.fabric-loader\",\"version\":\"0.16.10\"},{\"uid\":\"org.lwjgl3\",\"version\":\"3.3.3\"}]}";
    plan = parse_pack_manifest(PackKind::MultiMC, components,
        "[General]\nname=Local fixture\nJavaPath=C:/desktop/java.exe\nPreLaunchCommand=not-imported\n");
    check(plan.name == "Local fixture" && plan.game.loader.kind == LoaderKind::Fabric, "MMC name and loader");
    check(plan.game.java_major == 21 && plan.downloads.empty() && plan.notices.size() == 3 && plan.packwiz_url.empty() && plan.notices[1].find("not-imported") != std::string::npos, "MMC ignores desktop execution hooks");
}
void catalog_tests() {
    Settings settings; settings.curseforge_key = "test-only-key";
    FakeHttp http; CatalogClient client(http, settings);
    http.steps.push_back({"GET", "https://api.modrinth.com/v2/search?", "{\"hits\":[{\"project_id\":\"project1\",\"slug\":\"fixture\",\"title\":\"Fixture\",\"description\":\"Public pack\",\"downloads\":123}],\"total_hits\":21}", 200,
        [](const HttpRequest& req) {
            anonymous(req);
            check(req.url.find("query=hello%20%26%20world") != std::string::npos, "query encoding");
            check(req.url.find("project_type%3Amodpack") != std::string::npos, "modpack-only facet");
            check(req.url.find("versions%3A1.21.1") != std::string::npos, "Minecraft facet");
        }});
    auto page = client.search(Provider::Modrinth, {"hello & world", "1.21.1", 0, 20});
    check(page.projects[0].id == "project1" && page.total == 21 && page.has_more, "search pagination");
    http.steps.push_back({"GET", "/mods/search?gameId=432&classId=4471", "{\"data\":[" + project(true,false) + "],\"pagination\":{\"totalCount\":1}}", 200, authenticated});
    page = client.search(Provider::CurseForge, {"fixture", "1.21.1", 0, 20});
    check(page.projects[0].provider == Provider::CurseForge && !page.has_more, "CF pack search");
    rejects("catalog.invalid_query", [&] { client.search(Provider::CurseForge, {"", "", 9999, 20}); });
    http.steps.push_back({"GET", "/project/project1", "{\"id\":\"project1\",\"slug\":\"fixture\",\"title\":\"Fixture\"}", 200, anonymous});
    http.steps.push_back({"GET", "/version/version1", mr_version(mr_version_file("README.txt", true) + "," + mr_version_file("fixture.mrpack", false)), 200, anonymous});
    auto version = client.version(Provider::Modrinth, "project1", "version1");
    check(version.archive.file_name == "fixture.mrpack" && version.archive.bytes == 42, "actual archive chosen from metadata");
    check(version.archive.hashes.sha512 == sha512 && version.loaders[0] == LoaderKind::Fabric, "version hashes and loader");
    http.steps.push_back({"GET", "/mods/10", "{\"data\":" + project() + "}", 200, authenticated});
    http.steps.push_back({"GET", "/mods/10/files/20", "{\"data\":" + cf_file(true, "pack.zip") + "}", 200, authenticated});
    version = client.version(Provider::CurseForge, "10", "20");
    check(version.archive.file_name == "pack.zip" && version.archive.hashes.md5 == md5, "CF exact file metadata");
    check(version.minecraft_versions == std::vector<std::string>({"1.21.1"}) && version.loaders[0] == LoaderKind::NeoForge, "CF metadata separates loader tags");
    http.steps.push_back({"GET", "/search?", "{}", 429, anonymous});
    rejects("catalog.rate_limited", [&] { client.search(Provider::Modrinth, {}); });
    std::atomic_bool cancelled{true};
    rejects("catalog.cancelled", [&] { client.search(Provider::Modrinth, {}, &cancelled); });
    check(http.steps.empty(), "all HTTP fixture requests consumed");
    FakeHttp no_requests; CatalogClient no_key(no_requests, Settings{});
    no_requests.steps.push_back({"GET","https://cf.polymc.org/api","{}",503,anonymous});
    rejects("catalog.cf_key_unavailable", [&] { no_key.search(Provider::CurseForge, {}); });
    no_requests.steps.push_back({"GET","https://cf.polymc.org/api","{\"ok\":true,\"token\":\"public-fixture-key\"}",200,anonymous});
    no_requests.steps.push_back({"GET","/mods/search?gameId=432&classId=6","{\"data\":["+project()+"],\"pagination\":{\"totalCount\":1}}",200,
        [](const HttpRequest& req){check(req.headers.at("x-api-key")=="public-fixture-key","automatic CF key used only at official API");
            check(req.url.find("modLoaderType=4")!=std::string::npos,"Fabric filter on CF mod search");}});
    auto mods=no_key.search(Provider::CurseForge,{"example","1.21.1",0,20,ContentKind::Mod,LoaderKind::Fabric});
    check(mods.projects.size()==1 && no_requests.steps.empty(),"public resolver enables individual CF mod search");
    settings.curseforge_key = "bad\r\nheader";
    rejects("catalog.cf_key_invalid", [&] { CatalogClient invalid(no_requests, settings); });
}
void mod_catalog_tests() {
    Settings settings;settings.curseforge_key="test-only-key";
    FakeHttp http;CatalogClient client(http,settings);
    std::string mod_project="{\"id\":\"project1\",\"project_type\":\"mod\",\"slug\":\"fixture\",\"title\":\"Fixture Mod\"}";
    http.steps.push_back({"GET","/search?","{\"hits\":["+mod_project+"],\"total_hits\":1}",200,
        [](const HttpRequest& request){anonymous(request);check(request.url.find("project_type%3Amod%22")!=std::string::npos,"individual mod facet");
            check(request.url.find("categories%3Afabric")!=std::string::npos,"Fabric search facet");}});
    auto found=client.search(Provider::Modrinth,{"fixture","1.21.1",0,20,ContentKind::Mod,LoaderKind::Fabric});
    check(found.projects.size()==1 && found.projects[0].website.find("/mod/")!=std::string::npos,"mod project website");
    http.steps.push_back({"GET","/project/project1",mod_project,200,anonymous});
    http.steps.push_back({"GET","/version/version1",mr_version(mr_version_file("notes.txt",true)+","+mr_version_file("example.jar",false)),200,anonymous});
    auto exact=client.version(Provider::Modrinth,"project1","version1",nullptr,ContentKind::Mod);
    check(exact.archive.file_name=="example.jar" && exact.archive.hashes.sha512==sha512,"verified mod JAR selected instead of readme");
    for(auto loader:{LoaderKind::Fabric,LoaderKind::Forge,LoaderKind::Quilt}) {
        http.steps.push_back({"GET","/project/project1",mod_project,200,anonymous});
        http.steps.push_back({"GET","/project/project1/version?","["+mr_version(mr_version_file("example.jar",true))+"]",200,anonymous});
        auto versions=client.versions(Provider::Modrinth,"project1","1.21.1",nullptr,ContentKind::Mod,loader);
        check(versions.size()==(loader==LoaderKind::Forge?0u:1u),"version loader filtering including Quilt/Fabric compatibility");
    }
    http.steps.push_back({"GET","/mods/10","{\"data\":"+project()+"}",200,authenticated});
    http.steps.push_back({"GET","/mods/10/files/20","{\"data\":"+cf_file(true)+"}",200,authenticated});
    exact=client.version(Provider::CurseForge,"10","20",nullptr,ContentKind::Mod);
    check(exact.archive.file_name=="example.jar" && exact.loaders[0]==LoaderKind::NeoForge,"CF individual mod file");
    http.steps.push_back({"GET","/project/project1",mod_project,200,anonymous});
    http.steps.push_back({"GET","/version/version1",mr_version(mr_version_file("pack.mrpack",true)),200,anonymous});
    rejects("catalog.no_mod_jar",[&]{client.version(Provider::Modrinth,"project1","version1",nullptr,ContentKind::Mod);});
    check(http.steps.empty(),"all mod requests consumed");
}
void resolver_tests() {
    Settings settings; settings.curseforge_key = "test-only-key";
    FakeHttp http; CatalogClient client(http, settings);
    auto queue_metadata = [&](bool allow, bool direct) {
        http.steps.push_back({"POST", "/v1/mods", "{\"data\":[" + project(allow) + "]}", 200,
            [](const HttpRequest& req) { authenticated(req); check(req.body == "{\"modIds\":[10]}", "numeric project ID request"); }});
        http.steps.push_back({"POST", "/v1/mods/files", "{\"data\":[" + cf_file(direct) + "]}", 200,
            [](const HttpRequest& req) { authenticated(req); check(req.body == "{\"fileIds\":[20]}", "exact numeric file ID request"); }});
    };
    auto plan = parse_pack_manifest(PackKind::CurseForge, cf_pack(false));
    queue_metadata(true, true);
    client.resolve_pack(plan);
    check(plan.downloads[0].path == "mods/example.jar" && plan.downloads[0].optional, "resolved CF filename and optional marker");
    check(plan.downloads[0].urls.size() == 1 && !plan.downloads[0].manual && plan.expanded_bytes == 42, "approved direct download plan");

    plan = parse_pack_manifest(PackKind::CurseForge, cf_pack());
    queue_metadata(false, true); // Even a populated URL cannot override explicit author opt-out.
    http.steps.push_back({"GET", "/version_file/" + sha1 + "?algorithm=sha1", "{}", 404, anonymous});
    client.resolve_pack(plan);
    check(plan.state == InstanceState::NeedsFiles && plan.downloads[0].manual, "author restriction remains manual");
    check(plan.downloads[0].urls.empty(), "no direct URL used or CDN path invented");
    check(plan.downloads[0].manual_url == "https://www.curseforge.com/minecraft/mc-mods/example/download/20", "official manual download page");

    plan = parse_pack_manifest(PackKind::CurseForge, cf_pack());
    queue_metadata(false, false);
    auto alternative = mr_version(mr_version_file("wrong-primary.jar", true, std::string(40, 'd')) + "," + mr_version_file("same-file.jar", false));
    http.steps.push_back({"GET", "/version_file/" + sha1, alternative, 200, anonymous});
    client.resolve_pack(plan);
    check(!plan.downloads[0].manual && plan.downloads[0].urls[0] == "https://cdn.modrinth.com/data/same-file.jar", "exact hash match, not unrelated primary");
    check(plan.downloads[0].path == "mods/example.jar" && plan.downloads[0].hashes.md5 == md5 && plan.downloads[0].hashes.sha512 == sha512, "destination and original expected checksums retained");

    plan = parse_pack_manifest(PackKind::CurseForge, cf_pack());
    queue_metadata(false, false);
    auto wrong_size = mr_version(mr_version_file("wrong-size.jar", true));
    wrong_size.replace(wrong_size.find("\"size\":42"), 9, "\"size\":43");
    http.steps.push_back({"GET", "/version_file/" + sha1, wrong_size, 200, anonymous});
    client.resolve_pack(plan);
    check(plan.downloads[0].manual && plan.downloads[0].urls.empty(), "hash lookup still requires matching size");
    check(http.steps.empty(), "resolution queue exhausted");

    plan = parse_pack_manifest(PackKind::CurseForge, cf_pack());
    http.steps.push_back({"POST", "/mods", "{\"data\":[" + project() + "]}", 200, authenticated});
    auto mismatch = cf_file(true);
    mismatch.replace(mismatch.find("\"modId\":10"), 10, "\"modId\":11");
    http.steps.push_back({"POST", "/mods/files", "{\"data\":[" + mismatch + "]}", 200, authenticated});
    rejects("catalog.identity_mismatch", [&] { client.resolve_pack(plan); });
    check(plan.downloads[0].path.empty() && plan.downloads[0].hashes.empty(), "failed resolution commits no partial plan");
}
}
int main() {
    try {
        parser_tests(); catalog_tests(); mod_catalog_tests(); resolver_tests();
        std::cout << "CATALOG_TEST_PASS checks=" << checks << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
