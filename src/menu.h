// Menu side of EXVS Overlay: settings, hotkeys, frame timing and the Dear ImGui window.
#pragma once
#include <windows.h>

#define OVERLAY_VERSION "1.2.3"
#define OVERLAY_WIDEN2_(x) L##x
#define OVERLAY_WIDEN_(x) OVERLAY_WIDEN2_(x)
#define OVERLAY_VERSION_W OVERLAY_WIDEN_(OVERLAY_VERSION)

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

// update.cpp (updates from the GitHub releases)
enum { UPD_IDLE, UPD_CHECKING, UPD_LATEST, UPD_AVAILABLE, UPD_DOWNLOADING, UPD_INSTALLED, UPD_FAILED };
struct UpdInfo { int state; const char *version, *notes, *error, *repo; unsigned long long done, total; bool can_install; };
bool upd_supported(void);
void upd_startup(bool auto_check);   // init thread: cleanup + (optionally) a check ~20 s later
void upd_check_now(void);
void upd_install(void);              // download + swap the DLL; used from the next game start
void upd_info(UpdInfo *u);

// input_focus.cpp (input while the game isn't focused)
void focus_init(void);
long focus_blocked_calls(void);
bool focus_hid_pad(XINPUT_GAMEPAD_ *g);   // latest DirectInput (HID) pad the loader read, as an Xbox pad

// split.cpp (splitscreen: one of several copies of the game started by the EXVS Launcher)
void split_init(void);            // reads [split] from this copy's config.ini (done on first use)
int split_log_player(void);       // player number for the log prefix (0 until loaded)
const char *ov_cfg_dir(void);     // folder of this copy's config.ini / card.ini (the exe folder unless split)
bool split_active(void);
int split_player(void);           // 1..4, 0 = not split
int split_pad(void);              // XInput slot this copy reads, -1 = none
bool split_keyboard(void);        // the keyboard reaches this copy
bool split_virtual_key(int vk);   // card / coin key held by the overlay for this copy only
void split_set_virtual_key(int vk, bool down);

// training.cpp (Custom Training: loads Training\training_bridge.dll and applies the launcher's Training\training.ini)
void training_init(void);
bool training_block_start(void);          // a 2v2 match is running: hide Start from the game
int training_start_vk(void);              // [keyboard] Start of config.ini
WORD training_start_pad_mask(void);       // XInput Start + [controller] ArcadeStartButton
bool training_start_hid_button(USHORT b); // HID button number listed in [controller] / [controller-1] Start
void training_note_start_blocked(const char *from);

// menu.cpp
int menu_key_vk(const char *name);  // loader key name -> virtual key (0 = unknown)
bool menu_keyboard_blocked(void);   // keyboard / mouse: menu open, not focused (option), or not this split player's
bool menu_pad_blocked(void);        // controllers: menu open, or not focused (option; never in splitscreen)
bool menu_auto_update(void);
int menu_upscale_mode(void);        // 0 off, 1 FSR 1
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
bool menu_game_input_blocked(void);   // menu open, or the game isn't focused (option)
void menu_filter_gamepad(XINPUT_GAMEPAD_ *g);   // what the game sees (menu open / card button hidden)
void menu_feed_gamepad(void);     // between backend NewFrame and ImGui::NewFrame
void menu_draw(UINT bb_width, UINT bb_height);
