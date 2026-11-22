# Changelog

All notable changes to EXVS Overlay. The release workflow (.github/workflows/build-release.yml) uses the
section of the released version as the release notes.

## [Unreleased]
### Custom Training
- **5 and 6 units**: 1v4, 2v3, 3v2, 4v1, and 1v5, 2v4, 3v3, 4v2, 5v1, from the launcher's CPU slots 5 and 6
  (`Unit5..6` / `Cpu5..6` / `Ex5..6` / `Team5..6` / `On5..6` in `Training\training.ini`). The game's training list
  holds 4 units, so the bridge runs 4 and the overlay hands the match a copy of the list with 1-2 more entries
  (game 0x7A0774): each a copy of a CPU of the same team with that slot's unit, CPU behavior and EX. Three places made for 4 are adjusted while it's on: the battle log upload
  keeps 4 rows (it overflowed its stack and closed the game), the bridge's result screen keeps the first 4 rows,
  and the game's result builder skips the player its 4-row table doesn't have. The result screen shows you and
  the first 3 others; the rematch keeps all the units. 5 units tried in game (1v4 and others: match, result
  screen, rematch); 6 units not yet.
- The bridge gets up to 2 of your CPUs and at least one enemy (the first ones, in slot order); the other CPUs are
  the extra units. With 3 CPUs or fewer, slots 5-6 can stand in for slots that are off.
- While extra units are on, a stack overrun or exception in the game is logged (where, and the call chain) before
  the game ends, to find any other 4-unit limit.

## [1.2.1] - 2026-11-21
### Custom Training
- **Any layout up to 4 units**: 1v1, 1v2, 2v1, 1v3, 2v2 and 3v1, from the launcher's per-CPU teams (`Team2..4` /
  `On2..4` in `Training\training.ini`; the old `Partner` key still works). The launcher's slots go into the bridge's
  4 positions with your CPUs first and the enemies after, so a 2v2 is the bridge's own; for the others the bridge
  runs its 2v2 and the overlay moves positions to the other team or takes them out after the bridge's setup and
  again before every rematch. The result screen gets made-up rows for positions taken out (on the team that has
  fewer than 2), and its 2-per-team check is off for those layouts. 1v2 / 1v3 without team cost keep the game's
  own training.
- **1v3 with team cost** now has the **result screen** and the **automatic rematch**, staying 1v3:
  - the bridge's result data wanted 2 players per team and gave up with 1 vs 3 (no result screen, back to the
    menu). In 1v3 those two checks are turned off in the loaded bridge (memory only, the file stays unmodified)
  - its rematch makes the match again from the training slots, with the teams put back as 2v2: a detour on the
    game's match restart (RVA 0x7A0710) moves the partner to the enemy team again right before
- **1v2 with team cost** now has the **result screen** and the **automatic rematch** too, staying 1v2:
  - the bridge's result data is made for 4 players (2 teams of 2, for the game's result builder). In 1v2 a 4th
    row is added for the partner that was taken out (a copy of an enemy's row with the partner's unit, on your
    team), and the bridge's check that every row got its place after the game's builder is skipped for it
  - its rematch waited for a finished result screen (phase 3, no errors); in 1v2 that check is off too, so a
    failed result screen still leads to the rematch. The partner is taken out again at the match restart
- When a 1v3 / 1v2 match with team cost ends, the bridge's finish state (and the rest of its rule state) is
  logged, so a failed result screen / rematch can be told apart.

## [1.2.0] - 2026-11-21
### Custom Training (EXVS Launcher)
- New: VS Training as a 2v2, 1v2 or 1v3, set up on the launcher's **Custom Training** page (your unit, partner
  and two enemies, CPU behavior and EX of each one, random stage, team cost). When it's on, the overlay loads
  `Training\training_bridge.dll` (aerial's 2v2 Training Extension 1.1 bridge, shipped unmodified by the launcher)
  inside the game and applies the launcher's settings from `Training\training.ini` whenever they change; the state
  goes to `Training\status.ini`.
- It checks the build instead of exact file hashes: any 1.13.30 client works (PE build stamp `0x69E2081B`),
  with or without other exe mods, as long as the two 2v2 code spots are untouched. Only the 2v2 patches of
  aerial's `isohook_2v2.dll` are applied; that DLL itself isn't loaded. No DLL injection, no administrator rights.
- Not in splitscreen copies or the Live Monitor. Runs even with the overlay menu turned off.
- Random EX: a new F / S / E is drawn for every match of a slot set to Random.
- Random unit by category (a cost or the bosses, from the launcher's `SlotPool<n>`): the overlay picks one for
  every match (the bridge only has one random list for all slots).
- **1v2** (partner off in the launcher): the bridge only runs 4 units, so it isn't loaded. The overlay applies only
  the player-count and gate patches (the game then builds you + 2 CPUs) and hooks the training builder to write the
  enemies' unit, EX and CPU behavior into those 3 slots. **1v3** (partner on the enemy team) adds the bridge's
  4th-unit patch, so the game builds 4 units, and the partner slot goes to the enemy team. Random stage works there
  too (a playable stage from the game's list, written into the training match like the bridge does). Start is
  hidden while that training runs. Switching from 2v2 to 1v2 needs a game restart once the bridge has been loaded.
- **1v3 with team cost** (experimental): the bridge runs its normal 2v2 (so its team cost, COST OVER, finish, result
  screen work) and a second detour, chained in front of the bridge's on the training builder, moves the partner
  slot to the enemy team right after the bridge fills the slots. The bridge's VS intro is off for it (its VS screen
  lines up 2 per side and the game crashed there), so switching between 2v2 and 1v3 with team cost needs a game
  restart; no automatic rematch yet. The bridge's finish / rematch state is logged when a match ends. 1v3 without
  team cost keeps the bridge-less path.
- **1v2 with team cost** (experimental): like 1v3, the bridge runs its 2v2 (team cost with the real unit costs,
  COST OVER, finish) and the chained builder detour then takes the partner slot out (4 units -> 3, the enemy
  slots moved down). Same limits as 1v3 with team cost: no VS intro, no automatic rematch, a game restart to
  switch from or to 2v2.
- During a 2v2 match the game doesn't see **Start** (keyboard key, XInput Start / `ArcadeStartButton`, DirectInput
  `Start` buttons of `config.ini`): the training pause menu is made for 1v1 and froze the game. Start works again
  when the match is over.
- When the bridge refuses to start, the 2v2 code patches are undone (the game stays a normal 1v1 instead of 1v2
  with default units). Bosses made playable by the Mod Manager get their boss bit back in memory only while
  the bridge checks the game.

## [1.1.0] - 2026-11-19
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
