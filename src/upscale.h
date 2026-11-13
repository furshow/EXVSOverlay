// FSR 1 upscaling (upscale.cpp) - the parts overlay.cpp calls.
#pragma once
#include <dxgi1_4.h>
#include <d3d12.h>

void up_install_factory(void **factory_vtable);                    // before the game creates its swapchain
void up_install_swapchain(void **swapchain_vtable, void *orig_resize_buffers);
void up_bypass(bool on);                                           // while creating our own dummy swapchain
bool up_resize(IDXGISwapChain *sc, UINT n, UINT w, UINT h, DXGI_FORMAT f, UINT flags, HRESULT *hr);   // true = handled
void up_frame(IDXGISwapChain *sc);                                 // every Present, before the menu is drawn
HRESULT up_real_get_buffer(IDXGISwapChain *sc, UINT i, REFIID riid, void **out);
HRESULT up_real_desc1(IDXGISwapChain1 *sc, DXGI_SWAP_CHAIN_DESC1 *d);

// overlay.cpp
void ov_release_targets(void);
ID3D12CommandQueue *ov_game_queue(void);
