#include "mineswitch/heap.hpp"
#include <cassert>
#include <cstdint>
#include <iostream>

int main() {
    using mineswitch::heapLimitForProcessMemory;
    constexpr std::uint64_t mib = 1024 * 1024;
    assert(heapLimitForProcessMemory(0) == 1900);
    assert(heapLimitForProcessMemory(512 * mib) == 1900); // applet cannot launch a game
    assert(heapLimitForProcessMemory(3343908864ULL) == 1900); // Portal/title takeover
    assert(heapLimitForProcessMemory(3444572160ULL) == 1900); // HOME forwarder keeps native headroom
    assert(heapLimitForProcessMemory(4096 * mib) == 1900);
    assert(heapLimitForProcessMemory(6144 * mib) == 5120);
    assert(heapLimitForProcessMemory(6656 * mib) == 5632);
    assert(heapLimitForProcessMemory(6924 * mib) == 5900);
    assert(heapLimitForProcessMemory(7168 * mib) == 5900);
    assert(heapLimitForProcessMemory(8192 * mib) == 5900);
    for (std::uint64_t amount = 4097; amount <= 12288; ++amount) {
        const auto limit = heapLimitForProcessMemory(amount * mib);
        assert(limit > 1900 && limit <= mineswitch::kPortableMaxHeapMiB);
        assert((limit % 512 == 0 || limit==5900) && static_cast<std::uint64_t>(limit) + 1024 <= amount);
    }
    for(int maximum:{1900,3072,5632,5900}) {
        int current=512;bool reached=false;
        for(unsigned i=0;i<20;i++) {
            current=mineswitch::nextHeapOption(current,maximum);
            assert(current>=512 && current<=maximum);
            reached|=current==maximum;
        }
        assert(reached && mineswitch::nextHeapOption(maximum,maximum)==512);
    }
    std::cout << "HEAP_POLICY_PASS standard, unknown, expanded process budgets and native reserve\n";
}
