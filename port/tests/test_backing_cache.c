/* runtime_memory.c backing copies (runtime_memory_read/write_backing): repeated copies from one
 * region come from the per-thread region cache without the table lock, a map change invalidates
 * the cache (unmapped -> 0, remapped -> the new physical pages), disabled-optimization bit 1
 * turns the cache off, and copies racing map changes stay correct. The table lock is held
 * directly (no generation change) to tell a lock-free copy from one that waits for it. */
#include "../src/runtime_memory.c"
#include <stdarg.h>

uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *nid) {
    (void)table; (void)count; (void)nid; return 0;
}
void runtime_wait_begin(const char *kind, const void *object, const char *name,
                        const void *const *owner, uintptr_t frame) {
    (void)kind; (void)object; (void)name; (void)owner; (void)frame;
}
void runtime_wait_end(void) {}
int32_t *runtime_errno(void) { static int32_t e; return &e; }
#ifdef _WIN32
void compat_sleep_ns(uint64_t ns) { Sleep((DWORD)(ns/1000000)); }
#endif

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#c); ++failures; } } while (0)

static void nap_ms(unsigned ms) {
#ifdef _WIN32
    Sleep(ms);
#else
    struct timespec t={ms/1000,(long)(ms%1000)*1000000}; nanosleep(&t,NULL);
#endif
}

#define SIZE (4*PAGE)
static unsigned char *at_phys(int64_t phys) { return backing_base+phys; }
static void *map_phys(void *where, int64_t phys, int fixed) {
    void *out=where;
    int32_t r=direct_map(&out,SIZE,3,fixed ? MAP_FIXED_FLAG : 0,phys,PAGE);
    CHECK(r==0);
    return r ? NULL : out;
}

static void test_cache_and_invalidation(void) {
    int64_t pa=-1, pb=-1;
    CHECK(direct_allocate(0,(int64_t)POOL_SIZE,SIZE,0,0,&pa)==0);
    CHECK(direct_allocate(0,(int64_t)POOL_SIZE,SIZE,0,0,&pb)==0);
    unsigned char *x=map_phys(NULL,pa,0);
    if (!x) return;
    memset(at_phys(pa),0x11,SIZE); memset(at_phys(pb),0x22,SIZE);
    unsigned char out[64];
    /* The first copy looks the table up and fills this thread's cache... */
    CHECK(runtime_memory_read_backing((uintptr_t)x+100,out,sizeof out)==1 && out[0]==0x11 && out[63]==0x11);
    /* ...a nested copy under this thread's exclusive lock still works (odd generation: no cache). */
    write_lock();
    CHECK(runtime_memory_read_backing((uintptr_t)x+200,out,sizeof out)==1 && out[0]==0x11);
    write_unlock();
    CHECK(runtime_memory_read_backing((uintptr_t)x+100,out,sizeof out)==1);
    memset(at_phys(pa),0x33,SIZE);
    CHECK(runtime_memory_read_backing((uintptr_t)x,out,sizeof out)==1 && out[0]==0x33);
    /* Writes through a cached entry land in the guest mapping. */
    unsigned char in[64]; memset(in,0x44,sizeof in);
    CHECK(runtime_memory_write_backing((uintptr_t)x+SIZE-64,in,sizeof in)==1);
    CHECK(x[SIZE-64]==0x44 && x[SIZE-1]==0x44);
    /* A copy that runs past the region's end falls back to the table: it is not mapped there. */
    CHECK(runtime_memory_read_backing((uintptr_t)x+SIZE-32,out,64)==0);
    /* Unmapped: the stale entry must not be used. */
    CHECK(direct_unmap(x,SIZE)==0);
    CHECK(runtime_memory_read_backing((uintptr_t)x,out,sizeof out)==0);
    CHECK(runtime_memory_write_backing((uintptr_t)x,in,sizeof in)==0);
    /* Remapped at the same address onto other physical pages: the copy reads the new pages. */
    unsigned char *y=map_phys(x,pb,1);
    CHECK(y==x);
    CHECK(runtime_memory_read_backing((uintptr_t)x,out,sizeof out)==1 && out[0]==0x22);
    CHECK(runtime_memory_read_backing((uintptr_t)x+8,out,sizeof out)==1 && out[0]==0x22);
    CHECK(direct_unmap(x,SIZE)==0);
    CHECK(direct_release((uint64_t)pa,SIZE)==0 && direct_release((uint64_t)pb,SIZE)==0);
}

/* The helper thread warms its own cache, then copies again while the table lock is held. */
typedef struct { uintptr_t address; int write; int warm_ok, result, done; unsigned char buffer[64]; } Probe;
static void *probe_thread(void *arg) {
    Probe *p=arg;
    unsigned char scratch[64];
    p->warm_ok=runtime_memory_read_backing(p->address,scratch,sizeof scratch);
    __atomic_store_n(&p->done,1,__ATOMIC_RELEASE);
    while (__atomic_load_n(&p->done,__ATOMIC_ACQUIRE)!=2) nap_ms(1);
    p->result=p->write ? runtime_memory_write_backing(p->address,p->buffer,sizeof p->buffer)
                       : runtime_memory_read_backing(p->address,p->buffer,sizeof p->buffer);
    __atomic_store_n(&p->done,3,__ATOMIC_RELEASE);
    return NULL;
}
/* 1 when the helper's second copy finished while another thread held the table lock. */
static int lock_free_copy(uintptr_t address, int write, unsigned char fill, unsigned char *buffer) {
    Probe p={address,write,0,0,0,{0}};
    memset(p.buffer,fill,sizeof p.buffer);
    pthread_t t;
    pthread_create(&t,NULL,probe_thread,&p);
    while (__atomic_load_n(&p.done,__ATOMIC_ACQUIRE)!=1) nap_ms(1);
    CHECK(p.warm_ok==1);
    table_wrlock();
    __atomic_store_n(&p.done,2,__ATOMIC_RELEASE);
    for (unsigned i=0;i<500 && __atomic_load_n(&p.done,__ATOMIC_ACQUIRE)!=3;++i) nap_ms(1);
    int lock_free=__atomic_load_n(&p.done,__ATOMIC_ACQUIRE)==3;
    table_wrunlock();
    pthread_join(t,NULL);
    CHECK(p.result==1);
    if (buffer) memcpy(buffer,p.buffer,sizeof p.buffer);
    return lock_free;
}

static void test_lock_free_and_toggle(void) {
    int64_t pa=-1;
    CHECK(direct_allocate(0,(int64_t)POOL_SIZE,SIZE,0,0,&pa)==0);
    unsigned char *x=map_phys(NULL,pa,0);
    if (!x) return;
    memset(at_phys(pa),0x55,SIZE);
    unsigned char out[64];
    CHECK(lock_free_copy((uintptr_t)x+PAGE,0,0,out)==1);
    CHECK(out[0]==0x55 && out[63]==0x55);
    CHECK(lock_free_copy((uintptr_t)x+2*PAGE,1,0x66,NULL)==1);
    CHECK(x[2*PAGE]==0x66 && x[2*PAGE+63]==0x66);
    /* TLG_TOGGLE_FILE bit 1 (region cache) off: every copy goes through the lock again. */
    __atomic_store_n(&runtime_disabled_optimizations,1,__ATOMIC_RELEASE);
    CHECK(lock_free_copy((uintptr_t)x+PAGE,0,0,out)==0);
    CHECK(out[0]==0x55);
    CHECK(lock_free_copy((uintptr_t)x+3*PAGE,1,0x77,NULL)==0);
    CHECK(x[3*PAGE]==0x77);
    __atomic_store_n(&runtime_disabled_optimizations,0,__ATOMIC_RELEASE);
    CHECK(direct_unmap(x,SIZE)==0);
    CHECK(direct_release((uint64_t)pa,SIZE)==0);
}

/* Copies on two threads while a third keeps changing the table (protect and map/unmap): every
 * copy sees its own last write, and the writer's wait for lock-free writes always ends. */
static unsigned char *stable;
static int stop;
static void *copier(void *arg) {
    unsigned slot=(unsigned)(uintptr_t)arg;
    uintptr_t address=(uintptr_t)stable+slot*PAGE;
    unsigned char in[256], out[256];
    for (unsigned i=0;i<200000 && !__atomic_load_n(&stop,__ATOMIC_ACQUIRE);++i) {
        memset(in,(int)(i*7+slot),sizeof in);
        if (!runtime_memory_write_backing(address,in,sizeof in)) { ++failures; break; }
        if (!runtime_memory_read_backing(address,out,sizeof out) || memcmp(in,out,sizeof in)) {
            fprintf(stderr,"FAIL: copier %u iteration %u\n",slot,i); ++failures; break;
        }
    }
    return NULL;
}
static void test_concurrent_map_changes(void) {
    int64_t pa=-1, pb=-1;
    CHECK(direct_allocate(0,(int64_t)POOL_SIZE,SIZE,0,0,&pa)==0);
    CHECK(direct_allocate(0,(int64_t)POOL_SIZE,SIZE,0,0,&pb)==0);
    stable=map_phys(NULL,pa,0);
    unsigned char *scratch=map_phys(NULL,pb,0);
    if (!stable || !scratch) return;
    uint64_t generation=__atomic_load_n(&table_generation,__ATOMIC_ACQUIRE);
    pthread_t t[2];
    for (uintptr_t i=0;i<2;++i) pthread_create(&t[i],NULL,copier,(void *)i);
    for (unsigned i=0;i<2000;++i) {
        CHECK(kernel_mprotect(scratch,SIZE,(i & 1) ? 1 : 3)==0);
        if (!(i % 16)) {
            CHECK(direct_unmap(scratch,SIZE)==0);
            CHECK(map_phys(scratch,pb,1)==scratch);
        }
    }
    __atomic_store_n(&stop,1,__ATOMIC_RELEASE);
    for (int i=0;i<2;++i) pthread_join(t[i],NULL);
    CHECK(__atomic_load_n(&table_generation,__ATOMIC_ACQUIRE)>generation);
    CHECK(__atomic_load_n(&backing_writers,__ATOMIC_ACQUIRE)==0);
    CHECK(direct_unmap(stable,SIZE)==0 && direct_unmap(scratch,SIZE)==0);
    CHECK(direct_release((uint64_t)pa,SIZE)==0 && direct_release((uint64_t)pb,SIZE)==0);
}

int main(void) {
    setvbuf(stdout,NULL,_IONBF,0);
    runtime_memory_reserve();
    test_cache_and_invalidation();
    test_lock_free_and_toggle();
    test_concurrent_map_changes();
    if (failures) { fprintf(stderr,"%d failure(s)\n",failures); return 1; }
    printf("PASS: backing copies through the region cache\n");
    return 0;
}
