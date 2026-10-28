#include "horizon_file_io.h"
#include <switch.h>
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static FILE *logfile;
static int failures;
#define CHECK(label, expr) do { int ok=!!(expr); if(!ok)failures++; fprintf(logfile,"WORLD_IO %s %s errno=%d\n",label,ok?"PASS":"FAIL",errno);fflush(logfile); } while(0)
int main(void) {
    consoleInit(NULL);
    Result rc=socketInitializeDefault();
    int net=R_SUCCEEDED(rc)?nxlinkConnectToHost(false,false):-1;
    logfile=net>=0?fdopen(net,"w"):stdout;
    char root[256];
    snprintf(root,sizeof(root),"sdmc:/switch/world-io-%016" PRIx64,armGetSystemTick());
    CHECK("root",mkdir(root,0700)==0);
    const char *names[]={"Новый мир","New World","Новый мир (1)","Another World"};
    for(unsigned i=0;i<sizeof(names)/sizeof(names[0]);i++) {
        char dir[320],path[350];
        snprintf(dir,sizeof(dir),"%s/%s",root,names[i]);
        snprintf(path,sizeof(path),"%s/session.lock",dir);
        fprintf(logfile,"WORLD_IO name=%s\n",names[i]);
        CHECK("mkdir",mkdir(dir,0700)==0);
        DIR *d=opendir(root);struct dirent *e;
        while(d && (e=readdir(d)))fprintf(logfile,"ENTRY [%s] len=%zu\n",e->d_name,strlen(e->d_name));
        if(d)closedir(d);
        FsFileSystem *fs; char raw[FS_MAX_PATH]; FsDir native;
        if(fsdevTranslatePath(root,&fs,raw)==0 && R_SUCCEEDED(fsFsOpenDirectory(fs,raw,FsDirOpenMode_ReadDirs|FsDirOpenMode_ReadFiles,&native))) {
            FsDirectoryEntry ent; s64 count=0;
            Result rd;
            while(R_SUCCEEDED(rd=fsDirRead(&native,&count,1,&ent)) && count)fprintf(logfile,"RAW_ENTRY [%s] len=%zu\n",ent.name,strlen(ent.name));
            fprintf(logfile,"RAW_END rc=%x count=%ld\n",rd,(long)count);
            fsDirClose(&native);
        }
        char *key=nxio_canonical_sd_path(dir);
        fprintf(logfile,"CANON [%s] errno=%d requested=[%s]\n",key?key:"NULL",errno,dir);
        CHECK("canonical_directory",key && strcmp(key,dir)==0);free(key);
        int fd=nxio_open(path,O_CREAT|O_WRONLY,0600);
        CHECK("create_session",fd>=0);
        if(fd>=0) {
            CHECK("write_snowman",nxio_write(fd,"\xe2\x98\x83",3)==3);
            CHECK("force",nxio_fsync(fd)==0);
            CHECK("lock",nxio_lock_whole_file(fd)==0);
            CHECK("unlock",nxio_unlock_whole_file(fd)==0);
            CHECK("close",nxio_close(fd)==0);
            fd=nxio_open(path,O_CREAT|O_WRONLY,0600);
            CHECK("reopen",fd>=0);
            if(fd>=0) CHECK("reclose",nxio_close(fd)==0);
        }
        if(access(path,F_OK)==0) CHECK("remove_file",unlink(path)==0);
    }
    for(unsigned i=0;i<sizeof(names)/sizeof(names[0]);i++) {
        char dir[320];snprintf(dir,sizeof(dir),"%s/%s",root,names[i]);
        CHECK("remove_dir",rmdir(dir)==0);
    }
    CHECK("cleanup",rmdir(root)==0);
    fprintf(logfile,"WORLD_IO_DONE failures=%d\n",failures);fflush(logfile);
    if(logfile!=stdout)fclose(logfile);
    if(R_SUCCEEDED(rc))socketExit();
    consoleExit(NULL);
    return failures?1:0;
}
