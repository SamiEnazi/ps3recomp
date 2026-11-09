/*
 * vk_overlay_cheats -- the in-game menu's Cheats page (vk_overlay.cpp shows it as
 * its last tab when the title registered cheats: include/ps3emu/ps3_cheats.h).
 * The page lists the registered cheats by group; every change applies at once
 * through the cheat's own apply callback. Nothing here is saved: cheats are off at
 * every start.
 */
#include "imgui.h"

#include "../../include/ps3emu/ps3_cheats.h"

#include <stdio.h>
#include <string.h>

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

/* The menu's timed test script: "cheat=id=value" (vk_overlay.cpp script_poll). */
extern "C" int ps3_overlay_cheat_set(const char* id, double v)
{
    ps3_cheat* c = ps3_cheat_find(id);
    if (!c) { fprintf(stderr, "[cheats] no cheat '%s'\n", id); return 0; }
    return ps3_cheat_set(c, v);
}

/* The page, inside the menu's tab (vk_overlay.cpp). `w` = the menu's combo width. */
extern "C" void vk_overlay_cheats_page(float w)
{
    if (g_ps3_cheats.refresh) g_ps3_cheats.refresh();
    const int n = ps3_cheats_count();
    bool saves = false;
    for (int i = 0; i < n; i++) if (g_ps3_cheats.e[i]->flags & PS3_CHEAT_SAVES) saves = true;

    ImGui::PushStyleColor(ImGuiCol_Text, kCheatOrange);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted("Cheats are off at every start and change the game while they are on.");
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
