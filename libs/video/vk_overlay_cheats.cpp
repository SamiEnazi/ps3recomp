/*
 * vk_overlay_cheats -- the in-game menu's Cheats page (vk_overlay.cpp shows it as
 * its last tab when the title registered cheats: include/ps3emu/ps3_cheats.h).
 * The page lists the registered cheats by group; every change applies at once
 * through the cheat's own apply callback. "Remember cheats" (default on) keeps the ones
 * that are on in the menu's settings file and sets them again at the next start, with
 * a notice naming them; off = all off at every start.
 */
#include "imgui.h"

#include "../../include/ps3emu/ps3_cheats.h"

#include <stdio.h>
#include <string.h>

/* vk_overlay.cpp: the settings file was changed (saved soon after); a notice in a corner. */
extern "C" void vk_overlay_settings_changed(void);
extern "C" void vk_overlay_notice_for(const char* text, double secs);

/* "Remember cheats" (default on): the cheats that can be remembered (ps3_cheat_rememberable)
 * are kept in the menu's settings file as PS3_CHEAT_<ID>=<value> lines and set again when the
 * window opens; PS3_CHEATS_REMEMBER=0 in the file = off (nothing kept, nothing set). */
static bool s_remember = true;

static const ImVec4 kCheatCyan(0.20f, 0.80f, 0.95f, 1.0f);
static const ImVec4 kCheatGrey(0.62f, 0.66f, 0.72f, 1.0f);
static const ImVec4 kCheatOrange(1.0f, 0.60f, 0.20f, 1.0f);
static const ImVec4 kCheatGreen(0.45f, 0.85f, 0.45f, 1.0f);

static void cheat_hint(const char* text)
{
    ImGui::PushStyleColor(ImGuiCol_Text, kCheatGrey);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}

static void cheat_failed(const ps3_cheat* c)
{
    ImGui::SameLine();
    ImGui::TextColored(kCheatOrange, "not applied");
    (void)c;
}

/* One cheat's row(s). `w` the width of a combo or slider. */
static void cheat_row(ps3_cheat* c, float w)
{
    ImGui::PushID(c);
    const bool off = c->unavailable != NULL;
    ImGui::BeginDisabled(off);
    static ps3_cheat* s_failed;   /* the last cheat whose apply said no */
    bool changed = false;
    switch (c->kind) {
    case PS3_CHEAT_TOGGLE: {
        bool v = c->value != 0.0;
        if (ImGui::Checkbox(c->name, &v)) { c->value = v ? 1.0 : 0.0; changed = true; }
        if (v) { ImGui::SameLine(); ImGui::TextColored(kCheatGreen, "ON"); }
        break;
    }
    case PS3_CHEAT_CHOICE: {
        int v = (int)c->value;
        ImGui::SetNextItemWidth(w);
        if (c->choices && c->n_choices > 0 && ImGui::Combo(c->name, &v, c->choices, c->n_choices)) { c->value = v; changed = true; }
        break;
    }
    case PS3_CHEAT_INT: {
        int v = (int)c->value;
        ImGui::SetNextItemWidth(w);
        if (ImGui::SliderInt(c->name, &v, (int)c->min, (int)c->max, c->fmt ? c->fmt : "%d")) { c->value = v; changed = true; }
        break;
    }
    case PS3_CHEAT_FLOAT: {
        float v = (float)c->value;
        ImGui::SetNextItemWidth(w);
        if (ImGui::SliderFloat(c->name, &v, (float)c->min, (float)c->max, c->fmt ? c->fmt : "%.2f")) { c->value = v; changed = true; }
        break;
    }
    case PS3_CHEAT_ACTION: {
        if (c->max > c->min) {
            int v = (int)c->value;
            ImGui::SetNextItemWidth(w * 0.7f);
            ImGui::SliderInt("##amount", &v, (int)c->min, (int)c->max, c->fmt ? c->fmt : "%d");
            c->value = v;
            ImGui::SameLine();
        }
        if (ImGui::Button(c->button ? c->button : c->name)) changed = true;
        if (c->button && c->max <= c->min) { ImGui::SameLine(); ImGui::TextUnformatted(c->name); }
        break;
    }
    default:
        break;
    }
    if (changed) {
        const int ok = c->apply ? c->apply(c) : 0;
        fprintf(stderr, "[cheats] menu: %s = %g%s\n", c->id, c->value, ok ? "" : " (not applied)");
        s_failed = ok ? (s_failed == c ? NULL : s_failed) : c;
        if (s_remember && ps3_cheat_rememberable(c)) vk_overlay_settings_changed();
    }
    if (s_failed == c) cheat_failed(c);
    if (c->flags & PS3_CHEAT_SAVES) {
        ImGui::SameLine();
        ImGui::TextColored(kCheatOrange, "save");
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("The game writes this into your save the next time it saves.");
    }
    ImGui::EndDisabled();
    if (off) { ImGui::SameLine(); ImGui::TextColored(kCheatGrey, "(%s)", c->unavailable); }
    if (c->help && *c->help) cheat_hint(c->help);
    ImGui::PopID();
}

extern "C" int vk_overlay_cheats_count(void) { return ps3_cheats_count(); }
extern "C" int vk_overlay_cheats_remember(void) { return s_remember ? 1 : 0; }

/* The menu's timed test script: "cheat=id=value" (vk_overlay.cpp script_poll); id "remember"
 * is the Remember cheats switch. */
extern "C" int ps3_overlay_cheat_set(const char* id, double v)
{
    if (!strcmp(id, "remember")) {
        s_remember = v != 0.0;
        vk_overlay_settings_changed();
        fprintf(stderr, "[cheats] remember = %d\n", s_remember ? 1 : 0);
        return 1;
    }
    ps3_cheat* c = ps3_cheat_find(id);
    if (!c) { fprintf(stderr, "[cheats] no cheat '%s'\n", id); return 0; }
    const int ok = ps3_cheat_set(c, v);
    if (s_remember && ps3_cheat_rememberable(c)) vk_overlay_settings_changed();
    return ok;
}

/* The settings file's lines this page owns (the file writer skips them in the lines it keeps). */
extern "C" int vk_overlay_cheats_owns_key(const char* k)
{
    return !strcmp(k, "PS3_CHEATS_REMEMBER") || !strncmp(k, "PS3_CHEAT_", 10);
}

/* The settings file (vk_overlay.cpp settings_save): the switch when off, else the remembered
 * cheats that are on (ps3_cheat_is_on). */
extern "C" void vk_overlay_cheats_write(FILE* f)
{
    if (!s_remember) { fprintf(f, "PS3_CHEATS_REMEMBER=0\n"); fprintf(stderr, "[cheats] remembered: off\n"); return; }
    char name[64], line[256] = "";
    size_t n = 0;
    for (int i = 0; i < ps3_cheats_count(); i++) {
        const ps3_cheat* c = g_ps3_cheats.e[i];
        if (!ps3_cheat_rememberable(c) || !ps3_cheat_is_on(c) || c->unavailable) continue;
        ps3_cheat_env_name(c, name, sizeof name);
        fprintf(f, "%s=%g\n", name, c->value);
        if (n < sizeof line - 40) n += (size_t)snprintf(line + n, sizeof line - n, " %s=%g", c->id, c->value);
    }
    fprintf(stderr, "[cheats] remembered:%s\n", n ? line : " (none)");
}

/* The window opened (vk_overlay_attach; the file was read into the environment before main):
 * set the remembered cheats again and say which are on, for a few seconds. Their apply
 * callbacks only switch things on; the title's hooks act once the game is in play. */
extern "C" void vk_overlay_cheats_attach(void)
{
    if (!ps3_cheats_count()) return;
    if (g_ps3_cheats.refresh) g_ps3_cheats.refresh();
    { const char* e = getenv("PS3_CHEATS_REMEMBER"); s_remember = !(e && e[0] == '0'); }
    if (!s_remember) return;
    char name[64], on[200] = "";
    size_t n = 0;
    int k = 0;
    for (int i = 0; i < ps3_cheats_count(); i++) {
        ps3_cheat* c = g_ps3_cheats.e[i];
        if (!ps3_cheat_rememberable(c)) continue;
        ps3_cheat_env_name(c, name, sizeof name);
        const char* e = getenv(name);
        if (!e || !*e) continue;
        if (!ps3_cheat_set(c, atof(e)) || !ps3_cheat_is_on(c)) continue;
        if (c->kind == PS3_CHEAT_CHOICE && c->choices && (int)c->value < c->n_choices)
            n += (size_t)snprintf(on + n, n < sizeof on ? sizeof on - n : 0, "%s%s: %s", k ? ", " : "", c->name,
                                  c->choices[(int)c->value]);
        else if (c->kind == PS3_CHEAT_TOGGLE)
            n += (size_t)snprintf(on + n, n < sizeof on ? sizeof on - n : 0, "%s%s", k ? ", " : "", c->name);
        else
            n += (size_t)snprintf(on + n, n < sizeof on ? sizeof on - n : 0, "%s%s: %g", k ? ", " : "", c->name, c->value);
        k++;
    }
    if (k) {
        char msg[240];
        snprintf(msg, sizeof msg, "Cheats on: %s", on);
        vk_overlay_notice_for(msg, 8.0);
        fprintf(stderr, "[cheats] %s (remembered; F2 -> Cheats)\n", msg);
    }
}

/* The page, inside the menu's tab (vk_overlay.cpp). `w` = the menu's combo width. */
extern "C" void vk_overlay_cheats_page(float w)
{
    if (g_ps3_cheats.refresh) g_ps3_cheats.refresh();
    const int n = ps3_cheats_count();
    bool saves = false;
    for (int i = 0; i < n; i++) if (g_ps3_cheats.e[i]->flags & PS3_CHEAT_SAVES) saves = true;

    if (ImGui::Checkbox("Remember cheats", &s_remember)) vk_overlay_settings_changed();
    ImGui::SameLine();
    ImGui::TextColored(kCheatGrey, s_remember ? "(switched on again at the next start)" : "(all off at the next start)");
    ImGui::PushStyleColor(ImGuiCol_Text, kCheatOrange);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(s_remember ? "Cheats change the game while they are on. One-time actions (Add ...) are never repeated."
                                      : "Cheats are off at every start and change the game while they are on.");
    if (saves)
        ImGui::TextUnformatted(g_ps3_cheats.note ? g_ps3_cheats.note
                               : "Cheats marked \"save\" change your save: the game keeps their effect the next "
                                 "time it saves. Back up your saves first if you want to keep a clean game.");
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();

    const char* groups[PS3_CHEATS_MAX];
    int ng = 0;
    for (int i = 0; i < n; i++) {
        const char* g = g_ps3_cheats.e[i]->group ? g_ps3_cheats.e[i]->group : "Cheats";
        bool seen = false;
        for (int k = 0; k < ng; k++) if (!strcmp(groups[k], g)) seen = true;
        if (!seen) groups[ng++] = g;
    }
    for (int k = 0; k < ng; k++) {
        ImGui::Spacing();
        ImGui::TextColored(kCheatCyan, "%s", groups[k]);
        ImGui::Separator();
        for (int i = 0; i < n; i++) {
            ps3_cheat* c = g_ps3_cheats.e[i];
            if (!strcmp(c->group ? c->group : "Cheats", groups[k])) cheat_row(c, w);
        }
    }
}
