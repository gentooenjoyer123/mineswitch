#ifndef MINESWITCH_MIMALLOC_NEWLIB_ADAPTER_H
#define MINESWITCH_MIMALLOC_NEWLIB_ADAPTER_H
#include <stddef.h>
#ifdef MINESWITCH_MIMALLOC_HOST
struct _reent { int _errno; };
struct mallinfo {
    size_t arena,ordblks,smblks,hblks,hblkhd,usmblks,fsmblks,uordblks,fordblks,keepcost;
};
#else
#include <reent.h>
#include <malloc.h>
#endif
void* _malloc_r(struct _reent*,size_t);
void* _calloc_r(struct _reent*,size_t,size_t);
void* _realloc_r(struct _reent*,void*,size_t);
void* _reallocf_r(struct _reent*,void*,size_t);
void* _memalign_r(struct _reent*,size_t,size_t);
void _free_r(struct _reent*,void*);
void* _valloc_r(struct _reent*,size_t);
void* _pvalloc_r(struct _reent*,size_t);
size_t _malloc_usable_size_r(struct _reent*,void*);
struct mallinfo _mallinfo_r(struct _reent*);
int _mallopt_r(struct _reent*,int,int);
int _malloc_trim_r(struct _reent*,size_t);
void _malloc_stats_r(struct _reent*);
void _mstats_r(struct _reent*,char*);
int nxmi_posix_memalign(void**,size_t,size_t);
void* nxmi_aligned_alloc(size_t,size_t);
void nxmi_report(void);
#endif
