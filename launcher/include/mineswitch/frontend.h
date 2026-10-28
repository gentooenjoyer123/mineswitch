#ifndef MINESWITCH_FRONTEND_H
#define MINESWITCH_FRONTEND_H
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    char *main_class;
    char *options_file;
    char *working_directory;
    char *preparation_summary;
    int preparation_only;
    int argc;
    char **argv;
} MineSwitchLaunch;
/* All UI, input, download worker and renderer resources are released on return.
 * 1: launch JVM exactly once. 0: owner exits. -1: frontend initialization failed. */
/* A nonzero heap override is validated and saved once to SD settings. */
int mineswitch_frontend(const char *storage_root, MineSwitchLaunch *launch, int heap_override_mib);
const char *mineswitch_frontend_error(void);
void mineswitch_free_launch(MineSwitchLaunch *launch);
#ifdef __SWITCH__
/* Bounded diagnostic line sent to the existing launcher log connection. */
void mineswitch_frontend_diagnostic(const char *line);
#endif
#ifdef __cplusplus
}
#endif
#endif
