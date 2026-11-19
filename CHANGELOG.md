# Changelog

All notable changes to EXVS Overlay. The release workflow (.github/workflows/build-release.yml) uses the
section of the released version as the release notes.

## [Unreleased]
### Splitscreen (EXVS Launcher)
- When the game is started for one player of a splitscreen session (a `[split]` section in that player's
  `config.ini`), the copy only sees its own Xbox / XInput controller, as controller 1. The keyboard only
  reaches the keyboard player's copy, and controllers keep working while another copy has the focus.
- The card / coin buttons on a controller press the key for that copy only.
- The window isn't resized by the overlay there (the launcher tiles the windows), and only player 1's copy
  checks for updates. Log lines start with the player (`[P2]`).
- The overlay reads `config.ini` from the folder the game was started with (`vsac29_Release.exe "<folder>"`),
  like the loader does.

## [1.0.1] - 2026-11-14
### Controllers
- DirectInput controllers (arcade sticks in PS3/PS4 mode, e.g. Hori Fighting Stick Mini 4) now show in the
  controller overlay and work for the menu (Back + Start, navigation). They're read from what the game's
  loader already receives, with the PS3/PS4 button order (Square, Cross, Circle, Triangle, L1, R1, L2, R2...).
- New look for the arcade-stick controller overlay, in the style of fighting-game training modes: a rounded
  octagonal gate where the ball slides to each direction and leaves a fading light trail, and each button has its own
  colour (dim at rest, lit when pressed).

### Menu
- More details in `EXVSOverlay.log` when the mouse doesn't work in the menu (focus, cursor position, window size),
  to help with bug reports.

## [1.0.0] - 2026-11-14
First public release.

### Menu
- In-game menu drawn with Dear ImGui: **Insert**, or hold **Back + Start** on a controller for half a second.
- Works on DirectX 12 and on vkd3d-proton / DXVK.

### Display
- Game resolution (all SnowLauncher resolutions) and window size (all 24 SnowLauncher sizes), saved in
  `SnowLauncher.ini` so the launcher sees the same values. "Save and restart game" applies the resolution.
- Keeps the window size you chose. This fixes the game not going fullscreen when started from the launcher.
- Controller overlay (input display), Xbox controller or arcade stick style.
- Draggable FPS counter.
- Hide the battle HUD (**F9**). Based on HideUI (MIT, EXVS2OB-BGMTool contributors).

### Graphics
- AMD FSR 1 upscaling (EASU + RCAS) with a sharpness setting. Off by default; needs a game restart.

### Controls
- Arcade buttons: hold a controller button to press the game's Card or Coin key. These buttons can be hidden
  from the game.
- The game ignores your keyboard and controller while another window has the focus (browser, Discord, ...).
  On by default.
- The game ignores your keyboard, mouse and controller while the menu is open.

### Training (Client 1.13.30, Training / Practice only)
- Frame meter (startup, active, recovery, advantage).
- Hurtboxes, hitboxes and skeleton view.

### Updates
- Checks the releases of this repo when the game starts and installs the new DLL from **About > Updates**.
