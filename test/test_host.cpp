// Test host for EXVS Overlay: a tiny D3D12 "game" in a hidden window (nothing appears on screen).
// Loads build/xinput9_1_0.dll, renders N frames (with a ResizeBuffers in the middle), then copies the
// last presented backbuffer to overlay_test.bmp so the menu drawing can be checked without the game.
// Usage: test_host.exe [frames]      (EXVSOVERLAY_TEST=1 is set so the menu opens by itself)
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(x) do { HRESULT hr_ = (x); if (FAILED(hr_)) { printf("FAIL %s -> 0x%08lx (line %d)\n", #x, (unsigned long)hr_, __LINE__); return 1; } } while (0)

int main(int argc, char **argv) {
    int frames = argc > 1 ? atoi(argv[1]) : 90;
    bool up = getenv("TEST_UPSCALE") != NULL;   // FSR: game 1280x720 then 960x540, window 1920x1080
    if (!getenv("TEST_NOMENU")) SetEnvironmentVariableA("EXVSOVERLAY_TEST", "1");
    if (up) SetEnvironmentVariableA("EXVSOVERLAY_TEST_UPSCALE", "1");
    char dll[MAX_PATH]; GetModuleFileNameA(NULL, dll, MAX_PATH); strcpy(strrchr(dll, '\\') + 1, "xinput9_1_0.dll");
    HMODULE ov = LoadLibraryA(dll);
    if (!ov) { printf("FAIL LoadLibrary %s (%lu)\n", dll, GetLastError()); return 1; }
    const char *exports[] = { "XInputGetState", "XInputSetState", "XInputGetCapabilities", "XInputGetDSoundAudioDeviceGuids" };
    for (const char *e : exports) printf("export %-34s %s\n", e, GetProcAddress(ov, e) ? "ok" : "MISSING");
    Sleep(1500);   // let the overlay thread install its hooks

    WNDCLASSW wc = {}; wc.lpfnWndProc = DefWindowProcW; wc.hInstance = GetModuleHandleW(NULL); wc.lpszClassName = L"OvTestHost";
    RegisterClassW(&wc);
    RECT wr = { 0, 0, 1920, 1080 }; AdjustWindowRect(&wr, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = up ? CreateWindowW(wc.lpszClassName, L"overlay test", WS_OVERLAPPEDWINDOW, 0, 0, wr.right - wr.left, wr.bottom - wr.top, NULL, NULL, wc.hInstance, NULL)
                   : CreateWindowW(wc.lpszClassName, L"overlay test", WS_OVERLAPPEDWINDOW, 0, 0, 1280, 720, NULL, NULL, wc.hInstance, NULL);

    IDXGIFactory4 *fac; CHECK(CreateDXGIFactory1(IID_PPV_ARGS(&fac)));
    ID3D12Device *dev; CHECK(D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev)));
    D3D12_COMMAND_QUEUE_DESC qd = {}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue *q; CHECK(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&q)));
    UINT W = 1280, H = 720; const UINT N = 3;
    DXGI_SWAP_CHAIN_DESC1 sd = {}; sd.Width = W; sd.Height = H; sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.BufferCount = N; sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    IDXGISwapChain1 *sc1; CHECK(fac->CreateSwapChainForHwnd(q, hwnd, &sd, NULL, NULL, &sc1));
    IDXGISwapChain3 *sc; CHECK(sc1->QueryInterface(IID_PPV_ARGS(&sc)));

    D3D12_DESCRIPTOR_HEAP_DESC hd = {}; hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors = N;
    ID3D12DescriptorHeap *rtvh; CHECK(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtvh)));
    UINT inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    ID3D12Resource *bb[N] = {}; D3D12_CPU_DESCRIPTOR_HANDLE rtv[N];
    auto make_rtvs = [&]() {
        for (UINT i = 0; i < N; i++) {
            sc->GetBuffer(i, IID_PPV_ARGS(&bb[i]));
            rtv[i] = rtvh->GetCPUDescriptorHandleForHeapStart(); rtv[i].ptr += i * inc;
            dev->CreateRenderTargetView(bb[i], NULL, rtv[i]);
        }
    };
    make_rtvs();
    ID3D12CommandAllocator *al; CHECK(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&al)));
    ID3D12GraphicsCommandList *cl; CHECK(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, al, NULL, IID_PPV_ARGS(&cl)));
    cl->Close();
    ID3D12Fence *fence; CHECK(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
    UINT64 fv = 0; HANDLE ev = CreateEventW(NULL, FALSE, FALSE, NULL);
    auto gpu_wait = [&]() { q->Signal(fence, ++fv); fence->SetEventOnCompletion(fv, ev); WaitForSingleObject(ev, 5000); };
    auto barrier = [&](ID3D12Resource *r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
        D3D12_RESOURCE_BARRIER x = {}; x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; x.Transition.pResource = r;
        x.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES; x.Transition.StateBefore = a; x.Transition.StateAfter = b;
        cl->ResourceBarrier(1, &x);
    };

    UINT last = 0;
    for (int f = 0; f < frames; f++) {
        if (f == frames / 3) {   // exercise the ResizeBuffers hook
            gpu_wait();
            for (UINT i = 0; i < N; i++) { bb[i]->Release(); bb[i] = NULL; }
            if (up) { W = 960; H = 540; } else { W = 1920; H = 1080; }
            HRESULT hr = sc->ResizeBuffers(N, W, H, DXGI_FORMAT_UNKNOWN, 0);
            printf("ResizeBuffers -> 0x%08lx\n", (unsigned long)hr);
            if (FAILED(hr)) return 1;
            make_rtvs();
        }
        if (f == frames * 2 / 3 && getenv("TEST_SABOTAGE_WINDOW")) {   // act like the game resizing its window
            SetWindowLongPtrW(hwnd, GWL_STYLE, WS_OVERLAPPEDWINDOW);
            SetWindowPos(hwnd, NULL, 100, 100, 1296, 759, SWP_NOZORDER | SWP_FRAMECHANGED);
            printf("sabotage: window set back to a normal 1280x720 window at frame %d\n", f);
        }
        if (getenv("TEST_DRAG")) {   // drag the controller overlay from the bottom-left corner (client coords)
            auto at = [&](int x, int y) { return MAKELPARAM(x, y); };
            if (f == 8) PostMessageW(hwnd, WM_MOUSEMOVE, 0, at(134, 626));
            if (f == 10) PostMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, at(134, 626));
            if (f > 10 && f <= 30) { int t = f - 10; PostMessageW(hwnd, WM_MOUSEMOVE, MK_LBUTTON, at(134 + (640 - 134) * t / 20, 626 + (200 - 626) * t / 20)); }
            if (f == 32) PostMessageW(hwnd, WM_LBUTTONUP, 0, at(640, 200));
        }
        UINT i = sc->GetCurrentBackBufferIndex();
        al->Reset(); cl->Reset(al, NULL);
        barrier(bb[i], D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
        float t = (float)f / frames; const float col[4] = { 0.10f + 0.3f * t, 0.25f, 0.35f, 1.0f };
        cl->ClearRenderTargetView(rtv[i], col, 0, NULL);
        if (up) {   // edges for the upscaler: a staircase diagonal, thin lines, a fine checker
            const float white[4] = { 0.95f, 0.95f, 0.9f, 1 }, red[4] = { 0.9f, 0.15f, 0.1f, 1 };
            D3D12_RECT r[400]; int nr = 0;
            int s = (int)W / 64;   // scales with the game resolution
            for (int k = 0; k < 40 && nr < 400; k++) r[nr++] = { (LONG)(s * 2 + k * s / 2), (LONG)(s * 2 + k * s / 3), (LONG)(s * 2 + k * s / 2 + s), (LONG)(s * 2 + k * s / 3 + s / 3) };
            for (int k = 0; k < 12 && nr < 400; k++) r[nr++] = { (LONG)(W / 2 + k * 6), (LONG)(s * 2), (LONG)(W / 2 + k * 6 + 1 + k / 4), (LONG)(H / 2) };
            for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) if (((x + y) & 1) && nr < 400)
                r[nr++] = { (LONG)(s * 2 + x * 2), (LONG)(H * 2 / 3 + y * 2), (LONG)(s * 2 + x * 2 + 2), (LONG)(H * 2 / 3 + y * 2 + 2) };
            cl->ClearRenderTargetView(rtv[i], white, nr, r);
            D3D12_RECT big = { (LONG)(W * 3 / 4), (LONG)(H * 2 / 3), (LONG)(W * 3 / 4 + s * 3), (LONG)(H * 2 / 3 + s * 3) };
            cl->ClearRenderTargetView(rtv[i], red, 1, &big);
        }
        barrier(bb[i], D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
        cl->Close();
        ID3D12CommandList *lists[] = { cl };
        q->ExecuteCommandLists(1, lists);
        HRESULT hr = sc->Present(0, 0);
        if (FAILED(hr)) { printf("Present failed 0x%08lx at frame %d\n", (unsigned long)hr, f); return 1; }
        gpu_wait();
        last = i;
        MSG m; while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
        Sleep(8);
    }
    RECT cr; GetClientRect(hwnd, &cr);
    printf("window client size at end: %ldx%ld, style 0x%08lx, visible %d\n", cr.right, cr.bottom,
           (unsigned long)GetWindowLongPtrW(hwnd, GWL_STYLE), IsWindowVisible(hwnd));

    auto dump = [&](ID3D12Resource *res, const char *name) -> int {
        D3D12_RESOURCE_DESC rd = res->GetDesc();
        UINT RW = (UINT)rd.Width, RH = rd.Height;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp; UINT64 total;
        dev->GetCopyableFootprints(&rd, 0, 1, 0, &fp, NULL, NULL, &total);
        D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC bd = {}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = total; bd.Height = 1; bd.DepthOrArraySize = 1;
        bd.MipLevels = 1; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ID3D12Resource *rb; CHECK(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, NULL, IID_PPV_ARGS(&rb)));
        al->Reset(); cl->Reset(al, NULL);
        barrier(res, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
        dst.pResource = rb; dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = fp;
        src.pResource = res; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; src.SubresourceIndex = 0;
        cl->CopyTextureRegion(&dst, 0, 0, 0, &src, NULL);
        barrier(res, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
        cl->Close();
        ID3D12CommandList *lists[] = { cl };
        q->ExecuteCommandLists(1, lists);
        gpu_wait();
        unsigned char *p; rb->Map(0, NULL, (void **)&p);
        char out[MAX_PATH]; GetModuleFileNameA(NULL, out, MAX_PATH); strcpy(strrchr(out, '\\') + 1, name);
        FILE *f = fopen(out, "wb");
        int rowb = RW * 3, pad = (4 - rowb % 4) % 4; DWORD img = (rowb + pad) * RH;
        BITMAPFILEHEADER bfh = {}; bfh.bfType = 0x4D42; bfh.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER); bfh.bfSize = bfh.bfOffBits + img;
        BITMAPINFOHEADER bih = {}; bih.biSize = sizeof(bih); bih.biWidth = RW; bih.biHeight = RH; bih.biPlanes = 1; bih.biBitCount = 24;
        fwrite(&bfh, sizeof(bfh), 1, f); fwrite(&bih, sizeof(bih), 1, f);
        long non_bg = 0, black = 0; const unsigned char zero[3] = {};
        for (int y = (int)RH - 1; y >= 0; y--) {
            unsigned char *row = p + fp.Footprint.RowPitch * y;
            for (UINT x = 0; x < RW; x++) {
                unsigned char px[3] = { row[x * 4 + 2], row[x * 4 + 1], row[x * 4 + 0] };
                if (abs(px[1] - 64) > 8) non_bg++;   // background green channel is 0.25*255
                if (px[0] + px[1] + px[2] == 0) black++;
                fwrite(px, 3, 1, f);
            }
            fwrite(zero, pad, 1, f);
        }
        fclose(f); rb->Unmap(0, NULL); rb->Release();
        printf("wrote %s (%ux%u), pixels differing from background: %ld, pure black: %ld\n", out, RW, RH, non_bg, black);
        return 0;
    };
    if (dump(bb[last], "overlay_test.bmp")) return 1;   // what the game rendered (its own buffer)
    typedef HRESULT (*PReal)(IDXGISwapChain *, UINT, REFIID, void **);
    PReal real = (PReal)GetProcAddress(ov, "OvTestRealBuffer");
    if (up && real) {
        ID3D12Resource *rr = NULL;
        if (FAILED(real(sc, last, IID_PPV_ARGS(&rr)))) { printf("FAIL real buffer\n"); return 1; }
        if (dump(rr, "overlay_test_screen.bmp")) return 1;   // what reaches the screen
        rr->Release();
    }
    return 0;
}
