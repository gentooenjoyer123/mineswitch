#include <switch.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "page_pool.h"
#include "newlib_adapter.h"

enum { PIECE=65536, COUNT=16, TOTAL=PIECE*COUNT };
static unsigned pass,fail;
static bool check(const char* label,bool ok,Result rc) {
    printf("GPU_ALIAS_CHECK %s %s rc=0x%08x\n",label,ok?"PASS":"FAIL",rc);fflush(stdout);
    if(ok)pass++;else fail++;return ok;
}
static u32 attrs(void* p) {
    MemoryInfo info;u32 flags;
    return R_SUCCEEDED(svcQueryMemory(&info,&flags,(u64)p))?info.attr:UINT32_MAX;
}
static bool gpu_map(void* p,const char* label) {
    NvMap map={0};NvAddressSpace as={0};iova_t address=0;
    armDCacheFlush(p,TOTAL);
    Result rc=nvMapCreate(&map,p,TOTAL,PIECE,NvKind_Pitch,true);
    if(!check(label,R_SUCCEEDED(rc),rc))return false;
    rc=nvAddressSpaceCreate(&as,PIECE);
    if(check("gpu_address_space",R_SUCCEEDED(rc),rc)) {
        rc=nvAddressSpaceMap(&as,map.handle,true,NvKind_Pitch,&address);
        if(check("gpu_iova_map",R_SUCCEEDED(rc),rc)) {
            rc=nvAddressSpaceUnmap(&as,address);
            check("gpu_iova_unmap",R_SUCCEEDED(rc),rc);
        }
        nvAddressSpaceClose(&as);
    }
    nvMapClose(&map);return R_SUCCEEDED(rc);
}
int main(void) {
    if(R_SUCCEEDED(socketInitializeDefault()))nxlinkStdio();
    puts("GPU_ALIAS_PROBE_BEGIN");fflush(stdout);
    void* source[COUNT]={0};void* guard[COUNT]={0};void* mirror=NULL;
    VirtmemReservation* reservation=NULL;unsigned mapped=0;
    bool nv=false,map_service=false,wrote_alias=false;
    Result rc=nvInitialize();
    if(!check("nv_initialize",R_SUCCEEDED(rc),rc))goto cleanup;
    nv=true;rc=nvMapInit();
    if(!check("nvmap_initialize",R_SUCCEEDED(rc),rc))goto cleanup;
    map_service=true;
    void* control=NULL;
    if(check("control_backing",nxmi_pool_alloc(TOTAL,PIECE,true,&control)==0,0)) {
        gpu_map(control,"ordinary_heap_nvmap");nxmi_pool_free(control,TOTAL);
    }
    for(unsigned i=0;i<COUNT;i++) {
        if(!check("source_piece_and_guard",nxmi_pool_alloc(2*PIECE,PIECE,true,&source[i])==0,0))goto cleanup;
        guard[i]=(char*)source[i]+PIECE;
        memset(source[i],0x20+i,PIECE);memset(guard[i],0x71,PIECE);
        if(i && !check("nonadjacent_sources",(uintptr_t)source[i]!=(uintptr_t)source[i-1]+PIECE,0))goto cleanup;
        armDCacheFlush(source[i],PIECE);
    }
    virtmemLock();
    void* found=virtmemFindStack(TOTAL+PIECE,PIECE);
    if(found) {
        mirror=(void*)(((uintptr_t)found+PIECE-1)&~(uintptr_t)(PIECE-1));
        reservation=virtmemAddReservation(mirror,TOTAL);
    }
    virtmemUnlock();
    if(!check("reserve_contiguous_virtual_range",reservation!=NULL,0))goto cleanup;
    for(;mapped<COUNT;mapped++) {
        rc=svcMapMemory((char*)mirror+mapped*PIECE,source[mapped],PIECE);
        if(!check("map_piece",R_SUCCEEDED(rc),rc))goto cleanup;
    }
    for(unsigned i=0;i<COUNT;i++) {
        unsigned char* p=(unsigned char*)mirror+i*PIECE;
        check("aliased_real_bytes",p[0]==0x20+i && p[PIECE-1]==0x20+i,0);
        p[0]=0x80+i;
    }
    wrote_alias=true;
    rc=svcSetMemoryAttribute(mirror,TOTAL,8,8);
    printf("GPU_ALIAS_UNCACHED_CAPABILITY rc=0x%08x attributes=0x%08x\n",rc,attrs(mirror));
    if(R_SUCCEEDED(rc)) {
        check("uncached_attribute_applied",(attrs(mirror)&8)!=0,0);
        Result restore=svcSetMemoryAttribute(mirror,TOTAL,8,0);
        if(!check("restore_cache_attribute",R_SUCCEEDED(restore),restore))svcExitProcess();
    }
    gpu_map(mirror,"fragmented_stack_nvmap_cached");
cleanup:
    if(mirror && mapped)armDCacheFlush(mirror,mapped*PIECE);
    while(mapped) {
        --mapped;rc=svcUnmapMemory((char*)mirror+mapped*PIECE,source[mapped],PIECE);
        if(!check("unmap_restore_source",R_SUCCEEDED(rc),rc))svcExitProcess();
        check("source_roundtrip",((unsigned char*)source[mapped])[0]==(wrote_alias?0x80:0x20)+mapped,0);
    }
    if(reservation) {virtmemLock();virtmemRemoveReservation(reservation);virtmemUnlock();}
    for(unsigned i=0;i<COUNT;i++) {
        if(guard[i]) {
            check("guard_preserved",((unsigned char*)guard[i])[0]==0x71 && ((unsigned char*)guard[i])[PIECE-1]==0x71,0);
            nxmi_pool_free(guard[i],PIECE);
        }
        if(source[i])nxmi_pool_free(source[i],PIECE);
    }
    if(map_service)nvMapExit();
    if(nv)nvExit();
    nxmi_report();
    printf("GPU_ALIAS_PROBE_RESULT pass=%u fail=%u\n",pass,fail);fflush(stdout);
    svcSleepThread(300000000);
    appletSetTerminateResult(0);
    Service* endpoint=appletGetServiceSession_SelfController();
    if(endpoint && serviceIsActive(endpoint)) {
        Service request=*endpoint;serviceAssumeDomain(&request);
        if(R_SUCCEEDED(serviceDispatch(&request,0)))for(;;)svcSleepThread(1000000000);
    }
    svcExitProcess();
}
