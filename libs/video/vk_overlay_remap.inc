/*
 * vk_overlay_remap.inc -- F2 > Controls: the bindings table, rebinding live
 * (GitHub issue #6). Included by vk_overlay.cpp (it uses that file's hint,
 * section, S, kGrey/kOrange, s_n_pads, PAD_* and the notices).
 *
 * The rules -- what a press is, the keys the window keeps, one input per PS3
 * control, Swap -- are libs/input/pad_remap.h. cellPad applies a new map at
 * once (cellPad_host_set_input_map: the next poll reads it; the prompt glyphs,
 * rsx_button_icons, hash the map every frame and redraw) and writes it into
 * the player's input.toml (cellPad_host_save_input_map -> pim_save_file: the
 * file's comments and other sections kept; a temporary file moved over it).
 *
 * A change: click a cell (or Cross on it) -> a dialog waits for the press.
 * While it waits, every key the window gets comes here (vk_overlay_event), the
 * frame's queued key presses, mouse presses and wheel turns are taken before
 * ImGui (remap_event; releases, and clicks on the dialog's Clear / Cancel,
 * still reach it), a controller press is read from g_pad_overlay_raw, and the
 * menu gets no controller input (feed_pad -> remap_pad_hold). Esc, Cancel,
 * Circle (for a key) or 10 s without a press cancel. An input another control
 * has -> "Swap / Cancel".
 *
 * Test hook (the timed script, PS3RECOMP_OVERLAY_MENU_AT): "bind=<control>=<input>"
 * binds as the page would (the control's keys become that one key; a mouse or
 * controller input replaces that column) and saves; "bind=reset" = Reset.
 */
extern "C" {
void cellPad_host_set_input_map(const PadInputMap* m);
int  cellPad_host_save_input_map(void);
void cellPad_host_input_map_path(char* out, size_t n);
}

enum { RM_IDLE, RM_CAPTURE, RM_CONFLICT, RM_RESET };
enum { RM_SLOT_KEY0, RM_SLOT_KEY1, RM_SLOT_MOUSE, RM_SLOT_PAD };

static struct {
    int      state;
    int      ctl, slot;          /* the control and the cell being changed */
    double   t0, drawn_t;        /* the capture's start; the page's last frame */
    bool     open_cap, open_swap, open_reset;   /* OpenPopup at the page's level */
    /* the capture, filled from the frame's events (remap_event) and the controller */
    bool     have, cancel;
    PrmInput got;
    const char* why;             /* that press is not bindable: why */
    uint32_t pad_prev;           /* controller inputs down at the last look */
    int      swallow_scancode;   /* a captured key: its repeats and release stay out of ImGui */
    bool     pad_release;        /* after a controller capture: no menu pad input until all are up */
    uint16_t nav_prev;
    ImVec2   btn_min[2], btn_max[2];   /* the dialog's Clear and Cancel (clicks there are ImGui's) */
    /* the conflict being asked about */
    PrmInput pend;
    int      pend_kslot, pend_old;
    uint32_t owners;
    bool     modal_cancel;       /* Esc / Circle on the Swap or Reset dialog */
    int      order[PIM_N_CONTROLS][2];   /* what the key cells showed: a key stays in its cell */
    bool     order_init;
    char     status[400];
    bool     status_bad;
} s_rm;

static void remap_order_reset(void)
{
    for (int c = 0; c < PIM_N_CONTROLS; c++) s_rm.order[c][0] = s_rm.order[c][1] = -1;
    s_rm.order_init = true;
}

/* Still on screen? A wait whose page is gone (the save list over the menu)
 * ends: its keys must not vanish. (2 s: a loading screen's slow frames are
 * not "gone".) */
static bool remap_live(void)
{
    if (s_rm.state == RM_IDLE) return false;
    if (now_s() - s_rm.drawn_t > 2.0) { s_rm.state = RM_IDLE; s_rm.pad_release = false; return false; }
    return true;
}
static bool remap_capturing(void) { return remap_live() && s_rm.state == RM_CAPTURE; }
static void remap_cancel(void) { s_rm.state = RM_IDLE; s_rm.pad_release = false; s_rm.swallow_scancode = 0; }
/* Esc while a Swap / Reset dialog is up: that dialog's Cancel. */
static bool remap_escape(void)
{
    if (!remap_live() || (s_rm.state != RM_CONFLICT && s_rm.state != RM_RESET)) return false;
    s_rm.modal_cancel = true;
    return true;
}

/* The frame's queued events, before ImGui: true = taken. */
static bool remap_event(const SDL_Event* e)
{
    if ((e->type == SDL_KEYDOWN || e->type == SDL_KEYUP) && s_rm.swallow_scancode &&
        (int)e->key.keysym.scancode == s_rm.swallow_scancode) {
        if (e->type == SDL_KEYUP) s_rm.swallow_scancode = 0;   /* (a held Enter must not press Swap) */
        return true;
    }
    if (!remap_capturing()) return false;
    switch (e->type) {
    case SDL_KEYDOWN: {
        if (e->key.repeat || s_rm.have || s_rm.cancel) return true;
        PrmInput in = { -1, 0 };
        const char* why = NULL;
        const int r = prm_capture_key((int)e->key.keysym.scancode, (e->key.keysym.mod & KMOD_ALT) != 0, &in, &why);
        if (r == PRM_CAP_CANCEL) s_rm.cancel = true;
        else if (r != PRM_CAP_INPUT) s_rm.why = why;
        else if (s_rm.slot == RM_SLOT_PAD) s_rm.why = "This cell takes a controller button (Esc cancels).";
        else { s_rm.got = in; s_rm.have = true; s_rm.swallow_scancode = (int)e->key.keysym.scancode; }
        return true; }
    case SDL_KEYUP:
        return false;                            /* releases are ImGui's (the Enter that opened this wait) */
    case SDL_TEXTINPUT: case SDL_TEXTEDITING:
        return true;
    case SDL_MOUSEBUTTONDOWN: {
        const float x = (float)e->button.x, y = (float)e->button.y;
        for (int i = 0; i < 2; i++)
            if (x >= s_rm.btn_min[i].x && x < s_rm.btn_max[i].x && y >= s_rm.btn_min[i].y && y < s_rm.btn_max[i].y)
                return false;                    /* Clear / Cancel */
        if (s_rm.have || s_rm.cancel) return true;
        if (s_rm.slot == RM_SLOT_PAD) { s_rm.why = "This cell takes a controller button (Esc or Cancel to stop)."; return true; }
        const uint32_t bit = prm_mouse_from_sdl(e->button.button);
        if (bit) { s_rm.got.kind = PRM_MOUSE; s_rm.got.code = bit; s_rm.have = true; }
        else s_rm.why = "The game cannot read this mouse button.";
        return true; }
    case SDL_MOUSEBUTTONUP:
        return false;                            /* finishes a Clear / Cancel click; harmless otherwise */
    case SDL_MOUSEWHEEL: {
        if (s_rm.have || s_rm.cancel || s_rm.slot == RM_SLOT_PAD) return true;
        int wy = e->wheel.y;
        if (e->wheel.direction == SDL_MOUSEWHEEL_FLIPPED) wy = -wy;   /* cellPad reads the wheel unflipped */
        const uint32_t bit = prm_mouse_from_wheel(wy);
        if (bit) { s_rm.got.kind = PRM_MOUSE; s_rm.got.code = bit; s_rm.have = true; }
        return true; }
    default:
        return false;
    }
}

/* feed_pad: true = the menu gets no controller input this frame (a press is
 * awaited; or one was just bound and is still held). Circle cancels a wait
 * for a key and the Swap / Reset dialogs. */
static bool remap_pad_hold(uint16_t b)
{
    const uint16_t pressed = b & (uint16_t)~s_rm.nav_prev;
    s_rm.nav_prev = b;
    if (s_rm.pad_release) {
        if (b || (g_pad_overlay_raw & PRM_PAD_CAPTURE_MASK)) return true;
        s_rm.pad_release = false;
    }
    if (!remap_live()) return false;
    /* (a Circle that cancels is still down next frame: the menu would take it as its own
     * Circle and close -> held out until every button is up) */
    if (s_rm.state == RM_CAPTURE) {
        if (s_rm.slot != RM_SLOT_PAD && (pressed & PAD_CIRCLE)) { s_rm.cancel = true; s_rm.pad_release = true; }
        return true;
    }
    if (pressed & PAD_CIRCLE) { s_rm.modal_cancel = true; s_rm.pad_release = true; return true; }
    return false;
}

/* The new map: live now, then into input.toml. */
static void remap_commit(const PadInputMap& m, const char* what)
{
    cellPad_host_set_input_map(&m);
    const int r = cellPad_host_save_input_map();
    char path[600];
    cellPad_host_input_map_path(path, sizeof path);
    s_rm.status_bad = r < 0;
    if (r > 0)       snprintf(s_rm.status, sizeof s_rm.status, "%s. Saved to %s", what, path);
    else if (r == 0) snprintf(s_rm.status, sizeof s_rm.status, "%s. For this session only: PS3_INPUT_MAP is empty, nothing is saved.", what);
    else             snprintf(s_rm.status, sizeof s_rm.status, "%s, but %s could not be written: it applies until the game closes.", what, path);
    fprintf(stderr, "[overlay] controls: %s\n", s_rm.status);
    vk_overlay_notice(r < 0 ? "Controls changed, but input.toml could not be written" : what);
}

static void remap_script(const char* spec)
{
    PadInputMap m;
    cellPad_host_input_map(&m);
    char s[96];
    snprintf(s, sizeof s, "%s", spec ? spec : "");
    if (!SDL_strcasecmp(s, "reset")) { prm_reset(&m); remap_order_reset(); remap_commit(m, "Bindings reset to the defaults"); return; }
    char* eq = strchr(s, '=');
    if (eq) *eq = 0;
    pim_lower(s);
    const int ctl = pim_lookup(pim_controls, PIM_COUNT(pim_controls), s);
    PrmInput in = { -1, 0 };
    if (!eq || ctl < 0 || !prm_parse_input(eq + 1, &in)) { fprintf(stderr, "[overlay] bind=%s: not understood\n", spec); return; }
    if (in.kind == PRM_KEY) prm_clear_keys(&m, ctl);   /* the control's keys become this one */
    const uint32_t owners = prm_assign(&m, ctl, in, -1);
    char a[24], what[160];
    snprintf(what, sizeof what, "%s: %s (taken from 0x%X)", prm_row_of(ctl)->name, prm_input_name(in, a, sizeof a), (unsigned)owners);
    remap_commit(m, what);
}

/* The names of the controls in `mask`: "R1 (Bolt, use)", "R1 and L2". */
static void remap_names(uint32_t mask, char* out, size_t n)
{
    out[0] = 0;
    int k = 0, total = 0;
    for (int i = 0; i < PIM_N_CONTROLS; i++) if (mask >> prm_rows[i].ctl & 1u) total++;
    for (int i = 0; i < PIM_N_CONTROLS; i++) {
        if (!(mask >> prm_rows[i].ctl & 1u)) continue;
        const size_t len = strlen(out);
        const char* sep = k == 0 ? "" : k == total - 1 ? " and " : ", ";
        if (total == 1) snprintf(out + len, n - len, "%s (%s)", prm_rows[i].name, prm_rows[i].does);
        else snprintf(out + len, n - len, "%s%s", sep, prm_rows[i].name);
        k++;
    }
}

/* What a mouse / controller cell shows: the first input, "+N" for more. */
static void remap_bits_label(uint32_t bits, int kind, char* out, size_t n)
{
    int cnt = 0, first = -1;
    for (int b = 0; b < 32; b++) if (bits >> b & 1u) { if (first < 0) first = b; cnt++; }
    if (first < 0) { snprintf(out, n, "-"); return; }
    char buf[24];
    PrmInput in = { kind, kind == PRM_MOUSE ? 1u << first : (uint32_t)first };
    const char* name = prm_input_name(in, buf, sizeof buf);
    if (cnt == 1) snprintf(out, n, "%s", name);
    else if (cnt == 2 && kind == PRM_PAD) {
        int second = -1;
        for (int b = first + 1; b < 32; b++) if (bits >> b & 1u) { second = b; break; }
        snprintf(out, n, "%s, %s", name, prm_pad_name((uint32_t)second));
    }
    else snprintf(out, n, "%s +%d", name, cnt - 1);
}

static void remap_begin(int ctl, int slot, double now)
{
    s_rm.state = RM_CAPTURE;
    s_rm.ctl = ctl; s_rm.slot = slot;
    s_rm.t0 = now;
    s_rm.have = s_rm.cancel = false;
    s_rm.why = NULL;
    s_rm.pad_prev = g_pad_overlay_raw;          /* held now (the Cross that clicked) = not a press */
    s_rm.btn_min[0] = s_rm.btn_max[0] = s_rm.btn_min[1] = s_rm.btn_max[1] = ImVec2(0.0f, 0.0f);
    s_rm.open_cap = true;
}

/* Put `in` on control ctl now (no conflict, or Swap was chosen). */
static void remap_apply(PadInputMap& m, int ctl, PrmInput in, int kslot, int old_key)
{
    const uint32_t owners = prm_assign(&m, ctl, in, in.kind == PRM_KEY ? old_key : -1);
    char a[24], b[24], who[200], what[300];
    const char* nm = prm_input_name(in, a, sizeof a);
    if (in.kind == PRM_KEY && kslot >= 0 && kslot < 2) {
        const int canon = (int)prm_key_canon(in.code);
        s_rm.order[ctl][kslot] = canon;
        const int f = prm_first(owners);         /* the swapped-in key takes the cell the taken one had */
        if (f >= 0 && old_key >= 0)
            for (int j = 0; j < 2; j++) if (s_rm.order[f][j] == canon) s_rm.order[f][j] = (int)prm_key_canon((uint32_t)old_key);
    }
    if (!owners) snprintf(what, sizeof what, "%s: %s", prm_row_of(ctl)->name, nm);
    else {
        remap_names(owners, who, sizeof who);
        snprintf(what, sizeof what, "%s: %s (taken from %s%s%s)", prm_row_of(ctl)->name, nm, who,
                 in.kind == PRM_KEY && old_key >= 0 ? ", which gets " : "",
                 in.kind == PRM_KEY && old_key >= 0 ? prm_key_name((uint32_t)old_key, b, sizeof b) : "");
    }
    remap_commit(m, what);
}

static void remap_center_next(void)
{
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
}
static const ImGuiWindowFlags kRemapModal = ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings;

/* "Press a key or mouse button": the capture's dialog, and what the press does. */
static void remap_capture_popup(PadInputMap& m, const int slots[2], double now)
{
    if (s_rm.open_cap) { ImGui::OpenPopup("###rmcap"); s_rm.open_cap = false; }
    remap_center_next();
    if (!ImGui::BeginPopupModal("Change a binding###rmcap", NULL, kRemapModal)) return;
    if (s_rm.state != RM_CAPTURE) { ImGui::CloseCurrentPopup(); ImGui::EndPopup(); return; }
    const PrmRow* r = prm_row_of(s_rm.ctl);
    const bool pad = s_rm.slot == RM_SLOT_PAD;
    ImGui::Text("%s  -  %s", r->name, r->does);
    ImGui::Spacing();
    ImGui::TextUnformatted(pad ? "Press a button on the controller." : "Press a key or a mouse button (or turn the wheel).");
    hint(pad ? "Esc cancels." : "Esc cancels (Circle on a controller too).");
    if (pad && !s_n_pads) ImGui::TextColored(kOrange, "No controller is connected.");
    if (s_rm.why) ImGui::TextColored(kOrange, "%s", s_rm.why);
    { const int left = (int)(10.99 - (now - s_rm.t0));
      ImGui::PushStyleColor(ImGuiCol_Text, kGrey);
      ImGui::Text("Closes by itself in %d s.", left > 0 ? left : 0);
      ImGui::PopStyleColor(); }
    ImGui::Spacing();
    const bool clear = ImGui::Button("Clear");
    s_rm.btn_min[0] = ImGui::GetItemRectMin(); s_rm.btn_max[0] = ImGui::GetItemRectMax();
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) s_rm.cancel = true;
    s_rm.btn_min[1] = ImGui::GetItemRectMin(); s_rm.btn_max[1] = ImGui::GetItemRectMax();

    /* a controller press: bits newly down in every pad's raw inputs */
    if (pad && !s_rm.have && !s_rm.cancel) {
        const uint32_t raw = g_pad_overlay_raw;
        const uint32_t newly = raw & ~s_rm.pad_prev;
        s_rm.pad_prev = raw;
        const int bit = prm_pad_from_raw(newly);
        if (bit >= 0) { s_rm.got.kind = PRM_PAD; s_rm.got.code = (uint32_t)bit; s_rm.have = true; }
        else if (newly & (1u << 5)) s_rm.why = "The PS / Xbox / Home button belongs to the system.";
    }

    bool done = false;
    const int ctl = s_rm.ctl;
    const int kslot = s_rm.slot <= RM_SLOT_KEY1 ? s_rm.slot : -1;
    if (clear) {
        char a[24], what[120];
        if (kslot >= 0) {
            if (slots[kslot] >= 0) {
                snprintf(what, sizeof what, "%s: %s cleared", r->name, prm_key_name((uint32_t)slots[kslot], a, sizeof a));
                prm_clear(&m, ctl, PRM_KEY, slots[kslot]);
                s_rm.order[ctl][kslot] = -1;
                remap_commit(m, what);
            }
        } else if (pad ? m.pad[ctl] != 0 : m.mouse[ctl] != 0) {
            prm_clear(&m, ctl, pad ? PRM_PAD : PRM_MOUSE, -1);
            snprintf(what, sizeof what, "%s: no %s input", r->name, pad ? "controller" : "mouse");
            remap_commit(m, what);
        }
        done = true;
    } else if (s_rm.cancel) {
        done = true;
    } else if (now - s_rm.t0 > 10.99) {
        vk_overlay_notice("Nothing pressed: the binding is unchanged");
        done = true;
    } else if (s_rm.have) {
        PrmInput in = s_rm.got;
        s_rm.have = false;
        if (in.kind == PRM_PAD) s_rm.pad_release = true;   /* that button is still down: not the menu's */
        /* a key from the mouse cell: the first empty key cell, else the first */
        const int ks = in.kind != PRM_KEY ? -1 : kslot >= 0 ? kslot : slots[0] < 0 ? 0 : slots[1] < 0 ? 1 : 0;
        const int old_key = ks >= 0 ? slots[ks] : -1;
        if (prm_has(&m, ctl, in)) {
            /* already this control's: nothing to do (the other key cell: say so) */
            if (in.kind == PRM_KEY && old_key >= 0 && (int)prm_key_canon(in.code) != old_key) {
                char a[24], msg[120];
                snprintf(msg, sizeof msg, "%s is already on %s", prm_key_name(in.code, a, sizeof a), r->name);
                vk_overlay_notice(msg);
            }
            done = true;
        } else {
            const uint32_t owners = prm_owners(&m, ctl, in);
            if (owners) {
                s_rm.pend = in; s_rm.pend_kslot = ks; s_rm.pend_old = old_key; s_rm.owners = owners;
                s_rm.modal_cancel = false;
                s_rm.state = RM_CONFLICT;
                s_rm.open_swap = true;
                ImGui::CloseCurrentPopup();
            } else {
                remap_apply(m, ctl, in, ks, old_key);
                done = true;
            }
        }
    }
    if (done) { s_rm.state = RM_IDLE; ImGui::CloseCurrentPopup(); }
    ImGui::EndPopup();
}

/* "E is already used by R1": Swap / Cancel. */
static void remap_swap_popup(PadInputMap& m)
{
    if (s_rm.open_swap) { ImGui::OpenPopup("###rmswap"); s_rm.open_swap = false; }
    remap_center_next();
    if (!ImGui::BeginPopupModal("Already in use###rmswap", NULL, kRemapModal)) return;
    if (s_rm.state != RM_CONFLICT) { ImGui::CloseCurrentPopup(); ImGui::EndPopup(); return; }
    const int ctl = s_rm.ctl;
    const PrmInput in = s_rm.pend;
    char a[24], b[24], who[200];
    const char* nm = prm_input_name(in, a, sizeof a);
    remap_names(s_rm.owners, who, sizeof who);
    ImGui::Text("%s is already used by %s.", nm, who);
    /* what Swap gives the other control: the key in the cell being changed, or
     * the mouse / controller input this control had */
    const int first = prm_first(s_rm.owners);
    char gives[60] = "";
    if (in.kind == PRM_KEY && s_rm.pend_old >= 0) snprintf(gives, sizeof gives, "%s", prm_key_name((uint32_t)s_rm.pend_old, b, sizeof b));
    else if (in.kind == PRM_MOUSE && (m.mouse[ctl] & ~in.code)) remap_bits_label(m.mouse[ctl] & ~in.code, PRM_MOUSE, gives, sizeof gives);
    else if (in.kind == PRM_PAD && (m.pad[ctl] & ~(1u << in.code))) remap_bits_label(m.pad[ctl] & ~(1u << in.code), PRM_PAD, gives, sizeof gives);
    const PrmRow* fr = prm_row_of(first);
    if (gives[0]) ImGui::Text("Swap: %s gets %s instead.", fr->name, gives);
    else ImGui::Text("Swap: %s goes to %s, and %s is left without it.", nm, prm_row_of(ctl)->name, fr->name);
    if (s_rm.owners & ~(1u << first)) hint("(The other controls just lose it.)");
    ImGui::Spacing();
    const bool swap = ImGui::Button("Swap");
    ImGui::SameLine();
    const bool cancel = ImGui::Button("Cancel");
    ImGui::SetItemDefaultFocus();                /* a stray Cross does not swap */
    if (swap) remap_apply(m, ctl, in, s_rm.pend_kslot, s_rm.pend_old);
    if (swap || cancel || s_rm.modal_cancel) { s_rm.state = RM_IDLE; s_rm.modal_cancel = false; ImGui::CloseCurrentPopup(); }
    ImGui::EndPopup();
}

static void remap_reset_popup(PadInputMap& m)
{
    if (s_rm.open_reset) { ImGui::OpenPopup("###rmreset"); s_rm.open_reset = false; }
    remap_center_next();
    if (!ImGui::BeginPopupModal("Reset the controls?###rmreset", NULL, kRemapModal)) return;
    if (s_rm.state != RM_RESET) { ImGui::CloseCurrentPopup(); ImGui::EndPopup(); return; }
    ImGui::TextUnformatted("Every key, mouse button and controller button back to the defaults?");
    hint("input.toml keeps its comments and its other settings (mouse, sticks).");
    ImGui::Spacing();
    const bool reset = ImGui::Button("Reset");
    ImGui::SameLine();
    const bool cancel = ImGui::Button("Cancel");
    ImGui::SetItemDefaultFocus();
    if (reset) { prm_reset(&m); remap_order_reset(); remap_commit(m, "Bindings reset to the defaults"); }
    if (reset || cancel || s_rm.modal_cancel) { s_rm.state = RM_IDLE; s_rm.modal_cancel = false; ImGui::CloseCurrentPopup(); }
    ImGui::EndPopup();
}

/* The page's bindings part (tab_controls). */
static void remap_section(double now)
{
    s_rm.drawn_t = now;
    if (!s_rm.order_init) remap_order_reset();
    PadInputMap m;
    cellPad_host_input_map(&m);
    section("Buttons and keys");
    int cap_slots[2] = { -1, -1 };
    const ImGuiTableFlags tf = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp;
    if (ImGui::BeginTable("bindings", 5, tf)) {
        ImGui::TableSetupColumn("PS3 control", ImGuiTableColumnFlags_WidthStretch, 1.8f);
        ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Other key", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Mouse", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Controller", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableHeadersRow();
        for (int i = 0; i < PIM_N_CONTROLS; i++) {
            const PrmRow& r = prm_rows[i];
            int slots[2];
            const int extra = prm_key_slots(&m, r.ctl, s_rm.order[r.ctl], slots);
            s_rm.order[r.ctl][0] = slots[0]; s_rm.order[r.ctl][1] = slots[1];
            if (s_rm.state != RM_IDLE && s_rm.ctl == r.ctl) { cap_slots[0] = slots[0]; cap_slots[1] = slots[1]; }
            ImGui::TableNextRow();
            ImGui::PushID(r.ctl);
            ImGui::TableSetColumnIndex(0);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(r.name);
            ImGui::SameLine();
            ImGui::TextColored(kGrey, "%s", r.does);
            for (int col = 0; col < 4; col++) {
                ImGui::TableSetColumnIndex(col + 1);
                char name[48], label[80], tmp[24];
                if (col < 2) {
                    if (slots[col] < 0) snprintf(name, sizeof name, "-");
                    else if (col == 1 && extra) snprintf(name, sizeof name, "%s +%d", prm_key_name((uint32_t)slots[col], tmp, sizeof tmp), extra);
                    else snprintf(name, sizeof name, "%s", prm_key_name((uint32_t)slots[col], tmp, sizeof tmp));
                } else if (col == 2) remap_bits_label(m.mouse[r.ctl], PRM_MOUSE, name, sizeof name);
                else remap_bits_label(m.pad[r.ctl], PRM_PAD, name, sizeof name);
                const bool waiting = s_rm.state == RM_CAPTURE && s_rm.ctl == r.ctl && s_rm.slot == col;
                snprintf(label, sizeof label, "%s###c%d", waiting ? "..." : name, col);
                if (ImGui::Button(label, ImVec2(-FLT_MIN, 0.0f)) && s_rm.state == RM_IDLE) remap_begin(r.ctl, col, now);
                /* right-click, or Square on a controller: clear the cell */
                const bool clear = s_rm.state == RM_IDLE &&
                    (ImGui::IsItemClicked(ImGuiMouseButton_Right) ||
                     (ImGui::IsItemFocused() && ImGui::IsKeyPressed(ImGuiKey_GamepadFaceLeft, false)));
                if (clear) {
                    char what[120];
                    if (col < 2 && slots[col] >= 0) {
                        snprintf(what, sizeof what, "%s: %s cleared", r.name, prm_key_name((uint32_t)slots[col], tmp, sizeof tmp));
                        prm_clear(&m, r.ctl, PRM_KEY, slots[col]);
                        s_rm.order[r.ctl][col] = -1;
                        remap_commit(m, what);
                    } else if (col == 2 && m.mouse[r.ctl]) {
                        prm_clear(&m, r.ctl, PRM_MOUSE, -1);
                        snprintf(what, sizeof what, "%s: no mouse input", r.name);
                        remap_commit(m, what);
                    } else if (col == 3 && m.pad[r.ctl]) {
                        prm_clear(&m, r.ctl, PRM_PAD, -1);
                        snprintf(what, sizeof what, "%s: no controller input", r.name);
                        remap_commit(m, what);
                    }
                }
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip) && s_rm.state == RM_IDLE)
                    ImGui::SetTooltip("%s - %s\n%s\nClick to change, right-click to clear (Cross / Square on a controller).",
                                      r.name, r.does, name);
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    remap_capture_popup(m, cap_slots, now);
    remap_swap_popup(m);
    remap_reset_popup(m);

    ImGui::Spacing();
    if (ImGui::Button("Reset to defaults") && s_rm.state == RM_IDLE) {
        s_rm.state = RM_RESET; s_rm.modal_cancel = false; s_rm.open_reset = true;
    }
    if (s_rm.status[0]) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(s_rm.status_bad ? kOrange : kGrey, "%s", s_rm.status);
        ImGui::PopTextWrapPos();
    } else {
        char path[600];
        cellPad_host_input_map_path(path, sizeof path);
        char line[700];
        if (path[0]) snprintf(line, sizeof line, "Changes apply at once and are saved to %s (its comments and other settings stay).", path);
        else snprintf(line, sizeof line, "Changes apply at once; PS3_INPUT_MAP is empty, so they last for this session only.");
        hint(line);
    }
    hint("Click a cell and press the new key, mouse button or controller button; right-click clears it. One "
         "input drives one PS3 control: taking one that is in use asks to swap. Two keys per control; Enter and "
         "keypad Enter count as one key.");
    hint("Kept for the window and this menu: F1, F2, F3, F7, F9, F11, Esc, Alt (Alt+Enter, Alt+Tab, Alt+F4), the "
         "Windows key and Print Screen.");
    bool fam = false;
    for (int f = 0; f < PIM_N_FAM; f++) if (m.pad_fam_set[f]) fam = true;
    hint(fam ? "Controller buttons are by position (bottom = Cross on every pad). input.toml has a "
               "[controller.<family>] section: its lines still win for that kind of pad. Steam Input works too."
             : "Controller buttons are by position (bottom = Cross on every pad); the sticks always move the PS3 "
               "sticks. Steam Input remapping works too.");
}
