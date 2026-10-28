#include "mineswitch/catalog.hpp"
#include "mineswitch/import.hpp"
#include "mineswitch/packwiz.hpp"
#include <mbedtls/md.h>
#include <unistd.h>
#include <algorithm>
#include <fstream>
#include <iostream>

using namespace mineswitch;
static unsigned checks;
static void check(bool value, const char* text) { if (!value) throw std::runtime_error(text); checks++; }
template <class F> static void rejects(const std::string& code, F call) {
    try { call(); } catch (const Error& e) { check(e.code == code, ("unexpected rejection " + e.code).c_str()); return; }
    throw std::runtime_error("expected rejection " + code);
}
static std::string sha256(const std::string& data) {
    unsigned char out[32];
    mbedtls_md(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), reinterpret_cast<const unsigned char*>(data.data()), data.size(), out);
    static const char digits[] = "0123456789abcdef";
    std::string text;
    for (unsigned char c : out) { text += digits[c >> 4]; text += digits[c & 15]; }
    return text;
}
static std::string read(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), {}};
}
static void write(const std::string& path, const std::string& data) { std::ofstream(path, std::ios::binary) << data; }

static std::string decode(const std::string& url) {
    std::string out;
    for (std::size_t i = 0; i < url.size(); ++i)
        if (url[i] == '%' && i + 2 < url.size()) { out += static_cast<char>(std::stoi(url.substr(i + 1, 2), nullptr, 16)); i += 2; }
        else out += url[i];
    return out;
}
class PackHttp : public HttpClient {
public:
    std::map<std::string, std::string> files;
    std::vector<std::string> requested, downloaded;
    HttpResponse request(const HttpRequest& request, const std::atomic_bool*) override {
        requested.push_back(request.url);
        HttpResponse response;
        auto found = files.find(decode(request.url));
        response.status = found == files.end() ? 404 : 200;
        if (found != files.end()) response.body = found->second;
        return response;
    }
    void download(const Download& artifact, const std::string& path, ProgressCallback, const std::atomic_bool*) override {
        auto found = files.find(decode(artifact.urls.at(0)));
        if (found == files.end()) throw Error("download_status", "Download server returned HTTP 404");
        downloaded.push_back(artifact.urls[0]);
        write(path, found->second);
        if (!artifact.hashes.sha256.empty() && sha256(found->second) != artifact.hashes.sha256) {
            unlink(path.c_str());
            throw Error("download_hash", "Downloaded artifact failed integrity verification");
        }
    }
};
static const std::string base = "https://packs.example/slop/";
struct Pack {
    std::string version = "1";
    std::vector<std::pair<std::string, std::string>> files; // path, contents
    std::vector<std::string> preserve;
    std::string loader = "21.1.242";
    void publish(PackHttp& http) const {
        std::string index = "hash-format = \"sha256\"\n";
        for (const auto& file : files) {
            http.files[base + file.first] = file.second;
            index += "\n[[files]]\nfile = \"" + file.first + "\"\nhash = \"" + sha256(file.second) + "\"\n";
            for (const auto& kept : preserve) if (kept == file.first) index += "preserve = true\n";
        }
        http.files[base + "index.toml"] = index;
        http.files[base + "pack.toml"] = "name = \"SLOP\"\nversion = \"" + version + "\"\npack-format = \"packwiz:1.1.0\"\n\n"
            "[index]\nfile = \"index.toml\"\nhash-format = \"sha256\"\nhash = \"" + sha256(index) + "\"\n\n"
            "[versions]\nminecraft = \"1.21.1\"\nneoforge = \"" + loader + "\"\n";
    }
};

int main() {
    char pattern[] = "/tmp/mineswitch-packwiz-XXXXXX";
    auto directory = mkdtemp(pattern);
    if (!directory) return 1;
    std::string root = directory;
    try {
        // TOML subset used by packwiz.
        auto doc = parseToml("# comment\nname = \"A \\\"q\\\" \\u00e9\"\nlit = 'C:\\path'\n[download]\nurl = \"https://x/y z.jar\" # trailing\n"
                             "hash-format = \"sha1\"\nmode = 'url'\n[update.curseforge]\nfile-id = 123\nproject-id = 456\n"
                             "[options]\nacceptable-game-versions = [\n  \"1.21\",\n  \"1.21.1\",\n]\ninline = { a = 1, b = \"x\" }\n"
                             "[[files]]\nfile = \"a\"\npreserve = true\n[[files]]\nfile = \"b\"\n\"quoted key\" = \"\"\"\nmulti\nline\"\"\"\n");
        check(doc.get("name") == "A \"q\" \xc3\xa9", "basic string escapes");
        check(doc.get("lit") == "C:\\path", "literal string");
        check(doc.get("download.url") == "https://x/y z.jar" && doc.get("download.mode") == "url", "table values and comments");
        check(doc.get("update.curseforge.file-id") == "123" && doc.get("update.curseforge.project-id") == "456", "dotted table, integers");
        check(doc.get("options.acceptable-game-versions") == "1.21\n1.21.1", "multi-line array");
        check(doc.get("options.inline.a") == "1" && doc.get("options.inline.b") == "x", "inline table");
        check(doc.arrays.at("files").size() == 2 && doc.arrays.at("files")[0].at("preserve") == "true" &&
              doc.arrays.at("files")[1].at("quoted key") == "multi\nline", "arrays of tables");
        rejects("packwiz.format", [] { parseToml("a = 1\na = 2\n"); });
        rejects("packwiz.format", [] { parseToml("a = \"open\n"); });
        rejects("packwiz.format", [] { parseToml("[t\n"); });

        // The real Prism export from SLOP.
        const std::string command = "\"$INST_JAVA\" -jar \"$INST_MC_DIR/packwiz-installer-bootstrap.jar\" --bootstrap-main-jar "
            "\"$INST_MC_DIR/packwiz-installer.jar\" --pack-folder \"$INST_MC_DIR\" --multimc-folder \"$INST_DIR\" "
            "\"https://slop.87-251-76-213.sslip.io/slop/pack.toml\"";
        check(packwizUrlFromCommand(command) == "https://slop.87-251-76-213.sslip.io/slop/pack.toml", "packwiz URL from command");
        check(packwizUrlFromCommand("echo https://example.com/pack.toml").empty(), "other commands are not packwiz");
        auto plan = parse_pack_manifest(PackKind::MultiMC,
            "{\"formatVersion\":1,\"components\":[{\"uid\":\"org.lwjgl3\",\"version\":\"3.3.3\"},{\"uid\":\"net.minecraft\",\"version\":\"1.21.1\"},"
            "{\"uid\":\"net.neoforged\",\"version\":\"21.1.242\"}]}",
            "[General]\nname=SLOP\niconKey=slop\nOverrideCommands=true\nPreLaunchCommand=" + command + "\n");
        check(plan.packwiz_url == "https://slop.87-251-76-213.sslip.io/slop/pack.toml" && plan.icon == "slop" &&
              plan.game.loader.kind == LoaderKind::NeoForge && plan.state != InstanceState::Unsupported, "MultiMC packwiz plan");

        Storage storage(root + "/library");
        storage.initialize();
        Instance instance;
        instance.id = newId();
        instance.name = "SLOP";
        instance.root = joinPath(storage.root(), "instances/" + instance.id);
        instance.game.minecraft_version = "1.21.1";
        instance.game.loader = {LoaderKind::NeoForge, "21.1.200"};
        instance.packwiz_url = base + "pack.toml";
        createDirectories(joinPath(instance.root, "game"));
        storage.saveInstance(instance);
        const auto game = joinPath(instance.root, "game");
        PackHttp http;
        Settings settings;
        check(!packwizSynced(instance), "not synced before the first run");

        Pack pack;
        pack.files = {{"mods/CR7 MOD NEOFORGE 1.21.1.jar", "jar-1"}, {"config/forgeendertech/Biomes/minecraft$beach.cfg", "beach"},
                      {"config/MouseTweaks.cfg", "defaults"}, {"mods/old.jar", "old"}};
        pack.preserve = {"config/MouseTweaks.cfg"};
        pack.publish(http);
        write(joinPath(game, "options.txt"), "lang:ru_ru");
        auto first = syncPackwiz(storage, http, settings, instance, false, {}, nullptr);
        check(first.changed && first.downloaded == 4 && first.version == "1", "first sync downloads every file");
        check(read(game + "/mods/CR7 MOD NEOFORGE 1.21.1.jar") == "jar-1" && read(game + "/config/forgeendertech/Biomes/minecraft$beach.cfg") == "beach",
              "files placed at index paths");
        check(std::find(http.downloaded.begin(), http.downloaded.end(), base + "mods/CR7%20MOD%20NEOFORGE%201.21.1.jar") != http.downloaded.end() &&
              std::find(http.downloaded.begin(), http.downloaded.end(), base + "config/forgeendertech/Biomes/minecraft%24beach.cfg") != http.downloaded.end(),
              "URL path encoding");
        check(instance.game.loader.version == "21.1.242", "loader version follows pack.toml");
        check(storage.listInstances().at(0).game.loader.version == "21.1.242", "updated game is saved");
        check(packwizSynced(instance) && read(game + "/options.txt") == "lang:ru_ru", "synced, unrelated files kept");

        http.requested.clear();
        http.downloaded.clear();
        auto again = syncPackwiz(storage, http, settings, instance, false, {}, nullptr);
        check(!again.changed && http.requested.size() == 1 && http.downloaded.empty(), "unchanged pack costs one request");

        // User edits: a preserved config survives, a disabled mod stays disabled when updated.
        write(game + "/config/MouseTweaks.cfg", "mine");
        check(::rename((game + "/mods/CR7 MOD NEOFORGE 1.21.1.jar").c_str(), (game + "/mods/CR7 MOD NEOFORGE 1.21.1.jar.disabled").c_str()) == 0, "disable fixture");
        pack.version = "2";
        pack.files = {{"mods/CR7 MOD NEOFORGE 1.21.1.jar", "jar-2"}, {"config/forgeendertech/Biomes/minecraft$beach.cfg", "beach"},
                      {"config/MouseTweaks.cfg", "new defaults"}, {"mods/new.jar", "new"}};
        pack.publish(http);
        http.downloaded.clear();
        auto second = syncPackwiz(storage, http, settings, instance, false, {}, nullptr);
        check(second.changed && second.downloaded == 2 && second.removed == 1 && second.kept == 2, "incremental update");
        check(!pathExists(game + "/mods/old.jar") && read(game + "/mods/new.jar") == "new", "removed and added mods");
        check(read(game + "/mods/CR7 MOD NEOFORGE 1.21.1.jar.disabled") == "jar-2" && !pathExists(game + "/mods/CR7 MOD NEOFORGE 1.21.1.jar"),
              "updated mod stays disabled");
        check(read(game + "/config/MouseTweaks.cfg") == "mine", "preserved file is not overwritten");

        // A manual loader choice survives until the pack changes its versions.
        instance.game.loader.version = "21.1.100";
        storage.saveInstance(instance);
        pack.version = "3";
        pack.files.push_back({"resourcepacks/slopweather-music.zip", "zip"});
        pack.publish(http);
        syncPackwiz(storage, http, settings, instance, false, {}, nullptr);
        check(instance.game.loader.version == "21.1.100", "same pack versions keep the wizard choice");
        pack.loader = "21.1.250";
        pack.version = "4";
        pack.publish(http);
        auto bumped = syncPackwiz(storage, http, settings, instance, false, {}, nullptr);
        check(bumped.game_changed && instance.game.loader.version == "21.1.250", "new pack loader applied");

        // A modified file is replaced when the index lists it again; tampering is detected.
        write(game + "/mods/new.jar", "local edit");
        auto forced = syncPackwiz(storage, http, settings, instance, true, {}, nullptr);
        check(forced.changed && read(game + "/mods/new.jar") == "new", "forced sync repairs a modified file");
        http.files[base + "index.toml"] += "\n";
        pack.version = "5";
        rejects("packwiz.hash", [&] { syncPackwiz(storage, http, settings, instance, true, {}, nullptr); });
        pack.publish(http);
        http.files[base + "mods/new.jar"] = "evil";
        write(game + "/mods/new.jar", "local edit");
        rejects("download_hash", [&] { syncPackwiz(storage, http, settings, instance, true, {}, nullptr); });
        check(packwizSynced(instance), "failed sync keeps the last complete state");

        // Metafiles: side and optional flags, path escapes.
        Pack meta;
        meta.publish(http);
        std::string jar = "metajar";
        std::string metafile = "name = \"Meta\"\nfilename = \"meta-1.0.jar\"\nside = \"client\"\n\n[download]\nurl = \"https://cdn.example/meta-1.0.jar\"\n"
                               "hash-format = \"sha256\"\nhash = \"" + sha256(jar) + "\"\n";
        std::string server = "name = \"Server\"\nfilename = \"server.jar\"\nside = \"server\"\n\n[download]\nurl = \"https://cdn.example/s.jar\"\n"
                             "hash-format = \"sha256\"\nhash = \"" + sha256("s") + "\"\n";
        http.files["https://cdn.example/meta-1.0.jar"] = jar;
        http.files[base + "mods/meta.pw.toml"] = metafile;
        http.files[base + "mods/server.pw.toml"] = server;
        std::string index = "hash-format = \"sha256\"\n\n[[files]]\nfile = \"mods/meta.pw.toml\"\nhash = \"" + sha256(metafile) +
                            "\"\nmetafile = true\n\n[[files]]\nfile = \"mods/server.pw.toml\"\nhash = \"" + sha256(server) + "\"\nmetafile = true\n";
        http.files[base + "index.toml"] = index;
        http.files[base + "pack.toml"] = "name = \"SLOP\"\nversion = \"6\"\n[index]\nfile = \"index.toml\"\nhash-format = \"sha256\"\nhash = \"" +
                                         sha256(index) + "\"\n";
        auto metasync = syncPackwiz(storage, http, settings, instance, false, {}, nullptr);
        check(read(game + "/mods/meta-1.0.jar") == "metajar" && !pathExists(game + "/mods/server.jar") && metasync.skipped == 1,
              "metafile download, server-only skipped");
        check(!pathExists(game + "/mods/new.jar") && !pathExists(game + "/mods/CR7 MOD NEOFORGE 1.21.1.jar.disabled"), "files of the previous index removed");
        http.requested.clear();
        std::string index2 = index + "\n[[files]]\nfile = \"config/a.txt\"\nhash = \"" + sha256("a") + "\"\n";
        http.files[base + "config/a.txt"] = "a";
        http.files[base + "index.toml"] = index2;
        http.files[base + "pack.toml"] = "version = \"7\"\n[index]\nfile = \"index.toml\"\nhash-format = \"sha256\"\nhash = \"" + sha256(index2) + "\"\n";
        syncPackwiz(storage, http, settings, instance, false, {}, nullptr);
        check(std::count(http.requested.begin(), http.requested.end(), base + "mods/meta.pw.toml") == 0, "unchanged metafile not refetched");
        check(std::count(http.requested.begin(), http.requested.end(), base + "mods/server.pw.toml") == 1, "skipped metafile is rechecked");

        std::string escape = "filename = \"x.jar\"\n[download]\nurl = \"https://cdn.example/x.jar\"\nhash-format = \"sha256\"\nhash = \"" + sha256("x") + "\"\n";
        std::string bad_index = "hash-format = \"sha256\"\n[[files]]\nfile = \"../mods/x.pw.toml\"\nhash = \"" + sha256(escape) + "\"\nmetafile = true\n";
        http.files[base + "index.toml"] = bad_index;
        http.files[base + "pack.toml"] = "[index]\nfile = \"index.toml\"\nhash-format = \"sha256\"\nhash = \"" + sha256(bad_index) + "\"\n";
        rejects("packwiz.format", [&] { syncPackwiz(storage, http, settings, instance, false, {}, nullptr); });
        instance.packwiz_url = "http://packs.example/slop/pack.toml";
        rejects("packwiz.url", [&] { syncPackwiz(storage, http, settings, instance, false, {}, nullptr); });

        std::cout << "PACKWIZ_TEST_PASS checks=" << checks << "\n";
        removeOwnedTree(root);
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << " after " << checks << " checks\n";
        try { removeOwnedTree(root); } catch (...) { }
        return 1;
    }
}
