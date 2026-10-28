/* Report actual GM20B topology instead of libdrm_nouveau's desktop placeholder. */
#include "nouveau_topology.h"

bool mineswitch_graph_units(uint32_t gpcs,uint32_t tpcs_per_gpc,uint32_t gpc_mask,
                           const uint32_t* masks,size_t count,uint64_t* units) {
    if (!units || !masks || !gpcs || gpcs>8 || !tpcs_per_gpc || tpcs_per_gpc>8 || count<gpcs ||
            !gpc_mask || (gpc_mask & ~((1u<<gpcs)-1))) return false;
    uint32_t active_gpcs=0,active_tpcs=0;
    for (uint32_t i=0;i<gpcs;i++) if (gpc_mask & (1u<<i)) {
        if (!masks[i] || (masks[i] & ~((1u<<tpcs_per_gpc)-1))) return false;
        ++active_gpcs;
        active_tpcs+=(uint32_t)__builtin_popcount(masks[i]);
    }
    *units=((uint64_t)active_tpcs<<8)|active_gpcs;
    return true;
}

#ifdef __SWITCH__
#include <switch.h>
#include <nouveau.h>
#include <stdatomic.h>
#include <stdio.h>
#include <inttypes.h>

/* Public libdrm query number from nouveau_drm.h. Other queries delegate intact. */
enum { MINESWITCH_GRAPH_UNITS_QUERY=13 };
int __real_nouveau_getparam(struct nouveau_device*,uint64_t,uint64_t*);
int __wrap_nouveau_getparam(struct nouveau_device* device,uint64_t param,uint64_t* value) {
    if (device && device->chipset==0x120 && value && param==MINESWITCH_GRAPH_UNITS_QUERY) {
        const nvioctl_gpu_characteristics* gpu=nvGpuGetCharacteristics();
        uint32_t count=0;const uint32_t* masks=nvGpuGetTpcMasks(&count);
        uint64_t actual;
        if (gpu && mineswitch_graph_units(gpu->num_gpc,gpu->num_tpc_per_gpc,gpu->gpc_mask,masks,count,&actual)) {
            uint64_t previous=0;
            int previous_rc=__real_nouveau_getparam(device,param,&previous);
            *value=actual;
            static atomic_flag announced=ATOMIC_FLAG_INIT;
            if (!atomic_flag_test_and_set_explicit(&announced,memory_order_relaxed))
                printf("MINESWITCH_GPU_TOPOLOGY previous=0x%" PRIx64 " previous_rc=%d actual=0x%" PRIx64
                       " gpc=%u sm=%u source=libnx_characteristics_and_tpc_masks\n",
                       previous,previous_rc,actual,(unsigned)(actual&255),(unsigned)(actual>>8));
            return 0;
        }
    }
    return __real_nouveau_getparam(device,param,value);
}
#endif
