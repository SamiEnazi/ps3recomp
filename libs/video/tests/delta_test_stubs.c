/* (rnd1) Stubs for test_rsx_draw_delta's Windows link: the runtime pieces the
 * draw engine names but the test does not exercise. */
#include <stdint.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
void (*g_vm_host_io_hook)(uint32_t ea, uint32_t len, int end);
void gcm_split_run(uint32_t kind, uint32_t a, uint32_t b, uint32_t c) { (void)kind; (void)a; (void)b; (void)c; }
void ps3_host_place_self_once(int role) { (void)role; }
int ps3_limits_format(char* buf, int len, int size) { (void)buf; (void)size; return len; }
int ps3_limits_take_frame(char* buf, int len, int size) { (void)buf; (void)size; return len; }
int ps3_limits_pending(void) { return 0; }
unsigned long long ps3_ms_now(void) { return GetTickCount64(); }
