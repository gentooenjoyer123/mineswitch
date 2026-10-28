#include "nouveau_topology.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
    uint32_t masks[]={3,0,1};uint64_t units=0;
    assert(mineswitch_graph_units(1,2,1,masks,1,&units) && units==0x201);
    masks[0]=1;
    assert(mineswitch_graph_units(1,2,1,masks,1,&units) && units==0x101);
    masks[0]=3;
    assert(mineswitch_graph_units(3,2,5,masks,3,&units) && units==0x302);
    assert(!mineswitch_graph_units(1,2,1,masks,0,&units));
    assert(!mineswitch_graph_units(1,2,2,masks,1,&units));
    assert(!mineswitch_graph_units(1,2,0,masks,1,&units));
    masks[0]=4;
    assert(!mineswitch_graph_units(1,2,1,masks,1,&units));
    masks[0]=0;
    assert(!mineswitch_graph_units(1,2,1,masks,1,&units));
    assert(!mineswitch_graph_units(0,2,1,masks,1,&units));
    assert(!mineswitch_graph_units(32,2,1,masks,1,&units));
    assert(!mineswitch_graph_units(1,32,1,masks,1,&units));
    assert(!mineswitch_graph_units(1,2,1,NULL,1,&units));
    assert(!mineswitch_graph_units(1,2,1,masks,1,NULL));
    puts("NOUVEAU_TOPOLOGY_PASS real Switch topology,disabled units,sparse GPCs,malformed masks rejected");
}
