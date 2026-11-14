# EXVS Overlay

In-game menu for EXVSIB (`vsac29_Release.exe`, DirectX 12), drawn with Dear ImGui.

## Install / remove
The easiest way is [EXVS Launcher](https://github.com/furshow/EXVSLauncher): it bundles the DLL and installs or
removes it from **Settings > In-game overlay**. By hand:

1. Copy `xinput9_1_0.dll` (from the releases, or `build/xinput9_1_0.dll` after building) next to `vsac29_Release.exe`. If a `xinput9_1_0.dll` is already there, back it up first.
2. Start the game as usual. Press **Insert** (or **Back + Start** on a controller) to open/close the menu.
3. To remove it, delete `xinput9_1_0.dll`. No game files are changed.

The DLL writes `EXVSOverlay.ini` (settings) and `EXVSOverlay.log` (diagnostics) next to the exe.
Set `enabled=0` in the ini to turn it off without deleting the DLL.

## How it works
- The game imports `XINPUT9_1_0.dll`, so a proxy with that name loads at startup. It forwards the four
  XInput exports to `System32\xinput9_1_0.dll`. While the menu is open the game gets an idle controller.
- A worker thread creates a throwaway D3D12 device + swapchain + queue to find the vtables, then patches
  `Present` (8), `ResizeBuffers` (13), `Present1` (22) and `ID3D12CommandQueue::ExecuteCommandLists` (10).
- The first DIRECT queue that runs command lists is captured and used to render the overlay.
- ImGui is initialized on demand, the first time something is shown. When nothing is shown, no GPU work is added.
- ImGui works in backbuffer pixels. Mouse messages are rescaled from window client size to backbuffer size.

## Build
Clone with submodules (Dear ImGui is a git submodule in `third_party/`):

```bash
git clone --recursive <repo url>
# or, in an existing clone: git submodule update --init
```

Requires Windows, Python 3 and the pip package `ziglang` (used as the C++ compiler through `py -m ziglang`).
No other SDK is needed. From Git Bash:

    bash build.sh

This produces `build/xinput9_1_0.dll` and `build/test_host.exe`. `test_host.exe` is a minimal D3D12 app in a
hidden window. It loads the DLL, renders frames (with a `ResizeBuffers` in the middle), and saves
`build/overlay_test.bmp` with the menu forced open (`EXVSOVERLAY_TEST=1`).

## Input while the game isn't focused
Menu > Controls > "When the game isn't focused" (ini ignore_unfocused, on by default). The game reads its controls
through the loader (bngrw.dll / lmgrw.dll), which keeps getting them in the background, so typing in a browser reached
the game. src/input_focus.cpp patches the loader's imports of GetAsyncKeyState, GetRawInputData, HidP_GetUsages and
HidP_GetUsageValue (only in the loader's IAT): while another window has the focus, keys read as up, raw keyboard and
mouse events become releases, no controller button is pressed, the hat and the X/Y axes are centred and other axes
keep their last value. XInput pads are filtered in our own XInputGetState. The same filter applies while the menu is
open with "Game ignores my keyboard, mouse and controller". test/focus_test.cpp checks it with a fake loader.

## Training tools (frame meter, skeleton view)
Menu > Training. Client 1.13.30 only (checked through the exe's PE timestamp and size) and only while a Training or
Practice-vs-CPU battle is running (the battle-flow object's class); in any other mode, online included, they stay off.
src/framedata.cpp reads the game with ReadProcessMemory (never writes): units and the camera are found by their RTTI
vtables in a background heap scan, then read every frame. test/fd_test.cpp runs the same code outside the game against
the running exe and prints what the meter would show.

## Updates
The overlay can update itself from the releases of this repo (src/update.cpp). About 20 s after the game starts it asks
the GitHub API for the latest release once (turn it off in **About > Updates**, ini auto_update=0, or set
EXVSOVERLAY_NO_UPDATE=1). If the tag is newer than OVERLAY_VERSION, the menu shows it and **Download and install**
downloads xinput9_1_0.dll as xinput9_1_0.new.dll, checks its size and SHA-256 (GitHub's asset digest), renames the
loaded DLL to xinput9_1_0.old.dll and puts the new one in place. It runs from the next game start (the menu offers a
restart); the .old.dll is deleted then.

The network part (check and download) runs in a helper process, rundll32 xinput9_1_0.dll,UpdateHelper check|download:
inside the game the loader (bngrw.dll) hooks the sockets for the card server and WinHTTP can't reach GitHub (error 12029).
The helper writes its result and progress to EXVSOverlay.update.ini next to the game exe; the game side then swaps the DLL.

The DLL carries the marker EXVSOverlayVersion=x.y.z; so EXVS Launcher knows an overlay that updated itself is newer
than the one it bundles and doesn't offer to replace it.

To publish a version: bump OVERLAY_VERSION in src/menu.h, build, and create a GitHub release whose tag is the version
(v0.13.0 or 0.13.0) with the bare build/xinput9_1_0.dll attached under exactly that name.


## Releases
Branching follows git flow: work goes to `develop` (feature branches from it), a `Release/x.y.z` branch is cut from
`develop` for each version and merged into `main` (and back into `develop`); fixes to a released version go in
`hotfix/x.y.z` from `main`.

To publish a version:
1. On the `Release/x.y.z` branch: set OVERLAY_VERSION in src/menu.h and add a `## [x.y.z]` section to CHANGELOG.md.
2. Merge it into `main`, then tag main: `git tag vx.y.z && git push origin vx.y.z`.
3. `.github/workflows/build-release.yml` builds the DLL, runs the tests that need no GPU and creates a draft release
   with `xinput9_1_0.dll` (the updater needs exactly that name) and the CHANGELOG notes. Review it and press
   **Publish**: from then on the overlay's updater offers it.
4. To bundle it in EXVS Launcher, put the version in the launcher's `overlay-version.txt` and release the launcher.

## License
MIT, see [LICENSE](LICENSE). Third-party code (Dear ImGui, AMD FidelityFX FSR 1, both MIT) is listed in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
Fan-made tool, not affiliated with Bandai Namco or Sunrise. No game files are included.
