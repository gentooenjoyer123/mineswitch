// Model the observed libnx non-replacing rename contract and inject a commit
// failure. All hashing/copy/recovery logic remains the actual install.c code.
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
int __real_rename(const char*,const char*);
int __wrap_rename(const char* from,const char* to) {
    struct stat info;
    if (stat(to,&info)==0) { errno=EEXIST; return -1; }
    const char* suffix=".mineswitch-part";
    size_t n=strlen(from),s=strlen(suffix);
    if (getenv("MINESWITCH_INSTALL_TEST_REPLACE_FAIL") && n>=s && !strcmp(from+n-s,suffix)) {
        errno=EIO; return -1;
    }
    return __real_rename(from,to);
}
