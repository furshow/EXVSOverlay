// Menu side of EXVS Overlay: settings, hotkeys, frame timing and the Dear ImGui window.
#pragma once
#include <windows.h>

#define OVERLAY_VERSION "0.12.0"

// Own copies of the XInput structs (including xinput.h would clash with our exported functions).
struct XINPUT_GAMEPAD_ { WORD wButtons; BYTE bLeftTrigger, bRightTrigger; SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY; };
struct XINPUT_STATE_ { DWORD dwPacketNumber; XINPUT_GAMEPAD_ Gamepad; };

// overlay.cpp
void ov_log(const char *fmt, ...);
const char *ov_dir(void);
DWORD ov_real_xinput_get_state(DWORD i, XINPUT_STATE_ *s);
HWND ov_game_hwnd(void);
void ov_set_window(int client_w, int client_h, bool borderless);   // async
bool ov_restart_game(void);   // closes and relaunches the game; returns false if it couldn't

// hud_hide.cpp
void hud_init(void);
bool hud_available(void);
bool hud_hidden(void);
void hud_set_hidden(bool hide);
const char *hud_version(void);

// upscale.cpp
struct UpStatus { bool wanted, active, failed; UINT game_w, game_h, out_w, out_h; int path; };
void up_status(UpStatus *s);

// menu.cpp
int menu_upscale_mode(void);         // 0 off, 1 FSR 1
float menu_upscale_sharpness(void);  // 0..1 (RCAS strength)
void menu_load_config(void);
bool menu_hud_enabled(void);
bool menu_enabled(void);
void menu_setup_style(UINT backbuffer_height);
void menu_set_gpu_name(const char *name);
void menu_tick_frame(void);       // every Present: timing + hotkeys
bool menu_should_draw(void);      // menu open or FPS counter on
bool menu_is_open(void);
bool menu_blocks_input(void);
void menu_filter_gamepad(XINPUT_GAMEPAD_ *g);   // what the game sees (menu open / card button hidden)
void menu_feed_gamepad(void);     // between backend NewFrame and ImGui::NewFrame
void menu_draw(UINT bb_width, UINT bb_height);
