#include "mineswitch/heap.hpp"
#include <algorithm>
#ifdef __SWITCH__
#include <switch.h>
#endif

namespace mineswitch {
int heapLimitForProcessMemory(std::uint64_t bytes) {
    constexpr std::uint64_t mib = 1024 * 1024;
    // The forwarder's extra 96 MiB stays native headroom: 1996 MiB of heap
    // ran Sodium out of native memory on 2026-10-25.
    if (bytes <= 4096 * mib) return MINESWITCH_STANDARD_MAX_HEAP_MIB;
    if (bytes / mib - 1024 >= kPortableMaxHeapMiB) return kPortableMaxHeapMiB;
    const auto available = ((bytes / mib - 1024) / 512) * 512;
    return static_cast<int>(std::min<std::uint64_t>(kPortableMaxHeapMiB, available));
}
int nextHeapOption(int current,int maximum) {
    const int sizes[] = {512,768,1024,1280,1536,1792,1900,2048,2560,3072,
                         3584,4096,4608,5120,5632,5900};
    for(int n:sizes)if(n>current && n<=maximum)return n;
    return current<maximum?maximum:std::min(512,maximum);
}
HeapBudget currentHeapBudget() {
    HeapBudget result;
#ifdef __SWITCH__
    u64 bytes = 0;
    if (R_SUCCEEDED(svcGetInfo(&bytes, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0)))
        result.process_bytes = bytes;
#endif
    result.max_mib = heapLimitForProcessMemory(result.process_bytes);
    return result;
}
}
