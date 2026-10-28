#include "mineswitch/storage.hpp"
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <stdexcept>

using namespace mineswitch;
namespace {
int checks = 0;
void check(bool condition, const std::string& label) {
    if (!condition) throw std::runtime_error("FAIL: " + label);
    ++checks;
}
void rejects(const std::string& code, const std::function<void()>& action) {
    try { action(); }
    catch (const Error& error) {
        check(error.code == code, "expected " + code + ", received " + error.code);
        return;
    }
    throw std::runtime_error("FAIL: expected " + code);
}
struct Temporary {
    std::string path;
    Temporary() {
        char pattern[] = "/tmp/mineswitch-storage-XXXXXX";
        const char* value = ::mkdtemp(pattern);
        if (!value) throw std::runtime_error("Cannot create test directory");
        path = value;
    }
    ~Temporary() { try { removeOwnedTree(path); } catch (...) {} }
};
void move(const std::string& from, const std::string& to) {
    if (::rename(from.c_str(), to.c_str()) != 0) throw std::runtime_error("Cannot stage journal fixture");
}
}
int main() {
    try {
        Temporary temporary;
        Storage storage(joinPath(temporary.path, "library"));
        auto settings = storage.loadSettings();
        check(settings.root == storage.root() && settings.heap_mib == 1024, "first-run settings");
        check(storage.loadProfiles().empty() && storage.listInstances().empty(), "first-run lists");
        check(storage.availableBytes() > 0, "available storage bytes");
        Profile profile {newId(), "Local_Player"};
        storage.saveProfiles({profile});
        const auto profiles = storage.loadProfiles();
        check(profiles.size() == 1 && profiles[0].id == profile.id && profiles[0].username == profile.username,
              "offline profile round trip");
        settings.profile_id = profile.id; settings.heap_mib = 1536;
        settings.curseforge_key = "local-fixture-key";
        settings.reduced_motion = true; settings.show_incompatible = true;
        storage.saveSettings(settings);
        const auto loaded = storage.loadSettings();
        check(loaded.profile_id == settings.profile_id && loaded.heap_mib == 1536 && loaded.reduced_motion &&
              loaded.show_incompatible && loaded.curseforge_key == settings.curseforge_key, "settings round trip");
        auto expanded_settings = settings;
        expanded_settings.heap_mib = 5900;
        storage.saveSettings(expanded_settings);
        check(storage.loadSettings().heap_mib == 5900, "expanded-memory preference persists without clamping");
        const auto legacy_path = joinPath(storage.root(), "settings.json");
        auto legacy = readTextFile(legacy_path);
        auto heap_value = legacy.find("5900");
        check(heap_value != std::string::npos, "heap setting fixture serialized");
        legacy.replace(heap_value, 4, "6144");
        atomicWriteText(legacy_path, legacy);
        check(storage.loadSettings().heap_mib == 5900, "legacy6144MiB preference migrates to5900MiB");
        expanded_settings.heap_mib = 5901;
        rejects("storage_schema", [&] { storage.saveSettings(expanded_settings); });
        storage.saveSettings(settings);
        rejects("storage_schema", [&] { storage.saveProfiles({{newId(), "User name"}}); });
        auto invalid_settings = settings; invalid_settings.heap_mib = 0;
        rejects("storage_schema", [&] { storage.saveSettings(invalid_settings); });

        Instance instance;
        instance.id = newId(); instance.name = "Local instance";
        instance.root = "/descriptive-only";
        instance.game.minecraft_version = "1.21.1";
        instance.game.loader = {LoaderKind::Fabric, "0.16.10"};
        instance.game.java_major = 21;
        instance.state = InstanceState::NeedsFiles; instance.reason = "One file missing";
        instance.provider_project = "project1"; instance.provider_version = "version1";
        instance.installed_bytes = 12345;
        storage.saveInstance(instance);
        const auto instances = storage.listInstances();
        check(instances.size() == 1, "instance saved");
        const auto& copy = instances.front();
        check(copy.id == instance.id && copy.name == instance.name && copy.game.minecraft_version == "1.21.1" &&
              copy.game.loader.kind == LoaderKind::Fabric && copy.game.loader.version == "0.16.10" &&
              copy.game.java_major == 21 && copy.state == instance.state && copy.reason == instance.reason &&
              copy.provider_project == instance.provider_project && copy.provider_version == instance.provider_version &&
              copy.installed_bytes == instance.installed_bytes, "instance fields round trip");
        check(copy.root == joinPath(storage.root(), "instances/" + instance.id), "instance root is derived");

        const auto settings_path = joinPath(storage.root(), "settings.json");
        const auto saved_settings = readTextFile(settings_path);
        atomicWriteText(settings_path + ".tmp", "unfinished write");
        check(storage.loadSettings().heap_mib == 1536 && !pathExists(settings_path + ".tmp"), "uncommitted temporary discarded");
        move(settings_path, settings_path + ".old");
        atomicWriteText(settings_path + ".tmp", "unfinished replacement");
        check(storage.loadSettings().heap_mib == 1536 && pathExists(settings_path) &&
              !pathExists(settings_path + ".old"), "interrupted rename restores old settings");
        atomicWriteText(settings_path + ".old", saved_settings);
        check(storage.loadSettings().heap_mib == 1536 && !pathExists(settings_path + ".old"), "committed current wins over backup");
        atomicWriteText(settings_path, "{\"schema\":1,\"schema\":1}");
        rejects("storage_json", [&] { storage.loadSettings(); });
        atomicWriteText(settings_path, saved_settings);
        const auto changed = saved_settings.find("\"heap_mib\": 1536");
        check(changed != std::string::npos, "fixture contains integer heap value");
        auto wrong_type = saved_settings;
        wrong_type.replace(changed, std::string("\"heap_mib\": 1536").size(), "\"heap_mib\": true");
        atomicWriteText(settings_path, wrong_type);
        rejects("storage_schema", [&] { storage.loadSettings(); });
        atomicWriteText(settings_path, saved_settings);

        const auto bounded = joinPath(temporary.path, "bounded.txt");
        atomicWriteText(bounded, std::string(65, 'a'));
        rejects("storage_size", [&] { readTextFile(bounded, 64); });
        check(readTextFile(bounded, 65).size() == 65, "bounded reader accepts its exact limit");
        check(validRelativePath("config/options.txt") && validRelativePath("resourcepacks/Local Pack.zip"), "ordinary relative paths");
        check(!validRelativePath("../config") && !validRelativePath("/config") &&
              !validRelativePath("config\\options.txt") && !validRelativePath("config/file. "), "ambiguous relative paths rejected");
        const auto id = newId();
        check(id.size() == 32 && id.find_first_not_of("0123456789abcdef") == std::string::npos, "generated ID format");

        const auto abandoned = joinPath(storage.root(), "transactions/" + id);
        createDirectories(joinPath(abandoned, "instances/unfinished/game"));
        atomicWriteText(joinPath(abandoned, "instances/unfinished/game/options.txt"), "staged data");
        const auto unrecognized = joinPath(storage.root(), "transactions/notes");
        createDirectories(unrecognized);
        const auto ordinary_file = joinPath(storage.root(), "transactions/" + newId());
        atomicWriteText(ordinary_file, "ordinary file");
        storage.initialize();
        check(pathExists(abandoned), "routine initialize never cleans active transactions");
        check(storage.cleanupTransactions() == 1 && !pathExists(abandoned), "explicit startup cleanup removes generated staging");
        check(pathExists(unrecognized) && pathExists(ordinary_file), "startup cleanup preserves unrecognized entries and files");
        check(storage.listInstances().size() == 1 && pathExists(copy.root), "startup cleanup preserves published instances");
        check(storage.cleanupTransactions() == 0, "startup cleanup is repeatable");
        atomicWriteText(joinPath(copy.root, "instance.json"), "incomplete metadata");
        check(storage.listInstances().front().state == InstanceState::Broken, "corrupt instance stays visible");
        std::cout << "Storage tests passed: " << checks << " checks\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
