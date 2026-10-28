#ifndef MINESWITCH_LOG_TEE_H
#define MINESWITCH_LOG_TEE_H
#include <stdbool.h>
#include <stdint.h>
void launcher_log_init(int network_fd);
void launcher_log_pump(void);
void launcher_console_update(void);
void launcher_console_release(void);
/* Main-thread startup fallback, before the JVM worker is created. */
void launcher_console_restore(void);
void launcher_log_drain(void);
bool launcher_native_fault_pending(void);
void horizon_native_exit_flush(void);
/* Exception-safe, bounded publication; leaves the fault unhandled afterward. */
void horizon_native_fault_report(uint64_t pc, uint64_t far, uint64_t sp,
    uint64_t lr, uint64_t fp, uint64_t thread_id, uint32_t esr, uint32_t description,
    const uint64_t* registers, int java_state);
#endif
