#include "mineswitch/catalog.hpp"
#include "mineswitch/content.hpp"
#include "mineswitch/import.hpp"
#include "mineswitch/nbt.hpp"
#include "mineswitch/prepare.hpp"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <minizip/unzip.h>
#include <minizip/zip.h>
#include <unistd.h>

using namespace mineswitch;
static unsigned checks;
static void check(bool value, const char *text) {
    if (!value)
        throw std::runtime_error(text);
    checks++;
}
template <class F> static void rejects(const std::string &code, F call) {
    try {
        call();
    } catch (const Error &e) {
        check(e.code == code, ("unexpected rejection " + e.code).c_str());
        return;
    }
    throw std::runtime_error("expected rejection " + code);
}
static void write(const std::string &path, const std::string &data) {
    std::ofstream out(path, std::ios::binary);
    out << data;
}
static void zip(const std::string &path, const std::string &member, const std::string &data) {
    zipFile file = zipOpen64(path.c_str(), APPEND_STATUS_CREATE);
    check(file, "create zip");
    check(zipOpenNewFileInZip64(file, member.c_str(), nullptr, nullptr, 0, nullptr, 0, nullptr, Z_DEFLATED, 6, 0) == ZIP_OK &&
              zipWriteInFileInZip(file, data.data(), data.size()) == ZIP_OK && zipCloseFileInZip(file) == ZIP_OK,
          "zip member");
    check(zipClose(file, nullptr) == ZIP_OK, "close zip");
}
class NoHttp : public HttpClient {
  public:
    HttpResponse request(const HttpRequest &, const std::atomic_bool *) override { throw std::runtime_error("unexpected request"); }
    void download(const Download &, const std::string &, ProgressCallback, const std::atomic_bool *) override {
        throw std::runtime_error("unexpected download");
    }
};
static Tag level(const std::string &name) {
    Tag root;
    root.type = TagType::Compound;
    auto &data = root.set("Data", TagType::Compound);
    data.set("LevelName", TagType::String).text = name;
    data.set("GameType", TagType::Int).integer = 2;
    data.set("hardcore", TagType::Byte).integer = 0;
    data.set("allowCommands", TagType::Byte).integer = 1;
    data.set("LastPlayed", TagType::Long).integer = 1760000000000LL;
    data.set("RandomSeed", TagType::Long).integer = -42;
    auto &version = data.set("Version", TagType::Compound);
    version.set("Name", TagType::String).text = "1.21.1";
    auto &list = data.set("ScheduledEvents", TagType::List);
    list.list_type = TagType::End;
    auto &longs = data.set("Longs", TagType::LongArray);
    longs.numbers = {1, -2, 1LL << 40};
    data.set("Spawn", TagType::Double).real = 12.5;
    data.set("Scale", TagType::Float).real = 0.25;
    return root;
}

int main() {
    char pattern[] = "/tmp/mineswitch-content-XXXXXX";
    auto directory = mkdtemp(pattern);
    if (!directory)
        return 1;
    std::string root = directory;
    try {
        // NBT round trip keeps every tag type and value.
        auto original = level("Тестовый мир");
        auto parsed = parseNbt(gzipData(writeNbt(original)));
        check(writeNbt(parsed) == writeNbt(original), "gzip NBT round trip is lossless");
        auto *data = parsed.find("Data");
        check(data && data->string("LevelName") == "Тестовый мир" && data->number("RandomSeed") == -42 &&
                  data->find("Longs")->numbers[2] == (1LL << 40) && data->find("Spawn")->real == 12.5,
              "NBT values survive");
        rejects("nbt.invalid", [] { parseNbt(std::string("\x0a\x00\x00\x08\x00\x01x\x00\x09", 9)); });
        rejects("nbt.invalid", [] { parseNbt(std::string("\x08\x00\x00", 3)); });

        Storage storage(root + "/library");
        storage.initialize();
        Instance instance;
        instance.id = newId();
        instance.name = "Fixture";
        instance.root = joinPath(storage.root(), "instances/" + instance.id);
        instance.game.minecraft_version = "1.21.1";
        instance.game.loader = {LoaderKind::Fabric, "0.16.5"};
        instance.notes = "line one\nline two";
        instance.heap_mib = 1536;
        auto game = joinPath(instance.root, "game");
        createDirectories(joinPath(game, "saves/Alpha"));
        createDirectories(joinPath(game, "mods"));
        storage.saveInstance(instance);
        auto stored = storage.listInstances().front();
        check(stored.notes == instance.notes && stored.heap_mib == 1536 && stored.last_played == 0, "new instance fields persist");

        // Worlds
        write(joinPath(game, "saves/Alpha/level.dat"), gzipData(writeNbt(level("Alpha World"))));
        write(joinPath(game, "saves/Alpha/region.mca"), std::string(300000, 'r'));
        write(joinPath(game, "saves/Alpha/session.lock"), "lock");
        createDirectories(joinPath(game, "saves/Broken"));
        write(joinPath(game, "saves/Broken/level.dat"), "not nbt");
        auto worlds = listWorlds(storage, instance);
        check(worlds.size() == 2, "both worlds listed");
        auto alpha = std::find_if(worlds.begin(), worlds.end(), [](const WorldInfo &w) { return w.folder == "Alpha"; });
        check(alpha != worlds.end() && alpha->name == "Alpha World" && alpha->game_type == 2 && alpha->cheats && alpha->version == "1.21.1",
              "level.dat metadata read");
        auto broken = std::find_if(worlds.begin(), worlds.end(), [](const WorldInfo &w) { return w.folder == "Broken"; });
        check(broken != worlds.end() && !broken->problem.empty(), "broken level.dat reported, not fatal");
        renameWorld(storage, instance, "Alpha", "Переименованный");
        check(listWorlds(storage, instance).front().name == "Переименованный", "world rename rewrites level.dat");
        auto copy = copyWorld(storage, instance, "Alpha");
        check(copy == "Alpha-copy" && pathExists(joinPath(game, "saves/Alpha-copy/region.mca")) &&
                  !pathExists(joinPath(game, "saves/Alpha-copy/session.lock")),
              "world copy omits session.lock");
        check(copyWorld(storage, instance, "Alpha") == "Alpha-copy-2", "second copy gets a new folder");
        auto backup = backupWorld(storage, instance, "Alpha");
        unzFile archive = unzOpen64(backup.c_str());
        check(archive && unzLocateFile(archive, "Alpha/region.mca", 1) == UNZ_OK && unzLocateFile(archive, "Alpha/session.lock", 1) != UNZ_OK,
              "backup ZIP contains the world without the lock");
        unzClose(archive);
        deleteWorld(storage, instance, "Alpha-copy-2");
        check(!pathExists(joinPath(game, "saves/Alpha-copy-2")), "world deleted");
        rejects("content.name", [&] { deleteWorld(storage, instance, "../Alpha"); });

        // Servers keep fields this launcher does not edit.
        Tag servers;
        servers.type = TagType::Compound;
        auto &list = servers.set("servers", TagType::List);
        list.list_type = TagType::Compound;
        Tag first;
        first.type = TagType::Compound;
        first.set("name", TagType::String).text = "One";
        first.set("ip", TagType::String).text = "one.example";
        first.set("icon", TagType::String).text = "BASE64";
        first.set("acceptTextures", TagType::Byte).integer = 1;
        list.children.push_back(first);
        write(joinPath(game, "servers.dat"), writeNbt(servers));
        auto entries = listServers(storage, instance);
        check(entries.size() == 1 && entries[0].address == "one.example" && entries[0].original == 0, "servers.dat read");
        entries[0].name = "Renamed";
        entries.push_back({"Two", "two.example:25570", -1});
        saveServers(storage, instance, entries);
        auto saved = parseNbt(readTextFile(joinPath(game, "servers.dat"), 1 << 20));
        const auto &items = saved.find("servers")->children;
        check(items.size() == 2 && items[0].string("name") == "Renamed" && items[0].string("icon") == "BASE64" &&
                  items[0].number("acceptTextures") == 1 && items[1].string("ip") == "two.example:25570",
              "server edits keep unknown fields");
        rejects("content.server", [&] { saveServers(storage, instance, {{"x", "", -1}}); });

        // Resource packs: zip with pack.mcmeta, folder pack, toggle, remove.
        createDirectories(joinPath(game, "resourcepacks/Folder Pack"));
        write(joinPath(game, "resourcepacks/Folder Pack/pack.mcmeta"), "{\"pack\":{\"description\":{\"text\":\"Folder\",\"extra\":[\" pack\"]}}}");
        zip(joinPath(game, "resourcepacks/Zip Pack.zip"), "pack.mcmeta", "{\"pack\":{\"description\":\"\xc2\xa7" "aGreen\"}}");
        write(joinPath(game, "resourcepacks/readme.txt"), "ignored");
        auto packs = listPacks(storage, instance, ContentKind::ResourcePack);
        check(packs.size() == 2 && packs[0].name == "Folder Pack" && packs[0].directory && packs[0].description == "Folder pack" &&
                  packs[1].name == "Zip Pack" && packs[1].description == "Green",
              "resource packs and descriptions listed");
        setPackEnabled(storage, instance, ContentKind::ResourcePack, "Zip Pack.zip", false);
        check(pathExists(joinPath(game, "resourcepacks/Zip Pack.zip.disabled")), "pack disabled by rename");
        packs = listPacks(storage, instance, ContentKind::ResourcePack);
        check(!packs[1].enabled && packs[1].name == "Zip Pack", "disabled pack keeps its name");
        removeContent(storage, instance, ContentKind::ResourcePack, "Folder Pack");
        check(!pathExists(joinPath(game, "resourcepacks/Folder Pack")), "folder pack removed");
        rejects("content.name", [&] { removeContent(storage, instance, ContentKind::Mod, "../instance.json"); });

        // Digests match an independent MurmurHash2/SHA-1 implementation.
        auto sample = joinPath(root, "sample.bin");
        write(sample, std::string("Hello World\n\tfingerprint 12345 \r\n!"));
        auto digest = digestFile(sample);
        check(digest.sha1 == "1a3a2d06acfdd0325c3d055e027b53da432f5320", "SHA-1 digest");
        check(digest.fingerprint == 3246747910u, "CurseForge fingerprint");

        // Logs and screenshots
        createDirectories(joinPath(game, "logs"));
        createDirectories(joinPath(game, "crash-reports"));
        write(joinPath(game, "logs/latest.log"), "a\r\nb\tc\n");
        write(joinPath(game, "logs/2026-10-01-1.log.gz"), gzipData("old\nlog\n"));
        write(joinPath(game, "crash-reports/crash-1.txt"), "boom");
        auto logs = listLogs(storage, instance);
        check(logs.size() == 3 && logs[0].name == "latest.log", "logs listed with latest first");
        auto lines = readLog(storage, instance, logs[0].path);
        check(lines.size() == 2 && lines[1] == "b    c", "log lines normalized");
        auto gz = std::find_if(logs.begin(), logs.end(), [](const MediaFile &m) { return m.name == "2026-10-01-1.log.gz"; });
        check(gz != logs.end() && readLog(storage, instance, gz->path) == std::vector<std::string>{"old", "log"}, "gzip log decompressed");
        rejects("content.path", [&] { readLog(storage, instance, sample); });
        createDirectories(joinPath(game, "screenshots"));
        write(joinPath(game, "screenshots/a.png"), "png");
        write(joinPath(game, "screenshots/b.txt"), "no");
        check(listScreenshots(storage, instance).size() == 1, "screenshots filtered by type");
        deleteScreenshot(storage, instance, "a.png");
        check(listScreenshots(storage, instance).empty(), "screenshot deleted");

        // Whole-instance copy, export and delete.
        write(joinPath(instance.root, "launch-plan.json"), "{}");
        write(joinPath(game, "mods/example.jar"), "jar");
        auto duplicate = duplicateInstance(storage, instance, "Копия");
        check(duplicate.id != instance.id && duplicate.name == "Копия" && duplicate.state == InstanceState::NeedsPreparation &&
                  pathExists(joinPath(duplicate.root, "game/mods/example.jar")) && !pathExists(joinPath(duplicate.root, "launch-plan.json")) &&
                  pathExists(joinPath(duplicate.root, "game/saves/Alpha/level.dat")) && !pathExists(joinPath(duplicate.root, "game/logs")),
              "duplicate copies game data and drops generated state");
        check(storage.listInstances().size() == 2, "duplicate is a listed instance");
        auto exported = exportInstance(storage, instance);
        NoHttp offline;
        ArchiveImporter importer(storage, offline, parse_pack_manifest);
        auto plan = importer.inspectArchive(exported);
        check(plan.kind == PackKind::MultiMC && plan.game.loader.kind == LoaderKind::Fabric && plan.game.loader.version == "0.16.5" &&
                  plan.state != InstanceState::Unsupported && plan.name == "Fixture",
              "exported ZIP imports back as a PolyMC instance");
        deleteInstance(storage, duplicate);
        check(storage.listInstances().size() == 1 && !pathExists(duplicate.root), "instance deleted");

        // User JVM options
        check(parseJvmArguments("  -Dfoo=bar   -XX:+AlwaysPreTouch -Xss2m ").size() == 3, "allowed JVM options");
        rejects("jvm_args", [] { parseJvmArguments("-Xmx4g"); });
        rejects("jvm_args", [] { parseJvmArguments("-javaagent:x.jar"); });
        rejects("jvm_args", [] { parseJvmArguments("-XX:+UseG1GC"); });
        rejects("jvm_args", [] { parseJvmArguments("-XX:ReservedCodeCacheSize=1g"); });
        check(parseJvmArguments("").empty(), "empty JVM options");
        removeOwnedTree(root);
    } catch (const std::exception &e) {
        std::cerr << "CONTENT_TEST_FAIL " << e.what() << "\n";
        return 1;
    }
    std::cout << "CONTENT_TESTS_PASS " << checks << " checks\n";
    return 0;
}
