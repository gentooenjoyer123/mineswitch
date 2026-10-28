#include "newlib_adapter.h"
#include "page_pool.h"
#include "large_pages.h"
#include "mimalloc.h"
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void* result(struct _reent* r,void* p) {
    if(!p) { if(r)r->_errno=ENOMEM;errno=ENOMEM; }
    return p;
}
static bool allocate_pages(size_t bytes,size_t alignment,void** out) {
    if(bytes>=256*1024)mi_process_init();
    return nxmi_large_alloc(bytes,alignment,out);
}
void* _malloc_r(struct _reent* r,size_t n) {
    void* p=NULL;
    if(!allocate_pages(n,sizeof(void*),&p))p=mi_malloc(n);
    return result(r,p);
}
void* _calloc_r(struct _reent* r,size_t n,size_t size) {
    if(size && n>SIZE_MAX/size)return result(r,NULL);
    void* p=NULL;size_t bytes=n*size;
    if(!allocate_pages(bytes,sizeof(void*),&p))return result(r,mi_calloc(n,size));
    if(p)memset(p,0,bytes);
    return result(r,p);
}
void _free_r(struct _reent* r,void* p) { (void)r;if(!nxmi_large_free(p))mi_free(p); }
void* _realloc_r(struct _reent* r,void* p,size_t n) {
    if(!p)return _malloc_r(r,n);
    if(!n) {_free_r(r,p);return NULL;}
    size_t requested=0,backing=0;
    bool direct=nxmi_large_lookup(p,&requested,&backing);
    if(direct && nxmi_large_shrink(p,n))return p;
    if(!direct && n<256*1024)return result(r,mi_realloc(p,n));
    if(n>PTRDIFF_MAX)return result(r,NULL);
    if(!direct)requested=mi_usable_size(p);
    void* next=_malloc_r(r,n);
    if(next) {memcpy(next,p,n<requested?n:requested);_free_r(r,p);}
    return next;
}
void* _reallocf_r(struct _reent* r,void* p,size_t n) {
    void* next=_realloc_r(r,p,n);
    if(!next && n)_free_r(r,p);
    return next;
}
void* _memalign_r(struct _reent* r,size_t alignment,size_t n) {
    if(!alignment || (alignment&(alignment-1))) { if(r)r->_errno=EINVAL;errno=EINVAL;return NULL; }
    void* p=NULL;
    if(!allocate_pages(n,alignment,&p))p=mi_malloc_aligned(n,alignment);
    return result(r,p);
}
void* _valloc_r(struct _reent* r,size_t n) { return _memalign_r(r,4096,n); }
void* _pvalloc_r(struct _reent* r,size_t n) {
    if(n>SIZE_MAX-4095) { if(r)r->_errno=ENOMEM;errno=ENOMEM;return NULL; }
    return _memalign_r(r,4096,n?(n+4095)&~(size_t)4095:4096);
}
size_t _malloc_usable_size_r(struct _reent* r,void* p) {
    (void)r;size_t requested,backing;
    return nxmi_large_lookup(p,&requested,&backing)?backing:mi_usable_size(p);
}
struct mallinfo _mallinfo_r(struct _reent* r) {
    (void)r;
    struct nxmi_pool_stats s;nxmi_pool_stats(&s,false);
    // Conservative backing inventory. Partial free slots within mimalloc's
    // pages are reusable by mimalloc, but aren't promised as free OS extents.
    return (struct mallinfo){.arena=s.capacity,.uordblks=s.reserved,
                            .fordblks=s.capacity-s.reserved};
}
int _mallopt_r(struct _reent* r,int option,int value) {
    (void)r;(void)option;(void)value;return 0; // Newlib-specific tuning is unsupported.
}
int _malloc_trim_r(struct _reent* r,size_t pad) {
    (void)r;(void)pad;
    struct nxmi_pool_stats before,after;nxmi_pool_stats(&before,false);
    mi_collect(true);nxmi_pool_stats(&after,false);
    return after.reserved<before.reserved;
}
void nxmi_report(void) {
    nxmi_large_report();
    struct nxmi_pool_stats s;nxmi_pool_stats(&s,true);
    printf("MINESWITCH_MIMALLOC version=%d capacity=%zu backing=%zu accessible=%zu peak=%zu"
           " pool_free=%zu largest_pool_extent=%zu allocations=%zu failures=%zu"
           " last_thread_failure_size=%zu last_thread_failure_alignment=%zu\n",
           mi_version(),s.capacity,s.reserved,s.accessible,s.peak,s.capacity-s.reserved,
           s.largest_free,s.allocations,s.failures,s.last_failure_size,s.last_failure_alignment);
}
void _malloc_stats_r(struct _reent* r) { (void)r;nxmi_report(); }
void _mstats_r(struct _reent* r,char* label) { (void)label;_malloc_stats_r(r); }
int nxmi_posix_memalign(void** out,size_t alignment,size_t n) {
    if(alignment<sizeof(void*) || (alignment&(alignment-1)))return EINVAL;
    int saved_errno=errno;
    void* p=_memalign_r(NULL,alignment,n);
    if(p)*out=p;
    errno=saved_errno;
    return p?0:ENOMEM;
}
void* nxmi_aligned_alloc(size_t alignment,size_t n) {
    if(!alignment || (alignment&(alignment-1)) || n%alignment) {errno=EINVAL;return NULL;}
    return _memalign_r(NULL,alignment,n);
}
#ifndef MINESWITCH_MIMALLOC_HOST
// Some SDK statistics wrappers share an archive member with _mstats_r. Supply
// the public ABI too, so none of those mixed implementation objects is pulled.
void* malloc(size_t n) { return _malloc_r(_REENT,n); }
void* calloc(size_t n,size_t size) { return _calloc_r(_REENT,n,size); }
void* realloc(void* p,size_t n) { return _realloc_r(_REENT,p,n); }
void* reallocf(void* p,size_t n) { return _reallocf_r(_REENT,p,n); }
void free(void* p) { _free_r(_REENT,p); }
void* memalign(size_t alignment,size_t n) { return _memalign_r(_REENT,alignment,n); }
void* valloc(size_t n) { return _valloc_r(_REENT,n); }
void* pvalloc(size_t n) { return _pvalloc_r(_REENT,n); }
size_t malloc_usable_size(void* p) { return _malloc_usable_size_r(_REENT,p); }
struct mallinfo mallinfo(void) { return _mallinfo_r(_REENT); }
int mallopt(int option,int value) { return _mallopt_r(_REENT,option,value); }
int malloc_trim(size_t pad) { return _malloc_trim_r(_REENT,pad); }
void malloc_stats(void) { _malloc_stats_r(_REENT); }
void mstats(char* label) { _mstats_r(_REENT,label); }
// The entire loader heap belongs to the page pool. Prevent any secondary sbrk
// allocator from claiming overlapping memory. libnx fills these globals first.
char* fake_heap_start;
char* fake_heap_end;
void* _sbrk_r(struct _reent* r,ptrdiff_t amount) {
    (void)amount;if(r)r->_errno=ENOMEM;errno=ENOMEM;return (void*)-1;
}
int posix_memalign(void** out,size_t alignment,size_t n) { return nxmi_posix_memalign(out,alignment,n); }
void* aligned_alloc(size_t alignment,size_t n) { return nxmi_aligned_alloc(alignment,n); }
#endif
