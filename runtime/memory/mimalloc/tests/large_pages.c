#include "large_pages.h"
#include "page_pool.h"
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

int main(void) {
    const size_t M=1024*1024,capacity=64*M,map_size=capacity+2*M;
    void* raw=mmap(NULL,map_size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    assert(raw!=MAP_FAILED);
    void* base=(void*)((((uintptr_t)raw+M-1)&~(uintptr_t)(M-1))+4096);
    assert(nxmi_pool_init(base,capacity)==0);
    void* blocks[64];
    for(unsigned i=0;i<64;i++) {
        assert(nxmi_pool_alloc(M,4096,true,&blocks[i])==0);
        assert(blocks[i]==(char*)base+i*M);
        memset(blocks[i],0x53,M);
    }
    for(unsigned i=20;i<23;i++)assert(nxmi_pool_free(blocks[i],M)==0);
    void* failed=NULL;
    assert(nxmi_pool_alloc(2637824,M,true,&failed)==ENOMEM);
    void* mesh=NULL;
    assert(nxmi_large_alloc(5*M/2,4096,&mesh) && mesh);
    memset(mesh,0x39,5*M/2);
    size_t requested,backing;
    assert(nxmi_large_lookup(mesh,&requested,&backing) && requested==5*M/2 && backing==5*M/2);
    /* Registry metadata remains readable with the entire payload protected. */
    assert(nxmi_pool_permission(mesh,backing,false)==0);
    assert(nxmi_large_lookup(mesh,&requested,&backing));
    assert(nxmi_large_free(mesh));
    assert(!nxmi_large_lookup(mesh,&requested,&backing) && !nxmi_large_free(mesh));
    assert(((unsigned char*)blocks[19])[M-1]==0x53 && ((unsigned char*)blocks[23])[0]==0x53);
    for(unsigned i=0;i<64;i++)if(i<20 || i>=23)assert(nxmi_pool_free(blocks[i],M)==0);
    struct nxmi_pool_stats s;nxmi_pool_stats(&s,true);
    assert(s.reserved==0 && s.largest_free==capacity);
    nxmi_large_report();
    puts("LARGE_PAGES_FRAGMENTATION_PASS original1MiB-aligned request fails,exact2.5MiB succeeds,protected payload free,guards preserved,no leaked pages");
    munmap(raw,map_size);
}
