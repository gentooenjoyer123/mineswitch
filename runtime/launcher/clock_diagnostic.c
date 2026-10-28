/* Read-only comparison of SDK cached/shared-memory and direct service clocks. */
#include "clock_diagnostic.h"
#include <switch.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

static void report_service(const char* path,const char* name,Result result,u64 value) {
    char utc[40]="unavailable";
    if (R_SUCCEEDED(result)) {
        time_t seconds=(time_t)value;
        struct tm calendar;
        if (gmtime_r(&seconds,&calendar)) strftime(utc,sizeof(utc),"%Y-%m-%dT%H:%M:%SZ",&calendar);
    }
    printf("CLOCK_DIAG path=%s clock=%s rc=0x%08x posix_seconds=%" PRIu64 " utc=%s\n",
        path,name,result,value,utc);
}
void launcher_clock_diagnostic(void) {
    u64 start=armGetSystemTick();
    struct timespec realtime={0},monotonic={0};
    struct timeval tod={0};
    int real_result=clock_gettime(CLOCK_REALTIME,&realtime),real_error=errno;
    int mono_result=clock_gettime(CLOCK_MONOTONIC,&monotonic),mono_error=errno;
    int tod_result=gettimeofday(&tod,NULL),tod_error=errno;
    printf("CLOCK_DIAG ticks_begin=%" PRIu64 " sizeof_time_t=%zu\n",start,sizeof(time_t));
    printf("CLOCK_DIAG path=clock_gettime clock=realtime rc=%d errno=%d seconds=%" PRId64 " nanos=%ld\n",
        real_result,real_result?real_error:0,(int64_t)realtime.tv_sec,realtime.tv_nsec);
    printf("CLOCK_DIAG path=clock_gettime clock=monotonic rc=%d errno=%d seconds=%" PRId64 " nanos=%ld\n",
        mono_result,mono_result?mono_error:0,(int64_t)monotonic.tv_sec,monotonic.tv_nsec);
    printf("CLOCK_DIAG path=gettimeofday rc=%d errno=%d seconds=%" PRId64 " micros=%ld\n",
        tod_result,tod_result?tod_error:0,(int64_t)tod.tv_sec,(long)tod.tv_usec);
    Result initialized=timeInitialize();
    printf("CLOCK_DIAG time_initialize_rc=0x%08x shared_memory=%d\n",
        initialized,R_SUCCEEDED(initialized)&&timeGetSharedmemAddr()!=NULL);
    if (R_SUCCEEDED(initialized)) {
        const TimeType types[]={TimeType_UserSystemClock,TimeType_NetworkSystemClock,TimeType_LocalSystemClock};
        const char* names[]={"user","network","local"};
        for (unsigned i=0;i<3;i++) {
            u64 shared=0,direct=0;
            Result fast=timeGetCurrentTime(types[i],&shared);
            report_service("timeGetCurrentTime",names[i],fast,shared);
            Service* clock=timeGetServiceSession_SystemClock(types[i]);
            Result ipc=MAKERESULT(Module_Libnx,LibnxError_NotInitialized);
            if (clock && serviceIsActive(clock)) {
                // ISystemClock::GetCurrentTime, command0, no input or mutation.
                // This is the same command used by the SDK's non-shared path.
                ipc=serviceDispatchOut(clock,0,direct);
            }
            report_service("ISystemClock.GetCurrentTime",names[i],ipc,direct);
        }
        timeExit();
    }
    u64 end=armGetSystemTick();
    printf("CLOCK_DIAG ticks_end=%" PRIu64 " elapsed_ns=%" PRIu64 " read_only=1\n",
        end,armTicksToNs(end-start));
}
