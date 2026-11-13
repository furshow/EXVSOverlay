// Upscaling for EXVS Overlay: AMD FidelityFX Super Resolution 1 (EASU upscale + RCAS sharpen).
//
// The game always renders at the resolution in config.ini (720p / 1080p / 2K / 4K) into its swapchain
// buffers, and the image is stretched to the window when presented. To upscale it ourselves the
// swapchain is taken over:
//   - the real swapchain buffers are made the size of the window's client area;
//   - the game is handed its own textures of the size it asked for ("fake" buffers) through hooked
//     IDXGISwapChain::GetBuffer / GetDesc / GetDesc1 / ResizeBuffers(1);
//   - on Present, the game's texture is upscaled into the real buffer (EASU, then RCAS), and the
//     overlay menu is drawn on top at full window resolution.
// This only happens when the upscaler was on when the game started (the swapchain has to be taken
// over when it is created or resized). Off at start = nothing here is touched.
//
// The shaders are a port of AMD FidelityFX FSR 1.0 (ffx_fsr1.h), MIT License,
// Copyright (c) 2021 Advanced Micro Devices, Inc.
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include "menu.h"
#include "upscale.h"

typedef HRESULT (STDMETHODCALLTYPE *PGetBuffer)(IDXGISwapChain *, UINT, REFIID, void **);
typedef HRESULT (STDMETHODCALLTYPE *PGetDesc)(IDXGISwapChain *, DXGI_SWAP_CHAIN_DESC *);
typedef HRESULT (STDMETHODCALLTYPE *PGetDesc1)(IDXGISwapChain1 *, DXGI_SWAP_CHAIN_DESC1 *);
typedef HRESULT (STDMETHODCALLTYPE *PResizeBuffers)(IDXGISwapChain *, UINT, UINT, UINT, DXGI_FORMAT, UINT);
typedef HRESULT (STDMETHODCALLTYPE *PResizeBuffers1)(IDXGISwapChain3 *, UINT, UINT, UINT, DXGI_FORMAT, UINT, const UINT *, IUnknown *const *);
typedef HRESULT (STDMETHODCALLTYPE *PCreateSwapChain)(IDXGIFactory *, IUnknown *, DXGI_SWAP_CHAIN_DESC *, IDXGISwapChain **);
typedef HRESULT (STDMETHODCALLTYPE *PCreateSwapChainForHwnd)(IDXGIFactory2 *, IUnknown *, HWND, const DXGI_SWAP_CHAIN_DESC1 *,
                                                            const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *, IDXGIOutput *, IDXGISwapChain1 **);
// MSVC ABI for a COM method returning a struct: hidden result pointer after `this`
typedef D3D12_RESOURCE_DESC *(STDMETHODCALLTYPE *PResGetDesc)(ID3D12Resource *, D3D12_RESOURCE_DESC *);
typedef void (STDMETHODCALLTYPE *PCreateView)(ID3D12Device *, ID3D12Resource *, const void *, D3D12_CPU_DESCRIPTOR_HANDLE);

static PGetBuffer oGetBuffer; static PGetDesc oGetDesc; static PGetDesc1 oGetDesc1;
static PResizeBuffers oResize; static PResizeBuffers1 oResize1;
static PCreateSwapChain oCreateSC; static PCreateSwapChainForHwnd oCreateSCHwnd;
static PResGetDesc oResGetDesc; static PCreateView oCreateRTV, oCreateSRV;

static const UINT MAXB = 8;
static bool s_wanted;            // upscaler was on at startup -> take over the game's swapchain
static bool s_failed;            // couldn't take it over; the game runs as usual
static bool s_active;
static volatile LONG s_bypass;   // creating our own dummy swapchain
static IDXGISwapChain *s_sc;     // the game's swapchain (not AddRef'd; the game keeps it alive)
static IDXGISwapChain3 *s_sc3;
static HWND s_hwnd;
static ID3D12Device *s_dev;
static ID3D12CommandQueue *s_queue;   // the queue the swapchain was created on (AddRef'd), or NULL
static UINT s_count, s_fw, s_fh, s_usage, s_flags;   // what the game sees
static DXGI_FORMAT s_ffmt;
static UINT s_rw, s_rh;                              // real buffers (= window client)
static ID3D12Resource *s_fake[MAXB], *s_real[MAXB], *s_mid;
static DXGI_FORMAT s_real_rtv_fmt;

// pipeline
static ID3D12DescriptorHeap *s_srv_heap, *s_rtv_heap;   // srv: fakes 0..7, mid 8; rtv: reals 0..7, mid 8
static UINT s_srv_inc, s_rtv_inc;
static ID3D12RootSignature *s_rs;
static ID3D12PipelineState *s_pso_easu, *s_pso_rcas, *s_pso_copy;
static DXGI_FORMAT s_pso_fmt;
static ID3D12CommandAllocator *s_alloc[MAXB]; static UINT64 s_alloc_fence[MAXB];
static ID3D12GraphicsCommandList *s_cl;
static ID3D12Fence *s_fence; static UINT64 s_fv; static HANDLE s_ev;
static bool s_pipe_failed;
static int s_last_path = -1;
// DXVK implements ResizeBuffers through ResizeBuffers1 and CreateSwapChain through CreateSwapChainForHwnd
// (virtual calls, so through our hooks again): inner calls pass straight through.
static thread_local int s_depth;
struct Depth { Depth() { s_depth++; } ~Depth() { s_depth--; } };

template <typename T> static void patch(void **vt, int i, void *hook, T *orig) {
    if (vt[i] == hook) return;
    DWORD old; VirtualProtect(&vt[i], sizeof(void *), PAGE_EXECUTE_READWRITE, &old);
    *orig = (T)vt[i];
    InterlockedExchangePointer(&vt[i], hook);
    VirtualProtect(&vt[i], sizeof(void *), old, &old);
}

static DXGI_FORMAT typeless_of(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_TYPELESS;
    case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_TYPELESS;
    case DXGI_FORMAT_R10G10B10A2_UNORM: return DXGI_FORMAT_R10G10B10A2_TYPELESS;
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return DXGI_FORMAT_R16G16B16A16_TYPELESS;
    default: return f;
    }
}
static bool is_typeless(DXGI_FORMAT f) {
    return f == DXGI_FORMAT_R8G8B8A8_TYPELESS || f == DXGI_FORMAT_B8G8R8A8_TYPELESS ||
           f == DXGI_FORMAT_R10G10B10A2_TYPELESS || f == DXGI_FORMAT_R16G16B16A16_TYPELESS;
}
static DXGI_FORMAT view_of(DXGI_FORMAT f) {   // typed view of a swapchain format
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default: return f;
    }
}

static bool is_fake(ID3D12Resource *r) {
    if (!r || !s_active) return false;
    for (UINT i = 0; i < MAXB; i++) if (s_fake[i] == r) return true;
    return false;
}

static void client_size(HWND h, UINT *w, UINT *hh) {
    RECT rc = {};
    if (h && GetClientRect(h, &rc)) { *w = (UINT)(rc.right - rc.left); *hh = (UINT)(rc.bottom - rc.top); }
    else *w = *hh = 0;
}

static void wait_all(void) {
    if (!s_fence || s_fv == 0 || s_fence->GetCompletedValue() >= s_fv) return;
    s_fence->SetEventOnCompletion(s_fv, s_ev);
    WaitForSingleObject(s_ev, 2000);
}

// ---------------------------------------------------------------------------------------------
// Hooks that keep the game's view consistent: its buffers are typeless textures (so it can make
// UNORM or sRGB views of them, like with real swapchain buffers) that report the swapchain format.
// ---------------------------------------------------------------------------------------------
static D3D12_RESOURCE_DESC *STDMETHODCALLTYPE hk_res_get_desc(ID3D12Resource *r, D3D12_RESOURCE_DESC *out) {
    D3D12_RESOURCE_DESC *p = oResGetDesc(r, out);
    if (is_fake(r)) out->Format = s_ffmt;
    return p;
}
static void STDMETHODCALLTYPE hk_create_rtv(ID3D12Device *d, ID3D12Resource *r, const void *desc, D3D12_CPU_DESCRIPTOR_HANDLE h) {
    D3D12_RENDER_TARGET_VIEW_DESC v;
    if (is_fake(r)) {
        const D3D12_RENDER_TARGET_VIEW_DESC *in = (const D3D12_RENDER_TARGET_VIEW_DESC *)desc;
        if (!in) { v = {}; v.Format = view_of(s_ffmt); v.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D; desc = &v; }
        else if (is_typeless(in->Format) || in->Format == DXGI_FORMAT_UNKNOWN) { v = *in; v.Format = view_of(s_ffmt); desc = &v; }
    }
    oCreateRTV(d, r, desc, h);
}
static void STDMETHODCALLTYPE hk_create_srv(ID3D12Device *d, ID3D12Resource *r, const void *desc, D3D12_CPU_DESCRIPTOR_HANDLE h) {
    D3D12_SHADER_RESOURCE_VIEW_DESC v;
    if (is_fake(r)) {
        const D3D12_SHADER_RESOURCE_VIEW_DESC *in = (const D3D12_SHADER_RESOURCE_VIEW_DESC *)desc;
        if (!in) {
            v = {}; v.Format = view_of(s_ffmt); v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; v.Texture2D.MipLevels = 1; desc = &v;
        } else if (is_typeless(in->Format) || in->Format == DXGI_FORMAT_UNKNOWN) { v = *in; v.Format = view_of(s_ffmt); desc = &v; }
    }
    oCreateSRV(d, r, desc, h);
}

static void release_real(void) {
    for (UINT i = 0; i < MAXB; i++) if (s_real[i]) { s_real[i]->Release(); s_real[i] = NULL; }
    if (s_mid) { s_mid->Release(); s_mid = NULL; }
}
static void release_fakes(void) {
    for (UINT i = 0; i < MAXB; i++)
        if (s_fake[i]) {
            ULONG left = s_fake[i]->Release();
            if (left) ov_log("upscale: the game still holds game buffer %u (%lu refs)", i, left);
            s_fake[i] = NULL;
        }
}

static bool create_fakes(void) {
    D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d = {};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width = s_fw; d.Height = s_fh; d.DepthOrArraySize = 1;
    d.MipLevels = 1; d.Format = typeless_of(s_ffmt); d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    if (s_usage & DXGI_USAGE_UNORDERED_ACCESS) d.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    for (UINT i = 0; i < s_count && i < MAXB; i++) {
        HRESULT r = s_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COMMON, NULL, IID_PPV_ARGS(&s_fake[i]));
        if (FAILED(r)) { ov_log("upscale: game buffer %u (%ux%u fmt %d) failed 0x%08lx", i, s_fw, s_fh, (int)d.Format, (unsigned long)r); return false; }
        s_fake[i]->SetName(L"EXVSOverlay game buffer");
    }
    if (!oResGetDesc) {   // first time: hook views/desc so the typeless textures look like swapchain buffers
        patch(*(void ***)s_fake[0], 10, (void *)hk_res_get_desc, &oResGetDesc);
        patch(*(void ***)s_dev, 18, (void *)hk_create_srv, &oCreateSRV);
        patch(*(void ***)s_dev, 20, (void *)hk_create_rtv, &oCreateRTV);
    }
    return true;
}

static HRESULT STDMETHODCALLTYPE hk_get_buffer(IDXGISwapChain *, UINT, REFIID, void **);
static HRESULT STDMETHODCALLTYPE hk_get_desc(IDXGISwapChain *, DXGI_SWAP_CHAIN_DESC *);
static HRESULT STDMETHODCALLTYPE hk_get_desc1(IDXGISwapChain1 *, DXGI_SWAP_CHAIN_DESC1 *);
static HRESULT STDMETHODCALLTYPE hk_resize1(IDXGISwapChain3 *, UINT, UINT, UINT, DXGI_FORMAT, UINT, const UINT *, IUnknown *const *);

static void hook_swapchain_vt(void **vt) {
    if (!oResize) oResize = (PResizeBuffers)vt[13];   // not hooked yet by overlay.cpp = the original
    patch(vt, 9, (void *)hk_get_buffer, &oGetBuffer);
    patch(vt, 12, (void *)hk_get_desc, &oGetDesc);
    patch(vt, 18, (void *)hk_get_desc1, &oGetDesc1);
    patch(vt, 39, (void *)hk_resize1, &oResize1);
}

static bool start_redirect(IDXGISwapChain *sc, ID3D12CommandQueue *q, UINT count, UINT fw, UINT fh, DXGI_FORMAT fmt,
                           UINT usage, UINT flags, UINT rw, UINT rh, const char *where) {
    if (FAILED(sc->QueryInterface(IID_PPV_ARGS(&s_sc3)))) { ov_log("upscale: no IDXGISwapChain3"); return false; }
    hook_swapchain_vt(*(void ***)sc);   // in case the game was faster than the overlay's own hooks
    s_sc3->Release();
    if (!s_dev && FAILED(sc->GetDevice(IID_PPV_ARGS(&s_dev)))) { ov_log("upscale: GetDevice failed"); return false; }
    if (FAILED(s_sc3->GetHwnd(&s_hwnd))) s_hwnd = NULL;
    s_count = count; s_fw = fw; s_fh = fh; s_ffmt = fmt; s_usage = usage; s_flags = flags; s_rw = rw; s_rh = rh;
    s_active = true;   // is_fake() needs it while creating
    if (!create_fakes()) { release_fakes(); s_active = false; return false; }
    if (q) { q->AddRef(); s_queue = q; }
    s_sc = sc;
    ov_log("upscale: took over the game's swapchain at %s: game %ux%u fmt %d x%u, screen %ux%u", where, fw, fh, (int)fmt, count, rw, rh);
    return true;
}

// ---------------------------------------------------------------------------------------------
// Swapchain hooks
// ---------------------------------------------------------------------------------------------
static HRESULT STDMETHODCALLTYPE hk_get_buffer(IDXGISwapChain *sc, UINT i, REFIID riid, void **out) {
    if (s_active && sc == s_sc) {
        if (!out) return E_POINTER;
        if (i >= s_count || i >= MAXB || !s_fake[i]) return DXGI_ERROR_INVALID_CALL;
        return s_fake[i]->QueryInterface(riid, out);
    }
    return oGetBuffer(sc, i, riid, out);
}
static HRESULT STDMETHODCALLTYPE hk_get_desc(IDXGISwapChain *sc, DXGI_SWAP_CHAIN_DESC *d) {
    HRESULT r = oGetDesc(sc, d);
    if (SUCCEEDED(r) && d && s_active && sc == s_sc) { d->BufferDesc.Width = s_fw; d->BufferDesc.Height = s_fh; d->BufferDesc.Format = s_ffmt; }
    return r;
}
static HRESULT STDMETHODCALLTYPE hk_get_desc1(IDXGISwapChain1 *sc, DXGI_SWAP_CHAIN_DESC1 *d) {
    HRESULT r = oGetDesc1(sc, d);
    if (SUCCEEDED(r) && d && s_active && (IDXGISwapChain *)sc == s_sc) { d->Width = s_fw; d->Height = s_fh; d->Format = s_ffmt; }
    return r;
}

bool up_resize(IDXGISwapChain *sc, UINT n, UINT w, UINT h, DXGI_FORMAT f, UINT fl, HRESULT *hr) {
    if (s_bypass || !oResize || s_depth) return false;
    Depth guard;
    if (s_active && sc == s_sc) {
        ov_release_targets();
        wait_all();
        release_fakes(); release_real();
        UINT cw, ch; client_size(s_hwnd, &cw, &ch);
        if (!w || !h) { w = cw; h = ch; }
        UINT n0 = n ? n : s_count; DXGI_FORMAT f0 = f != DXGI_FORMAT_UNKNOWN ? f : s_ffmt;
        UINT rw = cw ? cw : w, rh = ch ? ch : h;
        *hr = oResize(sc, n0, rw, rh, f0, fl);
        if (FAILED(*hr)) { ov_log("upscale: ResizeBuffers(%u, %ux%u) failed 0x%08lx", n0, rw, rh, (unsigned long)*hr); return true; }
        s_count = n0; s_fw = w; s_fh = h; s_ffmt = f0; s_flags = fl; s_rw = rw; s_rh = rh;
        if (!create_fakes()) { release_fakes(); *hr = E_OUTOFMEMORY; }
        ov_log("upscale: game resized to %ux%u (screen %ux%u)", w, h, rw, rh);
        return true;
    }
    if (s_wanted && !s_sc && !s_failed) {
        // missed the creation: take it over now (the game has released all its buffers)
        IDXGISwapChain1 *sc1 = NULL; DXGI_SWAP_CHAIN_DESC1 d = {};
        if (FAILED(sc->QueryInterface(IID_PPV_ARGS(&sc1)))) return false;
        HRESULT dr = oGetDesc1(sc1, &d); HWND hwnd = NULL; sc1->GetHwnd(&hwnd); sc1->Release();
        if (FAILED(dr) || !hwnd) return false;
        ov_release_targets();
        UINT cw, ch; client_size(hwnd, &cw, &ch);
        if (!w || !h) { w = cw; h = ch; }
        UINT n0 = n ? n : d.BufferCount; DXGI_FORMAT f0 = f != DXGI_FORMAT_UNKNOWN ? f : d.Format;
        UINT rw = cw ? cw : w, rh = ch ? ch : h;
        *hr = oResize(sc, n0, rw, rh, f0, fl);
        if (FAILED(*hr)) { s_failed = true; ov_log("upscale: ResizeBuffers to the window size failed 0x%08lx", (unsigned long)*hr); *hr = oResize(sc, n, w, h, f, fl); return true; }
        if (!start_redirect(sc, ov_game_queue(), n0, w, h, f0, d.BufferUsage, fl, rw, rh, "ResizeBuffers")) {
            s_failed = true;
            ov_log("upscale: could not take over the swapchain; running without upscaling");
            *hr = oResize(sc, n, w, h, f, fl);   // give the game what it asked for
        }
        return true;
    }
    return false;
}

static HRESULT STDMETHODCALLTYPE hk_resize1(IDXGISwapChain3 *sc, UINT n, UINT w, UINT h, DXGI_FORMAT f, UINT fl,
                                            const UINT *mask, IUnknown *const *queues) {
    HRESULT hr;
    if (up_resize((IDXGISwapChain *)sc, n, w, h, f, fl, &hr)) return hr;
    return oResize1(sc, n, w, h, f, fl, mask, queues);
}

static HRESULT STDMETHODCALLTYPE hk_create_sc_hwnd(IDXGIFactory2 *fac, IUnknown *dev, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1 *d,
                                                   const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *fs, IDXGIOutput *o, IDXGISwapChain1 **out) {
    ID3D12CommandQueue *q = NULL;
    if (s_depth || s_bypass || !s_wanted || s_sc || s_failed || !d || !out || !dev || FAILED(dev->QueryInterface(IID_PPV_ARGS(&q))))
        return oCreateSCHwnd(fac, dev, hwnd, d, fs, o, out);
    Depth guard;
    UINT cw, ch; client_size(hwnd, &cw, &ch);
    DXGI_SWAP_CHAIN_DESC1 real = *d;
    UINT fw = d->Width ? d->Width : cw, fh = d->Height ? d->Height : ch;
    if (cw && ch) { real.Width = cw; real.Height = ch; }
    HRESULT r = oCreateSCHwnd(fac, dev, hwnd, &real, fs, o, out);
    if (SUCCEEDED(r) && *out) {
        if (!start_redirect(*out, q, d->BufferCount, fw, fh, d->Format, d->BufferUsage, d->Flags, real.Width, real.Height, "creation")) {
            s_failed = true;
            ov_log("upscale: could not take over the swapchain; running without upscaling");
            oResize(*out, d->BufferCount, d->Width, d->Height, d->Format, d->Flags);
        }
    }
    q->Release();
    return r;
}

static HRESULT STDMETHODCALLTYPE hk_create_sc(IDXGIFactory *fac, IUnknown *dev, DXGI_SWAP_CHAIN_DESC *d, IDXGISwapChain **out) {
    ID3D12CommandQueue *q = NULL;
    if (s_depth || s_bypass || !s_wanted || s_sc || s_failed || !d || !out || !dev || FAILED(dev->QueryInterface(IID_PPV_ARGS(&q))))
        return oCreateSC(fac, dev, d, out);
    Depth guard;
    UINT cw, ch; client_size(d->OutputWindow, &cw, &ch);
    DXGI_SWAP_CHAIN_DESC real = *d;
    UINT fw = d->BufferDesc.Width ? d->BufferDesc.Width : cw, fh = d->BufferDesc.Height ? d->BufferDesc.Height : ch;
    if (cw && ch) { real.BufferDesc.Width = cw; real.BufferDesc.Height = ch; }
    HRESULT r = oCreateSC(fac, dev, &real, out);
    if (SUCCEEDED(r) && *out) {
        if (!start_redirect(*out, q, d->BufferCount, fw, fh, d->BufferDesc.Format, d->BufferUsage, d->Flags,
                            real.BufferDesc.Width, real.BufferDesc.Height, "creation")) {
            s_failed = true;
            ov_log("upscale: could not take over the swapchain; running without upscaling");
            oResize(*out, d->BufferCount, d->BufferDesc.Width, d->BufferDesc.Height, d->BufferDesc.Format, d->Flags);
        }
    }
    q->Release();
    return r;
}

void up_install_factory(void **fac_vt) {
    s_wanted = menu_upscale_mode() != 0;
    if (!s_wanted) { ov_log("upscale: off at startup (the swapchain is left alone)"); return; }
    if (GetEnvironmentVariableA("EXVSOVERLAY_TEST_LATE", NULL, 0)) return;   // test host: act as if creation was missed
    patch(fac_vt, 10, (void *)hk_create_sc, &oCreateSC);
    patch(fac_vt, 15, (void *)hk_create_sc_hwnd, &oCreateSCHwnd);
}
void up_install_swapchain(void **sc_vt, void *orig_resize) {
    oResize = (PResizeBuffers)orig_resize;
    if (s_wanted) hook_swapchain_vt(sc_vt);   // upscaler off at startup: leave the swapchain alone
}
void up_bypass(bool on) { InterlockedExchange(&s_bypass, on ? 1 : 0); }

HRESULT up_real_get_buffer(IDXGISwapChain *sc, UINT i, REFIID riid, void **out) {
    return oGetBuffer ? oGetBuffer(sc, i, riid, out) : sc->GetBuffer(i, riid, out);
}
HRESULT up_real_desc1(IDXGISwapChain1 *sc, DXGI_SWAP_CHAIN_DESC1 *d) {
    return oGetDesc1 ? oGetDesc1(sc, d) : sc->GetDesc1(d);
}

void up_status(UpStatus *st) {
    st->wanted = s_wanted; st->active = s_active && s_sc && !s_pipe_failed; st->failed = s_failed || s_pipe_failed;
    st->game_w = s_fw; st->game_h = s_fh; st->out_w = s_rw; st->out_h = s_rh; st->path = s_last_path;
}

// test host only: the real (window-sized) buffer, since GetBuffer now returns the game's texture
extern "C" __declspec(dllexport) HRESULT OvTestRealBuffer(IDXGISwapChain *sc, UINT i, REFIID riid, void **out) {
    return up_real_get_buffer(sc, i, riid, out);
}

// ---------------------------------------------------------------------------------------------
// Shaders: full-screen triangle; EASU (FSR 1 upscale), RCAS (FSR 1 sharpen), bilinear copy.
// Ported from ffx_fsr1.h (FsrEasuF / FsrRcasF) to plain HLSL with Load() instead of Gather.
// ---------------------------------------------------------------------------------------------
static const char SHADERS[] = R"HLSL(
Texture2D<float4> t0 : register(t0);
SamplerState s0 : register(s0);
cbuffer C : register(b0) { float4 k0; float4 k1; }   // k0 = (in/out x, in/out y, in w, in h), k1.x = sharpness

void vs(uint id : SV_VertexID, out float4 pos : SV_Position, out float2 uv : TEXCOORD0) {
    uv = float2((id << 1) & 2, id & 2);
    pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}

float4 ps_copy(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    return float4(t0.SampleLevel(s0, uv, 0).rgb, 1);
}

float3 tap(int2 p) { return t0.Load(int3(clamp(p, int2(0, 0), int2(k0.zw) - 1), 0)).rgb; }
float luma(float3 c) { return c.b * 0.5 + (c.r * 0.5 + c.g); }

void easu_set(inout float2 dir, inout float len, float w, float lA, float lB, float lC, float lD, float lE) {
    float dc = lD - lC, cb = lC - lB;
    float lenX = 1.0 / max(max(abs(dc), abs(cb)), 1.0e-6);
    float dirX = lD - lB;
    dir.x += dirX * w;
    lenX = saturate(abs(dirX) * lenX); lenX *= lenX;
    len += lenX * w;
    float ec = lE - lC, ca = lC - lA;
    float lenY = 1.0 / max(max(abs(ec), abs(ca)), 1.0e-6);
    float dirY = lE - lA;
    dir.y += dirY * w;
    lenY = saturate(abs(dirY) * lenY); lenY *= lenY;
    len += lenY * w;
}

void easu_tap(inout float3 aC, inout float aW, float2 off, float2 dir, float2 len, float lob, float clp, float3 c) {
    float2 v;
    v.x = off.x * dir.x + off.y * dir.y;
    v.y = off.x * (-dir.y) + off.y * dir.x;
    v *= len;
    float d2 = min(dot(v, v), clp);
    float wB = 2.0 / 5.0 * d2 - 1.0;
    float wA = lob * d2 - 1.0;
    wB *= wB; wA *= wA;
    wB = 25.0 / 16.0 * wB - (25.0 / 16.0 - 1.0);
    float w = wB * wA;
    aC += c * w; aW += w;
}

//    b c
//  e f g h
//  i j k l
//    n o
float4 ps_easu(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    float2 pp = pos.xy * k0.xy - 0.5;
    float2 fp = floor(pp); pp -= fp;
    int2 q = int2(fp);
    float3 b = tap(q + int2(0, -1)), c = tap(q + int2(1, -1));
    float3 e = tap(q + int2(-1, 0)), f = tap(q),              g = tap(q + int2(1, 0)), h = tap(q + int2(2, 0));
    float3 i = tap(q + int2(-1, 1)), j = tap(q + int2(0, 1)), k = tap(q + int2(1, 1)), l = tap(q + int2(2, 1));
    float3 n = tap(q + int2(0, 2)),  o = tap(q + int2(1, 2));
    float bL = luma(b), cL = luma(c), eL = luma(e), fL = luma(f), gL = luma(g), hL = luma(h);
    float iL = luma(i), jL = luma(j), kL = luma(k), lL = luma(l), nL = luma(n), oL = luma(o);

    float2 dir = 0; float len = 0;
    easu_set(dir, len, (1.0 - pp.x) * (1.0 - pp.y), bL, eL, fL, gL, jL);
    easu_set(dir, len, pp.x * (1.0 - pp.y),         cL, fL, gL, hL, kL);
    easu_set(dir, len, (1.0 - pp.x) * pp.y,         fL, iL, jL, kL, nL);
    easu_set(dir, len, pp.x * pp.y,                 gL, jL, kL, lL, oL);

    float2 dir2 = dir * dir;
    float dirR = dir2.x + dir2.y;
    bool zro = dirR < 1.0 / 32768.0;
    dirR = rsqrt(max(dirR, 1.0e-12));
    dirR = zro ? 1.0 : dirR;
    dir.x = zro ? 1.0 : dir.x;
    dir *= dirR;
    len = len * 0.5; len *= len;
    float stretch = dot(dir, dir) / max(abs(dir.x), abs(dir.y));
    float2 len2 = float2(1.0 + (stretch - 1.0) * len, 1.0 - 0.5 * len);
    float lob = 0.5 + ((1.0 / 4.0 - 0.04) - 0.5) * len;
    float clp = 1.0 / lob;

    float3 aC = 0; float aW = 0;
    easu_tap(aC, aW, float2( 0, -1) - pp, dir, len2, lob, clp, b);
    easu_tap(aC, aW, float2( 1, -1) - pp, dir, len2, lob, clp, c);
    easu_tap(aC, aW, float2(-1,  1) - pp, dir, len2, lob, clp, i);
    easu_tap(aC, aW, float2( 0,  1) - pp, dir, len2, lob, clp, j);
    easu_tap(aC, aW, float2( 0,  0) - pp, dir, len2, lob, clp, f);
    easu_tap(aC, aW, float2(-1,  0) - pp, dir, len2, lob, clp, e);
    easu_tap(aC, aW, float2( 1,  1) - pp, dir, len2, lob, clp, k);
    easu_tap(aC, aW, float2( 2,  1) - pp, dir, len2, lob, clp, l);
    easu_tap(aC, aW, float2( 2,  0) - pp, dir, len2, lob, clp, h);
    easu_tap(aC, aW, float2( 1,  0) - pp, dir, len2, lob, clp, g);
    easu_tap(aC, aW, float2( 1,  2) - pp, dir, len2, lob, clp, o);
    easu_tap(aC, aW, float2( 0,  2) - pp, dir, len2, lob, clp, n);
    float3 mn = min(min(f, g), min(j, k)), mx = max(max(f, g), max(j, k));   // de-ringing
    return float4(clamp(aC / aW, mn, mx), 1);
}

//    b
//  d e f
//    h
float4 ps_rcas(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    int2 p = int2(pos.xy);
    float3 b = tap(p + int2(0, -1)), d = tap(p + int2(-1, 0)), e = tap(p), f = tap(p + int2(1, 0)), h = tap(p + int2(0, 1));
    float3 mn4 = min(min(b, d), min(f, h)), mx4 = max(max(b, d), max(f, h));
    float3 hitMin = min(mn4, e) / max(4.0 * mx4, 1.0e-5);
    float3 hitMax = (1.0 - max(mx4, e)) / min(4.0 * mn4 - 4.0, -1.0e-5);
    float3 lobeRGB = max(-hitMin, hitMax);
    float lobe = max(-(0.25 - 1.0 / 16.0), min(max(lobeRGB.r, max(lobeRGB.g, lobeRGB.b)), 0.0)) * k1.x;
    float3 c = (lobe * (b + d + f + h) + e) / (4.0 * lobe + 1.0);
    return float4(saturate(c), 1);
}
)HLSL";

static ID3DBlob *compile(const char *entry, const char *target) {
    ID3DBlob *code = NULL, *err = NULL;
    HRESULT r = D3DCompile(SHADERS, sizeof(SHADERS) - 1, "upscale.hlsl", NULL, NULL, entry, target,
                           D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
    if (FAILED(r)) {
        ov_log("upscale: shader %s failed 0x%08lx: %s", entry, (unsigned long)r, err ? (const char *)err->GetBufferPointer() : "");
        if (code) code->Release();
        code = NULL;
    }
    if (err) err->Release();
    return code;
}

static ID3DBlob *s_vs, *s_ps_easu, *s_ps_rcas, *s_ps_copy;

static ID3D12PipelineState *make_pso(ID3DBlob *ps, DXGI_FORMAT fmt) {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d = {};
    d.pRootSignature = s_rs;
    d.VS = { s_vs->GetBufferPointer(), s_vs->GetBufferSize() };
    d.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
    d.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    d.SampleMask = UINT_MAX;
    d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID; d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    d.RasterizerState.DepthClipEnable = TRUE;
    d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    d.NumRenderTargets = 1; d.RTVFormats[0] = fmt; d.SampleDesc.Count = 1;
    ID3D12PipelineState *p = NULL;
    HRESULT r = s_dev->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&p));
    if (FAILED(r)) ov_log("upscale: pipeline (fmt %d) failed 0x%08lx", (int)fmt, (unsigned long)r);
    return p;
}

static bool init_pipeline(void) {
    s_vs = compile("vs", "vs_5_0"); s_ps_easu = compile("ps_easu", "ps_5_0");
    s_ps_rcas = compile("ps_rcas", "ps_5_0"); s_ps_copy = compile("ps_copy", "ps_5_0");
    if (!s_vs || !s_ps_easu || !s_ps_rcas || !s_ps_copy) return false;

    D3D12_DESCRIPTOR_RANGE range = {}; range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; range.NumDescriptors = 1;
    D3D12_ROOT_PARAMETER params[2] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; params[0].Constants.Num32BitValues = 8;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1; params[1].DescriptorTable.pDescriptorRanges = &range;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC smp = {};
    smp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    smp.AddressU = smp.AddressV = smp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    smp.MaxLOD = D3D12_FLOAT32_MAX; smp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC rd = {};
    rd.NumParameters = 2; rd.pParameters = params; rd.NumStaticSamplers = 1; rd.pStaticSamplers = &smp;
    ID3DBlob *blob = NULL, *err = NULL;
    if (FAILED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err))) {
        ov_log("upscale: root signature: %s", err ? (const char *)err->GetBufferPointer() : "?");
        if (err) err->Release();
        return false;
    }
    HRESULT r = s_dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&s_rs));
    blob->Release();
    if (FAILED(r)) { ov_log("upscale: CreateRootSignature failed"); return false; }

    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; hd.NumDescriptors = MAXB + 1; hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(s_dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&s_srv_heap)))) return false;
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    if (FAILED(s_dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&s_rtv_heap)))) return false;
    s_srv_inc = s_dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    s_rtv_inc = s_dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    for (UINT i = 0; i < MAXB; i++)
        if (FAILED(s_dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&s_alloc[i])))) return false;
    if (FAILED(s_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, s_alloc[0], NULL, IID_PPV_ARGS(&s_cl)))) return false;
    s_cl->Close();
    if (FAILED(s_dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&s_fence)))) return false;
    s_ev = CreateEventW(NULL, FALSE, FALSE, NULL);
    s_pso_easu = make_pso(s_ps_easu, DXGI_FORMAT_R8G8B8A8_UNORM);   // into the intermediate texture
    ov_log("upscale: FSR 1 pipeline ready");
    return s_pso_easu != NULL;
}

static D3D12_CPU_DESCRIPTOR_HANDLE cpu_at(ID3D12DescriptorHeap *h, UINT inc, UINT i) {
    D3D12_CPU_DESCRIPTOR_HANDLE c = h->GetCPUDescriptorHandleForHeapStart(); c.ptr += (SIZE_T)i * inc; return c;
}
static D3D12_GPU_DESCRIPTOR_HANDLE gpu_at(UINT i) {
    D3D12_GPU_DESCRIPTOR_HANDLE g = s_srv_heap->GetGPUDescriptorHandleForHeapStart(); g.ptr += (UINT64)i * s_srv_inc; return g;
}

// window-size buffers, their views, the intermediate texture and the game buffers' SRVs
static bool ensure_targets(void) {
    if (s_real[0] && s_mid) return true;
    release_real();
    DXGI_SWAP_CHAIN_DESC1 d;
    if (FAILED(oGetDesc1(s_sc3, &d))) return false;
    s_rw = d.Width; s_rh = d.Height;
    s_real_rtv_fmt = view_of(d.Format);
    for (UINT i = 0; i < s_count && i < MAXB; i++) {
        if (FAILED(oGetBuffer(s_sc, i, IID_PPV_ARGS(&s_real[i])))) { ov_log("upscale: real buffer %u unavailable", i); return false; }
        D3D12_RENDER_TARGET_VIEW_DESC rv = {}; rv.Format = s_real_rtv_fmt; rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        (oCreateRTV ? oCreateRTV(s_dev, s_real[i], &rv, cpu_at(s_rtv_heap, s_rtv_inc, i))
                    : s_dev->CreateRenderTargetView(s_real[i], &rv, cpu_at(s_rtv_heap, s_rtv_inc, i)));
        D3D12_SHADER_RESOURCE_VIEW_DESC sv = {};
        sv.Format = view_of(s_ffmt); sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; sv.Texture2D.MipLevels = 1;
        s_dev->CreateShaderResourceView(s_fake[i], &sv, cpu_at(s_srv_heap, s_srv_inc, i));
    }
    D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC md = {};
    md.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; md.Width = s_rw; md.Height = s_rh; md.DepthOrArraySize = 1; md.MipLevels = 1;
    md.Format = DXGI_FORMAT_R8G8B8A8_UNORM; md.SampleDesc.Count = 1; md.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    if (FAILED(s_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &md, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, NULL, IID_PPV_ARGS(&s_mid)))) {
        ov_log("upscale: intermediate texture %ux%u failed", s_rw, s_rh); return false;
    }
    s_dev->CreateRenderTargetView(s_mid, NULL, cpu_at(s_rtv_heap, s_rtv_inc, MAXB));
    s_dev->CreateShaderResourceView(s_mid, NULL, cpu_at(s_srv_heap, s_srv_inc, MAXB));
    if (s_pso_fmt != s_real_rtv_fmt || !s_pso_rcas || !s_pso_copy) {
        if (s_pso_rcas) s_pso_rcas->Release();
        if (s_pso_copy) s_pso_copy->Release();
        s_pso_rcas = make_pso(s_ps_rcas, s_real_rtv_fmt);
        s_pso_copy = make_pso(s_ps_copy, s_real_rtv_fmt);
        s_pso_fmt = s_real_rtv_fmt;
        if (!s_pso_rcas || !s_pso_copy) return false;
    }
    return true;
}

static void resize_real(UINT cw, UINT ch) {
    ov_release_targets();
    wait_all();
    release_real();
    Depth guard;
    HRESULT r = oResize(s_sc, s_count, cw, ch, DXGI_FORMAT_UNKNOWN, s_flags);
    ov_log("upscale: window is now %ux%u -> screen buffers resized (0x%08lx)", cw, ch, (unsigned long)r);
}

static void barrier(D3D12_RESOURCE_BARRIER *b, ID3D12Resource *r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    *b = {}; b->Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b->Transition.pResource = r;
    b->Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES; b->Transition.StateBefore = from; b->Transition.StateAfter = to;
}

static void draw_pass(ID3D12PipelineState *pso, D3D12_CPU_DESCRIPTOR_HANDLE rtv, UINT w, UINT h, UINT srv, const float k[8]) {
    s_cl->SetPipelineState(pso);
    s_cl->OMSetRenderTargets(1, &rtv, FALSE, NULL);
    D3D12_VIEWPORT vp = { 0, 0, (float)w, (float)h, 0, 1 };
    D3D12_RECT sr = { 0, 0, (LONG)w, (LONG)h };
    s_cl->RSSetViewports(1, &vp); s_cl->RSSetScissorRects(1, &sr);
    s_cl->SetGraphicsRoot32BitConstants(0, 8, k, 0);
    s_cl->SetGraphicsRootDescriptorTable(1, gpu_at(srv));
    s_cl->DrawInstanced(3, 1, 0, 0);
}

// Every Present of the game's swapchain while it is taken over: game texture -> window-size buffer.
void up_frame(IDXGISwapChain *sc) {
    if (!s_active || sc != s_sc || s_pipe_failed) return;
    ID3D12CommandQueue *q = s_queue ? s_queue : ov_game_queue();
    if (!q) return;
    if (!s_rs && !init_pipeline()) { s_pipe_failed = true; ov_log("upscale: disabled (pipeline failed); the picture may stay black"); return; }

    UINT cw, ch; client_size(s_hwnd, &cw, &ch);
    if (cw && ch && (cw != s_rw || ch != s_rh)) resize_real(cw, ch);
    if (!ensure_targets()) { release_real(); return; }

    UINT idx = s_sc3->GetCurrentBackBufferIndex();
    if (idx >= s_count || idx >= MAXB || !s_fake[idx] || !s_real[idx]) return;

    int mode = menu_upscale_mode();
    float sharp = menu_upscale_sharpness();
    bool upscale = s_fw < s_rw || s_fh < s_rh, same = s_fw == s_rw && s_fh == s_rh;
    int path = mode == 0 ? 0 : (upscale ? 1 : (same ? 2 : 0));   // 0 stretch, 1 EASU+RCAS, 2 RCAS only
    if (path != s_last_path) {
        static const char *names[] = { "plain stretch", "FSR 1 upscale + sharpen", "FSR 1 sharpen only (same size)" };
        ov_log("upscale: %ux%u -> %ux%u, %s", s_fw, s_fh, s_rw, s_rh, names[path]);
        s_last_path = path;
    }

    if (s_alloc_fence[idx] && s_fence->GetCompletedValue() < s_alloc_fence[idx]) {
        s_fence->SetEventOnCompletion(s_alloc_fence[idx], s_ev); WaitForSingleObject(s_ev, 2000);
    }
    s_alloc[idx]->Reset();
    s_cl->Reset(s_alloc[idx], NULL);
    D3D12_RESOURCE_BARRIER b[2];
    barrier(&b[0], s_fake[idx], D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    barrier(&b[1], s_real[idx], D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    s_cl->ResourceBarrier(2, b);
    s_cl->SetGraphicsRootSignature(s_rs);
    s_cl->SetDescriptorHeaps(1, &s_srv_heap);
    s_cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    D3D12_CPU_DESCRIPTOR_HANDLE real_rtv = cpu_at(s_rtv_heap, s_rtv_inc, idx);
    if (path == 1) {
        float ke[8] = { (float)s_fw / s_rw, (float)s_fh / s_rh, (float)s_fw, (float)s_fh, 0, 0, 0, 0 };
        barrier(&b[0], s_mid, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        s_cl->ResourceBarrier(1, b);
        draw_pass(s_pso_easu, cpu_at(s_rtv_heap, s_rtv_inc, MAXB), s_rw, s_rh, idx, ke);
        barrier(&b[0], s_mid, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        s_cl->ResourceBarrier(1, b);
        float kr[8] = { 1, 1, (float)s_rw, (float)s_rh, sharp, 0, 0, 0 };
        draw_pass(s_pso_rcas, real_rtv, s_rw, s_rh, MAXB, kr);
    } else if (path == 2) {
        float kr[8] = { 1, 1, (float)s_fw, (float)s_fh, sharp, 0, 0, 0 };
        draw_pass(s_pso_rcas, real_rtv, s_rw, s_rh, idx, kr);
    } else {
        float kc[8] = { 1, 1, (float)s_fw, (float)s_fh, 0, 0, 0, 0 };
        draw_pass(s_pso_copy, real_rtv, s_rw, s_rh, idx, kc);
    }
    barrier(&b[0], s_fake[idx], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    barrier(&b[1], s_real[idx], D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    s_cl->ResourceBarrier(2, b);
    s_cl->Close();
    ID3D12CommandList *lists[] = { s_cl };
    q->ExecuteCommandLists(1, lists);
    q->Signal(s_fence, ++s_fv);
    s_alloc_fence[idx] = s_fv;
}
