#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <cstdio>
using Microsoft::WRL::ComPtr;

static int run(HWND window) {
    ComPtr<IDXGIFactory4> factory;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) return 5;
    ComPtr<ID3D12Device> device;
    hr = D3D12CreateDevice(nullptr,D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device));
    if (FAILED(hr)) return 6;
    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    hr=device->CreateCommandQueue(&queueDesc,IID_PPV_ARGS(&queue));
    if (FAILED(hr)) return 7;
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width=160;
    desc.Height=120;
    desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count=1;
    desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount=2;
    desc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;
    ComPtr<IDXGISwapChain1> swap;
    hr=factory->CreateSwapChainForHwnd(queue.Get(),window,&desc,nullptr,nullptr,&swap);
    if(FAILED(hr)) return 8;
    for(int i=0;i<14;++i) {
        if (i == 5) PostMessageW(window, WM_KEYDOWN, VK_F5, 1 | (0x3f << 16));
        if (i == 6) PostMessageW(window, WM_KEYUP, VK_F5, 1 | (0x3f << 16) | 0xc0000000);
        // Independent hotkeys and a simultaneous pair exercise the same per-frame reader as the game.
        if(i==7 || i==11) PostMessageW(window,WM_KEYDOWN,'C',1|(0x2e<<16));
        if(i==8 || i==12) PostMessageW(window,WM_KEYUP,'C',1|(0x2e<<16)|0xc0000000);
        if(i==9 || i==11) PostMessageW(window,WM_KEYDOWN,'B',1|(0x30<<16));
        if(i==10 || i==12) PostMessageW(window,WM_KEYUP,'B',1|(0x30<<16)|0xc0000000);
        MSG message{};
        while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}
        if (FAILED(swap->Present(0,0))) return 9;
        Sleep(20);
    }
    ComPtr<ID3D12Fence> fence;
    if (FAILED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)))) return 10;
    const HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    if (!event) return 11;
    const bool finished=SUCCEEDED(queue->Signal(fence.Get(),1)) &&
        SUCCEEDED(fence->SetEventOnCompletion(1,event)) &&
        WaitForSingleObject(event,10000)==WAIT_OBJECT_0;
    CloseHandle(event);
    return finished?0:12;
}
int main() {
    const HMODULE proxy=LoadLibraryW(L".\\dxgi.dll");
    if (!proxy || !GetProcAddress(proxy,"ReShadeRegisterAddon")) return 2;
    WNDCLASSW wc{};
    wc.lpfnWndProc=DefWindowProcW;
    wc.hInstance=GetModuleHandleW(nullptr);
    wc.lpszClassName=L"QuietReShadeDX12Host";
    if (!RegisterClassW(&wc)) return 3;
    HWND hwnd=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE,wc.lpszClassName,
        L"Quiet Direct3D12 test",WS_OVERLAPPEDWINDOW,0,0,160,120,nullptr,nullptr,wc.hInstance,nullptr);
    if (!hwnd) return 4;
    // Destroy and recreate the entire runtime to exercise the preview cache's lifetime.
    int result=run(hwnd);
    if(result==0) result=run(hwnd);
    DestroyWindow(hwnd);
    if (result) std::fprintf(stderr,"D3D12 smoke stopped at stage %d\n",result);
    return result;
}
