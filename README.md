# EXVS Overlay

In-game menu for EXVSIB (`vsac29_Release.exe`, DirectX 12), drawn with Dear ImGui.

## Install / remove
The easiest way is [EXVS Launcher](https://github.com/JoseZavala98/EXVSLauncher): it bundles the DLL and installs or
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

## License
MIT, see [LICENSE](LICENSE). Third-party code (Dear ImGui, AMD FidelityFX FSR 1, both MIT) is listed in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
Fan-made tool, not affiliated with Bandai Namco or Sunrise. No game files are included.
