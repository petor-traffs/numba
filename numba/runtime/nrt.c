#include <stdarg.h>
#include <string.h> /* for memset */
#include "nrt.h"
#include "assert.h"

#if !defined MIN
#define MIN(a, b) ((a) < (b)) ? (a) : (b)
#endif


typedef int (*atomic_meminfo_cas_func)(void **ptr, void *cmp,
                                       void *repl, void **oldptr);

struct MemInfo{
    size_t         refct;
    dtor_function  dtor;
    void          *dtor_info;
    void          *data;
    size_t         size;    /* only used for NRT allocated memory */
};


struct MemSys{
    /* Atomic increment and decrement function */
    atomic_inc_dec_func atomic_inc, atomic_dec;
    /* Atomic CAS */
    atomic_meminfo_cas_func atomic_cas;
    /* Shutdown flag */
    int shutting;
    /* Stats */
    size_t stats_alloc, stats_free, stats_mi_alloc, stats_mi_free;

};

/* The Memory System object */
static MemSys TheMSys;

static
void nrt_meminfo_call_dtor(MemInfo *mi) {
    NRT_Debug(nrt_debug_print("nrt_meminfo_call_dtor %p\n", mi));
    /* call dtor */
    if (mi->dtor)
        mi->dtor(mi->data, mi->dtor_info);
    /* Clear and release MemInfo */
    NRT_MemInfo_destroy(mi);
}

void NRT_MemSys_init(void) {
    memset(&TheMSys, 0, sizeof(MemSys));
}

void NRT_MemSys_shutdown(void) {
    TheMSys.shutting = 1;
    /* Revert to use our non-atomic stub for all atomic operations
       because the JIT-ed version will be removed.
       Since we are at interpreter shutdown,
       it cannot be running multiple threads anymore. */
    NRT_MemSys_set_atomic_inc_dec_stub();
    NRT_MemSys_set_atomic_cas_stub();
}

void NRT_MemSys_set_atomic_inc_dec(atomic_inc_dec_func inc,
                                   atomic_inc_dec_func dec)
{
    TheMSys.atomic_inc = inc;
    TheMSys.atomic_dec = dec;
}

void NRT_MemSys_set_atomic_cas(atomic_cas_func cas) {
    TheMSys.atomic_cas = (atomic_meminfo_cas_func)cas;
}

size_t NRT_MemSys_get_stats_alloc() {
    return TheMSys.stats_alloc;
}

size_t NRT_MemSys_get_stats_free() {
    return TheMSys.stats_free;
}

size_t NRT_MemSys_get_stats_mi_alloc() {
    return TheMSys.stats_mi_alloc;
}

size_t NRT_MemSys_get_stats_mi_free() {
    return TheMSys.stats_mi_free;
}

static
size_t nrt_testing_atomic_inc(size_t *ptr){
    /* non atomic */
    size_t out = *ptr;
    out += 1;
    *ptr = out;
    return out;
}

static
size_t nrt_testing_atomic_dec(size_t *ptr){
    /* non atomic */
    size_t out = *ptr;
    out -= 1;
    *ptr = out;
    return out;
}

static
int nrt_testing_atomic_cas(void* volatile *ptr, void *cmp, void *val,
                           void * *oldptr){
    /* non atomic */
    void *old = *ptr;
    *oldptr = old;
    if (old == cmp) {
        *ptr = val;
         return 1;
    }
    return 0;

}

void NRT_MemSys_set_atomic_inc_dec_stub(void){
    NRT_MemSys_set_atomic_inc_dec(nrt_testing_atomic_inc,
                                  nrt_testing_atomic_dec);
}

void NRT_MemSys_set_atomic_cas_stub(void) {
    NRT_MemSys_set_atomic_cas(nrt_testing_atomic_cas);
}

void NRT_MemInfo_init(MemInfo *mi,void *data, size_t size, dtor_function dtor,
                      void *dtor_info)
{
    mi->refct = 1;  /* starts with 1 refct */
    mi->dtor = dtor;
    mi->dtor_info = dtor_info;
    mi->data = data;
    mi->size = size;
    /* Update stats */
    TheMSys.atomic_inc(&TheMSys.stats_mi_alloc);
}

MemInfo* NRT_MemInfo_new(void *data, size_t size, dtor_function dtor,
                         void *dtor_info)
{
    MemInfo * mi = NRT_Allocate(sizeof(MemInfo));
    NRT_MemInfo_init(mi, data, size, dtor, dtor_info);
    return mi;
}

size_t NRT_MemInfo_refcount(MemInfo *mi) {
    /* Should never returns 0 for a valid MemInfo */
    if (mi && mi->data)
        return mi->refct;
    else{
        return (size_t)-1;
    }
}

static
void nrt_internal_dtor_safe(void *ptr, void *info) {
    size_t size = (size_t) info;
    NRT_Debug(nrt_debug_print("nrt_internal_dtor_safe %p, %p\n", ptr, info));
    /* See NRT_MemInfo_alloc_safe() */
    memset(ptr, 0xDE, MIN(size, 256));
}

static
void *nrt_allocate_meminfo_and_data(size_t size, MemInfo **mi_out) {
    MemInfo *mi;
    char *base = NRT_Allocate(sizeof(MemInfo) + size);
    mi = (MemInfo*)base;
    *mi_out = mi;
    return base + sizeof(MemInfo);
}

MemInfo* NRT_MemInfo_alloc(size_t size) {
    MemInfo *mi;
    void *data = nrt_allocate_meminfo_and_data(size, &mi);
    NRT_Debug(nrt_debug_print("NRT_MemInfo_alloc %p\n", data));
    NRT_MemInfo_init(mi, data, size, NULL, NULL);
    return mi;
}

MemInfo* NRT_MemInfo_alloc_safe(size_t size) {
    MemInfo *mi;
    void *data = nrt_allocate_meminfo_and_data(size, &mi);
    /* Only fill up a couple cachelines with debug markers, to minimize
       overhead. */
    memset(data, 0xCB, MIN(size, 256));
    NRT_Debug(nrt_debug_print("NRT_MemInfo_alloc_safe %p %zu\n", data, size));
    NRT_MemInfo_init(mi, data, size, nrt_internal_dtor_safe, (void*)size);
    return mi;
}

static
void* nrt_allocate_meminfo_and_data_align(size_t size, unsigned align,
                                         MemInfo **mi)
{
    size_t offset, intptr, remainder;
    char *base = nrt_allocate_meminfo_and_data(size + 2 * align, mi);
    intptr = (size_t) base;
    /* See if we are aligned */
    remainder = intptr % align;
    if (remainder == 0){ /* Yes */
        offset = 0;
    } else { /* No, move forward `offset` bytes */
        offset = align - remainder;
    }
    return base + offset;
}

MemInfo* NRT_MemInfo_alloc_aligned(size_t size, unsigned align) {
    MemInfo *mi;
    void *data = nrt_allocate_meminfo_and_data_align(size, align, &mi);
    NRT_Debug(nrt_debug_print("NRT_MemInfo_alloc_aligned %p\n", data));
    NRT_MemInfo_init(mi, data, size, NULL, NULL);
    return mi;
}

MemInfo* NRT_MemInfo_alloc_safe_aligned(size_t size, unsigned align) {
    MemInfo *mi;
    void *data = nrt_allocate_meminfo_and_data_align(size, align, &mi);
    /* Only fill up a couple cachelines with debug markers, to minimize
       overhead. */
    memset(data, 0xCB, MIN(size, 256));
    NRT_Debug(nrt_debug_print("NRT_MemInfo_alloc_safe_aligned %p %zu\n",
                              data, size));
    NRT_MemInfo_init(mi, data, size, nrt_internal_dtor_safe, (void*)size);
    return mi;
}

void NRT_MemInfo_destroy(MemInfo *mi) {
    NRT_Free(mi);
    TheMSys.atomic_inc(&TheMSys.stats_mi_free);
}

void NRT_MemInfo_acquire(MemInfo *mi) {
    NRT_Debug(nrt_debug_print("NRT_acquire %p refct=%zu\n", mi,
                                                            mi->refct));
    assert(mi->refct > 0 && "RefCt cannot be zero");
    TheMSys.atomic_inc(&mi->refct);
}

void NRT_MemInfo_call_dtor(MemInfo *mi) {
    /* We have a destructor */
    nrt_meminfo_call_dtor(mi);
}

void NRT_MemInfo_release(MemInfo *mi) {
    NRT_Debug(nrt_debug_print("NRT_release %p refct=%zu\n", mi,
                                                            mi->refct));
    assert (mi->refct > 0 && "RefCt cannot be 0");
    /* RefCt drop to zero */
    if (TheMSys.atomic_dec(&mi->refct) == 0) {
        NRT_MemInfo_call_dtor(mi);
    }
}

void* NRT_MemInfo_data(MemInfo* mi) {
    return mi->data;
}

size_t NRT_MemInfo_size(MemInfo* mi) {
    return mi->size;
}


void NRT_MemInfo_dump(MemInfo *mi, FILE *out) {
    fprintf(out, "MemInfo %p refcount %zu\n", mi, mi->refct);
}

void* NRT_Allocate(size_t size) {
    void *ptr = malloc(size);
    NRT_Debug(nrt_debug_print("NRT_Allocate bytes=%llu ptr=%p\n", size, ptr));
    TheMSys.atomic_inc(&TheMSys.stats_alloc);
    return ptr;
}

void NRT_Free(void *ptr) {
    NRT_Debug(nrt_debug_print("NRT_Free %p\n", ptr));
    free(ptr);
    TheMSys.atomic_inc(&TheMSys.stats_free);
}

