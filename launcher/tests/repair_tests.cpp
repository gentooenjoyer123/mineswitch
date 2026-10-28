#include "mineswitch/repair.hpp"
#include <iostream>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <stdexcept>
using namespace mineswitch;
namespace {
int checks = 0;
void check(bool value, const char* label) { if (!value) throw std::runtime_error(label); ++checks; }
void moveFile(const std::string& from, const std::string& to) {
    if (::rename(from.c_str(), to.c_str())) throw std::runtime_error("cannot stage interrupted metadata rename");
}
struct Temporary {
    std::string root;
    Temporary() { char name[]="/tmp/mineswitch-repair-XXXXXX"; auto* value=::mkdtemp(name); if(!value)throw std::runtime_error("temporary directory");root=value; }
    ~Temporary() { try { removeOwnedTree(root); } catch (...) {} }
};
constexpr const char* hello_sha1 = "aaf4c61ddcc5e8a2dabede0f3b482cd9aea9434d";
std::string missing(const std::string& path) {
    return "{\"path\":\""+path+"\",\"bytes\":5,\"sha1\":\""+hello_sha1+"\",\"manual_url\":\"https://example.org/file\"}";
}
}
int main() {
    try {
        Temporary temp; Storage storage(temp.root); storage.initialize();
        Instance item; item.id=newId();item.root=joinPath(storage.root(),"instances/"+item.id);
        item.name="Manual completion";item.game.minecraft_version="1.21.1";item.state=InstanceState::NeedsFiles;
        createDirectories(joinPath(item.root,"game"));storage.saveInstance(item);
        const auto list=joinPath(item.root,"missing-files.json");
        const auto managed=joinPath(item.root,"managed-files.json");
        atomicWriteText(list,"["+missing("mods/one.jar")+","+missing("mods/two.jar")+"]");
        atomicWriteText(managed,"[]");
        check(missingFiles(item).size()==2,"published missing-file data loads");
        auto current=verifyManualFiles(storage,item);
        check(current.state==InstanceState::NeedsFiles && missingFiles(current).size()==2,"absent files stay unresolved");
        check(!pathExists(joinPath(item.root,"game/mods")),"verification leaves absent parent directory unresolved without creating it");
        createDirectories(joinPath(item.root,"game/mods"));
        atomicWriteText(joinPath(item.root,"game/mods/one.jar"),"hello");
        current=verifyManualFiles(storage,current);
        check(current.state==InstanceState::NeedsFiles && missingFiles(current).size()==1,"partial completion stays non-ready");
        check(current.installed_bytes==5,"verified file contributes to receipt size");
        auto receipt=readTextFile(managed);
        current=verifyManualFiles(storage,current);
        check(current.installed_bytes==5 && receipt==readTextFile(managed),"repeated verification is idempotent");
        moveFile(managed,managed+".old");
        atomicWriteText(managed+".tmp","unfinished receipt replacement");
        current=verifyManualFiles(storage,current);
        check(current.installed_bytes==5 && receipt==readTextFile(managed),"interrupted receipt rename recovers all prior accounting");
        check(!pathExists(managed+".old") && !pathExists(managed+".tmp"),"receipt journal is consumed on read");
        moveFile(list,list+".old");
        atomicWriteText(list+".tmp","unfinished missing-list replacement");
        check(missingFiles(current).size()==1,"missing-file display recovers an interrupted list rename");
        current=verifyManualFiles(storage,current);
        check(current.state==InstanceState::NeedsFiles && current.installed_bytes==5,
              "missing-list recovery preserves verification requirements and receipt accounting");
        check(!pathExists(list+".old") && !pathExists(list+".tmp"),"missing-list journal is consumed on read");
        atomicWriteText(list+".old","[]");
        check(missingFiles(current).size()==1 && !pathExists(list+".old"),"committed missing list wins over an older backup");
        atomicWriteText(joinPath(item.root,"game/mods/two.jar"),"world");
        current=verifyManualFiles(storage,current);
        check(current.state==InstanceState::NeedsFiles && missingFiles(current).size()==1,"same-size file still needs the published hash");
        atomicWriteText(joinPath(item.root,"game/mods/two.jar"),"hello");
        current=verifyManualFiles(storage,current);
        check(current.state==InstanceState::NeedsPreparation && missingFiles(current).empty(),"all matching files unlock preparation, not Ready");
        check(current.installed_bytes==10 && storage.listInstances().front().state==InstanceState::NeedsPreparation,"completed metadata persists");
        check(readTextFile(joinPath(item.root,"game/mods/one.jar"))=="hello","verification does not modify game files");
        current.state=InstanceState::NeedsFiles;storage.saveInstance(current);
        atomicWriteText(list,"[{\"path\":\"mods/one.jar\",\"bytes\":5}]");
        current=verifyManualFiles(storage,current);
        check(current.state==InstanceState::NeedsFiles,"a file without a published digest remains unresolved");
        atomicWriteText(list,"["+missing("mods/one.jar")+"]");
        auto before=readTextFile(list);std::atomic_bool cancel{true};
        try { (void)verifyManualFiles(storage,current,{},&cancel); throw std::runtime_error("expected cancellation"); }
        catch(const Error& e){check(e.code=="cancelled","cancellation reported");}
        check(readTextFile(list)==before,"cancelled verification does not commit metadata");
        cancel=false;
        try { (void)verifyManualFiles(storage,current,[&](const Progress&){cancel=true;},&cancel); throw std::runtime_error("expected cancellation"); }
        catch(const Error& e){check(e.code=="cancelled","cancellation during hashing reported");}
        check(readTextFile(list)==before,"mid-operation cancellation retains the missing list");
        cancel=false;
        moveFile(list,list+".saved");
        try { (void)verifyManualFiles(storage,current); throw std::runtime_error("expected missing metadata error"); }
        catch(const Error& e){check(e.code=="manual_metadata","truly absent missing list cannot unlock preparation");}
        moveFile(list+".saved",list);
        check(storage.listInstances().front().state==InstanceState::NeedsFiles,"absent metadata does not publish a new state");

        const auto bounded=joinPath(temp.root,"bounded.json");
        atomicWriteText(bounded,"[]\n");
        std::unique_ptr<json_t,decltype(&json_decref)> value(json_pack("[s]","ordinary fixture record"),json_decref);
        check(static_cast<bool>(value),"JSON fixture created");
        try { atomicWriteJson(bounded,value.get(),8); throw std::runtime_error("expected serialization bound"); }
        catch(const Error& e){check(e.code=="storage_size","JSON serialization enforces caller byte budget");}
        check(readTextFile(bounded)=="[]\n" && !pathExists(bounded+".tmp"),"oversized serialization cannot replace committed metadata");
        atomicWriteJson(bounded,value.get(),kInstanceManifestMaxBytes);
        std::unique_ptr<json_t,decltype(&json_decref)> recovered(readStoredJson(bounded,kInstanceManifestMaxBytes),json_decref);
        check(json_is_array(recovered.get()) && json_array_size(recovered.get())==1,"shared manifest budget supports writer and reader");
        try { (void)readStoredJson(bounded,4); throw std::runtime_error("expected bounded recovery read"); }
        catch(const Error& e){check(e.code=="storage_size","recovery-aware reader remains bounded");}
        check(readStoredJson(joinPath(temp.root,"absent.json"),kInstanceManifestMaxBytes)==nullptr,"recovery reader returns null for genuinely absent metadata");
        const auto text=joinPath(temp.root,"ordinary.txt");
        atomicWriteText(text,"current text");atomicWriteText(text+".old","ordinary neighboring file");
        check(readTextFile(text)=="current text" && pathExists(text+".old"),"plain text reads never consume neighboring files as journals");
        std::cout<<"REPAIR_TEST_PASS checks="<<checks<<'\n';return 0;
    } catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
