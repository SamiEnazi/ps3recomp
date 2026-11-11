/*
 * ps3_mouse_camera -- the mouse camera's direct mode, for a title's own camera
 * code (libs/input/cellPad.c, libs/input/pad_mouse_direct.h).
 *
 * PS3_MOUSE_MODE=direct (the default; [mouse] mode = "direct" in input.toml):
 * a title that knows its camera (inFAMOUS 2: the port's cheats.cpp hook on the
 * game's camera update) turns it by the mouse's motion itself -- no stick in
 * between, so no speed cap and no stick acceleration -- and leaves everything
 * else the camera does to the game. Each time its camera updates it calls
 *
 *     float dx, dy, sens, aim_sens;
 *     if (cellPad_host_mouse_direct_take(&dx, &dy, &sens, &aim_sens) && (dx || dy))
 *         yaw += dx * k * (aiming ? aim_sens : sens), pitch += dy * k * (...);
 *
 * dx, dy = raw mouse counts since the last call (+x right, +y towards the
 * player), the player's invert settings applied. While those calls keep coming
 * the pad poll leaves the motion to the title and the mouse moves no stick;
 * when they stop (a menu, the map, a cutscene) the mouse is a right stick again
 * within PMD_CLAIM_US. A 0 return = the mouse is not the camera's now (stick
 * mode, mouse camera off, controller-only input, a menu open): leave the camera
 * to the game, nothing was taken.
 *
 * cellPad_host_mouse_direct_provider(1): the title has such a camera (the
 * menu's Mouse entry shows whether direct mode can work in this build).
 */
#ifndef PS3EMU_PS3_MOUSE_CAMERA_H
#define PS3EMU_PS3_MOUSE_CAMERA_H

#ifdef __cplusplus
extern "C" {
#endif

int   cellPad_host_mouse_direct_take(float* dx, float* dy, float* sens, float* aim_sens);
void  cellPad_host_mouse_direct_provider(int on);
int   cellPad_host_mouse_direct_ready(void);
int   cellPad_host_mouse_mode(void);                 /* 1 direct, 0 stick */
void  cellPad_host_set_mouse_mode(int direct);
float cellPad_host_mouse_aim_sens(void);
void  cellPad_host_set_mouse_aim_sens(float sens);

#ifdef __cplusplus
}
#endif

#endif
