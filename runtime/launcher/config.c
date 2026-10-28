#include "config.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define MAX_OPTION_BYTES (64u*1024u)
#define MAX_CONFIG_BYTES (256u*1024u)
static bool append(LauncherOptions* result,char* line,size_t size) {
    if (size && line[size-1]=='\r') --size;
    if (!size || line[0]=='#') return true;
    char* copy=malloc(size+1);
    if (!copy) return false;
    memcpy(copy,line,size); copy[size]=0;
    JavaVMOption* values=realloc(result->values,(result->count+1)*sizeof(*values));
    if (!values) { free(copy); return false; }
    result->values=values;
    values[result->count++]=(JavaVMOption){.optionString=copy,.extraInfo=NULL};
    return true;
}
void launcher_free_options(LauncherOptions* options) {
    for (size_t i=0;i<options->count;i++) free(options->values[i].optionString);
    free(options->values);
    *options=(LauncherOptions){0};
}
bool launcher_read_options(const char* path,LauncherOptions* result) {
    *result=(LauncherOptions){0};
    FILE* input=fopen(path,"rb");
    if (!input) { fprintf(stderr,"JVM_OPTIONS open failed: %s\n",strerror(errno)); return false; }
    char* line=malloc(MAX_OPTION_BYTES+1);
    if (!line) { fclose(input); return false; }
    size_t used=0,total=0,number=1;
    bool ok=true;
    int ch;
    while ((ch=fgetc(input))!=EOF) {
        if (++total>MAX_CONFIG_BYTES || ch==0) { ok=false; break; }
        if (ch=='\n') {
            if (!append(result,line,used)) { ok=false; break; }
            used=0; ++number;
        } else {
            if (used==MAX_OPTION_BYTES) { ok=false; break; }
            line[used++]=(char)ch;
        }
    }
    if (ferror(input)) ok=false;
    if (ok && used && !append(result,line,used)) ok=false;
    free(line); fclose(input);
    if (!ok) {
        fprintf(stderr,"JVM_OPTIONS invalid or unavailable near line=%zu bytes=%zu\n",number,total);
        launcher_free_options(result);
    } else printf("JVM_OPTIONS loaded count=%zu bytes=%zu\n",result->count,total);
    return ok;
}
