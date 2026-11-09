#include <stdlib.h>
#include "../../include/ps3emu/log_first.h"   /* PS3_LOG_FIRST */
/*
 * ps3recomp - Memory management syscalls (implementation)
 */

#include "sys_memory.h"
#include "../../include/ps3emu/ps3_limits.h"   /* PS3RECOMP_LIMITS: runtime gauges */
#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Globals
 * -----------------------------------------------------------------------*/
sys_mem_alloc_info       g_sys_mem_allocs[SYS_MEMORY_ALLOC_MAX];
sys_mem_container_info   g_sys_mem_containers[SYS_MEMORY_CONTAINER_MAX];
sys_mmapper_shared_info  g_sys_mmapper_shared[SYS_MMAPPER_SHARED_MAX];

/* Bump allocator for main memory pool.
 * Starts after a reasonable offset to avoid the low addresses used by ELF. */
uint32_t g_sys_mem_bump_ptr = 0;

/* Total user memory size (default 213 MB, after kernel reservation) */
#define SYS_MEM_USER_TOTAL  (sys_mem_user_total())

/* PS3_USER_MEMORY_MB=<n> (read once; unset = the retail console): the user
 * memory a DEVELOPMENT console reports. A retail PS3 has 256 MB; a debug
 * console / devkit in "debug memory" mode reports more, and some titles size
 * their heaps from it. inFAMOUS 2 does (func_001EFC24 at 0x001EFC8C: total
 * > 0x17FFFFFF sets the byte at 0x00A2C840; func_000DD8AC then allocates its
 * main heap at 0xD400000 = 212 MB instead of 0x7900000 = 121 MB). The
 * sys_memory window grows to match (it is guest address space: 0x40000000
 * up, nothing else lives there). Values below 256 are ignored. */
uint32_t sys_mem_user_mb(void)
{
    static _Atomic uint32_t s_mb = 0;
    if (!s_mb) {
        const char* e = getenv("PS3_USER_MEMORY_MB");
        const unsigned long v = e ? strtoul(e, 0, 10) : 0;
        s_mb = (v >= 256 && v <= 1024) ? (uint32_t)v : 1;   /* 1 = not set */
    }
    return s_mb == 1 ? 0 : s_mb;
}
static uint32_t sys_mem_user_total(void)
{
    const uint32_t mb = sys_mem_user_mb();
    return mb ? (mb - 43u) << 20 : 213u * 1024u * 1024u;   /* the kernel's 43 MB, as on retail */
}

/* Guest window handed out by sys_memory_allocate / sys_mmapper.
 * 0x40000000+ matches where the real lv2 places these allocations (verified
 * against an RPCS3 boot log of Yakuza: Dead Souls); the window is outside
 * the pre-committed main region, so pages are committed on demand. */
/* PS3_MEM_ALLOC_BASE=<hex> overrides the window base (the window stays 256 MB). RPCS3
 * places 1 MB-page user allocations at 0x30000000 for inFamous; matching it makes guest
 * pointers line up with the oracle, so LS/RAM dumps diff without a constant offset. */
static uint32_t sys_mem_alloc_base(void)
{
    static _Atomic uint32_t s_base = 0;
    if (!s_base) {
        const char* e = getenv("PS3_MEM_ALLOC_BASE");
        uint32_t b = e ? (uint32_t)strtoul(e, 0, 16) : 0;
        s_base = (b && !(b & 0xFFFFF)) ? b : 0x40000000u;
    }
    return s_base;
}
#define SYS_MEM_ALLOC_BASE  (sys_mem_alloc_base())
#define SYS_MEM_ALLOC_END   (sys_mem_alloc_base() + (sys_mem_user_mb() > 256u ? sys_mem_user_mb() << 20 : 0x10000000u))

static uint32_t s_total_allocated = 0;

/* Guest threads are real host threads; serialize the bump allocator. */
#ifdef _WIN32
static SRWLOCK s_bump_lock = SRWLOCK_INIT;
static void bump_lock(void)   { AcquireSRWLockExclusive(&s_bump_lock); }
static void bump_unlock(void) { ReleaseSRWLockExclusive(&s_bump_lock); }
#else
#include <pthread.h>
static pthread_mutex_t s_bump_mtx = PTHREAD_MUTEX_INITIALIZER;
static void bump_lock(void)   { pthread_mutex_lock(&s_bump_mtx); }
static void bump_unlock(void) { pthread_mutex_unlock(&s_bump_mtx); }
#endif

/* (limits, 2026-11-09) Freed blocks are handed out again. The window used to be
 * a pure bump allocator: sys_memory_free only dropped the bookkeeping, so the
 * address space (and the committed pages under it) of every freed block was
 * gone for good, and a title that allocates and frees in a loop -- level or
 * district streaming -- runs into the end of the 256 MB window with most of it
 * free, where the console would have given the memory back. A freed range is
 * kept (merged with its neighbours, address-ordered) and the lowest one that
 * fits takes the next allocation, zeroed like a fresh one. PS3_MEM_REUSE=0 =
 * the old bump-only window. Under the bump lock. */
#define MEM_FREE_MAX 256
static struct { uint32_t addr, size; } s_mem_free[MEM_FREE_MAX];
static int      s_mem_free_n;
static uint32_t s_mem_free_bytes;
static ps3_lim  s_lim_mem_inuse  = PS3_LIM_INIT("mem_inuse_kb", 0);
static ps3_lim  s_lim_mem_window = PS3_LIM_INIT("mem_window_kb", 0);
static ps3_lim  s_lim_mem_allocs = PS3_LIM_INIT("mem_allocs", SYS_MEMORY_ALLOC_MAX);
static ps3_lim  s_lim_mem_frees  = PS3_LIM_INIT("mem_frees", 0);
static ps3_lim  s_lim_mem_reused = PS3_LIM_INIT("mem_reused_kb", 0);
static ps3_lim  s_lim_mem_flist  = PS3_LIM_INIT("mem_freelist", MEM_FREE_MAX);
static int mem_reuse_on(void)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("PS3_MEM_REUSE"); on = !(e && e[0] == '0'); }
    return on;
}
static void mem_note_locked(void)
{
    int n = 0;
    for (int i = 0; i < SYS_MEMORY_ALLOC_MAX; i++) n += g_sys_mem_allocs[i].active != 0;
    if (!s_lim_mem_inuse.cap) {
        s_lim_mem_inuse.cap  = (long)(sys_mem_user_total() / 1024u);
        s_lim_mem_window.cap = (long)((SYS_MEM_ALLOC_END - SYS_MEM_ALLOC_BASE) / 1024u);
    }
    ps3_lim_set(&s_lim_mem_allocs, n);
    ps3_lim_set(&s_lim_mem_inuse, (long)(s_total_allocated / 1024u));
    ps3_lim_set(&s_lim_mem_window, g_sys_mem_bump_ptr ?
                (long)((g_sys_mem_bump_ptr - SYS_MEM_ALLOC_BASE - s_mem_free_bytes) / 1024u) : 0);
    ps3_lim_set(&s_lim_mem_flist, s_mem_free_n);
}
static void mem_free_put_locked(uint32_t a, uint32_t sz)
{
    if (!sz) return;
    if (s_mem_free_n == MEM_FREE_MAX) {
        ps3_lim_full(&s_lim_mem_flist, "freed 0x%08X+0x%X not kept (the old leak)", a, sz);
        return;
    }
    int i = s_mem_free_n;
    while (i > 0 && s_mem_free[i - 1].addr > a) { s_mem_free[i] = s_mem_free[i - 1]; i--; }
    s_mem_free[i].addr = a; s_mem_free[i].size = sz;
    s_mem_free_n++;
    s_mem_free_bytes += sz;
    /* merge with the next, then with the previous */
    if (i + 1 < s_mem_free_n && s_mem_free[i].addr + s_mem_free[i].size == s_mem_free[i + 1].addr) {
        s_mem_free[i].size += s_mem_free[i + 1].size;
        memmove(&s_mem_free[i + 1], &s_mem_free[i + 2], (size_t)(s_mem_free_n - i - 2) * sizeof s_mem_free[0]);
        s_mem_free_n--;
    }
    if (i > 0 && s_mem_free[i - 1].addr + s_mem_free[i - 1].size == s_mem_free[i].addr) {
        s_mem_free[i - 1].size += s_mem_free[i].size;
        memmove(&s_mem_free[i], &s_mem_free[i + 1], (size_t)(s_mem_free_n - i - 1) * sizeof s_mem_free[0]);
        s_mem_free_n--;
    }
}
/* The lowest freed range that holds `size` at `align`; 0 = none. */
static uint32_t mem_free_get_locked(uint32_t size, uint32_t align)
{
    for (int i = 0; i < s_mem_free_n; i++) {
        const uint32_t lo = s_mem_free[i].addr, end = lo + s_mem_free[i].size;
        const uint32_t a = VM_ALIGN_UP(lo, align);
        if (a < lo || a + size < a || a + size > end) continue;
        /* take [a, a+size); the head [lo, a) and the tail [a+size, end) go back */
        memmove(&s_mem_free[i], &s_mem_free[i + 1], (size_t)(s_mem_free_n - i - 1) * sizeof s_mem_free[0]);
        s_mem_free_n--;
        s_mem_free_bytes -= end - lo;
        mem_free_put_locked(lo, a - lo);
        mem_free_put_locked(a + size, end - (a + size));
        return a;
    }
    return 0;
}

static void write_be32(uint32_t addr, uint32_t val)
{
    uint32_t* p = (uint32_t*)vm_to_host(addr);
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__ || defined(_WIN32)
    val = ((val >> 24) & 0xFF) | ((val >> 8) & 0xFF00) |
          ((val <<  8) & 0xFF0000) | ((val << 24) & 0xFF000000u);
#endif
    *p = val;
}

/* ---------------------------------------------------------------------------
 * sys_memory_allocate
 *
 * r3 = size
 * r4 = flags (page size)
 * r5 = pointer to receive allocated address (u32*)
 * -----------------------------------------------------------------------*/
int64_t sys_memory_allocate(ppu_context* ctx)
{
    uint32_t size      = LV2_ARG_U32(ctx, 0);
    uint32_t flags     = LV2_ARG_U32(ctx, 1);
    uint32_t addr_out  = LV2_ARG_PTR(ctx, 2);

    fprintf(stderr, "[sys_memory] allocate(size=0x%X, flags=0x%X)\n",
            size, flags);
    /* PS3_ALLOC_BT=1: the guest's saved-LR chain (frame+16 of each back-chain link),
     * same walk as the RPCS3 oracle's "[oracle] sys_memory_allocate backchain". */
    if (getenv("PS3_ALLOC_BT")) {
        extern uint64_t vm_read64(uint64_t a);
        char buf[512]; int p = snprintf(buf, sizeof buf, "lr=0x%x", (uint32_t)ctx->lr);
        uint32_t sp = (uint32_t)ctx->gpr[1];
        for (int i = 0; i < 16 && sp && sp < 0xF0000000u && p < (int)sizeof buf - 12; i++) {
            uint32_t next = (uint32_t)vm_read64(sp);
            if (!next || next < 0x10000u) break;
            p += snprintf(buf + p, sizeof buf - p, " 0x%x", (uint32_t)vm_read64(next + 16));
            sp = next;
        }
        fprintf(stderr, "[sys_memory] allocate backchain: %s\n", buf);
    }

    /* Determine alignment based on page size flags */
    uint32_t alignment;
    if (flags & SYS_MEMORY_PAGE_SIZE_1M) {
        alignment = 0x100000; /* 1 MB */
    } else {
        alignment = 0x10000;  /* 64 KB */
    }

    size = VM_ALIGN_UP(size, alignment);

    if (size == 0)
        return (int64_t)(int32_t)CELL_EINVAL;

    bump_lock();

    /* Initialize bump pointer on first call */
    if (g_sys_mem_bump_ptr == 0)
        g_sys_mem_bump_ptr = SYS_MEM_ALLOC_BASE;

    /* Find a free allocation slot */
    int slot = -1;
    for (int i = 0; i < SYS_MEMORY_ALLOC_MAX; i++) {
        if (!g_sys_mem_allocs[i].active) { slot = i; break; }
    }
    if (slot < 0) {
        ps3_lim_full(&s_lim_mem_allocs, "sys_memory_allocate 0x%X -> ENOMEM", size);
        bump_unlock();
        return (int64_t)(int32_t)CELL_ENOMEM;
    }

    /* A freed range first (PS3_MEM_REUSE) */
    uint32_t alloc_addr = mem_reuse_on() ? mem_free_get_locked(size, alignment) : 0;
    if (alloc_addr) {
        memset(vm_to_host(alloc_addr), 0, size);    /* still committed: zero it as a fresh commit is */
        ps3_lim_add(&s_lim_mem_reused, (long)(size / 1024u));
    } else {
        /* Align bump pointer */
        g_sys_mem_bump_ptr = VM_ALIGN_UP(g_sys_mem_bump_ptr, alignment);

        /* Check if we have room */
        if (g_sys_mem_bump_ptr + size > SYS_MEM_ALLOC_END) {
            ps3_lim_full(&s_lim_mem_window, "sys_memory_allocate 0x%X -> ENOMEM (%u KB in use, %u KB freed)",
                         size, s_total_allocated / 1024u, s_mem_free_bytes / 1024u);
            bump_unlock();
            return (int64_t)(int32_t)CELL_ENOMEM;
        }
        alloc_addr = g_sys_mem_bump_ptr;
        g_sys_mem_bump_ptr += size;
    }
    s_total_allocated += size;

    sys_mem_alloc_info* a = &g_sys_mem_allocs[slot];
    a->active       = 1;       /* claim the slot before unlocking */
    a->addr         = alloc_addr;
    a->size         = size;
    a->container_id = 0;
    a->page_size    = alignment;  /* 0x100000 (1M) or 0x10000 (64K) */
    mem_note_locked();

    bump_unlock();

    /* Commit the pages (window is outside the pre-committed main region);
     * fresh commits are already zeroed by the OS */
    if (vm_commit(alloc_addr, size) != CELL_OK) {
        a->active = 0;
        return (int64_t)(int32_t)CELL_ENOMEM;
    }

    fprintf(stderr, "[sys_memory] allocate -> 0x%08X\n", alloc_addr);

    if (addr_out != 0) {
        write_be32(addr_out, alloc_addr);
    }

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_memory_free
 *
 * r3 = address
 * -----------------------------------------------------------------------*/
int64_t sys_memory_free(ppu_context* ctx)
{
    uint32_t addr = LV2_ARG_U32(ctx, 0);

    bump_lock();
    for (int i = 0; i < SYS_MEMORY_ALLOC_MAX; i++) {
        if (g_sys_mem_allocs[i].active && g_sys_mem_allocs[i].addr == addr) {
            s_total_allocated -= g_sys_mem_allocs[i].size;
            g_sys_mem_allocs[i].active = 0;
            if (mem_reuse_on()) mem_free_put_locked(addr, g_sys_mem_allocs[i].size);
            ps3_lim_add(&s_lim_mem_frees, 1);
            mem_note_locked();
            bump_unlock();
            return CELL_OK;
        }
    }
    bump_unlock();

    return (int64_t)(int32_t)CELL_EINVAL;
}

/* ---------------------------------------------------------------------------
 * sys_memory_get_user_memory_size
 *
 * r3 = pointer to output struct: { u32 total, u32 available }
 * -----------------------------------------------------------------------*/
int64_t sys_memory_get_user_memory_size(ppu_context* ctx)
{
    uint32_t out_addr = LV2_ARG_PTR(ctx, 0);

    fprintf(stderr, "[sys_memory] get_user_memory_size()\n");
    if (out_addr != 0) {
        uint32_t total = SYS_MEM_USER_TOTAL;
        uint32_t avail = total > s_total_allocated ? total - s_total_allocated : 0;
        write_be32(out_addr + 0, total);
        write_be32(out_addr + 4, avail);
    }

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_memory_get_page_attribute (syscall 351 / 0x15F)
 *
 * r3 = address to query
 * r4 = pointer to sys_page_attribute_t:
 *        +0x00 attribute   (u64)
 *        +0x08 access_right(u64)
 *        +0x10 page_size   (u32)   <- the field titles actually check
 *        +0x14 pad         (u32)
 *
 * Was unimplemented (fell through to the return-0 stub), so the output struct
 * was never filled -> callers reading page_size got stack garbage. Dantelion2's
 * graphics init (func_009F6D40) queries a 1MB-page buffer and only proceeds to
 * construct a sub-object when page_size == 0x100000; the stub made it take the
 * failure path, leaving that sub-object NULL -> later null-deref crash.
 * -----------------------------------------------------------------------*/
int64_t sys_memory_get_page_attribute(ppu_context* ctx)
{
    uint32_t addr     = LV2_ARG_U32(ctx, 0);
    uint32_t attr_out = LV2_ARG_PTR(ctx, 1);

    /* Report the page size of the allocation containing addr. */
    uint32_t page_size = 0x10000;  /* default 64K */
    for (int i = 0; i < SYS_MEMORY_ALLOC_MAX; i++) {
        if (g_sys_mem_allocs[i].active &&
            addr >= g_sys_mem_allocs[i].addr &&
            addr <  g_sys_mem_allocs[i].addr + g_sys_mem_allocs[i].size) {
            page_size = g_sys_mem_allocs[i].page_size ? g_sys_mem_allocs[i].page_size : 0x10000;
            break;
        }
    }

    if (attr_out != 0) {
        write_be32(attr_out + 0x00, 0);          /* attribute    (hi) */
        write_be32(attr_out + 0x04, 0x00040000); /* attribute    (lo) PROT_READ_WRITE-ish */
        write_be32(attr_out + 0x08, 0);          /* access_right (hi) */
        write_be32(attr_out + 0x0C, 0x00000008); /* access_right (lo) PPU_THREAD-ish */
        write_be32(attr_out + 0x10, page_size);  /* page_size */
        write_be32(attr_out + 0x14, 0);          /* pad */
    }
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_memory_container_create
 *
 * r3 = pointer to receive container ID (u32*)
 * r4 = size
 * -----------------------------------------------------------------------*/
int64_t sys_memory_container_create(ppu_context* ctx)
{
    uint32_t id_out_addr = LV2_ARG_PTR(ctx, 0);
    uint32_t size        = LV2_ARG_U32(ctx, 1);

    int slot = -1;
    for (int i = 0; i < SYS_MEMORY_CONTAINER_MAX; i++) {
        if (!g_sys_mem_containers[i].active) { slot = i; break; }
    }
    if (slot < 0)
        return (int64_t)(int32_t)CELL_EAGAIN;

    sys_mem_container_info* c = &g_sys_mem_containers[slot];
    c->active     = 1;
    c->total_size = size;
    c->used_size  = 0;

    uint32_t container_id = (uint32_t)(slot + 1);
    if (id_out_addr != 0) {
        write_be32(id_out_addr, container_id);
    }

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_memory_container_destroy
 *
 * r3 = container_id
 * -----------------------------------------------------------------------*/
int64_t sys_memory_container_destroy(ppu_context* ctx)
{
    uint32_t container_id = LV2_ARG_U32(ctx, 0);

    if (container_id == 0 || container_id > SYS_MEMORY_CONTAINER_MAX)
        return (int64_t)(int32_t)CELL_ESRCH;

    sys_mem_container_info* c = &g_sys_mem_containers[container_id - 1];
    if (!c->active)
        return (int64_t)(int32_t)CELL_ESRCH;

    c->active = 0;
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_memory_container_get_size
 *
 * r3 = container_id
 * r4 = pointer to output struct: { u32 total, u32 used }
 * -----------------------------------------------------------------------*/
int64_t sys_memory_container_get_size(ppu_context* ctx)
{
    uint32_t container_id = LV2_ARG_U32(ctx, 0);
    uint32_t out_addr     = LV2_ARG_PTR(ctx, 1);

    if (container_id == 0 || container_id > SYS_MEMORY_CONTAINER_MAX)
        return (int64_t)(int32_t)CELL_ESRCH;

    sys_mem_container_info* c = &g_sys_mem_containers[container_id - 1];
    if (!c->active)
        return (int64_t)(int32_t)CELL_ESRCH;

    if (out_addr != 0) {
        write_be32(out_addr + 0, c->total_size);
        write_be32(out_addr + 4, c->used_size);
    }

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_mmapper_allocate_address
 *
 * r3 = size
 * r4 = flags
 * r5 = alignment
 * r6 = pointer to receive address (u32*)
 * -----------------------------------------------------------------------*/
int64_t sys_mmapper_allocate_address(ppu_context* ctx)
{
    uint32_t size      = LV2_ARG_U32(ctx, 0);
    /* uint32_t flags  = LV2_ARG_U32(ctx, 1); */
    uint32_t alignment = LV2_ARG_U32(ctx, 2);
    uint32_t addr_out  = LV2_ARG_PTR(ctx, 3);

    if (alignment == 0) alignment = 0x10000;
    size = VM_ALIGN_UP(size, alignment);

    bump_lock();

    if (g_sys_mem_bump_ptr == 0)
        g_sys_mem_bump_ptr = SYS_MEM_ALLOC_BASE;

    g_sys_mem_bump_ptr = VM_ALIGN_UP(g_sys_mem_bump_ptr, alignment);

    if (g_sys_mem_bump_ptr + size > SYS_MEM_ALLOC_END) {
        bump_unlock();
        return (int64_t)(int32_t)CELL_ENOMEM;
    }

    uint32_t alloc_addr = g_sys_mem_bump_ptr;
    g_sys_mem_bump_ptr += size;

    bump_unlock();

    /* Commit the reserved region */
    vm_commit(alloc_addr, size);

    if (addr_out != 0) {
        write_be32(addr_out, alloc_addr);
    }

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_mmapper_free_address
 *
 * r3 = address
 * -----------------------------------------------------------------------*/
int64_t sys_mmapper_free_address(ppu_context* ctx)
{
    /* uint32_t addr = LV2_ARG_U32(ctx, 0); */
    (void)ctx;
    /* In our bump allocator we don't actually free, just acknowledge */
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_mmapper_allocate_shared_memory
 *
 * r3 = key
 * r4 = size
 * r5 = flags
 * r6 = pointer to receive shared mem ID (u32*)
 * -----------------------------------------------------------------------*/
int64_t sys_mmapper_allocate_shared_memory(ppu_context* ctx)
{
    uint64_t key      = LV2_ARG_U64(ctx, 0);
    uint32_t size     = LV2_ARG_U32(ctx, 1);
    /* uint32_t flags = LV2_ARG_U32(ctx, 2); */
    uint32_t id_out   = LV2_ARG_PTR(ctx, 3);

    int slot = -1;
    for (int i = 0; i < SYS_MMAPPER_SHARED_MAX; i++) {
        if (!g_sys_mmapper_shared[i].active) { slot = i; break; }
    }
    if (slot < 0)
        return (int64_t)(int32_t)CELL_EAGAIN;

    sys_mmapper_shared_info* s = &g_sys_mmapper_shared[slot];
    s->active = 1;
    s->size   = VM_ALIGN_UP(size, VM_PAGE_SIZE);
    s->key    = key;
    s->addr   = 0;

    uint32_t shm_id = (uint32_t)(slot + 1);
    if (id_out != 0) {
        write_be32(id_out, shm_id);
    }

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_mmapper_allocate_memory (lv2 341)
 *
 * r3 = pointer to receive the allocated address (u32*)
 * r4 = size
 *
 * The ABI here was read off the caller rather than a header. GT5P's PDI device
 * worker calls this the moment it starts -- func_00941808 loads r3 with a
 * pointer and r4 with 0x300000, issues the syscall, retries on any non-zero
 * result, and on success immediately does `lwz r29, 0(r29)`, reading back
 * through the pointer it passed in r3.
 *
 * Answering CELL_OK without writing anything is the worst of both worlds: the
 * worker believes it has three megabytes, reads a null buffer back, and exits
 * on the spot -- which leaves the packed-filesystem device with no worker at
 * all and stalls asset loading. Answering an error is worse still, because the
 * caller retries forever.
 *
 * The flat VM has no distinction between reserving and mapping, so this hands
 * back committed guest memory directly and sys_mmapper_allocate_memory_from_
 * container below accepts it as already mapped.
 * -----------------------------------------------------------------------*/
int64_t sys_mmapper_allocate_memory(ppu_context* ctx)
{
    uint32_t addr_out = LV2_ARG_PTR(ctx, 0);
    uint32_t size     = LV2_ARG_U32(ctx, 1);

    if (size == 0)
        return (int64_t)(int32_t)CELL_EINVAL;

    size = VM_ALIGN_UP(size, VM_PAGE_SIZE);

    bump_lock();
    if (g_sys_mem_bump_ptr == 0)
        g_sys_mem_bump_ptr = SYS_MEM_ALLOC_BASE;
    g_sys_mem_bump_ptr = VM_ALIGN_UP(g_sys_mem_bump_ptr, VM_PAGE_SIZE);
    if (g_sys_mem_bump_ptr + size > SYS_MEM_ALLOC_END) {
        bump_unlock();
        return (int64_t)(int32_t)CELL_ENOMEM;
    }
    uint32_t addr = g_sys_mem_bump_ptr;
    g_sys_mem_bump_ptr += size;
    bump_unlock();

    vm_commit(addr, size);

    if (addr_out != 0)
        write_be32(addr_out, addr);

    { static int n = 0; if (PS3_LOG_FIRST(n, 4))
        fprintf(stderr, "[sys_memory] mmapper_allocate_memory size=0x%X -> 0x%08X\n",
                size, addr); }

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_mmapper_allocate_memory_from_container (lv2 342)
 *
 * The same allocation charged to a container. GT5P calls it straight after 341
 * with the address 341 handed back, so with a flat VM there is nothing further
 * to do -- the memory is already committed and addressable.
 * -----------------------------------------------------------------------*/
int64_t sys_mmapper_allocate_memory_from_container(ppu_context* ctx)
{
    (void)ctx;
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_mmapper_map_shared_memory
 *
 * r3 = address (where to map)
 * r4 = shared mem ID
 * r5 = flags
 * -----------------------------------------------------------------------*/
int64_t sys_mmapper_map_shared_memory(ppu_context* ctx)
{
    uint32_t addr   = LV2_ARG_U32(ctx, 0);
    uint32_t shm_id = LV2_ARG_U32(ctx, 1);
    /* uint32_t flags = LV2_ARG_U32(ctx, 2); */

    if (shm_id == 0 || shm_id > SYS_MMAPPER_SHARED_MAX)
        return (int64_t)(int32_t)CELL_ESRCH;

    sys_mmapper_shared_info* s = &g_sys_mmapper_shared[shm_id - 1];
    if (!s->active)
        return (int64_t)(int32_t)CELL_ESRCH;

    /* Commit the memory at the specified address */
    int32_t rc = vm_commit(addr, s->size);
    if (rc != CELL_OK)
        return (int64_t)(int32_t)rc;

    s->addr = addr;
    memset(vm_to_host(addr), 0, s->size);

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * Registration
 * -----------------------------------------------------------------------*/
void sys_memory_init(lv2_syscall_table* tbl)
{
    memset(g_sys_mem_allocs,     0, sizeof(g_sys_mem_allocs));
    memset(g_sys_mem_containers, 0, sizeof(g_sys_mem_containers));
    memset(g_sys_mmapper_shared, 0, sizeof(g_sys_mmapper_shared));
    g_sys_mem_bump_ptr = 0;
    s_total_allocated  = 0;

    lv2_syscall_register(tbl, SYS_MEMORY_ALLOCATE,             sys_memory_allocate);
    lv2_syscall_register(tbl, SYS_MEMORY_FREE,                 sys_memory_free);
    lv2_syscall_register(tbl, SYS_MEMORY_GET_USER_MEMORY_SIZE, sys_memory_get_user_memory_size);
    lv2_syscall_register(tbl, SYS_MEMORY_GET_PAGE_ATTRIBUTE,   sys_memory_get_page_attribute);
    lv2_syscall_register(tbl, SYS_MEMORY_CONTAINER_CREATE,     sys_memory_container_create);
    lv2_syscall_register(tbl, SYS_MEMORY_CONTAINER_DESTROY,    sys_memory_container_destroy);
    lv2_syscall_register(tbl, SYS_MEMORY_CONTAINER_GET_SIZE,   sys_memory_container_get_size);
    lv2_syscall_register(tbl, SYS_MMAPPER_ALLOCATE_ADDRESS,    sys_mmapper_allocate_address);
    lv2_syscall_register(tbl, SYS_MMAPPER_FREE_ADDRESS,        sys_mmapper_free_address);
    lv2_syscall_register(tbl, SYS_MMAPPER_ALLOCATE_SHARED_MEMORY, sys_mmapper_allocate_shared_memory);
    lv2_syscall_register(tbl, SYS_MMAPPER_MAP_SHARED_MEMORY,  sys_mmapper_map_shared_memory);
    lv2_syscall_register(tbl, 341, sys_mmapper_allocate_memory);
    lv2_syscall_register(tbl, 342, sys_mmapper_allocate_memory_from_container);
}
