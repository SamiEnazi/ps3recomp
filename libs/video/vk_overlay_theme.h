/*
 * vk_overlay_theme -- the overlay's look (the F2 menu, its pages, the save list
 * and the message dialog): dark charcoal panels with a slight blue tint and
 * electric accents, after inFAMOUS 2's menus. Two palettes, the karma choice:
 *   Hero      (default)  electric blue -- Cole's lightning
 *   Infamous             ember red / orange
 * PS3_OVERLAY_THEME=hero|infamous (the menu's Display page sets it; saved with
 * the other settings). No game assets: the bold face is Windows' own Segoe UI
 * Bold, loaded at run time (DejaVu Sans Bold elsewhere), else the regular font.
 * Render thread only (the overlay's ImGui frame).
 */
#ifndef PS3RECOMP_VK_OVERLAY_THEME_H
#define PS3RECOMP_VK_OVERLAY_THEME_H

#include "imgui.h"

struct VkOvTheme {
    ImVec4 accent;       /* the lightning: selected tab, active widgets, headings, checkmarks */
    ImVec4 accent_hi;    /* a brighter edge: the focused item, the selected row, the chosen button */
    ImVec4 accent_dim;   /* hovered and selected backgrounds */
    ImVec4 sel_bg;       /* a selected row's fill */
    ImVec4 panel;        /* window background */
    ImVec4 panel_hi;     /* a raised panel (a question over a list) */
    ImVec4 button;       /* a plain button */
    ImVec4 text, text_dim, warn;
};

const VkOvTheme& vk_ov_theme(void);
int     vk_ov_theme_red(void);                 /* 1 = Infamous */
void    vk_ov_theme_select(int red);           /* the palette only; vk_ov_theme_style makes the style */
int     vk_ov_theme_parse(const char* v);      /* "infamous" / "red" / "1" -> 1, else 0 */
void    vk_ov_theme_style(ImGuiStyle& st);     /* colours + sizes (unscaled: 720 lines) over StyleColorsDark */
void    vk_ov_theme_set_bold(ImFont* f);
ImFont* vk_ov_theme_bold(void);                /* NULL = the regular font */

/* A bold heading in the accent colour with a rule that fades out to the right. */
void vk_ov_heading(const char* text);
/* Begin() with the title in the bold face. */
bool vk_ov_begin(const char* name, bool* open, ImGuiWindowFlags flags);
/* A Yes/No-style button: `hl` = where the pad is (accent fill and a bright edge). */
bool vk_ov_choice(const char* label, bool hl, const ImVec2& size);

static inline ImU32 vk_ov_u32(const ImVec4& c, float alpha = 1.0f)
{
    return ImGui::ColorConvertFloat4ToU32(ImVec4(c.x, c.y, c.z, c.w * alpha));
}

#endif
