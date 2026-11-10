/*
 * ps3recomp - cellGcmSys HLE module implementation
 *
 * Manages RSX initialization, display buffer registration, flip control,
 * command buffer control, tile/zcull configuration, IO memory mapping,
 * report/label areas, and address-to-offset translation.
 *
 * Actual rendering is handled elsewhere -- this module just tracks state.
 */

#include "rsx_live_draw.h"
#include "../../include/ps3emu/log_first.h"   /* PS3_LOG_FIRST */
#include "rsx_draw_engine.h"
#include "cellGcmSys.h"
#include "../../runtime/platform/win32_compat.h"
#include "../../runtime/ppu/ppu_memory.h"   /* vm_write32 (translate + byte-swap, OOB-safe) */
#include "../../runtime/memory/vm.h"    /* VM_HLE_INJECT_BASE */
#include "../../runtime/spu/spu_coherency.h" /* lock-line bookkeeping for the blit row copy */
#include "rsx_commands.h"                    /* rsx_state, rsx_process_command_buffer */
#include "../../include/ps3emu/guest_call.h" /* g_ps3_guest_caller for direct handler dispatch */
#include "../../include/ps3emu/ps3_ftrace.h"  /* PS3_FTRACE timeline */
#include "../../include/ps3emu/ps3_toggles.h" /* GCM_STALL_GAP_MS, live */
#include "gcm_fifo_decode.h"                   /* JUMP forms */
#include "../../runtime/platform/host_place.h" /* PS3_CPU_PLACE: the draining thread's cores */
#include "rsx_split.h"                         /* (rsx-par) PS3RECOMP_RSX_SPLIT */

/* Guest EA of the GCM context (begin/end/current/callback) the title writes its
 * command stream into; recorded by cellGcmSetupContext, drained by the RSX. */
static u32 s_gcm_context_ea = 0;
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#include <unistd.h>   /* getpid: PS3_HANG_SAMPLE */
#endif

/* ---------------------------------------------------------------------------
 * Timestamp helpers (same pattern as sys_timer)
 * -----------------------------------------------------------------------*/

#ifdef _WIN32
static LARGE_INTEGER s_qpc_freq;
static int           s_qpc_init = 0;

static void ensure_qpc_init(void)
{
    if (!s_qpc_init) {
        QueryPerformanceFrequency(&s_qpc_freq);
        s_qpc_init = 1;
    }
}

static u64 get_timestamp_ns(void)
{
    LARGE_INTEGER now;
    ensure_qpc_init();
    QueryPerformanceCounter(&now);
    /* Convert to nanoseconds: (count * 1e9) / freq */
    return (u64)((double)now.QuadPart * 1000000000.0 / (double)s_qpc_freq.QuadPart);
}
#else
static u64 get_timestamp_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec * 1000000000ULL + (u64)ts.tv_nsec;
}
#endif

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

static int  s_gcm_initialized = 0;
static u32  s_flip_mode   = CELL_GCM_DISPLAY_VSYNC;
static u32  s_flip_status = CELL_GCM_FLIP_STATUS_DONE;
/* Flip requests (each puts the status back to WAITING) and the request a
 * pending flip completion was marked for: a completion delivered late must not
 * mark DONE a flip requested after it (see ppu_gcm_pump). */
static volatile u32 s_flip_req_seq, s_flip_tick_seq;
/* The status's read-modify-write transitions (request, tick, completion) run
 * on different threads; without a lock a completion could read "no newer
 * request" just before a request stored WAITING and then overwrite it. */
#ifdef _WIN32
#include <windows.h>
static volatile LONG s_flip_lock;
#define FLIP_LOCK()   do { while (InterlockedCompareExchange(&s_flip_lock, 1, 0)) YieldProcessor(); } while (0)
#define FLIP_UNLOCK() InterlockedExchange(&s_flip_lock, 0)
#else
static volatile int s_flip_lock;
#define FLIP_LOCK()   do { while (__atomic_exchange_n(&s_flip_lock, 1, __ATOMIC_ACQUIRE)) {} } while (0)
#define FLIP_UNLOCK() __atomic_store_n(&s_flip_lock, 0, __ATOMIC_RELEASE)
#endif

/* Count of guest flip requests (SetFlipCommand / SetPrepareFlip). The present
 * loop uses this as the frame boundary: presenting only when a flip arrived
 * keeps partially-drained frames off screen (the ticker otherwise presents on
 * a fixed 16ms clock, and a drain that catches the guest mid-frame -- e.g.
 * while it's blocked in the FIFO-wrap recycle callback -- would show a
 * clear+few-draws frame as a visible flicker). */
static volatile u32 s_flip_request_count = 0;

u32 cellGcm_flip_request_count(void)
{
    return s_flip_request_count;
}
static u32  s_debug_level = CELL_GCM_DEBUG_LEVEL0;

/* Display buffers */
static CellGcmDisplayInfo s_display_buffers[CELL_GCM_MAX_DISPLAY_BUFFER_NUM];
static int s_display_buffer_set[CELL_GCM_MAX_DISPLAY_BUFFER_NUM];
static u32 s_current_display_buffer_id = 0;
/* Set by the flip (guest thread, at a get==put frame boundary); consumed by
 * the ticker, which presents the accumulated batch BEFORE draining further --
 * presenting on a raw flip-count change raced the drain and showed empty or
 * mixed batches (wave: black flashes, layout flicker). */
static volatile int s_flip_pending = 0;
/* A flip _cellGcmSetFlipCommand queued in the FIFO: | bufferId, bit 8 = ours. */
#define GCM_FLIP_MARKER 0xFEAD0100u
/* ponytail: fixed headroom, far above one frame's commands (Simpsons ~10 KB);
 * derive it from the observed per-frame volume if a title outgrows it. */
#define GCM_FLIP_WRAP_BYTES 0x40000u

/* Flips handed to the present (GCM_FLIP_STATS). */
static volatile unsigned s_flips_shown;
int cellGcm_take_flip_pending(void)
{
    int v = s_flip_pending;
    s_flip_pending = 0;
    if (v) s_flips_shown++;
    return v;
}


/* Configuration */
static CellGcmConfig s_config;
/* Offset pages (1MB) known to be LOCAL-memory-derived (set by
 * cellGcmAddressToOffset when the guest converts a localAddress-range EA). */
static u8 s_local_offset_page[1024];
/* (rsx-vtx) Moves on whenever an RSX offset may translate to another address:
 * the IO tables written or cleared, a local page recorded, an init. The draw
 * engine's pair memo (PS3RECOMP_ENG_PAIR_MEMO) keeps translated pointers only
 * while it stands still. */
volatile u32 g_gcm_xlate_gen = 1;
static inline void gcm_xlate_moved(void) { __atomic_fetch_add(&g_gcm_xlate_gen, 1u, __ATOMIC_RELEASE); }

/* Command buffer control */
static CellGcmControl s_control;

/* Offset table storage
 *
 * set_offset_table/clear_offset_table keep these two host arrays in step with
 * the IO mappings, and that part was always fine. What was not: the tables
 * cellGcmGetOffsetTable hands out are indexed BY THE GUEST --
 * ioAddress[ea >> 20] and eaAddress[io >> 20] -- and it handed over 64-bit host
 * pointers through a struct whose guest form is two 4-byte fields, so the title
 * got two truncated addresses pointing at nothing it could read.
 *
 * The guest-visible copy lives in the reserved GCM window alongside the label
 * and control blocks, published on each call. 4096 pages covers the 4 GiB
 * address space at 1 MiB granularity, 8 KiB a side, and this is not a hot path.
 * 0xFFFF means "not mapped", as it does in the host tables. */
/* Default home for the HLE-visible GCM window; a port overrides it before
 * running guest code when its title claims this address range. */
uint32_t ppu_hle_inject_base = 0x20000000u;

static u16 s_io_address_table[65536];
static u16 s_ea_address_table[65536];

#define GCM_OFFSET_TABLE_PAGES   4096u                 /* 4 GiB / 1 MiB       */
#define GCM_OFFSET_TABLE_IO_ADDR (VM_HLE_INJECT_BASE + 0x3000u)           /* ea >> 20  -> io>>20 */
#define GCM_OFFSET_TABLE_EA_ADDR (VM_HLE_INJECT_BASE + 0x5000u)           /* io >> 20  -> ea>>20 */

/* Local memory bump allocator */
static u32 s_local_mem_allocated = 0;  /* next free offset in local memory */

/* IO memory mapping table */
typedef struct IoMapping {
    u32 ea;         /* effective address in main memory */
    u32 io;         /* IO offset (RSX-visible) */
    u32 size;       /* size of mapping */
    int active;     /* 1 if this slot is in use */
} IoMapping;

static IoMapping s_io_mappings[CELL_GCM_MAX_IO_MAPPINGS];
static int s_io_mapping_count = 0;
static void gcm_spu_put_box_update(void);   /* (perf17) after every change to s_io_mappings */
static int s_spu_put_mode = 0;              /* (perf17) see gcm_spu_put_box_update */

/* Callback handlers — these are GUEST OPD addresses passed by the
 * recompiled game, not host function pointers. Stored as uint32_t and
 * invoked through g_ps3_guest_caller (see ps3emu/guest_call.h). */

/* Handler mask exported for the RSX event delivery path. Bit layout matches
 * the RPCS3 SYS_RSX_EVENT enum (vblank=0x02, flip=0x04, user_cmd=0x80).
 * The runner's vblank tick reads this to decide which events to send to
 * the libgcm interrupt thread. Updated by Set*Handler below.
 *
 * If g_gcm_driver_info_ea is non-zero, the mask is also written to
 * driver_info + 0x12C0 so the game's own libgcm interrupt path can read it
 * directly from guest memory. The runner sets g_gcm_driver_info_ea during
 * RSX context allocation. */
uint32_t g_gcm_handler_mask = 0;
uint32_t g_gcm_driver_info_ea = 0;

static u32 s_flip_handler_opd     = 0;

/* GCM_FLIPCB_ONTICK=1: fire the guest flip handler only when the flip
 * completes (cellGcmTickFlip), not also when it is requested. */
static int gcm_flipcb_on_tick_only(void)
{
    static int v = -1;
    if (v < 0) v = getenv("GCM_FLIPCB_ONTICK") ? 1 : 0;
    return v;
}

static u32 s_vblank_handler_opd   = 0;
static u32 s_user_handler_opd     = 0;
static u32 s_second_v_handler_opd = 0;
/* YDKJ_HANDLERFIX: the handler OPD's code word (*(opd)) gets clobbered to 0 by a
 * mislifted store, so g_ps3_guest_caller reads code=0 -> "not registered" and the
 * game's per-flip/vblank callback never fires -> render/advance loop stalls. We
 * capture the code at Set*Handler time (OPD still valid) and restore the OPD word
 * before each invocation if it's been clobbered. */
extern u32 vm_read32(unsigned int);
static u32 s_flip_handler_code    = 0;
static u32 s_vblank_handler_code  = 0;
static void ydkj_restore_handler_opd(u32 opd, u32 code) {
    if (!getenv("YDKJ_HANDLERFIX") || !opd || !code) return;
    extern u8* vm_base; if (!vm_base) return;
    if (vm_read32(opd) == 0) {
        u8* p = vm_base + opd; p[0]=(u8)(code>>24); p[1]=(u8)(code>>16); p[2]=(u8)(code>>8); p[3]=(u8)code;
        static int _n=0; if(PS3_LOG_FIRST(_n, 6)) fprintf(stderr,"[HANDLERFIX] restored clobbered OPD 0x%08X code=0x%08X\n",opd,code);
    }
}
static void gcm_update_handler_mask(void)
{
    uint32_t m = 0;
    if (s_vblank_handler_opd) m |= 0x02;
    if (s_flip_handler_opd)   m |= 0x04;
    if (s_user_handler_opd)   m |= 0x80;
    g_gcm_handler_mask = m;
    if (g_gcm_driver_info_ea)
        vm_write32(g_gcm_driver_info_ea + 0x12C0, m);
}

/* Legacy host-typed slots kept around for any caller still treating
 * these as host pointers. New code should use the _opd slots. */
static CellGcmFlipHandler    s_flip_handler    = NULL;
static CellGcmVBlankHandler  s_vblank_handler  = NULL;
static CellGcmUserHandler    s_user_handler    = NULL;
static CellGcmSecondVHandler s_second_v_handler = NULL;

/* Flip timing */
static u64 s_last_flip_time = 0;

/* VBlank counter (incremented on each flip as approximation) */
static u32 s_vblank_count = 0;

/* IO map reservation tracking */
static u32 s_io_map_reserved = 0;  /* bytes reserved for IO mapping */

/* Default FIFO mode */
static s32 s_default_fifo_mode = 0;

/* Graphics / queue handlers */
static CellGcmGraphicsHandler s_graphics_handler = NULL;
static CellGcmQueueHandler    s_queue_handler    = NULL;

/* Second V / VBlank frequency */
static u32 s_second_v_frequency = 0;
static u32 s_vblank_frequency   = 0;

/* User command */
#ifdef _WIN32
static volatile LONG s_user_command = 0;
#define GCM_USER_STORE(cmd) InterlockedExchange(&s_user_command, (LONG)(cmd))
/* Pair the pending marker with its cause so a later producer cannot
 * overwrite a cause already claimed by the callback pump. */
static volatile LONG64 s_user_pending = 0;
#define GCM_USER_PENDING_STORE(value) InterlockedExchange64(&s_user_pending, (LONG64)(value))
#define GCM_USER_PENDING_TAKE() ((u64)InterlockedExchange64(&s_user_pending, 0))
#else
#include <stdatomic.h>
static atomic_uint s_user_command = 0;
#define GCM_USER_STORE(cmd) atomic_store(&s_user_command, (cmd))
static _Atomic(u64) s_user_pending = 0;
#define GCM_USER_PENDING_STORE(value) atomic_store(&s_user_pending, (value))
#define GCM_USER_PENDING_TAKE() atomic_exchange(&s_user_pending, 0)
#endif

/* Tile configuration (up to 15 tiles, 8 commonly used) */
static CellGcmTileInfo s_tiles[CELL_GCM_MAX_TILE_COUNT];

/* Zcull configuration (8 zcull regions) */
static CellGcmZcullInfo s_zcull[CELL_GCM_MAX_ZCULL_COUNT];

/* Report data area (256 slots, each 16 bytes) */
static CellGcmReportData s_report_data[CELL_GCM_MAX_REPORT_COUNT];

/* Label area (256 labels, each u32). Labels live in GUEST memory so the
 * recompiled game can poll them via vm_read32; cellGcmGetLabelAddress returns a
 * guest address into this window (16-byte spaced as on hardware). A host array
 * pointer would be read as a guest offset and land out of bounds. */
#define GCM_LABEL_GUEST_BASE  (VM_HLE_INJECT_BASE + 0x0000u)
#define GCM_LABEL_STRIDE      0x10u
static u32 s_labels[CELL_GCM_MAX_LABEL_COUNT];

/* GCM control register (put/get/ref). This MUST live in guest memory: the title
 * takes the pointer from cellGcmGetControlRegister, writes `put` when it kicks
 * the FIFO, and spins reading `get`/`ref` to know the RSX has caught up. A host
 * pointer would be read through vm_base as a guest address and never update, so
 * every FIFO-space / cellGcmFinish wait would hang. Placed just past the 256
 * label slots (0x03000000..0x03001000). Fields are big-endian (vm_write32). */
#define GCM_CONTROL_GUEST_ADDR (VM_HLE_INJECT_BASE + 0x2000u)

/* Synthetic guest code EA for the FIFO command-buffer-full callback. The title's
 * inline gcmReserve calls context->callback(context, count) when `current` nears
 * `end`; we point the context's callback OPD at this EA and register it in the
 * PPU function table (ppu_sysprx.cpp: hle_gcm_callback) so the indirect call
 * routes back into cellGcm_fifo_recycle(). Lives in the injected control page,
 * never real guest code. MUST match GCM_FIFO_CALLBACK_SENTINEL_EA there. */
#define GCM_FIFO_CALLBACK_SENTINEL_EA (VM_HLE_INJECT_BASE + 0x2F00u)

/* EA the ticker's RSX drain has consumed up to. Exposed so the wrap callback can
 * wait for the RSX to catch up before recycling the ring (else the frame's
 * just-written commands are lost). Updated in cellGcm_rsx_process_fifo. */
volatile u32 g_gcm_fifo_drained_ea = 0;

/* The guest variable cellGcmInit's ctx_out pointed at -- the title's own
 * gCellGcmCurrentContext. A title that switches to a command buffer of its own
 * does it by repointing this, and until something reads it back the runtime has
 * no way to notice. GCM_CTXDBG=1 reports it. */
static u32 s_gcm_ctx_out_ea = 0;

/* Set by cellGcm_syscall_bringup: this run reaches RSX through the lv2 sys_rsx_*
 * syscalls (libs/video/sys_rsx.c) rather than through the HLE cellGcm* imports. */
static int s_gcm_syscall_mode = 0;

/* ---------------------------------------------------------------------------
 * Internal helpers
 * -----------------------------------------------------------------------*/

/* Populate the offset table entries for an IO mapping */
static void populate_offset_table(u32 ea, u32 io, u32 size)
{
    /*
     * The offset table uses 1MB pages (20-bit shift).
     * ioAddress[ea >> 20] = io >> 20   (EA -> IO offset)
     * eaAddress[io >> 20] = ea >> 20   (IO offset -> EA)
     */
    /* Round UP: a mapping is described by whole 1MB table entries, so a size
     * that is not a multiple of 1MB still needs its final partial page mapped.
     * Truncating dropped that page -- and dropped the mapping ENTIRELY for any
     * size below 1MB (pages == 0) -- after which cellGcmResolveOffset finds no
     * IO entry and falls back to VRAM, where the guest never wrote anything. */
    u32 pages = (size + 0xFFFFFu) >> 20;  /* number of 1MB pages, rounded up */
    u32 ea_page = ea >> 20;
    u32 io_page = io >> 20;

    for (u32 i = 0; i < pages && (ea_page + i) < 65536 && (io_page + i) < 65536; i++) {
        s_io_address_table[ea_page + i] = (u16)(io_page + i);
        s_ea_address_table[io_page + i] = (u16)(ea_page + i);
    }
    gcm_xlate_moved();
}

/* Clear offset table entries for an IO mapping */
static void clear_offset_table(u32 ea, u32 io, u32 size)
{
    u32 pages = size >> 20;
    u32 ea_page = ea >> 20;
    u32 io_page = io >> 20;

    for (u32 i = 0; i < pages && (ea_page + i) < 65536 && (io_page + i) < 65536; i++) {
        s_io_address_table[ea_page + i] = 0xFFFF;
        s_ea_address_table[io_page + i] = 0xFFFF;
    }
    gcm_xlate_moved();
}

/* Find an IO mapping by EA */
static IoMapping* find_mapping_by_ea(u32 ea)
{
    for (int i = 0; i < CELL_GCM_MAX_IO_MAPPINGS; i++) {
        if (s_io_mappings[i].active && s_io_mappings[i].ea == ea)
            return &s_io_mappings[i];
    }
    return NULL;
}

/* Find an IO mapping by IO offset */
static IoMapping* find_mapping_by_io(u32 io)
{
    for (int i = 0; i < CELL_GCM_MAX_IO_MAPPINGS; i++) {
        if (s_io_mappings[i].active && s_io_mappings[i].io == io)
            return &s_io_mappings[i];
    }
    return NULL;
}

/* Find a free IO mapping slot */
static IoMapping* find_free_mapping(void)
{
    for (int i = 0; i < CELL_GCM_MAX_IO_MAPPINGS; i++) {
        if (!s_io_mappings[i].active)
            return &s_io_mappings[i];
    }
    return NULL;
}

/* Valid tiled pitch sizes (power-of-two aligned, common RSX values) */
static const u32 s_valid_pitches[] = {
    0x0200, 0x0300, 0x0400, 0x0500, 0x0600, 0x0700, 0x0800,
    0x0A00, 0x0C00, 0x0D00, 0x0E00, 0x1000, 0x1400, 0x1800,
    0x1A00, 0x1C00, 0x2000, 0x2800, 0x3000, 0x3400, 0x3800,
    0x4000, 0x5000, 0x6000, 0x6800, 0x7000, 0x8000, 0xA000,
    0xC000, 0xD000, 0xE000, 0x10000
};
static const int s_valid_pitch_count = sizeof(s_valid_pitches) / sizeof(s_valid_pitches[0]);

/* ---------------------------------------------------------------------------
 * API implementations -- Initialization
 * -----------------------------------------------------------------------*/

/* NID: 0xB2E761D4 */
s32 cellGcmInit(u32 cmdSize, u32 ioSize, u32 ioAddress)
{
    printf("[cellGcmSys] Init(cmdSize=0x%X, ioSize=0x%X, ioAddr=0x%08X)\n",
           cmdSize, ioSize, ioAddress);

    if (s_gcm_initialized) {
        printf("[cellGcmSys] WARNING: already initialized\n");
        return CELL_GCM_ERROR_FAILURE;
    }

    memset(s_display_buffers, 0, sizeof(s_display_buffers));
    memset(s_display_buffer_set, 0, sizeof(s_display_buffer_set));
    memset(&s_config, 0, sizeof(s_config));
    memset(&s_control, 0, sizeof(s_control));
    memset(s_io_mappings, 0, sizeof(s_io_mappings));
    memset(s_tiles, 0, sizeof(s_tiles));
    memset(s_zcull, 0, sizeof(s_zcull));
    memset(s_report_data, 0, sizeof(s_report_data));
    memset(s_labels, 0, sizeof(s_labels));
    memset(s_io_address_table, 0xFF, sizeof(s_io_address_table));
    memset(s_ea_address_table, 0xFF, sizeof(s_ea_address_table));
    gcm_xlate_moved();

    /*
     * Populate a plausible configuration.
     * In a real implementation these would reflect actual allocated memory.
     */
    s_config.localAddress    = 0xC0000000;  /* typical RSX local mem base */
    s_config.localSize       = 256 * 1024 * 1024;  /* 256 MB */
    s_config.ioAddress       = ioAddress;
    s_config.ioSize          = ioSize;
    s_config.memoryFrequency = 650000000;   /* 650 MHz */
    s_config.coreFrequency   = 500000000;   /* 500 MHz */

    s_local_mem_allocated = 0;
    s_io_mapping_count = 0;
    gcm_spu_put_box_update();   /* (perf17) no mappings (yet) */
    /* Unmapped = 0xFFFF. The tables are static (zero-initialized), so without
     * this every unmapped EA page silently aliased io page 0 (and vice versa),
     * producing plausible-but-wrong offsets instead of a translation failure. */
    memset(s_io_address_table, 0xFF, sizeof(s_io_address_table));
    memset(s_ea_address_table, 0xFF, sizeof(s_ea_address_table));
    gcm_xlate_moved();
    s_current_display_buffer_id = 0;
    s_flip_handler = NULL;
    s_vblank_handler = NULL;
    s_user_handler = NULL;
    s_second_v_handler = NULL;
    s_last_flip_time = get_timestamp_ns();
    s_flip_status = CELL_GCM_FLIP_STATUS_DONE;
    s_flip_mode = CELL_GCM_DISPLAY_VSYNC;
    s_debug_level = CELL_GCM_DEBUG_LEVEL0;
    s_vblank_count = 0;
    s_io_map_reserved = 0;
    s_default_fifo_mode = 0;
    s_graphics_handler = NULL;
    s_queue_handler = NULL;
    s_second_v_frequency = 0;
    s_vblank_frequency = 0;
    GCM_USER_STORE(0);
    GCM_USER_PENDING_STORE(0);

    /* Set up the initial IO mapping for the command buffer region */
    if (ioAddress != 0 && ioSize > 0) {
        populate_offset_table(ioAddress, 0, ioSize);

        s_io_mappings[0].ea     = ioAddress;
        s_io_mappings[0].io     = 0;
        s_io_mappings[0].size   = ioSize;
        s_io_mappings[0].active = 1;
        s_io_mapping_count = 1;
    }

    /* Zero the guest-visible control register (put/get/ref). */
    vm_write32(GCM_CONTROL_GUEST_ADDR + 0, 0);
    vm_write32(GCM_CONTROL_GUEST_ADDR + 4, 0);
    vm_write32(GCM_CONTROL_GUEST_ADDR + 8, 0);

    /* Watch SPU DMA for pushbuffers. A title that builds its command stream on
     * an SPU never tells libgcm where that stream is, so the write is the only
     * evidence of it -- see gcm_note_spu_put. Costs one NULL check per SPU DMA
     * for titles that do not. */
    { extern void (*g_spu_put_hook)(u32 ea, u32 size);
      void gcm_note_spu_put(u32 ea, u32 size);       /* defined with the walker */
      void gcm_forget_spu_blocks(void);
      gcm_forget_spu_blocks();
      g_spu_put_hook = gcm_note_spu_put;
      /* (critic2) PS3RECOMP_SPU_PUT_DESC (default 1; 0 = ascending, as before):
       * SPU PUTs and line commits into IO-mapped memory are written top-down
       * (runtime/spu/spu_dma.h spu_put_copy, gm_copy_to_desc). */
      extern int (*g_spu_put_rsx_hook)(u32 ea, u32 size);
      int gcm_spu_put_is_rsx(u32 ea, u32 size);
      const char* pd = getenv("PS3RECOMP_SPU_PUT_DESC");
      g_spu_put_rsx_hook = (pd && *pd == '0') ? NULL : gcm_spu_put_is_rsx;
      /* (perf17) PS3RECOMP_SPU_PUT_INLINE (default 1): the test inline against
       * the mappings' interval; 0 = the hook call for every PUT, as m17 */
      const char* pi = getenv("PS3RECOMP_SPU_PUT_INLINE");
      s_spu_put_mode = !g_spu_put_rsx_hook ? 0 : (pi && *pi == '0') ? 2 : 1;
      gcm_spu_put_box_update(); }

    s_gcm_initialized = 1;
    return CELL_OK;
}

/* Reusable _cellGcmInitBody core (NID 0x15BAE46B). See cellGcmSys.h for why the
 * guest vm is injected as callbacks. The CellGcmContextData layout here is the
 * one proven in the shipping Simpsons port (begin@0/end@4/current@8/callback@C);
 * getting it wrong makes the game read `current` as the callback OPD and stall
 * in cellGcmFlush. The command buffer is the start of the game's IO region. */
u32 cellGcmSetupContext(u32 ctx_out_addr, u32 cmdSize, u32 ioSize, u32 ioAddress,
                        CellGcmGuestAlloc galloc, CellGcmGuestWrite32 gwrite32)
{
    if (cmdSize < 0x10000)
        cmdSize = 0x10000;

    /* PSL1GHT's gcmInitBody passes ioAddress = 0, expecting gcm to allocate the
     * IO region itself (the official SDK has the app allocate and pass it).
     * Taking the 0 literally put the FIFO command buffer at guest address 0 --
     * the guest then wrote its command stream over low memory and every offset
     * resolution failed. Allocate a guest region of ioSize instead. */
    if (ioAddress == 0 && galloc)
        ioAddress = galloc(ioSize ? ioSize : 0x100000, 0x100000);

    cellGcmInit(cmdSize, ioSize, ioAddress);

    u32 cmdbuf = ioAddress;                 /* FIFO lives at the IO region start */
    u32 cdata  = galloc ? galloc(16, 16) : 0;
    if (cdata && gwrite32) {
        gwrite32(cdata + 0x0, cmdbuf);              /* begin   */
        gwrite32(cdata + 0x4, cmdbuf + cmdSize);    /* end     */
        gwrite32(cdata + 0x8, cmdbuf);              /* current */
        /* Command-buffer-full callback: a guest OPD {func, toc} routed to
         * hle_gcm_callback -> cellGcm_fifo_recycle. Without a real callback the
         * ring never recycles and the FIFO wedges once `current` reaches `end`. */
        u32 opd = galloc ? galloc(8, 8) : 0;
        if (opd) {
            gwrite32(opd + 0x0, GCM_FIFO_CALLBACK_SENTINEL_EA);  /* func EA */
            gwrite32(opd + 0x4, 0);                              /* toc (unused) */
            gwrite32(cdata + 0xC, opd);                          /* callback OPD */
        } else {
            gwrite32(cdata + 0xC, 0);
        }
        if (ctx_out_addr)
            gwrite32(ctx_out_addr, cdata);          /* *context = &ctxdata */
        s_gcm_ctx_out_ea = ctx_out_addr;   /* the title's gCellGcmCurrentContext */
        s_gcm_context_ea = cdata;                   /* RSX drains commands from here */
    }
    return cdata;
}

s32 cellGcmGetConfiguration(CellGcmConfig* config)
{
    /* `config` is a GUEST address; write the 6 u32 fields big-endian via
     * vm_write32 (a raw *config = s_config faults / is host-endian). The game
     * reads localAddress/localSize from here to lay out VRAM framebuffers. */
    uint32_t cfg = (uint32_t)(uintptr_t)config;
    if (!cfg)
        return CELL_GCM_ERROR_INVALID_VALUE;

    vm_write32(cfg +  0, s_config.localAddress);
    vm_write32(cfg +  4, s_config.ioAddress);
    vm_write32(cfg +  8, s_config.localSize);
    vm_write32(cfg + 12, s_config.ioSize);
    vm_write32(cfg + 16, s_config.memoryFrequency);
    vm_write32(cfg + 20, s_config.coreFrequency);
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * Command buffer control
 * -----------------------------------------------------------------------*/

/* NID: 0x8572A8E0 */
CellGcmControl* cellGcmGetControlRegister(void)
{
    /* Return the GUEST address of the control register (not &s_control, a host
     * pointer). The recompiled title reads/writes it through vm_base, so it must
     * be a guest EA for put/get/ref to actually flow. */
    return (CellGcmControl*)(uintptr_t)GCM_CONTROL_GUEST_ADDR;
}

/* ---------------------------------------------------------------------------
 * Display / flip
 * -----------------------------------------------------------------------*/

/* NID: 0xDB23E867 */
u32 cellGcmGetCurrentField(void)
{
    /* Returns 0 or 1 for interlaced; always 0 for progressive. */
    return 0;
}

/* NID: 0xA53D12AE */
void cellGcmSetFlipMode(u32 mode)
{
    printf("[cellGcmSys] SetFlipMode(mode=%u)\n", mode);
    s_flip_mode = mode;
}

/* NID: 0xC44D8F34 */
void cellGcmSetWaitFlip(void)
{
    /* Block until the pending flip completes (the present thread marks it done
     * via cellGcmTickFlip, ~once per display refresh). This throttles the title
     * to vsync; without it the guest loops unthrottled and many frames batch
     * into a single host present -> the console text stacks/duplicates. */
#ifdef _WIN32
    __declspec(dllimport) void __stdcall Sleep(unsigned long);
#endif  /* elsewhere: runtime/platform/win32_compat.h */
    for (int i = 0; i < 64 && s_flip_status == CELL_GCM_FLIP_STATUS_WAITING; i++)
        Sleep(1);
    /* A flip still WAITING after the bound is left to the tick. Forcing DONE
     * here skipped its completion: the tick only completes WAITING flips, so
     * the flip handler never ran for it, and inFAMOUS 2, whose handler writes
     * the FIFO put it deferred, hung with get == put whenever a frame took
     * longer than the bound. */
}

/* NID: 0xDF6476BD. The same wait without libgcm's command-buffer space check;
 * this HLE writes no command, so it is the same call. GH3 makes it ~36 times
 * a boot and got a faked CELL_OK (no wait at all) before this was registered. */
void cellGcmSetWaitFlipUnsafe(void) { cellGcmSetWaitFlip(); }

/* NID: 0x51C9D62B */
void cellGcmResetFlipStatus(void)
{
    { static int n = 0;
      if (getenv("FLIP_DBG") && PS3_LOG_FIRST(n, 12))
          fprintf(stderr, "[FLIP] ResetFlipStatus%c", 10); }
    FLIP_LOCK();
    s_flip_status = CELL_GCM_FLIP_STATUS_WAITING;
    s_flip_req_seq++;
    FLIP_UNLOCK();
}

/* NID: 0xE315A0B2 */
u32 cellGcmGetFlipStatus(void)
{
    /* Avoid host-calling the guest OPDs directly (they're not valid host
     * function pointers). Use cellGcmTickVBlank() / TickFlip() instead,
     * which dispatch via g_ps3_guest_caller — see below. */
    /* Report the CURRENT status; cellGcmTickFlip (the ticker's 60Hz vblank
     * beat) completes pending flips. The old self-completing version made
     * every wait-for-flip loop exit on its first poll, so titles ran
     * completely unpaced (wave: 95 fps with a fixed-dt simulation). */
    { static int n = 0;
      if (getenv("FLIP_DBG") && PS3_LOG_FIRST(n, 24))
          fprintf(stderr, "[FLIP] GetFlipStatus -> %u (0=DONE 1=WAITING)%c",
                  s_flip_status, 10); }
    return s_flip_status;
}

/* Host-side ticks. The game's host driver (e.g. flow main.cpp) calls
 * these periodically so registered guest handlers fire — that's what
 * drives the title-screen state machine for many games. */
#include "ps3emu/guest_call.h"

/* ---------------------------------------------------------------------------
 * Serialized vblank/flip/user handler delivery.
 *
 * Tick producers publish pending work. The runner pumps it at HLE boundaries
 * or on a dedicated callback thread. The pump serializes guest handlers with
 * one another; it does not stop unrelated guest execution on other threads.
 * -----------------------------------------------------------------------*/
#ifdef _WIN32
#include <windows.h>
static volatile LONG s_gcm_pending = 0;    /* bit0 = vblank, bit1 = flip */
#define GCM_PENDING_SET(bits)  InterlockedOr(&s_gcm_pending, (bits))
#define GCM_PENDING_TAKE()     InterlockedExchange(&s_gcm_pending, 0)
static volatile LONG s_gcm_pumping = 0;
#define GCM_PUMP_TRY_ENTER() (InterlockedCompareExchange(&s_gcm_pumping, 1, 0) == 0)
#define GCM_PUMP_LEAVE()     InterlockedExchange(&s_gcm_pumping, 0)
#else
#include <stdatomic.h>
static atomic_int s_gcm_pending = 0;
#define GCM_PENDING_SET(bits)  atomic_fetch_or(&s_gcm_pending, (bits))
#define GCM_PENDING_TAKE()     atomic_exchange(&s_gcm_pending, 0)
static atomic_flag s_gcm_pumping = ATOMIC_FLAG_INIT;
#define GCM_PUMP_TRY_ENTER() (!atomic_flag_test_and_set_explicit(&s_gcm_pumping, memory_order_acquire))
#define GCM_PUMP_LEAVE()     atomic_flag_clear_explicit(&s_gcm_pumping, memory_order_release)
#endif

/* Called by the vblank ticker thread. NO guest code -- advance the vblank count
 * and mark a vblank+flip tick pending for the main thread to deliver. */
static u32 s_fifo_getoff;   /* tentative: defined below */

void cellGcm_request_tick(void)
{
    s_vblank_count++;
    /* GCM_FLIP_NEEDS_FIFO=1: only mark a flip pending once the walker has
     * actually consumed up to `put`.
     *
     * A title's fifo-finish can use flip status as a fast path -- Twisted
     * Metal's reads it first and, if the flip is done, skips waiting for `get`
     * entirely. Completing a flip on the 60 Hz beat regardless of FIFO
     * progress therefore tells it the RSX has caught up when it has not, and
     * it recycles a FIFO block the walker is still inside, overwriting the
     * JUMP out of it. Off by default: a title whose FIFO never drains would
     * stop flipping entirely. */
    { static int need = -1;
      if (need < 0) { const char* e = getenv("GCM_FLIP_NEEDS_FIFO"); need = e ? atoi(e) : 0; }
      if (need) {
          u32 put = vm_read32(GCM_CONTROL_GUEST_ADDR + 0);
          GCM_PENDING_SET(s_fifo_getoff == put ? 3 : 1);
          return;
      } }
    GCM_PENDING_SET(3);
}

/* Deliver pending handlers on the calling host thread. Serialize the entire
 * claim-and-deliver sequence across threads: an HLE boundary and a host ticker
 * may both pump, and newer causes must not overtake already claimed callbacks.
 * A nonblocking guard also prevents nested callbacks from reentering. Leave
 * pending notifications untouched when another pump is active. */
void ppu_gcm_pump(void)
{
    if (!GCM_PUMP_TRY_ENTER()) return;
    long p = (long)GCM_PENDING_TAKE();
    u64 user = GCM_USER_PENDING_TAKE();
    /* GCM_PUMP_DBG=1: how often each guest handler is actually delivered. */
    { static int d = -1; static unsigned nv, nf, calls;
      if (d < 0) d = getenv("GCM_PUMP_DBG") ? 1 : 0;
      if (d) { calls++; if (p & 1) nv++; if (p & 2) nf++;
               if ((calls % 20000) == 0)
                   fprintf(stderr, "[gcm-pump] calls=%u vblank=%u flip=%u vcount=%u%c",
                           calls, nv, nf, s_vblank_count, 10); } }
    if ((p & 1) && s_vblank_handler_opd && g_ps3_guest_caller) {
        ydkj_restore_handler_opd(s_vblank_handler_opd, s_vblank_handler_code);
        g_ps3_guest_caller(s_vblank_handler_opd, (uint64_t)s_vblank_count,
                           0, 0, 0, 0, 0, 0, 0);
    }
    if (p & 2) {
        /* Delivery waits for a guest thread to reach an HLE call or a timer
         * sleep, so a newer flip may have been requested (status WAITING
         * again) since this completion was marked. Marking DONE then would
         * erase that request: the ticker only completes WAITING flips, so its
         * handler would never run. inFAMOUS 2 writes its next FIFO put from
         * the flip handler, and hung with get == put on exactly that. Leave
         * the newer flip WAITING for the next tick. */
        FLIP_LOCK();
        if (s_flip_tick_seq == s_flip_req_seq)
            s_flip_status = CELL_GCM_FLIP_STATUS_DONE;
        FLIP_UNLOCK();
        if (s_flip_handler_opd && g_ps3_guest_caller) {
            ydkj_restore_handler_opd(s_flip_handler_opd, s_flip_handler_code);
            g_ps3_guest_caller(s_flip_handler_opd, 1, 0, 0, 0, 0, 0, 0, 0);
        }
    }
    if (user) {
        u32 cmd = (u32)user;
        if (s_user_handler_opd && g_ps3_guest_caller)
            g_ps3_guest_caller(s_user_handler_opd, (uint64_t)cmd, 0, 0, 0, 0, 0, 0, 0);
    }
    GCM_PUMP_LEAVE();
}

/* Back-compat: the old direct entry points now just mark a tick pending (so any
 * caller other than the ticker still works) -- delivery stays on the main thread. */
void cellGcmTickVBlank(void) { s_vblank_count++; GCM_PENDING_SET(1); }
/* Only deliver a flip completion when a flip is actually outstanding.
 *
 * Ticking unconditionally at 60 Hz fires the guest's flip handler even when
 * the guest never asked for a flip. A title whose handler touches the flip
 * status then livelocks: Tokyo Jungle's main loop is
 *
 *     do { usleep(100); } while (cellGcmGetFlipStatus() != DONE);
 *     cellGcmResetFlipStatus();
 *
 * and its handler runs code that puts the status back to WAITING, so the
 * poll never saw DONE and the game never rendered another frame. On hardware
 * the callback IS the flip interrupt -- no flip, no callback. */
void cellGcmTickFlip(void)
{
    FLIP_LOCK();
    const int waiting = s_flip_status == CELL_GCM_FLIP_STATUS_WAITING;
    if (waiting) s_flip_tick_seq = s_flip_req_seq;
    FLIP_UNLOCK();
    if (waiting) GCM_PENDING_SET(2);
}

/* Drain the game's GCM FIFO into the RSX backend. Called from the present thread
 * (boot_main vblank_ticker). Not yet active: s_control.put stays 0 because
 * cellGcmGetControlRegister returns a HOST pointer, so the title's put writes
 * don't reach this struct (the control register must live in guest memory for
 * the recompiled code to update it via vm_write). Once that's fixed (+ the title
 * runs past its early self-exit to actually draw), parse get..put here with
 * rsx_process_command_buffer so the game's clears/draws render. */
/* Translate an RSX IO offset to a guest EA via the offset table (1MB pages,
 * populated by cellGcmInit / cellGcmMapMainMemory / MapEaIoAddress). */
/* Any of the per-word FIFO diagnostics enabled? Read once: the drain checked
 * five lazily-initialised flags on every data word of every command, on the
 * thread that is the frame-rate ceiling (T-0016). */
static int gcm_fifo_dbg(void)
{
    static int on = -1;
    if (on < 0)
        on = (getenv("GCM_SUBCH7") || getenv("GCM_OBJDBG") || getenv("GCM_SUBCH_GATE") ||
              getenv("GCM_VAL_WATCH") || getenv("RSX_LIVE_FEED_DBG")) ? 1 : 0;
    return on;
}

static u32 gcm_io2ea(u32 io)
{
    u32 page = io >> 20;
    if (page >= 65536) return 0;
    u16 ea_page = s_ea_address_table[page];
    if (ea_page == 0xFFFF) return 0;
    return ((u32)ea_page << 20) | (io & 0xFFFFFu);
}

/* RSX get pointer (IO offset) + one-deep CALL return slot. The FIFO-wrap
 * recycle path teleports these when it resets a ring without a JUMP command. */
unsigned long long ps3_ms_now(void)
{
#ifdef _WIN32
    return (unsigned long long)GetTickCount64();
#else
    /* Returned 0 here, which silently disabled every timeout built on it
     * (GCM_PARK_GRACE_MS, GCM_SEMA_ACQUIRE_MS) and zeroed the rate logs
     * (upstream 91ee29f). */
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000ull + (unsigned long long)ts.tv_nsec / 1000000ull;
#endif
}

static u32 s_sema_offset = 0;   /* NV406E semaphore offset (label window) */
static int s_sema_blocked_pass = 0;   /* this drain stopped on a semaphore acquire */
static int s_sema_live = 0;           /* the LAST drain pass ended on one (see cellGcm_drain_sema_blocked) */
static uint64_t s_ft_sema_t0;          /* PS3_FTRACE: when the walker began waiting on the current acquire */
/* (critic) what the frame clock's idle wait may end early on -- written by the
 * draining thread at the end of each pass, read by the same thread in
 * cellGcm_fifo_kick_wait and (s_sema_live / s_sema_offset only) by the SPU DMA
 * hook, which only uses them to decide whether to signal the kick event. */
static u32 s_sema_wait_val;            /* the value the blocked acquire waits for */
static u32 s_kw_put;                   /* `put` as the last drain pass read it */
static int s_kw_more;                  /* the last pass stopped at a flip with words left before put */
static u32 s_kw_park_word;             /* the self-JUMP word the last pass stopped on (s_park_off) */

/* RSX_SNAP_AT=<sec>, RSX_SNAP_RANGES=<ea>:<len>[,<ea>:<len>...], RSX_SNAP_DIR=<dir>:
 * photograph guest memory at two points of one frame, once, after <sec> s.
 *   A: the RSX writes label RSX_SNAP_LABEL_A (offset, default 0x860)
 *   B: an ACQUIRE on label RSX_SNAP_LABEL_B (default 0x890) is satisfied
 * Files: <dir>/<a|b>_<ea>.bin, raw guest bytes. Written for inFAMOUS 2's SPU
 * lighting, which turns the normals/depth copies into light in place between
 * label 134 (copies done) and label 137 (light done): A holds the job's
 * inputs, B its outputs, with the same frame's constants. */
static struct timespec s_snap_t0;      /* process start, for RSX_SNAP_AT */
__attribute__((constructor)) static void rsx_snap_t0_init(void) { clock_gettime(CLOCK_MONOTONIC, &s_snap_t0); }

void rsx_snap_event(int which, u32 offset)
{
    static int on = -1; static double at; static u32 lab[2] = { 0x860, 0x890 };
    static char dir[512], ranges[1024]; static int state;   /* 0 armed, 1 took A, 2 done */
    const struct timespec t0 = s_snap_t0;
    if (on < 0) {
        const char* e = getenv("RSX_SNAP_AT"); const char* r = getenv("RSX_SNAP_RANGES");
        const char* d = getenv("RSX_SNAP_DIR");
        on = (e && r && d) ? 1 : 0;
        if (on) { at = atof(e); snprintf(ranges, sizeof ranges, "%s", r); snprintf(dir, sizeof dir, "%s", d);
                  const char* a = getenv("RSX_SNAP_LABEL_A"); if (a) lab[0] = (u32)strtoul(a, 0, 16);
                  const char* b = getenv("RSX_SNAP_LABEL_B"); if (b) lab[1] = (u32)strtoul(b, 0, 16); }
    }
    if (!on || state == 2 || (offset & 0xFFFFu) != lab[which]) return;
    if (which == 1 && state != 1) return;
    if (which == 0) {
        struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
        if ((double)(now.tv_sec - t0.tv_sec) + (double)(now.tv_nsec - t0.tv_nsec) * 1e-9 < at) return;
    }
    extern uint8_t* vm_base;
    char buf[1024]; snprintf(buf, sizeof buf, "%s", ranges);
    for (char* tok = strtok(buf, ","); tok; tok = strtok(NULL, ",")) {
        char* c = strchr(tok, ':'); if (!c) continue;
        const u32 ea = (u32)strtoul(tok, 0, 16), len = (u32)strtoul(c + 1, 0, 16);
        char path[1100]; snprintf(path, sizeof path, "%s/%c_%08x.bin", dir, which ? 'b' : 'a', ea);
        FILE* f = fopen(path, "wb");
        if (f) { fwrite(vm_base + ea, 1, len, f); fclose(f); }
    }
    fprintf(stderr, "[rsx snap] %c at label offset 0x%X (label value 0x%08X) -> %s\n",
            which ? 'B' : 'A', offset, vm_read32(GCM_LABEL_GUEST_BASE + (offset & 0xFFFFu)), dir);
    state = which ? 2 : 1;
}
static int s_park_pass = 0;           /* this drain stopped on a self-JUMP park */
static u32 s_park_off;                 /* the park the walker is waiting on, and since when */
static unsigned long long s_park_since;
/* Drain timing for the "ref stuck" report: when the drain now running began
 * (0 = none running), when the last one ended and why it stopped. */
static volatile unsigned long long s_drain_in_ms, s_drain_end_ms;
static const char* volatile s_drain_why = "";
static volatile unsigned s_drain_count;
/* Backlog past which the drain stops honouring one-flip-per-tick and
 * catches up instead. Sized well above a frame's command list so a
 * title that keeps up never sees this path. */
#define GCM_FIFO_CATCHUP_BYTES 0x10000u
/* Command bytes the walker has executed since the last flip word: the size of
 * the frame that flip ends. */
static u32 s_frame_bytes;
static unsigned long long s_walk_bytes;   /* all of them, never reset */
/* GCM_FLIP_STATS=1: every 10 s, what happened at the flip words since the
 * last line -- frames' command bytes, the backlog (put - get) each flip met,
 * how often the catch-up below drained on past a flip, how many flips were
 * never shown (a flip reached while the one before was still unpresented), and
 * the longest gap between two flips. Each catch-up also gets a line. */
static int gcm_flip_stats_on(void)
{
    static int on = -1;
    if (on < 0) on = getenv("GCM_FLIP_STATS") != NULL;
    return on;
}
static struct {
    unsigned flips, catchups, merged, shown, wrapped, over64k;
    unsigned long long t0, last_flip, max_gap; unsigned gaps100;
    u32 max_backlog, nb; u32 bytes[4096];
} s_fst;
/* Flips requested through the import (not a FIFO word), and those of them
 * requested while the one before was still unpresented. */
static volatile unsigned s_flip_import, s_flip_import_merged;
/* ...and the command bytes the walker had executed since the import flip
 * before (sum and largest): a title that flips through the import has no flip
 * word in its stream, so this is its frame size. */
static unsigned long long s_imp_bytes_sum; static u32 s_imp_bytes_max;
static int gcm_u32_cmp(const void* a, const void* b)
{ const u32 x = *(const u32*)a, y = *(const u32*)b; return x < y ? -1 : x > y; }
static void gcm_flip_stats_note(u32 put, u32 get, u32 bytes, int catchup, int merged)
{
    extern unsigned long long ps3_ms_now(void);
    const unsigned long long now = ps3_ms_now();
    if (!s_fst.t0) s_fst.t0 = now;
    s_fst.flips++;
    if (catchup) s_fst.catchups++;
    if (merged) s_fst.merged++;
    if (put < get) s_fst.wrapped++;
    else { const u32 b = put - get;
           if (b > s_fst.max_backlog) s_fst.max_backlog = b;
           if (b >= GCM_FIFO_CATCHUP_BYTES) s_fst.over64k++; }
    if (s_fst.nb < 4096) s_fst.bytes[s_fst.nb++] = bytes;
    if (s_fst.last_flip) {
        const unsigned long long g = now - s_fst.last_flip;
        if (g > s_fst.max_gap) s_fst.max_gap = g;
        if (g >= 100) s_fst.gaps100++;
        static unsigned n;
        if (g >= 250 && n++ < 100)
            fprintf(stderr, "[gcm-flipstats] %llu ms since the last flip (frame %u bytes, backlog %d)\n",
                    g, bytes, put < get ? -1 : (int)(put - get));
    }
    s_fst.last_flip = now;
    if (catchup) {
        static unsigned n;
        if (n++ < 200)
            fprintf(stderr, "[gcm-flipstats] catch-up at flip: backlog %u bytes (put=0x%08X get=0x%08X), "
                            "frame %u bytes\n", put - get, put, get, bytes);
    }
}
/* GCM_FIFO_JUMP_NEW=0: the walker decodes only the old JUMP form, as before
 * (gcm_fifo_decode.h). Read once. */
static int gcm_fifo_jump_new(void)
{
    static int on = -1;
    if (__builtin_expect(on < 0, 0)) { const char* e = getenv("GCM_FIFO_JUMP_NEW"); on = (e && *e == '0') ? 0 : 1; }
    return on;
}
/* New-form JUMPs the walker met at a command position (GCM_FLIP_STATS). */
static unsigned s_new_jumps;
static void gcm_note_new_jump(u32 at, u32 w)
{
    s_new_jumps++;
    static unsigned n;
    if (gcm_flip_stats_on() && n++ < 16)
        fprintf(stderr, "[gcm-flipstats] new-form JUMP 0x%08X at io 0x%08X (%s)\n", w, at,
                gcm_fifo_jump_new() ? "followed" : "read as a method header: GCM_FIFO_JUMP_NEW=0");
}
/* Called at the end of every drain: the 10 s line. */
static void gcm_flip_stats_tick(void)
{
    if (!gcm_flip_stats_on()) return;
    extern unsigned long long ps3_ms_now(void);
    const unsigned long long now = ps3_ms_now();
    if (!s_fst.t0) { s_fst.t0 = now; return; }
    if (now - s_fst.t0 >= 10000) {
        const unsigned imp = s_flip_import, imp_m = s_flip_import_merged;
        static unsigned imp_last, imp_m_last;
        static unsigned shown_last;
        const unsigned shown_now = s_flips_shown;
        s_fst.shown = shown_now - shown_last; shown_last = shown_now;
        u32 lo = 0, med = 0, hi = 0;
        if (s_fst.nb) { qsort(s_fst.bytes, s_fst.nb, sizeof(u32), gcm_u32_cmp);
                        lo = s_fst.bytes[0]; med = s_fst.bytes[s_fst.nb / 2]; hi = s_fst.bytes[s_fst.nb - 1]; }
        fprintf(stderr, "[gcm-flipstats] %llu s: %u flips, %u shown, %u never shown, %u catch-ups; "
                        "frame bytes min/median/max %u/%u/%u; backlog at flip max %u, >=64K %u, put<get %u; "
                        "longest flip gap %llu ms, %u gaps >= 100 ms; import flips %u (%u never shown, "
                        "bytes per frame mean %llu max %u); new-form JUMPs %u\n",
                (now - s_fst.t0) / 1000, s_fst.flips, s_fst.shown, s_fst.merged, s_fst.catchups,
                lo, med, hi, s_fst.max_backlog, s_fst.over64k, s_fst.wrapped, s_fst.max_gap, s_fst.gaps100,
                imp - imp_last, imp_m - imp_m_last,
                imp != imp_last ? __atomic_exchange_n(&s_imp_bytes_sum, 0ull, __ATOMIC_RELAXED) / (imp - imp_last) : 0ull,
                __atomic_exchange_n(&s_imp_bytes_max, 0u, __ATOMIC_RELAXED), s_new_jumps);
        s_new_jumps = 0;
        imp_last = imp; imp_m_last = imp_m;
        memset(&s_fst, 0, sizeof s_fst);
        s_fst.t0 = now; s_fst.last_flip = now;
    }
}
/* (limits) PS3_STALL_MS: where the FIFO walker stands, in one line. */
int gcm_stall_snapshot(char* buf, int size)
{
    extern unsigned long long ps3_ms_now(void);
    const u32 put = vm_read32(GCM_CONTROL_GUEST_ADDR + 0);
    const u32 ref = vm_read32(GCM_CONTROL_GUEST_ADDR + 8);
    const u32 gea = gcm_io2ea(s_fifo_getoff);
    const unsigned long long since = s_park_since;
    return snprintf(buf, (size_t)size, " get=%X put=%X word=%08X park=%X/%llums sema=%X ref=%X lab134-137=%X/%X/%X/%X",
                    s_fifo_getoff, put, gea ? vm_read32(gea) : 0u, s_park_off,
                    since ? ps3_ms_now() - since : 0ull, s_sema_offset, ref,
                    vm_read32(GCM_LABEL_GUEST_BASE + 0x860), vm_read32(GCM_LABEL_GUEST_BASE + 0x870),
                    vm_read32(GCM_LABEL_GUEST_BASE + 0x880), vm_read32(GCM_LABEL_GUEST_BASE + 0x890));
}

/* How long a self-JUMP park may stand with `put` somewhere else before the
 * drain gives up on it being patched and follows `put` instead. The hardware
 * waits for ever; GCM_PARK_GRACE_MS=<ms> overrides the default. inFAMOUS 2
 * needs more than 50 once its SPUs are the bottleneck: the PPU can sit in a
 * light-task wait between moving put and patching the park, and a resync
 * there drops the frame's fence and hangs the title for good. */
static unsigned long long gcm_park_grace_ms(void)
{
    static unsigned long long ms = 0;
    if (!ms) { const char* e = getenv("GCM_PARK_GRACE_MS"); ms = (e && atoi(e) > 0) ? (unsigned long long)atoi(e) : 50ull; }
    return ms;
}
#define GCM_PARK_GRACE_MS gcm_park_grace_ms()
/* The walker's two wall-clock bounds -- the park grace above and the
 * semaphore acquire's (GCM_SEMA_ACQUIRE_MS) -- are there for a producer that
 * never comes. They must not fire because the whole process stood still: on a
 * starved Mac (load 13-16, mediaanalysisd and another recomp) a game thread
 * can be off the CPU for seconds and frames came minutes apart. Measured in
 * wall time, a park whose patch was one store away "expired" during such a
 * stall, the jump-park resync dropped the frame's commands with its fence,
 * and inFAMOUS 2 hung for good at the New Marais load. So they count only
 * time the walker saw pass in small steps: the drain re-checks a park or an
 * acquire every pass (about 1 ms), and a gap between two checks longer than
 * GCM_STALL_GAP_MS (default 100) means nobody ran -- that gap is not counted.
 * GCM_STALL_GAP_MS=0 counts all wall time, as before. Live (PS3_TOGGLE_FILE). */
static int s_stall_gap_ms = -1;
static void gcm_stall_gap_set(int id, int v) { (void)id; __atomic_store_n(&s_stall_gap_ms, v, __ATOMIC_RELAXED); }
static unsigned gcm_stall_gap_ms(void)
{
    int v = __atomic_load_n(&s_stall_gap_ms, __ATOMIC_RELAXED);
    if (__builtin_expect(v < 0, 0)) v = ps3_toggle_watch(PS3T_GCM_STALL_GAP, gcm_stall_gap_set);
    return v < 0 ? 0u : (unsigned)v;
}
/* GCM_LEAN_STATE: is rsx_process_method's state dead weight? Only while the
 * draw engine takes the stream and no vtable backend reads rsx_state. */
static int s_lean_state = -1;
static void gcm_lean_state_set(int id, int v) { (void)id; __atomic_store_n(&s_lean_state, v != 0, __ATOMIC_RELAXED); }
static int gcm_lean_state(void)
{
    int v = __atomic_load_n(&s_lean_state, __ATOMIC_RELAXED);
    if (__builtin_expect(v < 0, 0)) v = ps3_toggle_watch(PS3T_GCM_LEAN_STATE, gcm_lean_state_set);
    if (!v) return 0;
    static int eng = -1;
    if (eng < 0) eng = rsx_draw_engine_enabled();
    return eng && !rsx_get_backend();
}
/* GCM_SUBCH1_2D=1: subchannel 1 goes to the 2D path (the old split). */
static int gcm_subch1_2d(void)
{
    static int v = -1;
    if (v < 0) { const char* e = getenv("GCM_SUBCH1_2D"); v = e ? atoi(e) : 0; }
    return v;
}
/* May this header's data words go to the draw engine in one call
 * (rsx_draw_engine_burst)? Only when the per-word walk would do nothing with
 * them but rsx_draw_engine_method: a 3D subchannel, lean state, no live
 * engine, no per-word FIFO diagnostic or guest memory diagnostic, and none of
 * the methods the walker acts on itself (SET_REFERENCE, GET_REPORT, the
 * semaphore / label writes rsx_process_method keeps) in its range.
 * GCM_FIFO_BURST=0: every word the per-word way, as before. */
extern int vm_inline_ok;
static int gcm_fifo_burst_ok(u32 subch, u32 method, u32 type, u32 count)
{
    static int on = -1, live = -1, eng = -1;
    if (on < 0) { const char* e = getenv("GCM_FIFO_BURST"); on = !(e && *e == '0'); }
    if (!on || gcm_fifo_dbg() || !vm_inline_ok) return 0;
    if (!(subch == 0 || (subch == 1 && !gcm_subch1_2d()))) return 0;
    if (live < 0) live = rsx_live_draw_enabled();
    if (eng < 0) eng = rsx_draw_engine_enabled();
    if (live || !eng || !gcm_lean_state()) return 0;
    const u32 lo = method, hi = type == 0 ? method + (count - 1u) * 4u : method;
    if (hi > 0x1FFCu) return 0;
    if (lo <= 0x0050u && hi >= 0x0050u) return 0;
    if (lo <= 0x1800u && hi >= 0x1800u) return 0;
    if (lo <= 0x1D74u && hi >= 0x1D6Cu) return 0;
    return 1;
}
/* Run time of one wait: restart it, then add each check's step. */
typedef struct { unsigned long long last, run; } gcm_run_clock;
static void gcm_run_clock_start(gcm_run_clock* c, unsigned long long now) { c->last = now; c->run = 0; }
static unsigned long long gcm_run_clock_ms(gcm_run_clock* c, unsigned long long now)
{
    const unsigned long long step = now > c->last ? now - c->last : 0;
    const unsigned cap = gcm_stall_gap_ms();
    c->last = now;
    if (!cap || step <= cap) c->run += step;
    else {
        static unsigned n;
        if (n++ < 16)
            fprintf(stderr, "[cellGcmSys] %llu ms with no drain pass (process stalled?) not counted "
                            "against a park/acquire bound\n", step);
    }
    return c->run;
}
/* GCM_PARK_STATS=1: every 30 s, how the parks went since the last line:
 * stops at a new park, parks left through their patch (and the longest wait
 * for one), parks left another way, jump-park resyncs. */
static unsigned s_ps_parks, s_ps_patched, s_ps_other, s_ps_resync;
static unsigned long long s_ps_max_ms, s_ps_t0;
static void gcm_park_stats(void)
{
    static int on = -1;
    if (on < 0) on = getenv("GCM_PARK_STATS") != NULL;
    if (!on) return;
    const unsigned long long now = ps3_ms_now();
    if (!s_ps_t0) s_ps_t0 = now;
    if (now - s_ps_t0 < 30000) return;
    fprintf(stderr, "[cellGcmSys] park stats: %u parks, %u patched (longest %llu ms), %u left otherwise, %u jump-park resyncs\n",
            s_ps_parks, s_ps_patched, s_ps_max_ms, s_ps_other, s_ps_resync);
    s_ps_parks = s_ps_patched = s_ps_other = s_ps_resync = 0; s_ps_max_ms = 0; s_ps_t0 = now;
}
/* The walker has left the park it was waiting on (s_park_off). `patched`: it
 * left from the park's own address, i.e. the word there is no longer the
 * self-JUMP (a JUMP elsewhere, or commands written over it). Say so when
 * the patch came later than the old fixed 50 ms grace, i.e. when that grace
 * would have resynced past it. */
static void gcm_park_left(int patched)
{
    extern unsigned long long ps3_ms_now(void);
    if (patched && s_park_since) {
        const unsigned long long waited = ps3_ms_now() - s_park_since;
        static unsigned n;
        if (waited >= 50 && n++ < 64)
            fprintf(stderr, "[cellGcmSys] park at 0x%08X patched after %llu ms\n", s_park_off, waited);
        if (waited > s_ps_max_ms) s_ps_max_ms = waited;
    }
    if (patched) s_ps_patched++; else s_ps_other++;
    gcm_park_stats();
    s_park_off = 0; s_park_since = 0;
}
/* How close `current` must get to `end` before the runtime will recycle a ring
 * on the title's behalf. One gcmReserve is 8 bytes; a page of slack keeps a
 * title that is merely near the end from being touched. */
#define GCM_RECYCLE_SLACK 0x1000u
/* How far past the ring's start the walker must have read before the runtime
 * will recycle on the title's behalf. The guest resumes writing at `begin`, so
 * this is the amount of already-consumed head room it gets. */
#define GCM_RECYCLE_MARGIN 0x20000u
static u32 gcm_ea2io(u32 ea);   /* defined with the wrap callback below */
static u32 s_fifo_getoff  = 0;
static u32 s_last_ring_off = 0;   /* last offset the walker actually consumed */
static u32 s_fifo_calloff = 0;

/* ---------------------------------------------------------------------------
 * 2D transfer engines (FIFO subchannels != 0).
 *
 * libgcm binds: sub 1/6 = NV0039 (m2mf buffer copy), sub 2 = NV3062 (context
 * surface 2D), sub 3 = NV309E (swizzled surface), sub 4 = NV308A (image from
 * CPU / inline transfer), sub 5 = NV3089 (scaled image).
 *
 * cellGcmSetFragmentProgramParameter patches a fragment program's inline
 * constants by emitting NV3062 (destination surface) + NV308A (COLOR data
 * words) through the FIFO -- it does NOT CPU-write the ucode copy. Without
 * this engine the patched constants stay zero in local memory (demosaic: all
 * its texture-size constants -> UVs scaled by 0 -> uniform output).
 * -----------------------------------------------------------------------*/
extern u32 cellGcmResolveLocated(int local, u32 offset);

static struct {
    u32 dst_dma;      /* NV3062 0x0188 SET_CONTEXT_DMA_IMAGE_DESTIN */
    u32 color_fmt;    /* NV3062 0x0300 SET_COLOR_FORMAT             */
    u32 pitch;        /* NV3062 0x0304 SET_PITCH (dst in bits 16-31) */
    u32 dst_offset;   /* NV3062 0x030C SET_OFFSET_DESTIN            */
    u32 point;        /* NV308A 0x0304 SET_POINT ((y<<16)|x)        */
    u32 size_out;     /* NV308A 0x0308 SET_SIZE_OUT                 */
} s_gcm2d;

static struct {
    u32 src_dma, dst_surf, fmt, op, clip_pt, clip_sz, out_pt, out_sz;
    u32 ds_dx, dt_dy, in_sz, in_fmt, in_off, in_uv;
} s_nv3089;

/* NV309E "swizzled surface" -- the destination object for a scaled-image blit
 * whose target is a swizzled (Morton-ordered) texture rather than a linear
 * surface. Rubber Ducky binds it to the same subchannel this file previously
 * assumed was always NV308A (image-from-CPU), so its SET_OFFSET was being read
 * as an NV308A "point" and the blit destination was never picked up at all.
 * The two are told apart by method 0x0300: NV308A has no such method. */
static struct {
    u32 fmt;        /* 0x0300: (log2h << 24) | (log2w << 16) | format */
    u32 offset;     /* 0x0304: destination offset                     */
    int active;
    int subch;      /* which subchannel NV309E is bound to. Per-subchannel:
                     * this title has NV309E on 4 and NV308A on 5, and a global
                     * flag made 5's NV308A POINT (also 0x0304) be swallowed as
                     * an NV309E offset, breaking every inline transfer. */
} s_nv309e;

/* Execute one NV3089 blit into the currently-bound NV3062 destination surface.
 * Point-sampled: ds/dx and dt/dy are 20.12 fixed point and are 0x100000 (1.0)
 * for the straight uploads this exists for; a scaled blit still lands, just
 * without filtering. */
/* Morton/Z-order offset for an NV309E swizzled surface: interleave the low
 * min(l2w,l2h) bits of x and y, then append the remaining bits of the larger
 * dimension above them. */
static u32 nv309e_swz(u32 x, u32 y, u32 l2w, u32 l2h)
{
    u32 n = l2w < l2h ? l2w : l2h, off = 0;
    for (u32 i = 0; i < n; i++)
        off |= (((x >> i) & 1u) << (2 * i)) | (((y >> i) & 1u) << (2 * i + 1));
    if (l2w > l2h)      off |= (x >> n) << (2 * n);
    else if (l2h > l2w) off |= (y >> n) << (2 * n);
    return off;
}

/* A guest-memory row copy with the same lock-line bookkeeping as vm_write32
 * (ppu_loader.cpp VM_WRITE_COH), per 128-byte line instead of per pixel: an
 * SPU holding a reservation on a line the row overwrites loses it and gets
 * its lost-reservation event. The two byte swaps a vm_read32/vm_write32 pair
 * does cancel, so the bytes move verbatim. */
static void gcm_row_copy(u32 d, u32 s, u32 n)
{
    extern u8* vm_base;
    const u32 l0 = d & ~127u, l1 = (d + n - 1) & ~127u;
    u64 mask = 0; int locked = 0;
    for (u32 a = l0; a <= l1; a += 128)
        if (spu_coh_is_reserved(a)) {
            mask = spu_lockline_mask_range(d, n); spu_lockline_lock_mask(mask); locked = 1; break; }
    memmove(vm_base + d, vm_base + s, n);
    __atomic_signal_fence(__ATOMIC_SEQ_CST);
    for (u32 a = l0; a <= l1; a += 128)
        if (spu_coh_is_reserved(a)) {
            if (!locked) { mask = spu_lockline_mask_range(d, n); spu_lockline_lock_mask(mask); locked = 1; }
            spu_coh_notify_write_from_ppu(a);
        }
    if (locked) spu_lockline_unlock_mask(mask);
}

/* IMAGE_IN_POINT is 12.4 fixed point per axis, and DS_DX / DT_DY step the
 * source with 20 fractional bits (1.0 = 0x100000), so the origin in the
 * steps' scale is origin << 16: a one-texel origin (0x10) is 0x100000. It was
 * `<< 8`, 256 times too small, which read a one-texel origin as texel 0 (as
 * sp00nznet/ps3recomp#358). An origin below one texel (0x0-0xF, the usual
 * corner or half-texel centre) truncates to texel 0 either way. */
static inline u64 nv3089_origin(u32 p) { return (u64)p << 16; }

static void nv3089_blit(void)
{
    u32 out_w = s_nv3089.out_sz & 0xFFFF, out_h = s_nv3089.out_sz >> 16;
    u32 out_x = s_nv3089.out_pt & 0xFFFF, out_y = s_nv3089.out_pt >> 16;
    u32 in_pitch = s_nv3089.in_fmt & 0xFFFF;
    u32 dst_pitch = s_gcm2d.pitch >> 16;
    if (!out_w || !out_h || !in_pitch || !dst_pitch) return;
    /* The source may be a target the register-file engine drew, which exists
     * only on the GPU until it is written back: bring guest memory up to date
     * before the copies below read it. */
    { static int eng = -1;
      if (eng < 0) eng = rsx_draw_engine_enabled();
      if (eng) {
          const int dl = s_gcm2d.dst_dma != 0xFEED0001u;
          const u32 doff = s_gcm2d.dst_offset + (s_nv3089.out_pt >> 16) * dst_pitch
                                              + (s_nv3089.out_pt & 0xFFFF) * 4u;
          rsx_draw_engine_write_back_to(s_nv3089.src_dma != 0xFEED0001u, s_nv3089.in_off, in_pitch, dl, doff);
          /* A copy into a GPU light input that nothing reads any more: the
           * engine took its GPU snapshot above, the guest bytes are dead
           * (PS3RECOMP_ENG_GPU_LIGHT_SKIPCOPY, rsx_draw_engine.c). */
          if (rsx_draw_engine_gpu_light_dst_dead(dl, doff, in_pitch)) return;
      } }
    /* NV3089_GATE=<file>: log every blit's geometry while <file> exists, so a
     * capture covers the scene of interest instead of the first N since boot. */
    { static const char* gate = (const char*)-1; static unsigned poll; static int open_gate;
      if (gate == (const char*)-1) gate = getenv("NV3089_GATE");
      if (gate) {
          if ((poll++ & 63u) == 0) { FILE* g = fopen(gate, "rb"); open_gate = g != NULL; if (g) fclose(g); }
          if (open_gate) {
              int _sl = (s_nv3089.src_dma != 0xFEED0001u), _dl = (s_gcm2d.dst_dma != 0xFEED0001u);
              printf("[NV3089G] fmt=0x%X out=%ux%u at %u,%u in=%ux%u uv=0x%08X ds=0x%X dt=0x%X src=0x%08X/%u dst=0x%08X/%u swz=%d%c",
                     s_nv3089.fmt & 0xFF, out_w, out_h, out_x, out_y,
                     s_nv3089.in_sz & 0xFFFF, s_nv3089.in_sz >> 16, s_nv3089.in_uv,
                     s_nv3089.ds_dx, s_nv3089.dt_dy,
                     cellGcmResolveLocated(_sl, s_nv3089.in_off), in_pitch,
                     cellGcmResolveLocated(_dl, s_gcm2d.dst_offset), dst_pitch,
                     s_nv309e.active, 10);
          }
      } }
    /* A copy out of a surface the live renderer drew exists only on the GPU;
     * mirror 1:1 blits there too (the guest-memory copy below still runs). */
    if (s_nv3089.ds_dx == 0x100000u && s_nv3089.dt_dy == 0x100000u &&
        rsx_live_draw_enabled()) {
        const u32 u0 = (s_nv3089.in_uv & 0xFFFF) >> 4, v0 = (s_nv3089.in_uv >> 16) >> 4;
        const int sl = (s_nv3089.src_dma != 0xFEED0001u);
        const int dl = (s_gcm2d.dst_dma  != 0xFEED0001u);
        rsx_live_draw_blit(sl ? 0u : 1u, s_nv3089.in_off + v0 * in_pitch + u0 * 4u, in_pitch,
                           dl ? 0u : 1u, s_gcm2d.dst_offset + (s_nv3089.out_pt >> 16) * dst_pitch
                                         + (s_nv3089.out_pt & 0xFFFF) * 4u, dst_pitch,
                           out_w, out_h);
    } else if (rsx_live_draw_enabled()) {
        /* Scaled: a resolve. Copy it on the GPU into the display buffer; fall
         * back to presenting the source when that cannot be done. */
        const u32 sl = s_nv3089.src_dma != 0xFEED0001u ? 0u : 1u;
        const u32 dl = s_gcm2d.dst_dma  != 0xFEED0001u ? 0u : 1u;
        const u32 u0 = (s_nv3089.in_uv & 0xFFFF) >> 4, v0 = (s_nv3089.in_uv >> 16) >> 4;
        const int rok = rsx_live_draw_resolve_blit(sl, s_nv3089.in_off + v0 * in_pitch + u0 * 4u, in_pitch,
                                        s_nv3089.ds_dx, s_nv3089.dt_dy, dl, s_gcm2d.dst_offset,
                                        dst_pitch, out_x, out_y, out_w, out_h);
        if (!rok && s_nv3089.in_uv <= 0x00100010u && !s_nv3089.out_pt)
            rsx_live_draw_note_resolve(sl, s_nv3089.in_off, dl, s_gcm2d.dst_offset);
    }
    /* Only the 32-bit colour formats are handled; anything else would need a
     * per-format converter and is better skipped loudly than written wrong. */
    u32 f = s_nv3089.fmt & 0xFF;
    /* NOTE: format 0x3 appears here as a downsample chain (1024x512 -> 1024x256
     * -> 512x128 -> 256x64 -> 128x32, all into one destination) that builds a
     * mip/blur pyramid, and skipping it leaves that texture empty. Treating it
     * as 4-byte A8R8G8B8 like 7/8/0xD is WRONG -- it floods the scene blue
     * (flat-blue coverage 17k -> 537k pixels), so its pixel layout differs.
     * Decode it properly before enabling; do not just add it to this list. */
    /* NV309E swizzled destination. Colour format 0x3 is A8R8G8B8 in the
     * scaled-image enum; this title uses it to build a mip/blur pyramid into a
     * swizzled texture (1024x512 -> 1024x256 -> ... all through NV309E).
     * Writing it LINEARLY into the NV3062 destination is what flooded the scene
     * blue -- wrong destination and wrong layout. */
    { u32 l2w = (s_nv309e.fmt >> 16) & 0xFF, l2h = (s_nv309e.fmt >> 24) & 0xFF;
      static int en = -1;
      /* On by default: this is the mip/blur pyramid. Without it textures have
       * no mip levels, which shows up as aliased blocky tiles and a noise patch
       * where a minified surface should be, and the shell that samples the
       * pyramid draws untextured. NV309E_BLIT=0 disables. */
      if (en < 0) { const char* e = getenv("NV309E_BLIT"); en = e ? atoi(e) : 1; }
      if (en && f == 3 && s_nv309e.active && s_nv309e.offset &&
          (1u << l2w) == out_w && (1u << l2h) == out_h) {
        int sl = (s_nv3089.src_dma != 0xFEED0001u);
        u32 src2 = cellGcmResolveLocated(sl, s_nv3089.in_off);
        u32 dst2 = cellGcmResolveLocated(1, s_nv309e.offset);
        u32 u0b = s_nv3089.in_uv & 0xFFFF, v0b = s_nv3089.in_uv >> 16;
        /* Copy words verbatim through raw pointers rather than vm_read32/
         * vm_write32: this moves ~500k pixels per blit and several blits run per
         * frame, and the two byte swaps would cancel anyway. */
        { extern u8* vm_base;
          const u8* sbase = vm_base + src2;
          u8* dbase = vm_base + dst2;
          /* NV309E_FAST (default 1): the swizzle is separable -- the x bits and
           * the y bits of nv309e_swz never share a position, so
           * swz(x,y) = swz(x,0) | swz(0,y) -- and each row's x part is the same
           * table: one table lookup and one OR per pixel instead of a bit loop
           * per pixel (the RSX thread's 4th-largest cost in the docks walk).
           * Same words to the same places; NV309E_FAST=2 also runs the bit loop
           * and logs any pixel whose offset differs. =0: the old loop. */
          static int fast = -1;
          if (fast < 0) { const char* e = getenv("NV309E_FAST"); fast = e ? atoi(e) : 1; }
          static u32 s_xo[4096];
          if (fast && out_w <= 4096u) {
              for (u32 x = 0; x < out_w; x++) s_xo[x] = nv309e_swz(x, 0, l2w, l2h);
              const u64 su0 = nv3089_origin(u0b);
              const int unit = s_nv3089.ds_dx == 0x100000u;
              for (u32 y = 0; y < out_h; y++) {
                  const u64 sv = nv3089_origin(v0b) + (u64)y * s_nv3089.dt_dy;
                  const u8* srow = sbase + (u32)(sv >> 20) * in_pitch;
                  const u32 yo = nv309e_swz(0, y, l2w, l2h);
                  if (unit) {
                      const u8* s = srow + (u32)(su0 >> 20) * 4;
                      for (u32 x = 0; x < out_w; x++) {
                          u32 v; memcpy(&v, s + (u64)x * 4, 4);
                          memcpy(dbase + (u64)(s_xo[x] | yo) * 4, &v, 4);
                      }
                  } else {
                      for (u32 x = 0; x < out_w; x++) {
                          const u64 su = su0 + (u64)x * s_nv3089.ds_dx;
                          u32 v; memcpy(&v, srow + (u32)(su >> 20) * 4, 4);
                          memcpy(dbase + (u64)(s_xo[x] | yo) * 4, &v, 4);
                      }
                  }
                  if (fast == 2) {
                      static int bad;
                      for (u32 x = 0; x < out_w; x++)
                          if ((s_xo[x] | yo) != nv309e_swz(x, y, l2w, l2h) && PS3_LOG_FIRST(bad, 8))
                              fprintf(stderr, "[NV309E_FAST] offset mismatch x=%u y=%u l2w=%u l2h=%u\n", x, y, l2w, l2h);
                  }
              }
          } else
          for (u32 y = 0; y < out_h; y++) {
              u64 sv = nv3089_origin(v0b) + (u64)y * s_nv3089.dt_dy;
              const u8* srow = sbase + (u32)(sv >> 20) * in_pitch;
              for (u32 x = 0; x < out_w; x++) {
                  u64 su = nv3089_origin(u0b) + (u64)x * s_nv3089.ds_dx;
                  memcpy(dbase + (u64)nv309e_swz(x, y, l2w, l2h) * 4,
                         srow + (u32)(su >> 20) * 4, 4);
              }
          } }
        { static int _n = 0; static int cap = -1;
          if (cap < 0) { const char* e = getenv("NV3089_DBG"); cap = e ? atoi(e) : 0; }
          if (cap && _n < cap) { _n++;
            printf("[NV3089] swizzled %ux%u fmt=0x%X src=0x%08X -> NV309E dst=0x%08X%c",
                   out_w, out_h, f, src2, dst2, 10); } }
        return;
      } }
    /* 0x3 is A8R8G8B8 in the scaled-image format enum. This title uses it for
     * two different things: the swizzled mip/blur pyramid handled above, and
     * (with a LINEAR NV3062 destination) copies of the rendered scene surface
     * into the textures its water and effects sample -- e.g.
     * src=0xCE340000 (the scene) -> dst=0xC3746200, which is the single most
     * sampled blended texture in the frame. Skipping those left every one of
     * those textures empty. Same 4-byte pixel layout as the formats already
     * handled. */
    if (f != 3 /* A8R8G8B8 (scale-format enum) */ &&
        f != 7 /* A8R8G8B8 */ && f != 8 /* X8R8G8B8 */ && f != 0xD /* A8B8G8R8 */) {
        /* Log the GEOMETRY of a skipped blit, not just the format: a full-screen
         * copy from the scene surface to a registered display buffer is the
         * composite this title needs, and is worth telling apart from some
         * incidental small transfer. */
        { int _sl = (s_nv3089.src_dma != 0xFEED0001u);
          int _dl = (s_gcm2d.dst_dma  != 0xFEED0001u);
          printf("[NV3089] SKIPPED fmt=0x%X  %ux%u  src=0x%08X(pitch %u,%s) -> dst=0x%08X(pitch %u,%s) at %u,%u%c",
                 f, out_w, out_h,
                 cellGcmResolveLocated(_sl, s_nv3089.in_off), in_pitch, _sl?"LOCAL":"MAIN",
                 cellGcmResolveLocated(_dl, s_gcm2d.dst_offset), dst_pitch, _dl?"LOCAL":"MAIN",
                 out_x, out_y, 10); }
        static int _w = 0;
        if (PS3_LOG_FIRST(_w, 4)) printf("[NV3089] unsupported colour format 0x%X, blit skipped\n", f);
        return;
    }
    int src_local = (s_nv3089.src_dma != 0xFEED0001u);
    int dst_local = (s_gcm2d.dst_dma  != 0xFEED0001u);
    u32 src = cellGcmResolveLocated(src_local, s_nv3089.in_off);
    u32 dst = cellGcmResolveLocated(dst_local, s_gcm2d.dst_offset);
    u32 u0 = s_nv3089.in_uv & 0xFFFF, v0 = s_nv3089.in_uv >> 16;   /* 12.4 start */
    /* Clip to SET_CLIP_POINT/SIZE (destination coordinates). A title tiling a
     * resolve in 512x512 blocks relies on it to stop at the surface edge:
     * Tornado Outbreak's bottom band is 512 rows tall with 208 left in the
     * display buffer, and writing all 512 ran on into the VRAM after it --
     * where its full-screen quad's vertices were staged, so the quad drew with
     * all-zero positions. A zero clip size means unclipped. */
    u32 cx0 = s_nv3089.clip_pt & 0xFFFF, cy0 = s_nv3089.clip_pt >> 16;
    u32 cx1 = (s_nv3089.clip_sz & 0xFFFF) ? cx0 + (s_nv3089.clip_sz & 0xFFFF) : 0xFFFFFFFFu;
    u32 cy1 = (s_nv3089.clip_sz >> 16)    ? cy0 + (s_nv3089.clip_sz >> 16)    : 0xFFFFFFFFu;
    /* Bytes per pixel from the SDK enums (CELL_GCM_TRANSFER_SCALE_FORMAT_*:
     * 7 = R5G6B5, 8 = Y8 -- not 32-bit as they were once labelled). When the
     * destination surface format agrees it is a plain copy of that width; a
     * real format conversion keeps the old 4-byte copy rather than guess. */
    u32 sbpp = (f == 7) ? 2u : (f == 8) ? 1u : 4u;
    u32 dbpp = (s_gcm2d.color_fmt == 4u /* R5G6B5 */) ? 2u
             : (s_gcm2d.color_fmt == 1u /* Y8 */)     ? 1u : 4u;
    u32 bpp = (sbpp == dbpp) ? sbpp : 4u;
    /* NV3089_ROWCOPY (default 1): an unscaled blit goes a row at a time. The
     * per-pixel loop below paid a vm_read32 and a vm_write32 per pixel -- OOB
     * and null-store checks, the store watch, the reservation lookup and two
     * byte swaps that cancel -- and inFAMOUS 2 copies its 1280x720 G-buffer
     * surfaces to XDR this way every frame for the SPU lighting: a quarter of
     * the RSX thread's time. NV3089_ROWCOPY=0 keeps the per-pixel copy (it is
     * what the PPU_WWATCH / PPU_WVAL store diagnostics see). */
    { static int rc = -1;
      if (rc < 0) { const char* e = getenv("NV3089_ROWCOPY"); rc = e ? atoi(e) : 1; }
      if (rc && s_nv3089.ds_dx == 0x100000u && s_nv3089.dt_dy == 0x100000u) {
          /* the integer source start the per-pixel loop derives for x = y = 0 */
          const u32 sx0 = (u32)(nv3089_origin(u0) >> 20), sy0 = (u32)(nv3089_origin(v0) >> 20);
          u32 xa = 0, xb = out_w;                       /* destination x range after the clip */
          if (out_x < cx0) xa = cx0 - out_x;
          if (out_x + xb > cx1) xb = cx1 > out_x ? cx1 - out_x : 0;
          if (xa < xb) {
              const u32 n = (xb - xa) * bpp;
              for (u32 y = 0; y < out_h; y++) {
                  const u32 dy = out_y + y;
                  if (dy < cy0 || dy >= cy1) continue;
                  const u64 sa = (u64)src + (u64)(sy0 + y) * in_pitch + (u64)(sx0 + xa) * bpp;
                  const u64 da = (u64)dst + (u64)dy * dst_pitch + (u64)(out_x + xa) * bpp;
                  if (sa + n > 0x100000000ull || da + n > 0x100000000ull) continue;
                  gcm_row_copy((u32)da, (u32)sa, n);
              }
          }
          goto blit_done;
      } }
    for (u32 y = 0; y < out_h; y++) {
        u32 dy = out_y + y;
        if (dy < cy0 || dy >= cy1) continue;
        u64 sv = nv3089_origin(v0) + (u64)y * s_nv3089.dt_dy;   /* 12.20 */
        u32 sy = (u32)(sv >> 20);
        for (u32 x = 0; x < out_w; x++) {
            u32 dx = out_x + x;
            if (dx < cx0 || dx >= cx1) continue;
            u64 su = nv3089_origin(u0) + (u64)x * s_nv3089.ds_dx;
            u32 sx = (u32)(su >> 20);
            u32 s = src + sy * in_pitch + sx * bpp;
            u32 d = dst + dy * dst_pitch + dx * bpp;
            if (bpp == 4)      vm_write32(d, vm_read32(s));
            else if (bpp == 2) vm_write16(d, vm_read16(s));
            else               vm_write8(d, vm_read8(s));
        }
    }
blit_done:
    { static int _n = 0; static int cap = -1;
      if (cap < 0) { const char* e = getenv("NV3089_DBG"); cap = e ? atoi(e) : 0; }
      if (cap && _n < cap) { _n++;
        printf("[NV3089] %ux%u fmt=0x%X src=0x%08X(pitch %u) -> dst=0x%08X(pitch %u) at %u,%u\n",
               out_w, out_h, f, src, in_pitch, dst, dst_pitch, out_x, out_y); } }
}

static void gcm_2d_method(u32 subch, u32 method, u32 data)
{
    /* GCM2D_TRACE=1: histogram of (subchannel, method) actually seen. The
     * subchannel a title binds each 2D object to is libgcm-version-specific and
     * SET_OBJECT binds are not tracked, so state landing on an unexpected
     * subchannel is silently dropped -- which looks like a blit using stale
     * destination state rather than like missing state. */
    { static int tr = -1;
      if (tr < 0) { const char* e = getenv("GCM2D_TRACE"); tr = e ? atoi(e) : 0; }
      if (tr) {
          static u8 seen[8][1024]; static u64 n = 0;
          u32 mi = (method >> 2) & 1023;
          if (subch < 8 && !seen[subch][mi]) {
              seen[subch][mi] = 1;
              printf("[GCM2D-TRACE] subch=%u method=0x%04X (first, data=0x%08X)%c",
                     subch, method, data, 10);
          }
          if (++n == 400000ull) {
              printf("[GCM2D-TRACE] --- subchannels used: ");
              for (u32 sc = 0; sc < 8; sc++) {
                  u32 c = 0; for (u32 m = 0; m < 1024; m++) c += seen[sc][m];
                  if (c) printf("%u(%u methods) ", sc, c);
              }
              printf("---%c", 10);
          }
      } }
    /* Subchannel bindings are libgcm-version-specific (SET_OBJECT binds are
     * not tracked): demosaic's SDK emits dest-surface on sub 3 and image-from-
     * CPU on sub 5 (cellGcmSetInlineTransfer: 0x4630C / 0xCA304 / 0xA400
     * headers); other versions use the classic 2 / 4. Accept both. */
    if (subch == 2 || subch == 3) {         /* NV3062 context surface 2D */
        switch (method) {
        case 0x0184: case 0x0188: s_gcm2d.dst_dma = data; return;
        case 0x0300: s_gcm2d.color_fmt  = data; return;
        case 0x0304: s_gcm2d.pitch      = data; return;
        case 0x030C: s_gcm2d.dst_offset = data; return;
        }
        return;
    }
    if (subch == 4 || subch == 5) {         /* NV308A image-from-CPU, or NV309E */
        /* NV309E swizzled-surface state. SET_FORMAT (0x0300) does not exist on
         * NV308A, so seeing it identifies the object bound here. */
        if (method == 0x0300) { s_nv309e.fmt = data; s_nv309e.active = 1;
                                s_nv309e.subch = (int)subch;
            { static int _n = 0; if (PS3_LOG_FIRST(_n, 4))
                printf("[NV309E] format=0x%08X (log2 %ux%u fmt=0x%X)%c", data,
                       1u << ((data >> 16) & 0xFF), 1u << ((data >> 24) & 0xFF),
                       data & 0xFFFF, 10); }
            return; }
        if (method == 0x0304 && s_nv309e.active && s_nv309e.subch == (int)subch) {
            s_nv309e.offset = data; return; }
        switch (method) {
        case 0x0304: s_gcm2d.point    = data; return;
        case 0x0308: s_gcm2d.size_out = data; return;
        case 0x030C: return;                /* SIZE_IN */
        }
        if (method >= 0x0400 && method <= 0x07FC) {
            /* COLOR data word: write into the destination surface. Inline
             * transfers use format Y32 (4 bytes/px, one row per point.y).
             * DMA 0xFEED0000 = local memory, 0xFEED0001 = main memory. */
            u32 idx = (method - 0x0400) >> 2;
            u32 px  = (s_gcm2d.point & 0xFFFF) + idx;
            u32 py  = s_gcm2d.point >> 16;
            u32 dst_pitch = s_gcm2d.pitch >> 16;
            int local = (s_gcm2d.dst_dma != 0xFEED0001u);
            u32 base = cellGcmResolveLocated(local, s_gcm2d.dst_offset);
            { static int _it = 0;
              if (PS3_LOG_FIRST(_it, 6))
                  printf("[GCM2D] inline write dst=0x%08X (off=0x%X dma=0x%X pt=%u,%u pitch=%u) = 0x%08X\n",
                         base + py * dst_pitch + px * 4, s_gcm2d.dst_offset,
                         s_gcm2d.dst_dma, px, py, dst_pitch, data); }
            vm_write32(base + py * dst_pitch + px * 4, data);
            return;
        }
        return;
    }
    /* NV3089 "scaled image from memory" -- the blit PSGL uses to move a texture
     * it has assembled elsewhere into the VRAM the sampler reads. Without it a
     * title's textures stay whatever the destination was before (zero), so
     * geometry renders flat no matter how correct the sampler is.
     *
     * Registers: 0x0184 source DMA, 0x0198 destination surface (the NV3062
     * state above), 0x0300 colour format, 0x0304 operation, 0x0308/0x030C clip
     * point/size, 0x0310/0x0314 out point/size, 0x0318/0x031C ds/dx dt/dy in
     * 20.12 fixed point, 0x0400 in size, 0x0404 (origin<<16)|pitch, 0x0408 in
     * offset, 0x040C in u/v start -- which is also the trigger. */
    if (subch == 6 || subch == 7) {
        switch (method) {
        case 0x0184: s_nv3089.src_dma   = data; return;
        case 0x0198: s_nv3089.dst_surf  = data; return;
        case 0x0300: s_nv3089.fmt       = data; return;
        case 0x0304: s_nv3089.op        = data; return;
        case 0x0308: s_nv3089.clip_pt   = data; return;
        case 0x030C: s_nv3089.clip_sz   = data; return;
        case 0x0310: s_nv3089.out_pt    = data; return;
        case 0x0314: s_nv3089.out_sz    = data; return;
        case 0x0318: s_nv3089.ds_dx     = data; return;
        case 0x031C: s_nv3089.dt_dy     = data; return;
        case 0x0400: s_nv3089.in_sz     = data; return;
        case 0x0404: s_nv3089.in_fmt    = data; return;
        case 0x0408: s_nv3089.in_off    = data; return;
        case 0x040C: s_nv3089.in_uv     = data; nv3089_blit(); return;
        }
        return;
    }
    /* NV0039 / NV309E: not implemented yet -- log first sightings. */
    /* GCM2D_DBG=<N> raises the cap and adds the data word: the fixed 8 showed
     * only that SOMETHING was unhandled, not enough to implement it. */
    static int warned = 0, wcap = -1;
    if (wcap < 0) { const char* e = getenv("GCM2D_DBG"); wcap = e ? atoi(e) : 8; }
    if (warned < wcap) { warned++;
        printf("[GCM2D-UNH] subch %u method 0x%04X data=0x%08X\n", subch, method, data); }
    if (0)
        printf("[cellGcmSys] FIFO subch %u method 0x%04X (unhandled 2D engine)\n",
               subch, method);
}

/* Present gate for the ticker: a flip is ready once the drain has consumed
 * everything up to put. The guest blocks in its own WaitFlip right after
 * flipping, so the FIFO holds EXACTLY the completed frame -- no guest-side
 * blocking needed (the old spin serialized two vsync-class waits per frame
 * and halved the frame rate). Call AFTER draining. */
int cellGcm_take_flip_pending_synced(void)
{
    if (!s_flip_pending) return 0;
    u32 put = vm_read32(GCM_CONTROL_GUEST_ADDR + 0);
    u32 get = vm_read32(GCM_CONTROL_GUEST_ADDR + 4);
    if (get != put) return 0;   /* frame not fully drained yet */
    s_flip_pending = 0;
    s_flips_shown++;
    return 1;
}

/* ---- fence (SET_REFERENCE) observability ---------------------------------
 * cellGcmFinish waits for ctrl->ref with EQUALITY. On hardware every fence
 * value persists in the register for the RSX-time until the next fence
 * (~a frame), so an equality poll cannot miss it. Our drain consumes a whole
 * backlog in microseconds; publishing only the final value made intermediate
 * fences invisible -- LBP's boot deadlocked with a thread spinning on a fence
 * the counter had already passed (waiting 0x13 while ref climbed 0x51->0x1F4).
 * Queue every fence as it is drained and publish ONE per tick: the GPU work
 * is never throttled (draw/recycle proceed at full speed), only the ref
 * register advances at most one fence per tick, so every value is observable
 * for >= 16 ms, like hardware. (Throttling the DRAIN at fence boundaries
 * instead starved wrap recycles and wedged the backend -- don't.) */
#define GCM_REF_QLEN 4096
static u32 s_ref_q[GCM_REF_QLEN];
static volatile u32 s_ref_qhead = 0, s_ref_qtail = 0;   /* single producer+consumer: the ticker */

/* A JUMP/CALL whose target has no IO mapping must not be taken. Taking one
 * teleports the walker into unmapped space, where it can never reach `put`
 * again: the FIFO silently stops being processed, no fences are published, and
 * a title spinning on ctrl->ref waits forever with no indication why. Tokyo
 * Jungle did exactly this -- get left the 1 MB ring for IO 0x00F00000, which it
 * never mapped, while put sat at 0x604. Stop at the bad word and say so. */
static void gcm_fifo_bad_branch(const char* kind, u32 target, u32 word)
{
    static int n = 0;
    if (PS3_LOG_FIRST(n, 8))
        fprintf(stderr, "[cellGcmSys] FIFO %s to unmapped IO 0x%08X (word 0x%08X) "
                "-- not taken, drain stops here\n", kind, target, word);
}

/* A bad branch nearly always means the walker mis-counted a method batch a few
 * dwords back and is now reading vertex data -- a float 1.0f is 0x3F800000,
 * which decodes as JUMP 0x1F800000. Dump the run-up: the command that consumed
 * the wrong count is in here. */
/* Last words the walker actually consumed, so a bad branch can show the decode
 * chain that led there rather than raw memory around it. */
#define GCM_TRACE_N 64
static u32 s_tr_off[GCM_TRACE_N], s_tr_w[GCM_TRACE_N];
static u32 s_tr_i = 0;

/* A branch we cannot take leaves the walker pointing at whatever it
 * mis-decoded, and it never recovers -- get freezes for the rest of the
 * run and nothing renders. Resynchronise to `put` instead, the same trade
 * the unmapped-get path already makes: the commands between here and put
 * are lost, but the next frame is written from a clean boundary and the
 * FIFO lives. */
static void gcm_ref_push_at(u32 v, u32 getoff);

/* ---- SPU-built pushbuffers ------------------------------------------------
 * Some titles do not build the RSX command stream on the PPU at all. The
 * Orange Box is one: its libgcm context points at a 64 KB buffer in the
 * binary's own .bss that is never mapped into IO space, because that buffer is
 * where the PPU records draw commands for an SPU to consume. CB.SPU reads it
 * and DMAs the real pushbuffer into mapped main memory, then `put` is set past
 * the end of what it wrote.
 *
 * So libgcm is never told where the stream is, and the walker -- which starts
 * at IO 0 and has no JUMP to follow, because the SPU side did the binding --
 * scans tens of megabytes of unrelated memory, NOPs through the zeros (a zero
 * word is a valid NOP) and dies on the first vertex float it meets.
 *
 * The DMA is the only evidence there is, so record it. Each PUT that lands in
 * a mapped IO range is a candidate pushbuffer; the one that ends just short of
 * `put` is the block the RSX is actually being pointed at. */
#define GCM_SPU_BLK_N 16
static struct { u32 io_start, io_end; } s_spu_blk[GCM_SPU_BLK_N];
static int s_spu_blk_w = 0;

/* EA -> IO through the mapping list (range lookup; the by-ea helper is exact). */
static u32 gcm_ea2io_range(u32 ea, u32* room)
{
    for (int i = 0; i < CELL_GCM_MAX_IO_MAPPINGS; i++) {
        IoMapping* m = &s_io_mappings[i];
        if (!m->active || !m->size) continue;
        if (ea >= m->ea && ea < m->ea + m->size) {
            if (room) *room = m->ea + m->size - ea;
            return m->io + (ea - m->ea);
        }
    }
    return 0xFFFFFFFFu;
}

/* (critic2) g_spu_put_rsx_hook: does [ea, ea+size) touch IO-mapped memory (what
 * the RSX can read: the command ring, SPU-built command lists, vertex data)?
 * Read from SPU threads; the table changes only on a map/unmap call, and a
 * stale answer only picks the copy order. */
int gcm_spu_put_is_rsx(u32 ea, u32 size)
{
    for (int i = 0; i < CELL_GCM_MAX_IO_MAPPINGS; i++) {
        const IoMapping* m = &s_io_mappings[i];
        if (!m->active || !m->size) continue;
        if (ea < m->ea + m->size && ea + size > m->ea) return 1;
    }
    return 0;
}

/* (perf17) The SPU side's inline form of the test above (spu_dma.h
 * spu_put_is_rsx): the mappings' union as [lo, hi) when it is one interval
 * (exact), else their bounding box with the hook deciding inside it. Called
 * after every change to s_io_mappings. s_spu_put_mode: 0 = PUT_DESC off (never
 * RSX), 1 = this interval (PS3RECOMP_SPU_PUT_INLINE, default), 2 = the hook for
 * every PUT (PS3RECOMP_SPU_PUT_INLINE=0, as m17). */
static void gcm_spu_put_box_update(void)
{
    extern u32 g_spu_put_rsx_lo, g_spu_put_rsx_hi;
    extern int g_spu_put_rsx_exact;
    u32 lo = 0, hi = 0; int exact = 0;
    if (s_spu_put_mode == 2) { lo = 0; hi = 0xFFFFFFFFu; exact = 0; }
    else if (s_spu_put_mode == 1) {
        /* the active ranges sorted by ea, merged; one left = exact */
        u32 rs[CELL_GCM_MAX_IO_MAPPINGS], re[CELL_GCM_MAX_IO_MAPPINGS]; int n = 0;
        for (int i = 0; i < CELL_GCM_MAX_IO_MAPPINGS; i++) {
            const IoMapping* m = &s_io_mappings[i];
            if (!m->active || !m->size) continue;
            const u64 e64 = (u64)m->ea + m->size;
            const u32 s = m->ea, e = e64 > 0xFFFFFFFFull ? 0xFFFFFFFFu : (u32)e64;
            int k = n++;
            while (k > 0 && rs[k - 1] > s) { rs[k] = rs[k - 1]; re[k] = re[k - 1]; k--; }
            rs[k] = s; re[k] = e;
        }
        int pieces = 0; u32 cur_e = 0;
        for (int k = 0; k < n; k++) {
            if (k == 0 || rs[k] > cur_e) pieces++;
            if (k == 0 || re[k] > cur_e) cur_e = re[k];
        }
        if (n) { lo = rs[0]; hi = cur_e; exact = pieces == 1; }
    }
    /* exact off first, on last: a reader racing a map/unmap call may mix the
     * old and new interval for an instant, which (as a stale hook answer)
     * only picks the copy order of that one PUT */
    __atomic_store_n(&g_spu_put_rsx_exact, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_spu_put_rsx_lo, lo, __ATOMIC_RELEASE);
    __atomic_store_n(&g_spu_put_rsx_hi, hi, __ATOMIC_RELEASE);
    __atomic_store_n(&g_spu_put_rsx_exact, exact, __ATOMIC_RELEASE);
    { static int _n = 0;
      if (s_spu_put_mode && PS3_LOG_FIRST(_n, 16))
          fprintf(stderr, "[cellGcmSys] SPU PUT RSX range 0x%08X-0x%08X %s (mode %d)\n", lo, hi,
                  exact ? "exact" : "box, hook inside", s_spu_put_mode); }
}

void gcm_forget_spu_blocks(void)
{
    memset(s_spu_blk, 0, sizeof s_spu_blk);
    s_spu_blk_w = 0;
}

static void gcm_sema_kick_if(u32 ea, u32 size);   /* (critic) below, with the kick event */
void gcm_note_spu_put(u32 ea, u32 size)
{
    gcm_sema_kick_if(ea, size);   /* (critic) PS3RECOMP_GCM_SEMA_KICK: a label the walker waits on */
    if (!size || size > 0x40000u) return;
    u32 room = 0;
    u32 io = gcm_ea2io_range(ea, &room);
    if (io == 0xFFFFFFFFu || size > room) return;   /* not in RSX-visible memory */

    /* Coalesce with the previous block when an SPU emits one buffer in several
     * transfers -- otherwise only the last fragment is remembered and the
     * walker still starts in the middle of the stream. */
    int prev = (s_spu_blk_w + GCM_SPU_BLK_N - 1) % GCM_SPU_BLK_N;
    if (s_spu_blk[prev].io_end == io) { s_spu_blk[prev].io_end = io + size; return; }

    s_spu_blk[s_spu_blk_w].io_start = io;
    s_spu_blk[s_spu_blk_w].io_end   = io + size;
    s_spu_blk_w = (s_spu_blk_w + 1) % GCM_SPU_BLK_N;
}

/* The recorded block the RSX is being pointed at, or 0xFFFFFFFF.
 *
 * `put` sits at or just past the end of the block the title wants executed --
 * the gap is whatever libgcm appended after the SPU's data, a fence and its
 * argument in this title's case. Require the block to END within a small window
 * before `put` and to START before it, so an older double-buffered block (the
 * SPU alternates between two) is not chosen over the current one. */
static u32 gcm_spu_block_for_put(u32 put)
{
    u32 best = 0xFFFFFFFFu, best_end = 0;
    for (int i = 0; i < GCM_SPU_BLK_N; i++) {
        u32 s = s_spu_blk[i].io_start, e = s_spu_blk[i].io_end;
        if (!e || s >= put || e > put) continue;
        if (put - e > 0x40u) continue;              /* too far back to be current */
        if (e >= best_end) { best_end = e; best = s; }
    }
    return best;
}

/* A resync jumps `get` straight to `put`, which is what keeps the FIFO alive
 * when the walker cannot make progress -- but everything in between is
 * dropped, and if a SET_REFERENCE is in there the cellGcmFinish waiting on it
 * never wakes. Tokyo Jungle lost exactly one that way: two fences in the ring
 * (arg 0 at io 0x09A0, arg 1 at io 0x19E8), the walker parked before the
 * second, and the resync to put=0x19F0 stepped straight over it -- ref stayed
 * 0 while the title spun for ref==1 forever.
 *
 * Dropping DRAWS on a resync is survivable: a frame is lost. Dropping a FENCE
 * is not, it deadlocks the title. Sweep the skipped range first. */
static void gcm_fifo_resync_why(const char* why, u32* getoff, u32 put)
{
    static int n = 0;
    if (PS3_LOG_FIRST(n, 8))
        fprintf(stderr, "[cellGcmSys] FIFO resync (%s) 0x%08X -> put 0x%08X\n",
                why, *getoff, put);
    /* If an SPU DMA'd a pushbuffer that ends just short of `put`, that is the
     * stream the RSX is being pointed at -- land at its START and walk it,
     * instead of landing on `put` and skipping it. Resyncing to `put` keeps the
     * FIFO alive but drops every command in between, and SET_BEGIN_END is one
     * of them: the draws still arrive, with no primitive type, and are dropped
     * by the backend as prim=0. */
    u32 land = gcm_spu_block_for_put(put);
    if (land == 0xFFFFFFFFu) land = put;
    else { static int b = 0; if (PS3_LOG_FIRST(b, 8))
        fprintf(stderr, "[cellGcmSys] resync landing on SPU pushbuffer "
                "0x%08X (put 0x%08X) instead of skipping to put\n", land, put); }

    /* Sweep only the range still being skipped; the walker will consume the
     * fences inside the block itself, and pushing them here too would publish
     * every one twice. */
    u32 from = (*getoff <= land) ? *getoff : s_last_ring_off;
    if (from < land) {
        unsigned rescued = 0;
        for (u32 io = from; io + 8 <= land; io += 4) {
            u32 ea = gcm_io2ea(io); if (!ea) continue;
            u32 w = vm_read32(ea);
            if ((w >> 29) != 0 || (w & 0x1FFCu) != 0x0050u) continue;
            u32 dea = gcm_io2ea(io + 4); if (!dea) continue;
            gcm_ref_push_at(vm_read32(dea), io);
            rescued++;
        }
        if (rescued) { static int m = 0; if (PS3_LOG_FIRST(m, 8))
            fprintf(stderr, "[cellGcmSys] resync rescued %u fence(s) from"
                    " 0x%08X..0x%08X\n", rescued, from, land); }
    }
    *getoff = land;
}

static void gcm_fifo_dump_around(u32 getoff)
{
    /* Only ever called on a recovery (bad branch, stuck get), and capped, so
     * it no longer hides behind GCM_RECDBG: the words the walker stopped on
     * are the one thing a run that wedged once in three can still tell us,
     * and GCM_RECDBG is far too loud to leave on for a 15-minute run. */
    static int d = 0;
    if (d++ >= 3) return;
    for (u32 t = 0; t < GCM_TRACE_N; t++) {
        u32 k = (s_tr_i + t) % GCM_TRACE_N;
        if (!s_tr_w[k]) continue;
        u32 w = s_tr_w[k];
        fprintf(stderr, "[FIFOSTEP] io=%08X w=%08X  type=%u method=0x%04X count=%u\n",
                s_tr_off[k], w, w >> 29, w & 0x1FFCu, (w >> 18) & 0x7FFu);
    }
    for (int k = -20; k <= 2; k++) {
        u32 io = getoff + (u32)(k * 4);
        u32 ea = gcm_io2ea(io);
        if (!ea) continue;
        fprintf(stderr, "[FIFODUMP] %+3d io=%08X w=%08X%s\n",
                k, io, vm_read32(ea), k ? "" : "   <-- bad word");
    }
}


static void gcm_ref_push_at(u32 v, u32 getoff)
{
    { static int _d = -1; if (_d < 0) _d = getenv("GCM_REFLOG") ? 1 : 0;
      if (_d) fprintf(stderr, "[refq] drained fence 0x%X at getoff=%08X\n", v, getoff); }
    u32 t = s_ref_qtail;
    if (t - s_ref_qhead >= GCM_REF_QLEN) {   /* overflow: drop oldest (keeps liveness) */
        s_ref_qhead++;
        rsx_lim_event(RSX_LIM_GCM_REFQ, 0, "fence queue full: the oldest fence is dropped");
        static int _o = 0;
        if (PS3_LOG_FIRST(_o, 4)) fprintf(stderr, "[cellGcmSys] fence queue overflow -- oldest dropped\n");
    }
    s_ref_q[t % GCM_REF_QLEN] = v;
    __atomic_thread_fence(__ATOMIC_RELEASE);   /* (rsx-par) the value before the tail a poller reads (a compiler fence on x86) */
    s_ref_qtail = t + 1;
    { const u32 d = s_ref_qtail - s_ref_qhead;   /* (rsx5) PS3RECOMP_LIMITS */
      if (d > g_rsx_lim[RSX_LIM_GCM_REFQ].peak) { rsx_lim_cap(RSX_LIM_GCM_REFQ, GCM_REF_QLEN); rsx_lim_use(RSX_LIM_GCM_REFQ, d); } }
}

/* Global fence-publication counter: lets the lwmutex convoy trace correlate a
 * long lock hold with the number of paced fence publications inside it (the
 * 200us pacing x hundreds of one-ahead fences = the ~156ms holds). */
volatile long long g_gcm_ref_pub_count = 0;
/* Absolute microseconds from the QPC hardware counter -- the SAME origin in every
 * translation unit, so two probes in different files can be compared directly.
 * That is the whole point: every wrong conclusion about this stall came from
 * comparing orderings instead of times. */
unsigned long long ps3_qpc_us(void)
{
    static LARGE_INTEGER f; if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER n; QueryPerformanceCounter(&n);
    return (unsigned long long)(n.QuadPart / (f.QuadPart / 1000000));
}

static void gcm_ref_publish_one(void)
{
    u32 h = s_ref_qhead;
    if (h == s_ref_qtail) return;
    vm_write32(GCM_CONTROL_GUEST_ADDR + 8, s_ref_q[h % GCM_REF_QLEN]);
    /* stderr, deliberately: [DRAIN] uses printf and stdout is block-buffered
     * when redirected, so its interleaving with an stderr probe is not a
     * timeline. Same stream = same ordering. */
    { static int _rd = -1; if (_rd < 0) _rd = getenv("GCM_REFPUB") ? 1 : 0;
      if (_rd) { static unsigned long _n = 0; if (++_n <= 24)
        fprintf(stderr, "[refpub] t=%lluus #%lu wrote 0x%08X -> readback 0x%08X\n", ps3_qpc_us(), _n,
                s_ref_q[h % GCM_REF_QLEN], vm_read32(GCM_CONTROL_GUEST_ADDR + 8)); } }
    s_ref_qhead = h + 1;
    g_gcm_ref_pub_count++;
}

/* Read-driven fence publication. cellGcmFinish / FIFO-space waits spin-read the
 * ref register (GCM_CONTROL+8). The 60 Hz present-thread ticker publishes only
 * one queued fence per frame, so when present overruns during heavy boot load
 * the tick rate -- and thus fence publication -- collapses and an equality-
 * waiter stalls seconds per fence (the [finspin] boot crawl; the game is not
 * deadlocked, just starved). Let the spinning reader drive publication too:
 * this is called from vm_read32 on each ref read, advancing one queued fence,
 * globally paced to at most one per millisecond so every value stays visible
 * >= 1 ms to any equality-waiter (no skip-past). Decoupled from present, so a
 * wait makes progress even when the ticker is starved. Idempotent + additive:
 * it only ever publishes fences the ticker would eventually publish anyway. */
/* Serializes the FIFO walker between the 60 Hz present thread and the
 * poll-path below. TryAcquire on the poll path avoids re-entrancy (the walker
 * itself performs guest reads) and never blocks a spinning game thread. */
static SRWLOCK s_gcm_fifo_lock = SRWLOCK_INIT;
static void gcm_rsx_process_fifo_unlocked(void);

/* Kick event: a game thread whose fence wait finds the queue DRY signals the
 * present thread to run the FIFO walker NOW instead of on its next 16 ms
 * tick. A movie frame issues ~46 one-ahead cellGcmFinish waits; paying a
 * 16 ms tick per fence was ~0.75 s/frame (the 2 FPS intro). The walker (and
 * the D3D12 backend it feeds) stays on the present thread -- running it from
 * the polling game thread killed the process silently. */
static HANDLE s_gcm_kick_ev = NULL;
void* cellGcm_fifo_kick_event(void)
{
    if (!s_gcm_kick_ev) s_gcm_kick_ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    return (void*)s_gcm_kick_ev;
}

/* The present thread's idle wait. It used to be a flat Sleep(4), which meant
 * the kick above was created, signalled -- and waited on by nobody, so a
 * starved guest still paid the full tick. Waiting here instead costs nothing
 * when no one kicks (the timeout is the old sleep) and drains immediately when
 * someone does. */
/* (critic) Is there work for a drain pass right now? New words behind a moved
 * `put`, the rest of a frame the last pass left at its flip, a flip to present,
 * or the label a blocked acquire waits for now holding its value. */
static int gcm_kw_ready(void)
{
    if (s_kw_more || s_flip_pending) return 1;
    if (vm_read32(GCM_CONTROL_GUEST_ADDR + 0) != s_kw_put) return 1;
    if (s_sema_live && vm_read32(GCM_LABEL_GUEST_BASE + (s_sema_offset & 0xFFFFu)) == s_sema_wait_val) return 1;
    /* parked on a self-JUMP: inFAMOUS 2 moves put first and patches the park
     * second, so a patch does not move put */
    if (s_park_off && s_park_off == s_fifo_getoff) {
        const u32 ea = gcm_io2ea(s_fifo_getoff);
        if (ea && vm_read32(ea) != s_kw_park_word) return 1;
    }
    return 0;
}

void cellGcm_fifo_kick_wait(unsigned ms)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("GCM_FIFO_KICK"); on = (e && *e == '0') ? 0 : 1; }
    HANDLE ev = on ? (HANDLE)cellGcm_fifo_kick_event() : NULL;
    /* (critic) PS3RECOMP_GCM_SPIN_US=<us> (default 0 = off, as before): spin up
     * to <us> before the event wait, ending as soon as gcm_kw_ready(). The FIFO
     * has no doorbell, and the wait below is a 1 ms WaitForSingleObject that
     * the scheduler rounds to its timer ticks: in the city A/B of 2026-11-10
     * (cv2-*, split off) the RSX thread sat 0.5 / 1.0 / 1.5 / 2.0 ms -- tick
     * multiples -- between the drain that caught up with the title and the
     * present of that frame, ~1.0-1.4 ms of every ~9.2 ms dock frame. Timing
     * only: the passes this lets run earlier are the ones the timeout or a kick
     * would have run. */
    static int spin_us = -1; static LONGLONG spin_ticks;
    if (spin_us < 0) {
        const char* e = getenv("PS3RECOMP_GCM_SPIN_US");
        int v = e ? atoi(e) : 300;   /* (m16) default 300 us: +12% at the dock in the nt-* A/B of 2026-11-10; 0 = off */
        if (v < 0) v = 0;
        if (v > 4000) v = 4000;
        LARGE_INTEGER f; QueryPerformanceFrequency(&f);
        spin_ticks = f.QuadPart * (LONGLONG)v / 1000000;
        spin_us = v;
        if (v) fprintf(stderr, "[cellGcmSys] PS3RECOMP_GCM_SPIN_US=%d: the frame clock spins up to %d us for new FIFO work before it sleeps\n", v, v);
    }
    if (spin_us && s_gcm_initialized) {
        if (gcm_kw_ready()) return;
        LARGE_INTEGER t0, now; QueryPerformanceCounter(&t0);
        unsigned n = 0;
        do {
            for (int i = 0; i < 16; i++) YieldProcessor();
            if (gcm_kw_ready()) return;
            if (ev && (++n & 15u) == 0u && WaitForSingleObject(ev, 0) == WAIT_OBJECT_0) return;
            QueryPerformanceCounter(&now);
        } while (now.QuadPart - t0.QuadPart < spin_ticks);
    }
    if (ev) WaitForSingleObject(ev, ms);
    else    Sleep(ms);
}

/* (critic) PS3RECOMP_GCM_SEMA_KICK (default 1; 0 = as before): the walker
 * blocked on a semaphore acquire waits for a label that an SPU DMA (or, with
 * PS3RECOMP_RSX_SPLIT, the exec thread's release) writes, and neither kicked
 * the frame clock, so every acquire stall lasted at least one wait timeout
 * (median 1.83 ms with the split on, 0.9-1.3 ms off, in the cv-* / cv2-* city
 * runs). Signal the kick event when that exact label is written. Timing only:
 * the next pass re-reads the label as before. Never creates the event (only
 * the frame clock does), so it is safe from any thread. */
static int gcm_sema_kick_on(void)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("PS3RECOMP_GCM_SEMA_KICK"); on = !(e && *e == '0'); }
    return on;
}
static void gcm_sema_kick_if(u32 ea, u32 size)
{
    if (!__atomic_load_n(&s_sema_live, __ATOMIC_RELAXED) || !s_gcm_kick_ev || !gcm_sema_kick_on()) return;
    const u32 la = GCM_LABEL_GUEST_BASE + (__atomic_load_n(&s_sema_offset, __ATOMIC_RELAXED) & 0xFFFFu);
    if (la - ea < size) SetEvent(s_gcm_kick_ev);
}


/* Serializes fence publication between the present ticker and every guest
 * thread poll-reading the ref register (REFPOLL default-on made this path
 * multi-threaded; the SPSC queue head raced and publication order broke --
 * boot froze with all threads parked on fences that were never published in
 * order). TryAcquire: a contended poll just reads the current ref. */
static SRWLOCK s_ref_pub_lock = SRWLOCK_INIT;

void cellGcm_ref_on_poll(void)
{
    /* Pace: leave each published value observable for >= ~200us of spins
     * (equality-waiters at any poll rate see every value; still ~5000/s so
     * loading's fence storms clear in seconds, not minutes). */
    static LARGE_INTEGER s_freq;
    static volatile LONGLONG s_last_pub;
    /* GCM_REF_SOLO=1 (default off -- GCM_REF_STATS in the city: > 99.9% of the
     * reads find nothing queued, the RSX thread has not reached the fence, so
     * the pacing is not what the main thread waits for): while ONE thread is the
     * only one reading the ref -- no other thread read it for a whole pacing
     * gap -- each of its reads may advance one queued fence. It reads every
     * value it publishes (the read follows this call), and any other waiter
     * would have read within the gap, which is what the pacing already
     * assumes. inFAMOUS 2's hot waiter (func_0013ACE8: ref - target >= 0,
     * usleep(5)) otherwise waited 200 us per queued fence: 13-15% of the PPU
     * main thread in the city. Recorded before the TryAcquire, so a reader
     * that loses the lock still counts as a second reader. */
    static int s_solo_on = -1;
    static volatile LONG s_rp_tid;
    static volatile LONGLONG s_rp_multi;
    if (s_solo_on < 0) { const char* e = getenv("GCM_REF_SOLO"); s_solo_on = e && e[0] == '1'; }
    if (!s_freq.QuadPart) QueryPerformanceFrequency(&s_freq);
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    const LONG tid = (LONG)GetCurrentThreadId();
    if (s_solo_on && s_rp_tid != tid) { s_rp_multi = now.QuadPart; InterlockedExchange(&s_rp_tid, tid); }
    if (!TryAcquireSRWLockExclusive(&s_ref_pub_lock))
        return;
    LONGLONG min_gap = s_freq.QuadPart / 5000;          /* ~200us */
    const int solo = s_solo_on && s_rp_tid == tid && now.QuadPart - s_rp_multi >= min_gap;
    int dry = 0;
    /* GCM_REF_STATS=1: every ~5 s, how the ref reads went -- nothing queued
     * (the RSX thread has not reached the fence: RSX lag), held back by the
     * pacing gap, or advanced (paced / by a sole reader). */
    static int s_rs_on = -1;
    static u64 s_rs_n, s_rs_dry, s_rs_held, s_rs_pub, s_rs_solo; static LONGLONG s_rs_t;
    if (s_rs_on < 0) s_rs_on = getenv("GCM_REF_STATS") ? 1 : 0;
    const int queued = s_ref_qhead != s_ref_qtail;
    if (solo || now.QuadPart - s_last_pub >= min_gap) {
        if (s_rs_on && queued) { s_rs_pub++; if (now.QuadPart - s_last_pub < min_gap) s_rs_solo++; }
        gcm_ref_publish_one();
        s_last_pub = now.QuadPart;
        dry = (s_ref_qhead == s_ref_qtail);
    } else if (s_rs_on && queued) s_rs_held++;
    if (s_rs_on) {
        s_rs_n++; if (!queued) s_rs_dry++;
        if (!s_rs_t) s_rs_t = now.QuadPart;
        if (now.QuadPart - s_rs_t >= 5 * s_freq.QuadPart) {
            const double sec = (double)(now.QuadPart - s_rs_t) / (double)s_freq.QuadPart;
            fprintf(stderr, "[refstats] per s: %.0f ref reads, %.0f with nothing queued, %.0f held by pacing, "
                            "%.0f advanced (%.0f by a sole reader); queue %u\n",
                    s_rs_n / sec, s_rs_dry / sec, s_rs_held / sec, s_rs_pub / sec, s_rs_solo / sec,
                    (unsigned)(s_ref_qtail - s_ref_qhead));
            s_rs_n = s_rs_dry = s_rs_held = s_rs_pub = s_rs_solo = 0; s_rs_t = now.QuadPart;
        }
    }
    ReleaseSRWLockExclusive(&s_ref_pub_lock);
    if (dry && s_gcm_kick_ev)
        SetEvent(s_gcm_kick_ev);       /* dry: ask the present thread to walk */
    /* A guest spinning on a ref that has stopped moving for 2 s, with nothing
     * queued: say once where the FIFO walker stands. */
    if (dry) {
        static u32 last_ref; static LONGLONG since; static int told, reports;
        const u32 ref = vm_read32(GCM_CONTROL_GUEST_ADDR + 8);
        if (ref != last_ref || !since) { last_ref = ref; since = now.QuadPart; told = 0; }
        else if (!told && reports < 8 && now.QuadPart - since > 2 * s_freq.QuadPart) {
            told = 1; reports++;   /* again once ref has moved: loading stalls come first */
            const u32 put = vm_read32(GCM_CONTROL_GUEST_ADDR + 0);
            const u32 gea = gcm_io2ea(s_fifo_getoff);
            fprintf(stderr, "[cellGcmSys] ref stuck at 0x%X for 2 s: get=0x%08X put=0x%08X word@get=0x%08X "
                            "park_off=0x%08X sema_off=0x%X labels 134/135/136/137 = %X/%X/%X/%X\n",
                    ref, s_fifo_getoff, put, gea ? vm_read32(gea) : 0u, s_park_off, s_sema_offset,
                    vm_read32(GCM_LABEL_GUEST_BASE + 0x860), vm_read32(GCM_LABEL_GUEST_BASE + 0x870),
                    vm_read32(GCM_LABEL_GUEST_BASE + 0x880), vm_read32(GCM_LABEL_GUEST_BASE + 0x890));
            /* Is the walker stopped (and why), or still inside one drain? */
            { const unsigned long long ms = ps3_ms_now(), in = s_drain_in_ms, end = s_drain_end_ms;
              static unsigned last_count;
              const unsigned cnt = s_drain_count;
              fprintf(stderr, "[cellGcmSys]   drain: %s %llu ms; last stop \"%s\" %llu ms ago; %u drains since the last report\n",
                      in ? "running for" : "idle,", in ? ms - in : 0ull, s_drain_why,
                      end ? ms - end : 0ull, cnt - last_count);
              last_count = cnt; }
            /* The flip side: a title that writes its next put from the flip
             * handler (inFAMOUS 2) waits here when a completion never reaches
             * it. Status, request/tick sequence, undelivered handler bits. */
#ifdef _WIN32
            { const unsigned pend = (unsigned)s_gcm_pending;
#else
            { const unsigned pend = (unsigned)atomic_load(&s_gcm_pending);
#endif
              fprintf(stderr, "[cellGcmSys]   flip: status=%u req_seq=%u tick_seq=%u requests=%u pending=0x%X\n",
                      s_flip_status, s_flip_req_seq, s_flip_tick_seq, s_flip_request_count, pend); }
            /* PS3_HANG_PEEK=<ea>:<len>[,...] (hex): guest words to print with
             * the report -- a title's own sync state, read at the stall. */
            { const char* pk = getenv("PS3_HANG_PEEK");
              if (pk && *pk) { char buf[512]; strncpy(buf, pk, sizeof buf - 1); buf[sizeof buf - 1] = 0;
                  for (char* tok = strtok(buf, ","); tok; tok = strtok(NULL, ",")) {
                      unsigned ea = 0, len = 4;
                      if (sscanf(tok, "%x:%x", &ea, &len) < 1) continue;
                      if (len > 0x100) len = 0x100;
                      fprintf(stderr, "[cellGcmSys]   peek 0x%08X:", ea);
                      for (unsigned o = 0; o < len; o += 4) fprintf(stderr, " %08X", vm_read32(ea + o));
                      fputc('\n', stderr);
                  } } }
            gcm_fifo_dump_around(s_fifo_getoff);
            /* PS3_HANG_DUMP=1 (the launcher's debug mode): where every guest
             * thread is at that moment, not up to a minute later. */
            { static int hd = -1;
              if (hd < 0) { const char* e = getenv("PS3_HANG_DUMP"); hd = e && *e == '1'; }
              if (hd) { extern void ps3_thread_dump_now(const char* why);
                        ps3_thread_dump_now("the GPU has had nothing to do for 2 s"); } }
#if !defined(_WIN32)
            /* PS3_HANG_SAMPLE=<prefix>: also a 2 s `sample` of the host
             * threads (RSX, SPU, PPU) to <prefix>.<ref>.txt. macOS only. */
            { const char* hs = getenv("PS3_HANG_SAMPLE");
              if (hs && *hs) { char cmd[1200];
                  snprintf(cmd, sizeof cmd, "/usr/bin/sample %d 2 -file '%s.%X.txt' >/dev/null 2>&1 &",
                           (int)getpid(), hs, ref);
                  if (system(cmd) != 0) { /* best effort */ } } }
#endif
        }
    }
}

/* Hitch attribution (PS3RECOMP_PLAYLOG, perf 2026-11-09): time the RSX thread
 * spent inside FIFO drains (walking, drawing, uploading, waiting on the GPU in
 * a flush) and time the walker stood on a semaphore acquire (the SPU light
 * label), since the last present; cellGcm_hattr_take hands them over at the
 * present, which runs inside a drain (the part of that drain so far is
 * counted, and the drain goes on counting from there). Written and taken under
 * s_gcm_fifo_lock or by the draining thread; numbers only. */
static unsigned long long s_ha_drain_ns, s_ha_drain_t0, s_ha_sema_ns, s_ha_sema_t0, s_ha_sema_gaveup;
void cellGcm_hattr_take(unsigned long long* drain_ns, unsigned long long* sema_ns, unsigned long long* gaveup)
{
    const unsigned long long now = ps3_ft_now();
    unsigned long long d = s_ha_drain_ns;
    if (s_ha_drain_t0) { d += now - s_ha_drain_t0; s_ha_drain_t0 = now; }
    unsigned long long s = s_ha_sema_ns;
    if (s_ha_sema_t0) { s += now - s_ha_sema_t0; s_ha_sema_t0 = now; }
    *drain_ns = d; *sema_ns = s; *gaveup = s_ha_sema_gaveup;
    s_ha_drain_ns = s_ha_sema_ns = s_ha_sema_gaveup = 0;
}

void cellGcm_rsx_process_fifo(void)
{
    /* The host's frame clock thread drains the FIFO and encodes the draws:
     * the RSX role, once per thread (a guest thread that drains is already
     * placed and keeps its own role). */
    ps3_host_place_self_once(PS3_PLACE_RSX);
    { static int reg; if (!reg) { reg = 1; rsx_diag_thread_here(RSX_DIAG_WALKER); } }   /* (rsx-par2) PS3_STALL_MS */
    const uint64_t ft_t0 = PS3_FT_ON() ? ps3_ft_now() : 0;
    s_drain_in_ms = ps3_ms_now();
#if !defined(_WIN32)
    /* GCM_FLIP_STATS: a drain of 250 ms or more says how much of it this
     * thread was on the CPU -- work (shader builds, uploads) or a host that
     * did not run it -- and how many command bytes it walked. */
    struct timespec cpu0 = { 0, 0 };
    const int long_dbg = gcm_flip_stats_on();
    const unsigned long long bytes0 = s_walk_bytes;
    if (long_dbg) clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu0);
#endif
    AcquireSRWLockExclusive(&s_gcm_fifo_lock);
    s_ha_drain_t0 = ps3_ft_now();
    gcm_rsx_process_fifo_unlocked();
    if (g_rsx_drain_end_hook) g_rsx_drain_end_hook();   /* (rsx-par) PS3RECOMP_VK_BT: the recording worker idle */
    s_ha_drain_ns += ps3_ft_now() - s_ha_drain_t0;   /* (the present inside may have taken a part) */
    s_ha_drain_t0 = 0;
    ReleaseSRWLockExclusive(&s_gcm_fifo_lock);
    s_drain_end_ms = ps3_ms_now();
#if !defined(_WIN32)
    if (long_dbg && s_drain_end_ms - s_drain_in_ms >= 250) {
        struct timespec cpu1; clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu1);
        static unsigned n;
        if (n++ < 100)
            fprintf(stderr, "[gcm-flipstats] drain of %llu ms (%lld ms on the CPU), %llu command bytes, stopped: %s\n",
                    s_drain_end_ms - s_drain_in_ms,
                    (long long)((cpu1.tv_sec - cpu0.tv_sec) * 1000 + (cpu1.tv_nsec - cpu0.tv_nsec) / 1000000),
                    s_walk_bytes - bytes0, s_drain_why);
    }
#endif
    s_drain_in_ms = 0; s_drain_count++;
    if (ft_t0) {
        const uint64_t t1 = ps3_ft_now();
        if (t1 - ft_t0 >= ps3_ft_min_ns())
            ps3_ft_span(PS3_FT_DRAIN, 0, (uint32_t)s_sema_live, 0, ft_t0, t1);
    }
}

/* Did the last drain stop on a blocking semaphore acquire (GCM_SEMA_ACQUIRE)?
 * What unblocks it is an SPU's DMA write of the label, which kicks nothing, so
 * a present thread that sleeps a fixed 4 ms between drains adds up to 4 ms to
 * every such wait; it can poll faster while this holds. */
int cellGcm_drain_sema_blocked(void) { return s_sema_live; }

/* The walker's NV406E/NV4097 state machine (rsx_commands.c: SET_REFERENCE,
 * the back-end semaphore labels). File scope since (rsx-par) the split exec
 * thread runs those methods too (gcm_split_run). */
static rsx_state s_state;
static int  s_inited = 0;

/* (rsx-par) PS3RECOMP_RSX_SPLIT: what the per-word walk does with a word
 * besides handing it to the draw engine, run on the exec thread in FIFO order
 * (the engine's own dispatch of the word ran on the walker, ahead of it in
 * the queue -- the order the per-word walk has). Lean state only: the split
 * runs only when the walker's fast conditions hold (gcm_lean_state() among
 * them, no FIFO diagnostic, no live renderer). */
static void gcm_word_effects(u32 subch, u32 m, u32 dw, u32 getoff)
{
    if (subch == 7 && m == 0x0B00u) {
        extern void rsx_raise_user_cmd(u32 arg);
        rsx_raise_user_cmd(dw);
        return;
    }
    extern u32 g_rsx_last_reference;
    const u32 mfull = (subch << 13) | m;
    if (mfull == 0xEB00u || mfull == 0xEB04u) {
        cellGcmQueueUserCommand(dw);
    } else if (mfull == 0xE920u || mfull == 0xE924u) {
        cellGcmSetFlipCommand(dw & 7u);
    } else if (subch == 0 || (subch == 1 && !gcm_subch1_2d())) {
        if (!gcm_lean_state() || m == 0x0050u || (m >= 0x1D6Cu && m <= 0x1D74u))
            rsx_process_method(&s_state, m, dw);
        if (m == 0x50) gcm_ref_push_at(g_rsx_last_reference, getoff);
        if (m == 0x1800) {
            const u32 v = dw, idx = (v & 0xFFFFFFu) / 16u;
            { static int rn = 0; if (PS3_LOG_FIRST(rn, 6)) printf("[GET_REPORT] type=%u idx=%u%c", v >> 24, idx, 10); }
            if (idx < CELL_GCM_MAX_REPORT_COUNT) {
                s_report_data[idx].timestamp = get_timestamp_ns();
                s_report_data[idx].value = (v >> 24) == 1u ? 0xFFFFu : 0u;
            }
        }
    } else
        gcm_2d_method(subch, m, dw);
}
/* (rsx-par) does the per-word walk do anything with this word besides the
 * engine's dispatch (gcm_word_effects, under lean state)? */
static int gcm_word_needs_act(u32 subch, u32 m, int s1_2d)
{
    const u32 mfull = (subch << 13) | m;
    if (mfull == 0xEB00u || mfull == 0xEB04u || mfull == 0xE920u || mfull == 0xE924u) return 1;
    if (subch == 0 || (subch == 1 && !s1_2d))
        return m == 0x0050u || (m >= 0x1D6Cu && m <= 0x1D74u) || m == 0x1800u;
    return 1;                                   /* the 2D / memory-copy subchannels */
}
/* (rsx-par) a FIFO flip word's bookkeeping (the walker keeps the stop and the
 * diagnostics) */
static void gcm_flip_word_effects(u32 w)
{
    extern s32 cellGcmSetFlipCommand(u32 bufferId);
    if ((w & 0xFFFFFF00u) == GCM_FLIP_MARKER) {
        s_current_display_buffer_id = w & 7u;
        s_flip_pending = 1;
        s_flip_request_count++;
    } else {
        cellGcmSetFlipCommand(w & 0xFFu);
    }
}
void gcm_split_run(u32 kind, u32 a, u32 b, u32 c)
{
    switch (kind) {
    case RSX_SPLIT_ACT_WORD: gcm_word_effects(a >> 13, a & 0x1FFCu, b, c); break;
    case RSX_SPLIT_ACT_SEMA: vm_write32(a, b); gcm_sema_kick_if(a, 4u); break;   /* (critic) */
    case RSX_SPLIT_ACT_FLIP: gcm_flip_word_effects(a); break;
    default: break;
    }
}

static void gcm_rsx_process_fifo_unlocked(void)
{
    s_sema_live = 0;
    s_kw_more = 0;   /* (critic) set again at the end of a full pass */
    { static unsigned _n = 0; static unsigned long long _t0 = 0;
      extern unsigned long long ps3_ms_now(void);
      _n++;
      /* each switch read once: a getenv per drain showed in the RSX thread's profile */
      static int rate_on = -1; if (rate_on < 0) rate_on = getenv("GCM_RATE") != NULL;
      if (rate_on) {
          unsigned long long now = ps3_ms_now();
          if (_t0 == 0) _t0 = now;
          if (now - _t0 >= 2000) {
              fprintf(stderr, "[RATE] drains/sec=%.0f\n", _n * 1000.0 / (now - _t0));
              _n = 0; _t0 = now;
          }
      } }
    extern u32 g_rsx_last_reference;

    /* A statically-linked libgcm (PS3 firmware modules -- ps1_netemu) never calls
     * cellGcmSetupContext, so there is no CellGcmContextData EA to gate on: it gets
     * its ring through sys_rsx_context_allocate instead. The walk below only needs
     * `put` and the IO table, both of which the syscall layer fills; the one part
     * that does read the context (the ring-recycle heuristic) is separately guarded
     * by s_gcm_ctx_out_ea, which stays 0 on that path. */
    if (!s_gcm_context_ea && !s_gcm_syscall_mode) return;
    if (!s_inited) { rsx_state_init(&s_state); s_inited = 1; }

    /* Walk the FIFO exactly like the RSX: chase the guest-written `put` (an IO
     * offset in the control register), decoding method headers and following
     * JUMP/CALL/RET control words. The old linear drain copied context->current
     * onward and treated a JUMP as end-of-buffer -- fine for titles that write
     * one flat ring (cellmark), but PSL1GHT/Tiny3D immediately JUMPs into its
     * own command ring, so everything after the jump (including the reference
     * writes its waits spin on) silently never executed. */
    u32 put = vm_read32(GCM_CONTROL_GUEST_ADDR + 0);
    s_kw_put = put; s_kw_more = 0;   /* (critic) PS3RECOMP_GCM_SPIN_US */
    /* The words behind `put` were stored by another thread (the title, or the
     * recycle below) before it stored `put`. On x86 the hardware keeps loads
     * in order; on arm64 it does not, so without an acquire here the ring
     * word can be fetched before `put` was and come back stale -- the walker
     * then runs off the end of the previous frame's bytes until it meets a
     * word that is not a command, and parks there forever. */
    atomic_thread_fence(memory_order_acquire);

    /* GCM_CTXDBG=1: which context the title is actually driving. Ours is written
     * once at init; if the title repoints gCellGcmCurrentContext at a buffer of
     * its own, every field below moves with it and the FIFO the runtime walks
     * stops being the one the title fills. Reported on change, not per tick. */
    static int ctxdbg = -1; if (ctxdbg < 0) ctxdbg = getenv("GCM_CTXDBG") != NULL;
    if (ctxdbg && s_gcm_ctx_out_ea) {
        static u32 last_ptr = 0xFFFFFFFFu, last_cur = 0xFFFFFFFFu;
        u32 ptr = vm_read32(s_gcm_ctx_out_ea);
        u32 cur = ptr ? vm_read32(ptr + 0x8) : 0;
        if (ptr != last_ptr || cur != last_cur) {
            last_ptr = ptr; last_cur = cur;
            fprintf(stderr, "[CTX] gCellGcmCurrentContext@0x%08X -> 0x%08X  "
                    "begin=%08X end=%08X current=%08X callback=%08X  (ours=0x%08X)\n",
                    s_gcm_ctx_out_ea, ptr,
                    ptr ? vm_read32(ptr + 0x0) : 0, ptr ? vm_read32(ptr + 0x4) : 0,
                    cur, ptr ? vm_read32(ptr + 0xC) : 0, s_gcm_context_ea);
            fflush(stderr);
        }
    }

    u32 start_off = s_fifo_getoff;
    static int draindbg = -1; if (draindbg < 0) draindbg = getenv("GCM_DRAINDBG") != NULL;
    if (draindbg) {
        static int n = 0;
        if (n++ % 60 == 0)
            printf("[DRAIN] getoff=%08X put=%08X ref=%08X ctx.current=%08X\n",
                   s_fifo_getoff, put, vm_read32(GCM_CONTROL_GUEST_ADDR + 8),
                   vm_read32(s_gcm_context_ea + 0x8));
    }

    /* GCM_SCANREF=1 (one-shot): list every NV406E_SET_REFERENCE actually
     * present in 0..put. "The title waits for a fence we never published" has
     * two very different causes -- we missed the command, or it was never
     * submitted -- and only a direct scan of the ring separates them. */
    { static int scanned = 0;
      static int scan_on = -1; if (scan_on < 0) scan_on = getenv("GCM_SCANREF") != NULL;
      if (!scanned && scan_on && put >= 0x19F0) { scanned = 1;
        unsigned found = 0;
        for (u32 io = 0; io + 8 <= put; io += 4) {
            u32 ea = gcm_io2ea(io); if (!ea) continue;
            u32 w = vm_read32(ea);
            if ((w & 0x1FFCu) == 0x0050u && (w >> 29) == 0) {
                u32 dea = gcm_io2ea(io + 4);
                fprintf(stderr, "[SCANREF] io=%08X w=%08X count=%u arg=%08X\n",
                        io, w, (w >> 18) & 0x7FFu, dea ? vm_read32(dea) : 0xDEADu);
                found++;
            }
        }
        fprintf(stderr, "[SCANREF] %u SET_REFERENCE in 0..%08X\n", found, put);
      } }

    /* GCM_FIFO_SNAP=N: dump N snapshots of the raw words at both ends of the
     * ring -- what `get` is about to decode, and what the title just wrote at
     * `put`. When get and put drift apart, the two windows say immediately
     * whether get is grinding through NOPs/garbage or the title is writing
     * somewhere get can never reach. The [FIFOSTEP] trace only ever fired on a
     * bad branch, which is exactly the case where the drift is silent. */
    { static int snap = -1;
      if (snap < 0) { const char* e = getenv("GCM_FIFO_SNAP"); snap = e ? atoi(e) : 0; }
      if (snap > 0 && put) {
          snap--;
          fprintf(stderr, "[SNAP] get=%08X put=%08X\n", s_fifo_getoff, put);
          for (int k = 0; k < 16; k++) {
              u32 gio = s_fifo_getoff + (u32)(k * 4), pio = put + (u32)(k * 4);
              u32 gea = gcm_io2ea(gio), pea = gcm_io2ea(pio);
              fprintf(stderr, "[SNAP]  get+%-3d io=%08X w=%08X   put+%-3d io=%08X w=%08X\n",
                      k * 4, gio, gea ? vm_read32(gea) : 0xDEADDEADu,
                      k * 4, pio, pea ? vm_read32(pea) : 0xDEADDEADu);
          }
          fflush(stderr);
      } }

    /* GCM_FIFO_FAST (rsx3, default 1; 0 = every header the general way below):
     * a run of plain method headers -- what nearly all of a frame is -- in one
     * tight loop over the current 1 MB IO page: one translation per page, the
     * word read inline, and only the tests that can fire for such a header (the
     * burst test's switches are read once per drain). Anything else (a jump,
     * call, return, flip word, semaphore, a method the walker acts on, a header
     * crossing the page, a park being left) ends the run and goes through the
     * general walk exactly as before. Same calls, same order, same counters. */
    static int fast_sw = -1, fast_tc = -1, fast_live = -1, fast_eng = -1, fast_burst = -1;
    if (fast_sw < 0) {
        const char* e = getenv("GCM_FIFO_FAST"); fast_sw = !(e && *e == '0');
        const char* t = getenv("TCONST_FIFO"); fast_tc = t ? atoi(t) : 0;
        const char* b = getenv("GCM_FIFO_BURST"); fast_burst = !(b && *b == '0');
        fprintf(stderr, "[cellGcmSys] FIFO fast loop: %s (GCM_FIFO_FAST)\n", fast_sw ? "on" : "off");
    }
    if (fast_live < 0) fast_live = rsx_live_draw_enabled();
    if (fast_eng < 0) fast_eng = rsx_draw_engine_enabled();
    const int fast = fast_sw && !fast_tc && fast_burst && !gcm_fifo_dbg() && vm_inline_ok &&
                     !fast_live && fast_eng && gcm_lean_state();
    const int fast_s1 = !gcm_subch1_2d();
    /* (rsx-par) PS3RECOMP_RSX_SPLIT (default 1): the draw engine on its own
     * thread (rsx_split.c), this one walks and dispatches. Only when the fast
     * loop's conditions hold (lean state, no FIFO diagnostic, no live
     * renderer); otherwise the old path, with the state handed back first. */
    int split = 0;
    if (fast) split = rsx_split_drain_begin();
    else rsx_split_release();
    int budget = 0x100000;                    /* words per tick cap */
    /* GCM_DRAINDBG also reports WHY each pass stopped. A drain that ends far
     * short of `put` every tick is the signature of a FIFO that can never
     * catch up, and the reason is the whole diagnosis. */
    const char* why = "caught-up";
walk_more:
    while (s_fifo_getoff != put && budget-- > 0) {
        if (fast && !s_park_off) {
            u32 g = s_fifo_getoff;
            const u32 pg = g >> 20;
            const u32 eag = gcm_io2ea(g);
            int done = 0;
            if (eag) {
                extern u8* vm_base;
                const u8* hb = vm_base + (eag - (g & 0xFFFFFu));   /* the IO page's first byte */
                for (;;) {
                    const u32 w = __builtin_bswap32(__atomic_load_n((const u32*)(hb + (g & 0xFFFFFu)), __ATOMIC_RELAXED));
                    const u32 type = w >> 29;
                    if ((type != 0 && type != 2) || (w & 3u) || w == 0x00020000u ||
                        (w & 0xFFFF0000u) == 0xFEAD0000u) break;
                    const u32 count = (w >> 18) & 0x7FFu, method = w & 0x1FFCu, subch = (w >> 13) & 7u;
                    if (count) {   /* (a count-0 header is a plain skip in the general walk too) */
                        if (!(subch == 0 || (subch == 1 && fast_s1))) break;
                        if (subch == 0 && (method == 0x64u || method == 0x68u || method == 0x6Cu)) break;
                        const u32 hi = type == 0 ? method + (count - 1u) * 4u : method;
                        if (hi > 0x1FFCu || (method <= 0x0050u && hi >= 0x0050u) ||
                            (method <= 0x1800u && hi >= 0x1800u) || (method <= 0x1D74u && hi >= 0x1D6Cu)) break;
                        if (((g + 4u) >> 20) != pg || ((g + count * 4u) >> 20) != pg) break;
                    }
                    if (g < put) s_last_ring_off = g + 4;
                    s_tr_off[s_tr_i % GCM_TRACE_N] = g;
                    s_tr_w[s_tr_i % GCM_TRACE_N] = w;
                    s_tr_i++;
                    if (count) {
                        const u32* dws = (const u32*)(hb + ((g + 4u) & 0xFFFFFu));
                        if (split) rsx_split_burst((subch << 13) | method, type == 0, dws, count);
                        else       rsx_draw_engine_burst((subch << 13) | method, type == 0, dws, count);
                    }
                    s_frame_bytes += 4 + count * 4; s_walk_bytes += 4 + count * 4;
                    g += 4 + count * 4;
                    s_fifo_getoff = g;
                    done = 1;
                    if (g == put || (g >> 20) != pg || budget-- <= 0) break;
                }
            }
            if (done) continue;
        }
        u32 ea = gcm_io2ea(s_fifo_getoff);
        if (!ea) {
            /* get is sitting on an IO offset with no mapping -- the title
             * branched into a region and then unmapped it, or the offset was
             * never valid. Breaking silently (what this did) leaves get stuck
             * there FOREVER: it can never reach put, the FIFO stops draining,
             * no fences publish, and anything waiting on ctrl->ref hangs with
             * nothing logged. Resynchronise to put instead. Commands between
             * here and put are lost, which is bad, but a permanently dead FIFO
             * is worse -- and now it says so. */
            static u32 last_bad; static int n;
            if (s_fifo_getoff != last_bad || n < 4) {
                last_bad = s_fifo_getoff;
                if (PS3_LOG_FIRST(n, 16))
                    fprintf(stderr, "[cellGcmSys] FIFO get=0x%08X has no IO "
                            "mapping -- resyncing to put=0x%08X\n",
                            s_fifo_getoff, put);
            }
            s_fifo_getoff = put;
            why = "no-io-mapping";
            break;
        }
        u32 w = vm_read32(ea);
        /* Only remember offsets BELOW put -- those are positions in the
         * ring the title is writing. A park in another region is where the
         * walker gave up, not how far it got through the ring. */
        if (s_fifo_getoff < put) s_last_ring_off = s_fifo_getoff + 4;
        s_tr_off[s_tr_i % GCM_TRACE_N] = s_fifo_getoff;
        s_tr_w[s_tr_i % GCM_TRACE_N] = w;
        s_tr_i++;
        u32 type = w >> 29;
        /* Both JUMP forms (gcm_fifo_decode.h). GCM_FIFO_JUMP_NEW=0 decodes
         * the old form only, as before; a new-form word is counted either way
         * (GCM_FLIP_STATS). */
        u32 jump_tgt = 0;
        const int is_jump = gcm_fifo_jump_target(w, &jump_tgt, gcm_fifo_jump_new());
        if (__builtin_expect(gcm_fifo_is_new_jump(w), 0)) gcm_note_new_jump(s_fifo_getoff, w);
        /* The walker is reading something other than the park it waited on:
         * it has left it, whether by the patched word (a JUMP elsewhere, or
         * commands written over the park) or by any resync. Forget it here,
         * not only when a patched JUMP is followed: a park left another way
         * kept its start time, so the next stop at the same address (the
         * game reuses its blocks) reported the whole interval as one wait
         * ("patched after 120946 ms" in a run with no stall), and with the
         * same `put` it inherited a grace that had already run out, so the
         * jump-park resync could fire on a park that was seconds old. */
        if (s_park_off && (s_fifo_getoff != s_park_off || !is_jump ||
                           jump_tgt != s_park_off))
            gcm_park_left(s_fifo_getoff == s_park_off);

        if ((w & 0xFFFF0000u) == 0xFEAD0000u) {
            /* lv1 driver flip: statically-linked libgcm cellGcmSetFlip writes
             * this word into the FIFO instead of calling any import. Fire the
             * flip (status/count/handler) and STOP this drain so the ticker's
             * flip-gated present shows exactly the completed frame -- draining
             * on would mix the next frame's head into the batch (wave: every
             * frame split across 4 presents, layout flashing/zooming). */
            extern s32 cellGcmSetFlipCommand(u32 bufferId);
            s_fifo_getoff += 4;
            /* Count the flip WORDS the title actually writes into the FIFO,
             * separately from the presents that result. When a title stops
             * appearing on screen the first question is which side stopped:
             * this counter still climbing while presents do not is a runtime
             * problem, both stopping together is the guest's. */
            { static unsigned long long flips = 0; static int dbg = -1;
              if (dbg < 0) dbg = getenv("GCM_FLIPCOUNT") ? 1 : 0;
              ++flips;
              if (dbg && (flips <= 8ull || (flips % 200ull) == 0))
                  fprintf(stderr, "[flipword] %llu FIFO flip words decoded%c",
                          flips, 10); }
            if (PS3_FT_ON()) { const uint64_t t = ps3_ft_now();
                               ps3_ft_span(PS3_FT_FLIP_FIFO, 0, w & 7u, 0, t, t); }
            const int unshown = s_flip_pending;   /* the flip before is still not on screen */
            if (split) {
                rsx_split_act(RSX_SPLIT_ACT_FLIP, w, 0, 0);   /* (rsx-par) behind the frame's draws */
            } else if ((w & 0xFFFFFF00u) == GCM_FLIP_MARKER) {
                /* Queued by _cellGcmSetFlipCommand, which already did the
                 * guest-visible half of the request. */
                s_current_display_buffer_id = w & 7u;
                s_flip_pending = 1;
                s_flip_request_count++;
            } else {
                cellGcmSetFlipCommand(w & 0xFFu);
            }
            /* ...unless the FIFO is badly backlogged. One flip per drain is
             * right while `get` is keeping up with `put`; when it is megabytes
             * behind it is a deadlock, because the title's ring can only be
             * reclaimed as `get` advances. VF5 submits ~5x faster than one
             * flip per drain consumes, so its 4 MB ring fills, its own
             * buffer-full callback can never free space, and AMGL prints
             * "Command Buffer Overflow" forever. Past the threshold keep
             * draining -- the intermediate presents are frames we are too far
             * behind to show anyway. No-op for a title that is keeping up. */
            const int stop = put < s_fifo_getoff ||
                             put - s_fifo_getoff < GCM_FIFO_CATCHUP_BYTES;
            if (gcm_flip_stats_on())
                gcm_flip_stats_note(put, s_fifo_getoff, s_frame_bytes, !stop, unshown);
            s_frame_bytes = 0;
            if (stop) { why = "flip"; break; }
            continue;
        }

        if (is_jump) {                         /* JUMP: 0x20000000 | offset, or offset | 1 */
            { static int _rd = -1; if (_rd < 0) _rd = getenv("GCM_RECDBG") ? 1 : 0;
              if (_rd) fprintf(stderr, "[JMP] %08X -> %08X (put=%08X)\n",
                               s_fifo_getoff, jump_tgt, put); }
            { u32 tgt = jump_tgt;
              if (!gcm_io2ea(tgt)) { gcm_fifo_bad_branch("JUMP", tgt, w); gcm_fifo_dump_around(s_fifo_getoff); gcm_fifo_resync_why("unmapped-JUMP", &s_fifo_getoff, put); break; }
              /* A JUMP to its own address is the "park the RSX here" idiom:
               * the title leaves it at the write head so the GPU stops if it
               * catches up, and overwrites it when the next segment is
               * appended. Taking it re-reads the same word forever -- 38.8 M
               * times in one Twisted Metal run, which is the drain burning the
               * CPU the guest needs. Stop this pass instead; the next one
               * re-reads the word, so a patched jump is still followed. */
              if (tgt == s_fifo_getoff) {
                  /* Parked. If the title has meanwhile moved `put` somewhere
                   * else, it has switched to its other segment and left this
                   * park standing -- it only patches a park when it reuses that
                   * block. Waiting for a patch that will not come freezes the
                   * FIFO for the rest of the run, so follow the write head. */
                  /* ...but not at once. A title that links its segments by
                   * patching the park moves `put` first and patches second:
                   * inFAMOUS 2 ends each of its main-memory command blocks in
                   * a park, writes the next commands at the ring start, sets
                   * put, and only then rewrites the park as a JUMP to them. A
                   * drain that lands between the two saw put elsewhere, went
                   * straight to it, and dropped everything from the ring
                   * start to put -- the frame's fence with it, so the title
                   * waited on that label for the rest of the run. Give the
                   * patch time to arrive, and resync only for a park that
                   * has stood with the same put for longer than any title
                   * takes between the two stores. */
                  if (put != s_fifo_getoff) {
                      extern unsigned long long ps3_ms_now(void);
                      static u32 park_put;
                      static gcm_run_clock grace;               /* restarts when put moves */
                      const unsigned long long now = ps3_ms_now();
                      if (s_park_off != s_fifo_getoff || park_put != put) {
                          if (s_park_off != s_fifo_getoff) { s_park_since = now; s_ps_parks++; }
                          s_park_off = s_fifo_getoff; park_put = put; gcm_run_clock_start(&grace, now);
                      } else if (gcm_run_clock_ms(&grace, now) >= GCM_PARK_GRACE_MS) {
                          s_park_off = 0; s_park_since = 0; s_ps_resync++;
                          gcm_fifo_dump_around(s_fifo_getoff);
                          gcm_fifo_resync_why("jump-park", &s_fifo_getoff, put);
                      }
                  }
                  why = "parked";
                  s_park_pass = 1;
                  s_kw_park_word = w;   /* (critic) PS3RECOMP_GCM_SPIN_US: ends when the park is patched */
                  break;
              }
              s_fifo_getoff = tgt; }
            continue;
        }
        if ((w & 3) == 2) {                    /* CALL: offset | 2 */
            { u32 tgt = w & 0x1FFFFFFCu;
              if (!gcm_io2ea(tgt)) { gcm_fifo_bad_branch("CALL", tgt, w); gcm_fifo_dump_around(s_fifo_getoff); /* (critic2) the step trace, as for a JUMP */ gcm_fifo_resync_why("unmapped-CALL", &s_fifo_getoff, put); break; }
              s_fifo_calloff = s_fifo_getoff + 4;
              s_fifo_getoff  = tgt; }
            continue;
        }
        if (w == 0x00020000u) {                /* RET */
            s_fifo_getoff = s_fifo_calloff;
            s_fifo_calloff = 0;
            continue;
        }
        if (type == 0 || type == 2) {          /* method (incrementing / NI) */
            u32 count  = (w >> 18) & 0x7FF;
            u32 method = w & 0x1FFCu;
            u32 subch  = (w >> 13) & 7;
            /* TCONST_FIFO=1: how the title actually streams vertex constants.
             * NV4097_SET_TRANSFORM_CONSTANT is a 32-register window; if a block
             * arrives NON-INCREMENTING then every dword carries the same method
             * and any handler that derives the constant slot from the method
             * offset writes them all on top of each other. */
            { static int tf = -1;
              if (tf < 0) { const char* e = getenv("TCONST_FIFO"); tf = e ? atoi(e) : 0; }
              if (tf && subch == 0) {
                  static u32 hist[2048]; static u32 tot[2048]; static int dumped = 0;
                  u32 mi = (method >> 2) & 2047;
                  hist[mi]++; tot[mi] += count;
                  static u64 seen = 0;
                  /* TCONST_FIFO=N (N > 1): dump after N thousand blocks instead of
                   * 200k, to look at a later phase than early boot. */
                  if (++seen == (tf > 1 ? (u64)tf * 1000ull : 200000ull) && !dumped) { dumped = 1;
                      fprintf(stderr, "[TCFIFO] method histogram (blocks, dwords):%c", 10);
                      for (u32 k = 0; k < 2048; k++) if (hist[k])
                          fprintf(stderr, "[TCFIFO]   0x%04X  %8u blocks %10u dwords%c",
                                  k << 2, hist[k], tot[k], 10);
                  }
              } }
            /* NV406E semaphore: OFFSET(0x64) / ACQUIRE(0x68) / RELEASE(0x6C).
             *
             * These were falling through to rsx_process_method as "unknown method" --
             * i.e. no-ops. A title that syncs with cellGcmSetFlipCommandWithWaitLabel and
             * cellGcmGetLabelAddress (Twisted Metal does both) then waits on a label the
             * RSX is supposed to write and never sees it move, so it never appends its
             * next FIFO segment and the GPU sits on a park forever.
             *
             * RELEASE writes the value; ACQUIRE stops this drain pass without consuming
             * the method, so the next pass re-reads it -- which is what the hardware does
             * while it waits. Semaphore offsets index the same label window
             * cellGcmGetLabelAddress hands out. */
            if (subch == 0 && (method == 0x64u || method == 0x68u || method == 0x6Cu)) {
                int sem_blocked = 0;
                for (u32 i = 0; i < count; i++) {
                    u32 dea = gcm_io2ea(s_fifo_getoff + 4 + i * 4);
                    if (!dea) break;
                    u32 m = (type == 0) ? method + i * 4 : method;
                    u32 v = vm_read32(dea);
                    u32 la = GCM_LABEL_GUEST_BASE + (s_sema_offset & 0xFFFFu);
                    /* (rsx-par) an acquire reads the label only once every queued
                     * action that may write one (the release it may wait for) ran */
                    if (split && m == 0x68u) rsx_split_wait_labels();
                    /* GCM_SEMA_TRACE=<n>: the first n NV406E semaphore methods with
                     * the label's current value, to read a title's RSX<->SPU/PPU
                     * sync protocol (GCM_RECDBG alone shows 12). */
                    { static int sn = 0, cap = -1;
                      if (cap < 0) { const char* e = getenv("GCM_SEMA_TRACE"); cap = e ? atoi(e) : (getenv("GCM_RECDBG") ? 12 : 0); }
                      if (cap && PS3_LOG_FIRST(sn, cap))
                        fprintf(stderr, "[SEMA] m=0x%02X v=0x%08X off=0x%X label=0x%08X\n", m, v,
                                m == 0x64u ? v : s_sema_offset,
                                vm_read32(GCM_LABEL_GUEST_BASE + ((m == 0x64u ? v : s_sema_offset) & 0xFFFFu))); }
                    if (m == 0x68u && vm_read32(la) == v) rsx_snap_event(1, s_sema_offset);
                    /* PS3_FTRACE: an acquire this walker sat on is satisfied now. */
                    if (m == 0x68u && s_ft_sema_t0 && vm_read32(la) == v) {
                        ps3_ft_span(PS3_FT_SEMA, 0, s_sema_offset, v, s_ft_sema_t0, ps3_ft_now());
                        s_ft_sema_t0 = 0;
                    }
                    if (m == 0x68u && s_ha_sema_t0 && vm_read32(la) == v) {   /* hitch attribution */
                        s_ha_sema_ns += ps3_ft_now() - s_ha_sema_t0;
                        s_ha_sema_t0 = 0;
                    }
                    if (m == 0x64u)      s_sema_offset = v;
                    else if (m == 0x6Cu) {
                        if (split) rsx_split_act(RSX_SPLIT_ACT_SEMA, la, v, 0);   /* (rsx-par) behind the draws before it */
                        else vm_write32(la, v);
                    }
                    else if (m == 0x68u && vm_read32(la) != v) {
            /* GCM_SEMA_ACQUIRE=1 makes ACQUIRE actually block, which is what
             * the hardware does. Off by default: a title whose label nothing
             * ever writes would wedge the drain permanently, and RELEASE on
             * its own is the half that unblocks a waiting guest. */
            static int blk = -1; static unsigned blk_ms;
            if (blk < 0) { const char* e = getenv("GCM_SEMA_ACQUIRE"); blk = e ? atoi(e) : 0;
                           const char* t = getenv("GCM_SEMA_ACQUIRE_MS"); blk_ms = t ? (unsigned)atoi(t) : 0; }
            if (blk) {
                /* GCM_SEMA_ACQUIRE_MS=<ms>: a bounded wait. The hardware waits
                 * for ever; an emulated producer that never comes (a boot-time
                 * label nobody services yet) would wedge the drain, so after
                 * <ms> on the same acquire it is consumed and said so. inFAMOUS 2
                 * acquires the label its SPU lighting writes each frame: waiting
                 * here is what orders the RSX's sampling after that write. */
                static u32 b_off, b_val; static unsigned long long b_t0;
                static gcm_run_clock b_run;   /* time the wait ran (GCM_STALL_GAP_MS) */
                extern unsigned long long ps3_ms_now(void);
                const unsigned long long now = ps3_ms_now();
                if (!b_t0 || b_off != s_sema_offset || b_val != v) { b_off = s_sema_offset; b_val = v; b_t0 = now;
                                                                      gcm_run_clock_start(&b_run, now); }
                if (!blk_ms || gcm_run_clock_ms(&b_run, now) < blk_ms) {
                    if (PS3_FT_ON() && !s_ft_sema_t0) s_ft_sema_t0 = ps3_ft_now();
                    if (!s_ha_sema_t0) s_ha_sema_t0 = ps3_ft_now();
                    s_sema_wait_val = v;   /* (critic) cellGcm_fifo_kick_wait's early end */
                    sem_blocked = 1; break;
                }
                if (s_ft_sema_t0) {
                    ps3_ft_span(PS3_FT_SEMA_GAVEUP, 0, s_sema_offset, v, s_ft_sema_t0, ps3_ft_now());
                    s_ft_sema_t0 = 0;
                }
                if (s_ha_sema_t0) { s_ha_sema_ns += ps3_ft_now() - s_ha_sema_t0; s_ha_sema_t0 = 0; }
                s_ha_sema_gaveup++;
                { static int tn; if (PS3_LOG_FIRST(tn, 12))
                    fprintf(stderr, "[SEMA] acquire off=0x%X == 0x%08X gave up after %u ms (label 0x%08X)\n",
                            s_sema_offset, v, blk_ms, vm_read32(la)); }
                b_t0 = 0;
            }
        }
                }
                if (sem_blocked) { s_sema_blocked_pass = 1; s_sema_live = 1; break; }
                s_frame_bytes += 4 + count * 4; s_walk_bytes += 4 + count * 4;
                s_fifo_getoff += 4 + count * 4;
                continue;
            }
            /* A header whose words all go to the draw engine and nothing else
             * (gcm_fifo_burst_ok): handed over whole, read straight from its
             * one IO page, instead of a translation, an out-of-line guest
             * read and the per-word routing below for every word. */
            if (count && gcm_fifo_burst_ok(subch, method, type, count)) {
                const u32 io0 = s_fifo_getoff + 4u, io1 = s_fifo_getoff + count * 4u;
                const u32 ea0 = gcm_io2ea(io0);
                if (ea0 && (io0 >> 20) == (io1 >> 20)) {
                    extern u8* vm_base;
                    if (split) rsx_split_burst((subch << 13) | method, type == 0, (const u32*)(vm_base + ea0), count);
                    else rsx_draw_engine_burst((subch << 13) | method, type == 0,
                                               (const u32*)(vm_base + ea0), count);
                    s_frame_bytes += 4 + count * 4; s_walk_bytes += 4 + count * 4;
                    s_fifo_getoff += 4 + count * 4;
                    continue;
                }
            }
            const int fifo_dbg = gcm_fifo_dbg();
            for (u32 i = 0; i < count; i++) {
                u32 dea = gcm_io2ea(s_fifo_getoff + 4 + i * 4);
                if (!dea) break;
                u32 m = (type == 0) ? method + i * 4 : method;
                /* Each data word is read once: the paths below all take it. */
                const u32 dw = vm_read32(dea);
                /* GCM_SET_USER_COMMAND is method 0xEB00, which this decode
                 * splits into subchannel 7 / method 0x0B00 -- so it was landing
                 * in the 2D engine path and being dropped as unrecognised. It
                 * is not a 2D method at all: it tells the RSX to raise a
                 * USER_CMD interrupt, which is what ps1_netemu's flip path
                 * blocks on. */
                if (fifo_dbg) { static int sd = -1; static unsigned long s7 = 0;
                  if (sd < 0) sd = getenv("GCM_SUBCH7") ? 1 : 0;
                  if (sd && subch == 7 && (__atomic_fetch_add(&s7, 1, __ATOMIC_RELAXED) < 16))
                      fprintf(stderr, "[subch7] method=0x%04X data=0x%08X\n",
                              m, dw); }
                if (subch == 7 && m == 0x0B00u) {
                    extern void rsx_raise_user_cmd(u32 arg);
                    if (split) rsx_split_act(RSX_SPLIT_ACT_WORD, (subch << 13) | m, dw, s_fifo_getoff);
                    else rsx_raise_user_cmd(dw);
                    continue;
                }
                /* GCM_SUBCH1_3D=1: treat subchannel 1 as the 3D object too.
                 * The title issues NV4097 methods on it -- 0x1A80..0x1AC0 is
                 * SET_TEXTURE_OFFSET for units 4-6 -- and the 2D path silently
                 * discards whatever it does not recognise, so that state never
                 * reaches the renderer. SET_OBJECT binds are not tracked, which
                 * is what makes the subchannel-to-engine mapping a guess. */
                /* GCM_OBJDBG=1: log every SET_OBJECT (method 0). The handle a
                 * title binds to a subchannel is what says which engine that
                 * subchannel drives; comparing subchannel 1 against subchannel 0
                 * turns the routing below from a guess into a reading. */
                if (fifo_dbg) { static int od = -1;
                  if (od < 0) { const char* e = getenv("GCM_OBJDBG"); od = e ? 1 : 0; }
                  if (od && m == 0) {
                      static unsigned seen[8]; static int have[8];
                      const unsigned h = dw;
                      if (subch < 8 && (!have[subch] || seen[subch] != h)) {
                          have[subch] = 1; seen[subch] = h;
                          printf("[GCM-OBJ] subch=%u SET_OBJECT handle=0x%08X%c",
                                 subch, h, 10); fflush(stdout);
                      } } }
                /* The subchannel is a binding slot, not an engine selector: a
                 * title may bind NV4097 to something other than subchannel 0.
                 * Twisted Metal uses subchannel 1, and caner (canersaka) hit
                 * the same thing in Yakuza Dead Souls, where SPU-built command
                 * lists bind NV4097 elsewhere -- his rsx_live_draw.c masks the
                 * subchannel out of the method for exactly this reason.
                 *
                 * gcm_2d_method only ever handles subchannels 2..7 (NV3062,
                 * NV308A/NV309E, NV3089), so anything arriving on 1 was being
                 * dropped on the floor -- for this title that included
                 * SET_SHADER_PROGRAM, leaving one stale fragment program bound
                 * for every draw in the game. Treat the subchannels the 2D path
                 * does not claim as 3D. GCM_SUBCH1_2D=1 restores the old split.
                 */
                const int s1_2d = gcm_subch1_2d();
                /* GCM_SUBCH_GATE=<file>: while <file> exists, log every method on a
                 * non-zero subchannel -- what each binding slot actually carries. */
                if (fifo_dbg) { static const char* sg = (const char*)-1; static unsigned poll; static int open_g;
                  if (sg == (const char*)-1) sg = getenv("GCM_SUBCH_GATE");
                  const u32 cm3 = m & 0x1FFCu;
                  /* GCM_VAL_WATCH=<hex>: every FIFO word carrying that value, any method. */
                  { static u32 vw = 1;
                    if (vw == 1) { const char* e = getenv("GCM_VAL_WATCH"); vw = e ? (u32)strtoul(e, 0, 16) : 0; }
                    if (vw && dw == vw) {
                        static int vn; if (PS3_LOG_FIRST(vn, 64))
                            printf("[valwatch] subch=%u method=0x%04X = 0x%08X get=0x%X%c", subch, m, vw, s_fifo_getoff, 10);
                    } }
                  /* ...and surface offsets (colour 0x210/0x218/0x288/0x28C, zeta 0x214) and texture 1 offset */
                  if (sg && !subch && (cm3 == 0x210 || cm3 == 0x214 || cm3 == 0x218 || cm3 == 0x288 || cm3 == 0x28C ||
                                       cm3 == 0x1A20)) {
                      if ((poll++ & 255u) == 0) { FILE* g = fopen(sg, "rb"); open_g = g != NULL; if (g) fclose(g); }
                      if (open_g) printf("[surfreg] 0x%04X = 0x%08X%c", cm3, dw, 10);
                  }
                  if (sg && subch) {
                      if ((poll++ & 255u) == 0) { FILE* g = fopen(sg, "rb"); open_g = g != NULL; if (g) fclose(g); }
                      if (open_g) printf("[subch] %u 0x%04X = 0x%08X%c", subch, m, dw, 10);
                  } }
                /* Mirror the whole method stream into the live NV4097->D3D12
                 * engine (caner / canersaka). Inert unless RSX_LIVE_DRAW is set.
                 * It wants the raw method with its subchannel bits -- it
                 * canonicalises with & 0x1FFC itself. */
                { static int live = -1;
                  if (live < 0) live = rsx_live_draw_enabled();
                  if (live) rsx_live_draw_method((subch << 13) | m, dw); }

                /* And into the platform-neutral register-file draw engine,
                 * which is the macOS path under PS3RECOMP_RSX_ENGINE=dispatch.
                 * It wants the raw method with its subchannel bits for the
                 * same reason the live engine does, and masks them itself. */
                { static int eng = -1;
                  if (eng < 0) eng = rsx_draw_engine_enabled();
                  if (eng) {
                      if (split) rsx_split_method((subch << 13) | m, dw);
                      else rsx_draw_engine_method((subch << 13) | m, dw);
                  } }

                /* RSX_LIVE_FEED_DBG=1: what the FIFO actually carries. Counted
                 * for any run, live engine or not, so the method stream and the
                 * backend's draw callbacks can be compared in one measurement. */
                if (fifo_dbg) { static int fdbg = -1;
                  if (fdbg < 0) fdbg = getenv("RSX_LIVE_FEED_DBG") ? 1 : 0;
                  if (fdbg) {
                      static long long n_be = 0, n_va = 0, n_ia = 0, tick = 0;
                      const u32 cm = m & 0x1FFCu;
                      if      (cm == 0x1808u) n_be++;
                      else if (cm == 0x1814u) n_va++;
                      else if (cm == 0x1824u) n_ia++;
                      if (((++tick) % 200000) == 0)
                          printf("[feed] begin_end=%lld draw_arrays=%lld draw_index=%lld\n",
                                 n_be, n_va, n_ia), fflush(stdout);
                  } }

                /* Driver methods (0xE9xx, 0xEBxx) occupy bits 2-15 of the
                 * FIFO word; the standard 11-bit extraction (w & 0x1FFC)
                 * truncates them.  Reconstruct the full address the same way
                 * the draw-engine call above does. */
                { const u32 mfull = (subch << 13) | m;
                if (split) {
                    /* (rsx-par) the rest on the exec thread, behind the draws before it */
                    if (gcm_word_needs_act(subch, m, s1_2d))
                        rsx_split_act(RSX_SPLIT_ACT_WORD, mfull, dw, s_fifo_getoff);
                } else if (mfull == 0xEB00u || mfull == 0xEB04u) {
                    cellGcmQueueUserCommand(dw);
                } else if (mfull == 0xE920u || mfull == 0xE924u) {
                    cellGcmSetFlipCommand(dw & 7u);
                } else if (subch == 0 || (subch == 1 && !s1_2d)) {
                    /* GCM_LEAN_STATE (live): with the draw engine fed and no
                     * vtable backend registered, rsx_process_method's rsx_state
                     * is read by nobody -- only its label/semaphore writes and
                     * the reference fence leave it -- yet it decoded every word
                     * again (13% of the RSX thread on the New Marais walk).
                     * Those methods still go through; the rest is skipped.
                     * 0 = every method, as before. */
                    if (!gcm_lean_state() || m == 0x0050u || (m >= 0x1D6Cu && m <= 0x1D74u))
                        rsx_process_method(&s_state, m, dw);
                    /* NV406E_SET_REFERENCE: queue the fence value for PACED
                     * publication (gcm_ref_publish below) instead of letting a
                     * later fence in the same batch overwrite it. */
                    if (m == 0x50) gcm_ref_push_at(g_rsx_last_reference, s_fifo_getoff);
                    /* NV4097_GET_REPORT: (type << 24) | offset into the report
                     * area. Sample counts are not measured, so a ZPASS query
                     * reports "visible" -- the conservative answer; reading 0
                     * tells the title every queried object is occluded. */
                    if (m == 0x1800) {
                        const u32 v = dw, idx = (v & 0xFFFFFFu) / 16u;
                        { static int rn = 0; if (PS3_LOG_FIRST(rn, 6)) printf("[GET_REPORT] type=%u idx=%u%c", v >> 24, idx, 10); }
                        if (idx < CELL_GCM_MAX_REPORT_COUNT) {
                            s_report_data[idx].timestamp = get_timestamp_ns();
                            s_report_data[idx].value = (v >> 24) == 1u ? 0xFFFFu : 0u;
                        }
                    }
                } else
                    gcm_2d_method(subch, m, dw);
                }
            }
            s_frame_bytes += 4 + count * 4; s_walk_bytes += 4 + count * 4;
            s_fifo_getoff += 4 + count * 4;
            continue;
        }
        { static int _uw = 0; if (PS3_LOG_FIRST(_uw, 16) && getenv("RTT_DUMP"))
            fprintf(stderr, "[FIFOUW] unknown word 0x%08X at getoff 0x%X\n", w, s_fifo_getoff); }
        s_fifo_getoff += 4;                    /* unknown word: skip */
    }
    /* (rsx-par) split: caught up -- if the title has moved put meanwhile, walk
     * on while the exec thread works (up to ~4 ms of drain), instead of
     * stopping here to wait for it and starting the next drain cold. Then
     * everything queued runs before anything below (GET, the fence
     * publication, the ring recycle) and before the caller presents. */
    if (split) {
        if (s_fifo_getoff == put && budget > 0 && !strcmp(why, "caught-up") &&
            ps3_ms_now() - s_drain_in_ms < 4) {
            const u32 np = vm_read32(GCM_CONTROL_GUEST_ADDR + 0);
            atomic_thread_fence(memory_order_acquire);
            if (np != put) { put = np; goto walk_more; }
        }
        rsx_split_sync();
    }

    /* Walker watchdog. Everything above recovers from a *recognised* stall --
     * a branch we cannot take, a park the title abandoned. A walker that stops
     * for any other reason simply freezes `get` for the rest of the run and
     * nothing renders again, and which of those happens is a race, so the same
     * boot renders one time in five. If `get` has not moved across several
     * passes while `put` is somewhere else, there is work the walker will
     * never reach: resynchronise to the write head. Same trade as the
     * unmapped-get path -- commands are lost, the FIFO lives. */
    /* The stuck-detector above only fires when `get` stops MOVING. A walker
     * going in circles moves plenty -- it just never arrives. Twisted Metal
     * and flOw both write one ring and their JUMPs lead to `put`; VF5 does
     * not. AMGL leaves libgcm's default ring at IO 0 parked on its own JUMP
     * loop and drives the RSX from a second buffer it mapped at IO 0xA00000,
     * so the walker circulates the default ring forever, `get` never reaches
     * `put`, the title's ring is never reclaimed, and AMGL prints "Command
     * Buffer Overflow" until it runs off the end of its own mapping.
     *
     * A drain that burns its ENTIRE word budget without arriving is that
     * signature: no legitimate frame leaves a million words pending, and a
     * genuine burst clears within a tick or two. Circling does not, so
     * require several passes in a row before following the write head.
     *
     * ponytail: consecutive-budget-burn is a heuristic, not a proof. The real
     * fix is knowing which buffer the RSX is bound to -- that needs the
     * SetQueueHandler / context-switch path modelled, not a walker rule. */
    { static int burned = 0, passes = -1;
      if (passes < 0) { const char* e = getenv("GCM_FIFO_CIRCLE_PASSES");
                        passes = e ? atoi(e) : 4; }
      if (passes && budget <= 0 && s_fifo_getoff != put) {
          if (++burned >= passes) {
              static int n = 0;
              if (PS3_LOG_FIRST(n, 8))
                  fprintf(stderr, "[cellGcmSys] FIFO walker circling (get=0x%08X, "
                          "put=0x%08X, full budget burned x%d) -- following the "
                          "write head\n", s_fifo_getoff, put, burned);
              gcm_fifo_resync_why("circling", &s_fifo_getoff, put);
              burned = 0;
          }
      } else burned = 0; }

    /* A drain parked on a semaphore acquire (GCM_SEMA_ACQUIRE) holds `get`
     * on purpose: that is the wait, not a stuck walker. The acquire's own
     * bound (GCM_SEMA_ACQUIRE_MS) is what ends it.
     *
     * So does a self-JUMP park, and its grace (GCM_PARK_GRACE_MS) is what ends
     * that. This counter counts drain passes, not time: a park stops every
     * pass at the same `get`, so it fired a few milliseconds in -- before the
     * title had patched the park into a JUMP to the commands it had just put
     * at the ring start -- and the resync dropped them. inFAMOUS 2 stopped
     * presenting for good that way ~40 s into the daytime pier ("stuck-get"
     * at a 0x2055C940 self-jump with no "jump-park" before it), and in about
     * one New Marais run in five. */
    { static u32 last = 0xFFFFFFFFu; static int stuck = 0;
      if (s_sema_blocked_pass || s_park_pass) {
          stuck = 0; last = s_fifo_getoff; s_sema_blocked_pass = 0; s_park_pass = 0; }
      else if (s_fifo_getoff == last && s_fifo_getoff != put) {
          if (++stuck >= 8) { gcm_fifo_dump_around(s_fifo_getoff);
                             gcm_fifo_resync_why("stuck-get", &s_fifo_getoff, put); stuck = 0; }
      } else { stuck = 0; last = s_fifo_getoff; } }

    /* Publish progress: get chases put; ref advances at most ONE queued fence
     * per tick (gcm_ref_push/gcm_ref_publish_one above) so every SET_REFERENCE
     * value stays observable to equality-waiters. The wrap recycle path polls
     * the drained EA. */
    s_drain_why = s_sema_live ? "sema-acquire" : why;
    /* (critic) stopped at a flip with words left: the next pass has work at once */
    s_kw_more = !s_sema_live && why && !strcmp(why, "flip") && s_fifo_getoff != put;
    gcm_flip_stats_tick();
    if (draindbg) {
        static int n2 = 0;
        if (n2++ % 60 == 0)
            fprintf(stderr, "[DRAINEND] %s: %08X -> %08X (%u bytes), put=%08X\n",
                    why, start_off, s_fifo_getoff, s_fifo_getoff - start_off, put);
    }
    /* Recycle the TITLE'S OWN command buffer when its callback will not.
     *
     * gCellGcmCurrentContext points at the title's context, not the one
     * cellGcmInit handed back: Virtua Fighter 5's is a 512 KB ring at EA
     * 0x4AE00000, and the buffer-full callback AMGL installs (0x006A8030) only
     * prints "[AMGL]:[ERROR] Command Buffer Overflow!" and returns 0 -- which
     * libgcm's gcmReserve reads as "retry", so the title writes straight past
     * `end` into unmapped memory and never renders again.
     *
     * Hardware never reaches that state because the ring is recycled once the
     * RSX has consumed it. Do the same here, on the title's context, and only
     * when it is provably safe: the walker has drained everything the title
     * submitted (get == put) and `current` is within one reserve of `end`.
     * A title whose own callback recycles never reaches that state, so this is
     * inert for every port that works today.
     *
     * ponytail: append the JUMP at `current` the way the SDK's own callback
     * does, rather than tracking a second copy of the ring's geometry. */
    if (s_gcm_ctx_out_ea) {
        u32 ctx = vm_read32(s_gcm_ctx_out_ea);
        u32 begin = ctx ? vm_read32(ctx + 0x0) : 0;
        u32 end   = ctx ? vm_read32(ctx + 0x4) : 0;
        u32 cur   = ctx ? vm_read32(ctx + 0x8) : 0;
        /* `get == put` -- a fully drained FIFO -- was too strict. It holds
         * while a title is only clearing, and stops holding the moment its SPU
         * work starts producing real command volume: the walker then always has
         * a backlog, the recycle never fires, and the ring overflows again.
         * That is exactly where Virtua Fighter 5 stalls once its "SPU Delegate"
         * group starts feeding the renderer.
         *
         * Hardware does not need a drained FIFO either -- only that the bytes
         * about to be overwritten have already been read. So require the walker
         * to be a margin PAST `begin` instead: the head of the ring is consumed,
         * which is the region the guest is about to write. */
        u32 io_begin_chk = begin ? gcm_ea2io(begin) : 0xFFFFFFFFu;
        int head_consumed =
            (io_begin_chk != 0xFFFFFFFFu) &&
            (s_fifo_getoff == put ||
             (s_fifo_getoff > io_begin_chk &&
              s_fifo_getoff - io_begin_chk >= GCM_RECYCLE_MARGIN));
        /* Already overrun. gcmReserve writes past `end` when its callback
         * declines to recycle (AMGL's only prints), and once the guest is out
         * there the walker follows into memory that is not the ring: `get`
         * stops advancing, head_consumed can never become true again, and the
         * title is wedged for good -- which is exactly where Virtua Fighter 5
         * stops, at the same flip every run however long it is left.
         *
         * There is nothing left to protect at that point. Recycling loses
         * whatever was written past the end; not recycling loses the rest of
         * the run. */
        /* A pass that stopped at a flip has a backlog on purpose (the next
         * frame), and teleporting `get` to `begin` would drop it -- fence
         * writes included, so the title then waits on a reference that never
         * comes (The Simpsons Arcade Game froze at its 4th recycle). The next
         * pass drains to `put` and recycles then. */
        if (!strcmp(why, "flip") && s_fifo_getoff != put) head_consumed = 0;
        /* Only once the title has been SEEN to overrun. Near `end` is also
         * where a title whose callback does recycle sits just before calling
         * it: Tornado Outbreak's steps its context through 64 KB segments of a
         * larger ring, and recycling under it from this thread rewound put/get
         * behind its callback's bookkeeping -- which then waited forever for a
         * GET that could not arrive. A callback that declines (AMGL's) always
         * overruns first, so the first overrun is recycled by the path above
         * and near-end recycling is enabled from then on. */
        static int s_title_overran = 0;
        if (begin && end > begin && cur >= end) { head_consumed = 1; s_title_overran = 1; }
        if (begin && end > begin && cur >= begin && cur + GCM_RECYCLE_SLACK >= end
                && head_consumed && s_title_overran) {
            u32 io_begin = gcm_ea2io(begin);
            if (io_begin != 0xFFFFFFFFu) {
                u32 jmp_at = (cur + 4 <= end) ? cur : end - 4;
                vm_write32(jmp_at, 0x20000000u | io_begin);   /* JUMP -> begin */
                vm_write32(ctx + 0x8, begin);                 /* current = begin */
                vm_write32(GCM_CONTROL_GUEST_ADDR + 0, io_begin);  /* put */
                s_fifo_getoff = io_begin;                          /* get */
                put = io_begin;
                static int n = 0;
                if (PS3_LOG_FIRST(n, 8))
                    fprintf(stderr, "[cellGcmSys] recycled the title's own ring "
                            "(ctx=0x%08X begin=0x%08X end=0x%08X) -- its callback "
                            "does not%c", ctx, begin, end, 10);
            }
        }
    }

    g_gcm_fifo_drained_ea = gcm_io2ea(s_fifo_getoff);
    /* GCM_GET_EQ_PUT=1: publish `get` as having reached `put` rather than where
     * the walker actually is. A probe, not a fix -- it removes the back-pressure
     * a title uses to avoid overwriting commands the GPU has not read. It exists
     * to answer one question for a title that reports its ring full while the
     * walker says it is drained: is the title reading `get`, or something else?
     * If the overflow survives this, `get` is not what it consults. */
    { static int eq = -1;
      if (eq < 0) { const char* e = getenv("GCM_GET_EQ_PUT"); eq = e ? atoi(e) : 0; }
      vm_write32(GCM_CONTROL_GUEST_ADDR + 4, eq ? put : s_fifo_getoff); }

    AcquireSRWLockExclusive(&s_ref_pub_lock);                           /* ref */
    gcm_ref_publish_one();
    ReleaseSRWLockExclusive(&s_ref_pub_lock);
}

/* FIFO command-buffer-full callback body. The title's inline gcmReserve calls
 * context->callback(context, count) (routed here via the synthetic OPD) when the
 * ring's `current` nears `end`. Real libgcm flushes, waits for the RSX `get` to
 * pass, then recycles `current` to `begin`. We mirror that: wait (bounded) for
 * the ticker drain to consume everything up to `current`, then reset current to
 * begin. Runs on the guest thread; the ticker owns all rendering, so we only
 * pointer-shuffle here (the drain's own wrap-detect handles s_get). Without this
 * the ring never recycles and the whole pipeline wedges once the FIFO wraps
 * (~200 frames at the 256KB default), which looked like a GPU/driver hang. */
static u32 gcm_ea2io(u32 ea)
{
    u32 page = ea >> 20;
    if (page >= 65536) return 0xFFFFFFFFu;
    u16 io_page = s_io_address_table[page];
    if (io_page == 0xFFFF) return 0xFFFFFFFFu;
    return ((u32)io_page << 20) | (ea & 0xFFFFFu);
}

void cellGcm_fifo_recycle(u32 ctx_ea)
{
    if (!ctx_ea) return;
    u32 begin   = vm_read32(ctx_ea + 0x0);
    u32 current = vm_read32(ctx_ea + 0x8);
    if (current <= begin) return;                       /* nothing to recycle */

    static int blinkdbg = -1; if (blinkdbg < 0) blinkdbg = getenv("CELLMARK_BLINKDBG") != NULL;
    if (blinkdbg)
        printf("[RECYCLE] ring wrap: current=0x%08X -> begin=0x%08X\n", current, begin);

    static int s_recdbg = -1;
    if (s_recdbg < 0) s_recdbg = getenv("GCM_RECDBG") ? 1 : 0;
    if (s_recdbg)
        fprintf(stderr, "[REC>] tid=%lu begin=%08X cur=%08X put=%08X get=%08X drained=%08X\n",
                GetCurrentThreadId(), begin, current,
                vm_read32(GCM_CONTROL_GUEST_ADDR + 0),
                vm_read32(GCM_CONTROL_GUEST_ADDR + 4),
                g_gcm_fifo_drained_ea);

    /* Do what the SDK's default command-buffer-full callback does: append a
     * JUMP-to-begin at the write head and move `put` to begin. The FIFO walker
     * consumes the tail, follows the jump, and idles at begin; then it's safe
     * for the guest to write from begin (that region was consumed long ago). */
    u32 io_begin = gcm_ea2io(begin);
    if (io_begin != 0xFFFFFFFFu) {
        vm_write32(current, 0x20000000u | io_begin);            /* JUMP begin  */
        /* The jump has to be in guest memory before `put` moves behind the
         * walker's `get`. Program order is not enough on arm64: the walker
         * may observe the new `put`, find `get` != `put`, and read the old
         * word at `current` before the jump lands there. */
        atomic_thread_fence(memory_order_release);
        vm_write32(GCM_CONTROL_GUEST_ADDR + 0, io_begin);        /* put = begin */
    }

    /* Wait (bounded ~2s) for the walker to consume the tail + take the jump so
     * no commands are lost; a stalled ticker degrades to dropped commands. */
    int spins = 0;
    while (g_gcm_fifo_drained_ea != begin && spins < 2000) { Sleep(1); spins++; }
    if (spins >= 2000) {
        static int warned = 0;
        if (PS3_LOG_FIRST(warned, 4))
            printf("[cellGcmSys] fifo recycle: drain stalled (drained=0x%08X begin=0x%08X)\n",
                   g_gcm_fifo_drained_ea, begin);
    }

    vm_write32(ctx_ea + 0x8, begin);                    /* recycle ring to base */

    if (s_recdbg)
        fprintf(stderr, "[REC<] tid=%lu cur-now=%08X put=%08X get=%08X drained=%08X spins=%d\n",
                GetCurrentThreadId(), vm_read32(ctx_ea + 0x8),
                vm_read32(GCM_CONTROL_GUEST_ADDR + 0),
                vm_read32(GCM_CONTROL_GUEST_ADDR + 4),
                g_gcm_fifo_drained_ea, spins);
}

/* NID: 0xDC09357E */
s32 cellGcmSetDisplayBuffer(u32 bufferId, u32 offset, u32 pitch,
                            u32 width, u32 height)
{
    printf("[cellGcmSys] SetDisplayBuffer(id=%u, offset=0x%X, pitch=%u, %ux%u)\n",
           bufferId, offset, pitch, width, height);

    if (bufferId >= CELL_GCM_MAX_DISPLAY_BUFFER_NUM)
        return CELL_GCM_ERROR_INVALID_VALUE;

    s_display_buffers[bufferId].offset = offset;
    s_display_buffers[bufferId].pitch  = pitch;
    s_display_buffers[bufferId].width  = width;
    s_display_buffers[bufferId].height = height;
    s_display_buffer_set[bufferId] = 1;

    /* Tell the live engine too, or it has no registered scanout to present and
     * falls back to whatever surface happens to be current. Display buffers are
     * always in RSX local memory (location 0). */
    if (rsx_live_draw_enabled())
        rsx_live_draw_set_display_buffer(bufferId, 0, offset, pitch, width, height);
    if (rsx_draw_engine_enabled())
        rsx_draw_engine_set_display_buffer(bufferId, 0, offset, pitch, width, height);

    return CELL_OK;
}

/* Backend query (not a guest API): is this raw RSX offset a registered
 * display buffer? Used to split display draws (-> backbuffer) from
 * offscreen render-to-texture passes. */
int cellGcmOffsetIsDisplay(u32 offset)
{
    for (int i = 0; i < CELL_GCM_MAX_DISPLAY_BUFFER_NUM; i++)
        if (s_display_buffer_set[i] && s_display_buffers[i].offset == offset)
            return 1;
    return 0;
}

/* NID: 0xEAA52F23 */
/* How many display buffers the title has actually registered. cellResc needs
 * it to know how far to rotate its flip target; nothing else about the set is
 * anyone else's business, so this stays a count rather than exposing the array. */
u32 cellGcm_display_buffer_count(void)
{
    u32 n = 0;
    for (u32 i = 0; i < CELL_GCM_MAX_DISPLAY_BUFFER_NUM; i++)
        if (s_display_buffer_set[i]) n++;
    return n;
}

static s32 gcm_flip_request(u32 bufferId, int in_fifo)
{
    /* GCM_FLIPCOUNT=1: every flip, with a timestamp. FLIP_DBG caps at 20 lines,
     * which answers "did it ever flip?" and not "is it still flipping, and how
     * fast?" -- the question that matters when a title stops appearing on
     * screen. Virtua Fighter 5 does not use the in-FIFO 0xFEADxxxx flip word at
     * all (that counter stays at zero for the whole run); it flips through
     * cellGcmSetFlip, so this is where its frame rate actually is. */
    { static int dbg = -1;
      if (dbg < 0) dbg = getenv("GCM_FLIPCOUNT") ? 1 : 0;
      if (dbg) {
          extern unsigned long long ps3_ms_now(void);
          static unsigned long long n = 0, t0 = 0;
          unsigned long long now = ps3_ms_now();
          if (!t0) t0 = now;
          ++n;
          if (n <= 400ull || (n % 10ull) == 0)
              fprintf(stderr, "[flip] %llu at %llu ms%c", n, now - t0, 10);
      } }

    /* GCM_FLIP_BT=<n>: dump the flipping thread's guest stack for the flips
     * around n. When a title renders correctly for a while and then stops
     * submitting -- no error, no unresolved import, no overflow -- the only
     * thing separating the last good frame from the first missing one is what
     * the render path was doing on each. This is the window onto that. */
    { static long long at = -2;
      if (at == -2) { const char* e = getenv("GCM_FLIP_BT"); at = e ? atoll(e) : -1; }
      if (at >= 0) {
          static unsigned long long m = 0;
          ++m;
          if ((long long)m >= at - 5 && (long long)m <= at + 5) {
              extern PPU_THREAD_LOCAL ppu_context* g_active_ctx;
              extern void ppu_dump_guest_stack(ppu_context*, const char*);
              if (g_active_ctx) {
                  char tag[48];
                  snprintf(tag, sizeof tag, "flip#%llu", m);
                  ppu_dump_guest_stack(g_active_ctx, tag);
                  { extern void ppu_dump_bctrl_ring(uint32_t, const char*);
                    ppu_dump_bctrl_ring((uint32_t)g_active_ctx->thread_id, tag); }
              }
          }
      } }

    { static int _n=0; if (getenv("FLIP_DBG") && PS3_LOG_FIRST(_n, 20))
        fprintf(stderr, "[FLIP] SetFlipCommand(buf=%u) set=%d\n",
                bufferId, bufferId < CELL_GCM_MAX_DISPLAY_BUFFER_NUM ? s_display_buffer_set[bufferId] : -1); }
    if (bufferId >= CELL_GCM_MAX_DISPLAY_BUFFER_NUM)
        return CELL_GCM_ERROR_INVALID_VALUE;

    if (!s_display_buffer_set[bufferId])
        return CELL_GCM_ERROR_INVALID_VALUE;


    /* in_fifo: the flip sits in the command stream as a GCM_FLIP_MARKER, and
     * the drain does this half when it reaches it (see the 0xFEAD case). */
    if (!in_fifo) {
        s_flip_import++;
        if (s_flip_pending) s_flip_import_merged++;
        if (gcm_flip_stats_on()) {
            const u32 b = __atomic_exchange_n(&s_frame_bytes, 0u, __ATOMIC_RELAXED);
            __atomic_fetch_add(&s_imp_bytes_sum, (unsigned long long)b, __ATOMIC_RELAXED);
            u32 m = __atomic_load_n(&s_imp_bytes_max, __ATOMIC_RELAXED);
            while (b > m && !__atomic_compare_exchange_n(&s_imp_bytes_max, &m, b, 1, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
        }
        s_current_display_buffer_id = bufferId;
        s_flip_pending = 1;   /* ticker: present BEFORE the next drain */
        s_flip_request_count++;
    }
    /* Flip requested but not yet shown: a subsequent cellGcmSetWaitFlip blocks
     * until the present thread's cellGcmTickFlip marks it done (vsync). */
    FLIP_LOCK();
    s_flip_status = CELL_GCM_FLIP_STATUS_WAITING;
    s_flip_req_seq++;
    FLIP_UNLOCK();
    s_last_flip_time = get_timestamp_ns();
    if (PS3_FT_ON()) { const uint64_t t = ps3_ft_now();
                       ps3_ft_span(PS3_FT_FLIP_REQ, 0, bufferId, (uint32_t)in_fifo, t, t); }

    /* Invoke via OPD resolution, not a raw call into guest code.
     *
     * On hardware this callback fires when the flip COMPLETES, at vsync --
     * which is what cellGcmTickFlip does. Calling it again here, at request
     * time, is a compatibility wart: some titles need the early edge, but a
     * title that issues its first flip while still initialising then runs its
     * handler against half-built state. Tokyo Jungle faults that way, in
     * func_001F2D3C, dereferencing a singleton it has not constructed yet.
     * GCM_FLIPCB_ONTICK=1 leaves the callback to the tick alone. */
    if (s_flip_handler_opd && g_ps3_guest_caller && !gcm_flipcb_on_tick_only())
        g_ps3_guest_caller(s_flip_handler_opd, 0, 0, 0, 0, 0, 0, 0, 0);  /* head 0 = primary display */

    return CELL_OK;
}

s32 cellGcmSetFlipCommand(u32 bufferId) { return gcm_flip_request(bufferId, 0); }

/* The real _cellGcmSetFlipCommand appends the flip to the command buffer, so
 * the RSX flips once it has executed everything queued before it. Firing it
 * at call time instead presented whatever the drain had reached by then --
 * and the title has not even flushed its frame's tail yet: The Simpsons Arcade
 * Game calls it with ctx->current ~2.4 KB past put, so frames showed without
 * their last draws (a dialog, a dimming overlay) for one flip. Write a marker
 * at ctx->current instead; the drain stops at it and fires the flip there,
 * exactly as it does for the 0xFEAD words a statically-linked libgcm emits.
 * Returns 0 when there is no room or ctx is not a mapped command buffer. */
static int gcm_flip_into_fifo(u32 ctx, u32 bufferId)
{
    if (!ctx || bufferId >= CELL_GCM_MAX_DISPLAY_BUFFER_NUM ||
        !s_display_buffer_set[bufferId]) return 0;
    const u32 end = vm_read32(ctx + 4), cur = vm_read32(ctx + 8);
    /* Never near the end: those bytes are the title's reserve before its
     * ring wraps, and the recycle above keys on them. */
    if (cur + 8 + GCM_RECYCLE_SLACK > end || gcm_ea2io(cur) == 0xFFFFFFFFu) return 0;
    vm_write32(cur, GCM_FLIP_MARKER | bufferId);
    /* Wrap here, on the guest thread, once the ring is nearly used -- what the
     * SDK's buffer-full callback does. Otherwise the drain thread recycles it
     * (see "recycled the title's own ring"), rewriting ctx->current and put
     * while the guest may be between advancing current and flushing: put
     * then lands on begin, the guest's closing fence is never executed, and
     * it spins on `ref` for good. Safe when the walker has left the head of
     * the ring well behind, which is the region the guest writes next.
     *
     * Only for a ring the runtime owns, i.e. one whose buffer-full callback is
     * still the runtime's (cellGcmSetupContext installs it). A title with its
     * own callback manages its own buffers, and its context need not be a ring
     * at all: Killzone 3 flips on a 0x30000-byte draw context whose `end`
     * moves down as it allocates from the top, so it always looks nearly full.
     * Rewinding it to `begin` in mid-frame cut the frame short, dropping the
     * fence release in its tail, and the title then waited for it forever. */
    const u32 begin = vm_read32(ctx), io_begin = gcm_ea2io(begin);
    const u32 cb_opd = vm_read32(ctx + 0xC);
    const int runtime_ring = cb_opd && vm_read32(cb_opd) == GCM_FIFO_CALLBACK_SENTINEL_EA;
    if (runtime_ring && end - (cur + 4) < GCM_FLIP_WRAP_BYTES && io_begin != 0xFFFFFFFFu &&
        s_fifo_getoff >= io_begin + GCM_RECYCLE_MARGIN) {
        vm_write32(cur + 4, 0x20000000u | io_begin);   /* JUMP -> begin */
        vm_write32(ctx + 8, begin);
        return 1;
    }
    vm_write32(ctx + 8, cur + 4);
    return 1;
}

/* cellGcmSetFlip(context, buffer_id) — immediate flip request. PSL1GHT's
 * gcmSetFlip import (NID 0xDC09357E) lands here; without it vkcube's flip was
 * a no-op, GetFlipStatus never cleared, and init timed out into exit(-1). */
s32 cellGcmSetFlip(void* context, u32 bufferId)
{
    (void)context;   /* command context — flip is immediate in HLE */
    return cellGcmSetFlipCommand(bufferId);
}

/* NID: 0xD01B570F */
s32 cellGcmSetFlipCommandWithWaitLabel(u32 bufferId, u32 labelIndex, u32 labelValue)
{
    if (labelIndex >= CELL_GCM_MAX_LABEL_COUNT)
        return CELL_GCM_ERROR_INVALID_VALUE;

    /* Wait until the label reaches the expected value (instant in HLE). Labels
     * live in guest memory (see cellGcmGetLabelAddress) so the game can poll. */
    vm_write32(GCM_LABEL_GUEST_BASE + labelIndex * GCM_LABEL_STRIDE, labelValue);

    return cellGcmSetFlipCommand(bufferId);
}

/* NID: 0xA2478CA3 */
s32 cellGcmSetPrepareFlip(void* ctx, u32 bufferId)
{
    (void)ctx;  /* command buffer context -- not used in HLE */

    if (bufferId >= CELL_GCM_MAX_DISPLAY_BUFFER_NUM)
        return CELL_GCM_ERROR_INVALID_VALUE;

    if (!s_display_buffer_set[bufferId])
        return CELL_GCM_ERROR_INVALID_VALUE;

    printf("[cellGcmSys] SetPrepareFlip(bufferId=%u)\n", bufferId);

    s_current_display_buffer_id = bufferId;
    s_flip_status = CELL_GCM_FLIP_STATUS_DONE;
    s_flip_request_count++;
    s_last_flip_time = get_timestamp_ns();

    /* Invoke the guest flip handler via OPD resolution -- s_flip_handler holds
     * the raw guest OPD (e.g. 0x530D70); calling it as a host function pointer
     * jumps into guest code and crashes. See cellGcmSetFlipCommand for why
     * GCM_FLIPCB_ONTICK exists. */
    if (s_flip_handler_opd && g_ps3_guest_caller && !gcm_flipcb_on_tick_only())
        g_ps3_guest_caller(s_flip_handler_opd, 0, 0, 0, 0, 0, 0, 0, 0);

    return CELL_OK;
}

/* NID: 0x1BFAB6EE */
CellGcmDisplayInfo* cellGcmGetDisplayBufferByFlipIndex(u32 index)
{
    u32 id = index % CELL_GCM_MAX_DISPLAY_BUFFER_NUM;
    return &s_display_buffers[id];
}

/* NID: 0x8BADE8BE */
u32 cellGcmGetCurrentDisplayBufferId(void)
{
    return s_current_display_buffer_id;
}

/* NID: 0xD9B7653E */
void cellGcmSetFlipHandler(CellGcmFlipHandler handler)
{
    /* `handler` is a guest OPD address — store as such and remember
     * the legacy host-typed value too for any callers still using
     * the old API style. Real dispatch goes through
     * cellGcmDispatchVBlank()/DispatchFlip() via g_ps3_guest_caller. */
    printf("[cellGcmSys] SetFlipHandler(opd=0x%08X)\n", (unsigned)(size_t)handler);
    s_flip_handler_opd = (u32)(size_t)handler;
    s_flip_handler = handler;
    { u32 c = s_flip_handler_opd ? vm_read32(s_flip_handler_opd) : 0; if (c) s_flip_handler_code = c; }
    gcm_update_handler_mask();
}

/* NID: 0xA547ADDE */
void cellGcmSetVBlankHandler(CellGcmVBlankHandler handler)
{
    printf("[cellGcmSys] SetVBlankHandler(opd=0x%08X)\n", (unsigned)(size_t)handler);
    s_vblank_handler_opd = (u32)(size_t)handler;
    s_vblank_handler = handler;
    { u32 c = s_vblank_handler_opd ? vm_read32(s_vblank_handler_opd) : 0; if (c) s_vblank_handler_code = c; }
    gcm_update_handler_mask();
}

/* NID: 0xF9BFCDA3 */
void cellGcmSetSecondVHandler(CellGcmSecondVHandler handler)
{
    printf("[cellGcmSys] SetSecondVHandler(opd=0x%08X)\n", (unsigned)(size_t)handler);
    s_second_v_handler_opd = (u32)(size_t)handler;
    s_second_v_handler = handler;
}

/* NID: 0x0B4B62D5 */
void cellGcmSetUserHandler(CellGcmUserHandler handler)
{
    printf("[cellGcmSys] SetUserHandler(opd=0x%08X)\n", (unsigned)(size_t)handler);
    s_user_handler_opd = (u32)(size_t)handler;
    s_user_handler = handler;
    gcm_update_handler_mask();
}

/* NID: 0x21AC3697 */
u64 cellGcmGetLastFlipTime(void)
{
    return s_last_flip_time;
}

/* ---------------------------------------------------------------------------
 * Address translation / IO mapping
 * -----------------------------------------------------------------------*/

/* NID: 0x0E6B0DFF */
/* Mirror the host tables into the guest window the title will index. */
static void gcm_publish_offset_tables(void)
{
    for (u32 i = 0; i < GCM_OFFSET_TABLE_PAGES; i++) {
        vm_write16(GCM_OFFSET_TABLE_IO_ADDR + i * 2, s_io_address_table[i]);
        vm_write16(GCM_OFFSET_TABLE_EA_ADDR + i * 2, s_ea_address_table[i]);
    }
}

s32 cellGcmGetOffsetTable(CellGcmOffsetTable* table)
{
    u32 ea = (u32)(uintptr_t)table;
    if (!ea)
        return CELL_GCM_ERROR_INVALID_VALUE;

    gcm_publish_offset_tables();

    /* The guest struct is two 4-byte EAs, not two host pointers. */
    vm_write32(ea + 0, GCM_OFFSET_TABLE_IO_ADDR);
    vm_write32(ea + 4, GCM_OFFSET_TABLE_EA_ADDR);
    return CELL_OK;
}

/* NID: 0xDB769B32 */
static u32 gcm_io_alloc(u32 size);   /* defined below */

s32 cellGcmAddressToOffset(u32 address, u32* offset)
{
    /* `offset` is a GUEST address; write big-endian via vm_write32. */
    uint32_t off_ea = (uint32_t)(uintptr_t)offset;
    if (!off_ea)
        return CELL_GCM_ERROR_INVALID_VALUE;
    /* A2O_DBG=<N>: the EA->offset conversions a title asks for. A texture bound
     * at an offset nothing ever writes usually means the offset came from an EA
     * in a space we classified wrongly, and that is only visible here. */
    { static int cap = -1, k = 0;
      if (cap < 0) { const char* e = getenv("A2O_DBG"); cap = e ? atoi(e) : 0; }
      if (cap && k < cap) { k++;
        u32 nz = 0;
        for (u32 i = 0; i < 0x400u; i += 13) if (vm_base && vm_base[address + i]) nz++;
        printf("[A2O] ea=0x%08X  (first 1KB nonzero=%u/79)\n", address, nz); } }

    /* Local memory: offset = address - localBase. Record the offset page as
     * LOCAL-derived so cellGcmResolveOffset can disambiguate it from an IO
     * (main-memory) offset with the same page number -- the two offset spaces
     * overlap, and a title with a large IO region (gcm/cube: 16MB) otherwise
     * gets its VRAM vertex/texture/FP reads resolved into the empty command
     * buffer. */
    if (address >= s_config.localAddress &&
        address < s_config.localAddress + s_config.localSize) {
        u32 loff = address - s_config.localAddress;
        if ((loff >> 20) < sizeof(s_local_offset_page)) {
            if (!s_local_offset_page[loff >> 20]) { s_local_offset_page[loff >> 20] = 1; gcm_xlate_moved(); }
        }
        else rsx_lim_event(RSX_LIM_GCM_TABLES, 0, "local-memory page past 1 GB not recorded");
        vm_write32(off_ea, loff);
        return CELL_OK;
    }

    /* IO-mapped main memory: consult offset table */
    u32 page = address >> 20;
    if (page < 65536 && s_io_address_table[page] != 0xFFFF) {
        u32 io_page = s_io_address_table[page];
        vm_write32(off_ea, (io_page << 20) | (address & 0xFFFFF));
        return CELL_OK;
    }

    /* Legacy fallback for initial IO region */
    if (s_config.ioAddress != 0 && address >= s_config.ioAddress &&
        address < s_config.ioAddress + s_config.ioSize) {
        vm_write32(off_ea, address - s_config.ioAddress);
        return CELL_OK;
    }

    /* Auto-map unmapped main memory. On real hardware PSL1GHT pre-maps its
     * whole RSX heap in gcmInitBody, so its libraries never call MapMainMemory
     * before handing an EA to the RSX (Tiny3D's command ring lives in an
     * sys_mmapper region). Mirror that by mapping the 1MB page on first use.
     *
     * The bound used to be a flat `< 0x40000000`, which is not a description of
     * anything -- RSX local memory is already handled at the top of this
     * function against its real base, and the raw-SPU window at 0x30000000 sits
     * *below* the old bound, so it was not keeping that out either. What it did
     * do was exclude titles whose RSX-visible heap lives high: Virtua Fighter 5
     * maps 0x4A900000..0x4B400000 and then hands us 0x4B400040, which fell past
     * 0x40000000, skipped the auto-map, and failed -- 976 times a boot, leaving
     * the title with no offsets to draw with (20 draws in five minutes).
     *
     * So the test is now what it always meant: anything below RSX local memory
     * is main memory. Local is checked first and returns above, so reaching
     * here with an address under localAddress means an unmapped main-memory
     * page, which is exactly the case this fallback exists for. */
    /* NULL is not a buffer. Auto-mapping it costs an IO page and, worse, hands
     * the title a real-looking offset for guest address 0 -- so an EA the title
     * computed as NULL (a failed allocation, an out-param it never filled)
     * silently becomes a valid RSX offset instead of an error it could notice.
     * The Orange Box's chooser asks for exactly this, and the page it burned was
     * the one the following cellGcmMapMainMemory would otherwise have taken. */
    if (address < 0x100000u) {
        printf("[cellGcmSys] AddressToOffset REFUSED: ea 0x%08X is not a mappable "
               "address (page 0)\n", address);
        vm_write32(off_ea, 0);
        return CELL_GCM_ERROR_INVALID_VALUE;
    }

    u32 auto_map_limit = s_config.localAddress ? s_config.localAddress : 0x40000000u;
    if (address < auto_map_limit) {
        u32 ea_page = address & ~0xFFFFFu;
        u32 io      = gcm_io_alloc(0x100000u);
        populate_offset_table(ea_page, io, 0x100000u);
        /* Record it in the mapping list too, not just the offset table.
         *
         * These are two different structures answering two different questions,
         * and only the offset table was being filled in. The FIFO walker
         * resolves io -> ea through the MAPPING LIST (find_io_mapping_by_io),
         * so an auto-mapped range was invisible to it: a JUMP into one reported
         * "no IO mapping", the walker resynced straight to put, and every fence
         * in the skipped range was lost -- which strands a title spinning on
         * ctrl->ref with no way to see why. Both paths have to agree about what
         * is mapped. */
        IoMapping* m = find_free_mapping();
        if (m) {
            m->ea = ea_page; m->io = io; m->size = 0x100000u; m->active = 1;
            s_io_mapping_count++; gcm_spu_put_box_update();   /* (perf17) */
        } else {
            printf("[cellGcmSys] WARNING: auto-map has no free IO mapping slot; "
                   "the FIFO will not be able to walk io 0x%08X\n", io);
            rsx_lim_event(RSX_LIM_GCM_IOMAP, 0, "IO mapping table full: an auto-mapped range is not walkable");
        }
        printf("[cellGcmSys] AddressToOffset: auto-mapped ea 0x%08X -> io 0x%08X\n",
               ea_page, io);
        vm_write32(off_ea, io | (address & 0xFFFFFu));
        return CELL_OK;
    }

    printf("[cellGcmSys] WARNING: AddressToOffset failed for 0x%08X\n", address);
    vm_write32(off_ea, 0);
    { static int _d = -1; if (_d < 0) _d = getenv("A2O_FAILDBG") ? 1 : 0;
      if (_d) { static int _n = 0; if (PS3_LOG_FIRST(_n, 12))
          fprintf(stderr, "[A2O] FAILED ea=0x%08X (not in local or IO space)%c",
                  address, 10); } }
    return CELL_GCM_ERROR_FAILURE;
}

/* IO-offset space bump allocator for main-memory mappings. Starts PAST the
 * Init command-buffer region: handing out io 0 (the old s_local_mem_allocated
 * bump, which begins at 0) remapped the FIFO's own io page -- demosaic's
 * cellGcmMapMainMemory(sys heap) got offset 0, io2ea(0) then resolved into its
 * heap (all zeros) and the RSX walker consumed every frame's commands as
 * no-ops (no draws, label fence never written, guest waited forever). */
static u32 s_io_alloc_next = 0;
static u32 gcm_io_alloc(u32 size)
{
    if (s_io_alloc_next == 0)
        s_io_alloc_next = (s_config.ioSize + 0xFFFFFu) & ~0xFFFFFu;
    if (s_io_alloc_next < 0x100000u) s_io_alloc_next = 0x100000u;
    u32 io = s_io_alloc_next;
    s_io_alloc_next += (size + 0xFFFFFu) & ~0xFFFFFu;
    return io;
}

/* NID: 0x2A6FBA9C */
s32 cellGcmMapMainMemory(u32 ea, u32 size, u32* offset)
{
    /* Log the REQUEST before validating it. Every rejection below used to
     * happen before the only printf in this function, so a title whose
     * mapping we refused looked -- in the log -- like a title that never asked.
     * Virtua Fighter 5 then fails cellGcmAddressToOffset ~976 times a boot for
     * addresses immediately past the region it did get, draws 20 times instead
     * of thousands, and never flips; the refusal that would explain it was
     * invisible. Same lesson as the savedata early returns. */
    printf("[cellGcmSys] MapMainMemory(ea=0x%08X, size=0x%X)\n", ea, size);

    if (!offset) {
        printf("[cellGcmSys] MapMainMemory REFUSED: offset out-param is NULL\n");
        return CELL_GCM_ERROR_INVALID_VALUE;
    }
    /* Size and EA must both be 1MB aligned. */
    if (size == 0 || (size & 0xFFFFF) != 0) {
        printf("[cellGcmSys] MapMainMemory REFUSED: size 0x%X is not a non-zero multiple of 1MB\n", size);
        return CELL_GCM_ERROR_INVALID_ALIGNMENT;
    }
    if ((ea & 0xFFFFF) != 0) {
        printf("[cellGcmSys] MapMainMemory REFUSED: ea 0x%08X is not 1MB aligned\n", ea);
        return CELL_GCM_ERROR_INVALID_ALIGNMENT;
    }

    /* Allocate a fresh IO region past the Init command-buffer window. */
    u32 io_offset = gcm_io_alloc(size);

    IoMapping* mapping = find_free_mapping();
    if (!mapping) {
        printf("[cellGcmSys] WARNING: no free IO mapping slots\n");
        rsx_lim_event(RSX_LIM_GCM_IOMAP, 0, "IO mapping table full: MapMainMemory refused");
        return CELL_GCM_ERROR_FAILURE;
    }

    mapping->ea     = ea;
    mapping->io     = io_offset;
    mapping->size   = size;
    mapping->active = 1;
    s_io_mapping_count++; gcm_spu_put_box_update();   /* (perf17) */

    populate_offset_table(ea, io_offset, size);

    vm_write32((uint32_t)(uintptr_t)offset, io_offset);   /* guest out-param */
    return CELL_OK;
}

/* NID: 0x5A41C10F */
s32 cellGcmMapEaIoAddress(u32 ea, u32 io, u32 size)
{
    /* Logged before validation, and every failure path named -- same reason as
     * cellGcmMapMainMemory above. Two of these returned silently AFTER the log
     * line as well, so even a call that got that far could vanish. */
    printf("[cellGcmSys] MapEaIoAddress(ea=0x%08X, io=0x%08X, size=0x%X)\n", ea, io, size);

    if ((ea & 0xFFFFF) != 0 || (io & 0xFFFFF) != 0) {
        printf("[cellGcmSys] MapEaIoAddress REFUSED: ea 0x%08X / io 0x%08X not 1MB aligned\n", ea, io);
        return CELL_GCM_ERROR_INVALID_ALIGNMENT;
    }
    if (size == 0 || (size & 0xFFFFF) != 0) {
        printf("[cellGcmSys] MapEaIoAddress REFUSED: size 0x%X is not a non-zero multiple of 1MB\n", size);
        return CELL_GCM_ERROR_INVALID_ALIGNMENT;
    }
    if (find_mapping_by_ea(ea) != NULL) {
        printf("[cellGcmSys] MapEaIoAddress REFUSED: ea 0x%08X is already mapped\n", ea);
        return CELL_GCM_ERROR_ADDRESS_OVERWRAP;
    }

    IoMapping* mapping = find_free_mapping();
    if (!mapping) {
        printf("[cellGcmSys] MapEaIoAddress REFUSED: no free IO mapping slots\n");
        rsx_lim_event(RSX_LIM_GCM_IOMAP, 0, "IO mapping table full: MapEaIoAddress refused");
        return CELL_GCM_ERROR_FAILURE;
    }

    mapping->ea     = ea;
    mapping->io     = io;
    mapping->size   = size;
    mapping->active = 1;
    s_io_mapping_count++; gcm_spu_put_box_update();   /* (perf17) */

    populate_offset_table(ea, io, size);

    return CELL_OK;
}

/* NID: 0xDB23E867 (disambiguation by context) */
s32 cellGcmUnmapEaIoAddress(u32 ea)
{
    IoMapping* mapping = find_mapping_by_ea(ea);
    if (!mapping)
        return CELL_GCM_ERROR_FAILURE;

    printf("[cellGcmSys] UnmapEaIoAddress(ea=0x%08X)\n", ea);

    clear_offset_table(mapping->ea, mapping->io, mapping->size);
    mapping->active = 0;
    s_io_mapping_count--; gcm_spu_put_box_update();   /* (perf17) */

    return CELL_OK;
}

/* NID: 0x3B9BD5BD */
s32 cellGcmUnmapIoAddress(u32 io)
{
    IoMapping* mapping = find_mapping_by_io(io);
    if (!mapping)
        return CELL_GCM_ERROR_FAILURE;

    printf("[cellGcmSys] UnmapIoAddress(io=0x%08X)\n", io);

    clear_offset_table(mapping->ea, mapping->io, mapping->size);
    mapping->active = 0;
    s_io_mapping_count--; gcm_spu_put_box_update();   /* (perf17) */

    return CELL_OK;
}

/* NID: 0xC47D0812 */
s32 cellGcmIoOffsetToAddress(u32 ioOffset, u32* ea)
{
    uint32_t ea_ea = (uint32_t)(uintptr_t)ea;   /* guest out-param */
    if (!ea_ea)
        return CELL_GCM_ERROR_INVALID_VALUE;

    u32 page = ioOffset >> 20;
    if (page < 65536 && s_ea_address_table[page] != 0xFFFF) {
        u32 ea_page = s_ea_address_table[page];
        vm_write32(ea_ea, (ea_page << 20) | (ioOffset & 0xFFFFF));
        return CELL_OK;
    }

    printf("[cellGcmSys] WARNING: IoOffsetToAddress failed for 0x%08X\n", ioOffset);
    vm_write32(ea_ea, 0);
    return CELL_GCM_ERROR_FAILURE;
}

/* Resolve an RSX FIFO offset (from a vertex-array / texture / surface method)
 * to a guest effective address the backend can read via vm_base + addr.
 *
 * An RSX offset refers to either IO-mapped *main* memory or *local* video
 * memory, distinguished on hardware by a per-object location bit that the
 * command stream no longer carries by the time the backend sees it. Resolve by
 * table: if the offset's 1MB page is IO-mapped, it's main memory; otherwise it
 * is local video memory (localAddress + offset). IO mappings are sparse (the
 * command/IO window is a few MB) while local objects sit high in the local
 * heap, so this disambiguates every real case — only offsets inside the small
 * IO window are treated as main. */
u32 cellGcmResolveOffset(u32 offset)
{
    u32 page = offset >> 20;
    /* A page the guest derived from a LOCAL EA resolves to VRAM even when the
     * IO table also covers that page number (overlapping offset spaces). */
    if (page < sizeof(s_local_offset_page) && s_local_offset_page[page])
        return s_config.localAddress + offset;
    if (page < 65536) {
        u16 ea_page = s_ea_address_table[page];
        if (ea_page != 0 && ea_page != 0xFFFF)
            return ((u32)ea_page << 20) | (offset & 0xFFFFF);
    }
    return s_config.localAddress + offset;
}

/* IO-table-first resolve: skip the local-page shortcut and ask the IO offset
 * table, falling back to VRAM only if the page is unmapped. cellGcmResolveOffset
 * prefers local for any page the guest ever derived from a local EA, which is
 * right for a title that really does keep the data in VRAM but wrong for one
 * that builds its textures in main memory on pages whose numbers happen to
 * collide. Returns 0 when the IO table has no mapping, so a caller can tell
 * "not IO-mapped" from "IO-mapped at 0". */
u32 cellGcmResolveIO(u32 offset)
{
    u32 page = offset >> 20;
    if (page < 65536) {
        u16 ea_page = s_ea_address_table[page];
        if (ea_page != 0 && ea_page != 0xFFFF)
            return ((u32)ea_page << 20) | (offset & 0xFFFFF);
    }
    return 0;
}

/* Location-aware resolve. RSX offsets are TWO overlapping number spaces --
 * LOCAL (VRAM, ea = localAddress + offset) and MAIN (IO-mapped, via the
 * offset table). cellGcmResolveOffset guesses table-first, which breaks when
 * a title's IO region is large enough to cover the same page numbers as its
 * local allocations (gcm/cube: ioSize 16MB, FP ucode at local 0xB90000 --
 * table hit returned the empty command-buffer EA instead of VRAM). Callers
 * that KNOW the location (texture format bits, SET_SHADER_PROGRAM bits) must
 * use this. `local` != 0 -> local memory. */
u32 cellGcmResolveLocated(int local, u32 offset)
{
    if (local)
        return s_config.localAddress + offset;
    return cellGcmResolveOffset(offset);
}

/* ---------------------------------------------------------------------------
 * Label / report / timestamp
 * -----------------------------------------------------------------------*/

/* NID: 0xF80196C1 */
u32* cellGcmGetLabelAddress(u8 index)
{
    if (index >= CELL_GCM_MAX_LABEL_COUNT) {
        printf("[cellGcmSys] WARNING: GetLabelAddress index %u out of range\n", index);
        return NULL;
    }
    return (u32*)(uintptr_t)(GCM_LABEL_GUEST_BASE + (u32)index * GCM_LABEL_STRIDE);
}

/* NID: 0x8572ADE4 */
CellGcmReportData* cellGcmGetReportDataAddress(u32 index)
{
    if (index >= CELL_GCM_MAX_REPORT_COUNT) {
        printf("[cellGcmSys] WARNING: GetReportDataAddress index %u out of range\n", index);
        return NULL;
    }
    return &s_report_data[index];
}

/* NID: 0x97FC4B73 */
/* NID: 0x99D397AC. The value NV4097_GET_REPORT stored at this index (the
 * walker fills it). Unregistered, every call returned a faked 0. */
u32 cellGcmGetReport(u32 type, u32 index)
{
    { static unsigned n = 0; /* unsigned: a signed counter wraps negative and then logs every call (as sp00nznet/ps3recomp#364) */
      if (__atomic_fetch_add(&n, 1, __ATOMIC_RELAXED) < 8 || (n % 2000) == 0) printf("[cellGcmSys] GetReport(type=%u, index=%u) -> %u%c", type, index,
                          index < CELL_GCM_MAX_REPORT_COUNT ? s_report_data[index].value : 0u, 10); }
    if (index >= CELL_GCM_MAX_REPORT_COUNT) return 0;
    return s_report_data[index].value;
}

u64 cellGcmGetTimeStamp(u32 index)
{
    if (index >= CELL_GCM_MAX_REPORT_COUNT)
        return 0;

    /*
     * On real hardware this reads the RSX timestamp for a given report index.
     * We return the host timestamp in nanoseconds as a reasonable approximation.
     */
    s_report_data[index].timestamp = get_timestamp_ns();
    return s_report_data[index].timestamp;
}

/* ---------------------------------------------------------------------------
 * Tile / Zcull configuration
 * -----------------------------------------------------------------------*/

/* NID: 0x0B4B62D5 */
s32 cellGcmSetTile(u8 index, u8 location, u32 offset, u32 size,
                   u32 pitch, u8 comp, u16 base, u8 bank)
{
    if (index >= CELL_GCM_MAX_TILE_COUNT)
        return CELL_GCM_ERROR_INVALID_VALUE;

    printf("[cellGcmSys] SetTile(index=%u, loc=%u, offset=0x%X, size=0x%X, pitch=%u)\n",
           index, location, offset, size, pitch);

    s_tiles[index].offset = offset;
    s_tiles[index].size   = size;
    s_tiles[index].pitch  = pitch;
    s_tiles[index].format = comp;
    s_tiles[index].base   = base;
    s_tiles[index].bank   = bank;
    s_tiles[index].bound  = 0;

    /* Build tile register value (simplified) */
    s_tiles[index].tile  = (location << 0) | (pitch << 8);
    s_tiles[index].limit = offset + size - 1;

    return CELL_OK;
}

/* NID: 0x06EDEA25 */
s32 cellGcmSetZcull(u8 index, u32 offset, u32 width, u32 height,
                    u32 cullStart, u32 zFormat, u32 aaFormat,
                    u32 zcullDir, u32 zcullFormat,
                    u32 sFunc, u32 sRef, u32 sMask)
{
    if (index >= CELL_GCM_MAX_ZCULL_COUNT)
        return CELL_GCM_ERROR_INVALID_VALUE;

    printf("[cellGcmSys] SetZcull(index=%u, offset=0x%X, %ux%u)\n",
           index, offset, width, height);

    s_zcull[index].offset      = offset;
    s_zcull[index].width       = width;
    s_zcull[index].height      = height;
    s_zcull[index].cullStart   = cullStart;
    s_zcull[index].zFormat     = zFormat;
    s_zcull[index].aaFormat    = aaFormat;
    s_zcull[index].zcullDir    = zcullDir;
    s_zcull[index].zcullFormat = zcullFormat;
    s_zcull[index].sFunc       = sFunc;
    s_zcull[index].sRef        = sRef;
    s_zcull[index].sMask       = sMask;
    s_zcull[index].bound       = 0;

    return CELL_OK;
}

/* NID: 0x2BB1F1E5 */
s32 cellGcmBindTile(u8 index)
{
    if (index >= CELL_GCM_MAX_TILE_COUNT)
        return CELL_GCM_ERROR_INVALID_VALUE;

    printf("[cellGcmSys] BindTile(index=%u)\n", index);
    s_tiles[index].bound = 1;
    return CELL_OK;
}

/* NID: 0x1F61F9D3 */
s32 cellGcmBindZcull(u8 index)
{
    if (index >= CELL_GCM_MAX_ZCULL_COUNT)
        return CELL_GCM_ERROR_INVALID_VALUE;

    printf("[cellGcmSys] BindZcull(index=%u)\n", index);
    s_zcull[index].bound = 1;
    return CELL_OK;
}

/* NID: 0x75843042 */
s32 cellGcmUnbindTile(u8 index)
{
    if (index >= CELL_GCM_MAX_TILE_COUNT)
        return CELL_GCM_ERROR_INVALID_VALUE;

    printf("[cellGcmSys] UnbindTile(index=%u)\n", index);
    s_tiles[index].bound = 0;
    return CELL_OK;
}

/* NID: 0xA093BE1C */
s32 cellGcmUnbindZcull(u8 index)
{
    if (index >= CELL_GCM_MAX_ZCULL_COUNT)
        return CELL_GCM_ERROR_INVALID_VALUE;

    printf("[cellGcmSys] UnbindZcull(index=%u)\n", index);
    s_zcull[index].bound = 0;
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * Misc
 * -----------------------------------------------------------------------*/

/* NID: 0x107BF789
 * SDK prototype (cell/gcm.h): uint32_t cellGcmGetTiledPitchSize(const uint32_t size)
 * -- returns the pitch BY VALUE in r3 (no out-pointer!). The old signature here
 * took (size, u32* pitch-out) and returned a status code, so every caller got 0
 * / an error code as its "pitch" -- gcm/cube's `bne` on the result then skipped
 * its entire config+framebuffer-alloc block (GetConfiguration never ran, local
 * memory base stayed 0, AddressToOffset(0) auto-mapped a bogus IO alias, and the
 * FIFO teleported into it). Returns 0 only for size > max, like hardware. */
u32 cellGcmGetTiledPitchSize(u32 size)
{
    u32 r = 0;
    /* Smallest valid tiled pitch >= size (RSX supports only specific pitches). */
    if (size) {
        for (int i = 0; i < s_valid_pitch_count; i++) {
            if (s_valid_pitches[i] >= size) { r = s_valid_pitches[i]; break; }
        }
    }
    { static int _n=0; if (PS3_LOG_FIRST(_n, 4)) fprintf(stderr, "[cellGcmSys] GetTiledPitchSize(0x%X) -> 0x%X\n", size, r); }
    return r;
}

/* NID: 0xBC982946 */
void cellGcmSetDebugOutputLevel(u32 level)
{
    printf("[cellGcmSys] SetDebugOutputLevel(level=%u)\n", level);
    s_debug_level = level;
}

/* ---------------------------------------------------------------------------
 * Additional functions needed by Tokyo Jungle
 * -----------------------------------------------------------------------*/

/* Notify data area — similar to report data, 16 bytes per slot */
static u8 s_notify_data[256 * 16];

void* cellGcmGetNotifyDataAddress(u32 index)
{
    if (index >= 256) return NULL;
    return &s_notify_data[index * 16];
}

/* Timestamp location — returns CELL_GCM_LOCATION_LOCAL or MAIN */
/* The Location suffix names an ARGUMENT, not an out-parameter: it is the
 * caller saying which memory the report lives in, CELL_GCM_LOCATION_LOCAL or
 * _MAIN, exactly as cellGcmGetReportDataLocation and
 * cellGcmGetReportDataAddressLocation take it three hundred lines below. This
 * one alone was declared u32* and treated the value as a guest address to
 * write the location INTO, which inverts the parameter and loses the return.
 *
 * A title passing CELL_GCM_LOCATION_MAIN therefore had a 1 taken for an
 * effective address, and vm_write32 stored four bytes at guest address 1.
 * Yakuza: Dead Souls does that during display setup and dies there.
 *
 * It returns the timestamp, like cellGcmGetTimeStamp, whose body this is with
 * a location the report table does not distinguish. */
u64 cellGcmGetTimeStampLocation(u32 index, u32 location)
{
    (void)location;

    if (index >= CELL_GCM_MAX_REPORT_COUNT)
        return 0;

    s_report_data[index].timestamp = get_timestamp_ns();
    return s_report_data[index].timestamp;
}

/* SetTileInfo — alternative to SetTile with same parameters */
s32 cellGcmSetTileInfo(u8 index, u8 location, u32 offset, u32 size,
                       u32 pitch, u8 comp, u16 base, u8 bank)
{
    /* Delegates to existing SetTile */
    return cellGcmSetTile(index, location, offset, size, pitch, comp, base, bank);
}

/* Default FIFO size — configures command buffer size before init */
static u32 s_default_fifo_size = 0x40000; /* 256KB default */

s32 cellGcmSetDefaultFifoSize(u32 size)
{
    printf("[cellGcmSys] SetDefaultFifoSize(0x%X)\n", size);
    s_default_fifo_size = size;
    return CELL_OK;
}

/* Internal flip commands — called directly by some games */
/* libgcm's internal exports pass the command context FIRST -- taking
 * bufferId as arg1 made the range check eat the context pointer, so the
 * flip counter never advanced (wave: presents free-ran 4x per frame,
 * layout flashing/zooming). */
/* NID 0x21397818 is gcmSetFlipCommand (PSL1GHT libgcm_sys exports.h) -- it
 * was misassigned to cellGcmGetLabelAddress (real NID 0xF80196C1), so every
 * import-based flip (LBP: cellGcmSetFlip -> stub 0x7482FC(ctx, id)) landed
 * in the label getter and no flip was EVER issued: the intro-movie pump ran,
 * drew, "flipped" into the wrong function, and the screen stayed black. */
/* NID: 0x21397818 */
s32 _cellGcmSetFlipCommand(void* ctx, u32 bufferId)
{
    if (gcm_flip_into_fifo((u32)(uintptr_t)ctx, bufferId))
        return gcm_flip_request(bufferId, 1);
    return cellGcmSetFlipCommand(bufferId);
}

/* NID: 0xD8F88E1A */
s32 _cellGcmSetFlipCommandWithWaitLabel(void* ctx, u32 bufferId,
                                        u32 labelIndex, u32 labelValue)
{
    (void)ctx;
    return cellGcmSetFlipCommandWithWaitLabel(bufferId, labelIndex, labelValue);
}

/* ---------------------------------------------------------------------------
 * Additional functions (RPCS3 parity)
 * -----------------------------------------------------------------------*/

/* FIFO command buffer callback — called when put pointer wraps.
 * In recomp we don't have a real GPU consuming FIFO, so this is a no-op. */
s32 cellGcmCallback(void* context, u32 count)
{
    (void)context;
    (void)count;
    return CELL_OK;
}

/* Map RSX local memory — returns the base address and size of local VRAM */
s32 cellGcmMapLocalMemory(u32* address, u32* size)
{
    if (!address || !size)
        return CELL_GCM_ERROR_INVALID_VALUE;

    /* Guest out-params, like every other pointer parameter in this file. */
    vm_write32((uint32_t)(uintptr_t)address, s_config.localAddress);
    vm_write32((uint32_t)(uintptr_t)size,    s_config.localSize);

    printf("[cellGcmSys] MapLocalMemory(address=0x%08X, size=0x%X)\n",
           s_config.localAddress, s_config.localSize);
    return CELL_OK;
}

/* Shutdown RSX — clear all state */
void cellGcmTerminate(void)
{
    printf("[cellGcmSys] Terminate\n");

    s_gcm_initialized = 0;
    s_flip_mode   = CELL_GCM_DISPLAY_VSYNC;
    s_flip_status = CELL_GCM_FLIP_STATUS_DONE;
    s_debug_level = CELL_GCM_DEBUG_LEVEL0;
    s_vblank_count = 0;
    s_io_map_reserved = 0;
    s_default_fifo_mode = 0;
    GCM_USER_STORE(0);
    GCM_USER_PENDING_STORE(0);

    memset(s_display_buffers, 0, sizeof(s_display_buffers));
    memset(s_display_buffer_set, 0, sizeof(s_display_buffer_set));
    memset(&s_config, 0, sizeof(s_config));
    memset(&s_control, 0, sizeof(s_control));
    memset(s_io_mappings, 0, sizeof(s_io_mappings));
    memset(s_tiles, 0, sizeof(s_tiles));
    memset(s_zcull, 0, sizeof(s_zcull));
    memset(s_report_data, 0, sizeof(s_report_data));
    memset(s_labels, 0, sizeof(s_labels));
    memset(s_io_address_table, 0xFF, sizeof(s_io_address_table));
    memset(s_ea_address_table, 0xFF, sizeof(s_ea_address_table));
    gcm_xlate_moved();

    s_flip_handler     = NULL;
    s_vblank_handler   = NULL;
    s_user_handler     = NULL;
    s_second_v_handler = NULL;
    s_graphics_handler = NULL;
    s_queue_handler    = NULL;
    s_local_mem_allocated = 0;
    s_io_mapping_count = 0;
    gcm_spu_put_box_update();   /* (perf17) no mappings (yet) */
    /* Unmapped = 0xFFFF. The tables are static (zero-initialized), so without
     * this every unmapped EA page silently aliased io page 0 (and vice versa),
     * producing plausible-but-wrong offsets instead of a translation failure. */
    memset(s_io_address_table, 0xFF, sizeof(s_io_address_table));
    memset(s_ea_address_table, 0xFF, sizeof(s_ea_address_table));
    gcm_xlate_moved();
    s_current_display_buffer_id = 0;
    s_last_flip_time = 0;
}

/* Return remaining IO map space (256MB total IO space minus already mapped) */
u32 cellGcmGetMaxIoMapSize(void)
{
    /* RSX has a 256MB IO address window */
    u32 total_io = 256 * 1024 * 1024;
    u32 used = 0;

    for (int i = 0; i < CELL_GCM_MAX_IO_MAPPINGS; i++) {
        if (s_io_mappings[i].active)
            used += s_io_mappings[i].size;
    }

    return (used < total_io) ? (total_io - used) : 0;
}

/* Reserve IO map space (pre-allocation for future mappings) */
s32 cellGcmReserveIoMapSize(u32 size)
{
    u32 max_size = cellGcmGetMaxIoMapSize();
    if (size > max_size - s_io_map_reserved)
        return CELL_GCM_ERROR_FAILURE;

    s_io_map_reserved += size;
    return CELL_OK;
}

/* Unreserve IO map space */
s32 cellGcmUnreserveIoMapSize(u32 size)
{
    if (size > s_io_map_reserved)
        return CELL_GCM_ERROR_FAILURE;

    s_io_map_reserved -= size;
    return CELL_OK;
}

/* Return incrementing VBlank counter */
u32 cellGcmGetVBlankCount(void)
{
    /* Approximate: increment each time queried (games use this for timing) */
    return s_vblank_count++;
}

/* Return tile info for a given index */
CellGcmTileInfo* cellGcmGetTileInfo(u8 index)
{
    if (index >= CELL_GCM_MAX_TILE_COUNT) {
        printf("[cellGcmSys] WARNING: GetTileInfo index %u out of range\n", index);
        return NULL;
    }
    return &s_tiles[index];
}

/* Return zcull info for a given index */
CellGcmZcullInfo* cellGcmGetZcullInfo(u8 index)
{
    if (index >= CELL_GCM_MAX_ZCULL_COUNT) {
        printf("[cellGcmSys] WARNING: GetZcullInfo index %u out of range\n", index);
        return NULL;
    }
    return &s_zcull[index];
}

/* Return display buffer info by index */
CellGcmDisplayInfo* cellGcmGetDisplayInfo(u32 index)
{
    if (index >= CELL_GCM_MAX_DISPLAY_BUFFER_NUM) {
        printf("[cellGcmSys] WARNING: GetDisplayInfo index %u out of range\n", index);
        return NULL;
    }
    return &s_display_buffers[index];
}

/* Set default FIFO mode (before init) */
void cellGcmInitDefaultFifoMode(s32 mode)
{
    printf("[cellGcmSys] InitDefaultFifoMode(mode=%d)\n", mode);
    s_default_fifo_mode = mode;
}

/* Set command buffer to defaults (reset put/get pointers) */
void cellGcmSetDefaultCommandBuffer(void)
{
    printf("[cellGcmSys] SetDefaultCommandBuffer\n");
    s_control.put = 0;
    s_control.get = 0;
    s_control.ref = 0;

    /* On hardware this re-points gCellGcmCurrentContext at the default context
     * cellGcmInit built. Zeroing a host-side struct is not that: a title that
     * calls it to go back to the big default buffer stays on whatever smaller
     * segment it had switched to.
     *
     * GCM_DEFAULT_CTX_REPOINT=1 does the real thing -- write our context's EA
     * into the guest variable cellGcmInit's ctx_out pointed at, and rewind that
     * context. Off by default because a title managing its own segment chain
     * may hold pointers into the old one; this exists to be measured, not
     * assumed.
     *
     * Measured once now, on Virtua Fighter 5, which is the case this was
     * written for. VF5's AMGL keeps its own command buffers (it repoints
     * gCellGcmCurrentContext at a 512 KB segment of its own inside the 4 MB it
     * maps) and calls this entry to recover when one fills. Getting a host-side
     * struct zeroed instead leaves it on the exhausted segment, so it allocates
     * another, fills that, and asks again:
     *
     *     [AMGL]:[ERROR] Command Buffer Overflow!   x91, 636 calls to here
     *
     * With the repoint: **0 overflows**, and the command packets reaching the
     * draw engine go from 12,756 to 15,097 over the same five minutes. Still
     * one title's evidence, so still opt-in -- but it is no longer unmeasured,
     * and a port whose title manages its own segments should try it. */
    if (getenv("GCM_DEFAULT_CTX_REPOINT") && s_gcm_ctx_out_ea && s_gcm_context_ea) {
        vm_write32(s_gcm_context_ea + 0x8, vm_read32(s_gcm_context_ea + 0x0));
        vm_write32(s_gcm_ctx_out_ea, s_gcm_context_ea);
        printf("[cellGcmSys] SetDefaultCommandBuffer: repointed "
               "gCellGcmCurrentContext@0x%08X -> 0x%08X\n",
               s_gcm_ctx_out_ea, s_gcm_context_ea);
    }
}

/* Debug dump — no-op in recomp */
void cellGcmDumpGraphicsError(void)
{
    printf("[cellGcmSys] DumpGraphicsError (no-op)\n");
}

/* Default command word size: 0x400 (1024) words */
u32 cellGcmGetDefaultCommandWordSize(void)
{
    return 0x400;
}

/* Default segment word size: 0x100 (256) words */
u32 cellGcmGetDefaultSegmentWordSize(void)
{
    return 0x100;
}

/* Immediate flip — perform flip right now */
s32 cellGcmSetFlipImmediate(u32 bufferId)
{
    printf("[cellGcmSys] SetFlipImmediate(bufferId=%u)\n", bufferId);
    return cellGcmSetFlipCommand(bufferId);
}

/* Set flip status directly */
void cellGcmSetFlipStatus(u32 status)
{
    s_flip_status = status;
}

/* Store graphics handler callback */
void cellGcmSetGraphicsHandler(CellGcmGraphicsHandler handler)
{
    printf("[cellGcmSys] SetGraphicsHandler(%p)\n", (void*)(size_t)handler);
    s_graphics_handler = handler;
}

/* Store queue handler callback */
void cellGcmSetQueueHandler(CellGcmQueueHandler handler)
{
    printf("[cellGcmSys] SetQueueHandler(%p)\n", (void*)(size_t)handler);
    s_queue_handler = handler;
}

/* Set second V frequency */
void cellGcmSetSecondVFrequency(u32 freq)
{
    printf("[cellGcmSys] SetSecondVFrequency(%u)\n", freq);
    s_second_v_frequency = freq;
}

/* Set VBlank frequency */
void cellGcmSetVBlankFrequency(u32 freq)
{
    printf("[cellGcmSys] SetVBlankFrequency(%u)\n", freq);
    s_vblank_frequency = freq;
}

/* Store user command value */
void cellGcmSetUserCommand(u32 cmd)
{
    GCM_USER_STORE(cmd);
}

/* Called when a FIFO consumer retires a user-interrupt method. Publishing
 * the cause and pending marker is atomic; guest code runs later in the pump.
 * Like the driver cause register, multiple pending commands coalesce. */
void cellGcmQueueUserCommand(u32 cmd)
{
    GCM_USER_STORE(cmd);
    GCM_USER_PENDING_STORE((1ULL << 32) | cmd);
}

/* Invalidate a tile region (unbind + clear) */
s32 cellGcmSetInvalidateTile(u8 index)
{
    if (index >= CELL_GCM_MAX_TILE_COUNT)
        return CELL_GCM_ERROR_INVALID_VALUE;

    printf("[cellGcmSys] SetInvalidateTile(index=%u)\n", index);

    s_tiles[index].bound = 0;
    memset(&s_tiles[index], 0, sizeof(CellGcmTileInfo));
    return CELL_OK;
}

/* EA IO address remap — stub, sorting not needed in recomp */
void cellGcmSortRemapEaIoAddress(void)
{
    /* No-op: IO address remapping not required for recomp */
}

/* Report data with location parameter */
CellGcmReportData* cellGcmGetReportDataAddressLocation(u32 index, u32 location)
{
    (void)location;  /* We only have one report area */

    if (index >= CELL_GCM_MAX_REPORT_COUNT) {
        printf("[cellGcmSys] WARNING: GetReportDataAddressLocation index %u out of range\n", index);
        return NULL;
    }
    return &s_report_data[index];
}

/* Get report value at index with location */
u32 cellGcmGetReportDataLocation(u32 index, u32 location)
{
    (void)location;

    if (index >= CELL_GCM_MAX_REPORT_COUNT)
        return 0;

    return s_report_data[index].value;
}

/* ---------------------------------------------------------------------------
 * VRAM upload registry
 *
 * A title whose texture uploads land at an address that does not match the
 * offset it later programs into SET_TEXTURE_OFFSET leaves the sampler reading
 * untouched memory. Recording what was uploaded lets the texture path find the
 * real bytes by size instead of trusting the offset. Diagnostic bridge: it is
 * a lookup, not a fix for the offset bookkeeping itself.
 * -----------------------------------------------------------------------*/
#define RSX_UPLOAD_LOG 256
static struct { u32 dst, size; int claimed; } s_uploads[RSX_UPLOAD_LOG];
static u32 s_upload_n;

void rsx_note_vram_upload(u32 dst_ea, u32 size)
{
    if (!size) return;
    s_uploads[s_upload_n % RSX_UPLOAD_LOG].dst     = dst_ea;
    s_uploads[s_upload_n % RSX_UPLOAD_LOG].size    = size;
    s_uploads[s_upload_n % RSX_UPLOAD_LOG].claimed = 0;
    s_upload_n++;
}

/* OLDEST unclaimed upload of exactly `want` bytes, claimed on return. Matching
 * by size alone hands every same-sized surface the same image; consuming them in
 * order pairs the Nth bind of a size with the Nth upload of that size, which is
 * the order a title loads and then draws its materials in. Returns 0 if none. */
u32 rsx_find_vram_upload(u32 want)
{
    u32 n = s_upload_n < RSX_UPLOAD_LOG ? s_upload_n : RSX_UPLOAD_LOG;
    u32 first = s_upload_n > RSX_UPLOAD_LOG ? s_upload_n - RSX_UPLOAD_LOG : 0;
    for (u32 i = 0; i < n; i++) {
        u32 idx = (first + i) % RSX_UPLOAD_LOG;
        if (s_uploads[idx].size == want && !s_uploads[idx].claimed) {
            s_uploads[idx].claimed = 1;
            return s_uploads[idx].dst;
        }
    }
    return 0;
}

/* Let the same pairing repeat next frame. */
void rsx_reset_upload_claims(void)
{
    for (u32 i = 0; i < RSX_UPLOAD_LOG; i++) s_uploads[i].claimed = 0;
}

/* ---------------------------------------------------------------------------
 * sys_rsx_* bridge (libs/video/sys_rsx.c)
 *
 * A PS3 firmware module links libgcm statically and drives RSX through the lv2
 * syscalls instead of importing cellGcmSys, so none of the HLE entry points above
 * ever run. Everything the FIFO walker needs is still here -- it just has to be
 * reachable from the syscall layer. These four accessors are that surface; the
 * walker, the method decoder and rsx_live_draw are untouched.
 * -----------------------------------------------------------------------*/

/* Bring the GCM state up the way cellGcmInit would, minus the IO window (the
 * driver maps that itself with sys_rsx_context_iomap). Returns the guest base of
 * RSX local memory, which is what sys_rsx_memory_allocate hands back. */
u32 cellGcm_syscall_bringup(u32 local_size)
{
    if (!s_gcm_initialized)
        cellGcmInit(0x10000, 0, 0);
    if (local_size)
        s_config.localSize = local_size;
    s_gcm_syscall_mode = 1;
    return s_config.localAddress;
}

/* Guest EA of the put/get/ref triple. The driver gets this as
 * sys_rsx_context_allocate's lpar_dma_control + 0x40 (verified in the image:
 * `lwz r3,0x18(r9)` / `addi r3,r3,64`), so the syscall returns this minus 0x40
 * and the driver's own flushes land exactly where the walker reads. */
u32 cellGcm_control_guest_addr(void) { return GCM_CONTROL_GUEST_ADDR; }

/* sys_rsx_context_iomap / iounmap: the same 1MB-page offset table
 * cellGcmMapEaIoAddress fills, which is what gcm_io2ea() and every offset
 * resolution read. */
void cellGcm_syscall_iomap(u32 ea, u32 io, u32 size)
{
    populate_offset_table(ea, io, size);
    /* (rsx5) a free slot, as the other mapping paths take: appending at the
     * count overwrote an active mapping once an unmap had left a hole */
    IoMapping* m = find_free_mapping();
    if (m) {
        m->ea = ea; m->io = io; m->size = size; m->active = 1;
        s_io_mapping_count++; gcm_spu_put_box_update();   /* (perf17) */
    } else rsx_lim_event(RSX_LIM_GCM_IOMAP, 0, "IO mapping table full: a syscall mapping is not walkable");
    rsx_lim_cap(RSX_LIM_GCM_IOMAP, CELL_GCM_MAX_IO_MAPPINGS); rsx_lim_use(RSX_LIM_GCM_IOMAP, (unsigned long long)s_io_mapping_count);
    if (!s_config.ioSize) { s_config.ioAddress = ea; s_config.ioSize = size; }
}

void cellGcm_syscall_iounmap(u32 io, u32 size)
{
    u32 page = io >> 20;
    u32 ea = (page < 65536 && s_ea_address_table[page] != 0xFFFF)
             ? ((u32)s_ea_address_table[page] << 20) : 0;
    if (ea) clear_offset_table(ea, io, size);
}

/* sys_rsx_context_attribute(0x001): the driver's explicit FIFO reset. Move the
 * walker with it -- leaving s_fifo_getoff where it was makes the next drain walk
 * from a stale offset through whatever the driver has since overwritten. */
void cellGcm_syscall_set_fifo(u32 put, u32 get)
{
    vm_write32(GCM_CONTROL_GUEST_ADDR + 0, put);
    vm_write32(GCM_CONTROL_GUEST_ADDR + 4, get);
    s_fifo_getoff  = get;
    s_fifo_calloff = 0;
}
