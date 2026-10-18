// SPDX-License-Identifier: GPL-2.0-or-later
// Particle heap reuse (src/particle_heap_hook.h): the game starts its particle allocator on the
// half of the pool it used two frames ago. Everything the GPU wrote there is dead by the game's
// own double buffering, so the buffer cache forgets it instead of downloading it when the game
// writes the first new particle records.
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "core/memory.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"

extern "C" void __attribute__((sysv_abi)) tlg_particle_heap_begin(void* allocator, uint64_t base,
                                                                  int32_t size) {
    // Begin only starts an idle allocator (cur == 0); otherwise the half is still in use.
    uint64_t cur;
    std::memcpy(&cur, static_cast<const unsigned char*>(allocator) + 8, sizeof(cur));
    auto* rasterizer = Core::Memory::Instance()->GetRasterizer();
    if (cur || size <= 0 || !rasterizer) {
        return;
    }
    static std::atomic<uint64_t> discards{0}, skipped{0};
    if (!rasterizer->GetBufferCache().DiscardDeadGpuData(base, static_cast<uint64_t>(size))) {
        skipped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const uint64_t n = discards.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n == 1 || n % 3600 == 0) {
        std::printf("Particle heap: dead GPU data discarded at %#llx (%llu MiB), %llu times, %llu skipped\n",
                    static_cast<unsigned long long>(base), static_cast<unsigned long long>(size >> 20),
                    static_cast<unsigned long long>(n),
                    static_cast<unsigned long long>(skipped.load(std::memory_order_relaxed)));
        std::fflush(stdout);
    }
}
