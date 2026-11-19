// EXVS Overlay - in-game menu for EXVSIB (vsac29_Release.exe, DirectX 12).
//
// Loaded as a xinput9_1_0.dll proxy placed next to the game exe. XInput calls are forwarded to the
// real System32 DLL. On load, a worker thread creates a throwaway D3D12 device + swapchain to find
// the vtables of IDXGISwapChain and ID3D12CommandQueue, then patches:
//   IDXGISwapChain::Present / IDXGISwapChain1::Present1  -> draw the Dear ImGui overlay
//   IDXGISwapChain::ResizeBuffers                       -> drop our backbuffer references
//   ID3D12CommandQueue::ExecuteCommandLists             -> capture the game's DIRECT queue
// Nothing in the game folder is modified; deleting the DLL removes everything.

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx12.h"
#include "menu.h"
#include "framedata.h"
#include "upscale.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// ---------------------------------------------------------------------------------------------
// Logging + paths
// ---------------------------------------------------------------------------------------------
static char g_dir[MAX_PATH];          // folder of the game exe (with trailing backslash)
static CRITICAL_SECTION g_log_cs;

void ov_log(const char *fmt, ...) {
    char path[MAX_PATH]; snprintf(path, sizeof(path), "%sEXVSOverlay.log", g_dir);
    EnterCriticalSection(&g_log_cs);
    FILE *f = fopen(path, "a");
    if (f) {
        SYSTEMTIME t; GetLocalTime(&t);
        fprintf(f, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
        if (split_log_player()) fprintf(f, "[P%d] ", split_log_player());   // splitscreen: every copy writes here
        va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
        fputc('\n', f); fclose(f);
    }
    LeaveCriticalSection(&g_log_cs);
}

const char *ov_dir(void) { return g_dir; }

// ---------------------------------------------------------------------------------------------
// XInput proxy
// ---------------------------------------------------------------------------------------------
typedef DWORD (WINAPI *PGetState)(DWORD, XINPUT_STATE_ *);
typedef DWORD (WINAPI *PSetState)(DWORD, void *);
typedef DWORD (WINAPI *PGetCaps)(DWORD, DWORD, void *);
typedef DWORD (WINAPI *PGetGuids)(DWORD, GUID *, GUID *);
static HMODULE g_real_xinput;
static PGetState rGetState; static PSetState rSetState; static PGetCaps rGetCaps; static PGetGuids rGetGuids;
static INIT_ONCE g_xinput_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK load_real_xinput(PINIT_ONCE, PVOID, PVOID *) {
    char p[MAX_PATH]; GetSystemDirectoryA(p, MAX_PATH); strcat(p, "\\xinput9_1_0.dll");
    g_real_xinput = LoadLibraryA(p);
    if (g_real_xinput) {
        rGetState = (PGetState)GetProcAddress(g_real_xinput, "XInputGetState");
        rSetState = (PSetState)GetProcAddress(g_real_xinput, "XInputSetState");
        rGetCaps = (PGetCaps)GetProcAddress(g_real_xinput, "XInputGetCapabilities");
        rGetGuids = (PGetGuids)GetProcAddress(g_real_xinput, "XInputGetDSoundAudioDeviceGuids");
    }
    return TRUE;
}
static void ensure_xinput(void) { InitOnceExecuteOnce(&g_xinput_once, load_real_xinput, NULL, NULL); }

DWORD ov_real_xinput_get_state(DWORD i, XINPUT_STATE_ *s) {
    ensure_xinput();
    return rGetState ? rGetState(i, s) : ERROR_DEVICE_NOT_CONNECTED;
}

extern "C" {
// Splitscreen: this copy only has its player's controller, as controller 1.
static bool split_slot(DWORD *i) {
    if (!split_active()) return true;
    if (*i != 0 || split_pad() < 0) return false;
    *i = (DWORD)split_pad();
    return true;
}
__declspec(dllexport) DWORD WINAPI XInputGetState(DWORD i, XINPUT_STATE_ *s) {
    if (!split_slot(&i)) return ERROR_DEVICE_NOT_CONNECTED;
    DWORD r = ov_real_xinput_get_state(i, s);
    // While the menu is open the game sees an idle controller; the card button can be hidden too.
    if (r == ERROR_SUCCESS && s) menu_filter_gamepad(&s->Gamepad);
    return r;
}
__declspec(dllexport) DWORD WINAPI XInputSetState(DWORD i, void *s) { if (!split_slot(&i)) return ERROR_DEVICE_NOT_CONNECTED; ensure_xinput(); return rSetState ? rSetState(i, s) : ERROR_DEVICE_NOT_CONNECTED; }
__declspec(dllexport) DWORD WINAPI XInputGetCapabilities(DWORD i, DWORD f, void *c) { if (!split_slot(&i)) return ERROR_DEVICE_NOT_CONNECTED; ensure_xinput(); return rGetCaps ? rGetCaps(i, f, c) : ERROR_DEVICE_NOT_CONNECTED; }
__declspec(dllexport) DWORD WINAPI XInputGetDSoundAudioDeviceGuids(DWORD i, GUID *a, GUID *b) { if (!split_slot(&i)) return ERROR_DEVICE_NOT_CONNECTED; ensure_xinput(); return rGetGuids ? rGetGuids(i, a, b) : ERROR_DEVICE_NOT_CONNECTED; }
}

// ---------------------------------------------------------------------------------------------
// D3D12 overlay state
// ---------------------------------------------------------------------------------------------
typedef HRESULT (STDMETHODCALLTYPE *PPresent)(IDXGISwapChain *, UINT, UINT);
typedef HRESULT (STDMETHODCALLTYPE *PPresent1)(IDXGISwapChain1 *, UINT, UINT, const DXGI_PRESENT_PARAMETERS *);
typedef HRESULT (STDMETHODCALLTYPE *PResizeBuffers)(IDXGISwapChain *, UINT, UINT, UINT, DXGI_FORMAT, UINT);
typedef void (STDMETHODCALLTYPE *PExecuteCommandLists)(ID3D12CommandQueue *, UINT, ID3D12CommandList *const *);

static PPresent oPresent; static PPresent1 oPresent1; static PResizeBuffers oResizeBuffers;
static PExecuteCommandLists oExecuteCommandLists;

static ID3D12CommandQueue *g_queue;       // the game's DIRECT queue (captured, AddRef'd)
static ID3D12Device *g_device;
static IDXGISwapChain3 *g_swap;           // swapchain we initialized for (not AddRef'd)
static HWND g_hwnd; static WNDPROC g_orig_wndproc;
static bool g_imgui_ready; static bool g_init_failed;
static CRITICAL_SECTION g_imgui_cs;       // WndProc thread vs render thread

static const UINT MAX_BUFFERS = 8;
static UINT g_buffer_count;
static ID3D12Resource *g_backbuffers[MAX_BUFFERS];
static D3D12_CPU_DESCRIPTOR_HANDLE g_rtv[MAX_BUFFERS];
static ID3D12CommandAllocator *g_alloc[MAX_BUFFERS];
static UINT64 g_alloc_fence[MAX_BUFFERS];
static ID3D12DescriptorHeap *g_rtv_heap, *g_srv_heap;
static ID3D12GraphicsCommandList *g_cmdlist;
static ID3D12Fence *g_fence; static UINT64 g_fence_value; static HANDLE g_fence_event;
static DXGI_FORMAT g_rtv_format;
static UINT g_bb_width, g_bb_height;

static void wait_fence(UINT64 v) {
    if (!g_fence || v == 0 || g_fence->GetCompletedValue() >= v) return;
    g_fence->SetEventOnCompletion(v, g_fence_event);
    WaitForSingleObject(g_fence_event, 2000);
}

static void release_targets(void) {
    wait_fence(g_fence_value);
    for (UINT i = 0; i < MAX_BUFFERS; i++) {
        if (g_backbuffers[i]) { g_backbuffers[i]->Release(); g_backbuffers[i] = NULL; }
        if (g_alloc[i]) { g_alloc[i]->Release(); g_alloc[i] = NULL; }
        g_alloc_fence[i] = 0;
    }
    if (g_rtv_heap) { g_rtv_heap->Release(); g_rtv_heap = NULL; }
    g_buffer_count = 0;
}

static DXGI_FORMAT rtv_format_for(DXGI_FORMAT f) {
    switch (f) {   // backbuffers can't be typeless as RTV; pick the UNORM view
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default: return f;
    }
}

static bool create_targets(IDXGISwapChain3 *sc) {
    DXGI_SWAP_CHAIN_DESC1 d; if (FAILED(up_real_desc1(sc, &d))) return false;   // window-size buffers when upscaling
    g_buffer_count = d.BufferCount > MAX_BUFFERS ? MAX_BUFFERS : d.BufferCount;
    g_bb_width = d.Width; g_bb_height = d.Height;
    g_rtv_format = rtv_format_for(d.Format);

    D3D12_DESCRIPTOR_HEAP_DESC hd = {}; hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors = g_buffer_count;
    if (FAILED(g_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_rtv_heap)))) return false;
    UINT inc = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_CPU_DESCRIPTOR_HANDLE h = g_rtv_heap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < g_buffer_count; i++) {
        if (FAILED(up_real_get_buffer(sc, i, IID_PPV_ARGS(&g_backbuffers[i])))) return false;
        D3D12_RENDER_TARGET_VIEW_DESC rd = {}; rd.Format = g_rtv_format; rd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        g_rtv[i] = h;
        g_device->CreateRenderTargetView(g_backbuffers[i], &rd, h);
        h.ptr += inc;
        if (FAILED(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_alloc[i])))) return false;
    }
    ov_log("targets: %u buffers %ux%u format %d (rtv %d)", g_buffer_count, d.Width, d.Height, (int)d.Format, (int)g_rtv_format);
    return true;
}

// The backbuffer can differ from the window's client size (DXGI stretches it), and ImGui works in
// backbuffer pixels, so mouse positions from window messages are rescaled.
static volatile LONG g_client_w, g_client_h;
static volatile LONG g_mouse_msgs;   // mouse messages seen while the menu is open (diagnostics)

// The game doesn't always deliver mouse messages to the window the overlay hooks (the loader can
// capture them), so while the menu is open the mouse is also read straight from Windows each frame.
// Mouse diagnostics while the menu is open: a few samples while it's open and a summary when it
// closes, so a "no cursor in the menu" report can be told apart (no focus / mouse outside / no input).
static struct {
    LONG msgs_at_open; int downs, frames, unfocused, outside, samples;
    DWORD opened_at, last_fg_pid;
} g_md;

static void exe_name_of(DWORD pid, char *out, DWORD n) {
    out[0] = 0;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) { snprintf(out, n, "?"); return; }
    char path[MAX_PATH]; DWORD len = MAX_PATH;
    if (QueryFullProcessImageNameA(h, 0, path, &len)) {
        const char *b = strrchr(path, '\\');
        snprintf(out, n, "%s", b ? b + 1 : path);
    } else snprintf(out, n, "?");
    CloseHandle(h);
}

static void log_mouse_sample(const char *when) {
    HWND fg = GetForegroundWindow();
    DWORD pid = 0; if (fg) GetWindowThreadProcessId(fg, &pid);
    char exe[64]; exe_name_of(pid, exe, sizeof exe);
    bool fg_game = pid == GetCurrentProcessId();
    POINT s = {}, c = {};
    BOOL have = GetCursorPos(&s);
    c = s; if (g_hwnd) ScreenToClient(g_hwnd, &c);
    RECT wr = {}; if (g_hwnd) GetWindowRect(g_hwnd, &wr);
    float mx = -1, my = -1; bool valid = false;
    if (g_imgui_ready) {
        EnterCriticalSection(&g_imgui_cs);
        ImVec2 m = ImGui::GetIO().MousePos; mx = m.x; my = m.y; valid = ImGui::IsMousePosValid(&m);
        LeaveCriticalSection(&g_imgui_cs);
    }
    char mpos[48];
    if (!g_imgui_ready) snprintf(mpos, sizeof mpos, "not ready");
    else if (!valid) snprintf(mpos, sizeof mpos, "none (no cursor is drawn)");
    else snprintf(mpos, sizeof mpos, "%.0f,%.0f", mx, my);
    ov_log("mouse %s: cursor %s%ld,%ld (client %ld,%ld), window %ld,%ld-%ld,%ld client %ldx%ld backbuffer %ux%u, "
           "foreground %s (pid %lu %s, hwnd %s game's), menu mouse %s",
           when, have ? "" : "unknown ", s.x, s.y, c.x, c.y, wr.left, wr.top, wr.right, wr.bottom,
           (long)g_client_w, (long)g_client_h, g_bb_width, g_bb_height,
           fg_game ? "is the game" : "is NOT the game", pid, exe, fg == g_hwnd ? "=" : "!=",
           mpos);
}

// every frame, also while nothing is drawn (so the menu closing is always noticed)
static void mouse_diag_tick(void) {
    static bool was_open;
    bool open = menu_is_open();
    if (open != was_open) {
        was_open = open;
        if (open) {
            memset(&g_md, 0, sizeof g_md);
            g_md.msgs_at_open = g_mouse_msgs; g_md.opened_at = GetTickCount();
        } else {
            char exe[64] = "-"; if (g_md.unfocused) exe_name_of(g_md.last_fg_pid, exe, sizeof exe);
            ov_log("mouse while the menu was open: %ld window messages, %d clicks read directly, %d frames read, "
                   "%d frames without focus (last foreground: %s), %d frames with the mouse outside the game",
                   g_mouse_msgs - g_md.msgs_at_open, g_md.downs, g_md.frames, g_md.unfocused, exe, g_md.outside);
        }
    }
    if (!open) return;
    static const DWORD at_ms[] = { 1000, 4000, 10000 };
    if (g_md.samples < 3 && GetTickCount() - g_md.opened_at >= at_ms[g_md.samples]) {
        char when[32]; snprintf(when, sizeof when, "%lus after opening", at_ms[g_md.samples] / 1000);
        log_mouse_sample(when);
        g_md.samples++;
    }
}

static void poll_mouse(void) {
    if (!menu_is_open() || !g_hwnd) return;
    HWND fg = GetForegroundWindow();
    DWORD pid = 0; if (fg) GetWindowThreadProcessId(fg, &pid);
    if (pid != GetCurrentProcessId()) { g_md.unfocused++; g_md.last_fg_pid = pid; return; }   // only when the game has focus
    POINT p; if (!GetCursorPos(&p) || !ScreenToClient(g_hwnd, &p)) return;
    g_md.frames++;
    ImGuiIO &io = ImGui::GetIO();
    float x = (float)p.x, y = (float)p.y;
    if (g_client_w > 0 && g_client_h > 0) { x = x * g_bb_width / g_client_w; y = y * g_bb_height / g_client_h; }
    if (x < 0 || y < 0 || x >= g_bb_width || y >= g_bb_height) g_md.outside++;
    io.AddMousePosEvent(x, y);
    static bool prev[3];
    const int vks[3] = { VK_LBUTTON, VK_RBUTTON, VK_MBUTTON };
    for (int i = 0; i < 3; i++) {
        bool down = (GetAsyncKeyState(vks[i]) & 0x8000) != 0;
        if (down != prev[i]) { io.AddMouseButtonEvent(i, down); prev[i] = down; if (down && i == 0) g_md.downs++; }
    }
}

static LPARAM scale_mouse_lparam(UINT msg, LPARAM lp) {
    bool client_coords = msg == WM_MOUSEMOVE || (msg >= WM_LBUTTONDOWN && msg <= WM_MBUTTONDBLCLK) ||
                         (msg >= WM_XBUTTONDOWN && msg <= WM_XBUTTONDBLCLK);
    if (!client_coords || g_client_w <= 0 || g_client_h <= 0) return lp;
    int x = (short)LOWORD(lp), y = (short)HIWORD(lp);
    x = MulDiv(x, (int)g_bb_width, g_client_w);
    y = MulDiv(y, (int)g_bb_height, g_client_h);
    return MAKELPARAM((WORD)(short)x, (WORD)(short)y);
}

static LRESULT CALLBACK hk_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    // test host only: its window is hidden, so Windows reports "mouse left" right after every move
    static int ignore_leave = -1;
    if (ignore_leave < 0) ignore_leave = GetEnvironmentVariableA("EXVSOVERLAY_TEST_NOLEAVE", NULL, 0) ? 1 : 0;
    if (ignore_leave && msg == WM_MOUSELEAVE) return CallWindowProcW(g_orig_wndproc, hwnd, msg, wp, lp);
    if (g_imgui_ready && menu_is_open()) {
        if (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) InterlockedIncrement(&g_mouse_msgs);
        EnterCriticalSection(&g_imgui_cs);
        ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, scale_mouse_lparam(msg, lp));
        LeaveCriticalSection(&g_imgui_cs);
        if (menu_blocks_input()) {
            if ((msg >= WM_KEYFIRST && msg <= WM_KEYLAST) || (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST)) return 0;
            if (msg == WM_SETCURSOR && LOWORD(lp) == HTCLIENT) { SetCursor(NULL); return TRUE; }  // ImGui draws its own
        }
    }
    return CallWindowProcW(g_orig_wndproc, hwnd, msg, wp, lp);
}

static void srv_alloc(ImGui_ImplDX12_InitInfo *, D3D12_CPU_DESCRIPTOR_HANDLE *cpu, D3D12_GPU_DESCRIPTOR_HANDLE *gpu) {
    // This ImGui version only allocates the font texture: one descriptor is enough.
    *cpu = g_srv_heap->GetCPUDescriptorHandleForHeapStart();
    *gpu = g_srv_heap->GetGPUDescriptorHandleForHeapStart();
}
static void srv_free(ImGui_ImplDX12_InitInfo *, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_GPU_DESCRIPTOR_HANDLE) {}

static bool init_overlay(IDXGISwapChain3 *sc) {
    if (FAILED(sc->GetDevice(IID_PPV_ARGS(&g_device)))) { ov_log("GetDevice failed"); return false; }
    if (FAILED(sc->GetHwnd(&g_hwnd))) g_hwnd = NULL;
    if (!create_targets(sc)) { ov_log("create_targets failed"); return false; }

    D3D12_DESCRIPTOR_HEAP_DESC sd = {}; sd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; sd.NumDescriptors = 4;
    sd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(g_device->CreateDescriptorHeap(&sd, IID_PPV_ARGS(&g_srv_heap)))) { ov_log("srv heap failed"); return false; }
    if (FAILED(g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_alloc[0], NULL, IID_PPV_ARGS(&g_cmdlist)))) { ov_log("cmdlist failed"); return false; }
    g_cmdlist->Close();
    if (FAILED(g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence)))) { ov_log("fence failed"); return false; }
    g_fence_event = CreateEventW(NULL, FALSE, FALSE, NULL);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = NULL; io.LogFilename = NULL;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    menu_setup_style(g_bb_height);

    if (!ImGui_ImplWin32_Init(g_hwnd)) { ov_log("ImGui win32 init failed (hwnd %p)", (void *)g_hwnd); return false; }
    ImGui_ImplDX12_InitInfo ii;
    ii.Device = g_device; ii.CommandQueue = g_queue; ii.NumFramesInFlight = (int)g_buffer_count;
    ii.RTVFormat = g_rtv_format; ii.DSVFormat = DXGI_FORMAT_UNKNOWN;
    ii.SrvDescriptorHeap = g_srv_heap; ii.SrvDescriptorAllocFn = srv_alloc; ii.SrvDescriptorFreeFn = srv_free;
    if (!ImGui_ImplDX12_Init(&ii)) { ov_log("ImGui dx12 init failed"); return false; }

    if (g_hwnd) g_orig_wndproc = (WNDPROC)SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, (LONG_PTR)hk_wndproc);

    DXGI_ADAPTER_DESC ad = {}; IDXGIFactory4 *fac = NULL;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&fac)))) {
        IDXGIAdapter *a = NULL;
        if (SUCCEEDED(fac->EnumAdapterByLuid(g_device->GetAdapterLuid(), IID_PPV_ARGS(&a)))) { a->GetDesc(&ad); a->Release(); }
        fac->Release();
    }
    char gpu[128]; WideCharToMultiByte(CP_UTF8, 0, ad.Description, -1, gpu, sizeof(gpu), NULL, NULL);
    menu_set_gpu_name(gpu);
    ov_log("overlay ready: hwnd %p, gpu '%s'", (void *)g_hwnd, gpu);
    g_swap = sc;
    return true;
}

static void render_overlay(IDXGISwapChain *sc_base) {
    if (!g_hwnd) {                           // known early so the window can be set before the menu is used
        DXGI_SWAP_CHAIN_DESC d;
        if (SUCCEEDED(sc_base->GetDesc(&d))) g_hwnd = d.OutputWindow;
    }
    menu_tick_frame();                       // frame timing + hotkey polling, even when hidden
    if (g_init_failed || !g_queue) return;
    mouse_diag_tick();

    IDXGISwapChain3 *sc = NULL;
    if (FAILED(sc_base->QueryInterface(IID_PPV_ARGS(&sc)))) return;
    sc->Release();                           // the game keeps it alive; we only borrow it

    if (!g_imgui_ready) {
        if (!menu_should_draw()) return;     // initialize lazily, the first time something is shown
        if (!init_overlay(sc)) { g_init_failed = true; ov_log("overlay disabled after init failure"); return; }
        g_imgui_ready = true;
    }
    if (sc != g_swap) return;                // a second swapchain (unlikely): leave it alone
    if (!menu_should_draw()) return;         // nothing visible -> zero GPU work
    if (g_buffer_count == 0 && !create_targets(sc)) { release_targets(); return; }

    UINT idx = sc->GetCurrentBackBufferIndex();
    if (idx >= g_buffer_count) return;

    RECT rc;
    if (g_hwnd && GetClientRect(g_hwnd, &rc)) { g_client_w = rc.right - rc.left; g_client_h = rc.bottom - rc.top; }

    EnterCriticalSection(&g_imgui_cs);
    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::GetIO().DisplaySize = ImVec2((float)g_bb_width, (float)g_bb_height);
    poll_mouse();
    menu_feed_gamepad();
    ImGui::NewFrame();
    menu_draw(g_bb_width, g_bb_height);
    ImGui::Render();
    LeaveCriticalSection(&g_imgui_cs);

    wait_fence(g_alloc_fence[idx]);
    g_alloc[idx]->Reset();
    g_cmdlist->Reset(g_alloc[idx], NULL);
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = g_backbuffers[idx];
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    g_cmdlist->ResourceBarrier(1, &b);
    g_cmdlist->OMSetRenderTargets(1, &g_rtv[idx], FALSE, NULL);
    g_cmdlist->SetDescriptorHeaps(1, &g_srv_heap);
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), g_cmdlist);
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    g_cmdlist->ResourceBarrier(1, &b);
    g_cmdlist->Close();
    ID3D12CommandList *lists[] = { g_cmdlist };
    oExecuteCommandLists(g_queue, 1, lists);
    g_queue->Signal(g_fence, ++g_fence_value);
    g_alloc_fence[idx] = g_fence_value;
}

static HRESULT STDMETHODCALLTYPE hk_present(IDXGISwapChain *sc, UINT sync, UINT flags) {
    if (!(flags & DXGI_PRESENT_TEST)) { up_frame(sc); render_overlay(sc); }
    return oPresent(sc, sync, flags);
}
static HRESULT STDMETHODCALLTYPE hk_present1(IDXGISwapChain1 *sc, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS *p) {
    if (!(flags & DXGI_PRESENT_TEST)) { up_frame(sc); render_overlay(sc); }
    return oPresent1(sc, sync, flags, p);
}
static HRESULT STDMETHODCALLTYPE hk_resize_buffers(IDXGISwapChain *sc, UINT n, UINT w, UINT h, DXGI_FORMAT f, UINT fl) {
    if (g_imgui_ready) release_targets();    // must drop backbuffer refs or ResizeBuffers fails
    HRESULT r;
    if (up_resize(sc, n, w, h, f, fl, &r)) {  // upscaling: the game gets its own buffers of that size
        ov_log("ResizeBuffers(%u, %ux%u, fmt %d) -> 0x%08lx (game buffers; screen keeps the window size)", n, w, h, (int)f, (unsigned long)r);
        return r;
    }
    r = oResizeBuffers(sc, n, w, h, f, fl);
    ov_log("ResizeBuffers(%u, %ux%u, fmt %d) -> 0x%08lx", n, w, h, (int)f, (unsigned long)r);
    return r;                                // targets are recreated lazily on the next Present
}
static void STDMETHODCALLTYPE hk_execute_command_lists(ID3D12CommandQueue *q, UINT n, ID3D12CommandList *const *l) {
    if (!g_queue) {
        D3D12_COMMAND_QUEUE_DESC d = q->GetDesc();
        if (d.Type == D3D12_COMMAND_LIST_TYPE_DIRECT) {
            q->AddRef(); g_queue = q;
            ov_log("captured DIRECT command queue %p", (void *)q);
        }
    }
    oExecuteCommandLists(q, n, l);
}

// ---------------------------------------------------------------------------------------------
// Window size / borderless. Done on a worker thread: SetWindowPos from the render thread could
// deadlock if the window belongs to another thread that is waiting on rendering.
// ---------------------------------------------------------------------------------------------
HWND ov_game_hwnd(void) { return g_hwnd; }
ID3D12CommandQueue *ov_game_queue(void) { return g_queue; }
void ov_release_targets(void) { if (g_imgui_ready) release_targets(); }

struct WindowReq { int w, h; bool borderless; };

static DWORD WINAPI window_thread(LPVOID p) {
    WindowReq r = *(WindowReq *)p; delete (WindowReq *)p;
    HWND hwnd = g_hwnd; if (!hwnd) return 0;
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);
    bool visible = IsWindowVisible(hwnd) != FALSE;   // never show a hidden window (test host)
    LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    RECT rc;
    if (r.borderless) {
        style = (style & ~(WS_OVERLAPPEDWINDOW | WS_DLGFRAME | WS_BORDER)) | WS_POPUP | WS_VISIBLE;
        ex &= ~(WS_EX_DLGMODALFRAME | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE | WS_EX_WINDOWEDGE);
        rc = mi.rcMonitor;
    } else {
        style = (style & ~(WS_POPUP | WS_THICKFRAME | WS_MAXIMIZEBOX)) | WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_VISIBLE;
        ex |= WS_EX_WINDOWEDGE;
        RECT client = { 0, 0, r.w, r.h };
        AdjustWindowRectEx(&client, (DWORD)style, FALSE, (DWORD)ex);
        int ww = client.right - client.left, wh = client.bottom - client.top;
        const RECT &wa = mi.rcWork;
        int x = wa.left + ((wa.right - wa.left) - ww) / 2, y = wa.top + ((wa.bottom - wa.top) - wh) / 2;
        if (x < wa.left) x = wa.left;
        if (y < wa.top) y = wa.top;
        rc = { x, y, x + ww, y + wh };
    }
    if (!visible) style &= ~WS_VISIBLE;
    SetWindowLongPtrW(hwnd, GWL_STYLE, style);
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex);
    SetWindowPos(hwnd, HWND_TOP, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top,
                 SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_ASYNCWINDOWPOS | (visible ? SWP_SHOWWINDOW : 0));
    ov_log("window -> %s %dx%d at %ld,%ld", r.borderless ? "borderless" : "windowed",
           (int)(rc.right - rc.left), (int)(rc.bottom - rc.top), rc.left, rc.top);
    return 0;
}

void ov_set_window(int w, int h, bool borderless) {
    WindowReq *r = new WindowReq{ w, h, borderless };
    HANDLE t = CreateThread(NULL, 0, window_thread, r, 0, NULL);
    if (t) CloseHandle(t); else delete r;
}

// ---------------------------------------------------------------------------------------------
// Restart. The game can't reload its resolution, so it is closed and started again with the same
// command line, working directory and environment (+ EXVSOVERLAY_RESTARTED=1). The relaunch is done
// by a helper process (rundll32 running RestartHelperW from this DLL) that waits for the game to
// exit. The helper inherits the game's elevation.
// ---------------------------------------------------------------------------------------------
static bool g_is_helper;   // this DLL was loaded by rundll32 for RestartHelperW
static HMODULE g_self;

static void restart_file(wchar_t *out, size_t n) {
    wchar_t dir[MAX_PATH]; MultiByteToWideChar(CP_ACP, 0, g_dir, -1, dir, MAX_PATH);
    _snwprintf(out, n, L"%lsEXVSOverlay.restart", dir);
}

bool ov_restart_game(void) {
    // save "<cwd>\n<command line>" for the helper
    wchar_t path[MAX_PATH]; restart_file(path, MAX_PATH);
    wchar_t cwd[MAX_PATH]; GetCurrentDirectoryW(MAX_PATH, cwd);
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) { ov_log("restart: cannot write restart file (%lu)", GetLastError()); return false; }
    DWORD wr; const wchar_t *cmd = GetCommandLineW(), nl = L'\n';
    WriteFile(f, cwd, (DWORD)(wcslen(cwd) * 2), &wr, NULL); WriteFile(f, &nl, 2, &wr, NULL);
    WriteFile(f, cmd, (DWORD)(wcslen(cmd) * 2), &wr, NULL);
    CloseHandle(f);

    wchar_t self[MAX_PATH], sys[MAX_PATH], line[MAX_PATH * 2 + 64];
    GetModuleFileNameW(g_self, self, MAX_PATH);
    GetSystemDirectoryW(sys, MAX_PATH);
    _snwprintf(line, sizeof(line) / sizeof(line[0]), L"\"%ls\\rundll32.exe\" \"%ls\",RestartHelper %lu", sys, self, GetCurrentProcessId());
    STARTUPINFOW si = { sizeof(si) }; PROCESS_INFORMATION pi;
    if (!CreateProcessW(NULL, line, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, cwd, &si, &pi)) {
        ov_log("restart: helper failed to start (%lu)", GetLastError());
        DeleteFileW(path);
        return false;
    }
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    ov_log("restart: helper pid %lu started, closing the game", pi.dwProcessId);
    Sleep(300);
    TerminateProcess(GetCurrentProcess(), 0);   // same as the loader's Kill key
    return true;
}

extern "C" __declspec(dllexport) void CALLBACK RestartHelperW(HWND, HINSTANCE, LPWSTR args, int) {
    DWORD pid = (DWORD)wcstoul(args, NULL, 10);
    HANDLE game = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (game) { DWORD r = WaitForSingleObject(game, 30000); CloseHandle(game); if (r != WAIT_OBJECT_0) { ov_log("helper: game %lu did not exit", pid); return; } }
    Sleep(1500);   // let the old process release its files, sockets and the window

    wchar_t path[MAX_PATH]; restart_file(path, MAX_PATH);
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) { ov_log("helper: no restart file"); return; }
    static wchar_t buf[8192]; DWORD rd = 0;
    ReadFile(f, buf, sizeof(buf) - 2, &rd, NULL); CloseHandle(f); DeleteFileW(path);
    buf[rd / 2] = 0;
    wchar_t *nl = wcschr(buf, L'\n'); if (!nl) { ov_log("helper: bad restart file"); return; }
    *nl = 0; wchar_t *cwd = buf, *cmd = nl + 1;

    SetEnvironmentVariableW(L"EXVSOVERLAY_RESTARTED", L"1");
    STARTUPINFOW si = { sizeof(si) }; PROCESS_INFORMATION pi;
    if (CreateProcessW(NULL, cmd, NULL, NULL, FALSE, 0, NULL, cwd, &si, &pi)) {
        ov_log("helper: game restarted (pid %lu)", pi.dwProcessId);
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    } else {
        ov_log("helper: relaunch failed (%lu)", GetLastError());
    }
}

// ---------------------------------------------------------------------------------------------
// Hook installation (vtable patching)
// ---------------------------------------------------------------------------------------------
template <typename T> static void patch_vtable(void **vtbl, int index, void *hook, T *orig) {
    if (vtbl[index] == hook) return;
    DWORD old; VirtualProtect(&vtbl[index], sizeof(void *), PAGE_EXECUTE_READWRITE, &old);
    *orig = (T)vtbl[index];
    InterlockedExchangePointer(&vtbl[index], hook);
    VirtualProtect(&vtbl[index], sizeof(void *), old, &old);
}

static bool install_hooks(void) {
    WNDCLASSEXW wc = {}; wc.cbSize = sizeof(wc); wc.lpfnWndProc = DefWindowProcW; wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"EXVSOverlayDummy";
    RegisterClassExW(&wc);
    HWND w = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, NULL, NULL, wc.hInstance, NULL);

    bool ok = false;
    IDXGIFactory4 *fac = NULL; ID3D12Device *dev = NULL; ID3D12CommandQueue *q = NULL; IDXGISwapChain1 *sc = NULL;
    do {
        if (!w) { ov_log("dummy window failed"); break; }
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&fac)))) { ov_log("CreateDXGIFactory1 failed"); break; }
        up_install_factory(*(void ***)fac);   // first: the game may create its swapchain any moment now
        up_bypass(true);
        if (FAILED(D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev)))) { ov_log("D3D12CreateDevice failed"); break; }
        D3D12_COMMAND_QUEUE_DESC qd = {}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (FAILED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&q)))) { ov_log("CreateCommandQueue failed"); break; }
        DXGI_SWAP_CHAIN_DESC1 sd = {}; sd.Width = 64; sd.Height = 64; sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.SampleDesc.Count = 1; sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.BufferCount = 2;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        HRESULT scr = fac->CreateSwapChainForHwnd(q, w, &sd, NULL, NULL, &sc);
        up_bypass(false);
        if (FAILED(scr)) { ov_log("dummy swapchain failed"); break; }

        void **sc_vt = *(void ***)sc;
        void **q_vt = *(void ***)q;
        patch_vtable(q_vt, 10, (void *)hk_execute_command_lists, &oExecuteCommandLists);
        patch_vtable(sc_vt, 13, (void *)hk_resize_buffers, &oResizeBuffers);
        patch_vtable(sc_vt, 22, (void *)hk_present1, &oPresent1);
        patch_vtable(sc_vt, 8, (void *)hk_present, &oPresent);
        up_install_swapchain(sc_vt, (void *)oResizeBuffers);
        ov_log("hooks installed (Present %p, Present1 %p, ResizeBuffers %p, ExecuteCommandLists %p)",
               (void *)oPresent, (void *)oPresent1, (void *)oResizeBuffers, (void *)oExecuteCommandLists);
        ok = true;
    } while (0);
    up_bypass(false);
    if (sc) sc->Release();
    if (q) q->Release();
    if (dev) dev->Release();
    if (fac) fac->Release();
    if (w) DestroyWindow(w);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return ok;
}

static DWORD WINAPI init_thread(LPVOID) {
    // an update while the game was running leaves the previous DLL renamed to xinput9_1_0.old.dll
    split_init();   // splitscreen settings of this copy (logs which player it is)
    char old[MAX_PATH]; snprintf(old, sizeof(old), "%sxinput9_1_0.old.dll", g_dir);
    if (DeleteFileA(old)) ov_log("removed leftover %s", old);
    menu_load_config();
    // splitscreen: only player 1's copy looks for updates (the others would download the same one)
    upd_startup(menu_enabled() && menu_auto_update() && split_player() <= 1);
    if (!menu_enabled()) { ov_log("overlay disabled in EXVSOverlay.ini (enabled=0)"); return 0; }
    focus_init();                         // keyboard / controller while the game isn't focused
    if (menu_hud_enabled()) hud_init();   // hide-HUD detours: early, before the game draws any HUD
    if (!install_hooks()) ov_log("hook installation failed; the game runs normally without the overlay");
    fd_init(GetCurrentProcess(), (uintptr_t)GetModuleHandleW(NULL));   // Training tools (checks the exe version itself)
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        g_self = inst;
        InitializeCriticalSection(&g_log_cs);
        InitializeCriticalSection(&g_imgui_cs);
        char exe[MAX_PATH]; GetModuleFileNameA(NULL, exe, MAX_PATH);
        const char *exe_name = strrchr(exe, '\\'); exe_name = exe_name ? exe_name + 1 : exe;
        if (_stricmp(exe_name, "rundll32.exe") == 0) {
            // restart helper: game folder = folder of this DLL, no hooks
            g_is_helper = true;
            GetModuleFileNameA(inst, g_dir, MAX_PATH);
            char *s = strrchr(g_dir, '\\'); if (s) s[1] = 0;
            return TRUE;
        }
        GetModuleFileNameA(NULL, g_dir, MAX_PATH);
        char *s = strrchr(g_dir, '\\'); if (s) s[1] = 0;
        ov_log("---- EXVS Overlay %s loaded into %s (pid %lu)", OVERLAY_VERSION, exe, GetCurrentProcessId());

        HANDLE t = CreateThread(NULL, 0, init_thread, NULL, 0, NULL);
        if (t) CloseHandle(t);
    }
    return TRUE;
}
