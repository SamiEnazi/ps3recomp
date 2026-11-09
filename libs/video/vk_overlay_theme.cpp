/*
 * vk_overlay_theme -- see vk_overlay_theme.h.
 */
#include "vk_overlay_theme.h"

#include <string.h>

static ImVec4 mix(const ImVec4& a, const ImVec4& b, float t)
{
    return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t);
}
static ImVec4 alpha(const ImVec4& c, float a) { return ImVec4(c.x, c.y, c.z, a); }

/* panels and text are shared; only the accents change with karma */
#define SD_PANELS                                                              \
    ImVec4(0.050f, 0.058f, 0.076f, 0.97f), /* panel     */                   \
    ImVec4(0.072f, 0.084f, 0.108f, 0.99f), /* panel_hi  */                   \
    ImVec4(0.105f, 0.122f, 0.158f, 1.00f), /* button    */                   \
    ImVec4(0.925f, 0.945f, 0.970f, 1.00f), /* text      */                   \
    ImVec4(0.600f, 0.650f, 0.720f, 1.00f), /* text_dim  */                   \
    ImVec4(1.000f, 0.620f, 0.220f, 1.00f)  /* warn      */

static const VkOvTheme kHero = {
    ImVec4(0.30f, 0.72f, 1.00f, 1.0f),     /* accent: electric blue */
    ImVec4(0.74f, 0.93f, 1.00f, 1.0f),     /* accent_hi */
    ImVec4(0.12f, 0.33f, 0.51f, 1.0f),     /* accent_dim */
    ImVec4(0.075f, 0.19f, 0.30f, 1.0f),    /* sel_bg */
    SD_PANELS
};
static const VkOvTheme kInfamous = {
    ImVec4(1.00f, 0.38f, 0.15f, 1.0f),     /* accent: ember */
    ImVec4(1.00f, 0.78f, 0.50f, 1.0f),     /* accent_hi */
    ImVec4(0.52f, 0.16f, 0.07f, 1.0f),     /* accent_dim */
    ImVec4(0.27f, 0.085f, 0.04f, 1.0f),    /* sel_bg */
    SD_PANELS
};

static int     s_red;
static ImFont* s_bold;

const VkOvTheme& vk_ov_theme(void) { return s_red ? kInfamous : kHero; }
int  vk_ov_theme_red(void) { return s_red; }
void vk_ov_theme_select(int red) { s_red = red ? 1 : 0; }
void vk_ov_theme_set_bold(ImFont* f) { s_bold = f; }
ImFont* vk_ov_theme_bold(void) { return s_bold; }

int vk_ov_theme_parse(const char* v)
{
    if (!v || !*v) return 0;
    return !strcmp(v, "1") || !strcmp(v, "infamous") || !strcmp(v, "Infamous") || !strcmp(v, "INFAMOUS") ||
           !strcmp(v, "red") || !strcmp(v, "Red");
}

void vk_ov_theme_style(ImGuiStyle& st)
{
    const VkOvTheme& t = vk_ov_theme();
    ImGui::StyleColorsDark(&st);
    st.FontSizeBase = 17.0f;
    /* small rounding, a bit more room: readable from the sofa with a pad */
    st.WindowRounding = 6.0f; st.ChildRounding = 5.0f; st.FrameRounding = 4.0f; st.PopupRounding = 5.0f;
    st.ScrollbarRounding = 4.0f; st.GrabRounding = 3.0f; st.TabRounding = 4.0f;
    st.WindowPadding = ImVec2(20, 16); st.FramePadding = ImVec2(12, 6);
    st.ItemSpacing = ImVec2(12, 9); st.ItemInnerSpacing = ImVec2(8, 6);
    st.WindowBorderSize = 1.0f; st.ChildBorderSize = 1.0f; st.PopupBorderSize = 1.0f; st.FrameBorderSize = 0.0f;
    st.TabBorderSize = 0.0f; st.TabBarBorderSize = 2.0f; st.TabBarOverlineSize = 2.0f;
    st.GrabMinSize = 14.0f; st.ScrollbarSize = 14.0f;
    st.WindowTitleAlign = ImVec2(0.0f, 0.5f);

    ImVec4* c = st.Colors;
    const ImVec4 frame(0.090f, 0.106f, 0.138f, 1.0f), tab(0.072f, 0.084f, 0.110f, 1.0f);
    const ImVec4 title(0.034f, 0.040f, 0.054f, 1.0f);
    c[ImGuiCol_Text]                 = t.text;
    c[ImGuiCol_TextDisabled]         = ImVec4(0.42f, 0.46f, 0.52f, 1.0f);
    c[ImGuiCol_WindowBg]             = t.panel;
    c[ImGuiCol_ChildBg]              = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg]              = t.panel_hi;
    c[ImGuiCol_Border]               = alpha(t.accent, 0.30f);
    c[ImGuiCol_BorderShadow]         = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBg]              = frame;
    c[ImGuiCol_FrameBgHovered]       = mix(frame, t.accent_dim, 0.45f);
    c[ImGuiCol_FrameBgActive]        = mix(frame, t.accent_dim, 0.80f);
    c[ImGuiCol_TitleBg]              = title;
    c[ImGuiCol_TitleBgActive]        = title;
    c[ImGuiCol_TitleBgCollapsed]     = title;
    c[ImGuiCol_MenuBarBg]            = tab;
    c[ImGuiCol_ScrollbarBg]          = ImVec4(0.03f, 0.035f, 0.045f, 0.60f);
    c[ImGuiCol_ScrollbarGrab]        = ImVec4(0.17f, 0.20f, 0.25f, 1.0f);
    c[ImGuiCol_ScrollbarGrabHovered] = t.accent_dim;
    c[ImGuiCol_ScrollbarGrabActive]  = t.accent;
    c[ImGuiCol_CheckMark]            = t.accent;
    c[ImGuiCol_CheckboxSelectedBg]   = mix(frame, t.accent_dim, 0.55f);
    c[ImGuiCol_SliderGrab]           = alpha(t.accent, 0.85f);
    c[ImGuiCol_SliderGrabActive]     = t.accent_hi;
    c[ImGuiCol_Button]               = t.button;
    c[ImGuiCol_ButtonHovered]        = t.accent_dim;
    c[ImGuiCol_ButtonActive]         = t.accent;
    c[ImGuiCol_Header]               = alpha(t.accent_dim, 0.60f);
    c[ImGuiCol_HeaderHovered]        = alpha(t.accent_dim, 0.90f);
    c[ImGuiCol_HeaderActive]         = t.accent;
    c[ImGuiCol_Separator]            = alpha(t.accent, 0.22f);
    c[ImGuiCol_SeparatorHovered]     = t.accent;
    c[ImGuiCol_SeparatorActive]      = t.accent_hi;
    c[ImGuiCol_ResizeGrip]           = alpha(t.accent, 0.20f);
    c[ImGuiCol_ResizeGripHovered]    = t.accent;
    c[ImGuiCol_ResizeGripActive]     = t.accent_hi;
    c[ImGuiCol_InputTextCursor]      = t.accent_hi;
    c[ImGuiCol_Tab]                  = tab;
    c[ImGuiCol_TabHovered]           = t.accent_dim;
    c[ImGuiCol_TabSelected]          = mix(t.accent_dim, t.accent, 0.30f);
    c[ImGuiCol_TabSelectedOverline]  = t.accent_hi;
    c[ImGuiCol_TabDimmed]            = tab;
    c[ImGuiCol_TabDimmedSelected]    = mix(t.accent_dim, t.accent, 0.30f);   /* the page stays lit while the pad is in it */
    c[ImGuiCol_TabDimmedSelectedOverline] = t.accent_hi;
    c[ImGuiCol_PlotLines]            = t.accent;
    c[ImGuiCol_PlotLinesHovered]     = t.accent_hi;
    c[ImGuiCol_PlotHistogram]        = t.accent;
    c[ImGuiCol_PlotHistogramHovered] = t.accent_hi;
    c[ImGuiCol_TableHeaderBg]        = tab;
    c[ImGuiCol_TableBorderStrong]    = alpha(t.accent, 0.25f);
    c[ImGuiCol_TableBorderLight]     = ImVec4(1, 1, 1, 0.06f);
    c[ImGuiCol_TableRowBg]           = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt]        = ImVec4(1, 1, 1, 0.03f);
    c[ImGuiCol_TextLink]             = t.accent;
    c[ImGuiCol_TextSelectedBg]       = alpha(t.accent, 0.35f);
    c[ImGuiCol_TreeLines]            = alpha(t.accent, 0.30f);
    c[ImGuiCol_DragDropTarget]       = t.accent_hi;
    c[ImGuiCol_DragDropTargetBg]     = alpha(t.accent, 0.15f);
    c[ImGuiCol_UnsavedMarker]        = t.accent;
    c[ImGuiCol_NavCursor]            = t.accent_hi;    /* the focused item's bright edge */
    c[ImGuiCol_NavWindowingHighlight]= alpha(t.accent_hi, 0.70f);
    c[ImGuiCol_NavWindowingDimBg]    = ImVec4(0, 0, 0, 0.50f);
    c[ImGuiCol_ModalWindowDimBg]     = ImVec4(0, 0, 0, 0.55f);
}

void vk_ov_heading(const char* text)
{
    const VkOvTheme& t = vk_ov_theme();
    const ImGuiStyle& st = ImGui::GetStyle();
    ImGui::Dummy(ImVec2(1.0f, st.ItemSpacing.y * 0.5f));
    ImGui::PushFont(s_bold, st.FontSizeBase * 1.08f);
    ImGui::PushStyleColor(ImGuiCol_Text, t.accent);
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
    ImGui::PopFont();
    /* the rule: lit at the left, fading out to the right */
    const float k = st.FontScaleDpi > 0.0f ? st.FontScaleDpi : 1.0f;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x, h = 2.0f * k;
    ImGui::GetWindowDrawList()->AddRectFilledMultiColor(p, ImVec2(p.x + (w > 1.0f ? w : 1.0f), p.y + h),
        vk_ov_u32(t.accent, 0.9f), vk_ov_u32(t.accent, 0.0f), vk_ov_u32(t.accent, 0.0f), vk_ov_u32(t.accent, 0.9f));
    ImGui::Dummy(ImVec2(1.0f, h + st.ItemSpacing.y * 0.3f));
}

bool vk_ov_begin(const char* name, bool* open, ImGuiWindowFlags flags)
{
    ImGui::PushFont(s_bold, 0.0f);   /* the title bar in the bold face */
    const bool r = ImGui::Begin(name, open, flags);
    ImGui::PopFont();
    return r;
}

bool vk_ov_choice(const char* label, bool hl, const ImVec2& size)
{
    const VkOvTheme& t = vk_ov_theme();
    const float k = ImGui::GetStyle().FontScaleDpi > 0.0f ? ImGui::GetStyle().FontScaleDpi : 1.0f;
    ImGui::PushStyleColor(ImGuiCol_Button, hl ? mix(t.accent_dim, t.accent, 0.15f) : t.button);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hl ? mix(t.accent_dim, t.accent, 0.35f) : t.accent_dim);
    ImGui::PushStyleColor(ImGuiCol_Border, hl ? t.accent_hi : ImVec4(0, 0, 0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, hl ? 2.0f * k : 0.0f);
    ImGui::PushFont(hl ? s_bold : NULL, 0.0f);
    const bool r = ImGui::Button(label, size);
    ImGui::PopFont();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(3);
    return r;
}
