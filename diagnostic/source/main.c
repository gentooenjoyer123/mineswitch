#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>

#include <switch.h>

extern char *fake_heap_start;
extern char *fake_heap_end;

static FILE *network_log;

static bool dump_instance_log(const char *instance) {
    if(strlen(instance)!=32 || strspn(instance,"0123456789abcdef")!=32 || !network_log)return false;
    char path[192];
    snprintf(path,sizeof(path),"sdmc:/switch/mineswitch/instances/%s/game/logs/latest.log",instance);
    FILE *source=fopen(path,"rb");if(!source)return false;
    if(fseek(source,0,SEEK_END)!=0){fclose(source);return false;}
    long size=ftell(source);
    if(size<0 || fseek(source,size>2*1024*1024?size-2*1024*1024:0,SEEK_SET)!=0){fclose(source);return false;}
    fprintf(network_log,"INSTANCE_LOG_BEGIN %s bytes=%ld\n",path,size);
    char buffer[4096];size_t total=0,n;
    while(total<2*1024*1024 && (n=fread(buffer,1,sizeof(buffer),source))) {
        if(fwrite(buffer,1,n,network_log)!=n)break;
        total+=n;
    }
    bool ok=!ferror(source) && total==(size>2*1024*1024?2*1024*1024:(size_t)size);
    fclose(source);
    fprintf(network_log,"\nINSTANCE_LOG_END bytes=%zu complete=%d\n",total,ok);fflush(network_log);
    return ok;
}

static bool dump_instance_crash(const char *instance) {
    if(strlen(instance)!=32 || strspn(instance,"0123456789abcdef")!=32 || !network_log)return false;
    char directory[160],latest[128]={0},path[320];
    snprintf(directory,sizeof(directory),"sdmc:/switch/mineswitch/instances/%s/game/crash-reports",instance);
    DIR *dir=opendir(directory);if(!dir)return false;
    struct dirent *entry;
    while((entry=readdir(dir))) {
        size_t n=strlen(entry->d_name);
        if(n>=sizeof(latest) || strncmp(entry->d_name,"crash-",6) || n<10 || strcmp(entry->d_name+n-4,".txt"))continue;
        if(strcmp(entry->d_name,latest)>0)snprintf(latest,sizeof(latest),"%s",entry->d_name);
    }
    closedir(dir);if(!latest[0])return false;
    snprintf(path,sizeof(path),"%s/%s",directory,latest);
    FILE *source=fopen(path,"rb");if(!source)return false;
    fprintf(network_log,"INSTANCE_REPORT_BEGIN %s\n",path);
    char buffer[4096];size_t total=0,n;
    while(total<2*1024*1024 && (n=fread(buffer,1,sizeof(buffer),source))) {
        if(fwrite(buffer,1,n,network_log)!=n)break;
        total+=n;
    }
    bool ok=feof(source) && !ferror(source);fclose(source);
    fprintf(network_log,"\nINSTANCE_REPORT_END bytes=%zu complete=%d\n",total,ok);fflush(network_log);
    return ok;
}

/* Read only an explicitly selected JVM report, never a general SD path. */
static bool dump_jvm_error(const char *pid) {
    if (!pid[0] || strlen(pid) > 10 || strspn(pid, "0123456789") != strlen(pid))
        return false;
    char path[96];
    snprintf(path, sizeof(path), "sdmc:/switch/mineswitch/hs_err_pid%s.log", pid);
    FILE *source = fopen(path, "rb");
    if (!source || !network_log) {
        if (source) fclose(source);
        return false;
    }
    fprintf(network_log, "JVM_REPORT_BEGIN %s\n", path);
    char buffer[4096];
    size_t total = 0, count;
    bool ok = true;
    while (total < 2 * 1024 * 1024 && (count = fread(buffer, 1, sizeof(buffer), source))) {
        if (fwrite(buffer, 1, count, network_log) != count) { ok = false; break; }
        total += count;
    }
    ok = ok && !ferror(source) && feof(source);
    fclose(source);
    fprintf(network_log, "\nJVM_REPORT_END bytes=%zu complete=%d\n", total, ok);
    fflush(network_log);
    return ok;
}

static void report(const char *format, ...) {
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    fflush(stdout);

    if (network_log) {
        va_start(args, format);
        vfprintf(network_log, format, args);
        va_end(args);
        fflush(network_log);
    }
}

static void report_result(const char *operation, Result rc) {
    report("%s: %s (0x%08" PRIx32 ")\n", operation,
           R_SUCCEEDED(rc) ? "OK" : "FAILED", (uint32_t)rc);
}

static void report_memory(void) {
    u64 total = 0;
    u64 used = 0;
    Result total_rc = svcGetInfo(&total, InfoType_TotalMemorySize,
                                CUR_PROCESS_HANDLE, 0);
    Result used_rc = svcGetInfo(&used, InfoType_UsedMemorySize,
                               CUR_PROCESS_HANDLE, 0);
    report_result("memory.total.query", total_rc);
    report_result("memory.used.query", used_rc);
    if (R_SUCCEEDED(total_rc)) {
        report("Process memory budget: %.2f MiB (%" PRIu64 " bytes)\n",
               (double)total / (1024.0 * 1024.0), (uint64_t)total);
    }
    if (R_SUCCEEDED(used_rc)) {
        report("Process memory committed: %.2f MiB (%" PRIu64 " bytes)\n",
               (double)used / (1024.0 * 1024.0), (uint64_t)used);
    }

    uintptr_t heap_start = (uintptr_t)fake_heap_start;
    uintptr_t heap_end = (uintptr_t)fake_heap_end;
    if (heap_end >= heap_start) {
        report("Heap reserved by libnx: %.2f MiB (%" PRIuPTR " bytes)\n",
               (double)(heap_end - heap_start) / (1024.0 * 1024.0),
               heap_end - heap_start);
    }
    report("Reserved heap is included in committed memory.\n");
}

static void thread_worker(void *argument) {
    uint64_t *result = argument;
    uint64_t sum = 0;
    for (uint64_t i = 1; i <= 10000; ++i) {
        sum += i;
    }
    *result = sum;
}

static void check_thread(void) {
    Thread thread;
    uint64_t result = 0;
    Result rc = threadCreate(&thread, thread_worker, &result, NULL,
                             0x4000, 0x2c, -2);
    report_result("thread.create", rc);
    if (R_FAILED(rc)) {
        return;
    }
    rc = threadStart(&thread);
    report_result("thread.start", rc);
    if (R_SUCCEEDED(rc)) {
        rc = threadWaitForExit(&thread);
        report_result("thread.join", rc);
        if (R_SUCCEEDED(rc)) {
            report("Thread checksum: %" PRIu64 " (expected 50005000) %s\n",
                   result, result == UINT64_C(50005000) ? "PASS" : "FAIL");
        }
    }
    report_result("thread.close", threadClose(&thread));
}

static void check_jit_allocation(void) {
    Jit jit = {0};
    Result rc = jitCreate(&jit, 4096);
    report_result("jit.allocate.4096", rc);
    if (R_SUCCEEDED(rc)) {
        report("JIT allocation type: %d\n", (int)jit.type);
        report_result("jit.close", jitClose(&jit));
    }
    report("JIT allocation only; no generated code was executed.\n");
}

int main(int argc, char **argv) {
    consoleInit(NULL);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);

    printf("MineSwitch diagnostic 0.1.1\nConnecting network log...\n");
    consoleUpdate(NULL);

    Result socket_rc = socketInitializeDefault();
    int network_error = 0;
    if (R_SUCCEEDED(socket_rc)) {
        int network_fd = nxlinkConnectToHost(false, false);
        if (network_fd >= 0) {
            network_log = fdopen(network_fd, "w");
            if (!network_log) {
                network_error = errno;
                close(network_fd);
            }
        } else {
            network_error = errno;
        }
    }

    report("MINESWITCH_DIAG_BEGIN version=0.1.1\n");
    if(argc==2 && strncmp(argv[1],"--instance-log=",15)==0) {
        bool ok=dump_instance_log(argv[1]+15);
        report("Instance log read: %s\n",ok?"OK":"FAILED");
        if(network_log){fclose(network_log);network_log=NULL;}
        if(R_SUCCEEDED(socket_rc))socketExit();
        consoleExit(NULL);
        return ok?0:1;
    }
    if (argc == 2 && (strncmp(argv[1], "--jvm-error=", 12) == 0 ||
                      strncmp(argv[1], "--instance-crash=", 17) == 0)) {
        bool ok = strncmp(argv[1],"--jvm-error=",12)==0?dump_jvm_error(argv[1]+12):dump_instance_crash(argv[1]+17);
        report("JVM report read: %s\n", ok ? "OK" : "FAILED");
        if (network_log) { fclose(network_log); network_log = NULL; }
        if (R_SUCCEEDED(socket_rc)) socketExit();
        consoleExit(NULL);
        return ok ? 0 : 1;
    }
    report_result("socket.initialize", socket_rc);
    report("NXLink log: %s\n", network_log ? "CONNECTED" : "UNAVAILABLE");
    if (R_SUCCEEDED(socket_rc) && !network_log) {
        report("NXLink connect errno: %d (%s)\n", network_error,
               strerror(network_error));
    }
    u32 version = hosversionGet();
    report("Horizon: %u.%u.%u; Atmosphere: %s\n",
           HOSVER_MAJOR(version), HOSVER_MINOR(version), HOSVER_MICRO(version),
           hosversionIsAtmosphere() ? "yes" : "no");
    AppletType applet_type = appletGetAppletType();
    report("Applet type: %d (%s)\n", (int)applet_type,
           applet_type == AppletType_Application ? "application" : "applet/other");
    report_memory();
    check_thread();
    check_jit_allocation();
    report("MINESWITCH_DIAG_CHECKS_COMPLETE\n");
    report("Press + to exit. Automatic return in 20 seconds.\n");

    u64 start = armGetSystemTick();
    unsigned last_heartbeat = 0;
    while (appletMainLoop()) {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_Plus) {
            report("Exit requested with +.\n");
            break;
        }
        u64 elapsed_ns = armTicksToNs(armGetSystemTick() - start);
        unsigned seconds = (unsigned)(elapsed_ns / UINT64_C(1000000000));
        if (seconds >= 20) {
            report("Automatic return.\n");
            break;
        }
        if (seconds / 5 > last_heartbeat) {
            last_heartbeat = seconds / 5;
            report("Heartbeat: %u seconds\n", seconds);
        }
        consoleUpdate(NULL);
    }

    report("MINESWITCH_DIAG_END\n");
    if (network_log) {
        // Close every socket-backed FILE before socketExit removes soc:.
        // Keeping stderr on the console also makes newlib's exit cleanup safe.
        FILE *finished_log = network_log;
        network_log = NULL;
        fclose(finished_log);
    }
    if (R_SUCCEEDED(socket_rc)) {
        socketExit();
    }
    consoleExit(NULL);
    return 0;
}
