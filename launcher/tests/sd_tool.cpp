#include "mineswitch/catalog.hpp"
#include "mineswitch/import.hpp"
#include "mineswitch/prepare.hpp"
#include <algorithm>
#include <iostream>
using namespace mineswitch;
int main(int argc, char** argv) {
    if (argc != 4) { std::cerr << "Usage: sd_tool ROOT inspect|import|prepare ARCHIVE_OR_INSTANCE_ID\n"; return 2; }
    try {
        Storage storage(argv[1]); storage.initialize(); storage.cleanupTransactions();
        Settings settings = storage.loadSettings();
        CurlHttpClient http; CatalogClient catalog(http, settings);
        ArchiveImporter importer(storage, http, parse_pack_manifest,
            [&](PackPlan& plan, const std::atomic_bool* cancel) { catalog.resolve_pack(plan, cancel); });
        std::string command(argv[2]);
        if (command == "inspect") {
            auto plan = importer.inspectArchive(argv[3]);
            std::cout << "PACK_INSPECT_OK name=" << plan.name << " minecraft=" << plan.game.minecraft_version
                      << " downloads=" << plan.downloads.size() << '\n';
        } else if (command == "import") {
            auto instance = importer.importArchive(argv[3]);
            std::cout << "PACK_IMPORT_OK id=" << instance.id << " name=" << instance.name
                      << " bytes=" << instance.installed_bytes << '\n';
        } else if (command == "prepare") {
            auto instances = storage.listInstances();
            auto item = std::find_if(instances.begin(), instances.end(), [&](const Instance& i) { return i.id == argv[3]; });
            if (item == instances.end()) throw Error("instance_missing", "Instance not found");
            GamePreparer preparer(storage, http);
            auto plan = preparer.prepare(*item, settings, {newId(), "Player"});
            std::cout << "LAUNCH_PLAN_OK main=" << plan.main_class << " preparation_only=" << plan.preparation_only
                      << " options=" << plan.jvm_options.size() << " arguments=" << plan.game_arguments.size() << '\n';
        } else return 2;
        return 0;
    } catch (const Error& e) { std::cerr << e.code << ": " << e.what() << '\n'; return 1; }
      catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
