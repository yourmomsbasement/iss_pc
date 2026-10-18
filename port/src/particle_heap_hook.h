/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef TLG_PARTICLE_HEAP_HOOK_H
#define TLG_PARTICLE_HEAP_HOOK_H
#include <stdint.h>
#include <string.h>

/* inFAMOUS Second Son's particle manager (singleton at image 0x7d3eb10 in CUSA00004 01.07) owns a
 * 60 MiB pool split into two 30 MiB halves, used on alternate frames through a linear allocator
 * at manager+8 ({base, cur, end, peak}). Each frame the manager starts the allocator on one half:
 *   Begin(alloc = manager+8, base = pool + parity * 0x1e00000, size = 0x1e00000)
 * and bump-allocates particle buffers and draw-indirect arguments from it (about 13 MiB, almost
 * all written by compute shaders). Frame N overwrites what frame N-2 left in the half: the game's
 * own double buffering makes that data dead. The emulator still tracks it as GPU-modified, so the
 * game's first small CPU write to each such page waited for a download through the GPU command
 * thread (~7 ms per fault, a fifth of the time in the test scene).
 *
 * Both callers of Begin with the particle allocator are matched by code, not offset (the call in
 * the frame update and a tail jump in a wrapper), and redirected to a stub that tells the host the
 * half is being reused, then runs Begin unchanged. */
static const unsigned char tlg_particle_call_site[] = {
    0x49,0x8d,0x7c,0x24,0x08,           /* lea rdi, [r12+8] */
    0xba,0x00,0x00,0xe0,0x01,           /* mov edx, 0x1e00000 */
    0x49,0x63,0x44,0x24,0x28,           /* movsxd rax, [r12+0x28] */
    0x48,0x89,0x7d,0x98,                /* mov [rbp-0x68], rdi */
    0x48,0x69,0xf0,0x00,0x00,0xe0,0x01, /* imul rsi, rax, 0x1e00000 */
    0x49,0x03,0x34,0x24,                /* add rsi, [r12] */
    0xe8                                /* call Begin */
};
static const unsigned char tlg_particle_jump_site[] = {
    0x48,0x63,0x47,0x28,                /* movsxd rax, [rdi+0x28] */
    0xba,0x00,0x00,0xe0,0x01,           /* mov edx, 0x1e00000 */
    0x48,0x69,0xf0,0x00,0x00,0xe0,0x01, /* imul rsi, rax, 0x1e00000 */
    0x48,0x03,0x37,                     /* add rsi, [rdi] */
    0x48,0x8d,0x7f,0x08,                /* lea rdi, [rdi+8] */
    0xe9                                /* jmp Begin */
};
/* Begin(alloc, base, size): only when the allocator is idle (cur == 0). */
static const unsigned char tlg_particle_begin[] = {
    0x48,0x83,0x7f,0x08,0x00,           /* cmp qword [rdi+8], 0 */
    0x75,0x11,                          /* jne ret */
    0x48,0x63,0xc2,                     /* movsxd rax, edx */
    0x48,0x89,0x37,                     /* mov [rdi], rsi */
    0x48,0x89,0x77,0x08,                /* mov [rdi+8], rsi */
    0x48,0x01,0xf0,                     /* add rax, rsi */
    0x48,0x89,0x47,0x10,                /* mov [rdi+0x10], rax */
    0xc3                                /* ret */
};

typedef struct {
    uint64_t call_rel;  /* image offset of the call's rel32 */
    uint64_t jump_rel;  /* image offset of the tail jump's rel32 */
    uint64_t begin;     /* image offset of Begin */
} TlgParticleSites;

static inline int64_t tlg_particle_find(const unsigned char *code, uint64_t size,
                                        const unsigned char *pattern, uint64_t length) {
    int64_t found = -1;
    for (uint64_t i = 0; i + length + 4 <= size; ++i) {
        if (code[i] != pattern[0] || memcmp(code + i, pattern, length)) continue;
        if (found >= 0) return -2; /* not unique */
        found = (int64_t)i;
    }
    return found;
}
static inline uint64_t tlg_particle_target(const unsigned char *code, uint64_t rel) {
    int32_t delta;
    memcpy(&delta, code + rel, 4);
    return rel + 4 + (int64_t)delta;
}
/* 1 when both sites exist once in code[0, size) and branch to the expected Begin. */
static inline int tlg_particle_find_sites(const unsigned char *code, uint64_t size, TlgParticleSites *out) {
    const int64_t call = tlg_particle_find(code, size, tlg_particle_call_site, sizeof(tlg_particle_call_site));
    const int64_t jump = tlg_particle_find(code, size, tlg_particle_jump_site, sizeof(tlg_particle_jump_site));
    if (call < 0 || jump < 0) return 0;
    out->call_rel = (uint64_t)call + sizeof(tlg_particle_call_site);
    out->jump_rel = (uint64_t)jump + sizeof(tlg_particle_jump_site);
    out->begin = tlg_particle_target(code, out->call_rel);
    return tlg_particle_target(code, out->jump_rel) == out->begin &&
           out->begin + sizeof(tlg_particle_begin) <= size &&
           !memcmp(code + out->begin, tlg_particle_begin, sizeof(tlg_particle_begin));
}

/* SysV stub entered by call or jump with rdi = allocator, rsi = base, edx = size: keeps the
 * arguments across helper(alloc, base, size), then tail-jumps to Begin. Three pushes keep the
 * helper's stack 16-byte aligned (entry is 8 mod 16, as after any call). */
enum { TLG_PARTICLE_STUB_BYTES = 30 };
static inline void tlg_particle_build_stub(unsigned char code[TLG_PARTICLE_STUB_BYTES], uintptr_t helper,
                                           uintptr_t begin) {
    static const unsigned char pattern[TLG_PARTICLE_STUB_BYTES] = {
        0x57, 0x56, 0x52,                   /* push rdi; push rsi; push rdx */
        0x48,0xb8, 0,0,0,0,0,0,0,0,         /* movabs rax, helper */
        0xff,0xd0,                          /* call rax */
        0x5a, 0x5e, 0x5f,                   /* pop rdx; pop rsi; pop rdi */
        0x48,0xb8, 0,0,0,0,0,0,0,0,         /* movabs rax, begin */
        0xff,0xe0                           /* jmp rax */
    };
    memcpy(code, pattern, sizeof(pattern));
    memcpy(code + 5, &helper, 8);
    memcpy(code + 20, &begin, 8);
}
#endif
