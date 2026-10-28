#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* SDK main thread only, after releasing the previous UI/console window.
 * begin returns 1 on success or 0 when the caller should retain its fallback.
 * Stages: 1 runtime, 2 Java VM, 3 entry class, 4 game/mod initialization.
 * Other stage values retain the previous phase. Input/Plus belongs to caller.
 * end is idempotent and must finish before GLFW creates its native window.
 */
int mineswitch_loading_begin(const char *title);
void mineswitch_loading_frame(int stage);
void mineswitch_loading_end(void);

#ifdef __cplusplus
}
#endif
