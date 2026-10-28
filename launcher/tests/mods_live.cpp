#include "mineswitch/catalog.hpp"
#include "mineswitch/mods.hpp"
#include <unistd.h>
#include <iostream>
using namespace mineswitch;
int main(){
    char pattern[]="/tmp/mineswitch-live-mods-XXXXXX";const char* made=mkdtemp(pattern);if(!made)return 1;
    std::string root=made;
    try {
        Storage storage(root);storage.initialize();CurlHttpClient http;
        Settings settings;CatalogClient catalog(http,settings);ModManager manager(storage,http);
        for(auto provider:{Provider::Modrinth,Provider::CurseForge}) {
            auto versions=catalog.versions(provider,provider==Provider::Modrinth?"modmenu":"308702","1.21.1",nullptr,true,LoaderKind::Fabric);
            if(versions.empty())throw Error("live.empty","No compatible Mod Menu version");
            auto version=catalog.version(provider,versions.front().project_id,versions.front().id,nullptr,true);
            Instance instance;instance.id=newId();instance.name="Owned live test";
            instance.root=joinPath(root,"instances/"+instance.id);instance.game.minecraft_version="1.21.1";instance.game.loader={LoaderKind::Fabric,"0.19.3"};
            createDirectories(joinPath(instance.root,"game/mods"));storage.saveInstance(instance);
            manager.install(instance,version);
            auto list=manager.list(instance);if(list.size()!=1||!list[0].enabled)throw Error("live.list","Installed mod absent");
            manager.setEnabled(instance,list[0].filename,false);list=manager.list(instance);
            if(list[0].enabled)throw Error("live.toggle","Disabled state not persisted");
            manager.setEnabled(instance,list[0].filename,true);
            std::cout<<"LIVE_MOD_PASS provider="<<(provider==Provider::Modrinth?"Modrinth":"CurseForge")
                     <<" id="<<version.project_id<<" version="<<version.id<<" bytes="<<version.archive.bytes
                     <<" name="<<list[0].name<<" hashes_verified=1 toggle_roundtrip=1\n";
        }
        removeOwnedTree(root);return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';try{removeOwnedTree(root);}catch(...){}return 1;}
}
