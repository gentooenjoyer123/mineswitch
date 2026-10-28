#ifndef MINESWITCH_NOUVEAU_TOPOLOGY_H
#define MINESWITCH_NOUVEAU_TOPOLOGY_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
bool mineswitch_graph_units(uint32_t gpcs,uint32_t tpcs_per_gpc,uint32_t gpc_mask,
                           const uint32_t* tpc_masks,size_t mask_count,uint64_t* units);
#endif
