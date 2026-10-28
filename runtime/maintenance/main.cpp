#include <switch.h>
#include <jansson.h>
#include <mbedtls/md.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <cerrno>
#include <cstring>
#include <cstdio>
#include <string>
#include <vector>
#include <stdexcept>
#include "owned_nros.h"

static FILE* log_file;
static bool quitting;
static void report(const std::string& s){printf("%s\n",s.c_str());if(log_file){fprintf(log_file,"%s\n",s.c_str());fflush(log_file);}}
static bool read_exact(int fd,void* data,size_t n){auto p=(char*)data;while(n){ssize_t got=recv(fd,p,n,0);if(got<=0)return false;p+=got;n-=got;}return true;}
static bool write_exact(int fd,const void* data,size_t n){auto p=(const char*)data;while(n){ssize_t got=send(fd,p,n,0);if(got<=0)return false;p+=got;n-=got;}return true;}
static void send_json(int fd,json_t* value){char* text=json_dumps(value,JSON_COMPACT);if(!text)throw std::runtime_error("JSON allocation failed");uint32_t n=htonl(strlen(text));bool ok=write_exact(fd,&n,4)&&write_exact(fd,text,strlen(text));free(text);json_decref(value);if(!ok)throw std::runtime_error("Connection closed");}
static std::string field(json_t* root,const char* name){auto p=json_object_get(root,name);if(!json_is_string(p)||json_string_length(p)>1024)throw std::runtime_error("Invalid request field");return std::string(json_string_value(p),json_string_length(p));}
static std::string basename(const std::string& p){return p.substr(p.find_last_of('/')+1);}
static bool valid_path(const std::string& path){return path.rfind("sdmc:/switch/",0)==0 && path.size()<500 && path.find("..") == std::string::npos && path.find('\\')==std::string::npos && path.find('\0')==std::string::npos;}
static bool hex_digest(const std::string& s){return s.size()==64 && s.find_first_not_of("0123456789abcdef")==std::string::npos;}
static bool candidate(const std::string& name){if(name==MAINTENANCE_FILENAME)return true;for(const auto& item:owned_nros)if(name==item.name)return true;return false;}
static bool owned(const std::string& name,const std::string& hash){if(name==MAINTENANCE_FILENAME)return true;for(const auto& item:owned_nros)if(name==item.name&&hash==item.sha256)return true;return false;}
static std::string finish_hash(mbedtls_md_context_t* context){unsigned char digest[32];if(mbedtls_md_finish(context,digest))throw std::runtime_error("Hash failed");char result[65];for(unsigned i=0;i<32;i++)snprintf(result+2*i,3,"%02x",digest[i]);return result;}
struct Hash {mbedtls_md_context_t value;Hash(){mbedtls_md_init(&value);if(mbedtls_md_setup(&value,mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),0)||mbedtls_md_starts(&value))throw std::runtime_error("Hash initialization failed");}~Hash(){mbedtls_md_free(&value);}};
static std::string hash_file(const std::string& path){
    struct stat info{};if(lstat(path.c_str(),&info)||!S_ISREG(info.st_mode)||info.st_size<0||info.st_size>2LL*1024*1024*1024)throw std::runtime_error("Invalid NRO file");
    FILE* f=fopen(path.c_str(),"rb");if(!f)throw std::runtime_error("Cannot read NRO");
    Hash hash;char buffer[64*1024];size_t n;
    while((n=fread(buffer,1,sizeof(buffer),f)))if(mbedtls_md_update(&hash.value,(unsigned char*)buffer,n)){fclose(f);throw std::runtime_error("Hash update failed");}
    bool failed=ferror(f);fclose(f);if(failed)throw std::runtime_error("NRO read failed");return finish_hash(&hash.value);
}
static void scan(const std::string& directory,unsigned depth,json_t* entries,unsigned& visited){
    if(depth>6 || visited>12000)return;
    DIR* d=opendir(directory.c_str());if(!d)return;
    while(auto entry=readdir(d)) {
        std::string name=entry->d_name;if(name=="."||name=="..")continue;
        if(++visited>12000)break;
        std::string path=directory+"/"+name;struct stat info{};if(lstat(path.c_str(),&info))continue;
        if(S_ISDIR(info.st_mode)) {
            if(directory=="sdmc:/switch/mineswitch" && (name=="runtime"||name=="instances"||name=="cache"||name=="minecraft"))continue;
            scan(path,depth+1,entries,visited);
        } else if(S_ISREG(info.st_mode) && candidate(name) && valid_path(path) && path!="sdmc:/switch/mineswitch/MineSwitch.nro") {
            try {
                report("INVENTORY "+path);auto hash=hash_file(path);
                json_array_append_new(entries,json_pack("{s:s,s:s,s:I,s:b}","path",path.c_str(),"sha256",hash.c_str(),"bytes",(json_int_t)info.st_size,"owned",owned(name,hash)));
            }catch(const std::exception& e){report("INVENTORY_SKIP "+path+" "+e.what());
                json_array_append_new(entries,json_pack("{s:s,s:b,s:s}","path",path.c_str(),"owned",0,"error",e.what()));}
        }
    }
    closedir(d);
}
static void put(int fd,json_t* request){
    auto kind=field(request,"kind"),expected=field(request,"sha256");
    const char* destination=kind=="launcher"?"sdmc:/switch/mineswitch/MineSwitch.nro":
        kind=="nsp"?"sdmc:/switch/mineswitch/install/MineSwitch.nsp":
        kind=="receipt"?"sdmc:/switch/mineswitch/install/build.json":nullptr;
    // Pack archives for the launcher's import browser; plain file names only.
    std::string import_path;
    if(kind=="import"){
        auto name=field(request,"path");
        bool ok=!name.empty()&&name.size()<=96&&name[0]!='.';
        for(char c:name)ok=ok&&(isalnum((unsigned char)c)||c=='.'||c=='-'||c=='_');
        auto ends=[&](const char* x){size_t k=strlen(x);return name.size()>k&&name.compare(name.size()-k,k,x)==0;};
        if(!ok||!(ends(".zip")||ends(".mrpack")))throw std::runtime_error("Invalid import file name");
        import_path="sdmc:/switch/mineswitch/imports/"+name;destination=import_path.c_str();
        mkdir("sdmc:/switch",0777);mkdir("sdmc:/switch/mineswitch",0777);mkdir("sdmc:/switch/mineswitch/imports",0777);
    }
    auto n=json_object_get(request,"bytes");
    if(!destination || !hex_digest(expected) || !json_is_integer(n) || json_integer_value(n)<=0 ||
       json_integer_value(n)>128*1024*1024 || (kind=="receipt"&&json_integer_value(n)>65536))throw std::runtime_error("Invalid deployment request");
    std::string path=destination,temp=path+"."+MAINTENANCE_SUFFIX+".new",backup=path+"."+MAINTENANCE_SUFFIX+".previous";
    mkdir("sdmc:/switch",0777);mkdir("sdmc:/switch/mineswitch",0777);mkdir("sdmc:/switch/mineswitch/install",0777);
    int output=open(temp.c_str(),O_WRONLY|O_CREAT|O_EXCL,0600);if(output<0)throw std::runtime_error("Cannot create deployment staging file");
    bool closed=false;
    try {
        send_json(fd,json_pack("{s:b}","ready",1));Hash hash;char buffer[64*1024];size_t remaining=(size_t)json_integer_value(n);
        while(remaining){size_t chunk=remaining<sizeof(buffer)?remaining:sizeof(buffer);if(!read_exact(fd,buffer,chunk))throw std::runtime_error("Transfer interrupted");
            size_t done=0;while(done<chunk){ssize_t wrote=write(output,buffer+done,chunk-done);if(wrote<=0)throw std::runtime_error("SD write failed");done+=wrote;}
            if(mbedtls_md_update(&hash.value,(unsigned char*)buffer,chunk))throw std::runtime_error("Transfer hash failed");
            remaining-=chunk;
        }
        if(fsync(output)!=0)throw std::runtime_error("SD sync failed");
        close(output);closed=true;
        auto actual=finish_hash(&hash.value);if(actual!=expected)throw std::runtime_error("Transfer checksum mismatch");
        struct stat info{};bool existed=lstat(path.c_str(),&info)==0;
        // Keep exactly one rollback copy: Horizon's rename does not replace an old one.
        struct stat old{};if(existed && lstat(backup.c_str(),&old)==0 && S_ISREG(old.st_mode))unlink(backup.c_str());
        if(existed && (!S_ISREG(info.st_mode)||rename(path.c_str(),backup.c_str())))throw std::runtime_error("Cannot preserve previous deployment");
        if(rename(temp.c_str(),path.c_str())){if(existed)rename(backup.c_str(),path.c_str());throw std::runtime_error("Cannot publish deployment");}
        report("DEPLOYED "+path+" sha256="+actual);
        send_json(fd,json_pack("{s:b,s:s,s:s,s:b}","ok",1,"path",path.c_str(),"sha256",actual.c_str(),"previous_preserved",existed));
    }catch(...){if(!closed)close(output);unlink(temp.c_str());throw;}
}
static void handle(int fd){
    uint32_t size;if(!read_exact(fd,&size,4))return;size=ntohl(size);if(!size||size>65536)throw std::runtime_error("Invalid request size");
    std::string data(size,'\0');if(!read_exact(fd,data.data(),size))return;
    json_error_t error{};json_t* request=json_loadb(data.data(),data.size(),JSON_REJECT_DUPLICATES,&error);
    if(!request)throw std::runtime_error("Invalid request JSON");
    struct Close {json_t* p;~Close(){json_decref(p);}} close{request};
    if(field(request,"token")!=MAINTENANCE_TOKEN)throw std::runtime_error("Authentication failed");
    auto op=field(request,"op");
    if(op=="status"){send_json(fd,json_pack("{s:b,s:s}","ok",1,"service","MineSwitch maintenance"));return;}
    if(op=="put"){put(fd,request);return;}
    if(op=="inventory"){
        auto entries=json_array();unsigned visited=0;scan("sdmc:/switch",0,entries,visited);
        auto response=json_pack("{s:b,s:b}","ok",1,"truncated",visited>12000);json_object_set_new(response,"entries",entries);send_json(fd,response);return;
    }
    if(op=="delete"){
        auto path=field(request,"path"),expected=field(request,"sha256");
        if(!valid_path(path)||!hex_digest(expected)||!candidate(basename(path))||!owned(basename(path),expected)||path=="sdmc:/switch/mineswitch/MineSwitch.nro")throw std::runtime_error("Deletion is outside owned test applications");
        if(hash_file(path)!=expected)throw std::runtime_error("NRO changed; preserving file");
        if(unlink(path.c_str()))throw std::runtime_error("Cannot remove NRO: "+std::string(strerror(errno)));
        report("REMOVED "+path);send_json(fd,json_pack("{s:b}","ok",1));return;
    }
    if(op=="title_check"){
        Result rc=nsInitialize();bool found=false;
        if(R_SUCCEEDED(rc)){
            NsApplicationRecord records[64];s32 count=0;int offset=0;
            do{rc=nsListApplicationRecord(records,64,offset,&count);if(R_FAILED(rc))break;
                for(int i=0;i<count;i++)if(records[i].application_id==UINT64_C(0x01004D5357490000))found=true;
                offset+=count;
            }while(count==64 && offset<4096);
            nsExit();
        }
        send_json(fd,json_pack("{s:b,s:I,s:b}","ok",R_SUCCEEDED(rc),"result",(json_int_t)rc,"collision",found));return;
    }
    if(op=="exit"){send_json(fd,json_pack("{s:b}","ok",1));quitting=true;return;}
    throw std::runtime_error("Unknown operation");
}
int main(){
    consoleInit(nullptr);Result rc=socketInitializeDefault();if(R_FAILED(rc))return 1;
    int logger=nxlinkConnectToHost(false,false);if(logger>=0)log_file=fdopen(logger,"w");
    int server=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);int reuse=1;setsockopt(server,SOL_SOCKET,SO_REUSEADDR,&reuse,sizeof(reuse));
    sockaddr_in address{};address.sin_family=AF_INET;address.sin_port=htons(31339);address.sin_addr.s_addr=INADDR_ANY;
    if(server<0||bind(server,(sockaddr*)&address,sizeof(address))||listen(server,2)){report("MAINTENANCE_LISTEN_FAILED");return 2;}
    appletSetAutoSleepDisabled(true);report("MAINTENANCE_READY port=31339");
    u64 started=armGetSystemTick();
    while(!quitting && appletMainLoop() && armTicksToNs(armGetSystemTick()-started)<UINT64_C(3600000000000)){
        consoleUpdate(nullptr);fd_set set;FD_ZERO(&set);FD_SET(server,&set);timeval wait{0,250000};
        if(select(server+1,&set,nullptr,nullptr,&wait)<=0)continue;
        sockaddr_in peer{};socklen_t len=sizeof(peer);int client=accept(server,(sockaddr*)&peer,&len);if(client<0)continue;
        if(peer.sin_addr.s_addr!=inet_addr("192.168.3.163")){close(client);continue;}
        timeval timeout{30,0};setsockopt(client,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));setsockopt(client,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout));
        try{handle(client);}catch(const std::exception& e){report(std::string("MAINTENANCE_ERROR ")+e.what());try{send_json(client,json_pack("{s:b,s:s}","ok",0,"error",e.what()));}catch(...) {}}
        close(client);
    }
    close(server);appletSetAutoSleepDisabled(false);report("MAINTENANCE_DONE");
    if(log_file){fclose(log_file);log_file=nullptr;}socketExit();consoleExit(nullptr);return 0;
}
