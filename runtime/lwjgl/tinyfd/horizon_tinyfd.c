/* MineSwitch Horizon compatibility backend for TinyFileDialogs' unavailable
 * dialog path. This is not the upstream native dialog implementation. No UI is
 * presented: query reports no_solution and operations return cancellation/failure.
 */
#include "tinyfiledialogs.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>

char tinyfd_version[8]="0.1.0";
char tinyfd_needs[]="MineSwitch Horizon adapter: native dialogs are unavailable";
char tinyfd_response[1024]="no_solution";
int tinyfd_verbose=0,tinyfd_silent=0,tinyfd_allowCursesDialogs=0;
int tinyfd_forceConsole=0,tinyfd_assumeGraphicDisplay=0;

static void unavailable(const char* operation,const char* title,const char* message) {
    errno=ENOSYS;
    if (title && !strcmp(title,"tinyfd_query")) return;
    if (!tinyfd_silent) {
        fprintf(stderr,"[TinyFD] Horizon %s unavailable",operation);
        if (title && *title) fprintf(stderr,": %s",title);
        if (message && *message) fprintf(stderr,"\n%s",message);
        fputc('\n',stderr);
    }
}
const char* tinyfd_getGlobalChar(const char* name) {
    if (!name) return NULL;
    if (!strcmp(name,"tinyfd_version")) return tinyfd_version;
    if (!strcmp(name,"tinyfd_needs")) return tinyfd_needs;
    if (!strcmp(name,"tinyfd_response")) return tinyfd_response;
    return NULL;
}
int tinyfd_getGlobalInt(const char* name) {
    if (!name) return -1;
    if (!strcmp(name,"tinyfd_verbose")) return tinyfd_verbose;
    if (!strcmp(name,"tinyfd_silent")) return tinyfd_silent;
    if (!strcmp(name,"tinyfd_allowCursesDialogs") || !strcmp(name,"tinyfd_forceConsole") ||
        !strcmp(name,"tinyfd_assumeGraphicDisplay")) return 0;
    return -1;
}
int tinyfd_setGlobalInt(const char* name,int value) {
    if (!name || (value!=0 && value!=1)) return -1;
    if (!strcmp(name,"tinyfd_verbose")) { tinyfd_verbose=value; return value; }
    if (!strcmp(name,"tinyfd_silent")) { tinyfd_silent=value; return value; }
    errno=ENOSYS; return -1;
}
void tinyfd_beep(void) { unavailable("beep",NULL,NULL); }
int tinyfd_notifyPopup(const char* title,const char* message,const char* icon) {
    (void)icon; unavailable("notification",title,message); return 0;
}
int tinyfd_messageBox(const char* title,const char* message,const char* type,const char* icon,int button) {
    (void)type; (void)icon; (void)button;
    unavailable("message box",title,message); return 0;
}
char* tinyfd_inputBox(const char* title,const char* message,const char* initial) {
    (void)initial; unavailable("input box",title,message); return NULL;
}
char* tinyfd_saveFileDialog(const char* title,const char* path,int count,const char* const* patterns,const char* description) {
    (void)path; (void)count; (void)patterns; (void)description;
    unavailable("save dialog",title,NULL); return NULL;
}
char* tinyfd_openFileDialog(const char* title,const char* path,int count,const char* const* patterns,const char* description,int multiple) {
    (void)path; (void)count; (void)patterns; (void)description; (void)multiple;
    unavailable("open dialog",title,NULL); return NULL;
}
char* tinyfd_selectFolderDialog(const char* title,const char* path) {
    (void)path; unavailable("folder dialog",title,NULL); return NULL;
}
char* tinyfd_colorChooser(const char* title,const char* hex,const unsigned char initial[3],unsigned char result[3]) {
    (void)hex; (void)initial; (void)result;
    unavailable("color chooser",title,NULL); return NULL;
}
