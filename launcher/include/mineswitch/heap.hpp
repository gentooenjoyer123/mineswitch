#pragma once
#include <cstdint>
#include <string>
#include "heap_limits.h"

namespace mineswitch {
constexpr int kPortableMaxHeapMiB = MINESWITCH_PORTABLE_MAX_HEAP_MIB;
struct HeapBudget {
    std::uint64_t process_bytes = 0;
    int max_mib = MINESWITCH_STANDARD_MAX_HEAP_MIB;
};
// Use the memory actually exposed by Horizon, including expanded-memory mods.
// Normal/unknown processes cap at 1900 MiB, also with the forwarder's larger
// 3285 MiB budget; expanded pools retain 1 GiB
// for native code, graphics, JVM metadata and stacks, with a5900MiB ceiling.
int heapLimitForProcessMemory(std::uint64_t process_bytes);
int nextHeapOption(int current_mib,int maximum_mib);
HeapBudget currentHeapBudget();
// ParallelGC collects on every core but needs ~heap/32 of native memory for
// its mark bitmap; "auto" picks it only on expanded-memory consoles.
bool useParallelGC(const std::string& setting, const HeapBudget&);
// Heap ceiling with ParallelGC: 512 MiB of the native reserve stays for it.
int parallelHeapLimit(const HeapBudget&);
}
