#ifndef MINESWITCH_LAUNCH_CONFIG_H
#define MINESWITCH_LAUNCH_CONFIG_H
#include <stdbool.h>
#include <stddef.h>
#include <jni.h>
typedef struct { JavaVMOption* values; size_t count; } LauncherOptions;
bool launcher_read_options(const char* path,LauncherOptions* result);
void launcher_free_options(LauncherOptions* options);
#endif
