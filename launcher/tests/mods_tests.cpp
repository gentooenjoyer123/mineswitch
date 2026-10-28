#include "mineswitch/mods.hpp"
#include "mineswitch/import.hpp"
#include <minizip/zip.h>
#include <unistd.h>
#include <fstream>
#include <iostream>

using namespace mineswitch;
static unsigned checks;
static void check(bool value,const char* text){if(!value)throw std::runtime_error(text);checks++;}
template<class F>static void rejects(const std::string& code,F call){try{call();}catch(const Error& e){check(e.code==code,"unexpected rejection");return;}throw std::runtime_error("expected rejection "+code);}
static void jar(const std::string& path,const std::string& id="example") {
    zipFile file=zipOpen64(path.c_str(),APPEND_STATUS_CREATE);check(file,"create real jar");
    std::string metadata="{\"schemaVersion\":1,\"id\":\""+id+"\",\"name\":\"Example Mod\",\"version\":\"1.0\"}";
    check(zipOpenNewFileInZip64(file,"fabric.mod.json",nullptr,nullptr,0,nullptr,0,nullptr,Z_DEFLATED,Z_DEFAULT_COMPRESSION,0)==ZIP_OK,"jar entry");
    check(zipWriteInFileInZip(file,metadata.data(),metadata.size())==ZIP_OK && zipCloseFileInZip(file)==ZIP_OK,"jar metadata");
    check(zipClose(file,nullptr)==ZIP_OK,"close jar");
}
class FixtureHttp:public HttpClient {
public:
    std::string source;bool interrupted=false;unsigned calls=0;
    HttpResponse request(const HttpRequest&,const std::atomic_bool*)override{throw std::runtime_error("unexpected metadata");}
    void download(const Download&,const std::string& destination,ProgressCallback,const std::atomic_bool*)override {
        calls++;std::ifstream input(source,std::ios::binary);std::ofstream output(destination,std::ios::binary);
        output<<input.rdbuf();output.close();if(interrupted)throw Error("cancelled","fixture interruption");
    }
};
int main(){
    char pattern[]="/tmp/mineswitch-mods-XXXXXX";auto directory=mkdtemp(pattern);if(!directory)return 1;
    std::string root=directory;
    try {
        Storage storage(root+"/library");storage.initialize();FixtureHttp http;http.source=root+"/download.jar";jar(http.source);
        Instance instance;instance.id=newId();instance.name="Fixture";instance.root=joinPath(storage.root(),"instances/"+instance.id);
        instance.game.minecraft_version="1.21.1";instance.game.loader={LoaderKind::Fabric,"0.19.3"};
        createDirectories(joinPath(instance.root,"game/mods"));storage.saveInstance(instance);
        ModManager manager(storage,http);check(manager.list(instance).empty(),"empty list");
        CatalogVersion version;version.minecraft_versions={"1.21.1"};version.loaders={LoaderKind::Fabric};
        version.archive.file_name="example.jar";version.archive.urls={"https://cdn.modrinth.com/fixture.jar"};
        version.archive.hashes.sha256=verifyFile(http.source);
        manager.install(instance,version);auto mods=manager.list(instance);
        check(mods.size()==1 && mods[0].enabled && mods[0].id=="example" && mods[0].name=="Example Mod" && mods[0].version=="1.0","actual metadata and install");
        auto path=joinPath(instance.root,"game/mods/example.jar");auto hash=verifyFile(path);
        manager.setEnabled(instance,"example.jar",false);
        check(!pathExists(path) && verifyFile(path+".disabled")==hash,"disable preserves all bytes");
        check(!manager.list(instance)[0].enabled,"disabled state read from disk");
        rejects("mods.exists",[&]{manager.install(instance,version);});
        manager.setEnabled(instance,"example.jar.disabled",true);
        check(verifyFile(path)==hash && !pathExists(path+".disabled"),"enable round trip");
        version.archive.file_name="other.jar";
        rejects("mods.duplicate",[&]{manager.install(instance,version);});
        check(!pathExists(joinPath(instance.root,"game/mods/other.jar")),"duplicate ID never published");
        jar(http.source,"second");version.archive.hashes.sha256=verifyFile(http.source);version.archive.file_name="EXAMPLE.jar";
        rejects("mods.exists",[&]{manager.install(instance,version);});
        version.archive.file_name="second.jar";http.interrupted=true;
        rejects("cancelled",[&]{manager.install(instance,version);});http.interrupted=false;
        check(!pathExists(joinPath(instance.root,"game/mods/second.jar")) && storage.cleanupTransactions()==0,"interrupted download cleans stage without publication");
        version.archive.hashes.sha256=std::string(64,'0');
        try{manager.install(instance,version);throw std::runtime_error("bad hash accepted");}catch(const Error&){checks++;}
        check(verifyFile(path)==hash && manager.list(instance).size()==1,"hash failure preserves installed files");
        version.archive.hashes.sha256=verifyFile(http.source);version.loaders={LoaderKind::Forge};
        rejects("mods.compatibility",[&]{manager.install(instance,version);});version.loaders={LoaderKind::Fabric};
        version.minecraft_versions={"1.20.1"};rejects("mods.compatibility",[&]{manager.install(instance,version);});version.minecraft_versions={"1.21.1"};
        version.archive.manual=true;rejects("mods.manual_download",[&]{manager.install(instance,version);});version.archive.manual=false;
        version.archive.file_name="../escape.jar";rejects("mods.download",[&]{manager.install(instance,version);});
        rejects("mods.filename",[&]{manager.setEnabled(instance,"../example.jar",false);});
        version.archive.file_name="second.jar";manager.install(instance,version);check(manager.list(instance).size()==2,"second independent mod installed");
        auto disabled=path+".disabled";jar(disabled,"collision");
        rejects("mods.exists",[&]{manager.setEnabled(instance,"example.jar",false);});
        check(verifyFile(path)==hash,"destination collision never overwrites original");unlink(disabled.c_str());
        auto linked=joinPath(instance.root,"game/mods/link.jar");check(symlink(http.source.c_str(),linked.c_str())==0,"symlink fixture");
        bool refused=false;try{manager.setEnabled(instance,"link.jar",false);}catch(const Error&){refused=true;}
        check(refused && !pathExists(linked+".disabled"),"symlink cannot be toggled or replace an outside file");unlink(linked.c_str());
        {   // Fabric accepts raw newlines in strings and comments in fabric.mod.json.
            auto lenient=joinPath(instance.root,"game/mods/lenient.jar");
            zipFile file=zipOpen64(lenient.c_str(),APPEND_STATUS_CREATE);check(file,"create lenient jar");
            std::string metadata="{\"schemaVersion\":1, // comment\n\"id\":\"lenient\",\"name\":\"Lenient\",\"description\":\"two\nlines\",\"version\":\"2\"}";
            check(zipOpenNewFileInZip64(file,"fabric.mod.json",nullptr,nullptr,0,nullptr,0,nullptr,Z_DEFLATED,Z_DEFAULT_COMPRESSION,0)==ZIP_OK,"lenient entry");
            check(zipWriteInFileInZip(file,metadata.data(),metadata.size())==ZIP_OK && zipCloseFileInZip(file)==ZIP_OK && zipClose(file,nullptr)==ZIP_OK,"lenient write");
            bool found=false;for(const auto& mod:manager.list(instance))found|=mod.id=="lenient" && mod.name=="Lenient" && mod.problem.empty();
            check(found,"lenient fabric.mod.json is read like Fabric Loader does");unlink(lenient.c_str());
        }
        std::cout<<"MODS_TEST_PASS checks="<<checks<<" real jars,hashes,toggle roundtrip,duplicates,cancellation,collisions,path/loader/version checks\n";
        removeOwnedTree(root);return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';try{removeOwnedTree(root);}catch(...){}return 1;}
}
