#include "mineswitch/mods.hpp"
#include "mineswitch/content.hpp"
#include "mineswitch/import.hpp"
#include <set>
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <memory>
#include <mutex>
#include <sys/stat.h>
#include <unistd.h>
#include <minizip/unzip.h>
#ifdef __linux__
#include <fcntl.h>
#include <sys/syscall.h>
#endif

namespace mineswitch {
namespace {
std::mutex mods_mutex;
constexpr std::uint64_t max_mod_bytes=1024ULL*1024*1024;
std::string lower(std::string s) {
    for(char& c:s)if(c>='A' && c<='Z')c=static_cast<char>(c-'A'+'a');
    return s;
}
bool ends(const std::string& s,const std::string& suffix) {
    return s.size()>=suffix.size() && s.compare(s.size()-suffix.size(),suffix.size(),suffix)==0;
}
bool mod_name(const std::string& name) {
    auto normalized=lower(name);
    return validRelativePath(name) && name.find('/')==std::string::npos &&
           (ends(normalized,".jar") || ends(normalized,".jar.disabled"));
}
std::string instance_root(const Storage& storage,const Instance& instance) {
    if(instance.id.empty() || instance.id.size()>64 ||
       instance.id.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-")!=std::string::npos)
        throw Error("mods.instance","Invalid instance");
    auto root=joinPath(storage.root(),"instances/"+instance.id);
    if(!instance.root.empty() && instance.root!=root)throw Error("mods.instance","The instance is outside the library");
    requireSafePath(root);return root;
}
void cancelled(const std::atomic_bool* stop) {
    if(stop && stop->load())throw Error("cancelled","Operation cancelled");
}
std::string zip_text(unzFile zip,const char* member) {
    if(unzLocateFile(zip,member,1)!=UNZ_OK)return {};
    unz_file_info64 info{};
    if(unzGetCurrentFileInfo64(zip,&info,nullptr,0,nullptr,0,nullptr,0)!=UNZ_OK || info.uncompressed_size>1024*1024)
        throw Error("mods.metadata","Mod metadata is invalid or too large");
    if(unzOpenCurrentFile(zip)!=UNZ_OK)throw Error("mods.metadata","Could not read mod metadata");
    std::string data(static_cast<std::size_t>(info.uncompressed_size),'\0');
    std::size_t done=0;int count=0;
    while(done<data.size() && (count=unzReadCurrentFile(zip,data.data()+done,static_cast<unsigned>(data.size()-done)))>0)
        done+=static_cast<unsigned>(count);
    int closed=unzCloseCurrentFile(zip);
    if(done!=data.size() || count<0 || closed!=UNZ_OK)throw Error("mods.metadata","Mod metadata is corrupted");
    return data;
}
std::string field(json_t* object,const char* key) {
    auto value=json_object_get(object,key);
    if(!json_is_string(value))return {};
    auto size=json_string_length(value);
    if(size>512 || std::strlen(json_string_value(value))!=size)return {};
    return std::string(json_string_value(value),size);
}
// Fabric Loader reads fabric.mod.json leniently: raw newlines/tabs inside
// strings and comments are accepted by the game, so they are accepted here.
std::string lenient_json(const std::string& in) {
    std::string out;out.reserve(in.size());
    bool string=false;
    for(std::size_t i=0;i<in.size();++i){
        char c=in[i];
        if(string){
            if(c=='\\' && i+1<in.size()){out+=c;out+=in[++i];continue;}
            if(c=='"')string=false;
            if(c=='\n'){out+="\\n";continue;}
            if(c=='\r'){out+="\\r";continue;}
            if(c=='\t'){out+="\\t";continue;}
            out+=c;continue;
        }
        if(c=='"')string=true;
        else if(c=='/' && i+1<in.size() && in[i+1]=='/'){while(i<in.size() && in[i]!='\n')++i;out+='\n';continue;}
        else if(c=='/' && i+1<in.size() && in[i+1]=='*'){auto end=in.find("*/",i+2);i=end==std::string::npos?in.size():end+1;out+=' ';continue;}
        out+=c;
    }
    return out;
}
InstalledMod inspect(const std::string& path,const std::string& filename,LoaderKind required=LoaderKind::Unknown) {
    InstalledMod result;result.filename=filename;result.name=filename;result.enabled=!ends(lower(filename),".disabled");
    requireSafePath(path);
    struct stat info{};
    if(lstat(path.c_str(),&info)!=0 || !S_ISREG(info.st_mode) || info.st_size<0)throw Error("mods.file","Expected a regular mod file");
    result.bytes=static_cast<std::uint64_t>(info.st_size);
    unzFile raw=unzOpen64(path.c_str());
    if(!raw)throw Error("mods.archive","The file is not a valid JAR");
    struct Close {unzFile file;~Close(){unzClose(file);}} close{raw};
    auto fabric=zip_text(raw,"fabric.mod.json");
    auto quilt=fabric.empty()?zip_text(raw,"quilt.mod.json"):std::string{};
    auto forge=(required==LoaderKind::Forge || required==LoaderKind::NeoForge)?zip_text(raw,"META-INF/mods.toml"):std::string{};
    auto neo=required==LoaderKind::NeoForge?zip_text(raw,"META-INF/neoforge.mods.toml"):std::string{};
    bool supported=required==LoaderKind::Unknown ||
        (required==LoaderKind::Fabric && !fabric.empty()) ||
        (required==LoaderKind::Quilt && (!fabric.empty() || !quilt.empty())) ||
        (required==LoaderKind::Forge && !forge.empty()) ||
        (required==LoaderKind::NeoForge && (!neo.empty() || !forge.empty()));
    if(!supported)throw Error("mods.loader","The JAR has no metadata for the selected loader");
    auto text=fabric.empty()?quilt:fabric;
    if(!text.empty()) {
        json_error_t error{};
        text=lenient_json(text);
        std::unique_ptr<json_t,decltype(&json_decref)> root(json_loadb(text.data(),text.size(),JSON_REJECT_DUPLICATES,&error),json_decref);
        if(!root || !json_is_object(root.get()))throw Error("mods.metadata","The mod JSON is corrupted");
        auto mod=fabric.empty()?json_object_get(root.get(),"quilt_loader"):root.get();
        if(!json_is_object(mod))throw Error("mods.metadata","The mod description is missing");
        result.id=field(mod,"id");result.version=field(mod,"version");
        auto name=field(fabric.empty()?json_object_get(mod,"metadata"):mod,"name");
        if(!name.empty())result.name=name;
        if(result.id.empty())throw Error("mods.metadata","The mod ID is missing");
    }
    return result;
}
std::vector<InstalledMod> list_at(const std::string& folder) {
    std::vector<InstalledMod> result;
    if(!pathExists(folder))return result;
    requireSafePath(folder);
    DIR* raw=opendir(folder.c_str());if(!raw)throw Error("mods.list","Could not open the mods folder");
    struct Close {DIR* d;~Close(){closedir(d);}} close{raw};
    while(auto entry=readdir(raw)) {
        std::string filename=entry->d_name;
        if(!mod_name(filename))continue;
        if(result.size()>=4096)throw Error("mods.limit","Too many mods in one instance");
        auto path=joinPath(folder,filename);
        try {result.push_back(inspect(path,filename));}
        catch(const Error& error) {
            InstalledMod mod;mod.filename=filename;mod.name=filename;
            mod.enabled=!ends(lower(filename),".disabled");mod.problem=error.what();result.push_back(std::move(mod));
        }
    }
    std::sort(result.begin(),result.end(),[](const auto& a,const auto& b){return lower(a.name)<lower(b.name);});
    return result;
}
void publish(const std::string& from,const std::string& to) {
    requireSafePath(from);requireSafePath(to,true);
#ifdef __linux__
    if(syscall(SYS_renameat2,AT_FDCWD,from.c_str(),AT_FDCWD,to.c_str(),1u)!=0)
#else
    // Horizon's filesystem rename fails when the destination already exists.
    if(std::rename(from.c_str(),to.c_str())!=0)
#endif
        throw Error("mods.rename","Could not move the mod file: "+std::string(std::strerror(errno)));
}
void check_conflicts(const std::vector<InstalledMod>& mods,const InstalledMod& incoming,const std::string& ignore={}) {
    for(const auto& mod:mods) {
        if(mod.filename==ignore)continue;
        auto old_name=lower(mod.filename),new_name=lower(incoming.filename);
        if(ends(old_name,".disabled"))old_name.resize(old_name.size()-9);
        if(ends(new_name,".disabled"))new_name.resize(new_name.size()-9);
        if(old_name==new_name)throw Error("mods.exists","File already installed: "+mod.filename);
        if(incoming.enabled && mod.enabled && !incoming.id.empty() && mod.id==incoming.id)
            throw Error("mods.duplicate","This mod is already enabled: "+mod.name+". Disable the old version first.");
    }
}
}
std::vector<InstalledMod> ModManager::list(const Instance& instance) {
    std::lock_guard<std::mutex> lock(mods_mutex);
    return list_at(joinPath(instance_root(storage_,instance),"game/mods"));
}
void ModManager::setEnabled(const Instance& instance,const std::string& filename,bool enabled) {
    std::lock_guard<std::mutex> lock(mods_mutex);
    if(!mod_name(filename))throw Error("mods.filename","Invalid mod file name");
    bool current=!ends(lower(filename),".disabled");if(current==enabled)return;
    auto folder=joinPath(instance_root(storage_,instance),"game/mods");
    auto from=joinPath(folder,filename);
    auto target=enabled?filename.substr(0,filename.size()-9):filename+".disabled";
    auto mods=list_at(folder);
    auto found=std::find_if(mods.begin(),mods.end(),[&](const auto& mod){return mod.filename==filename;});
    if(found==mods.end())throw Error("mods.missing","The mod no longer exists");
    auto incoming=*found;incoming.filename=target;incoming.enabled=enabled;
    check_conflicts(mods,incoming,filename);
    publish(from,joinPath(folder,target));
}
namespace {
struct Staged {
    std::string stage,path;
    InstalledMod mod;
    std::uint64_t bytes=0;
    ~Staged(){if(!stage.empty())try{removeOwnedTree(stage);}catch(...) {}}
};
}
static void download_mod(Storage& storage_,HttpClient& http_,const Instance& instance,const CatalogVersion& version,
                         ProgressCallback progress,const std::atomic_bool* stop,Staged& staged) {
    cancelled(stop);
    auto kind=instance.game.loader.kind;
    if(kind==LoaderKind::Vanilla || kind==LoaderKind::Unknown)throw Error("mods.loader","This instance needs a mod loader first");
    bool loader=std::find(version.loaders.begin(),version.loaders.end(),kind)!=version.loaders.end() ||
        (kind==LoaderKind::Quilt && std::find(version.loaders.begin(),version.loaders.end(),LoaderKind::Fabric)!=version.loaders.end());
    if(!loader || std::find(version.minecraft_versions.begin(),version.minecraft_versions.end(),instance.game.minecraft_version)==version.minecraft_versions.end())
        throw Error("mods.compatibility","This mod version does not match the instance's Minecraft or loader");
    auto file=version.archive;
    if(!mod_name(file.file_name) || !ends(lower(file.file_name),".jar") || file.hashes.empty())
        throw Error("mods.download","The catalog did not provide a verifiable JAR");
    if(file.manual || file.urls.empty())throw Error("mods.manual_download","The author allows manual download only: "+file.manual_url);
    auto available=storage_.availableBytes();constexpr std::uint64_t margin=32*1024*1024;
    if(available<=margin || file.bytes>available-margin)throw Error("disk_space","Not enough space on the SD card");
    file.max_bytes=std::min({file.max_bytes,max_mod_bytes,available-margin});
    staged.stage=joinPath(storage_.root(),"transactions/"+newId());createDirectories(staged.stage);
    staged.path=joinPath(staged.stage,"download.jar");
    http_.download(file,staged.path,progress,stop);cancelled(stop);
    verifyFile(staged.path,file.hashes,file.max_bytes,stop);
    staged.mod=inspect(staged.path,file.file_name,kind);
    staged.bytes=file.bytes;
    if(file.bytes && staged.mod.bytes!=file.bytes)throw Error("mods.size","The JAR size does not match the catalog");
}
void ModManager::install(const Instance& instance,const CatalogVersion& version,ProgressCallback progress,const std::atomic_bool* stop) {
    auto root=instance_root(storage_,instance);
    Staged staged;
    download_mod(storage_,http_,instance,version,progress,stop,staged);
    std::lock_guard<std::mutex> lock(mods_mutex);
    auto folder=joinPath(root,"game/mods");createDirectories(folder);
    check_conflicts(list_at(folder),staged.mod);cancelled(stop);
    publish(staged.path,joinPath(folder,staged.mod.filename));
    if(progress) {try {progress({"installed",staged.mod.name,staged.bytes,staged.bytes,1,1});}catch(...) {}}
}
void ModManager::remove(const Instance& instance,const std::string& filename) {
    std::lock_guard<std::mutex> lock(mods_mutex);
    if(!mod_name(filename))throw Error("mods.filename","Invalid mod file name");
    removeContent(storage_,instance,ContentKind::Mod,filename);
}
void ModManager::update(const Instance& instance,const std::string& filename,const CatalogVersion& newer,
                        ProgressCallback progress,const std::atomic_bool* stop) {
    if(!mod_name(filename))throw Error("mods.filename","Invalid mod file name");
    auto root=instance_root(storage_,instance);
    Staged staged;
    download_mod(storage_,http_,instance,newer,progress,stop,staged);
    std::lock_guard<std::mutex> lock(mods_mutex);
    auto folder=joinPath(root,"game/mods");
    auto mods=list_at(folder);
    auto found=std::find_if(mods.begin(),mods.end(),[&](const auto& mod){return mod.filename==filename;});
    if(found==mods.end())throw Error("mods.missing","The mod no longer exists");
    staged.mod.enabled=found->enabled;
    if(!staged.mod.enabled)staged.mod.filename+=".disabled";
    // Park the old file inside the private stage so a failed publish can restore it.
    auto parked=joinPath(staged.stage,"previous.jar");
    publish(joinPath(folder,filename),parked);
    try {
        auto remaining=list_at(folder);
        check_conflicts(remaining,staged.mod);
        publish(staged.path,joinPath(folder,staged.mod.filename));
    } catch(...) {
        try {publish(parked,joinPath(folder,filename));}catch(...) {}
        throw;
    }
    if(progress) {try {progress({"installed",staged.mod.name,staged.bytes,staged.bytes,1,1});}catch(...) {}}
}
std::vector<std::string> ModManager::installWithDependencies(const Instance& instance,CatalogClient& catalog,
                                                             const CatalogVersion& version,ProgressCallback progress,
                                                             const std::atomic_bool* stop) {
    std::vector<std::string> installed;
    install(instance,version,progress,stop);
    installed.push_back(version.name);
    if(version.dependencies.empty())return installed;
    // Recognize what is already present so a dependency is not installed twice.
    std::set<std::string> present{version.project_id};
    {
        std::map<std::string,LocalFile> files;
        auto folder=joinPath(instance_root(storage_,instance),"game/mods");
        for(const auto& mod:list(instance)) {
            if(progress) {try {progress({"scan",mod.name,0,0,static_cast<std::uint32_t>(files.size()),0});}catch(...) {}}
            try {files[mod.filename]=digestFile(joinPath(folder,mod.filename),stop);}
            catch(const Error& error) {if(error.code=="cancelled")throw;}
        }
        for(const auto& item:catalog.identify(files,ContentKind::Mod,stop))present.insert(item.second.project_id);
    }
    std::vector<std::pair<Dependency,int>> queue;
    for(const auto& dependency:version.dependencies)queue.push_back({dependency,1});
    while(!queue.empty() && installed.size()<64) {
        cancelled(stop);
        auto [dependency,depth]=queue.front();queue.erase(queue.begin());
        if(dependency.project_id.empty() || present.count(dependency.project_id) || depth>8)continue;
        present.insert(dependency.project_id);
        auto candidates=catalog.versions(version.provider,dependency.project_id,instance.game.minecraft_version,stop,
                                         ContentKind::Mod,instance.game.loader.kind);
        auto chosen=candidates.begin();
        if(!dependency.version_id.empty()) {
            auto exact=std::find_if(candidates.begin(),candidates.end(),[&](const auto& v){return v.id==dependency.version_id;});
            if(exact!=candidates.end())chosen=exact;
        }
        if(chosen==candidates.end())
            throw Error("mods.dependency","No compatible version of a required dependency ("+dependency.project_id+")");
        auto exact=catalog.version(chosen->provider,chosen->project_id,chosen->id,stop,ContentKind::Mod);
        try {
            install(instance,exact,progress,stop);
            installed.push_back(exact.name);
        } catch(const Error& error) {
            // The same mod may already be installed from a file the catalogs do not know.
            if(error.code!="mods.duplicate" && error.code!="mods.exists")throw;
        }
        for(const auto& next:exact.dependencies)queue.push_back({next,depth+1});
    }
    return installed;
}
}
