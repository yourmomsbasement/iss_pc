/* Linux rwlocks behind PS4 pointer-to-handle ABI. A hashed registry validates handles and
   protects lifetime; each thread tracks the rwlocks it holds itself, so the common paths take
   no global lock: acquire looks the handle up under one shard's read lock, unlock and nested
   reads only look at the calling thread's own list. Waiting happens outside the registry. */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>
#include <limits.h>
typedef struct Rwlock {
    pthread_rwlock_t native;
    /* Threads waiting for or holding it (entries in their held lists): destroy fails while nonzero. */
    unsigned users;
    size_t reads, writes, unlocks;
    struct Rwlock *next;
} Rwlock;
typedef struct { int64_t seconds, nanoseconds; } GuestTime;
#define BUCKETS 4096
#define SHARDS 64
static Rwlock *buckets[BUCKETS];
/* Bucket b is guarded by shards[b % SHARDS]; padded so shards do not share cache lines. */
static struct { _Alignas(64) pthread_rwlock_t lock; } shards[SHARDS]={[0 ... SHARDS-1]={PTHREAD_RWLOCK_INITIALIZER}};
static pthread_mutex_t static_init=PTHREAD_MUTEX_INITIALIZER;
static size_t created, reads, writes, unlocks; /* destroyed rwlocks' counts, and creations (atomic) */
static int32_t error(int e) {
    if (!e) return 0;
    unsigned code;
    switch(e) {
    case EPERM: code=1; break;
    case EDEADLK: code=11; break;
    case ENOMEM: code=12; break;
    case EBUSY: code=16; break;
    case EINVAL: code=22; break;
    case EAGAIN: code=35; break;
    case ETIMEDOUT: code=60; break;
    default: fprintf(stderr,"STOP: unmapped rwlock error %d\n",e); exit(21);
    }
    return (int32_t)(UINT32_C(0x80020000)|code);
}
static size_t bucket_of(const void *handle) {
    return (size_t)((((uintptr_t)handle>>4)*UINT64_C(0x9E3779B97F4A7C15))>>52) & (BUCKETS-1);
}
static pthread_rwlock_t *shard_of(size_t bucket) { return &shards[bucket%SHARDS].lock; }
/* Requires the bucket's shard lock. Compares addresses only, so any handle value is safe. */
static Rwlock *find_in(size_t bucket, const Rwlock *handle) {
    for (Rwlock *r=buckets[bucket]; r; r=r->next) if (r==handle) return r;
    return NULL;
}
static int create(Rwlock **out) {
    Rwlock *r=calloc(1,sizeof(*r));
    if (!r) return ENOMEM;
    int e=pthread_rwlock_init(&r->native,NULL);
    if (e) { free(r); return e; }
    size_t b=bucket_of(r);
    pthread_rwlock_wrlock(shard_of(b));
    r->next=buckets[b]; buckets[b]=r;
    pthread_rwlock_unlock(shard_of(b));
    __atomic_fetch_add(&created,1,__ATOMIC_RELAXED);
    *out=r; return 0;
}
/* ---- The calling thread's held rwlocks (an entry also exists while it waits for one) ---- */
typedef struct { Rwlock *lock; unsigned readers, writer; } Held;
#define HELD_INLINE 8
/* A thread that once held more than HELD_INLINE rwlocks at a time keeps its grown list. */
static _Thread_local struct { Held *items; unsigned count, capacity; Held inline_items[HELD_INLINE]; } held;
static Held *held_find(const Rwlock *r) {
    Held *items=held.items ? held.items : held.inline_items;
    for (unsigned i=0;i<held.count;++i) if (items[i].lock==r) return &items[i];
    return NULL;
}
/* Requires the registry shard of r (r cannot be destroyed while it is being counted). */
static Held *held_add(Rwlock *r) {
    if (!held.items) { held.items=held.inline_items; held.capacity=HELD_INLINE; }
    if (held.count==held.capacity) {
        unsigned capacity=held.capacity*2;
        Held *grown=malloc(capacity*sizeof(*grown));
        if (!grown) return NULL;
        memcpy(grown,held.items,held.count*sizeof(*grown));
        if (held.items!=held.inline_items) free(held.items);
        held.items=grown; held.capacity=capacity;
    }
    __atomic_fetch_add(&r->users,1,__ATOMIC_RELAXED);
    Held *h=&held.items[held.count++];
    *h=(Held){.lock=r};
    return h;
}
/* Removes an entry that holds nothing; the release is the thread's last access to the rwlock. */
static void held_prune(Held *h) {
    if (h->readers || h->writer) return;
    Rwlock *r=h->lock;
    *h=held.items[--held.count];
    __atomic_fetch_sub(&r->users,1,__ATOMIC_RELEASE);
}
/* PS4 rwlock attributes only carry a reader/writer preference and pshared; host rwlocks keep their
 * default policy, so an attribute object is a tagged allocation that rw_init validates and ignores. */
#define RWATTR_MAGIC UINT64_C(0x5257415454520001)
static ABI int32_t rwattr_init(uint64_t **attr) {
    if (!attr) return error(EINVAL);
    uint64_t *a=malloc(2*sizeof(uint64_t));
    if (!a) return error(ENOMEM);
    a[0]=RWATTR_MAGIC; a[1]=0; *attr=a; return 0;
}
static ABI int32_t rwattr_destroy(uint64_t **attr) {
    if (!attr || !*attr || (*attr)[0]!=RWATTR_MAGIC) return error(EINVAL);
    (*attr)[0]=0; free(*attr); *attr=NULL; return 0;
}
static ABI int32_t rw_init(Rwlock **out, void *const *attr, const char *name) {
    (void)name;
    if (!out || (attr && !*attr)) return error(EINVAL);
    if (attr && *(const uint64_t *)*attr!=RWATTR_MAGIC) {
        fputs("STOP: unknown PS4 rwlock attribute object\n",stderr); exit(21);
    }
    /* POSIX allows uninitialized storage, so never read *out during init. */
    return error(create(out));
}
/* A null handle is statically initialized: created once, even under contention. */
static int ensure_created(Rwlock **handle) {
    if (__atomic_load_n(handle,__ATOMIC_ACQUIRE)) return 0;
    pthread_mutex_lock(&static_init);
    int e=0;
    if (!*handle) {
        Rwlock *r=NULL;
        e=create(&r);
        if (!e) __atomic_store_n(handle,r,__ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&static_init);
    return e;
}
enum Operation { READ, WRITE, TRY_READ, TRY_WRITE, TIMED_READ, TIMED_WRITE };
static int32_t acquire(Rwlock **handle, enum Operation op, const GuestTime *time) {
    if (!handle) return error(EINVAL);
    int writer=op==WRITE || op==TRY_WRITE || op==TIMED_WRITE;
    int attempt=op==TRY_READ || op==TRY_WRITE;
    int timed=op==TIMED_READ || op==TIMED_WRITE;
    int e=ensure_created(handle);
    if (e) return error(e);
    Rwlock *r=__atomic_load_n(handle,__ATOMIC_ACQUIRE);
    /* Already an entry: the rwlock is valid and stays alive while this thread is counted. */
    Held *h=held_find(r);
    if (h && (h->writer || (writer && h->readers))) return error(attempt ? EBUSY : EDEADLK);
    if (h && h->readers==UINT_MAX) return error(EAGAIN);
    if (!h) {
        size_t b=bucket_of(r);
        pthread_rwlock_rdlock(shard_of(b));
        if (!find_in(b,r)) e=EINVAL;
        else if (!(h=held_add(r))) e=ENOMEM;
        pthread_rwlock_unlock(shard_of(b));
        if (e) return error(e);
    }
    /* Timed operations first try the lock, matching POSIX timeout validation. */
    if (attempt || timed) e=writer ? pthread_rwlock_trywrlock(&r->native) : pthread_rwlock_tryrdlock(&r->native);
    else e=writer ? pthread_rwlock_wrlock(&r->native) : pthread_rwlock_rdlock(&r->native);
    if (timed && e==EBUSY) {
        if (!time || time->nanoseconds<0 || time->nanoseconds>=1000000000) e=EINVAL;
        else {
            struct timespec deadline={.tv_sec=(time_t)time->seconds,.tv_nsec=(long)time->nanoseconds};
            e=writer ? pthread_rwlock_timedwrlock(&r->native,&deadline) : pthread_rwlock_timedrdlock(&r->native,&deadline);
        }
    }
    if (!e) {
        if (writer) { h->writer=1; __atomic_fetch_add(&r->writes,1,__ATOMIC_RELAXED); }
        else { ++h->readers; __atomic_fetch_add(&r->reads,1,__ATOMIC_RELAXED); }
    }
    held_prune(h);
    return error(e);
}
static ABI int32_t rw_read(Rwlock **r) { return acquire(r,READ,NULL); }
static ABI int32_t rw_write(Rwlock **r) { return acquire(r,WRITE,NULL); }
static ABI int32_t rw_tryread(Rwlock **r) { return acquire(r,TRY_READ,NULL); }
static ABI int32_t rw_trywrite(Rwlock **r) { return acquire(r,TRY_WRITE,NULL); }
static ABI int32_t rw_timedread(Rwlock **r,const GuestTime *t) { return acquire(r,TIMED_READ,t); }
static ABI int32_t rw_timedwrite(Rwlock **r,const GuestTime *t) { return acquire(r,TIMED_WRITE,t); }
static ABI int32_t rw_unlock(Rwlock **handle) {
    if (!handle) return error(EINVAL);
    Rwlock *r=__atomic_load_n(handle,__ATOMIC_ACQUIRE);
    Held *h=held_find(r);
    if (!h) {
        /* Not held by this thread: a valid rwlock is EPERM, anything else EINVAL. */
        size_t b=bucket_of(r);
        pthread_rwlock_rdlock(shard_of(b));
        int valid=r && find_in(b,r);
        pthread_rwlock_unlock(shard_of(b));
        return error(valid ? EPERM : EINVAL);
    }
    int e=pthread_rwlock_unlock(&r->native);
    if (!e) {
        if (h->writer) h->writer=0; else --h->readers;
        __atomic_fetch_add(&r->unlocks,1,__ATOMIC_RELAXED);
    }
    held_prune(h);
    return error(e);
}
static ABI int32_t rw_destroy(Rwlock **handle) {
    if (!handle) return error(EINVAL);
    pthread_mutex_lock(&static_init); /* not while a static initialization of it is in progress */
    Rwlock *r=*handle;
    if (!r) { pthread_mutex_unlock(&static_init); return 0; }
    size_t b=bucket_of(r);
    pthread_rwlock_wrlock(shard_of(b));
    int e=0;
    if (!find_in(b,r)) e=EINVAL;
    else if (__atomic_load_n(&r->users,__ATOMIC_ACQUIRE)) e=EBUSY;
    else if (!(e=pthread_rwlock_destroy(&r->native))) {
        Rwlock **link=&buckets[b];
        while (*link!=r) link=&(*link)->next;
        *link=r->next;
        __atomic_fetch_add(&reads,r->reads,__ATOMIC_RELAXED);
        __atomic_fetch_add(&writes,r->writes,__ATOMIC_RELAXED);
        __atomic_fetch_add(&unlocks,r->unlocks,__ATOMIC_RELAXED);
        free(r); *handle=(Rwlock *)(uintptr_t)1;
    }
    pthread_rwlock_unlock(shard_of(b));
    pthread_mutex_unlock(&static_init);
    return error(e);
}
uintptr_t runtime_rwlock_resolve(const char *name) {
    if (!strcmp(name,"6ULAa0fq4jA#libkernel")) return (uintptr_t)rw_init;
    if (!strcmp(name,"BB+kb08Tl9A#libkernel")) return (uintptr_t)rw_destroy;
    if (!strcmp(name,"Ox9i0c7L5w0#libkernel")) return (uintptr_t)rw_read;
    if (!strcmp(name,"mqdNorrB+gI#libkernel")) return (uintptr_t)rw_write;
    if (!strcmp(name,"XD3mDeybCnk#libkernel")) return (uintptr_t)rw_tryread;
    if (!strcmp(name,"bIHoZCTomsI#libkernel")) return (uintptr_t)rw_trywrite;
    if (!strcmp(name,"+L98PIbGttk#libkernel")) return (uintptr_t)rw_unlock;
    if (!strcmp(name,"iPtZRWICjrM#libkernel")) return (uintptr_t)rw_timedread;
    if (!strcmp(name,"adh--6nIqTk#libkernel")) return (uintptr_t)rw_timedwrite;
    if (!strcmp(name,"yOfGg-I1ZII#libkernel")) return (uintptr_t)rwattr_init;
    if (!strcmp(name,"i2ifZ3fS2fo#libkernel")) return (uintptr_t)rwattr_destroy;
    return 0;
}
void runtime_rwlock_report(void) {
    size_t r=__atomic_load_n(&reads,__ATOMIC_RELAXED), w=__atomic_load_n(&writes,__ATOMIC_RELAXED);
    size_t u=__atomic_load_n(&unlocks,__ATOMIC_RELAXED);
    for (size_t b=0;b<BUCKETS;++b) {
        pthread_rwlock_rdlock(shard_of(b));
        for (const Rwlock *l=buckets[b]; l; l=l->next) {
            r+=__atomic_load_n(&l->reads,__ATOMIC_RELAXED); w+=__atomic_load_n(&l->writes,__ATOMIC_RELAXED);
            u+=__atomic_load_n(&l->unlocks,__ATOMIC_RELAXED);
        }
        pthread_rwlock_unlock(shard_of(b));
    }
    printf("Runtime: rwlocks created=%zu, reads=%zu, writes=%zu, unlocks=%zu\n",
           __atomic_load_n(&created,__ATOMIC_RELAXED),r,w,u);
}
