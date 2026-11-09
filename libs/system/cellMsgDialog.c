/*
 * ps3recomp - cellMsgDialog HLE implementation
 *
 * Prints dialog messages to stdout and queues callbacks for sysutil polling.
 * No actual UI is rendered.
 */

#include "cellMsgDialog.h"
#include "cellSysutil.h"
#include "ps3emu/guest_call.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>   /* tolower: the yes/no auto-answer heuristic */
#include "../../runtime/platform/win32_compat.h"   /* GetTickCount64 on POSIX too */
#include "../../runtime/ppu/ppu_memory.h"   /* GUEST_PTR, vm_read/vm_write: guest EA -> host */
#include "ps3emu/ps3_msgdlg.h"   /* the player answers Yes/No and OK dialogs (vk_overlay_saves.cpp) */

/* Pointer parameters here are GUEST addresses, and the dialog callback is a
 * guest OPD -- the HLE ABI adapter passes both straight through as guest
 * values. Printing msgString as a host char* faulted the moment Tokyo Jungle
 * opened its data-install prompt, and calling s_callback as a host function
 * pointer would jump into the middle of guest memory. */
extern uint8_t* vm_base;

static const char* guest_str(const void* p)
{
    uint32_t ea = (uint32_t)(uintptr_t)p;
    return (ea && vm_base) ? (const char*)(vm_base + ea) : "<null>";
}

/* Do not run a completion before Open2 returns: games establish their
 * dialog wait state after opening, then consume the result on a sysutil poll. */
static s32 queue_dialog_callback(CellMsgDialogCallback cb, int32_t result,
                                 void* userdata)
{
    return cellSysutilQueueGuestCallback((u32)(uintptr_t)cb,
        (u64)(int64_t)result, (u64)(uintptr_t)userdata);
}

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

static int                   s_dialog_open = 0;
static CellMsgDialogCallback s_callback    = NULL;
static void*                 s_userdata    = NULL;
static CellMsgDialogType     s_type        = 0;

/* The dialog the player is answering (ps3_msgdlg.h). The state above is now
 * also touched by the overlay's thread (the answer), so it changes under one
 * small lock; the provider is never called while it is held. s_id names each
 * Open; s_ui is set while the provider shows it. */
static unsigned     s_id, s_next_id;
static int          s_ui;
static volatile int s_msg_lock;
static void msg_lock(void)   { while (__atomic_exchange_n(&s_msg_lock, 1, __ATOMIC_ACQUIRE)) { } }
static void msg_unlock(void) { __atomic_store_n(&s_msg_lock, 0, __ATOMIC_RELEASE); }

/* Close the open dialog (only dialog `id` when non-zero) and queue the title's
 * callback with `result`, delivered on its next cellSysutilCheckCallback.
 * CELL_OK and nothing queued when it is not open (any more). */
static s32 dialog_finish(unsigned id, s32 result)
{
    msg_lock();
    if (!s_dialog_open || (id && s_id != id)) { msg_unlock(); return CELL_OK; }
    CellMsgDialogCallback cb = s_callback;
    void* ud = s_userdata;
    s_dialog_open = 0;
    s_ui = 0;
    s_callback = NULL;
    msg_unlock();
    return cb ? queue_dialog_callback(cb, result, ud) : CELL_OK;
}

static void dialog_answered(unsigned id, int button)   /* the provider: the player chose */
{
    printf("[cellMsgDialog] the player answered %s%c",
           button == CELL_MSGDIALOG_BUTTON_YES ? "YES/OK" : button == CELL_MSGDIALOG_BUTTON_NO ? "NO"
           : button == CELL_MSGDIALOG_BUTTON_ESCAPE ? "ESCAPE (Circle)" : "?", 10);
    dialog_finish(id, button);
}

/* PS3_MSGDIALOG=0 or MSGDIALOG_ANSWER set: Yes/No and OK are answered by
 * themselves (the knob a headless run or a title's workaround already uses). */
static int dialog_ui_on(void)
{
    static int v = -1;
    if (v < 0) {
        const char* e = getenv("PS3_MSGDIALOG");
        v = !(e && e[0] == '0') && !getenv("MSGDIALOG_ANSWER");
    }
    return v && g_ps3_msgdlg.open != NULL;
}

/* Deadline for auto-dismissing a no-button dialog; 0 = not armed.
 *
 * A BUTTON_TYPE_NONE dialog waits for the title to take it down (see
 * cellMsgDialogOpen2), which is right for a progress notice the title closes
 * itself -- Virtua Fighter 5 Aborts its "Checking game data" notice when the
 * check finishes. It is wrong for the notice at the END of that check:
 * "Check complete." is a no-button dialog the title expects a PERSON to
 * dismiss, and with nobody there the title waits on it forever.
 *
 * Both arrive as type 0x01, so nothing in the type distinguishes them, and
 * guessing from the message text would be the same fragile trick the yes/no
 * answer already has to use. A grace period needs no guess: the title gets
 * first refusal on closing its own dialog, and only one it has left up beyond
 * the deadline is dismissed for it. MSGDIALOG_AUTODISMISS_MS tunes it; 0 waits
 * forever, which is the behaviour to pick if a title turns out to hold a
 * legitimate notice open longer than the default. */
static ULONGLONG s_nobutton_deadline = 0;



static unsigned nobutton_grace_ms(void)
{
    static int ms = -1;
    if (ms < 0) { const char* e = getenv("MSGDIALOG_AUTODISMISS_MS");
                  ms = e ? atoi(e) : 2000; if (ms < 0) ms = 0; }
    return (unsigned)ms;
}


/* Progress bar state */
#define MAX_PROGRESS_BARS 2

typedef struct {
    u32  value;
    char message[256];
} ProgressBarState;

static ProgressBarState s_progress[MAX_PROGRESS_BARS];

/* ---------------------------------------------------------------------------
 * API implementations
 * -----------------------------------------------------------------------*/

/* Does this yes/no prompt offer to ABORT something the title is doing?
 * Case-insensitive substring match on the words a title uses for it. Only
 * consulted when MSGDIALOG_ANSWER is unset; the call site explains why the
 * dialog's own type field cannot answer this. */
static int msg_offers_to_abort(const char* msg)
{
    static const char* const words[] = { "cancel", "quit", "abort", NULL };
    if (!msg) return 0;
    for (int w = 0; words[w]; w++) {
        const size_t n = strlen(words[w]);
        for (const char* p = msg; *p; p++) {
            size_t i = 0;
            while (i < n && p[i] && (char)tolower((unsigned char)p[i]) == words[w][i]) i++;
            if (i == n) return 1;
        }
    }
    return 0;
}

s32 cellMsgDialogOpen2(CellMsgDialogType type, const char* msgString,
                        CellMsgDialogCallback callback, void* userdata,
                        void* extParam)
{
    printf("[cellMsgDialog] Open2(type=0x%08X, msg='%s')\n",
           type, guest_str(msgString));

    if (s_dialog_open) {
        printf("[cellMsgDialog] WARNING: dialog already open, closing previous\n");
    }

    msg_lock();
    const unsigned old_ui = (s_dialog_open && s_ui) ? s_id : 0;
    s_dialog_open = 1;
    s_callback    = callback;
    s_userdata    = userdata;
    s_type        = type;
    s_ui          = 0;
    const unsigned id = s_id = ++s_next_id ? s_next_id : ++s_next_id;
    msg_unlock();
    if (old_ui && g_ps3_msgdlg.close) g_ps3_msgdlg.close(old_ui);   /* replaced: off the screen */
    memset(s_progress, 0, sizeof(s_progress));

    /* Print the message so developers can see it */
    if (msgString) {
        printf("========================================\n");
        printf("[DIALOG] %s\n", guest_str(msgString));
        printf("========================================\n");
    }

    /* Determine button type and auto-respond */
    u32 button_type = type & 0x00000030;
    int has_progress = (type & 0x0000F000) != 0;

    /* Yes/No and OK: the PLAYER answers, as on the console (ps3_msgdlg.h; the
     * overlay draws it). Open2 returns now and the button reaches the title's
     * callback on a later sysutil poll, where hardware delivers it. The cursor
     * starts where the type says (DEFAULT_CURSOR_NO), Circle answers ESCAPE
     * unless DISABLE_CANCEL_ON. inFAMOUS 2's "Do you want to save before
     * loading?" (0x117) used to be answered YES: Load always went through a
     * save first, and backing out of that save cancelled the load. */
    if (!has_progress && (button_type == CELL_MSGDIALOG_TYPE_BUTTON_TYPE_YESNO ||
                          button_type == CELL_MSGDIALOG_TYPE_BUTTON_TYPE_OK) && dialog_ui_on() &&
        g_ps3_msgdlg.open(id, msgString ? guest_str(msgString) : "",
                          button_type == CELL_MSGDIALOG_TYPE_BUTTON_TYPE_YESNO ? PS3_MSGDLG_YESNO : PS3_MSGDLG_OK,
                          (type & CELL_MSGDIALOG_TYPE_DEFAULT_CURSOR_NO) != 0,
                          (type & CELL_MSGDIALOG_TYPE_DISABLE_CANCEL_ON) == 0,
                          (type & CELL_MSGDIALOG_TYPE_SE_TYPE_NORMAL) == 0, dialog_answered)) {
        msg_lock();
        if (s_dialog_open && s_id == id) s_ui = 1;
        msg_unlock();
        printf("[cellMsgDialog] shown: waiting for the player%c", 10);
        return CELL_OK;
    }
    if (!has_progress && button_type == CELL_MSGDIALOG_TYPE_BUTTON_TYPE_NONE && msgString &&
        dialog_ui_on() && g_ps3_msgdlg.notice)
        g_ps3_msgdlg.notice(guest_str(msgString));   /* a no-button notice: its text on screen for a while */

    /* A BUTTON_TYPE_NONE dialog is an informational notice with no buttons: on
     * hardware it stays on screen until the TITLE takes it down with
     * cellMsgDialogClose/Abort, and only then does its callback fire. We used
     * to queue that callback immediately, which told the title its own notice
     * had already been dismissed.
     *
     * Virtua Fighter 5 shows what that costs. It puts up "Checking game data.
     * This may take several minutes" (type 0x01 -- SE_NORMAL, BUTTON_TYPE_NONE),
     * we instantly reported it closed, the title read that as the player
     * backing out and asked "Are you sure you want to cancel checking the game
     * data?", and on being told no it reopened the notice -- 26 times in a
     * 260-second boot, which is where the boot time was going.
     *
     * So a no-button dialog waits, exactly like the progress bar already did.
     * Close() still queues BUTTON_NONE and Abort() still queues BUTTON_ESCAPE,
     * so the title gets its callback -- when it asks for it. */
    /* OFF BY DEFAULT, and that is a deliberate retreat rather than a doubt
     * about the diagnosis. Holding the no-button dialog open is what hardware
     * does and it removes VF5's check loop completely (26 reopenings -> 1).
     * It then stalls the title HARDER than the bug did: it stops flipping
     * altogether at ~3,300 frames, where the old behaviour looped noisily but
     * still reached its logo sequence. Dismissing the leftover "Check
     * complete." after a grace period does not help, as NONE or as ESCAPE, and
     * neither does absorbing the title's own Close/Abort afterwards -- all
     * three were built and measured. So there is a third thing VF5 needs after
     * its game-data check that we do not do, and the loop was hiding it.
     *
     * Correct-but-hangs is worse than wrong-but-boots for a title people are
     * trying to run, so this waits behind MSGDIALOG_NOBUTTON_WAITS=1 until the
     * stall behind it is understood. */
    static int nobutton_waits = -1;
    if (nobutton_waits < 0) nobutton_waits = getenv("MSGDIALOG_NOBUTTON_WAITS") ? 1 : 0;
    int stays_open = has_progress ||
                     (nobutton_waits &&
                      button_type == CELL_MSGDIALOG_TYPE_BUTTON_TYPE_NONE);

    if (!stays_open) {
        /* Queue the headless auto-response for non-progress dialogs */
        s32 result = CELL_MSGDIALOG_BUTTON_OK;

        if (button_type == CELL_MSGDIALOG_TYPE_BUTTON_TYPE_YESNO) {
            /* MSGDIALOG_ANSWER=no answers every yes/no prompt NO instead. The
             * auto-answer is a guess about what the title wants, and yes is not
             * always the boot-friendliest one: a "use game data?" prompt
             * answered yes sends the title down an install/cache path a port may
             * have nothing behind, where no just plays from disc. A knob costs
             * less than a rebuild to try the other branch. */
            /* MSGDIALOG_ANSWER=auto answers whichever button lets the title
             * CONTINUE -- NO for a prompt that offers to abort something, YES
             * otherwise:
             *
             *   "Do you want to use game data?"               -> YES
             *   "Are you sure you want to cancel checking the
             *    game data?"                                  -> NO
             *   "Do you want to cancel the load operation?"   -> NO
             *
             * That is what a person would answer, and it stops the runtime
             * cancelling a title's own work. It is NOT the default, and that is
             * a measurement rather than a preference.
             *
             * Virtua Fighter 5 asks the last two during its boot. Answered YES
             * it cancels its own game-data check and then its own load, which
             * sounds unambiguously bad -- but cancelling the check is how that
             * title gets ON with its boot here, and answering NO instead leaves
             * it reopening the check forever. Over five minutes, =auto executes
             * 0 draw groups; the default YES executes 12,829 and reaches
             * cellSaveDataFixedLoad2. So the destructive answer is currently
             * the one that works, which means something downstream of a
             * COMPLETED check is missing, and being right about the button is
             * worth less than the title booting until that is found.
             *
             * The dialog TYPE cannot decide this either way: both VF5 prompts
             * arrive as 0x11 with the default cursor on YES, because on
             * hardware the cursor only says where the highlight starts and a
             * person reads the sentence. So =auto keys on the text, English
             * words only, and a localised build falls through to YES. */
            static int forced = -1;   /* 0 = force YES, 1 = force NO, 2 = auto */
            if (forced < 0) {
                const char* e = getenv("MSGDIALOG_ANSWER");
                if      (e && (e[0] == 'n' || e[0] == 'N')) forced = 1;   /* always NO */
                else if (e && (e[0] == 'a' || e[0] == 'A')) forced = 2;   /* heuristic */
                else                                        forced = 0;   /* always YES (default) */
            }
            int auto_no = (forced == 2) ? msg_offers_to_abort(guest_str(msgString)) : 0;
            int no_ = (forced == 2) ? auto_no : forced;
            result = no_ ? CELL_MSGDIALOG_BUTTON_NO : CELL_MSGDIALOG_BUTTON_YES;
            printf("[cellMsgDialog] Auto-responding: %s%s\n", no_ ? "NO" : "YES",
                   auto_no ? " (prompt offers to abort)" : "");
        } else if (button_type == CELL_MSGDIALOG_TYPE_BUTTON_TYPE_OK) {
            result = CELL_MSGDIALOG_BUTTON_OK;
            printf("[cellMsgDialog] Auto-responding: OK\n");
        } else {
            result = CELL_MSGDIALOG_BUTTON_NONE;
            printf("[cellMsgDialog] Auto-responding: NONE (no buttons)\n");
        }

        /* Close and queue the callback; never enter guest code here. */
        return dialog_finish(id, result);
    } else {
        printf("[cellMsgDialog] %s dialog opened (waits for Close/Abort)\n",
               has_progress ? "Progress bar" : "No-button");
        s_nobutton_deadline = (!has_progress && nobutton_grace_ms())
            ? GetTickCount64() + nobutton_grace_ms() : 0;
    }

    return CELL_OK;
}

/* cellMsgDialogOpen -- the pre-3.40 entry point, identical arguments and
 * behaviour to Open2 (RPCS3 forwards it the same way). Registering only Open2
 * meant a title on the older API got the unresolved-NID default and then waited
 * forever for a callback that could never fire. Virtua Fighter 5 opens one
 * during boot and stops dead there. */
s32 cellMsgDialogOpen(CellMsgDialogType type, const char* msgString,
                      CellMsgDialogCallback callback, void* userdata,
                      void* extParam)
{
    return cellMsgDialogOpen2(type, msgString, callback, userdata, extParam);
}

s32 cellMsgDialogClose(float delayMs)
{
    printf("[cellMsgDialog] Close(delay=%.1f ms)\n", delayMs);

    msg_lock();
    if (!s_dialog_open) {
        msg_unlock();
        return CELL_MSGDIALOG_ERROR_DIALOG_NOT_OPENED;
    }
    const unsigned id = s_id;
    const int ui = s_ui;
    s_nobutton_deadline = 0;
    msg_unlock();
    if (ui && g_ps3_msgdlg.close) g_ps3_msgdlg.close(id);   /* the title took it down: off the screen */
    return dialog_finish(id, CELL_MSGDIALOG_BUTTON_NONE);
}

s32 cellMsgDialogAbort(void)
{
    printf("[cellMsgDialog] Abort()\n");

    msg_lock();
    if (!s_dialog_open) {
        msg_unlock();
        return CELL_MSGDIALOG_ERROR_DIALOG_NOT_OPENED;
    }
    const unsigned id = s_id;
    const int ui = s_ui;
    s_nobutton_deadline = 0;
    msg_unlock();
    if (ui && g_ps3_msgdlg.close) g_ps3_msgdlg.close(id);
    return dialog_finish(id, CELL_MSGDIALOG_BUTTON_ESCAPE);
}

s32 cellMsgDialogProgressBarSetMsg(u32 progressBarIndex, const char* msgString)
{
    if (progressBarIndex >= MAX_PROGRESS_BARS)
        return CELL_MSGDIALOG_ERROR_PARAM;

    if (!s_dialog_open)
        return CELL_MSGDIALOG_ERROR_DIALOG_NOT_OPENED;

    if (msgString) {
        strncpy(s_progress[progressBarIndex].message, GUEST_PTR(msgString, const char*),
                sizeof(s_progress[progressBarIndex].message) - 1);
        s_progress[progressBarIndex].message[sizeof(s_progress[progressBarIndex].message) - 1] = '\0';
        printf("[cellMsgDialog] ProgressBar[%u] msg='%s'\n", progressBarIndex, guest_str(msgString));
    }

    return CELL_OK;
}

s32 cellMsgDialogProgressBarReset(u32 progressBarIndex)
{
    if (progressBarIndex >= MAX_PROGRESS_BARS)
        return CELL_MSGDIALOG_ERROR_PARAM;

    if (!s_dialog_open)
        return CELL_MSGDIALOG_ERROR_DIALOG_NOT_OPENED;

    s_progress[progressBarIndex].value = 0;
    printf("[cellMsgDialog] ProgressBar[%u] reset to 0%%\n", progressBarIndex);

    return CELL_OK;
}

s32 cellMsgDialogProgressBarInc(u32 progressBarIndex, u32 delta)
{
    if (progressBarIndex >= MAX_PROGRESS_BARS)
        return CELL_MSGDIALOG_ERROR_PARAM;

    if (!s_dialog_open)
        return CELL_MSGDIALOG_ERROR_DIALOG_NOT_OPENED;

    s_progress[progressBarIndex].value += delta;
    if (s_progress[progressBarIndex].value > 100)
        s_progress[progressBarIndex].value = 100;

    printf("[cellMsgDialog] ProgressBar[%u] = %u%%\n",
           progressBarIndex, s_progress[progressBarIndex].value);

    return CELL_OK;
}


/* Called from cellSysutilCheckCallback each time the title polls sysutil.
 * Dismisses a no-button dialog the title has left open past its grace period;
 * see s_nobutton_deadline for why this cannot be decided at open time. */
void cellMsgDialog_tick(void)
{
    if (!s_dialog_open || !s_nobutton_deadline) return;
    if (GetTickCount64() < s_nobutton_deadline) return;
    s_nobutton_deadline = 0;
    printf("[cellMsgDialog] no-button dialog auto-dismissed after %u ms\n",
           nobutton_grace_ms());
    dialog_finish(0, CELL_MSGDIALOG_BUTTON_NONE);
}
