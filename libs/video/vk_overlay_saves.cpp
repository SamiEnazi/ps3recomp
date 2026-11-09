/*
 * vk_overlay_saves -- the system's save-data list ("choose a save") dialog,
 * drawn by the in-game overlay (include/ps3emu/ps3_savedlg.h has the contract).
 *
 * cellSaveDataListSave2 / ListLoad2 (libs/system/cellSaveData.c) hand the list
 * the title's funcList set to show() on the calling guest thread, which blocks
 * until the player acts. The dialog takes the settings menu's window while it
 * is up (vk_overlay.cpp: set_open, so the same input hold -- the title reads a
 * neutral pad --, free cursor and RSX-thread drawing), and the menu's pages are
 * not shown meanwhile.
 *
 *   one row per save: ICON0.PNG, SUB_TITLE, DETAIL lines, date and time;
 *   "New Save Data" on a save list (the title's newData);
 *   Cross / Enter / a click on the selected row   choose (an existing save on a
 *                                                 save list asks "Overwrite?")
 *   Triangle / Delete                            delete (asks first; the list stays)
 *   Circle / Esc / Backspace                     back: CELL_SAVEDATA_RET_CANCEL
 *   D-pad / left stick / arrow keys / mouse      move
 * Every question starts on "No". Input is ignored for the first 0.3 s, so a
 * press still held from the game's own menu does not pick at once.
 *
 * stderr lines "[savedlg] ..." say what was shown and what the player chose
 * (tests wait on them). PS3_SAVEDATA_DIALOG=0: no dialog (the List calls pick
 * by themselves, as before). A dialog that is never drawn for 20 s (window
 * gone or hidden) backs out, so a save is never made blind.
 *
 * The system's message dialog lives here too (include/ps3emu/ps3_msgdlg.h,
 * libs/system/cellMsgDialog.c): a Yes/No or OK question the title asks with
 * cellMsgDialogOpen2. Nothing waits on it (Open2 returned at once); the
 * player's button goes back to cellMsgDialog, which queues the title's
 * callback. Left/Right choose, Cross/Enter confirm, Circle/Esc = ESCAPE unless
 * the title disabled cancel; the cursor starts where the title says. It is
 * drawn over the save list if both are ever up, and takes the input first.
 * "[msgdlg] ..." lines on stderr. PS3_MSGDIALOG=0: answered by themselves.
 * A no-button notice's text is shown in a corner for 4 s.
 */
#include "imgui.h"
#include "imgui_internal.h"   /* ImGui::RegisterUserTexture: a texture the Vulkan renderer uploads (1.92) */

#include "../../include/ps3emu/ps3_savedlg.h"
#include "../../include/ps3emu/ps3_msgdlg.h"
#include "rsx_image_io.h"
#include "vk_overlay_theme.h"   /* the menu's look (Hero blue / Infamous red) */

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <vector>

#include <float.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

extern "C" int vk_overlay_can_show(void);   /* vk_overlay.cpp */
extern "C" void vk_overlay_notice_for(const char* text, double secs);

/* CELL_MSGDIALOG_BUTTON_* (libs/system/cellMsgDialog.h) */
enum { MD_BTN_YES = 1, MD_BTN_NO = 2, MD_BTN_OK = 1, MD_BTN_ESCAPE = 3 };

/* CELL_PAD_CTRL_* (libs/input/cellPad.h), as vk_overlay.cpp passes them */
enum {
    SD_UP = 1 << 4, SD_RIGHT = 1 << 5, SD_DOWN = 1 << 6, SD_LEFT = 1 << 7,
    SD_TRIANGLE = 1 << 12, SD_CIRCLE = 1 << 13, SD_CROSS = 1 << 14,
};
/* what the player did this frame */
enum { A_UP = 1, A_DOWN = 2, A_SIDE = 4, A_OK = 8, A_BACK = 16, A_DEL = 32, A_YES = 64, A_NO = 128 };

static const double kGrace = 0.30;   /* s after opening before a press counts */

static double sd_now(void)
{
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

/* ---- icons: decoded once per distinct PNG (inFAMOUS 2 writes the same one into
 * every save), uploaded by ImGui's Vulkan renderer as user textures ------------- */
enum { ICON_MAX = 32 };
struct SdIcon { uint64_t key; ImTextureData* tex; bool registered; };
static SdIcon     s_icon[ICON_MAX];
static int        s_n_icon;
static std::mutex s_icon_lock;

static uint64_t fnv64(const unsigned char* p, unsigned n)
{
    uint64_t h = 1469598103934665603ull;
    for (unsigned i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ull; }
    return h ^ n;
}

/* Guest thread: the icon's slot, -1 = none. */
static int icon_get(const unsigned char* png, unsigned size)
{
    if (!png || size < 8) return -1;
    const uint64_t key = fnv64(png, size);
    {
        std::lock_guard<std::mutex> g(s_icon_lock);
        for (int i = 0; i < s_n_icon; i++)
            if (s_icon[i].key == key) return s_icon[i].tex ? i : -1;
        if (s_n_icon >= ICON_MAX) return -1;
    }
    unsigned char* rgba = NULL;
    unsigned w = 0, h = 0;
    ImTextureData* t = NULL;
    if (rsx_image_load_png_mem(png, size, &rgba, &w, &h) && w <= 2048 && h <= 2048) {
        t = new ImTextureData();
        t->Create(ImTextureFormat_RGBA32, (int)w, (int)h);
        memcpy(t->Pixels, rgba, (size_t)w * h * 4);
        t->UseColors = true;
    }
    free(rgba);
    std::lock_guard<std::mutex> g(s_icon_lock);
    if (s_n_icon >= ICON_MAX) { delete t; return -1; }
    s_icon[s_n_icon].key = key;          /* a PNG that does not decode is remembered too */
    s_icon[s_n_icon].tex = t;
    s_icon[s_n_icon].registered = false;
    const int slot = s_n_icon++;
    return t ? slot : -1;
}

/* Render thread (ImGui frame): the slot's texture, registered with ImGui the first time. */
static ImTextureData* icon_tex(int i)
{
    if (i < 0) return NULL;
    std::lock_guard<std::mutex> g(s_icon_lock);
    if (i >= s_n_icon || !s_icon[i].tex) return NULL;
    if (!s_icon[i].registered) { ImGui::RegisterUserTexture(s_icon[i].tex); s_icon[i].registered = true; }
    return s_icon[i].tex;
}

/* ---- the dialog's state ------------------------------------------------------ */
struct SdRow { std::string dir, title, sub, detail; long long mtime; bool is_new; int icon; };

enum { PH_IDLE, PH_WANT_OPEN, PH_SHOWING, PH_WANT_CLOSE };
static std::mutex              s_m;          /* everything below except the render-thread block */
static std::condition_variable s_cv;
static std::atomic<int>        s_phase(PH_IDLE);
static bool   s_waiting;        /* a guest thread waits for the player */
static bool   s_answered;       /* ... and the player has acted */
static bool   s_shutdown;
static bool   s_drawn;          /* drawn since the last show() */
static int    s_action, s_index;
static std::vector<SdRow> s_rows;
static bool   s_is_save;
static int    s_cur;
static std::string s_msg;
static unsigned s_gen;          /* a new list was handed over */
static double s_open_t;         /* when the dialog came up */
static std::mutex s_show_lock;  /* one guest caller at a time */
static std::atomic<int> s_back(0);   /* Esc, from the event pump */
static double s_answer_t;       /* when the player acted */

/* the message dialog (ps3_msgdlg.h), under s_m */
enum { MD_IDLE, MD_WANT_OPEN, MD_SHOWING, MD_WANT_CLOSE };
static std::atomic<int> s_md(MD_IDLE);
static unsigned    s_md_id;
static std::string s_md_text;
static int         s_md_buttons;            /* PS3_MSGDLG_YESNO / _OK */
static bool        s_md_cancel, s_md_error, s_md_yes;   /* Circle allowed, SE_TYPE_ERROR, the cursor on Yes */
static ps3_msgdlg_answer_fn s_md_answer;    /* NULL once answered or closed */
static double      s_md_open_t;
static bool        s_win;                   /* the overlay's window is open for a system dialog (the pump's thread) */

/* render thread only */
static int      s_confirm;      /* 0 none, 1 "Overwrite?", 2 "Delete?" */
static bool     s_yes;          /* the highlighted answer */
static unsigned s_seen_gen;
static bool     s_scroll;       /* bring the cursor's row into view */
static unsigned s_act;          /* A_* from the controller, since the last frame */
static uint32_t s_pad_prev;
static bool     s_keys_held;    /* Enter / Space / Delete / Backspace down (ImGui's view) */
static int      s_rep_dir;
static double   s_rep_t;

static void phase_down(void)    /* s_m held: the guest is done with the dialog */
{
    const int p = s_phase.load();
    if (p == PH_WANT_OPEN) s_phase = PH_IDLE;
    else if (p == PH_SHOWING) s_phase = PH_WANT_CLOSE;
}

static const char* row_name(const SdRow& r)
{
    if (r.is_new) return "New Save Data";
    if (!r.sub.empty()) return r.sub.c_str();
    if (!r.title.empty()) return r.title.c_str();
    return r.dir.c_str();
}

static void answer(int action, int index)   /* s_m held */
{
    s_action = action;
    s_index = index;
    s_answered = true;
    s_answer_t = sd_now();
    s_confirm = 0;
    const char* what = action == PS3_SAVEDLG_PICK ? "picked" : action == PS3_SAVEDLG_DELETE ? "delete" : "backed out";
    if (index >= 0 && index < (int)s_rows.size())
        fprintf(stderr, "[savedlg] %s %d '%s' (%s%s)\n", what, index, s_rows[index].dir.c_str(),
                row_name(s_rows[index]), s_rows[index].is_new ? ", new" : "");
    else
        fprintf(stderr, "[savedlg] %s\n", what);
    s_cv.notify_all();
}

static std::string cstr(const char* s, size_t cap) { return std::string(s, strnlen(s, cap)); }

/* ---- the provider (any guest thread) ----------------------------------------- */
static int sd_show(const ps3_savedlg_request* rq, int* action, int* index)
{
    if (!rq || !action || !index || rq->n < 0 || (rq->n && !rq->entries)) return 0;
    std::lock_guard<std::mutex> one(s_show_lock);
    {
        std::lock_guard<std::mutex> g(s_m);
        if (s_shutdown || !vk_overlay_can_show()) { phase_down(); return 0; }
    }
    std::vector<SdRow> rows;
    rows.reserve((size_t)rq->n);
    for (int i = 0; i < rq->n; i++) {
        const ps3_savedlg_entry& e = rq->entries[i];
        SdRow r;
        r.dir = cstr(e.dir, sizeof e.dir);
        r.title = cstr(e.title, sizeof e.title);
        r.sub = cstr(e.subtitle, sizeof e.subtitle);
        r.detail = cstr(e.detail, sizeof e.detail);
        r.mtime = e.mtime;
        r.is_new = e.is_new != 0;
        r.icon = icon_get(e.icon, e.icon_size);   /* decoded here, off the render thread */
        rows.push_back(r);
    }
    std::unique_lock<std::mutex> lk(s_m);
    s_rows.swap(rows);
    s_is_save = rq->is_save != 0;
    const int n = (int)s_rows.size();
    s_cur = rq->focus < 0 ? 0 : rq->focus >= n ? (n ? n - 1 : 0) : rq->focus;
    s_msg = rq->message ? rq->message : "";
    s_gen++;
    s_answered = false;
    s_waiting = true;
    s_drawn = false;
    const int p = s_phase.load();
    if (p == PH_IDLE) s_phase = PH_WANT_OPEN;
    else if (p == PH_WANT_CLOSE) { s_phase = PH_SHOWING; s_open_t = sd_now(); }   /* still up: stays */
    fprintf(stderr, "[savedlg] open: %s list, %d entr%s, cursor on %d%s%s%s\n", s_is_save ? "save" : "load",
            n, n == 1 ? "y" : "ies", s_cur, n ? " '" : "", n ? s_rows[s_cur].dir.c_str() : "", n ? "'" : "");
    const auto t0 = std::chrono::steady_clock::now();
    while (!s_answered && !s_shutdown) {
        s_cv.wait_for(lk, std::chrono::milliseconds(200));
        if (!s_answered && !s_drawn && std::chrono::steady_clock::now() - t0 > std::chrono::seconds(20)) {
            fprintf(stderr, "[savedlg] not drawn for 20 s (window hidden or gone): backing out\n");
            answer(PS3_SAVEDLG_CANCEL, -1);
        }
    }
    if (!s_answered) { s_action = PS3_SAVEDLG_CANCEL; s_index = -1; }   /* the window went away */
    *action = s_action;
    *index = s_index;
    s_waiting = false;
    if (s_action != PS3_SAVEDLG_DELETE || s_shutdown) phase_down();   /* after a DELETE it stays up */
    return 1;
}

static void sd_close(void)
{
    std::lock_guard<std::mutex> g(s_m);
    s_waiting = false;
    phase_down();
}

/* ---- the message dialog's provider (any guest thread) -------------------------- */
static int md_open(unsigned id, const char* text, int buttons, int default_no, int can_cancel,
                   int is_error, ps3_msgdlg_answer_fn answer)
{
    std::lock_guard<std::mutex> g(s_m);
    if (s_shutdown || !answer || (buttons != PS3_MSGDLG_YESNO && buttons != PS3_MSGDLG_OK) || !vk_overlay_can_show())
        return 0;
    s_md_id = id;
    s_md_text = text ? text : "";
    s_md_buttons = buttons;
    s_md_cancel = can_cancel != 0;
    s_md_error = is_error != 0;
    s_md_yes = buttons == PS3_MSGDLG_OK || !default_no;
    s_md_answer = answer;
    const int st = s_md.load();
    if (st == MD_IDLE || st == MD_WANT_OPEN) s_md = MD_WANT_OPEN;
    else { s_md = MD_SHOWING; s_md_open_t = sd_now(); }   /* replaced one that is still on screen */
    fprintf(stderr, "[msgdlg] open %u: \"%s\" (%s, cursor on %s%s)\n", id, s_md_text.c_str(),
            buttons == PS3_MSGDLG_YESNO ? "yes/no" : "ok", s_md_yes ? (buttons == PS3_MSGDLG_OK ? "OK" : "Yes") : "No",
            s_md_cancel ? ", Circle = escape" : "");
    return 1;
}

static void md_close(unsigned id)
{
    std::lock_guard<std::mutex> g(s_m);
    const int st = s_md.load();
    if (st == MD_IDLE || s_md_id != id) return;
    s_md = st == MD_WANT_OPEN ? MD_IDLE : MD_WANT_CLOSE;
    if (s_md_answer) fprintf(stderr, "[msgdlg] %u closed by the title\n", id);
    s_md_answer = NULL;
}

static void md_notice(const char* text) { if (text && *text) vk_overlay_notice_for(text, 4.0); }

/* ---- vk_overlay.cpp's side ---------------------------------------------------- */
extern "C" int vk_overlay_saves_active(void)
{
    return s_win || s_phase.load() != PH_IDLE || s_md.load() != MD_IDLE;
}

/* The event pump (the thread that draws): 1 = open the window for a system
 * dialog, 2 = close it (both the save list and the message dialog done). */
extern "C" int vk_overlay_saves_poll(void)
{
    if (!s_win && s_phase.load() == PH_IDLE && s_md.load() == MD_IDLE) return 0;
    std::lock_guard<std::mutex> g(s_m);
    const double now = sd_now();
    /* Not while the button or key that chose is still down: the title read a
     * neutral pad all along and would take it as a new press (up to 1.5 s). */
    const bool held = ((s_pad_prev & (SD_CROSS | SD_CIRCLE | SD_TRIANGLE)) || s_keys_held) && now - s_answer_t < 1.5;
    if (s_phase.load() == PH_WANT_OPEN) { s_phase = PH_SHOWING; s_open_t = now; s_scroll = true; }
    else if (s_phase.load() == PH_WANT_CLOSE && !held) { s_phase = PH_IDLE; s_rows.clear(); }
    if (s_md.load() == MD_WANT_OPEN) { s_md = MD_SHOWING; s_md_open_t = now; }
    else if (s_md.load() == MD_WANT_CLOSE && !held) s_md = MD_IDLE;
    const bool want = s_phase.load() != PH_IDLE || s_md.load() != MD_IDLE;
    if (want && !s_win) { s_win = true; return 1; }
    if (!want && s_win) { s_win = false; return 2; }
    return 0;
}

extern "C" void vk_overlay_saves_back(void) { s_back.store(1); }

/* The controller (port 0 and the others by position, as the menu reads it; plus
 * scripted presses), once per drawn frame before the frame: edges and a repeat. */
extern "C" void vk_overlay_saves_pad(uint32_t b, int lx, int ly)
{
    uint32_t dirs = b & (SD_UP | SD_DOWN | SD_LEFT | SD_RIGHT);
    if (ly < 64) dirs |= SD_UP; else if (ly > 192) dirs |= SD_DOWN;
    if (lx < 64) dirs |= SD_LEFT; else if (lx > 192) dirs |= SD_RIGHT;
    const uint32_t cur = (b & (SD_CROSS | SD_CIRCLE | SD_TRIANGLE)) | dirs;
    const uint32_t pressed = cur & ~s_pad_prev;
    s_pad_prev = cur;
    const double now = sd_now();
    double opened;
    { std::lock_guard<std::mutex> g(s_m); opened = s_md.load() == MD_SHOWING ? s_md_open_t : s_open_t; }
    if ((s_phase.load() != PH_SHOWING && s_md.load() != MD_SHOWING) || now - opened < kGrace) {
        s_rep_dir = 0;   /* held from before it opened: no press */
        return;
    }
    if (pressed & SD_CROSS) s_act |= A_OK;
    if (pressed & SD_CIRCLE) s_act |= A_BACK;
    if (pressed & SD_TRIANGLE) s_act |= A_DEL;
    const int dir = (dirs & SD_UP) ? A_UP : (dirs & SD_DOWN) ? A_DOWN : (dirs & (SD_LEFT | SD_RIGHT)) ? A_SIDE : 0;
    if (!dir) s_rep_dir = 0;
    else if (dir != s_rep_dir) { s_rep_dir = dir; s_rep_t = now + 0.40; s_act |= (unsigned)dir; }
    else if (now >= s_rep_t) { s_rep_t = now + 0.11; if (dir != A_SIDE) s_act |= (unsigned)dir; }
}

extern "C" void vk_overlay_saves_attach(void)
{
    { std::lock_guard<std::mutex> g(s_m); s_shutdown = false; }
    const char* e = getenv("PS3_SAVEDATA_DIALOG");
    if (e && e[0] == '0') {
        fprintf(stderr, "[savedlg] PS3_SAVEDATA_DIALOG=0: save and load lists pick by themselves\n");
    } else {
        g_ps3_savedlg.close = sd_close;
        g_ps3_savedlg.show = sd_show;
    }
    e = getenv("PS3_MSGDIALOG");
    if (e && e[0] == '0') {
        fprintf(stderr, "[msgdlg] PS3_MSGDIALOG=0: message dialogs are answered by themselves\n");
    } else {
        g_ps3_msgdlg.close = md_close;
        g_ps3_msgdlg.notice = md_notice;
        g_ps3_msgdlg.open = md_open;
    }
}

/* After the ImGui context is gone (vk_overlay_shutdown). */
extern "C" void vk_overlay_saves_shutdown(void)
{
    g_ps3_savedlg.show = NULL;
    g_ps3_savedlg.close = NULL;
    g_ps3_msgdlg.open = NULL;
    g_ps3_msgdlg.close = NULL;
    g_ps3_msgdlg.notice = NULL;
    ps3_msgdlg_answer_fn fire = NULL;
    unsigned fire_id = 0;
    {
        std::lock_guard<std::mutex> g(s_m);
        s_shutdown = true;
        s_phase = PH_IDLE;
        s_cv.notify_all();
        /* a question still up: answered as it was before there was a window */
        fire = s_md_answer; fire_id = s_md_id;
        s_md_answer = NULL;
        s_md = MD_IDLE;
        s_win = false;
    }
    if (fire) {
        fprintf(stderr, "[msgdlg] %u: the window went away, answered %s\n", fire_id, s_md_buttons == PS3_MSGDLG_OK ? "OK" : "YES");
        fire(fire_id, s_md_buttons == PS3_MSGDLG_OK ? MD_BTN_OK : MD_BTN_YES);
    }
    std::lock_guard<std::mutex> g(s_icon_lock);
    for (int i = 0; i < s_n_icon; i++) delete s_icon[i].tex;
    s_n_icon = 0;
}

/* ---- drawing (render thread, inside the overlay's ImGui frame) ---------------- */
/* the theme's colours (vk_overlay_theme.h) */
static ImU32 col_text(void) { return vk_ov_u32(vk_ov_theme().text); }
static ImU32 col_dim(void) { return vk_ov_u32(vk_ov_theme().text_dim); }
static ImU32 col_accent(void) { return vk_ov_u32(vk_ov_theme().accent); }

/* The PlayStation face-button shapes, `r` = radius. */
static void glyph(ImDrawList* dl, ImVec2 c, float r, int which)
{
    const float t = r * 0.22f;
    switch (which) {
    case 0: {   /* Cross */
        const ImU32 col = IM_COL32(124, 178, 232, 255);
        dl->AddLine(ImVec2(c.x - r * 0.7f, c.y - r * 0.7f), ImVec2(c.x + r * 0.7f, c.y + r * 0.7f), col, t);
        dl->AddLine(ImVec2(c.x - r * 0.7f, c.y + r * 0.7f), ImVec2(c.x + r * 0.7f, c.y - r * 0.7f), col, t);
        break; }
    case 1: dl->AddCircle(c, r * 0.75f, IM_COL32(255, 102, 102, 255), 0, t); break;   /* Circle */
    case 2: dl->AddTriangle(ImVec2(c.x, c.y - r * 0.8f), ImVec2(c.x + r * 0.8f, c.y + r * 0.6f),   /* Triangle */
                            ImVec2(c.x - r * 0.8f, c.y + r * 0.6f), IM_COL32(64, 226, 160, 255), t); break;
    default: break;
    }
}

/* A clickable "<glyph> label  (key)" in the footer. */
static bool hint_button(const char* id, int which, const char* label, const char* key, bool enabled, float k)
{
    ImFont* f = ImGui::GetFont();
    const float fs = ImGui::GetFontSize();
    const ImVec2 ls = f->CalcTextSizeA(fs, FLT_MAX, 0.0f, label);
    const ImVec2 ks = f->CalcTextSizeA(fs * 0.85f, FLT_MAX, 0.0f, key);
    const float gw = fs * 1.2f, gap = 6.0f * k;
    const ImVec2 size(gw + gap + ls.x + gap + ks.x + 8.0f * k, fs * 1.6f);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton(id, size) && enabled;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (enabled && ImGui::IsItemHovered())
        dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), IM_COL32(255, 255, 255, 18), 5.0f * k);
    const float cy = p.y + size.y * 0.5f;
    if (enabled) glyph(dl, ImVec2(p.x + gw * 0.5f + 4.0f * k, cy), fs * 0.5f, which);
    dl->AddText(f, fs, ImVec2(p.x + gw + gap, cy - ls.y * 0.5f), enabled ? col_text() : IM_COL32(110, 116, 128, 255), label);
    dl->AddText(f, fs * 0.85f, ImVec2(p.x + gw + gap + ls.x + gap, cy - ks.y * 0.5f), col_dim(), key);
    return clicked;
}

static void fmt_time(long long t, char* out, size_t n)
{
    out[0] = 0;
    if (t <= 0) return;
    const time_t tt = (time_t)t;
    struct tm lt;
#ifdef _WIN32
    if (localtime_s(&lt, &tt) != 0) return;
#else
    if (!localtime_r(&tt, &lt)) return;
#endif
    strftime(out, n, "%Y-%m-%d  %H:%M", &lt);
}

static void draw_row(int i, const SdRow& r, float k, bool live, double now, unsigned* act)
{
    const float pad = 8.0f * k, icon_w = 160.0f * k, icon_h = 88.0f * k, row_h = icon_h + 2.0f * pad;
    ImFont* f = ImGui::GetFont();
    const float fs = ImGui::GetFontSize();
    ImGui::PushID(i);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    ImGui::InvisibleButton("row", ImVec2(w > 1.0f ? w : 1.0f, row_h));
    const bool hov = ImGui::IsItemHovered();
    if (live && !s_confirm && ImGui::IsItemClicked(0) && now - s_open_t >= kGrace) {
        if (s_cur == i) *act |= A_OK;   /* a click on the selected row chooses it */
        else s_cur = i;
    }
    if (i == s_cur && s_scroll) { ImGui::SetScrollHereY(0.5f); s_scroll = false; }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p1(p0.x + w, p0.y + row_h);
    if (i == s_cur) {
        const VkOvTheme& th = vk_ov_theme();   /* the selected row: lit, with a soft glow round its edge */
        dl->AddRect(ImVec2(p0.x - 2.0f * k, p0.y - 2.0f * k), ImVec2(p1.x + 2.0f * k, p1.y + 2.0f * k),
                    vk_ov_u32(th.accent, 0.28f), 8.0f * k, 0, 3.0f * k);
        dl->AddRectFilled(p0, p1, vk_ov_u32(th.sel_bg), 6.0f * k);
        dl->AddRect(p0, p1, vk_ov_u32(th.accent_hi), 6.0f * k, 0, 2.0f * k);
    } else if (hov) {
        dl->AddRectFilled(p0, p1, IM_COL32(255, 255, 255, 14), 6.0f * k);
    }
    /* the thumbnail (ICON0.PNG is 320x176 on the console) */
    const ImVec2 i0(p0.x + pad, p0.y + pad), i1(p0.x + pad + icon_w, p0.y + pad + icon_h);
    if (ImTextureData* t = icon_tex(r.icon)) {
        dl->AddImage(t->GetTexRef(), i0, i1);
    } else {
        dl->AddRectFilled(i0, i1, IM_COL32(18, 24, 32, 255), 4.0f * k);
        dl->AddRect(i0, i1, vk_ov_u32(vk_ov_theme().accent, 0.35f), 4.0f * k, 0, 1.0f * k);
        if (r.is_new) {   /* a plus */
            const ImVec2 c((i0.x + i1.x) * 0.5f, (i0.y + i1.y) * 0.5f);
            const float a = icon_h * 0.22f;
            dl->AddLine(ImVec2(c.x - a, c.y), ImVec2(c.x + a, c.y), col_accent(), 3.0f * k);
            dl->AddLine(ImVec2(c.x, c.y - a), ImVec2(c.x, c.y + a), col_accent(), 3.0f * k);
        }
    }
    /* the text */
    const float tx = i1.x + 14.0f * k;
    float ty = p0.y + pad - 1.0f * k;
    dl->PushClipRect(ImVec2(tx, p0.y), ImVec2(p1.x - pad, p1.y), true);
    const float big = fs * 1.25f, small = fs * 0.85f;
    char when[40];
    fmt_time(r.is_new ? 0 : r.mtime, when, sizeof when);
    float date_w = 0.0f;
    if (when[0]) {
        date_w = f->CalcTextSizeA(small, FLT_MAX, 0.0f, when).x;
        dl->AddText(f, small, ImVec2(p1.x - pad - date_w, ty + (big - small) * 0.5f), col_dim(), when);
    }
    dl->PushClipRect(ImVec2(tx, p0.y), ImVec2(p1.x - pad - date_w - 12.0f * k, p1.y), true);
    dl->AddText(f, big, ImVec2(tx, ty), col_text(), row_name(r));
    dl->PopClipRect();
    ty += big + 3.0f * k;
    const char* d = r.is_new ? (r.title.empty() ? "Make a new save." : r.title.c_str()) : r.detail.c_str();
    for (int line = 0; line < 4 && *d && ty + small <= p1.y - 2.0f * k; line++) {
        const char* e = strchr(d, '\n');
        if (!e) e = d + strlen(d);
        if (e > d) dl->AddText(f, small, ImVec2(tx, ty), col_dim(), d, e);
        ty += small * 1.12f;
        d = *e ? e + 1 : e;
    }
    dl->PopClipRect();
    ImGui::PopID();
}

/* The yes/no question over the list. */
static void draw_confirm(float k, unsigned* act)
{
    const ImGuiIO& io = ImGui::GetIO();
    const SdRow* r = s_cur >= 0 && s_cur < (int)s_rows.size() ? &s_rows[s_cur] : NULL;
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(480.0f * k, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowFocus();
    ImGui::PushStyleColor(ImGuiCol_WindowBg, vk_ov_theme().panel_hi);
    ImGui::PushStyleColor(ImGuiCol_Border, vk_ov_theme().accent);
    if (ImGui::Begin("##savedlg_confirm", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoNav |
                                                ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove)) {
        ImGui::PushFont(vk_ov_theme_bold(), ImGui::GetStyle().FontSizeBase * 1.2f);
        ImGui::TextUnformatted(s_confirm == 1 ? "Overwrite this save?" : "Delete this save?");
        ImGui::PopFont();
        if (r) {
            char when[40];
            fmt_time(r->mtime, when, sizeof when);
            ImGui::PushStyleColor(ImGuiCol_Text, vk_ov_theme().text_dim);
            ImGui::TextUnformatted(row_name(*r));
            if (when[0]) ImGui::TextUnformatted(when);
            ImGui::PopStyleColor();
        }
        ImGui::Spacing();
        ImGui::TextUnformatted(s_confirm == 1 ? "The game saved in it will be replaced." : "It cannot be brought back.");
        ImGui::Spacing();
        const ImVec2 bs(140.0f * k, 0.0f);
        for (int b = 0; b < 2; b++) {   /* Yes, No: the highlighted one is where the pad is */
            const bool hl = (b == 0) == s_yes;
            if (b) ImGui::SameLine();
            if (vk_ov_choice(b == 0 ? "Yes" : "No", hl, bs)) *act |= b == 0 ? A_YES : A_NO;
        }
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, vk_ov_theme().text_dim);
        ImGui::TextUnformatted("Left / Right: choose    Cross / Enter: confirm    Circle / Esc: back");
        ImGui::PopStyleColor();
    }
    ImGui::End();
    ImGui::PopStyleColor(2);
}

/* The save list (s_m held). `blocked`: a message dialog is over it and has the input. */
static void draw_list(float k, unsigned act, double now, bool blocked)
{
    if (s_seen_gen != s_gen) { s_seen_gen = s_gen; s_confirm = 0; s_scroll = true; }
    s_drawn = true;
    const ImGuiIO& io = ImGui::GetIO();
    const bool live = s_waiting && !s_answered && s_phase.load() == PH_SHOWING && !blocked;
    const int n = (int)s_rows.size();
    if (s_cur >= n) s_cur = n ? n - 1 : 0;

    /* the game dimmed behind it */
    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(0, 0, 0, 150));
    const ImVec2 want(860.0f * k, 600.0f * k);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(want.x < io.DisplaySize.x - 20 ? want.x : io.DisplaySize.x - 20,
                                    want.y < io.DisplaySize.y - 20 ? want.y : io.DisplaySize.y - 20), ImGuiCond_Always);
    if (!s_confirm) ImGui::SetNextWindowFocus();
    const char* title = s_is_save ? "Save Game###ps3savedlg" : "Load Game###ps3savedlg";
    if (vk_ov_begin(title, NULL, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                                  ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav)) {
        /* the game's name and how many saves */
        std::string game;
        int saves = 0;
        for (const SdRow& r : s_rows) { if (!r.is_new) saves++; if (game.empty() && !r.title.empty() && !r.is_new) game = r.title; }
        ImGui::PushStyleColor(ImGuiCol_Text, vk_ov_theme().text_dim);
        ImGui::Text("%s%s%d saved game%s", game.c_str(), game.empty() ? "" : "    ", saves, saves == 1 ? "" : "s");
        ImGui::PopStyleColor();
        const float footer = ImGui::GetFontSize() * 1.6f + ImGui::GetStyle().ItemSpacing.y * 2.0f +
                             (s_msg.empty() ? 0.0f : ImGui::GetTextLineHeightWithSpacing()) + 6.0f * k;
        ImGui::BeginChild("##savedlg_list", ImVec2(0, -footer), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoNav);
        if (n == 0) {
            const char* t = "There is no saved data.";
            const ImVec2 ts = ImGui::CalcTextSize(t);
            const ImVec2 av = ImGui::GetContentRegionAvail();
            ImGui::SetCursorPos(ImVec2((av.x - ts.x) * 0.5f, (av.y - ts.y) * 0.45f));
            ImGui::TextUnformatted(t);
        }
        for (int i = 0; i < n; i++) draw_row(i, s_rows[i], k, live, now, &act);
        ImGui::EndChild();
        if (!s_msg.empty()) ImGui::TextColored(vk_ov_theme().warn, "%s", s_msg.c_str());
        /* the footer: what each button does (each is clickable too) */
        const bool can_pick = live && !s_confirm && n > 0;
        const bool can_del = live && !s_confirm && n > 0 && !s_rows[s_cur].is_new;
        const char* ok = !n ? "Select" : !s_is_save ? "Load" : s_rows[s_cur].is_new ? "Save" : "Overwrite";
        if (!live && s_answered && s_action == PS3_SAVEDLG_DELETE) {
            ImGui::TextUnformatted("Deleting...");
        } else {
            if (hint_button("##ok", 0, ok, "Enter", can_pick, k) && now - s_open_t >= kGrace) act |= A_OK;
            ImGui::SameLine(0.0f, 18.0f * k);
            if (hint_button("##del", 2, "Delete", "Del", can_del, k) && now - s_open_t >= kGrace) act |= A_DEL;
            ImGui::SameLine(0.0f, 18.0f * k);
            if (hint_button("##back", 1, "Back", "Esc", live && !s_confirm, k) && now - s_open_t >= kGrace) act |= A_BACK;
        }
    }
    ImGui::End();
    if (s_confirm) draw_confirm(k, &act);

    /* act on it */
    if (!live) return;
    if (now - s_open_t < kGrace) act &= ~(unsigned)(A_OK | A_BACK | A_DEL | A_YES | A_NO);   /* still held from before */
    if (s_confirm) {
        if (act & A_YES) { s_yes = true; act |= A_OK; }
        if (act & A_NO) { s_yes = false; act |= A_OK; }
        if (act & (A_UP | A_DOWN | A_SIDE)) s_yes = !s_yes;
        if (act & A_BACK) {
            fprintf(stderr, "[savedlg] %s? no (back)\n", s_confirm == 1 ? "overwrite" : "delete");
            s_confirm = 0;
        } else if (act & A_OK) {
            fprintf(stderr, "[savedlg] %s? %s\n", s_confirm == 1 ? "overwrite" : "delete", s_yes ? "yes" : "no");
            if (s_yes) answer(s_confirm == 1 ? PS3_SAVEDLG_PICK : PS3_SAVEDLG_DELETE, s_cur);
            else s_confirm = 0;
        }
        return;
    }
    if ((act & A_UP) && s_cur > 0) { s_cur--; s_scroll = true; }
    if ((act & A_DOWN) && s_cur < n - 1) { s_cur++; s_scroll = true; }
    if (act & A_BACK) {
        answer(PS3_SAVEDLG_CANCEL, -1);
    } else if ((act & A_OK) && n > 0) {
        if (s_is_save && !s_rows[s_cur].is_new) {
            s_confirm = 1; s_yes = false;
            fprintf(stderr, "[savedlg] overwrite? asked for %d '%s'\n", s_cur, s_rows[s_cur].dir.c_str());
        } else {
            answer(PS3_SAVEDLG_PICK, s_cur);
        }
    } else if ((act & A_DEL) && n > 0 && !s_rows[s_cur].is_new) {
        s_confirm = 2; s_yes = false;
        fprintf(stderr, "[savedlg] delete? asked for %d '%s'\n", s_cur, s_rows[s_cur].dir.c_str());
    }
}

/* The message dialog (s_m held): the title's sentence and Yes/No or OK. The
 * button chosen is handed back in *fire_* so the caller reports it after
 * letting go of s_m (cellMsgDialog takes its own lock). */
static void draw_msg(float k, unsigned act, double now, ps3_msgdlg_answer_fn* fire, unsigned* fire_id, int* fire_btn)
{
    const ImGuiIO& io = ImGui::GetIO();
    const bool live = s_md.load() == MD_SHOWING && s_md_answer != NULL;
    const bool yesno = s_md_buttons == PS3_MSGDLG_YESNO;
    const float w = 620.0f * k < io.DisplaySize.x - 20.0f ? 620.0f * k : io.DisplaySize.x - 20.0f;
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(w, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowFocus();
    ImGui::PushStyleColor(ImGuiCol_WindowBg, vk_ov_theme().panel_hi);
    ImGui::PushStyleColor(ImGuiCol_Border, s_md_error ? vk_ov_theme().warn : vk_ov_theme().accent);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(26.0f * k, 22.0f * k));
    if (ImGui::Begin("##msgdlg", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoNav |
                                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove)) {
        ImGui::PushFont(NULL, ImGui::GetStyle().FontSizeBase * 1.15f);
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(s_md_text.c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopFont();
        ImGui::Dummy(ImVec2(1.0f, 12.0f * k));
        /* the buttons, centred; the highlighted one is where the pad is */
        const float bw = 150.0f * k, gap = ImGui::GetStyle().ItemSpacing.x;
        const float total = yesno ? bw * 2.0f + gap : bw;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (ImGui::GetContentRegionAvail().x - total) * 0.5f);
        for (int b = 0; b < (yesno ? 2 : 1); b++) {
            const bool hl = !yesno || ((b == 0) == s_md_yes);
            if (b) ImGui::SameLine();
            if (vk_ov_choice(!yesno ? "OK" : b == 0 ? "Yes" : "No", hl, ImVec2(bw, 0.0f)) && live)
                act |= (!yesno || b == 0) ? A_YES : A_NO;
        }
        ImGui::Dummy(ImVec2(1.0f, 6.0f * k));
        ImGui::PushStyleColor(ImGuiCol_Text, vk_ov_theme().text_dim);
        ImGui::TextUnformatted(yesno ? (s_md_cancel ? "Left / Right: choose    Cross / Enter: confirm    Circle / Esc: back"
                                                    : "Left / Right: choose    Cross / Enter: confirm")
                                     : (s_md_cancel ? "Cross / Enter: OK    Circle / Esc: back" : "Cross / Enter: OK"));
        ImGui::PopStyleColor();
    }
    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);

    if (!live) return;
    if (now - s_md_open_t < kGrace) return;   /* a press still held from before it opened */
    int btn = 0;
    if (act & A_YES) btn = yesno ? MD_BTN_YES : MD_BTN_OK;
    else if (act & A_NO) btn = MD_BTN_NO;
    else if (act & A_BACK) { if (s_md_cancel) btn = MD_BTN_ESCAPE; }
    else if (act & A_OK) btn = !yesno ? MD_BTN_OK : s_md_yes ? MD_BTN_YES : MD_BTN_NO;
    else if (yesno && (act & (A_UP | A_DOWN | A_SIDE))) s_md_yes = !s_md_yes;
    if (!btn) return;
    fprintf(stderr, "[msgdlg] %u answered %s\n", s_md_id,
            btn == MD_BTN_ESCAPE ? "ESCAPE (back)" : !yesno ? "OK" : btn == MD_BTN_YES ? "YES" : "NO");
    *fire = s_md_answer; *fire_id = s_md_id; *fire_btn = btn;
    s_md_answer = NULL;
    s_md = MD_WANT_CLOSE;
    s_answer_t = now;
}

extern "C" void vk_overlay_saves_draw(float k)
{
    ps3_msgdlg_answer_fn fire = NULL;
    unsigned fire_id = 0;
    int fire_btn = 0;
    {
        std::unique_lock<std::mutex> lk(s_m);
        const double now = sd_now();
        /* what the player did: the controller (since the last frame), keys, Esc */
        unsigned act = s_act;
        s_act = 0;
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, true)) act |= A_UP;
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, true)) act |= A_DOWN;
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, false) || ImGui::IsKeyPressed(ImGuiKey_RightArrow, false) ||
            ImGui::IsKeyPressed(ImGuiKey_Tab, false)) act |= A_SIDE;
        if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false) ||
            ImGui::IsKeyPressed(ImGuiKey_Space, false)) act |= A_OK;
        if (ImGui::IsKeyPressed(ImGuiKey_Delete, false)) act |= A_DEL;
        if (ImGui::IsKeyPressed(ImGuiKey_Backspace, false)) act |= A_BACK;
        if (s_back.exchange(0)) act |= A_BACK;
        s_keys_held = ImGui::IsKeyDown(ImGuiKey_Enter) || ImGui::IsKeyDown(ImGuiKey_KeypadEnter) ||
                      ImGui::IsKeyDown(ImGuiKey_Space) || ImGui::IsKeyDown(ImGuiKey_Delete) ||
                      ImGui::IsKeyDown(ImGuiKey_Backspace);
        const bool md = s_md.load() != MD_IDLE;
        if (s_phase.load() != PH_IDLE) draw_list(k, md ? 0u : act, now, md);
        else ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), ImGui::GetIO().DisplaySize, IM_COL32(0, 0, 0, 110));
        if (md) draw_msg(k, act, now, &fire, &fire_id, &fire_btn);
    }
    if (fire) fire(fire_id, fire_btn);   /* cellMsgDialog queues the title's callback */
}
