/*
 * ps3recomp - Vulkan command recording on a second thread (PS3RECOMP_VK_THREADED, default
 * on; =0 records on the RSX thread as before).
 *
 * Included once by rsx_vulkan_backend.c, after its entry-point pointers. With
 * the switch on (and draw batching on, the default), every vkCmd* the backend
 * records, and vkBegin/End/ResetCommandBuffer, goes through a thunk that copies
 * the call and its arrays into a single-producer ring; a worker thread makes the
 * real driver call. All of the backend's own logic (state, caches, arenas,
 * descriptor sets, image layouts) stays on the RSX thread, in the same order,
 * so the only thing that moves is the driver's recording cost.
 *
 * Ordering rules:
 *  - vkQueueSubmit drains the ring first (vk_tq_sync), so a submission, and
 *    everything a submission orders -- fences, readbacks, labels, flips --
 *    sees exactly the commands recorded before it.
 *  - The present (postfx, ImGui overlay, RenderDoc) records through its own
 *    entry points: vk_present_window drains the ring and records directly
 *    (s_tq.direct) until it returns.
 *  - Host-side object lifetimes are unchanged: the backend already keeps
 *    anything a recorded command names alive until its batch completed, which
 *    is after the submit, which is after the drain. Every vkDestroy* (and
 *    vkResetDescriptorPool) drains first anyway, a no-op on an empty ring.
 *  - vkUpdateDescriptorSets is queued too (a set is written once, before its
 *    first bind; the ring keeps that order), so the driver's descriptor work
 *    moves with the binds.
 *
 * Idea after Ryujinx's threaded renderer (MIT); own implementation, at the
 * vkCmd level rather than at the renderer interface.
 */
#ifndef PS3RECOMP_VK_THREADED_REC_H
#define PS3RECOMP_VK_THREADED_REC_H

#include "rsx_split.h"          /* (rsx-par2) rsx_diag_*: PS3_STALL_MS sees the worker */
#ifdef _WIN32
#include <intrin.h>
#pragma comment(lib, "synchronization.lib")   /* WaitOnAddress / WakeByAddressSingle */

#define TQ_RING_BYTES (16u << 20)    /* allocated with 4 KB of slack: a wrap filler's header may pass the end */

enum {
    TQ_WRAP = 0, TQ_BARRIER, TQ_CLEARC, TQ_CLEARDS, TQ_CI2B, TQ_CB2I, TQ_BEGINRP, TQ_ENDRP,
    TQ_BINDPIPE, TQ_BINDVB, TQ_BINDDS, TQ_PUSHC, TQ_VIEWPORT, TQ_SCISSOR, TQ_DRAW, TQ_SREF,
    TQ_BINDIB, TQ_DRAWIDX, TQ_DBIAS, TQ_BLIT, TQ_COPYIMG, TQ_RESETQ, TQ_TIMESTAMP,
    TQ_BEGINCB, TQ_ENDCB, TQ_RESETCB, TQ_UPDDS, TQ_PRESENT, TQ_BCALL, TQ_NOPS
};

typedef struct { u32 op, size; VkCommandBuffer cb; } tq_hdr;   /* size: whole record, 8-aligned */

static struct {
    int on;                 /* switch read and the thunks installed */
    int running;            /* worker thread alive */
    int direct;             /* the producer records itself (after a drain) */
    u8* ring;
    HANDLE thread;
    u64 wlocal;             /* producer: next write position (published or not) */
    VkResult err; u32 err_op;   /* the worker's first failed Begin/End/Reset */
    u64 n_cmds, n_syncs, n_wakes, n_full;
} s_tq;
static __declspec(align(64)) volatile u64  s_tq_w;     /* published write position */
static __declspec(align(64)) volatile u64  s_tq_r;     /* consumed position (worker) */
static __declspec(align(64)) volatile LONG s_tq_sleep; /* worker is (about to be) asleep */
static __declspec(align(64)) volatile LONG s_tq_bell;  /* rsx3: rung (incremented) to wake the worker in WAKE_KB mode */
static volatile LONG s_tq_quit;
static u64 s_tq_spin_ticks = 500000ull;   /* PS3RECOMP_VK_TQ_SPIN_US (vk_tq_install) */
static u64 s_tq_wake_bytes;               /* PS3RECOMP_VK_TQ_WAKE_KB (vk_tq_install) */
/* PS3RECOMP_VK_PRESENT_ASYNC (rsx3): the frame's present -- postfx, the overlay,
 * the submit and vkQueuePresentKHR -- is one more record in the ring, run by the
 * worker. The worker's own calls through the thunks go straight to the driver
 * (t_tq_worker), and a drain asked for on the worker is a no-op. */
static __declspec(thread) int t_tq_worker;
static int s_tq_nodrain;                                  /* producer: the next drains are not needed */
static u64 s_tq_pres_sent;                                /* producer: presents queued */
static __declspec(align(64)) volatile u64 s_tq_pres_done; /* worker: presents submitted */
typedef struct { tq_hdr h; VkFence fence; VkSemaphore acq;
                 VkImage img; VkImageView view; u32 w, ht; u64 seq; } tq_present_rec;
static void vk_present_exec(const tq_present_rec* r);    /* rsx_vulkan_backend.c */

/* The real entry points (the worker's). */
#define TQ_FNS(X) \
    X(vkCmdPipelineBarrier) X(vkCmdClearColorImage) X(vkCmdClearDepthStencilImage) \
    X(vkCmdCopyImageToBuffer) X(vkCmdCopyBufferToImage) X(vkCmdBeginRenderPass) \
    X(vkCmdEndRenderPass) X(vkCmdBindPipeline) X(vkCmdBindVertexBuffers) \
    X(vkCmdBindDescriptorSets) X(vkCmdPushConstants) X(vkCmdSetViewport) X(vkCmdSetScissor) \
    X(vkCmdDraw) X(vkCmdSetStencilReference) X(vkCmdBindIndexBuffer) X(vkCmdDrawIndexed) \
    X(vkCmdSetDepthBias) X(vkCmdBlitImage) X(vkCmdCopyImage) X(vkCmdResetQueryPool) \
    X(vkCmdWriteTimestamp) X(vkBeginCommandBuffer) X(vkEndCommandBuffer) \
    X(vkResetCommandBuffer) X(vkQueueSubmit) X(vkUpdateDescriptorSets)     X(vkDestroyImage) X(vkDestroyImageView) X(vkDestroyBuffer) X(vkFreeMemory)     X(vkDestroyFramebuffer) X(vkDestroySampler) X(vkDestroyPipeline) X(vkDestroyRenderPass)     X(vkDestroyDescriptorPool) X(vkResetDescriptorPool) X(vkDestroyQueryPool)
#define TQ_DECLARE(name) static PFN_##name r##name;
TQ_FNS(TQ_DECLARE)
#undef TQ_DECLARE

static void tq_exec(const u8* p);

static DWORD WINAPI tq_worker(LPVOID arg)
{
    (void)arg;
    t_tq_worker = 1;
    rsx_diag_thread_here(RSX_DIAG_WORKER);   /* (rsx-par2) PS3_STALL_MS sees this thread */
    u64 r = s_tq_r;
    for (;;) {
        const LONG bell = __atomic_load_n(&s_tq_bell, __ATOMIC_ACQUIRE);   /* before w: a ring after this is seen */
        u64 w = __atomic_load_n(&s_tq_w, __ATOMIC_ACQUIRE);
        if (r != w) {
            do {
                const u8* p = s_tq.ring + (r & (TQ_RING_BYTES - 1u));
                const u32 size = ((const tq_hdr*)p)->size;
                tq_exec(p);
                r += size;
            } while (r != w);
            __atomic_store_n(&s_tq_r, r, __ATOMIC_RELEASE);
            if (!s_tq_wake_bytes) continue;               /* the old way: chase the producer */
            /* rsx3: no chasing -- go on only for a full burst or a ring that came meanwhile */
            if (__atomic_load_n(&s_tq_w, __ATOMIC_ACQUIRE) - r >= s_tq_wake_bytes ||
                __atomic_load_n(&s_tq_bell, __ATOMIC_ACQUIRE) != bell) continue;
        }
        if (s_tq_quit) break;
        if (s_tq_wake_bytes) {                            /* sleep on the bell (or 2 ms) */
            InterlockedExchange(&s_tq_sleep, 1);
            LONG seen = bell;
            if (__atomic_load_n(&s_tq_bell, __ATOMIC_ACQUIRE) == bell && !s_tq_quit &&
                __atomic_load_n(&s_tq_w, __ATOMIC_ACQUIRE) - r < s_tq_wake_bytes)
                WaitOnAddress((volatile VOID*)&s_tq_bell, &seen, sizeof seen, 2);
            InterlockedExchange(&s_tq_sleep, 0);
            continue;
        }
        /* idle: spin a little (PS3RECOMP_VK_TQ_SPIN_US), then sleep until the
         * producer wakes it -- which it does only once PS3RECOMP_VK_TQ_WAKE_KB of
         * records are waiting, at a present, a drain or a full ring (tq_wake). */
        const u64 t0 = __rdtsc();
        int got = 0;
        while (__rdtsc() - t0 < s_tq_spin_ticks) {
            for (int k = 0; k < 16; k++) _mm_pause();
            if (__atomic_load_n(&s_tq_w, __ATOMIC_ACQUIRE) != r || s_tq_quit) { got = 1; break; }
        }
        if (got) continue;
        InterlockedExchange(&s_tq_sleep, 1);
        u64 seen = r;
        if (__atomic_load_n(&s_tq_w, __ATOMIC_ACQUIRE) == r && !s_tq_quit)
            WaitOnAddress((volatile VOID*)&s_tq_w, &seen, sizeof seen, 2);
        InterlockedExchange(&s_tq_sleep, 0);
    }
    return 0;
}

/* The producer's wake (rsx3, lead + spu3: the worker's ~150 us idle spin after
 * every burst kept it spinning the whole frame -- ~0.9 of a core taken from the
 * SPU workers and the main thread). A sleeping worker is woken only once
 * s_tq_wake_bytes of records wait (it then runs them as one burst), at a present
 * (tq_wake_force), a drain (vk_tq_sync) or a full ring; anything below the
 * threshold is picked up by its 2 ms wait timeout at the latest -- nothing waits
 * on it until the next submit, and a submit drains. PS3RECOMP_VK_TQ_WAKE_KB=0 and
 * PS3RECOMP_VK_TQ_SPIN_US=143 = the old behaviour (wake on every record). */
/* Wake the worker now, whichever way it sleeps (the bell is a full barrier). */
static void tq_ring(void)
{
    InterlockedIncrement(&s_tq_bell);
    s_tq.n_wakes++;
    WakeByAddressSingle((PVOID)&s_tq_bell);
    WakeByAddressSingle((PVOID)&s_tq_w);
}
static void tq_wake(void)
{
    if (!s_tq_sleep) return;
    if (!s_tq_wake_bytes) { s_tq.n_wakes++; WakeByAddressSingle((PVOID)&s_tq_w); return; }
    if (s_tq.wlocal - __atomic_load_n(&s_tq_r, __ATOMIC_ACQUIRE) >= s_tq_wake_bytes) tq_ring();
}
static void tq_wake_force(void)
{
    if (s_tq_wake_bytes) { tq_ring(); return; }
    MemoryBarrier();                   /* the published position before the sleep flag is read */
    if (s_tq_sleep) { s_tq.n_wakes++; WakeByAddressSingle((PVOID)&s_tq_w); }
}

/* (rsx5, round 3) The producer's time waiting for the worker since the last
 * HITCH line (tqw=): a worker stuck in the driver (a present, a destroy) shows
 * here, on whatever RSX-thread call had to drain. */
static unsigned long long s_tqw_ns;

/* Make every recorded call reach the driver before returning. */
/* (rsx-par2) until the worker has run everything published when called: r
 * reaching w OR PASSING it. It used to wait for r == w, which only the producer
 * itself can rely on (nothing is published while it waits); a call from any
 * other thread (PS3RECOMP_VK_BT's image_load from the texture-pack preload
 * thread) saw r jump past w while the producer kept recording, and spun for
 * ever -- holding the pack entry the engine thread was waiting for: the hang of
 * cv-B1-fight (2026-11-10). r and w only grow, so >= is right for everyone. */
static void vk_tq_sync(void)
{
    if (!s_tq.running || t_tq_worker || s_tq_nodrain) return;
    const u64 w = __atomic_load_n(&s_tq_w, __ATOMIC_ACQUIRE);
    if (__atomic_load_n(&s_tq_r, __ATOMIC_ACQUIRE) >= w) return;
    s_tq.n_syncs++;
    const u64 tw0 = ps3_ft_now();
    rsx_diag_wait("vk_tq_sync: the recording worker runs what was recorded");
    tq_ring();
    u32 spins = 0;
    while (__atomic_load_n(&s_tq_r, __ATOMIC_ACQUIRE) < w) {
        _mm_pause();
        if (++spins > 20000u) { SwitchToThread(); spins = 0; tq_ring(); }
    }
    rsx_diag_wait(NULL);
    s_tqw_ns += ps3_ft_now() - tw0;
    if (s_tq.err != VK_SUCCESS) {
        fprintf(stderr, "[RSX vulkan] threaded recording: op %u failed (VkResult %d)\n",
                s_tq.err_op, (int)s_tq.err);
        s_tq.err = VK_SUCCESS;
    }
}

/* Producer: until the worker has submitted present number `seq` (and every
 * record before it). */
static void vk_tq_present_wait(u64 seq)
{
    if (!s_tq.running || __atomic_load_n(&s_tq_pres_done, __ATOMIC_ACQUIRE) >= seq) return;
    const u64 tw0 = ps3_ft_now();
    rsx_diag_wait("vk_tq_present_wait: the worker submits the previous present");
    tq_ring();
    u32 spins = 0;
    while (__atomic_load_n(&s_tq_pres_done, __ATOMIC_ACQUIRE) < seq) {
        _mm_pause();
        if (++spins > 20000u) { SwitchToThread(); spins = 0; tq_ring(); }
    }
    rsx_diag_wait(NULL);
    s_tqw_ns += ps3_ft_now() - tw0;
}

/* Room for one record of `bytes` (header included); NULL: record it directly. */
static u8* tq_alloc(u32 op, u32 bytes, VkCommandBuffer cb)
{
    if (!s_tq.running || s_tq.direct || t_tq_worker) return NULL;
    bytes = (bytes + 7u) & ~7u;
    if (bytes > TQ_RING_BYTES / 8u) { vk_tq_sync(); return NULL; }
    u64 pos = s_tq.wlocal;
    const u32 off = (u32)(pos & (TQ_RING_BYTES - 1u));
    const u32 fill = (off + bytes > TQ_RING_BYTES) ? TQ_RING_BYTES - off : 0u;
    if (pos + fill + bytes - __atomic_load_n(&s_tq_r, __ATOMIC_ACQUIRE) > TQ_RING_BYTES) {
        s_tq.n_full++;
        rsx_lim_event(RSX_LIM_VK_TQ_RING, 0, "recording ring full: the RSX thread waits for the recording worker");   /* (rsx5) */
        const u64 tw0 = ps3_ft_now();
        rsx_diag_wait("tq_alloc: recording ring full");
        tq_ring();
        while (pos + fill + bytes - __atomic_load_n(&s_tq_r, __ATOMIC_ACQUIRE) > TQ_RING_BYTES) _mm_pause();
        rsx_diag_wait(NULL);
        s_tqw_ns += ps3_ft_now() - tw0;
    }
    if (fill) {
        tq_hdr* f = (tq_hdr*)(s_tq.ring + off);
        f->op = TQ_WRAP; f->size = fill; f->cb = VK_NULL_HANDLE;
        pos += fill;
    }
    tq_hdr* h = (tq_hdr*)(s_tq.ring + (pos & (TQ_RING_BYTES - 1u)));
    h->op = op; h->size = bytes; h->cb = cb;
    s_tq.wlocal = pos;
    return (u8*)h;
}
static void tq_commit(const u8* rec)
{
    s_tq.wlocal += ((const tq_hdr*)rec)->size;
    s_tq.n_cmds++;
    __atomic_store_n(&s_tq_w, s_tq.wlocal, __ATOMIC_RELEASE);
    tq_wake();
}

/* Records: the header, then the call's scalars, then its arrays. */
#define TQ_ARR(T, n) ((u32)sizeof(T) * (u32)(n))
typedef struct { tq_hdr h; VkPipelineStageFlags src, dst; VkDependencyFlags dep; u32 nm, nb, ni; } tq_barrier;
typedef struct { tq_hdr h; VkImage img; VkImageLayout lay; u32 n; union { VkClearColorValue c; VkClearDepthStencilValue ds; } v; } tq_clear;
typedef struct { tq_hdr h; VkImage img; VkImageLayout lay; VkBuffer buf; u32 n; } tq_bicopy;
typedef struct { tq_hdr h; VkRenderPassBeginInfo bi; VkSubpassContents contents; } tq_beginrp;
typedef struct { tq_hdr h; VkPipelineBindPoint bp; VkPipeline pipe; } tq_bindpipe;
typedef struct { tq_hdr h; u32 first, n; } tq_bindvb;   /* VkBuffer[n], VkDeviceSize[n] */
typedef struct { tq_hdr h; VkPipelineBindPoint bp; VkPipelineLayout lay; u32 first, n, ndyn; } tq_bindds;
typedef struct { tq_hdr h; VkPipelineLayout lay; VkShaderStageFlags st; u32 off, size; } tq_pushc;
typedef struct { tq_hdr h; u32 first, n; } tq_vpsc;
typedef struct { tq_hdr h; u32 a, b, c, d; int32_t e; } tq_draw;
typedef struct { tq_hdr h; VkBuffer buf; VkDeviceSize off; VkIndexType type; } tq_bindib;
typedef struct { tq_hdr h; float c, cl, s; } tq_dbias;
typedef struct { tq_hdr h; VkImage src; VkImageLayout sl; VkImage dst; VkImageLayout dl; u32 n; VkFilter f; } tq_imgcopy;
typedef struct { tq_hdr h; VkQueryPool pool; u32 a, b; VkPipelineStageFlagBits st; } tq_query;
typedef struct { tq_hdr h; VkCommandBufferBeginInfo bi; } tq_begincb;
typedef struct { tq_hdr h; VkDevice dev; u32 nw; } tq_updds;   /* VkWriteDescriptorSet[nw], then each one's infos */
/* (rsx-par) PS3RECOMP_VK_BT (vk_threaded_be.h): one backend entry point the
 * worker runs itself, in ring order; its arguments follow (8-aligned). */
typedef struct { tq_hdr h; void (*run)(const u8* args); } tq_bcall;
static int tq_ds_image(VkDescriptorType t)
{
    return t == VK_DESCRIPTOR_TYPE_SAMPLER || t == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ||
           t == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE || t == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ||
           t == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
}
static int tq_ds_buffer(VkDescriptorType t)
{
    return t == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || t == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER ||
           t == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC || t == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
}

static void tq_exec(const u8* p)
{
    const tq_hdr* h = (const tq_hdr*)p;
    VkCommandBuffer cb = h->cb;
    switch (h->op) {
    case TQ_WRAP: break;
    case TQ_BARRIER: {
        const tq_barrier* r = (const tq_barrier*)p;
        const VkMemoryBarrier* m = (const VkMemoryBarrier*)(r + 1);
        const VkBufferMemoryBarrier* b = (const VkBufferMemoryBarrier*)(m + r->nm);
        const VkImageMemoryBarrier* i = (const VkImageMemoryBarrier*)(b + r->nb);
        rvkCmdPipelineBarrier(cb, r->src, r->dst, r->dep, r->nm, r->nm ? m : NULL,
                              r->nb, r->nb ? b : NULL, r->ni, r->ni ? i : NULL);
        break; }
    case TQ_CLEARC: { const tq_clear* r = (const tq_clear*)p;
        rvkCmdClearColorImage(cb, r->img, r->lay, &r->v.c, r->n, (const VkImageSubresourceRange*)(r + 1)); break; }
    case TQ_CLEARDS: { const tq_clear* r = (const tq_clear*)p;
        rvkCmdClearDepthStencilImage(cb, r->img, r->lay, &r->v.ds, r->n, (const VkImageSubresourceRange*)(r + 1)); break; }
    case TQ_CI2B: { const tq_bicopy* r = (const tq_bicopy*)p;
        rvkCmdCopyImageToBuffer(cb, r->img, r->lay, r->buf, r->n, (const VkBufferImageCopy*)(r + 1)); break; }
    case TQ_CB2I: { const tq_bicopy* r = (const tq_bicopy*)p;
        rvkCmdCopyBufferToImage(cb, r->buf, r->img, r->lay, r->n, (const VkBufferImageCopy*)(r + 1)); break; }
    case TQ_BEGINRP: { const tq_beginrp* r = (const tq_beginrp*)p;
        VkRenderPassBeginInfo bi = r->bi;
        bi.pClearValues = bi.clearValueCount ? (const VkClearValue*)(r + 1) : NULL;
        rvkCmdBeginRenderPass(cb, &bi, r->contents); break; }
    case TQ_ENDRP: rvkCmdEndRenderPass(cb); break;
    case TQ_BINDPIPE: { const tq_bindpipe* r = (const tq_bindpipe*)p;
        rvkCmdBindPipeline(cb, r->bp, r->pipe); break; }
    case TQ_BINDVB: { const tq_bindvb* r = (const tq_bindvb*)p;
        const VkBuffer* b = (const VkBuffer*)(r + 1);
        rvkCmdBindVertexBuffers(cb, r->first, r->n, b, (const VkDeviceSize*)(b + r->n)); break; }
    case TQ_BINDDS: { const tq_bindds* r = (const tq_bindds*)p;
        const VkDescriptorSet* s = (const VkDescriptorSet*)(r + 1);
        rvkCmdBindDescriptorSets(cb, r->bp, r->lay, r->first, r->n, s, r->ndyn,
                                 r->ndyn ? (const u32*)(s + r->n) : NULL); break; }
    case TQ_PUSHC: { const tq_pushc* r = (const tq_pushc*)p;
        rvkCmdPushConstants(cb, r->lay, r->st, r->off, r->size, r + 1); break; }
    case TQ_VIEWPORT: { const tq_vpsc* r = (const tq_vpsc*)p;
        rvkCmdSetViewport(cb, r->first, r->n, (const VkViewport*)(r + 1)); break; }
    case TQ_SCISSOR: { const tq_vpsc* r = (const tq_vpsc*)p;
        rvkCmdSetScissor(cb, r->first, r->n, (const VkRect2D*)(r + 1)); break; }
    case TQ_DRAW: { const tq_draw* r = (const tq_draw*)p; rvkCmdDraw(cb, r->a, r->b, r->c, r->d); break; }
    case TQ_DRAWIDX: { const tq_draw* r = (const tq_draw*)p;
        rvkCmdDrawIndexed(cb, r->a, r->b, r->c, r->e, r->d); break; }
    case TQ_SREF: { const tq_draw* r = (const tq_draw*)p; rvkCmdSetStencilReference(cb, r->a, r->b); break; }
    case TQ_BINDIB: { const tq_bindib* r = (const tq_bindib*)p; rvkCmdBindIndexBuffer(cb, r->buf, r->off, r->type); break; }
    case TQ_DBIAS: { const tq_dbias* r = (const tq_dbias*)p; rvkCmdSetDepthBias(cb, r->c, r->cl, r->s); break; }
    case TQ_BLIT: { const tq_imgcopy* r = (const tq_imgcopy*)p;
        rvkCmdBlitImage(cb, r->src, r->sl, r->dst, r->dl, r->n, (const VkImageBlit*)(r + 1), r->f); break; }
    case TQ_COPYIMG: { const tq_imgcopy* r = (const tq_imgcopy*)p;
        rvkCmdCopyImage(cb, r->src, r->sl, r->dst, r->dl, r->n, (const VkImageCopy*)(r + 1)); break; }
    case TQ_RESETQ: { const tq_query* r = (const tq_query*)p; rvkCmdResetQueryPool(cb, r->pool, r->a, r->b); break; }
    case TQ_TIMESTAMP: { const tq_query* r = (const tq_query*)p; rvkCmdWriteTimestamp(cb, r->st, r->pool, r->a); break; }
    case TQ_BEGINCB: { const tq_begincb* r = (const tq_begincb*)p;
        VkResult e = rvkBeginCommandBuffer(cb, &r->bi);
        if (e != VK_SUCCESS && s_tq.err == VK_SUCCESS) { s_tq.err = e; s_tq.err_op = h->op; }
        break; }
    case TQ_ENDCB: { VkResult e = rvkEndCommandBuffer(cb);
        if (e != VK_SUCCESS && s_tq.err == VK_SUCCESS) { s_tq.err = e; s_tq.err_op = h->op; }
        break; }
    case TQ_RESETCB: { const tq_draw* r = (const tq_draw*)p;
        VkResult e = rvkResetCommandBuffer(cb, r->a);
        if (e != VK_SUCCESS && s_tq.err == VK_SUCCESS) { s_tq.err = e; s_tq.err_op = h->op; }
        break; }
    case TQ_UPDDS: {                   /* the ring copy is the worker's now: point it at its own infos */
        tq_updds* r = (tq_updds*)(void*)p;
        VkWriteDescriptorSet* w = (VkWriteDescriptorSet*)(r + 1);
        u8* q = (u8*)(w + r->nw);
        for (u32 k = 0; k < r->nw; k++) {
            if (tq_ds_image(w[k].descriptorType)) { w[k].pImageInfo = (const VkDescriptorImageInfo*)q; q += TQ_ARR(VkDescriptorImageInfo, w[k].descriptorCount); }
            else { w[k].pBufferInfo = (const VkDescriptorBufferInfo*)q; q += TQ_ARR(VkDescriptorBufferInfo, w[k].descriptorCount); }
        }
        rvkUpdateDescriptorSets(r->dev, r->nw, w, 0, NULL);
        break; }
    case TQ_BCALL: {                    /* (rsx-par) the backend's own code, on this thread */
        const tq_bcall* r = (const tq_bcall*)p;
        r->run((const u8*)(r + 1));
        break; }
    case TQ_PRESENT: {
        const tq_present_rec* r = (const tq_present_rec*)p;
        vk_present_exec(r);
        __atomic_store_n(&s_tq_pres_done, r->seq, __ATOMIC_RELEASE);
        break; }
    default: break;
    }
}

/* ---- the thunks (the producer's pvkCmd* while the switch is on) ---- */
static void VKAPI_CALL tq_vkCmdPipelineBarrier(VkCommandBuffer cb, VkPipelineStageFlags src, VkPipelineStageFlags dst,
    VkDependencyFlags dep, u32 nm, const VkMemoryBarrier* m, u32 nb, const VkBufferMemoryBarrier* b,
    u32 ni, const VkImageMemoryBarrier* i)
{
    u8* p = tq_alloc(TQ_BARRIER, sizeof(tq_barrier) + TQ_ARR(*m, nm) + TQ_ARR(*b, nb) + TQ_ARR(*i, ni), cb);
    if (!p) { rvkCmdPipelineBarrier(cb, src, dst, dep, nm, m, nb, b, ni, i); return; }
    tq_barrier* r = (tq_barrier*)p;
    r->src = src; r->dst = dst; r->dep = dep; r->nm = nm; r->nb = nb; r->ni = ni;
    u8* q = (u8*)(r + 1);
    if (nm) { memcpy(q, m, TQ_ARR(*m, nm)); q += TQ_ARR(*m, nm); }
    if (nb) { memcpy(q, b, TQ_ARR(*b, nb)); q += TQ_ARR(*b, nb); }
    if (ni) memcpy(q, i, TQ_ARR(*i, ni));
    tq_commit(p);
}
static void VKAPI_CALL tq_vkCmdClearColorImage(VkCommandBuffer cb, VkImage img, VkImageLayout lay,
    const VkClearColorValue* c, u32 n, const VkImageSubresourceRange* rg)
{
    u8* p = tq_alloc(TQ_CLEARC, sizeof(tq_clear) + TQ_ARR(*rg, n), cb);
    if (!p) { rvkCmdClearColorImage(cb, img, lay, c, n, rg); return; }
    tq_clear* r = (tq_clear*)p;
    r->img = img; r->lay = lay; r->n = n; r->v.c = *c;
    memcpy(r + 1, rg, TQ_ARR(*rg, n));
    tq_commit(p);
}
static void VKAPI_CALL tq_vkCmdClearDepthStencilImage(VkCommandBuffer cb, VkImage img, VkImageLayout lay,
    const VkClearDepthStencilValue* v, u32 n, const VkImageSubresourceRange* rg)
{
    u8* p = tq_alloc(TQ_CLEARDS, sizeof(tq_clear) + TQ_ARR(*rg, n), cb);
    if (!p) { rvkCmdClearDepthStencilImage(cb, img, lay, v, n, rg); return; }
    tq_clear* r = (tq_clear*)p;
    r->img = img; r->lay = lay; r->n = n; r->v.ds = *v;
    memcpy(r + 1, rg, TQ_ARR(*rg, n));
    tq_commit(p);
}
static void VKAPI_CALL tq_vkCmdCopyImageToBuffer(VkCommandBuffer cb, VkImage img, VkImageLayout lay,
    VkBuffer buf, u32 n, const VkBufferImageCopy* rg)
{
    u8* p = tq_alloc(TQ_CI2B, sizeof(tq_bicopy) + TQ_ARR(*rg, n), cb);
    if (!p) { rvkCmdCopyImageToBuffer(cb, img, lay, buf, n, rg); return; }
    tq_bicopy* r = (tq_bicopy*)p;
    r->img = img; r->lay = lay; r->buf = buf; r->n = n;
    memcpy(r + 1, rg, TQ_ARR(*rg, n));
    tq_commit(p);
}
static void VKAPI_CALL tq_vkCmdCopyBufferToImage(VkCommandBuffer cb, VkBuffer buf, VkImage img,
    VkImageLayout lay, u32 n, const VkBufferImageCopy* rg)
{
    u8* p = tq_alloc(TQ_CB2I, sizeof(tq_bicopy) + TQ_ARR(*rg, n), cb);
    if (!p) { rvkCmdCopyBufferToImage(cb, buf, img, lay, n, rg); return; }
    tq_bicopy* r = (tq_bicopy*)p;
    r->img = img; r->lay = lay; r->buf = buf; r->n = n;
    memcpy(r + 1, rg, TQ_ARR(*rg, n));
    tq_commit(p);
}
static void VKAPI_CALL tq_vkCmdBeginRenderPass(VkCommandBuffer cb, const VkRenderPassBeginInfo* bi, VkSubpassContents c)
{
    u8* p = bi->pNext ? (vk_tq_sync(), (u8*)NULL)
                      : tq_alloc(TQ_BEGINRP, sizeof(tq_beginrp) + TQ_ARR(VkClearValue, bi->clearValueCount), cb);
    if (!p) { rvkCmdBeginRenderPass(cb, bi, c); return; }
    tq_beginrp* r = (tq_beginrp*)p;
    r->bi = *bi; r->contents = c;
    if (bi->clearValueCount) memcpy(r + 1, bi->pClearValues, TQ_ARR(VkClearValue, bi->clearValueCount));
    tq_commit(p);
}
static void VKAPI_CALL tq_vkCmdEndRenderPass(VkCommandBuffer cb)
{
    u8* p = tq_alloc(TQ_ENDRP, sizeof(tq_hdr), cb);
    if (!p) { rvkCmdEndRenderPass(cb); return; }
    tq_commit(p);
}
static void VKAPI_CALL tq_vkCmdBindPipeline(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipeline pipe)
{
    u8* p = tq_alloc(TQ_BINDPIPE, sizeof(tq_bindpipe), cb);
    if (!p) { rvkCmdBindPipeline(cb, bp, pipe); return; }
    tq_bindpipe* r = (tq_bindpipe*)p; r->bp = bp; r->pipe = pipe;
    tq_commit(p);
}
static void VKAPI_CALL tq_vkCmdBindVertexBuffers(VkCommandBuffer cb, u32 first, u32 n, const VkBuffer* b, const VkDeviceSize* o)
{
    u8* p = tq_alloc(TQ_BINDVB, sizeof(tq_bindvb) + TQ_ARR(*b, n) + TQ_ARR(*o, n), cb);
    if (!p) { rvkCmdBindVertexBuffers(cb, first, n, b, o); return; }
    tq_bindvb* r = (tq_bindvb*)p; r->first = first; r->n = n;
    memcpy(r + 1, b, TQ_ARR(*b, n));
    memcpy((u8*)(r + 1) + TQ_ARR(*b, n), o, TQ_ARR(*o, n));
    tq_commit(p);
}
static void VKAPI_CALL tq_vkCmdBindDescriptorSets(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipelineLayout lay,
    u32 first, u32 n, const VkDescriptorSet* s, u32 ndyn, const u32* dyn)
{
    u8* p = tq_alloc(TQ_BINDDS, sizeof(tq_bindds) + TQ_ARR(*s, n) + TQ_ARR(u32, ndyn), cb);
    if (!p) { rvkCmdBindDescriptorSets(cb, bp, lay, first, n, s, ndyn, dyn); return; }
    tq_bindds* r = (tq_bindds*)p; r->bp = bp; r->lay = lay; r->first = first; r->n = n; r->ndyn = ndyn;
    memcpy(r + 1, s, TQ_ARR(*s, n));
    if (ndyn) memcpy((u8*)(r + 1) + TQ_ARR(*s, n), dyn, TQ_ARR(u32, ndyn));
    tq_commit(p);
}
static void VKAPI_CALL tq_vkCmdPushConstants(VkCommandBuffer cb, VkPipelineLayout lay, VkShaderStageFlags st,
    u32 off, u32 size, const void* data)
{
    u8* p = tq_alloc(TQ_PUSHC, sizeof(tq_pushc) + size, cb);
    if (!p) { rvkCmdPushConstants(cb, lay, st, off, size, data); return; }
    tq_pushc* r = (tq_pushc*)p; r->lay = lay; r->st = st; r->off = off; r->size = size;
    memcpy(r + 1, data, size);
    tq_commit(p);
}
static void VKAPI_CALL tq_vkCmdSetViewport(VkCommandBuffer cb, u32 first, u32 n, const VkViewport* v)
{
    u8* p = tq_alloc(TQ_VIEWPORT, sizeof(tq_vpsc) + TQ_ARR(*v, n), cb);
    if (!p) { rvkCmdSetViewport(cb, first, n, v); return; }
    tq_vpsc* r = (tq_vpsc*)p; r->first = first; r->n = n;
    memcpy(r + 1, v, TQ_ARR(*v, n));
    tq_commit(p);
}
static void VKAPI_CALL tq_vkCmdSetScissor(VkCommandBuffer cb, u32 first, u32 n, const VkRect2D* s)
{
    u8* p = tq_alloc(TQ_SCISSOR, sizeof(tq_vpsc) + TQ_ARR(*s, n), cb);
    if (!p) { rvkCmdSetScissor(cb, first, n, s); return; }
    tq_vpsc* r = (tq_vpsc*)p; r->first = first; r->n = n;
    memcpy(r + 1, s, TQ_ARR(*s, n));
    tq_commit(p);
}
static void VKAPI_CALL tq_vkCmdDraw(VkCommandBuffer cb, u32 vc, u32 ic, u32 fv, u32 fi)
{
    u8* p = tq_alloc(TQ_DRAW, sizeof(tq_draw), cb);
    if (!p) { rvkCmdDraw(cb, vc, ic, fv, fi); return; }
    tq_draw* r = (tq_draw*)p; r->a = vc; r->b = ic; r->c = fv; r->d = fi;
    tq_commit(p);
}
static void VKAPI_CALL tq_vkCmdDrawIndexed(VkCommandBuffer cb, u32 n, u32 inst, u32 first, int32_t vo, u32 fi)
{
    u8* p = tq_alloc(TQ_DRAWIDX, sizeof(tq_draw), cb);
    if (!p) { rvkCmdDrawIndexed(cb, n, inst, first, vo, fi); return; }
    tq_draw* r = (tq_draw*)p; r->a = n; r->b = inst; r->c = first; r->e = vo; r->d = fi;
    tq_commit(p);
}
static void VKAPI_CALL tq_vkCmdSetStencilReference(VkCommandBuffer cb, VkStencilFaceFlags f, u32 ref)
{
    u8* p = tq_alloc(TQ_SREF, sizeof(tq_draw), cb);
    if (!p) { rvkCmdSetStencilReference(cb, f, ref); return; }
    tq_draw* r = (tq_draw*)p; r->a = f; r->b = ref;
    tq_commit(p);
}
static void VKAPI_CALL tq_vkCmdBindIndexBuffer(VkCommandBuffer cb, VkBuffer buf, VkDeviceSize off, VkIndexType t)
{
    u8* p = tq_alloc(TQ_BINDIB, sizeof(tq_bindib), cb);
    if (!p) { rvkCmdBindIndexBuffer(cb, buf, off, t); return; }
    tq_bindib* r = (tq_bindib*)p; r->buf = buf; r->off = off; r->type = t;
    tq_commit(p);
}
static void VKAPI_CALL tq_vkCmdSetDepthBias(VkCommandBuffer cb, float c, float cl, float s)
{
    u8* p = tq_alloc(TQ_DBIAS, sizeof(tq_dbias), cb);
    if (!p) { rvkCmdSetDepthBias(cb, c, cl, s); return; }
    tq_dbias* r = (tq_dbias*)p; r->c = c; r->cl = cl; r->s = s;
    tq_commit(p);
}
static void VKAPI_CALL tq_vkCmdBlitImage(VkCommandBuffer cb, VkImage src, VkImageLayout sl, VkImage dst,
    VkImageLayout dl, u32 n, const VkImageBlit* rg, VkFilter f)
{
    u8* p = tq_alloc(TQ_BLIT, sizeof(tq_imgcopy) + TQ_ARR(*rg, n), cb);
    if (!p) { rvkCmdBlitImage(cb, src, sl, dst, dl, n, rg, f); return; }
    tq_imgcopy* r = (tq_imgcopy*)p; r->src = src; r->sl = sl; r->dst = dst; r->dl = dl; r->n = n; r->f = f;
    memcpy(r + 1, rg, TQ_ARR(*rg, n));
    tq_commit(p);
}
static void VKAPI_CALL tq_vkCmdCopyImage(VkCommandBuffer cb, VkImage src, VkImageLayout sl, VkImage dst,
    VkImageLayout dl, u32 n, const VkImageCopy* rg)
{
    u8* p = tq_alloc(TQ_COPYIMG, sizeof(tq_imgcopy) + TQ_ARR(*rg, n), cb);
    if (!p) { rvkCmdCopyImage(cb, src, sl, dst, dl, n, rg); return; }
    tq_imgcopy* r = (tq_imgcopy*)p; r->src = src; r->sl = sl; r->dst = dst; r->dl = dl; r->n = n; r->f = VK_FILTER_NEAREST;
    memcpy(r + 1, rg, TQ_ARR(*rg, n));
    tq_commit(p);
}
static void VKAPI_CALL tq_vkCmdResetQueryPool(VkCommandBuffer cb, VkQueryPool pool, u32 first, u32 n)
{
    u8* p = tq_alloc(TQ_RESETQ, sizeof(tq_query), cb);
    if (!p) { rvkCmdResetQueryPool(cb, pool, first, n); return; }
    tq_query* r = (tq_query*)p; r->pool = pool; r->a = first; r->b = n;
    tq_commit(p);
}
static void VKAPI_CALL tq_vkCmdWriteTimestamp(VkCommandBuffer cb, VkPipelineStageFlagBits st, VkQueryPool pool, u32 q)
{
    u8* p = tq_alloc(TQ_TIMESTAMP, sizeof(tq_query), cb);
    if (!p) { rvkCmdWriteTimestamp(cb, st, pool, q); return; }
    tq_query* r = (tq_query*)p; r->pool = pool; r->a = q; r->st = st;
    tq_commit(p);
}
static VkResult VKAPI_CALL tq_vkBeginCommandBuffer(VkCommandBuffer cb, const VkCommandBufferBeginInfo* bi)
{
    u8* p = (bi->pNext || bi->pInheritanceInfo) ? (vk_tq_sync(), (u8*)NULL) : tq_alloc(TQ_BEGINCB, sizeof(tq_begincb), cb);
    if (!p) return rvkBeginCommandBuffer(cb, bi);
    ((tq_begincb*)p)->bi = *bi;
    tq_commit(p);
    return VK_SUCCESS;                 /* a failure is reported at the next drain */
}
static VkResult VKAPI_CALL tq_vkEndCommandBuffer(VkCommandBuffer cb)
{
    u8* p = tq_alloc(TQ_ENDCB, sizeof(tq_hdr), cb);
    if (!p) return rvkEndCommandBuffer(cb);
    tq_commit(p);
    return VK_SUCCESS;
}
static VkResult VKAPI_CALL tq_vkResetCommandBuffer(VkCommandBuffer cb, VkCommandBufferResetFlags f)
{
    u8* p = tq_alloc(TQ_RESETCB, sizeof(tq_draw), cb);
    if (!p) return rvkResetCommandBuffer(cb, f);
    ((tq_draw*)p)->a = f;
    tq_commit(p);
    return VK_SUCCESS;
}
/* Host-side, but ordered with the binds that follow it: a set is written once
 * before its first bind, so writing it on the worker, ahead of that bind in the
 * ring, is the same order. Texel buffers, pNext chains and copies: directly. */
static void VKAPI_CALL tq_vkUpdateDescriptorSets(VkDevice dev, u32 nw, const VkWriteDescriptorSet* w,
                                                 u32 nc, const VkCopyDescriptorSet* c)
{
    u32 bytes = sizeof(tq_updds) + TQ_ARR(VkWriteDescriptorSet, nw);
    int ok = nc == 0;
    for (u32 k = 0; k < nw && ok; k++) {
        if (w[k].pNext) ok = 0;
        else if (tq_ds_image(w[k].descriptorType) && w[k].pImageInfo) bytes += TQ_ARR(VkDescriptorImageInfo, w[k].descriptorCount);
        else if (tq_ds_buffer(w[k].descriptorType) && w[k].pBufferInfo) bytes += TQ_ARR(VkDescriptorBufferInfo, w[k].descriptorCount);
        else ok = 0;
    }
    u8* p = NULL;
    if (ok) p = tq_alloc(TQ_UPDDS, bytes, VK_NULL_HANDLE);
    else if (s_tq.running && !s_tq.direct) vk_tq_sync();   /* the worker must not be behind a direct write */
    if (!p) { rvkUpdateDescriptorSets(dev, nw, w, nc, c); return; }
    tq_updds* r = (tq_updds*)(void*)p;
    r->dev = dev; r->nw = nw;
    VkWriteDescriptorSet* o = (VkWriteDescriptorSet*)(r + 1);
    memcpy(o, w, TQ_ARR(VkWriteDescriptorSet, nw));
    u8* q = (u8*)(o + nw);
    for (u32 k = 0; k < nw; k++) {
        if (tq_ds_image(w[k].descriptorType)) { memcpy(q, w[k].pImageInfo, TQ_ARR(VkDescriptorImageInfo, w[k].descriptorCount)); q += TQ_ARR(VkDescriptorImageInfo, w[k].descriptorCount); }
        else { memcpy(q, w[k].pBufferInfo, TQ_ARR(VkDescriptorBufferInfo, w[k].descriptorCount)); q += TQ_ARR(VkDescriptorBufferInfo, w[k].descriptorCount); }
        o[k].pImageInfo = NULL; o[k].pBufferInfo = NULL;
    }
    tq_commit(p);
}
/* Destroying (or resetting) anything a queued call may still name drains the
 * ring first; a no-op when it is empty, which is the steady state (dead objects
 * are destroyed after the batch's submit). */
static void VKAPI_CALL tq_vkDestroyImage(VkDevice d, VkImage o, const VkAllocationCallbacks* a) { vk_tq_sync(); rvkDestroyImage(d, o, a); }
static void VKAPI_CALL tq_vkDestroyImageView(VkDevice d, VkImageView o, const VkAllocationCallbacks* a) { vk_tq_sync(); rvkDestroyImageView(d, o, a); }
static void VKAPI_CALL tq_vkDestroyBuffer(VkDevice d, VkBuffer o, const VkAllocationCallbacks* a) { vk_tq_sync(); rvkDestroyBuffer(d, o, a); }
static void VKAPI_CALL tq_vkFreeMemory(VkDevice d, VkDeviceMemory o, const VkAllocationCallbacks* a) { vk_tq_sync(); rvkFreeMemory(d, o, a); }
static void VKAPI_CALL tq_vkDestroyFramebuffer(VkDevice d, VkFramebuffer o, const VkAllocationCallbacks* a) { vk_tq_sync(); rvkDestroyFramebuffer(d, o, a); }
static void VKAPI_CALL tq_vkDestroySampler(VkDevice d, VkSampler o, const VkAllocationCallbacks* a) { vk_tq_sync(); rvkDestroySampler(d, o, a); }
static void VKAPI_CALL tq_vkDestroyPipeline(VkDevice d, VkPipeline o, const VkAllocationCallbacks* a) { vk_tq_sync(); rvkDestroyPipeline(d, o, a); }
static void VKAPI_CALL tq_vkDestroyRenderPass(VkDevice d, VkRenderPass o, const VkAllocationCallbacks* a) { vk_tq_sync(); rvkDestroyRenderPass(d, o, a); }
static void VKAPI_CALL tq_vkDestroyDescriptorPool(VkDevice d, VkDescriptorPool o, const VkAllocationCallbacks* a) { vk_tq_sync(); rvkDestroyDescriptorPool(d, o, a); }
static VkResult VKAPI_CALL tq_vkResetDescriptorPool(VkDevice d, VkDescriptorPool o, VkDescriptorPoolResetFlags f) { vk_tq_sync(); return rvkResetDescriptorPool(d, o, f); }
static void VKAPI_CALL tq_vkDestroyQueryPool(VkDevice d, VkQueryPool o, const VkAllocationCallbacks* a) { vk_tq_sync(); rvkDestroyQueryPool(d, o, a); }
static VkResult VKAPI_CALL tq_vkQueueSubmit(VkQueue q, u32 n, const VkSubmitInfo* si, VkFence f)
{
    vk_tq_sync();                      /* the command buffers are complete before the driver sees them */
    return rvkQueueSubmit(q, n, si, f);
}

/* After the device entry points are loaded: PS3RECOMP_VK_THREADED (default on;
 * =0 off, =2 worker pinned to the E-cores) swaps the
 * thunks in and starts the worker. Needs draw batching (PS3RECOMP_VK_BATCH,
 * default on): unbatched, every operation is its own submit anyway. */
/* (rsx-par) PS3RECOMP_VK_BT (default 1; vk_threaded_be.h): the backend's draw
 * work on this worker too. It then does ~2-4 ms a frame of real work, so by
 * default it chases the producer (no wake threshold) and is not pinned to the
 * E-cores; PS3RECOMP_VK_TQ_WAKE_KB / PS3RECOMP_VK_THREADED=2 still win. */
static int vk_bt_env(void)
{
    static int v = -1;
    if (v < 0) { const char* e = getenv("PS3RECOMP_VK_BT"); v = (e && e[0] && e[0] != '0'); }   /* (next) default OFF (critic: +sync points, slower present); =1 opts in */
    return v;
}
/* (rsx-par2) PS3_STALL_MS: the recording ring, for the [stall-rsx] line */
static int vk_tq_diag(char* buf, int size)
{
    return snprintf(buf, (size_t)size, " tq: w=%llu r=%llu wlocal=%llu sleep=%ld direct=%d presents %llu/%llu;",
                    (unsigned long long)s_tq_w, (unsigned long long)s_tq_r, (unsigned long long)s_tq.wlocal,
                    (long)s_tq_sleep, s_tq.direct, (unsigned long long)s_tq_pres_done, (unsigned long long)s_tq_pres_sent);
}
static void vk_tq_install(void)
{
    const char* e = getenv("PS3RECOMP_VK_THREADED");
    const char* b = getenv("PS3RECOMP_VK_BATCH");
    if ((e && e[0] == '0') || (b && b[0] == '0')) return;   /* default on (2026-11-08) */
    s_tq.ring = (u8*)VirtualAlloc(NULL, TQ_RING_BYTES + 4096u, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!s_tq.ring) return;
#define TQ_SAVE(name) r##name = p##name;
    TQ_FNS(TQ_SAVE)
#undef TQ_SAVE
    s_tq_w = s_tq_r = 0; s_tq.wlocal = 0; s_tq_quit = 0; s_tq.err = VK_SUCCESS;
    { /* rsx3: the worker's idle spin and the producer's wake threshold */
      const char* sp = getenv("PS3RECOMP_VK_TQ_SPIN_US"); const char* wk = getenv("PS3RECOMP_VK_TQ_WAKE_KB");
      const double spin_us = sp ? atof(sp) : 20.0;
      const long wake_kb = wk ? atol(wk) : (vk_bt_env() ? 0L : 64L);
      LARGE_INTEGER f, a, b; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&a);
      const u64 c0 = __rdtsc();
      do { QueryPerformanceCounter(&b); } while ((b.QuadPart - a.QuadPart) * 1000000 < f.QuadPart * 2000);   /* 2 ms */
      const double ticks_per_us = (double)(__rdtsc() - c0) * (double)f.QuadPart / ((double)(b.QuadPart - a.QuadPart) * 1e6);
      s_tq_spin_ticks = (u64)(spin_us * ticks_per_us);
      s_tq_wake_bytes = wake_kb > 0 ? (u64)wake_kb << 10 : 0;
      fprintf(stderr, "[RSX vulkan] recording worker: idle spin %.0f us, wake at %ld KB (PS3RECOMP_VK_TQ_SPIN_US / _WAKE_KB)\n",
              spin_us, wake_kb > 0 ? wake_kb : 0L); }
    s_tq.thread = CreateThread(NULL, 0, tq_worker, NULL, 0, NULL);
    if (!s_tq.thread) { VirtualFree(s_tq.ring, 0, MEM_RELEASE); s_tq.ring = NULL; return; }
    SetThreadPriority(s_tq.thread, GetThreadPriority(GetCurrentThread()));
    /* =2 (rsx3: the default; =1 = on, unpinned): the worker on the E-cores (hybrid CPUs). Its real
     * driver work is ~1-2 ms a frame; on a P-core its idle spin shared a core with the hot threads. */
    if ((!e && !vk_bt_env()) || (e && atoi(e) == 2)) {
        ULONG len = 0;
        GetSystemCpuSetInformation(NULL, 0, &len, GetCurrentProcess(), 0);
        u8* buf = len ? (u8*)malloc(len) : NULL;
        ULONG ids[64]; ULONG n = 0; BYTE lo = 255, hi = 0;
        if (buf && GetSystemCpuSetInformation((PSYSTEM_CPU_SET_INFORMATION)buf, len, &len, GetCurrentProcess(), 0)) {
            for (int pass = 0; pass < 2; pass++)
                for (ULONG o = 0; o < len; ) {
                    const SYSTEM_CPU_SET_INFORMATION* c = (const SYSTEM_CPU_SET_INFORMATION*)(buf + o);
                    if (c->Type == CpuSetInformation) {
                        const BYTE ec = c->CpuSet.EfficiencyClass;
                        if (!pass) { if (ec < lo) lo = ec; if (ec > hi) hi = ec; }
                        else if (ec == lo && lo != hi && n < 64) ids[n++] = c->CpuSet.Id;
                    }
                    o += c->Size;
                }
        }
        if (n && SetThreadSelectedCpuSets(s_tq.thread, ids, n))
            fprintf(stderr, "[RSX vulkan] threaded recording: worker on %lu E-core CPU sets\n", (unsigned long)n);
        free(buf);
    }
    { typedef HRESULT (WINAPI *set_desc_fn)(HANDLE, PCWSTR);
      set_desc_fn sd = (set_desc_fn)(void*)GetProcAddress(GetModuleHandleA("kernel32.dll"), "SetThreadDescription");
      if (sd) sd(s_tq.thread, L"rsx_vk_record"); }
#define TQ_SWAP(name) p##name = tq_##name;
    TQ_FNS(TQ_SWAP)
#undef TQ_SWAP
    s_tq.running = 1;
    g_rsx_diag_backend = vk_tq_diag;          /* (rsx-par2) */
    s_tq.on = 1;
    fprintf(stderr, "[RSX vulkan] threaded command recording on (PS3RECOMP_VK_THREADED=0 = off, %u MB ring; worker hosttid=%lu, producer hosttid=%lu)\n",
            TQ_RING_BYTES >> 20, (unsigned long)GetThreadId(s_tq.thread), (unsigned long)GetCurrentThreadId());
}

/* Drain, stop the worker and put the real entry points back. */
static void vk_tq_stop(void)
{
    if (!s_tq.running) return;
    vk_tq_sync();
    InterlockedExchange(&s_tq_quit, 1);
    tq_ring();
    WaitForSingleObject(s_tq.thread, 5000);
    CloseHandle(s_tq.thread);
    s_tq.running = 0;
#define TQ_BACK(name) p##name = r##name;
    TQ_FNS(TQ_BACK)
#undef TQ_BACK
    fprintf(stderr, "[RSX vulkan] threaded recording: %llu calls, %llu drains, %llu wakes, %llu ring-full waits\n",
            (unsigned long long)s_tq.n_cmds, (unsigned long long)s_tq.n_syncs,
            (unsigned long long)s_tq.n_wakes, (unsigned long long)s_tq.n_full);
}

#else   /* not Windows: the switch does not exist */
static void vk_tq_install(void) {}
static void vk_tq_stop(void) {}
static void vk_tq_sync(void) {}
static struct { int on, running, direct; } s_tq;
static unsigned long long s_tqw_ns;
#define VK_TQ_NO_ASYNC_PRESENT 1
#endif

#endif /* PS3RECOMP_VK_THREADED_REC_H */
