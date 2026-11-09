/*
 * ps3_msgdlg -- the system's message dialog (cellMsgDialogOpen2: a sentence and
 * Yes/No or OK). On a PS3 the system draws it and the title's callback gets the
 * player's button on a later cellSysutilCheckCallback; Open2 itself returns at
 * once. cellMsgDialog.c hands Yes/No and OK dialogs to a provider -- the Vulkan
 * overlay (libs/video/vk_overlay_saves.cpp) -- which shows it and reports the
 * answer back through `answer`. With none (headless, no window, PS3_OVERLAY=0,
 * PS3_MSGDIALOG=0, or MSGDIALOG_ANSWER set) the old automatic answer stays.
 *
 * One weak object, as in ps3_savedlg.h, so cellMsgDialog.c and its tests build
 * without the overlay.
 */
#ifndef PS3EMU_PS3_MSGDLG_H
#define PS3EMU_PS3_MSGDLG_H

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_MSC_VER)
#  define PS3_MSGDLG_SHARED __declspec(selectany)
#else
#  define PS3_MSGDLG_SHARED __attribute__((weak))
#endif

enum { PS3_MSGDLG_YESNO = 1, PS3_MSGDLG_OK = 2 };

/* The player's answer to dialog `id`: CELL_MSGDIALOG_BUTTON_YES / _NO / _OK /
 * _ESCAPE (Circle). Called from the thread that draws; never twice for one id. */
typedef void (*ps3_msgdlg_answer_fn)(unsigned id, int button);

typedef struct ps3_msgdlg_provider {
    /* Show dialog `id` (any thread; returns at once). `text` is copied.
     * `buttons`: PS3_MSGDLG_YESNO or PS3_MSGDLG_OK; `default_no`: the cursor
     * starts on No; `can_cancel`: Circle answers ESCAPE; `is_error`: the title's
     * SE_TYPE_ERROR. 1 = shown (the answer comes through `answer`), 0 = it
     * cannot be shown (the caller answers by itself). */
    int  (*open)(unsigned id, const char* text, int buttons, int default_no, int can_cancel,
                 int is_error, ps3_msgdlg_answer_fn answer);
    /* The title took dialog `id` down itself (Close/Abort) or opened another over it. */
    void (*close)(unsigned id);
    /* A no-button notice's text, shown for a few seconds (the dialog itself is
     * answered as before: the title gets no wait it did not have). */
    void (*notice)(const char* text);
} ps3_msgdlg_provider;

PS3_MSGDLG_SHARED ps3_msgdlg_provider g_ps3_msgdlg = { 0, 0, 0 };

#ifdef __cplusplus
}
#endif

#endif
