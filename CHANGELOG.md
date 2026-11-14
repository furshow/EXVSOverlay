# Changelog

All notable changes to EXVS Overlay. The release workflow (.github/workflows/build-release.yml) uses the
section of the released version as the release notes.

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
