/* src/particle_heap_hook.h: finds the particle allocator's two Begin call sites by code (unique,
 * branching to the expected Begin), and runs the stub for real: the helper sees the arguments on
 * an aligned stack, and Begin still gets them unchanged. With an eboot.elf path as argument, the
 * sites are found in the game too (CUSA00004 01.07 offsets). */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/particle_heap_hook.h"
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif
#define SYSV __attribute__((sysv_abi))

static void put_branch(unsigned char *code, uint64_t rel, uint64_t target) {
    const int32_t delta = (int32_t)((int64_t)target - (int64_t)(rel + 4));
    memcpy(code + rel, &delta, 4);
}
/* Call site at 0x100, jump site at 0x200, Begin at 0x300. */
static void build(unsigned char *code, uint64_t size) {
    memset(code, 0xcc, size);
    memcpy(code + 0x100, tlg_particle_call_site, sizeof(tlg_particle_call_site));
    put_branch(code, 0x100 + sizeof(tlg_particle_call_site), 0x300);
    memcpy(code + 0x200, tlg_particle_jump_site, sizeof(tlg_particle_jump_site));
    put_branch(code, 0x200 + sizeof(tlg_particle_jump_site), 0x300);
    memcpy(code + 0x300, tlg_particle_begin, sizeof(tlg_particle_begin));
}

static struct { void *allocator; uint64_t base; int32_t size; uintptr_t rsp; int calls; } seen;
static SYSV void helper(void *allocator, uint64_t base, int32_t size) {
    seen.allocator = allocator;
    seen.base = base;
    seen.size = size;
    seen.rsp = (uintptr_t)__builtin_frame_address(0) + 8; /* rsp at entry (rbp = entry rsp - 8) */
    ++seen.calls;
    /* Clobber the argument registers: the stub must restore them for Begin. */
    __asm__ volatile("xor %%edi, %%edi\n\txor %%esi, %%esi\n\txor %%edx, %%edx" ::: "rdi", "rsi", "rdx");
}
static SYSV void begin(uint64_t *allocator, uint64_t base, int32_t size) {
    if (allocator[1]) return;
    allocator[0] = allocator[1] = base;
    allocator[2] = base + (uint64_t)(int64_t)size;
}

int main(int argc, char **argv) {
    static unsigned char code[0x400];
    TlgParticleSites sites;
    build(code, sizeof(code));
    assert(tlg_particle_find_sites(code, sizeof(code), &sites));
    assert(sites.call_rel == 0x100 + sizeof(tlg_particle_call_site));
    assert(sites.jump_rel == 0x200 + sizeof(tlg_particle_jump_site));
    assert(sites.begin == 0x300);
    /* Begin must have the expected body. */
    code[0x305] ^= 1;
    assert(!tlg_particle_find_sites(code, sizeof(code), &sites));
    /* Both sites must branch to the same Begin. */
    build(code, sizeof(code));
    put_branch(code, 0x200 + sizeof(tlg_particle_jump_site), 0x380);
    assert(!tlg_particle_find_sites(code, sizeof(code), &sites));
    /* A second copy of a site is ambiguous. */
    build(code, sizeof(code));
    memcpy(code + 0x3a0, tlg_particle_jump_site, sizeof(tlg_particle_jump_site));
    assert(!tlg_particle_find_sites(code, sizeof(code), &sites));
    /* Missing site, and a site whose rel32 would run past the end. */
    build(code, sizeof(code));
    code[0x100] = 0x90;
    assert(!tlg_particle_find_sites(code, sizeof(code), &sites));
    build(code, sizeof(code));
    assert(!tlg_particle_find_sites(code, 0x200 + sizeof(tlg_particle_jump_site) + 2, &sites));

    if (argc > 1) {
        FILE *file = fopen(argv[1], "rb");
        assert(file && !fseek(file, 0x4000, SEEK_SET));
        const uint64_t size = 0xcc55b8;
        unsigned char *text = malloc(size);
        assert(text && fread(text, 1, size, file) == size);
        fclose(file);
        assert(tlg_particle_find_sites(text, size, &sites));
        assert(sites.call_rel == 0x3ee87d && sites.jump_rel == 0x3ed668 && sites.begin == 0x577cb0);
        free(text);
    }

#ifdef _WIN32
    unsigned char *stub = VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
#else
    unsigned char *stub = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(stub != MAP_FAILED);
#endif
    assert(stub);
    unsigned char built[TLG_PARTICLE_STUB_BYTES];
    tlg_particle_build_stub(built, (uintptr_t)helper, (uintptr_t)begin);
    memcpy(stub, built, sizeof(built));
#ifdef _WIN32
    DWORD previous;
    assert(VirtualProtect(stub, 4096, PAGE_EXECUTE_READ, &previous));
    FlushInstructionCache(GetCurrentProcess(), stub, sizeof(built));
#else
    assert(!mprotect(stub, 4096, PROT_READ | PROT_EXEC));
#endif
    typedef void (SYSV *Begin)(uint64_t *, uint64_t, int32_t);
    const Begin hook = (Begin)(uintptr_t)stub;
    uint64_t allocator[4] = {0};
    hook(allocator, 0x106d558000ull, 0x1e00000);
    assert(seen.calls == 1 && seen.allocator == allocator && seen.base == 0x106d558000ull && seen.size == 0x1e00000);
    assert(seen.rsp % 16 == 8); /* a normal call's entry alignment */
    assert(allocator[0] == 0x106d558000ull && allocator[1] == 0x106d558000ull &&
           allocator[2] == 0x106d558000ull + 0x1e00000);
    /* Allocator in use: Begin leaves it alone; the helper still runs (it checks cur itself). */
    hook(allocator, 0x106f358000ull, 0x1e00000);
    assert(seen.calls == 2 && allocator[0] == 0x106d558000ull);
    puts("PASS: particle heap hook");
    return 0;
}
