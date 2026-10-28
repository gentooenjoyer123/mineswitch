#ifndef MINESWITCH_HORIZON_FFI_CLOSURE_H
#define MINESWITCH_HORIZON_FFI_CLOSURE_H
#include <ffi.h>
#ifdef __cplusplus
extern "C" {
#endif
ffi_status horizon_ffi_publish_closure(ffi_closure* closure);
void horizon_ffi_closure_counts(size_t* live,size_t* pools);
#ifdef __cplusplus
}
#endif
#endif
