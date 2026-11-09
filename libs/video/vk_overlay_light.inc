/* ---- the Lighting page (agent/light, 2026-11-09; ps3_lightfx.h) -------------
 * Included by vk_overlay.cpp after its page helpers (section, hint, combo_w,
 * extra_set). Its settings are not in kVar: a change goes to the environment
 * and the owning module's applier (ps3_setting_apply: they are all live) and
 * into the settings file as one of its other lines (extra_set). The widgets
 * show the modules' current values (ps3_lightfx_get) every frame, so nothing
 * here can go stale. Every option off = the game as the PS3 drew it.
 *
 * F3 (with or without the menu): compare -- every option off at once, F3
 * again puts the player's choice back (not saved: the file keeps the choice). */

static const char* const kLfxSoftName[] = {
    "Off (as on PS3)", "Soft", "Softer", "Softest" };
static const char* const kLfxSsaoName[] = {
    "Game (half resolution, as on PS3)", "Full resolution", "Full resolution, high" };

static struct {
    bool comparing;                       /* F3: the PS3 look shown, the choice kept here */
    char saved[PS3_LFX_COUNT][16];
} s_lfx;

static const char* lfx_off_value(int id) { return id == PS3_LFX_SSAO_STRENGTH ? "100" : "0"; }

/* The player's choice: live now, and in the file. */
static void lfx_choose(int id, int v)
{
    char b[16];
    snprintf(b, sizeof b, "%d", v);
    ps3_setting_apply(ps3_lightfx_name(id), b);
    extra_set(ps3_lightfx_name(id), b);
}

static void lfx_compare_toggle(void)
{
    if (!s_lfx.comparing) {
        bool any = false;
        for (int id = 0; id < PS3_LFX_COUNT; id++) {
            snprintf(s_lfx.saved[id], sizeof s_lfx.saved[id], "%d", ps3_lightfx_get(id));
            any = any || strcmp(s_lfx.saved[id], lfx_off_value(id)) != 0;
        }
        if (!any) { vk_overlay_notice("Lighting: all options are off (the PS3 look) -- nothing to compare"); return; }
        for (int id = 0; id < PS3_LFX_COUNT; id++) ps3_setting_apply(ps3_lightfx_name(id), lfx_off_value(id));
        s_lfx.comparing = true;
        vk_overlay_notice("Lighting: PS3 look (F3 = your settings)");
    } else {
        for (int id = 0; id < PS3_LFX_COUNT; id++) ps3_setting_apply(ps3_lightfx_name(id), s_lfx.saved[id]);
        s_lfx.comparing = false;
        vk_overlay_notice("Lighting: your settings (F3 = PS3 look)");
    }
}

/* "live" (a module took the setting) or "on restart". */
static void lfx_live_mark(int id)
{
    const bool live = ps3_setting_applier(ps3_lightfx_name(id)) != NULL;
    ImGui::SameLine();
    if (live) ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.45f, 1.0f), "live");
    else ImGui::TextColored(kGrey, "on restart");
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
        ImGui::SetTooltip("%s", ps3_lightfx_name(id));
}

static void tab_lighting(void)
{
    const bool gpu_light = env_on("PS3RECOMP_ENG_GPU_LIGHT");
    const bool gpu_ssao = gpu_light && env_on("PS3RECOMP_ENG_GPU_SSAO");
    if (s_lfx.comparing) {
        ImGui::TextColored(kOrange, "Comparing: the PS3 look is shown. Press F3 to get your settings back.");
        ImGui::BeginDisabled();
    }
    section("Shadows");
    { int v = ps3_lightfx_get(PS3_LFX_SOFT_SHADOWS);
      ImGui::SetNextItemWidth(combo_w());
      if (ImGui::Combo("Shadow softness", &v, kLfxSoftName, 4)) lfx_choose(PS3_LFX_SOFT_SHADOWS, v);
      lfx_live_mark(PS3_LFX_SOFT_SHADOWS); }
    hint("The sun's shadows come from a 1024x1024 shadow map, so their edges show small stair steps. "
         "Soft blends each edge over more of the map: smooth edges, shadows stay where they are. "
         "A change rebuilds a few shaders (one short stutter). Very small GPU cost.");

    section("Ambient occlusion (soft darkening in corners and under objects)");
    { int v = ps3_lightfx_get(PS3_LFX_SSAO);
      if (!gpu_ssao) ImGui::BeginDisabled();
      ImGui::SetNextItemWidth(combo_w());
      if (ImGui::Combo("Quality", &v, kLfxSsaoName, 3)) lfx_choose(PS3_LFX_SSAO, v);
      if (!gpu_ssao) ImGui::EndDisabled();
      lfx_live_mark(PS3_LFX_SSAO); }
    { int v = ps3_lightfx_get(PS3_LFX_SSAO_STRENGTH);
      if (!gpu_ssao) ImGui::BeginDisabled();
      ImGui::SetNextItemWidth(combo_w());
      if (ImGui::SliderInt("Strength", &v, 0, 300, v == 100 ? "100%% (game)" : "%d%%")) lfx_choose(PS3_LFX_SSAO_STRENGTH, v);
      if (!gpu_ssao) ImGui::EndDisabled();
      lfx_live_mark(PS3_LFX_SSAO_STRENGTH); }
    hint(gpu_ssao
         ? "The game computes it at half resolution (640x360) and stretches it, which leaves soft, blotchy "
           "edges. Full resolution computes every pixel with the game's own formula: finer contact shadows "
           "under feet, crates and railings. High uses twice the samples. Strength 100% is the game's."
         : "Needs the GPU lighting path (the default; the launcher's -SpuLighting turns it off).");

    section("Light");
    { bool v = ps3_lightfx_get(PS3_LFX_LIGHT_DITHER) != 0;
      if (!gpu_light) ImGui::BeginDisabled();
      if (ImGui::Checkbox("Smooth light gradients", &v)) lfx_choose(PS3_LFX_LIGHT_DITHER, v ? 1 : 0);
      if (!gpu_light) ImGui::EndDisabled();
      lfx_live_mark(PS3_LFX_LIGHT_DITHER); }
    hint(gpu_light
         ? "Street lamps, fires and other lights are stored with 8 bits per colour, which can show as bands "
           "in their soft falloff on walls and the ground. This turns the bands into an invisible fine grain. "
           "Same brightness."
         : "Needs the GPU lighting path (the default; the launcher's -SpuLighting turns it off).");

    section("Tone (a PC extra: changes the game's look)");
    { static const struct { int id; const char* label; } kTone[] = {
          { PS3_LFX_TONE_SHADOWS, "Shadows" }, { PS3_LFX_TONE_MIDTONES, "Midtones" },
          { PS3_LFX_TONE_HIGHLIGHTS, "Highlights" }, { PS3_LFX_CLARITY, "Clarity" } };
      for (const auto& t : kTone) {
          int v = ps3_lightfx_get(t.id);
          ImGui::SetNextItemWidth(combo_w());
          if (ImGui::SliderInt(t.label, &v, -100, 100, v ? "%+d" : "off")) lfx_choose(t.id, v);
          lfx_live_mark(t.id);
      } }
    hint("Shadows, Midtones and Highlights lift (or lower) their part of the brightness range; black and "
         "white stay where they are. Clarity adds local contrast between large shapes (more depth) without "
         "the fine grain of sharpening. All off is the game's own grading. Very small GPU cost.");

    if (s_lfx.comparing) ImGui::EndDisabled();
    section("Compare");
    if (ImGui::Button(s_lfx.comparing ? "Show my settings (F3)" : "Show the PS3 look (F3)")) lfx_compare_toggle();
    ImGui::SameLine();
    if (!s_lfx.comparing && ImGui::Button("Reset to the PS3 look"))
        for (int id = 0; id < PS3_LFX_COUNT; id++) lfx_choose(id, atoi(lfx_off_value(id)));
    hint("F3 flips between your lighting settings and the PS3 look at any time, also with this menu closed.");
}
