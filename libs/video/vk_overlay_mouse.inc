/* ---- [mouse] the mouse camera block of the Controls page (agent/mouse, 2026-11-11) ----
 * Included by vk_overlay.cpp before tab_controls (needs section, hint, combo_w,
 * extra_set); tab_controls calls mouse_camera_block() in one marked place.
 * Settings (not in kVar): PS3_MOUSE_MODE (direct / stick), PS3_MOUSE_SENS,
 * PS3_MOUSE_AIM_SENS -- live through cellPad's setters, kept in the settings
 * file as other lines (extra_set), shown from cellPad's current values every
 * frame. The direct mode itself: libs/input/pad_mouse_direct.h. */
#include "../../include/ps3emu/ps3_mouse_camera.h"
extern "C" void cellPad_host_mouse_settings(int* on, float* sens, int* invert_x, int* invert_y);
extern "C" void cellPad_host_set_mouse_settings(float sens, int invert_x, int invert_y);

static void mouse_camera_block(void)
{
    section("Mouse camera");
    int on = 0, ix = 0, iy = 0;
    float sens = 1.0f;
    cellPad_host_mouse_settings(&on, &sens, &ix, &iy);
    float aim = cellPad_host_mouse_aim_sens();
    int pick = cellPad_host_mouse_mode() ? 1 : 0;
    const bool ready = cellPad_host_mouse_direct_ready() != 0;
    static const char* const kModes[] = { "Like a stick", "Direct" };
    char b[16];
    ImGui::SetNextItemWidth(combo_w());
    if (ImGui::Combo("Mouse", &pick, kModes, 2)) {
        cellPad_host_set_mouse_mode(pick);
        extra_set("PS3_MOUSE_MODE", pick ? "direct" : "stick");
    }
    ImGui::SetNextItemWidth(combo_w());
    if (ImGui::SliderFloat("Mouse sensitivity", &sens, 0.1f, 5.0f, "%.2f", ImGuiSliderFlags_Logarithmic)) {
        cellPad_host_set_mouse_settings(sens, ix, iy);
        snprintf(b, sizeof b, "%.2f", sens);
        extra_set("PS3_MOUSE_SENS", b);
    }
    ImGui::BeginDisabled(!pick);
    ImGui::SetNextItemWidth(combo_w());
    if (ImGui::SliderFloat("Aiming sensitivity", &aim, 0.1f, 5.0f, "%.2f", ImGuiSliderFlags_Logarithmic)) {
        cellPad_host_set_mouse_aim_sens(aim);
        snprintf(b, sizeof b, "%.2f", aim);
        extra_set("PS3_MOUSE_AIM_SENS", b);
    }
    ImGui::EndDisabled();
    if (!on) hint("The mouse camera is off (PS3_MOUSE=0 or camera = false in input.toml).");
    else if (pick && ready)
        hint("Direct: the camera turns by how far the mouse moves -- no stick speed limit and no stick "
             "acceleration. Aiming sensitivity is used while aiming. In menus, the map and cutscenes the "
             "mouse works like a stick.");
    else if (pick)
        hint("Direct needs this build's camera hook (the cheats lift): until then the mouse works like a stick.");
    else
        hint("Like a stick: the mouse moves the right stick by its speed, as on a controller (the game's own "
             "stick speed and acceleration apply).");
}
