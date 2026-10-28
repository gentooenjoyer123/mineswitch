#include "install.h"
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <mbedtls/sha256.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef RUNTIME_SOURCE
#define RUNTIME_SOURCE "romfs:/jdk/"
#endif
#ifndef RUNTIME_TARGET
#define RUNTIME_TARGET "sdmc:/switch/mineswitch/runtime/"
#endif
#ifndef INSTALL_LOG_EVERY
#define INSTALL_LOG_EVERY 1
#endif
#define COPY_SIZE (64u*1024u)

static bool safe_relative(const char* path) {
    if (!*path || *path=='/' || strchr(path,':') || strchr(path,'\\')) return false;
    size_t length=strlen(path);
    const char* reserved[]={".mineswitch-part",".mineswitch-old"};
    for (unsigned i=0;i<2;i++) {
        size_t n=strlen(reserved[i]);
        if (length>=n && !strcmp(path+length-n,reserved[i])) return false;
    }
    for (const char* p=path;*p;) {
        const char* end=strchr(p,'/'); if (!end) end=p+strlen(p);
        size_t n=(size_t)(end-p);
        if (!n || (n==1 && p[0]=='.') || (n==2 && p[0]=='.' && p[1]=='.')) return false;
        for (const char* q=p;q<end;q++) if ((unsigned char)*q<32) return false;
        if (!*end) break;
        p=end+1; if (!*p) return false;
    }
    return true;
}
static bool ensure_directory(const char* path) {
    if (mkdir(path,0777)==0) return true;
    struct stat st;
    return errno==EEXIST && stat(path,&st)==0 && S_ISDIR(st.st_mode);
}
static bool parents(char* path) {
    char* start=strchr(path,':'); start=start?start+2:path+1;
    for (char* p=start;*p;p++) if (*p=='/') {
        *p=0; bool ok=ensure_directory(path); *p='/';
        if (!ok) return false;
    }
    return true;
}
static bool parse_hash(const char* text,unsigned char digest[32]) {
    for (unsigned i=0;i<32;i++) {
        int a=text[2*i],b=text[2*i+1];
        a=(a>='0'&&a<='9')?a-'0':(a>='a'&&a<='f')?a-'a'+10:-1;
        b=(b>='0'&&b<='9')?b-'0':(b>='a'&&b<='f')?b-'a'+10:-1;
        if (a<0 || b<0) return false;
        digest[i]=(unsigned char)((a<<4)|b);
    }
    return true;
}
static bool digest_file(const char* path,uint64_t expected_size,const unsigned char expected[32],unsigned char* buffer) {
    struct stat st;
    if (stat(path,&st) || !S_ISREG(st.st_mode) || (uint64_t)st.st_size!=expected_size) return false;
    FILE* file=fopen(path,"rb"); if (!file) return false;
    mbedtls_sha256_context hash; unsigned char digest[32]; uint64_t total=0;
    mbedtls_sha256_init(&hash);
    int result=mbedtls_sha256_starts_ret(&hash,0);
    size_t n;
    while (!result && (n=fread(buffer,1,COPY_SIZE,file))!=0) {
        total+=n; result=mbedtls_sha256_update_ret(&hash,buffer,n);
    }
    bool ok=!result && !ferror(file) && total==expected_size &&
        mbedtls_sha256_finish_ret(&hash,digest)==0 && !memcmp(digest,expected,32);
    fclose(file); mbedtls_sha256_free(&hash); return ok;
}
static bool recover_previous(const char* target) {
    char backup[PATH_MAX];
    if (snprintf(backup,sizeof(backup),"%s.mineswitch-old",target)>=(int)sizeof(backup)) return false;
    struct stat st;
    if (stat(target,&st)==0) return S_ISREG(st.st_mode);
    if (errno!=ENOENT) return false;
    if (stat(backup,&st)!=0) return errno==ENOENT;
    if (!S_ISREG(st.st_mode)) { errno=EINVAL; return false; }
    printf("INSTALL recover previous path=%s\n",target);
    return rename(backup,target)==0;
}
static bool commit_verified(const char* temporary,const char* target) {
    char backup[PATH_MAX];
    if (snprintf(backup,sizeof(backup),"%s.mineswitch-old",target)>=(int)sizeof(backup)) return false;
    if (rename(temporary,target)==0) {
        unlink(backup);
        return true;
    }
    // libnx fsdev's rename does not replace an existing destination. The new
    // file is already hashed and flushed. Retain the old file as a backup
    // until the rename of the verified replacement has succeeded.
    if (errno!=EEXIST && errno!=ENOTEMPTY) return false;
    struct stat st;
    if (stat(target,&st)!=0 || !S_ISREG(st.st_mode)) return false;
    if (unlink(backup)!=0 && errno!=ENOENT) return false;
    if (rename(target,backup)!=0) return false;
    if (rename(temporary,target)!=0) {
        int failure=errno;
        if (rename(backup,target)!=0)
            fprintf(stderr,"INSTALL rollback deferred path=%s; backup retained\n",target);
        errno=failure;
        return false;
    }
    unlink(backup);
    return true;
}
static bool copy_verified(const char* source,char* target,uint64_t expected_size,const unsigned char expected[32],unsigned char* buffer) {
    char temporary[PATH_MAX];
    if (snprintf(temporary,sizeof(temporary),"%s.mineswitch-part",target)>=(int)sizeof(temporary)) return false;
    if (!parents(target)) return false;
    FILE* input=fopen(source,"rb"); if (!input) return false;
    FILE* output=fopen(temporary,"wb");
    if (!output) { fclose(input); return false; }
    mbedtls_sha256_context hash; unsigned char digest[32]; uint64_t total=0;
    mbedtls_sha256_init(&hash);
    bool ok=mbedtls_sha256_starts_ret(&hash,0)==0;
    size_t n;
    while (ok && (n=fread(buffer,1,COPY_SIZE,input))!=0) {
        total+=n;
        ok=total<=expected_size && fwrite(buffer,1,n,output)==n &&
            mbedtls_sha256_update_ret(&hash,buffer,n)==0;
    }
    ok=ok && !ferror(input) && total==expected_size &&
       mbedtls_sha256_finish_ret(&hash,digest)==0 && !memcmp(digest,expected,32);
    if (ok) ok=fflush(output)==0 && fsync(fileno(output))==0;
    if (fclose(output)!=0) ok=false;
    fclose(input); mbedtls_sha256_free(&hash);
    if (ok) ok=commit_verified(temporary,target);
    if (!ok) unlink(temporary);
    return ok;
}
bool launcher_install_runtime(void) {
    FILE* manifest=fopen(RUNTIME_SOURCE "files.manifest","rb");
    if (!manifest) { fprintf(stderr,"INSTALL manifest: %s\n",strerror(errno)); return false; }
    unsigned char* buffer=malloc(COPY_SIZE);
    if (!buffer) { fclose(manifest); return false; }
    char line[PATH_MAX+128]; bool ok=true; unsigned checked=0,copied=0;
    while (fgets(line,sizeof(line),manifest)) {
        size_t length=strlen(line);
        if (!length || line[length-1]!='\n') { ok=false; break; }
        line[--length]=0;
        if (length<67 || line[64]!=' ') { ok=false; break; }
        unsigned char expected[32];
        if (!parse_hash(line,expected)) { ok=false; break; }
        if (line[65]<'0' || line[65]>'9') { ok=false; break; }
        errno=0; char* path=NULL;
        uint64_t size=strtoull(line+65,&path,10);
        if (errno || path==line+65 || *path!=' ') { ok=false; break; }
        ++path;
        if (!safe_relative(path)) { ok=false; break; }
        char source[PATH_MAX],target[PATH_MAX];
        if (snprintf(source,sizeof(source),RUNTIME_SOURCE "%s",path)>=(int)sizeof(source) ||
            snprintf(target,sizeof(target),RUNTIME_TARGET "%s",path)>=(int)sizeof(target)) { ok=false; break; }
        if (!recover_previous(target)) {
            fprintf(stderr,"INSTALL recovery failed path=%s error=%s\n",path,strerror(errno)); ok=false; break;
        }
        if (!digest_file(target,size,expected,buffer)) {
            if (checked % INSTALL_LOG_EVERY == 0)
                printf("INSTALL progress checked=%u path=%s bytes=%" PRIu64 "\n",checked,path,size);
            if (!copy_verified(source,target,size,expected,buffer)) {
                fprintf(stderr,"INSTALL failed path=%s error=%s\n",path,strerror(errno)); ok=false; break;
            }
            ++copied;
        }
        // A valid target makes stale installer scratch files unnecessary.
        char stale[PATH_MAX];
        if (snprintf(stale,sizeof(stale),"%s.mineswitch-old",target)<(int)sizeof(stale)) unlink(stale);
        if (snprintf(stale,sizeof(stale),"%s.mineswitch-part",target)<(int)sizeof(stale)) unlink(stale);
        ++checked;
    }
    if (ferror(manifest) || !checked) ok=false;
    free(buffer); fclose(manifest);
    printf("INSTALL complete=%d checked=%u copied=%u\n",ok,checked,copied);
    return ok;
}
