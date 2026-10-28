/* Copyright (c) 2026 MineSwitch contributors. SPDX-License-Identifier: GPL-2.0-only */
#include "nx_memory.h"
#include <switch.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static FILE* network_log;
static unsigned passed,failed;
static void report(const char* format,...) {
    va_list args;
    va_start(args,format); vprintf(format,args); va_end(args); fflush(stdout);
    if (network_log) {
        va_start(args,format); vfprintf(network_log,format,args); va_end(args); fflush(network_log);
    }
}
static bool check(const char* name,bool success) {
    if (success) ++passed; else ++failed;
    report("LARGE_HEAP_CHECK %s %s errno=%d sdk_rc=0x%08x\n",name,success?"PASS":"FAIL",errno,nxvm_last_result());
    return success;
}
static void memory(const char* phase) {
    u64 limit=0,used=0;
    svcGetInfo(&limit,InfoType_TotalMemorySize,CUR_PROCESS_HANDLE,0);
    svcGetInfo(&used,InfoType_UsedMemorySize,CUR_PROCESS_HANDLE,0);
    struct mallinfo info=mallinfo();
    extern char* fake_heap_start;
    extern char* fake_heap_end;
    report("LARGE_HEAP_MEMORY phase=%s limit=%" PRIu64 " mapped=%" PRIu64
           " heap_begin=%p heap_end=%p arena=%zu allocated=%zu free_arena=%zu top=%zu\n",
           phase,limit,used,(void*)fake_heap_start,(void*)fake_heap_end,
           info.arena,info.uordblks,info.fordblks,info.keepcost);
}
static bool permission(void* address,u32 expected) {
    MemoryInfo info;
    u32 page_info=0;
    Result result=svcQueryMemory(&info,&page_info,(u64)address);
    return R_SUCCEEDED(result) && info.perm==expected;
}
static bool zero(const unsigned char* bytes,size_t count) {
    for (size_t i=0;i<count;i++) if (bytes[i]) return false;
    return true;
}
static void reservation(size_t bytes) {
    errno=0;
    const size_t page=NXVM_PAGE_SIZE,alignment=65536;
    report("LARGE_HEAP_RESERVE bytes=%zu alignment=%zu\n",bytes,alignment);
    unsigned char* base=nxvm_reserve(bytes,alignment,false);
    if (!check("reserve",base!=NULL)) return;
    check("aligned",((uintptr_t)base&(alignment-1))==0);
    if (!check("actual_initial_none",permission(base,Perm_None) && permission(base+bytes-page,Perm_None))) goto release;
    if (!check("commit_ends",nxvm_commit(base,page,false) && nxvm_commit(base+bytes-page,page,false))) goto release;
    if (!check("actual_end_permissions",permission(base,Perm_Rw) && permission(base+bytes-page,Perm_Rw) && permission(base+page,Perm_None))) goto release;
    check("initial_zero",zero(base,page) && zero(base+bytes-page,page));
    memset(base,0x27,page); memset(base+bytes-page,0x91,page);
    check("ends_roundtrip",base[0]==0x27 && base[page-1]==0x27 && base[bytes-page]==0x91 && base[bytes-1]==0x91);
    if (!check("uncommit_first",nxvm_uncommit(base,page))) goto release;
    check("actual_uncommitted_none",permission(base,Perm_None));
    if (!check("recommit_first",nxvm_commit(base,page,false) && permission(base,Perm_Rw))) goto release;
    check("recommit_zero_preserve_other",zero(base,page) && base[bytes-page]==0x91 && base[bytes-1]==0x91);
    struct nxvm_memory_stats stats;
    nxvm_get_memory_stats(&stats);
    check("account_real_backing",stats.reservations==1 && stats.reserved_bytes==bytes && stats.backing_bytes==bytes && stats.committed_bytes==2*page);
release:
    if (!check("whole_release",nxvm_release(base,bytes))) {
        report("LARGE_HEAP_RELEASE_FAILED aborting further allocation checks\n");
        svcExitProcess();
    }
    struct nxvm_memory_stats after;
    nxvm_get_memory_stats(&after);
    check("no_reservations_left",after.reservations==0 && after.backing_bytes==0);
}
static void realloc_boundary(void) {
    const size_t old_size=((size_t)2<<30)-65536,new_size=((size_t)2<<30)+65536;
    extern unsigned long __malloc_trim_threshold;
    const unsigned long saved_trim=__malloc_trim_threshold;
    if (!check("retain_free_top",mallopt(M_TRIM_THRESHOLD,-1)==1)) return;
    // realloc is permitted to fail if it would need a second large allocation.
    // Establish actual adjacent free space before requiring in-place growth.
    unsigned char* old=malloc(new_size+1024*1024);
    if (!old) {
        check("realloc_spare_setup",false);
        mallopt(M_TRIM_THRESHOLD,saved_trim==ULONG_MAX?-1:(int)saved_trim);
        return;
    }
    volatile unsigned char* old_view=old;
    old_view[0]=0x3c; old_view[old_size-1]=0x5a;
    const uintptr_t original_address=(uintptr_t)old;
    unsigned char* smaller=realloc(old,old_size);
    const bool real_adjacent_tail=smaller && (uintptr_t)smaller==original_address;
    if (smaller) old=smaller;
    // No logging/allocation between the original allocation and its growth.
    unsigned char* grown=realloc(old,new_size);
    check("real_adjacent_tail",real_adjacent_tail);
    if (grown) {
        check("realloc_setup",true);
        volatile unsigned char* view=grown;
        check("realloc_cross_2GiB",true);
        check("realloc_preserved",view[0]==0x3c && view[old_size-1]==0x5a);
        view[new_size-1]=0x7d;
        check("realloc_new_end",view[new_size-1]==0x7d);
        const size_t impossible=(size_t)PTRDIFF_MAX;
        errno=0;
        void* rejected=realloc(grown,impossible);
        if (!rejected) {
            check("realloc_invalid_size",errno==ENOMEM);
            check("realloc_rejection_preserves_data",view[0]==0x3c && view[old_size-1]==0x5a && view[new_size-1]==0x7d);
            free(grown);
        } else {
            check("realloc_invalid_size",false);
            free(rejected);
        }
        void* small=malloc(64);
        check("allocator_still_usable",small!=NULL);
        free(small);
    } else {
        check("realloc_setup",true);
        check("realloc_cross_2GiB",false);
        check("realloc_failure_preserves_source",old[0]==0x3c && old[old_size-1]==0x5a);
        free(old);
    }
    check("restore_trimming",mallopt(M_TRIM_THRESHOLD,saved_trim==ULONG_MAX?-1:(int)saved_trim)==1);
    malloc_trim(0);
}
static void* aligned_blockers[4096];
static void fragmented_alignment(void) {
    const size_t mib=1024*1024,bytes=16*mib;
    unsigned char* original=memalign(bytes,bytes);
    if (!check("aligned_fragment_owner",original!=NULL)) return;
    const uintptr_t address=(uintptr_t)original;
    unsigned count=0;
    while (count<4096) {
        unsigned char* p=malloc(mib);
        if (!p) break;
        p[0]=0x39; p[mib-1]=0x81;
        aligned_blockers[count++]=p;
    }
    free(original);
    bool bounded=count<4096;
    unsigned char* node=bounded?nxvm_reserve(bytes,bytes,false):NULL;
    bool aligned=node && (uintptr_t)node==address;
    bool protected=node && permission(node,Perm_None) && permission(node+bytes-NXVM_PAGE_SIZE,Perm_None);
    bool committed=node && nxvm_commit(node,NXVM_PAGE_SIZE,false) && nxvm_commit(node+bytes-NXVM_PAGE_SIZE,NXVM_PAGE_SIZE,false);
    bool contents=false;
    if (committed) {
        node[0]=0x65;node[bytes-1]=0x92;
        contents=node[0]==0x65 && node[bytes-1]==0x92;
    }
    bool neighbors=true;
    for (unsigned i=0;i<count;i++) {
        const unsigned char* p=aligned_blockers[i];
        neighbors=neighbors && p[0]==0x39 && p[mib-1]==0x81;
    }
    bool released=node && nxvm_release(node,bytes);
    for (unsigned i=0;i<count;i++) { free(aligned_blockers[i]);aligned_blockers[i]=NULL; }
    check("aligned_fragment_bounded",bounded);
    check("aligned_fragment_exact_fit",aligned);
    check("aligned_fragment_actual_none",protected);
    check("aligned_fragment_commit",committed);
    check("aligned_fragment_contents",contents);
    check("aligned_fragment_neighbors",neighbors);
    check("aligned_fragment_release",released);
    struct nxvm_memory_stats stats;
    nxvm_get_memory_stats(&stats);
    check("aligned_fragment_no_leak",stats.reservations==0 && stats.backing_bytes==0);
    report("ALIGNED_FRAGMENT blockers=%u bytes_per_block=%zu\n",count,mib);
}
int main(void) {
    consoleInit(NULL);
    Result sockets=socketInitializeDefault();
    if (R_SUCCEEDED(sockets)) {
        int descriptor=nxlinkConnectToHost(false,false);
        if (descriptor>=0) { network_log=fdopen(descriptor,"w"); if (!network_log) close(descriptor); }
    }
    report("LARGE_HEAP_BEGIN applet=%d sizeof_size_t=%zu sizeof_ptrdiff=%zu\n",appletGetAppletType(),sizeof(size_t),sizeof(ptrdiff_t));
    memory("before");
    if (check("application_mode",appletGetAppletType()==AppletType_Application || appletGetAppletType()==AppletType_SystemApplication)) {
        reservation((size_t)2<<30);
        if (!failed) reservation(((size_t)2<<30)+65536);
        if (!failed) realloc_boundary();
        if (!failed) fragmented_alignment();
    }
    memory("after");
    report("LARGE_HEAP_DONE passed=%u failed=%u\n",passed,failed);
    uint64_t start=armGetSystemTick();
    while (appletMainLoop() && armTicksToNs(armGetSystemTick()-start)<UINT64_C(1000000000)) {
        consoleUpdate(NULL); svcSleepThread(16000000);
    }
    if (network_log) fclose(network_log);
    if (R_SUCCEEDED(sockets)) socketExit();
    consoleExit(NULL);
    return failed?1:0;
}
