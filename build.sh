#!/usr/bin/env bash
# Builds build/xinput9_1_0.dll (the overlay) and build/test_host.exe with zig (pip package "ziglang").
set -euo pipefail
cd "$(dirname "$0")"
# Python command: "py" (Windows launcher) by default; the GitHub workflow sets PY=python
PY="${PY:-py}"
ZIG="$PY -m ziglang"
IM=third_party/imgui
mkdir -p build
FLAGS="-target x86_64-windows-gnu -O2 -std=c++17 -fno-exceptions -fno-rtti -I$IM -I$IM/backends -Isrc -DWIN32_LEAN_AND_MEAN"

$ZIG c++ $FLAGS -shared -o build/xinput9_1_0.dll \
  src/overlay.cpp src/menu.cpp src/hud_hide.cpp src/upscale.cpp src/update.cpp src/framedata.cpp src/input_focus.cpp src/split.cpp src/training.cpp \
  $IM/imgui.cpp $IM/imgui_draw.cpp $IM/imgui_tables.cpp $IM/imgui_widgets.cpp \
  $IM/backends/imgui_impl_win32.cpp $IM/backends/imgui_impl_dx12.cpp \
  -DIMGUI_IMPL_WIN32_DISABLE_GAMEPAD \
  -ld3d12 -ldxgi -ld3dcompiler_47 -luser32 -lgdi32 -ldwmapi -limm32 -lwinhttp -lbcrypt

$ZIG c++ $FLAGS -o build/test_host.exe test/test_host.cpp -ld3d12 -ldxgi -luser32
rm -f build/*.lib build/*.pdb 2>/dev/null || true
ls -la build
